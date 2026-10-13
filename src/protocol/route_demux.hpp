// A sharer's channel to many peers, each under a route of its own: the host's
// channel with the host's routes (RCS_TRANSPORT_HOST, rcsdk/host_transport.h),
// or a forwarder's pipes with its peers' numbers (RCS_TRANSPORT_FORWARDER).
// A route stands in for the address a peer's datagrams come from
// (endpoint()), so UdpAcceptor runs the handshakes of every route side by
// side on the handshake socket, as on a UDP port. Once a handshake completed,
// the data of its route goes to the socket of that connection; hellos and
// finished messages always go to the handshake socket (a peer that connects
// anew on its route), as does data of a route with no connection (answered
// as unknown). A new connection on a route ends the one there was (NETWORK).
// What a socket sends goes to `send` with its route, a batch to `sendBatch`
// where the channel takes batches; a stream channel carries each datagram
// behind its length (two bytes, big-endian), framed per route.
//
// Bounds, so that a flood under made-up routes takes no more and keeps no
// viewer out: a connection's socket holds up to maxQueued (beyond, the
// session fell behind: QUEUE_FULL); the handshake socket maxWaiting, of which
// one route maxWaitingPerRoute; at most maxRoutes routes without a
// connection hold bytes at once (partial stream frames, waiting datagrams).
#pragma once

#include "protocol/udp_demux.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <span>

namespace rcsdk::protocol {

class RouteDemux final : public Demux {
public:
    enum class Kind { datagram, stream };
    // Hands one datagram (stream: the next bytes, whole frames) to the peer
    // on `route`: true when the channel took it.
    using Send = std::function<bool(uint64_t route, std::span<const uint8_t> bytes)>;
    // A datagram channel's: hands datagrams of a batch (DatagramSocket::
    // sendBatch) to the peer on `route` at once, in order (each `to` is the
    // route's stand-in): how many the channel took.
    using SendBatch = std::function<size_t(uint64_t route, std::span<const transport::Outgoing> datagrams)>;
    static constexpr size_t maxQueued = 8u << 20;
    static constexpr size_t maxWaiting = 1u << 20, maxWaitingPerRoute = 64u << 10;
    static constexpr size_t maxRoutes = 1024;

    // Without `sendBatch`, a batch's datagrams go to `send` one by one.
    RouteDemux(Kind kind, Send send, SendBatch sendBatch = {});
    ~RouteDemux() override;
    RouteDemux(const RouteDemux&) = delete;
    RouteDemux& operator=(const RouteDemux&) = delete;

    Kind kind() const noexcept;
    // What arrived from the peer on `route`. Datagram: one, 1..65535 bytes;
    // stream: the next bytes. Throws INVALID_ARGUMENT for a size the channel
    // cannot have carried, QUEUE_FULL when the session fell behind or too
    // many routes wait (the datagram is dropped; a stream that loses bytes
    // breaks its route's connection, and starts over).
    void receive(uint64_t route, std::span<const uint8_t> bytes);
    // The channel to `route` closed: what waits of it is dropped, its stream
    // starts over, and the connection on it fails (NETWORK).
    void closed(uint64_t route);
    // The channel to `route` was replaced by one to the same peer: what waits
    // of it is dropped, its stream starts over, and the connection on it goes
    // on, told so (DatagramSocket::restarts).
    void restarted(uint64_t route);
    // Datagrams `send` did not take.
    uint64_t refused() const noexcept;

    // The stand-in address of `route`, and the route an address stands for.
    static transport::Endpoint endpoint(uint64_t route);
    static std::optional<uint64_t> routeOf(const transport::Endpoint& address);

    std::unique_ptr<transport::DatagramSocket> handshakes() override;
    std::unique_ptr<transport::DatagramSocket> connection(uint32_t id, const transport::Endpoint& from) override;

    struct Shared;

private:
    std::shared_ptr<Shared> shared_; // Sockets keep it while they exist.
};

} // namespace rcsdk::protocol
