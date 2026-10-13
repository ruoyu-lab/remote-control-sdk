// A sharer's TCP port (RCS_TRANSPORT_TCP): the viewers' datagram connections
// (udp_connection.hpp) over TCP connections to it, each datagram behind its
// length (two bytes, big-endian), as UdpDemux shares a UDP port. A TCP
// connection is an address to the connections (its peer's address and
// port), so a viewer that connected anew (its network changed, its TCP
// connection broke) is the peer on a new path: data (0xD8) goes to the
// connection whose id it carries, over whichever TCP connection it came, and
// the connection proves the new one and moves there. Hellos and finished
// messages, and data under an id no connection has, go to the handshake
// socket, where UdpAcceptor runs the handshakes side by side (and answers
// such data as unknown). It does no cryptography.
//
// One thread accepts, reads and routes, writes what waits, and closes what is
// done. What a connection sends to a TCP connection is written from its own
// thread at once (what a round of it sends in one write), as far as the
// system takes it; beyond maxOutbound waiting, a datagram is dropped, as a
// full UDP socket drops it (the connection's resends and adaptive bitrate
// take it from there).
//
// A TCP connection belongs to the connections a handshake completed over it
// for, or that sent over it (a connection sends only where something it
// authenticated came from: whoever sends under a connection's id without its
// keys claims nothing); at most perConnection belong to one connection
// (beyond, the one it sent over least lately closes: never the one in use).
// One that belongs to none closes helloWait after it opened if no datagram
// came over it, or helloWait and the handshake timeout after; at most
// maxUnclaimed of them at once (a newer one closes the oldest). A connection
// hears when a TCP connection it had closed (transport::PeerClosed, after
// what came over it): the peer closed it, it broke, or this end closed it;
// the handshake socket hears of those a handshake came over. When a
// connection's socket goes, what belonged to it alone closes once what waits
// went out, and the peer closed its end too (a second at most; without the
// thread, at once: 250 ms at most), so that the connection's last words are
// not reset away. Those closed before a datagram came over them, or for the
// limits, are reported to `rejected`.
#pragma once

#include "protocol/udp_demux.hpp"

#include <functional>
#include <memory>
#include <thread>

namespace rcsdk::protocol {

class TcpDemux final : public Demux {
public:
    static constexpr size_t maxUnclaimed = 16, perConnection = 4;
    static constexpr size_t maxOutbound = 256 * 1024;
    static constexpr auto helloWait = 2s;

    // Listens on `local` (Failure NETWORK when it cannot) and serves; a
    // handshake takes `handshakeTimeout` at most (UdpAcceptor's).
    TcpDemux(const transport::Endpoint& local, std::chrono::milliseconds handshakeTimeout,
             std::function<void(const Failure&)> rejected);
    // Closes the port and the TCP connections that belong to no connection;
    // the others go with their connections' sockets.
    ~TcpDemux() override;
    TcpDemux(const TcpDemux&) = delete;
    TcpDemux& operator=(const TcpDemux&) = delete;

    transport::Endpoint local() const;
    std::unique_ptr<transport::DatagramSocket> handshakes() override;
    std::unique_ptr<transport::DatagramSocket> connection(uint32_t id, const transport::Endpoint& from) override;
    // The TCP connections open now.
    size_t connections() const;

    struct Shared;

private:
    void run() noexcept;

    std::shared_ptr<Shared> shared_; // Sockets keep it while they exist.
    std::thread thread_;
};

} // namespace rcsdk::protocol
