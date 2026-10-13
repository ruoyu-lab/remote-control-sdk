#include "protocol/tcp_demux.hpp"

#include "protocol/udp_connection.hpp"
#include "transport/framed_socket.hpp"
#include "transport/tcp_stream.hpp"
#include "util/bytes.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>

namespace rcsdk::protocol {
namespace {

using transport::Endpoint;

// What may wait for one connection, and for the handshakes (as UdpDemux).
constexpr size_t maxQueued = 8u << 20;
constexpr size_t maxHandshakeBytes = 1u << 20;
constexpr int readsPerTurn = 4;  // A TCP connection's share of a turn: the others are served too.
constexpr auto closingWait = 1s; // What a closing TCP connection may take to write what waits, and to be closed.
constexpr auto lastWords = 250ms; // The same once the thread is gone, on the thread that closes it.
constexpr auto turnWait = 10ms;
constexpr auto descriptorPause = 250ms; // No descriptor for the next peer: tried again after this.

std::optional<uint32_t> idOf(std::span<const uint8_t> datagram) {
    if (datagram.size() < connectionIdOffset + 4 || datagram[0] != dataKind) {
        return std::nullopt;
    }
    return uint32_t(loadBigEndian(datagram, connectionIdOffset, 4));
}

} // namespace

class TcpDemuxSocket;

// A datagram that waits to be read, or (closed) the news that the TCP
// connection from `from` closed.
struct TcpDatagram {
    Endpoint from;
    Bytes bytes;
    std::optional<std::string> closed;
};

struct TcpDemux::Shared {
    // One TCP connection to the port.
    struct Tcp {
        std::unique_ptr<transport::TcpStream> stream;
        Endpoint peer;
        // Since, and when a connection last sent over it: only where something
        // it authenticated came from, so no one makes it send there without
        // its keys.
        Clock::time_point opened, used{};
        bool spoke = false;        // A datagram came over it.
        bool handshake = false;    // A handshake's datagram came over it.
        std::vector<uint32_t> ids; // The connections it belongs to.
        std::unique_ptr<transport::Framing> frames = transport::lengthFraming();
        Bytes outbound; // Written in order before anything new.
        // Since: nothing more is routed; once what waits went, this end's
        // side is shut (`finished`), and it goes when the peer closed its
        // side too, or closingWait passed.
        std::optional<Clock::time_point> closing;
        bool finished = false;
        bool broken = false; // Nothing more goes over it: it goes at once.
    };

    Shared(const Endpoint& at, std::chrono::milliseconds timeout, std::function<void(const Failure&)> report)
        : listener(std::make_unique<transport::TcpListenSocket>(at.address(), at.port())),
          local(transport::localAddress(listener->handle())), handshakeTimeout(timeout), rejected(std::move(report)) {}

    std::unique_ptr<transport::TcpListenSocket> listener; // None once the demultiplexer went.
    const Endpoint local;
    const std::chrono::milliseconds handshakeTimeout;
    const std::function<void(const Failure&)> rejected;
    std::mutex mutex; // Below, and the sockets' queues.
    std::map<uint64_t, std::unique_ptr<Tcp>> tcp; // By the order they came.
    uint64_t next = 1;
    std::vector<std::pair<uint32_t, TcpDemuxSocket*>> routes; // Each connection's id.
    TcpDemuxSocket* handshakes = nullptr;
    bool stopped = false; // The thread is gone: nothing reads or writes for the sockets any more.
    std::atomic<bool> stopping{false};

    // Holding the mutex.
    Tcp* to(const Endpoint& peer);
    void route(Tcp& c, Bytes datagram);
    void claim(Tcp& c, uint32_t id);
    // `c` closes (`broken`: at once, else once what waits went out): the
    // connections it belonged to, and the handshake socket, hear of it.
    void close(Tcp& c, const std::string& why, bool broken, Clock::time_point now);
    void flush(Tcp& c);
    void reject(const Tcp& c, const std::string& why) const;
};

namespace {

// A closing TCP connection without the thread: what waits goes out, this
// end's side is shut, and what the peer still sends is read until it closes
// its side, lastWords at most.
void finishNow(TcpDemux::Shared::Tcp& c) {
    try {
        c.stream->writeFinal(c.outbound, lastWords);
    } catch (const Failure&) {
    }
}

} // namespace

// A connection's share of the port, or the handshake socket: it receives what
// the thread hands it, and its sends go to the TCP connection of the address.
class TcpDemuxSocket final : public transport::DatagramSocket {
public:
    TcpDemuxSocket(std::shared_ptr<TcpDemux::Shared> shared, std::optional<uint32_t> id, size_t limit)
        : shared_(std::move(shared)), id_(id), limit_(limit) {}

    ~TcpDemuxSocket() override {
        auto& s = *shared_;
        std::vector<std::unique_ptr<TcpDemux::Shared::Tcp>> last; // Closed here: no thread does.
        {
            std::lock_guard lock(s.mutex);
            if (!id_) {
                if (s.handshakes == this) {
                    s.handshakes = nullptr;
                }
                return;
            }
            std::erase_if(s.routes, [&](const auto& route) { return route.second == this; });
            if (std::any_of(s.routes.begin(), s.routes.end(), [&](const auto& r) { return r.first == *id_; })) {
                return; // Another connection of the id (1 in 2^32) still has them.
            }
            const auto now = Clock::now();
            for (auto it = s.tcp.begin(); it != s.tcp.end();) {
                auto& c = *it->second;
                if (!std::erase(c.ids, *id_) || !c.ids.empty() || c.closing) {
                    ++it;
                } else if (s.stopped) {
                    last.push_back(std::move(it->second));
                    it = s.tcp.erase(it);
                } else {
                    c.closing = now;
                    try { // Its last words (the connection's goodbye) at once; the thread does the rest.
                        s.flush(c);
                    } catch (const Failure&) {
                        c.broken = true;
                    }
                    ++it;
                }
            }
        }
        for (auto& c : last) {
            finishNow(*c);
        }
    }

    Endpoint local() const override { return shared_->local; }
    transport::Handle handle() const noexcept override { return transport::invalidHandle; }

    bool send(const Endpoint& to, std::span<const uint8_t> datagram) override {
        const transport::Outgoing one{&to, datagram};
        std::lock_guard lock(shared_->mutex);
        return write(to, {&one, 1}) == 1;
    }
    // Each run to one address in one write.
    size_t sendBatch(std::span<const transport::Outgoing> batch) override {
        std::lock_guard lock(shared_->mutex);
        for (size_t at = 0, end; at < batch.size(); at = end) {
            const Endpoint& to = *batch[at].to;
            for (end = at + 1; end < batch.size() && (batch[end].to == &to || *batch[end].to == to); ++end) {
            }
            write(to, batch.subspan(at, end - at));
        }
        return 0; // Unmarked: inside the stream.
    }

    std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint& from) override {
        std::lock_guard lock(shared_->mutex);
        while (!queue_.empty()) {
            auto datagram = std::move(queue_.front());
            queue_.pop_front();
            queuedBytes_ -= datagram.bytes.size();
            if (datagram.closed) {
                throw transport::PeerClosed(datagram.from, *datagram.closed);
            }
            if (datagram.bytes.size() > buffer.size()) {
                continue; // As a socket drops a datagram larger than the buffer.
            }
            std::copy(datagram.bytes.begin(), datagram.bytes.end(), buffer.begin());
            from = datagram.from;
            return datagram.bytes.size();
        }
        return std::nullopt;
    }

    bool buffered() const noexcept override {
        std::lock_guard lock(shared_->mutex);
        return !queue_.empty();
    }
    bool attachWaiter(transport::Waiter& waiter) const noexcept override {
        std::lock_guard lock(shared_->mutex);
        waiters_.push_back(&waiter);
        return true;
    }
    void detachWaiter(transport::Waiter& waiter) const noexcept override {
        std::lock_guard lock(shared_->mutex);
        std::erase(waiters_, &waiter);
    }

    // Holding the shared mutex. The news of a closed connection always fits.
    void push(TcpDatagram datagram) {
        if (!datagram.closed && queuedBytes_ + datagram.bytes.size() > limit_) {
            return;
        }
        queuedBytes_ += datagram.bytes.size();
        queue_.push_back(std::move(datagram));
        for (auto* waiter : waiters_) {
            waiter->wake();
        }
    }
    // The handshake socket: what waits of connection `id` goes to `socket`.
    void hand(uint32_t id, TcpDemuxSocket& socket) {
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (!it->closed && idOf(it->bytes) == id) {
                queuedBytes_ -= it->bytes.size();
                socket.push(std::move(*it));
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    // Holding the shared mutex: `datagrams` to the TCP connection of `to`,
    // each behind its length, written at once as far as the system takes
    // them; how many it took. Before one that would take what waits beyond
    // maxOutbound, what waits is written (no more once the system took less
    // than all); one that still does not fit is dropped, as a full socket
    // drops it (the connection's resends and adaptive bitrate take it from
    // there), and later ones that fit go on.
    size_t write(const Endpoint& to, std::span<const transport::Outgoing> datagrams) {
        auto& s = *shared_;
        auto* c = s.to(to);
        if (!c || c->broken) {
            return 0; // That TCP connection closed: nothing reaches the peer there any more.
        }
        if (id_) {
            c->used = Clock::now();
            s.claim(*c, *id_);
        }
        size_t taken = 0;
        try {
            bool full = false;
            for (const auto& outgoing : datagrams) {
                const auto datagram = outgoing.datagram;
                const auto fits = [&] {
                    return c->outbound.empty() || c->outbound.size() + 2 + datagram.size() <= TcpDemux::maxOutbound;
                };
                if (!fits() && !full) {
                    s.flush(*c);
                    full = !c->outbound.empty();
                }
                if (!fits()) {
                    continue;
                }
                appendBigEndian(c->outbound, datagram.size(), 2);
                c->outbound.insert(c->outbound.end(), datagram.begin(), datagram.end());
                ++taken;
            }
            s.flush(*c);
        } catch (const Failure& failure) {
            s.close(*c, std::string("sending over it failed: ") + failure.what(), true, Clock::now());
            return 0;
        }
        return taken;
    }

    const std::shared_ptr<TcpDemux::Shared> shared_;
    const std::optional<uint32_t> id_; // None: the handshake socket.
    const size_t limit_;
    // Under the shared mutex.
    std::deque<TcpDatagram> queue_;
    size_t queuedBytes_ = 0;
    mutable std::vector<transport::Waiter*> waiters_;
};

TcpDemux::Shared::Tcp* TcpDemux::Shared::to(const Endpoint& peer) {
    Tcp* found = nullptr;
    for (auto& [number, c] : tcp) { // The newest of that address: an older one is gone.
        if (c->peer == peer && !c->closing) {
            found = c.get();
        }
    }
    return found;
}

void TcpDemux::Shared::flush(Tcp& c) {
    size_t written = 0;
    while (written < c.outbound.size()) {
        const size_t taken = c.stream->writeSome(std::span<const uint8_t>(c.outbound).subspan(written));
        if (!taken) {
            break;
        }
        written += taken;
    }
    c.outbound.erase(c.outbound.begin(), c.outbound.begin() + ptrdiff_t(written));
}

void TcpDemux::Shared::reject(const Tcp& c, const std::string& why) const {
    if (rejected) {
        rejected(Failure(RCS_ERROR_NETWORK, c.peer.toString() + ": " + why));
    }
}

void TcpDemux::Shared::close(Tcp& c, const std::string& why, bool broken, Clock::time_point now) {
    if (!c.closing) {
        c.closing = now;
        const std::string news = "The TCP connection from " + c.peer.toString() + " closed: " + why;
        for (const uint32_t id : c.ids) {
            for (const auto& [routeId, socket] : routes) {
                if (routeId == id) {
                    socket->push({c.peer, {}, news});
                }
            }
        }
        if (c.handshake && handshakes) {
            handshakes->push({c.peer, {}, news});
        }
    }
    c.broken = c.broken || broken;
}

void TcpDemux::Shared::claim(Tcp& c, uint32_t id) {
    if (std::find(c.ids.begin(), c.ids.end(), id) != c.ids.end()) {
        return;
    }
    c.ids.push_back(id);
    std::vector<Tcp*> its; // The connection's TCP connections.
    for (auto& [number, other] : tcp) {
        if (!other->closing && std::find(other->ids.begin(), other->ids.end(), id) != other->ids.end()) {
            its.push_back(other.get());
        }
    }
    if (its.size() <= perConnection) {
        return;
    }
    std::erase(its, &c);
    Tcp& oldest =
        **std::min_element(its.begin(), its.end(), [](const Tcp* a, const Tcp* b) { return a->used < b->used; });
    close(oldest, "its connection has " + std::to_string(perConnection) + " newer ones", false, Clock::now());
}

void TcpDemux::Shared::route(Tcp& c, Bytes datagram) {
    c.spoke = true;
    const uint8_t kind = datagram[0];
    if (kind != clientHelloKind && kind != finishedKind && kind != dataKind) {
        return; // No client of this protocol sends it.
    }
    if (const auto id = idOf(datagram)) {
        bool routed = false;
        for (const auto& [routeId, socket] : routes) {
            if (routeId == *id) {
                socket->push({c.peer, datagram, std::nullopt});
                routed = true;
            }
        }
        if (routed) {
            return;
        }
    } else {
        c.handshake = true;
    }
    if (handshakes) {
        handshakes->push({c.peer, std::move(datagram), std::nullopt});
    }
}

TcpDemux::TcpDemux(const Endpoint& local, std::chrono::milliseconds handshakeTimeout,
                   std::function<void(const Failure&)> rejected)
    : shared_(std::make_shared<Shared>(local, handshakeTimeout, std::move(rejected))) {
    thread_ = log::thread([this] { run(); });
}

TcpDemux::~TcpDemux() {
    shared_->stopping = true;
    thread_.join();
    std::vector<std::unique_ptr<Shared::Tcp>> closing;
    {
        std::lock_guard lock(shared_->mutex);
        shared_->stopped = true;
        shared_->listener.reset();
        for (auto it = shared_->tcp.begin(); it != shared_->tcp.end();) {
            auto& c = it->second;
            if (!c->ids.empty() && !c->closing) {
                ++it;
                continue;
            }
            if (c->closing && !c->broken) {
                closing.push_back(std::move(c));
            }
            it = shared_->tcp.erase(it);
        }
    }
    for (auto& c : closing) {
        finishNow(*c);
    }
}

Endpoint TcpDemux::local() const {
    return shared_->local;
}

size_t TcpDemux::connections() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->tcp.size();
}

std::unique_ptr<transport::DatagramSocket> TcpDemux::handshakes() {
    auto socket = std::make_unique<TcpDemuxSocket>(shared_, std::nullopt, maxHandshakeBytes);
    std::lock_guard lock(shared_->mutex);
    shared_->handshakes = socket.get();
    return socket;
}

std::unique_ptr<transport::DatagramSocket> TcpDemux::connection(uint32_t id, const Endpoint& from) {
    auto socket = std::make_unique<TcpDemuxSocket>(shared_, id, maxQueued);
    auto& s = *shared_;
    std::lock_guard lock(s.mutex);
    s.routes.emplace_back(id, socket.get());
    if (s.handshakes) { // Its data that came before the connection did.
        s.handshakes->hand(id, *socket);
    }
    if (auto* c = s.to(from)) {
        c->used = Clock::now();
        s.claim(*c, id);
    }
    return socket;
}

void TcpDemux::run() noexcept {
    auto& s = *shared_;
    Bytes chunk(64 * 1024);
    Clock::time_point paused{};
    bool starved = false;
    while (!s.stopping) {
        std::vector<transport::Readiness> waiting;
        {
            std::lock_guard lock(s.mutex);
            if (Clock::now() >= paused) {
                waiting.push_back({s.listener->handle(), false});
            }
            for (const auto& [number, c] : s.tcp) {
                if (!c->broken) {
                    waiting.push_back({c->stream->handle(), false});
                    if (!c->outbound.empty()) {
                        waiting.push_back({c->stream->handle(), true});
                    }
                }
            }
        }
        transport::waitAny(waiting, turnWait);
        const auto now = Clock::now();
        std::lock_guard lock(s.mutex);
        while (now >= paused) { // Those that came.
            std::unique_ptr<transport::TcpStream> stream;
            try {
                stream = s.listener->tryAccept(s.stopping);
            } catch (const transport::NoDescriptor& failure) {
                if (!starved) {
                    log::warning("network", std::string(failure.what()) + "; another viewer waits until one is free");
                }
                starved = true;
                paused = now + descriptorPause;
                break;
            } catch (const Failure& failure) {
                log::warning("network", std::string("Accepting a TCP connection failed: ") + failure.what());
                break;
            }
            if (!stream) {
                break;
            }
            starved = false;
            auto c = std::make_unique<Shared::Tcp>();
            try {
                c->peer = stream->peer();
            } catch (const Failure&) {
                continue; // Gone before it was looked at.
            }
            c->stream = std::move(stream);
            c->opened = now;
            Shared::Tcp* oldest = nullptr;
            size_t unclaimed = 0;
            for (auto& [number, other] : s.tcp) {
                if (other->ids.empty() && !other->closing) {
                    oldest = oldest ? oldest : other.get();
                    ++unclaimed;
                }
            }
            if (unclaimed >= maxUnclaimed) {
                s.reject(*oldest, "Dropped while handshaking: " + std::to_string(maxUnclaimed) +
                                      " newer connections came");
                s.close(*oldest, "newer connections came", true, now);
            }
            s.tcp.emplace(s.next++, std::move(c));
        }
        for (auto it = s.tcp.begin(); it != s.tcp.end();) {
            auto& c = *it->second;
            if (!c.broken) {
                try {
                    for (int i = 0; i < readsPerTurn; ++i) {
                        const size_t taken = c.stream->readSome(chunk);
                        if (!taken) {
                            break;
                        }
                        if (c.closing) {
                            continue; // What comes after this end closed: read only for the peer to close too.
                        }
                        c.frames->push(std::span<const uint8_t>(chunk).first(taken));
                        Bytes none;
                        while (auto datagram = c.frames->pop(none)) {
                            s.route(c, std::move(*datagram));
                        }
                    }
                    s.flush(c);
                    if (c.closing && c.outbound.empty() && !c.finished) {
                        c.stream->finish();
                        c.finished = true;
                    }
                } catch (const Failure& failure) { // The peer closed, it broke, or its bytes broke the framing.
                    if (!c.closing && !c.spoke && c.ids.empty()) {
                        s.reject(c, failure.what());
                    }
                    s.close(c, failure.what(), true, now);
                }
            }
            if (!c.closing && c.ids.empty()) {
                if (!c.spoke && now - c.opened > helloWait) {
                    s.reject(c, "No handshake within " + secondsText(helloWait) + " of connecting");
                    s.close(c, "no handshake", true, now);
                } else if (now - c.opened > helloWait + s.handshakeTimeout) {
                    s.close(c, "no handshake completed over it", true, now); // UdpAcceptor reported why.
                }
            }
            if (c.broken || (c.closing && now - *c.closing > closingWait)) {
                it = s.tcp.erase(it);
            } else {
                ++it;
            }
        }
    }
}

} // namespace rcsdk::protocol
