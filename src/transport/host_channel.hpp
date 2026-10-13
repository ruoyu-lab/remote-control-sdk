// A viewer's channel to the sharer over the host's own channel
// (RCS_TRANSPORT_HOST), seen as the datagram socket the UDP connection runs
// on. The host hands in what arrives, from any thread; what the connection
// sends goes to the host's callback, on the connection's thread. A stream
// channel carries each datagram behind its length (two bytes, big-endian).
// (A sharer's channel has a route per viewer: protocol/route_demux.hpp.)
//
// One HostChannel lives for a run; each connection attempt reads it through a
// HostSocket of its own, the only one attached at a time.
#pragma once

#include "transport/socket.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace rcsdk::transport {

enum class HostKind { datagram, stream };

class HostSocket;

class HostChannel {
public:
    // Hands bytes to the host: true when its channel took them.
    using Send = std::function<bool(std::span<const uint8_t>)>;
    // What may wait to be read: beyond this the session fell behind.
    static constexpr size_t maxQueued = 8u << 20;

    HostChannel(HostKind kind, Send send);
    HostChannel(const HostChannel&) = delete;
    HostChannel& operator=(const HostChannel&) = delete;

    HostKind kind() const noexcept { return kind_; }

    // What arrived over the host's channel. Datagram: one, 1..65535 bytes;
    // stream: the next bytes. Throws INVALID_ARGUMENT for a size the channel
    // cannot have carried, QUEUE_FULL when the session fell behind (the
    // datagram is dropped; a stream that loses bytes breaks the connection).
    void receive(std::span<const uint8_t> bytes);
    // The host's channel closed: the connection over it reads what came
    // before (the other end's goodbye may be among it), then fails; a stream
    // starts over, and what comes after is the next connection's.
    void closed();
    // The host replaced its channel with one to the same end: what waits is
    // dropped, a stream starts over (a datagram cut short goes), and the
    // connection over it goes on, told so (HostSocket::restarts).
    void restarted();
    // Datagrams the host's channel did not take.
    uint64_t refused() const noexcept { return refused_.load(); }

private:
    friend class HostSocket;

    void queue(std::vector<uint8_t> datagram); // Holding mutex_.
    void breakConnection();                    // Holding mutex_.

    const HostKind kind_;
    const Send send_;
    std::mutex mutex_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t queuedBytes_ = 0;
    std::vector<uint8_t> partial_; // Stream: the start of a frame still arriving.
    HostSocket* reader_ = nullptr; // The socket of the current connection attempt.
    std::atomic<uint64_t> refused_{0};
    std::atomic<uint64_t> restarts_{0};
};

class HostSocket final : public DatagramSocket {
public:
    // Attaches to `channel` (detaching the socket attached before).
    explicit HostSocket(std::shared_ptr<HostChannel> channel);
    ~HostSocket() override;

    // The host's channel has no addresses: one placeholder stands for both ends.
    static Endpoint placeholder();
    Endpoint local() const override { return placeholder(); }
    Handle handle() const noexcept override;
    // To the other end, whatever `to`. False when the host's channel had no room.
    bool send(const Endpoint& to, std::span<const uint8_t> datagram) override;
    // Throws NETWORK once when the channel closed under the connection,
    // after what came before that.
    std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint& from) override;
    bool buffered() const noexcept override;
    uint64_t restarts() const noexcept override { return channel_->restarts_.load(); }
#ifdef __EMSCRIPTEN__
    bool attachWaiter(Waiter& waiter) const noexcept override;
    void detachWaiter(Waiter& waiter) const noexcept override;
#endif

private:
    friend class HostChannel;

    void wake() noexcept;  // Holding the channel's mutex.
    void drain() noexcept; // Holding the channel's mutex.

    std::shared_ptr<HostChannel> channel_;
    // The channel closed under the connection (channel's mutex), and what
    // came before that, read first.
    bool broken_ = false;
    std::deque<std::vector<uint8_t>> beforeClose_;
#if defined(__EMSCRIPTEN__)
    mutable std::vector<Waiter*> waiters_; // Woken instead of a pipe (channel's mutex).
#elif !defined(_WIN32)
    int pipe_[2] = {-1, -1}; // Readable while something waits.
#endif
};

} // namespace rcsdk::transport
