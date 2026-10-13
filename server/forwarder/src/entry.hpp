// The forwarder's entries beyond its UDP port, for ends that cannot use UDP:
// a TCP port, each hop datagram behind its length (two bytes, big-endian),
// and a WebSocket port (RFC 6455, transport/websocket.hpp: an HTTP/1.1
// upgrade on any path, then each datagram a binary message; ws:// only: TLS
// is a proxy's in front of it). Each connection is a route of a RouteDemux,
// so the forwarder's UdpAcceptor runs the hops' handshakes over it as over
// the UDP port, and the hops run on it unchanged.
//
// One thread serves an entry's connections: it accepts, reads (whole
// datagrams to the RouteDemux; a WebSocket's opening first, its pings
// answered), writes what waits, and closes what is done. A hop's datagrams
// are written from the hop's own thread as far as the connection takes them
// at once, each connection under a lock of its own (the hops write to their
// connections side by side): what a round of the hop sends (a batch, up to
// 64) framed one after another, each still its own frame (a WebSocket
// message each), in one write. What waits beyond maxOutbound is dropped, as
// a full UDP socket drops it, and the hop's resends and rate control take it
// from there: before a datagram that would take it beyond, what waits is
// written as far as the connection takes it (no more in that batch once it
// took less than all); the datagram is dropped when it still does not fit,
// and the batch's later ones that fit go on.
//
// Bounds against what a peer can make it hold: maxConnections at once (one
// beyond is closed at once); a WebSocket's opening within openWait, of at
// most websocket::maxHandshake bytes; a connection on which no hop completed
// its handshake within claimWait closes; what waits of a connection is one
// frame and a read inbound, maxOutbound outbound. A WebSocket peer that
// breaks RFC 6455 gets the close code that says so and is closed; one that
// sends no WebSocket opening gets the HTTP answer that says why.
#pragma once

#include "protocol/route_demux.hpp"
#include "transport/framed_socket.hpp"
#include "transport/tcp_stream.hpp"
#include "transport/websocket.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace rcsdk::forwarder {

enum class EntryKind { tcp, websocket };

// What a connection's bytes say, as its entry reads them (the fuzz target
// tests/fuzz/forwarder_entry.cpp drives it alone): TCP, each datagram behind
// its length; a WebSocket, its opening, then its frames.
class EntryReader {
public:
    explicit EntryReader(EntryKind kind);
    // The connection's next bytes: datagrams (1..65 535 bytes each) into
    // `datagrams`, what the peer is to get into `outbound` (the opening's
    // answer, pongs while `outbound` stays within Entry::maxOutbound, a
    // close). False once the connection is to close: its peer closed it, or
    // broke the rules (refused(); the HTTP answer or close code that says
    // why went to `outbound`); nothing more is taken.
    // Throws Failure for bytes that break the TCP framing.
    bool take(std::span<const uint8_t> bytes, std::vector<Bytes>& datagrams, Bytes& outbound);
    // Datagrams pass: TCP from the start, a WebSocket once its opening was accepted.
    bool upgraded() const noexcept { return upgraded_; }
    bool refused() const noexcept { return refused_; }

private:
    std::unique_ptr<transport::Framing> lengths_;         // TCP.
    Bytes opening_;                                       // WebSocket: its opening request, as far as it came.
    std::optional<transport::websocket::Decoder> frames_; // Once upgraded.
    bool upgraded_ = false, refused_ = false, done_ = false;
};

class Entry {
public:
    using Kind = EntryKind;
    static constexpr auto openWait = 5s;
    static constexpr auto claimWait = 10s;
    static constexpr size_t maxOutbound = 256 * 1024;

    // Listens on `listen` (Failure NETWORK when it cannot) and serves.
    Entry(Kind kind, const transport::Endpoint& listen, size_t maxConnections);
    ~Entry(); // Closes every connection.
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;

    Kind kind() const noexcept { return kind_; }
    uint16_t port() const noexcept { return port_; }
    // Where the hops' datagrams come in and go out, a route per connection.
    protocol::RouteDemux& routes() noexcept { return *routes_; }
    // The address of `route`'s connection (none once it closed).
    std::optional<transport::Endpoint> peer(uint64_t route) const;
    // A hop completed its handshake on `route`: its connection stays.
    void claim(uint64_t route);
    // The end on `route` left: its connection closes once what waits went.
    void close(uint64_t route);

    size_t connections() const;
    // Connections closed for breaking the rules: no opening, a bad one, a
    // frame RFC 6455 forbids, no hop in time, one too many.
    uint64_t refused() const noexcept { return refused_.load(); }

private:
    struct Connection;
    void run() noexcept;
    // The hop's datagrams to `route`'s connection: how many it took.
    size_t send(uint64_t route, std::span<const transport::Outgoing> datagrams);
    std::shared_ptr<Connection> find(uint64_t route) const;
    void flush(Connection& connection); // Holding its mutex.
    // What arrived on `connection`: datagrams for the routes. Holding its mutex.
    void read(Connection& connection, std::vector<Bytes>& datagrams, Clock::time_point now);

    const Kind kind_;
    const size_t maxConnections_;
    transport::TcpListenSocket listener_;
    uint16_t port_ = 0;
    std::unique_ptr<protocol::RouteDemux> routes_;
    mutable std::mutex mutex_; // Below (each connection's state: its own).
    std::map<uint64_t, std::shared_ptr<Connection>> connections_;
    uint64_t nextRoute_ = 1;
    std::atomic<uint64_t> refused_{0};
    std::atomic<bool> stopping_{false};
    Bytes chunk_ = Bytes(64 * 1024); // The thread's: what one read takes.
    std::thread thread_;             // Last.
};

} // namespace rcsdk::forwarder
