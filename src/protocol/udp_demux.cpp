#include "protocol/udp_demux.hpp"

#include "protocol/udp_connection.hpp"
#include "util/bytes.hpp"
#include "util/log.hpp"

#include <algorithm>

namespace rcsdk::protocol {
namespace {

using transport::Endpoint;

// What may wait for one connection: beyond it, its thread fell behind and
// datagrams drop, as on a full socket.
constexpr size_t maxQueued = 8u << 20;
// What may wait for the handshakes: hellos and finished messages, and data
// that came just before its connection was made.
constexpr size_t maxHandshakeBytes = 1u << 20;
constexpr size_t maxDatagram = 2048; // As a connection takes (UdpOptions::maxReceived).

// The connection id a data datagram carries; none for anything else.
std::optional<uint32_t> idOf(std::span<const uint8_t> datagram) {
    if (datagram.size() < connectionIdOffset + 4 || datagram[0] != dataKind) {
        return std::nullopt;
    }
    return uint32_t(loadBigEndian(datagram, connectionIdOffset, 4));
}

} // namespace

struct UdpDemux::Shared {
    explicit Shared(const Endpoint& local) : socket(local) {}

    transport::UdpSocket socket;
    std::mutex mutex;
    std::vector<std::pair<uint32_t, DemuxSocket*>> routes; // Each connection's id.
    DemuxSocket* handshakes = nullptr;
    std::exception_ptr failure; // The port's.
    bool stopping = false;
};

UdpDemux::UdpDemux(const Endpoint& local) : shared_(std::make_shared<Shared>(local)) {
    waiter_.watch(shared_->socket);
    thread_ = log::thread([this] { run(); });
}

UdpDemux::~UdpDemux() {
    {
        std::lock_guard lock(shared_->mutex);
        shared_->stopping = true;
    }
    waiter_.wake();
    thread_.join();
}

Endpoint UdpDemux::local() const {
    return shared_->socket.local();
}

std::unique_ptr<transport::DatagramSocket> UdpDemux::handshakes() {
    std::unique_ptr<DemuxSocket> socket(new DemuxSocket(shared_, maxHandshakeBytes));
    std::lock_guard lock(shared_->mutex);
    shared_->handshakes = socket.get();
    return socket;
}

std::unique_ptr<transport::DatagramSocket> UdpDemux::connection(uint32_t id, const Endpoint&) {
    std::unique_ptr<DemuxSocket> socket(new DemuxSocket(shared_, maxQueued));
    std::lock_guard lock(shared_->mutex);
    shared_->routes.emplace_back(id, socket.get());
    if (auto* handshakes = shared_->handshakes) { // Its data that came before the connection did.
        auto& waiting = handshakes->queue_;
        for (auto it = waiting.begin(); it != waiting.end();) {
            if (idOf(it->bytes) == id) {
                handshakes->queuedBytes_ -= it->bytes.size();
                socket->push(std::move(*it));
                it = waiting.erase(it);
            } else {
                ++it;
            }
        }
    }
    return socket;
}

void UdpDemux::run() noexcept {
    std::vector<uint8_t> buffer(maxDatagram);
    for (;;) {
        try {
            Endpoint from;
            for (int i = 0; i < 512; ++i) { // Then a look at whether to stop.
                const auto size = shared_->socket.receive(buffer, from);
                if (!size) {
                    break;
                }
                const uint8_t kind = *size ? buffer[0] : 0;
                if (kind != clientHelloKind && kind != finishedKind && kind != dataKind) {
                    continue; // No client of this protocol sends it.
                }
                const auto datagram = std::span(buffer).first(*size);
                std::lock_guard lock(shared_->mutex);
                bool routed = false;
                if (const auto id = idOf(datagram)) {
                    for (const auto& [routeId, socket] : shared_->routes) {
                        if (routeId == *id) {
                            socket->push({from, std::vector<uint8_t>(datagram.begin(), datagram.end())});
                            routed = true;
                        }
                    }
                }
                if (!routed && shared_->handshakes) {
                    shared_->handshakes->push({from, std::vector<uint8_t>(datagram.begin(), datagram.end())});
                }
            }
        } catch (...) {
            std::lock_guard lock(shared_->mutex);
            shared_->failure = std::current_exception();
            for (const auto& route : shared_->routes) {
                route.second->wake(); // Each finds the failure.
            }
            if (shared_->handshakes) {
                shared_->handshakes->wake();
            }
            return;
        }
        {
            std::lock_guard lock(shared_->mutex);
            if (shared_->stopping) {
                return;
            }
        }
        waiter_.wait(Clock::now() + 1s);
    }
}

// ---- DemuxSocket ------------------------------------------------------------------------

DemuxSocket::~DemuxSocket() {
    std::lock_guard lock(shared_->mutex);
    std::erase_if(shared_->routes, [&](const auto& route) { return route.second == this; });
    if (shared_->handshakes == this) {
        shared_->handshakes = nullptr;
    }
}

Endpoint DemuxSocket::local() const {
    return shared_->socket.local();
}

bool DemuxSocket::send(const Endpoint& to, std::span<const uint8_t> datagram) {
    return shared_->socket.send(to, datagram);
}

bool DemuxSocket::sendMarked(const Endpoint& to, std::span<const uint8_t> datagram, uint8_t dscp) {
    return shared_->socket.sendMarked(to, datagram, dscp);
}

size_t DemuxSocket::sendBatch(std::span<const transport::Outgoing> batch) {
    return shared_->socket.sendBatch(batch);
}

std::optional<size_t> DemuxSocket::receive(std::span<uint8_t> buffer, Endpoint& from) {
    std::lock_guard lock(shared_->mutex);
    while (!queue_.empty()) {
        auto datagram = std::move(queue_.front());
        queue_.pop_front();
        queuedBytes_ -= datagram.bytes.size();
        if (datagram.bytes.size() > buffer.size()) {
            continue; // As a socket drops a datagram larger than the buffer.
        }
        std::copy(datagram.bytes.begin(), datagram.bytes.end(), buffer.begin());
        from = datagram.from;
        return datagram.bytes.size();
    }
    if (shared_->failure) {
        std::rethrow_exception(shared_->failure);
    }
    return std::nullopt;
}

bool DemuxSocket::buffered() const noexcept {
    std::lock_guard lock(shared_->mutex);
    return !queue_.empty() || shared_->failure;
}

bool DemuxSocket::attachWaiter(transport::Waiter& waiter) const noexcept {
    std::lock_guard lock(shared_->mutex);
    waiters_.push_back(&waiter);
    return true;
}

void DemuxSocket::detachWaiter(transport::Waiter& waiter) const noexcept {
    std::lock_guard lock(shared_->mutex);
    std::erase(waiters_, &waiter);
}

void DemuxSocket::push(Datagram datagram) {
    if (queuedBytes_ + datagram.bytes.size() > limit_) {
        return;
    }
    queuedBytes_ += datagram.bytes.size();
    queue_.push_back(std::move(datagram));
    wake();
}

void DemuxSocket::wake() const noexcept {
    for (auto* waiter : waiters_) {
        waiter->wake();
    }
}

} // namespace rcsdk::protocol
