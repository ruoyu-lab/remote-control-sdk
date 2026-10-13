// One UDP port for several connections: a sharer serving several viewers.
// A reader thread takes every datagram from the port and routes it by its
// first byte and connection id only (it does no cryptography): data (0xD8)
// goes to the socket of the connection whose id it carries, from whatever
// address it comes (a viewer that changed networks); hellos and finished
// messages, and data under an id no connection has, go to the handshake
// socket, where UdpAcceptor (udp_connection.hpp) runs the handshakes side by
// side. Two connections of one id (a chance of 1 in 2^32 a pair) both get
// the datagram, and the one whose keys it is not for drops it. The sockets
// read what they are handed as the UDP connection reads any socket, and what
// they send leaves through the port.
#pragma once

#include "transport/socket.hpp"

#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rcsdk::protocol {

// Where a sharer's connections come in (UdpAcceptor): the handshakes of
// every peer on one socket, and each connection that completed on a socket
// of its own. The address a peer's datagrams come from is a UDP address, or
// stands for a route (route_demux.hpp).
class Demux {
public:
    virtual ~Demux() = default;
    // The socket the handshakes arrive on (one at a time).
    virtual std::unique_ptr<transport::DatagramSocket> handshakes() = 0;
    // A socket for the connection whose datagrams carry `id`, which the
    // handshake from `from` made: it receives them from now on, and those
    // that wait on the handshake socket.
    virtual std::unique_ptr<transport::DatagramSocket> connection(uint32_t id, const transport::Endpoint& from) = 0;
};

class DemuxSocket;

class UdpDemux final : public Demux {
public:
    // Binds `local` (Failure NETWORK) and starts reading.
    explicit UdpDemux(const transport::Endpoint& local);
    ~UdpDemux();
    UdpDemux(const UdpDemux&) = delete;
    UdpDemux& operator=(const UdpDemux&) = delete;

    transport::Endpoint local() const;
    std::unique_ptr<transport::DatagramSocket> handshakes() override;
    // By `id` alone: from whatever address its datagrams come.
    std::unique_ptr<transport::DatagramSocket> connection(uint32_t id, const transport::Endpoint& from) override;

    struct Shared;

private:
    void run() noexcept;

    std::shared_ptr<Shared> shared_; // Sockets keep it while they exist.
    transport::Waiter waiter_;
    std::thread thread_;
};

// A connection's share of the port, or the handshake socket. Its sends leave
// through the port; it receives what the reader hands it, and wakes the
// Waiters that watch it then.
class DemuxSocket final : public transport::DatagramSocket {
public:
    ~DemuxSocket() override; // Receives nothing more.

    transport::Endpoint local() const override;
    transport::Handle handle() const noexcept override { return transport::invalidHandle; }
    bool send(const transport::Endpoint& to, std::span<const uint8_t> datagram) override;
    bool sendMarked(const transport::Endpoint& to, std::span<const uint8_t> datagram, uint8_t dscp) override;
    size_t sendBatch(std::span<const transport::Outgoing> batch) override;
    // Throws NETWORK once the port failed.
    std::optional<size_t> receive(std::span<uint8_t> buffer, transport::Endpoint& from) override;
    bool buffered() const noexcept override;
    bool attachWaiter(transport::Waiter& waiter) const noexcept override;
    void detachWaiter(transport::Waiter& waiter) const noexcept override;

    struct Datagram {
        transport::Endpoint from;
        std::vector<uint8_t> bytes;
    };

private:
    friend class UdpDemux;
    DemuxSocket(std::shared_ptr<UdpDemux::Shared> shared, size_t limit) : shared_(std::move(shared)), limit_(limit) {}
    // Holding the shared mutex.
    void push(Datagram datagram);
    void wake() const noexcept;

    const std::shared_ptr<UdpDemux::Shared> shared_;
    const size_t limit_; // Bytes that may wait: beyond, datagrams drop as on a full socket.
    // Under the shared mutex.
    std::deque<Datagram> queue_;
    size_t queuedBytes_ = 0;
    mutable std::vector<transport::Waiter*> waiters_;
};

} // namespace rcsdk::protocol
