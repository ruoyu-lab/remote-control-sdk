#include "protocol/route_demux.hpp"

#include "protocol/udp_connection.hpp"
#include "util/bytes.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace rcsdk::protocol {
namespace {

using transport::Endpoint;

constexpr size_t frameHeader = 2;
constexpr size_t maxDatagram = 65535;

// A route's stand-in: an address of the discard-only prefix 100::/64 (RFC
// 6666), which no peer has, with the route in its last 64 bits.
constexpr uint8_t prefix[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};

} // namespace

class RouteSocket;

// A datagram of a route that waits to be read.
struct RouteDatagram {
    uint64_t route = 0;
    std::vector<uint8_t> bytes;
};

struct RouteDemux::Shared {
    Shared(Kind k, Send s, SendBatch b) : kind(k), send(std::move(s)), sendBatch(std::move(b)) {}

    struct Route {
        std::vector<uint8_t> partial; // Stream: the start of a frame still arriving.
        RouteSocket* socket = nullptr; // The connection on it.
        size_t waiting = 0;            // Its bytes on the handshake socket.
    };

    const Kind kind;
    const Send send;
    const SendBatch sendBatch;
    std::mutex mutex; // Below, and the sockets' queues.
    std::map<uint64_t, Route> routes;
    RouteSocket* handshakes = nullptr;
    std::atomic<uint64_t> refused{0};

    // Holding the mutex.
    void deliver(uint64_t route, std::vector<uint8_t> datagram);
    void drop(uint64_t route);    // What waits of it, and its partial frame.
    void forget(uint64_t route);  // Its entry, once nothing is left of it.
    Route& at(uint64_t route);    // Throws QUEUE_FULL when too many wait.
};

// A route's connection, or the handshake socket.
class RouteSocket final : public transport::DatagramSocket {
public:
    // `route`: its connection's; none: the handshake socket.
    RouteSocket(std::shared_ptr<RouteDemux::Shared> shared, std::optional<uint64_t> route)
        : shared_(std::move(shared)), route_(route) {}
    ~RouteSocket() override {
        std::lock_guard lock(shared_->mutex);
        if (route_) {
            if (const auto found = shared_->routes.find(*route_);
                found != shared_->routes.end() && found->second.socket == this) {
                found->second.socket = nullptr;
                shared_->forget(*route_);
            }
        } else if (shared_->handshakes == this) {
            shared_->handshakes = nullptr;
            std::erase_if(shared_->routes, [](auto& entry) {
                entry.second.waiting = 0;
                return !entry.second.socket && entry.second.partial.empty();
            });
        }
    }

    Endpoint local() const override { return RouteDemux::endpoint(0); }
    transport::Handle handle() const noexcept override { return transport::invalidHandle; }

    bool send(const Endpoint& to, std::span<const uint8_t> datagram) override {
        const auto route = RouteDemux::routeOf(to);
        if (!route || datagram.size() > maxDatagram) {
            fail(RCS_ERROR_INTERNAL, "A datagram for no route, or too large for the channel");
        }
        if (closed_) {
            return false; // The route is the next peer's now: nothing of this connection goes there.
        }
        bool taken;
        if (shared_->kind == RouteDemux::Kind::stream) {
            std::vector<uint8_t> frame(frameHeader + datagram.size());
            storeBigEndian(frame, 0, datagram.size(), frameHeader);
            std::copy(datagram.begin(), datagram.end(), frame.begin() + frameHeader);
            taken = shared_->send(*route, frame);
        } else {
            taken = shared_->send(*route, datagram);
        }
        if (!taken) {
            ++shared_->refused;
        }
        return taken;
    }
    // Each run to one route at once, where the channel takes batches.
    size_t sendBatch(std::span<const transport::Outgoing> batch) override {
        if (!shared_->sendBatch) {
            return DatagramSocket::sendBatch(batch);
        }
        for (size_t at = 0, end; at < batch.size(); at = end) {
            const Endpoint& to = *batch[at].to;
            const auto route = RouteDemux::routeOf(to);
            for (end = at; end < batch.size() && (batch[end].to == &to || *batch[end].to == to); ++end) {
                if (!route || batch[end].datagram.size() > maxDatagram) {
                    fail(RCS_ERROR_INTERNAL, "A datagram for no route, or too large for the channel");
                }
            }
            if (!closed_) {
                shared_->refused += end - at - shared_->sendBatch(*route, batch.subspan(at, end - at));
            }
        }
        return 0; // Unmarked: inside the channel.
    }

    // Throws NETWORK once when the channel to its route closed, after what
    // came before that (a goodbye among it).
    std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint& from) override {
        std::lock_guard lock(shared_->mutex);
        while (!queue_.empty()) {
            auto datagram = std::move(queue_.front());
            queue_.pop_front();
            queuedBytes_ -= datagram.bytes.size();
            if (!route_) {
                take(datagram.route, datagram.bytes.size());
            }
            if (datagram.bytes.size() > buffer.size()) {
                continue; // As a socket drops a datagram larger than the buffer.
            }
            std::copy(datagram.bytes.begin(), datagram.bytes.end(), buffer.begin());
            from = RouteDemux::endpoint(datagram.route);
            return datagram.bytes.size();
        }
        if (broken_) {
            broken_ = false;
            fail(RCS_ERROR_NETWORK, "The channel to the other end closed");
        }
        return std::nullopt;
    }

    bool buffered() const noexcept override {
        std::lock_guard lock(shared_->mutex);
        return broken_ || !queue_.empty();
    }
    uint64_t restarts() const noexcept override { return restarts_.load(); }
    bool attachWaiter(transport::Waiter& waiter) const noexcept override {
        std::lock_guard lock(shared_->mutex);
        waiters_.push_back(&waiter);
        return true;
    }
    void detachWaiter(transport::Waiter& waiter) const noexcept override {
        std::lock_guard lock(shared_->mutex);
        std::erase(waiters_, &waiter);
    }

    // Holding the shared mutex. False when it has no room.
    bool push(uint64_t route, std::vector<uint8_t> bytes, size_t limit) {
        if (queuedBytes_ + bytes.size() > limit) {
            return false;
        }
        queuedBytes_ += bytes.size();
        queue_.push_back({route, std::move(bytes)});
        wake();
        return true;
    }
    // The connection on its route ends: its next receive throws, once what
    // came before is taken (nothing after that is: it does not keep the
    // connection going).
    bool broken() const noexcept { return closed_; }
    void breakConnection() {
        broken_ = closed_ = true;
        wake();
    }
    void restarted() {
        ++restarts_;
        wake();
    }
    // The handshake socket: `route`'s datagrams go to `socket`, the
    // connection's own, those of its data first (in order).
    void hand(uint64_t route, RouteSocket& socket) {
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (it->route == route) {
                if (!it->bytes.empty() && it->bytes[0] == dataKind) {
                    socket.push(route, it->bytes, RouteDemux::maxQueued);
                }
                queuedBytes_ -= it->bytes.size();
                take(route, it->bytes.size());
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // The handshake socket: what waits of `route` goes.
    void dropRoute(uint64_t route) {
        std::erase_if(queue_, [&](const RouteDatagram& w) {
            if (w.route != route) {
                return false;
            }
            queuedBytes_ -= w.bytes.size();
            return true;
        });
    }

private:
    // The handshake socket gave a datagram of `route` away.
    void take(uint64_t route, size_t bytes) {
        if (const auto found = shared_->routes.find(route); found != shared_->routes.end()) {
            found->second.waiting -= std::min(found->second.waiting, bytes);
            shared_->forget(route);
        }
    }
    void wake() const noexcept {
        for (auto* waiter : waiters_) {
            waiter->wake();
        }
    }

    const std::shared_ptr<RouteDemux::Shared> shared_;
    const std::optional<uint64_t> route_;
    // Under the shared mutex.
    std::deque<RouteDatagram> queue_;
    size_t queuedBytes_ = 0;
    bool broken_ = false;
    std::atomic<bool> closed_{false}; // Its channel closed: it sends nothing more (broken_ is told once).
    mutable std::vector<transport::Waiter*> waiters_;
    std::atomic<uint64_t> restarts_{0};
};

void RouteDemux::Shared::forget(uint64_t route) {
    const auto found = routes.find(route);
    if (found != routes.end() && !found->second.socket && !found->second.waiting && found->second.partial.empty()) {
        routes.erase(found);
    }
}

RouteDemux::Shared::Route& RouteDemux::Shared::at(uint64_t route) {
    if (const auto found = routes.find(route); found != routes.end()) {
        return found->second;
    }
    const size_t idle = size_t(std::count_if(routes.begin(), routes.end(), [](const auto& r) { return !r.second.socket; }));
    if (idle >= maxRoutes) {
        fail(RCS_ERROR_QUEUE_FULL, "Too many routes wait for their handshake (" + std::to_string(maxRoutes) +
                                       "); route " + std::to_string(route) + " is dropped");
    }
    return routes[route];
}

void RouteDemux::Shared::deliver(uint64_t route, std::vector<uint8_t> datagram) {
    auto& state = routes.at(route);
    const bool data = datagram[0] == dataKind;
    if (data && state.socket) {
        if (state.socket->broken()) {
            return;
        }
        if (!state.socket->push(route, std::move(datagram), maxQueued)) {
            fail(RCS_ERROR_QUEUE_FULL, "The session fell behind what the channel delivers; the datagram is dropped");
        }
        return;
    }
    if (!handshakes || state.waiting + datagram.size() > maxWaitingPerRoute) {
        return; // Nothing takes handshakes now, or this route sent plenty: as a full socket drops it.
    }
    const size_t size = datagram.size();
    if (handshakes->push(route, std::move(datagram), maxWaiting)) {
        state.waiting += size;
    }
}

void RouteDemux::Shared::drop(uint64_t route) {
    const auto found = routes.find(route);
    if (found == routes.end()) {
        return;
    }
    found->second.partial.clear();
    if (handshakes && found->second.waiting) {
        handshakes->dropRoute(route);
    }
    found->second.waiting = 0;
}

RouteDemux::RouteDemux(Kind kind, Send send, SendBatch sendBatch)
    : shared_(std::make_shared<Shared>(kind, std::move(send), std::move(sendBatch))) {}

RouteDemux::~RouteDemux() = default;

RouteDemux::Kind RouteDemux::kind() const noexcept {
    return shared_->kind;
}

uint64_t RouteDemux::refused() const noexcept {
    return shared_->refused.load();
}

Endpoint RouteDemux::endpoint(uint64_t route) {
    uint8_t address[16] = {};
    std::copy(std::begin(prefix), std::end(prefix), address);
    storeBigEndian(std::span<uint8_t>(address), 8, route, 8);
    return Endpoint::fromBytes(address, 0);
}

std::optional<uint64_t> RouteDemux::routeOf(const Endpoint& address) {
    if (!address.ipv6() || address.port()) {
        return std::nullopt;
    }
    const auto bytes = address.addressBytes();
    if (!std::equal(std::begin(prefix), std::end(prefix), bytes.begin())) {
        return std::nullopt;
    }
    return loadBigEndian(bytes, 8, 8);
}

void RouteDemux::receive(uint64_t route, std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "No bytes: pass what arrived over the channel");
    }
    auto& s = *shared_;
    std::lock_guard lock(s.mutex);
    if (s.kind == Kind::datagram) {
        if (bytes.size() > maxDatagram) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "A datagram of " + std::to_string(bytes.size()) +
                                                 " bytes: the other end sends at most RCS_HOST_MAX_DATAGRAM "
                                                 "(pass each message as it came)");
        }
        s.at(route);
        try {
            s.deliver(route, std::vector<uint8_t>(bytes.begin(), bytes.end()));
        } catch (...) {
            s.forget(route);
            throw;
        }
        s.forget(route);
        return;
    }
    auto& state = s.at(route);
    auto& partial = state.partial;
    partial.insert(partial.end(), bytes.begin(), bytes.end());
    size_t at = 0;
    try {
        while (partial.size() - at >= frameHeader) {
            const size_t length = size_t(partial[at]) << 8 | partial[at + 1];
            if (partial.size() - at - frameHeader < length) {
                break;
            }
            const auto start = partial.begin() + ptrdiff_t(at + frameHeader);
            if (length) {
                s.deliver(route, std::vector<uint8_t>(start, start + ptrdiff_t(length)));
            }
            at += frameHeader + length;
        }
    } catch (...) {
        partial.clear(); // Bytes lost: what comes next is no frame boundary.
        if (state.socket) {
            state.socket->breakConnection();
        }
        s.forget(route);
        throw;
    }
    partial.erase(partial.begin(), partial.begin() + ptrdiff_t(at));
    s.forget(route);
}

void RouteDemux::closed(uint64_t route) {
    auto& s = *shared_;
    std::lock_guard lock(s.mutex);
    s.drop(route);
    if (const auto found = s.routes.find(route); found != s.routes.end() && found->second.socket) {
        found->second.socket->breakConnection();
        found->second.socket = nullptr; // What comes on the route from now on is a new peer's.
    }
    s.forget(route);
}

void RouteDemux::restarted(uint64_t route) {
    auto& s = *shared_;
    std::lock_guard lock(s.mutex);
    s.drop(route);
    if (const auto found = s.routes.find(route); found != s.routes.end() && found->second.socket) {
        found->second.socket->restarted();
    }
    s.forget(route);
}

std::unique_ptr<transport::DatagramSocket> RouteDemux::handshakes() {
    auto socket = std::make_unique<RouteSocket>(shared_, std::nullopt);
    std::lock_guard lock(shared_->mutex);
    shared_->handshakes = socket.get();
    return socket;
}

std::unique_ptr<transport::DatagramSocket> RouteDemux::connection(uint32_t, const Endpoint& from) {
    const auto route = routeOf(from);
    if (!route) {
        fail(RCS_ERROR_INTERNAL, "A connection from no route");
    }
    auto socket = std::make_unique<RouteSocket>(shared_, *route);
    auto& s = *shared_;
    std::lock_guard lock(s.mutex);
    auto& state = s.routes[*route];
    if (state.socket) {
        state.socket->breakConnection(); // Its peer connected anew on the route.
    }
    state.socket = socket.get();
    if (s.handshakes) {
        s.handshakes->hand(*route, *socket);
    }
    return socket;
}

} // namespace rcsdk::protocol
