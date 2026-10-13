#include "entry.hpp"

#include "util/bytes.hpp"

#include <algorithm>

namespace rcsdk::forwarder {

namespace {

constexpr int readsPerTurn = 4;  // A connection's share of a turn: others are served too.
constexpr auto closingWait = 1s; // What a closing connection may take to write what waits.
constexpr auto turnWait = 10ms;

} // namespace

EntryReader::EntryReader(EntryKind kind) {
    if (kind == EntryKind::tcp) {
        lengths_ = transport::lengthFraming();
        upgraded_ = true;
    }
}

bool EntryReader::take(std::span<const uint8_t> bytes, std::vector<Bytes>& datagrams, Bytes& outbound) {
    namespace ws = transport::websocket;
    const auto refuse = [&](std::span<const uint8_t> last) {
        outbound.insert(outbound.end(), last.begin(), last.end());
        refused_ = done_ = true;
        return false;
    };
    if (done_) {
        return false;
    }
    if (lengths_) {
        lengths_->push(bytes);
        Bytes none;
        while (auto datagram = lengths_->pop(none)) {
            datagrams.push_back(std::move(*datagram));
        }
        return true;
    }
    if (!upgraded_) {
        opening_.insert(opening_.end(), bytes.begin(), bytes.end());
        std::optional<ws::Accepted> accepted;
        try {
            accepted = ws::accept(opening_);
        } catch (const ws::Refused& refused) {
            const auto& answer = refused.response();
            return refuse(std::span(reinterpret_cast<const uint8_t*>(answer.data()), answer.size()));
        }
        if (!accepted) {
            return true;
        }
        const auto& answer = accepted->response;
        outbound.insert(outbound.end(), answer.begin(), answer.end());
        upgraded_ = true;
        frames_.emplace(true);
        bytes = std::span<const uint8_t>(opening_).subspan(accepted->consumed); // Frames that came with it.
    }
    try {
        frames_->push(bytes);
        opening_ = {};
        while (auto event = frames_->next()) {
            if (event->kind == ws::binary) {
                datagrams.push_back(std::move(event->payload));
            } else if (event->kind == ws::ping) {
                // Answered while what waits for the peer is within the
                // entry's bound: one that does not read its pongs gets no
                // more (RFC 6455 lets an end answer the latest ping only).
                const auto pong = ws::frame(ws::pong, event->payload, false);
                if (outbound.size() + pong.size() <= Entry::maxOutbound) {
                    outbound.insert(outbound.end(), pong.begin(), pong.end());
                }
            } else { // A close, answered with its code.
                const auto answer = ws::frame(
                    ws::close, std::span(event->payload).first(std::min<size_t>(event->payload.size(), 2)), false);
                outbound.insert(outbound.end(), answer.begin(), answer.end());
                done_ = true;
                return false;
            }
        }
    } catch (const ws::Violation& violation) {
        return refuse(ws::closeFrame(violation.code()));
    }
    return true;
}

struct Entry::Connection {
    explicit Connection(Kind kind) : reader(kind) {}
    uint64_t route = 0;
    std::optional<transport::Endpoint> peer;
    Clock::time_point opened = Clock::now();
    std::mutex mutex; // Below: the hop's thread writes, the entry's thread reads and writes.
    std::unique_ptr<transport::TcpStream> stream;
    bool claimed = false;
    std::optional<Clock::time_point> closing; // Since: nothing more is read or taken.
    EntryReader reader;
    Bytes outbound;
};

Entry::Entry(Kind kind, const transport::Endpoint& listen, size_t maxConnections)
    : kind_(kind), maxConnections_(maxConnections), listener_(listen.address(), listen.port()) {
    port_ = transport::localAddress(listener_.handle()).port();
    routes_ = std::make_unique<protocol::RouteDemux>(
        protocol::RouteDemux::Kind::datagram,
        [this](uint64_t route, std::span<const uint8_t> datagram) {
            const transport::Outgoing one{nullptr, datagram};
            return send(route, {&one, 1}) == 1;
        },
        [this](uint64_t route, std::span<const transport::Outgoing> datagrams) { return send(route, datagrams); });
    thread_ = std::thread([this] { run(); });
}

Entry::~Entry() {
    stopping_ = true;
    thread_.join();
}

std::shared_ptr<Entry::Connection> Entry::find(uint64_t route) const {
    std::lock_guard lock(mutex_);
    const auto found = connections_.find(route);
    return found == connections_.end() ? nullptr : found->second;
}

std::optional<transport::Endpoint> Entry::peer(uint64_t route) const {
    const auto c = find(route);
    return c ? c->peer : std::nullopt;
}

void Entry::claim(uint64_t route) {
    if (const auto c = find(route)) {
        std::lock_guard lock(c->mutex);
        c->claimed = true;
    }
}

void Entry::close(uint64_t route) {
    const auto found = find(route);
    if (!found) {
        return;
    }
    auto& c = *found;
    std::lock_guard lock(c.mutex);
    if (c.closing) {
        return;
    }
    if (kind_ == Kind::websocket && c.reader.upgraded()) {
        const auto bye = transport::websocket::closeFrame(transport::websocket::goingAway);
        c.outbound.insert(c.outbound.end(), bye.begin(), bye.end());
    }
    c.closing = Clock::now();
}

size_t Entry::connections() const {
    std::lock_guard lock(mutex_);
    return connections_.size();
}

size_t Entry::send(uint64_t route, std::span<const transport::Outgoing> datagrams) {
    const auto found = find(route);
    if (!found) {
        return 0;
    }
    auto& c = *found;
    std::lock_guard lock(c.mutex);
    if (c.closing || !c.reader.upgraded()) {
        return 0;
    }
    size_t taken = 0;
    try {
        bool full = false;
        for (const auto& outgoing : datagrams) {
            const auto datagram = outgoing.datagram;
            // TCP: behind its length; a WebSocket: a binary message, its header 2 bytes, 4 from 126 on.
            const size_t wire = datagram.size() + (kind_ == Kind::tcp || datagram.size() < 126 ? 2 : 4);
            const auto fits = [&] { return c.outbound.empty() || c.outbound.size() + wire <= maxOutbound; };
            if (!fits() && !full) {
                flush(c);
                full = !c.outbound.empty();
            }
            if (!fits()) {
                continue; // The peer reads slower than the hop sends: dropped, as a full socket drops it.
            }
            if (kind_ == Kind::tcp) {
                appendBigEndian(c.outbound, datagram.size(), 2);
                c.outbound.insert(c.outbound.end(), datagram.begin(), datagram.end());
            } else {
                transport::websocket::frame(c.outbound, transport::websocket::binary, datagram, false);
            }
            ++taken;
        }
        flush(c);
    } catch (const Failure&) {
        c.closing = Clock::now(); // Broken: the entry's thread ends it.
    }
    return taken;
}

void Entry::flush(Connection& c) {
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

void Entry::read(Connection& c, std::vector<Bytes>& datagrams, Clock::time_point now) {
    for (int i = 0; i < readsPerTurn && !c.closing; ++i) {
        const size_t taken = c.stream->readSome(chunk_); // Throws once the peer closed or the connection broke.
        if (!taken) {
            return;
        }
        if (!c.reader.take(std::span<const uint8_t>(chunk_).first(taken), datagrams, c.outbound)) {
            c.closing = now;
            refused_ += c.reader.refused();
        }
    }
}

void Entry::run() noexcept {
    const auto open = [this] {
        std::vector<std::shared_ptr<Connection>> list;
        std::lock_guard lock(mutex_);
        for (const auto& [route, c] : connections_) {
            list.push_back(c);
        }
        return list;
    };
    while (!stopping_) {
        std::vector<transport::Readiness> waiting{{listener_.handle(), false}};
        for (const auto& c : open()) {
            std::lock_guard lock(c->mutex);
            if (!c->closing) {
                waiting.push_back({c->stream->handle(), false});
            }
            if (!c->outbound.empty()) {
                waiting.push_back({c->stream->handle(), true});
            }
        }
        transport::waitAny(waiting, std::chrono::duration_cast<std::chrono::milliseconds>(turnWait));
        const auto now = Clock::now();
        for (;;) { // Those that came: one beyond the limit is closed at once.
            std::unique_ptr<transport::TcpStream> stream;
            try {
                stream = listener_.tryAccept(stopping_);
            } catch (const Failure&) {
                break; // No descriptor for it now: the next turn tries again.
            }
            if (!stream) {
                break;
            }
            std::lock_guard lock(mutex_);
            if (connections_.size() >= maxConnections_) {
                ++refused_;
                continue;
            }
            auto c = std::make_shared<Connection>(kind_);
            c->route = nextRoute_++;
            try {
                c->peer = stream->peer();
            } catch (const Failure&) {
            }
            c->stream = std::move(stream);
            connections_.emplace(c->route, std::move(c));
        }
        std::vector<std::pair<uint64_t, std::vector<Bytes>>> arrived;
        std::vector<uint64_t> gone;
        for (const auto& connection : open()) {
            auto& c = *connection;
            std::lock_guard lock(c.mutex);
            bool done = false;
            std::vector<Bytes> datagrams;
            try {
                read(c, datagrams, now);
                flush(c);
            } catch (const Failure&) {
                done = true; // The peer closed, the connection broke, or its bytes broke the framing.
            }
            if (!c.closing &&
                (!c.reader.upgraded() ? now - c.opened > openWait : !c.claimed && now - c.opened > claimWait)) {
                ++refused_;
                done = true;
            }
            done = done || (c.closing && (c.outbound.empty() || now - *c.closing > closingWait));
            if (!datagrams.empty()) {
                arrived.emplace_back(c.route, std::move(datagrams));
            }
            if (done) {
                gone.push_back(c.route);
                c.closing = c.closing.value_or(now); // A hop that still writes to it: nothing more is taken.
            }
        }
        if (!gone.empty()) {
            std::lock_guard lock(mutex_);
            for (const uint64_t route : gone) {
                connections_.erase(route);
            }
        }
        for (auto& [route, datagrams] : arrived) {
            for (const auto& datagram : datagrams) {
                try {
                    routes_->receive(route, datagram);
                } catch (const Failure&) {
                    // The hop fell behind, or too many routes wait: dropped, as a network drops it.
                }
            }
        }
        for (const uint64_t route : gone) {
            routes_->closed(route);
        }
    }
}

} // namespace rcsdk::forwarder
