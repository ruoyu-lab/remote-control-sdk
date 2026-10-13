// The datagram connection: the messages of protocol::Connection over
// datagrams of at most 1200 bytes over UDP, so they pass any path (IPv6's
// minimum MTU, tunnels, a TURN relay's framing); over a host's own channel,
// of the size it carries; over TCP (RCS_TRANSPORT_TCP), of up to 16 KiB, each
// behind its length, a TCP connection being one of the connection's paths
// (tcp_demux.hpp): a viewer whose TCP connection broke goes on over another.
//
// Datagrams, by first byte (0xD0..0xDF stays clear of STUN, 0..3, and TURN
// channel data, 64..79, which may share the socket):
//   0xD1 ClientHello cookie(16) zeros   0xD2 ServerHello   0xD3 Finished
//        The handshake of handshake.hpp, plaintext; the client repeats its
//        hello every 250 ms until answered and its finished message until the
//        server's first authenticated datagram arrives. None of them is
//        authenticated by itself: an answer or finished message that fails is
//        noted, not taken as the end, since anyone can send one (but a PIN's
//        client tests one answer, handshake.hpp, and fails with it). The
//        cookie is zeros until a Retry brought one; the zeros make the hello
//        as large as the ServerHello, so that nothing answers an address
//        that proved nothing with more than came from it (no amplification
//        towards a forged source).
//   0xD4 Retry cookie(16)
//        A server already working on 16 handshakes, or on one from the same
//        address, answers a hello without a valid cookie so, and keeps
//        nothing; any server so answers a PIN hello, whose answer takes one of
//        the PIN's attempts. The client sends the same hello with the cookie,
//        which proves it receives at its address; that hello replaces the
//        oldest pending handshake (or its own earlier one). Forged hellos (a
//        LAN flood with made-up source addresses) therefore cannot keep a
//        viewer out, nor use the PIN's attempts up.
//   0xD5 VersionNotice magic(8)
//        The server's answer to a hello of another protocol version: its
//        magic, which names its version. No larger than the hello, and at most
//        16 per 5 s, so it reflects nothing to a forged source.
//   0xD6 PinNotice notice(8)
//        The server's answer to a PIN hello once no PIN attempt is left
//        (transport::pinLockedNotice), within the same budget.
//   0xD7 Unknown cid(4)
//        A server's answer to data under a connection id it does not know: it
//        restarted, or dropped the connection (udp_demux.hpp; over the host's
//        channel, one waiting for its next viewer). At most 16 per 5 s, a
//        budget of its own, and smaller than what it answers. Not
//        authenticated: a client takes it only while its path is silent, from
//        the address it sends to (or over a way of its own to it that it is
//        proving: a new TCP connection), and then connects anew at once
//        instead of waiting out its connection.
//   0xD8 cid(4) packet(8) Aead(frame)
//        Authenticated with cid, kind and packet number. cid is the
//        receiver's connection id (handshake.hpp): a sharer's port routes by
//        it, and it names the connection on whatever path the datagram took.
//        Packet numbers increase per direction, over every path, and a
//        1024-packet window rejects replays.
// Frames (a malformed or unknown one fails the connection: only the peer can
// send them):
//   PING time(8) flags(1) / PONG time(8) flags(1)
//                                     every 250 ms: liveness and round-trip
//                                     time; flags bit 0: this end follows its
//                                     peer to a new path (UdpOptions::migrate)
//   MESSAGE stream(1) id(4) body      reliable messages: acknowledged,
//                                     retransmitted, delivered in order per
//                                     stream. Stream 0: control messages, sent
//                                     at once; 1: data parts, sent when no
//                                     video waits, within a congestion window
//                                     that yields to the video (see Impl)
//   ACK stream(1) [id(4)]...
//   VIDEO stream(1) frame(4) index(2) count(2) sent(4) bytes
//                                     a fragment of an encoded video message
//                                     of `stream` (each numbers its frames
//                                     and is in order on its own); `sent`:
//                                     microseconds when it left
//   REPORT window(4) expected(4) received(4) bytes(8) delay(8) spaced(4) unspaced(4) first(8)
//                                     every 50 ms in which video arrived: what
//                                     arrived and how late (rate_control.hpp),
//                                     and the fastest rates its pictures
//                                     arrived at (kbit/s; 0: none), of those
//                                     the path spaced (its bottleneck's rate)
//                                     and of those it did not (it is faster),
//                                     of the packets from `first` on
//   NACK [stream(1) frame(4) index(2)]...
//                                     fragments to resend; index 0xFFFF asks
//                                     for a whole frame of unknown length
//   GONE [stream(1) frame(4)]...      the answer for frames no longer kept
//                                     (older than a second after they left,
//                                     dropped with a backlog, or while the
//                                     path was silent): the receiver gives up
//                                     on them at once
//   CLOSE reason(1)                   the peer is stopping: for good (0), or
//                                     it will be back (1, Goodbye::restarting);
//                                     never over a lost path (one that came
//                                     once it is back would be stale)
//   AUDIO sent(4) body                an AudioPacket, never resent; `sent` as
//                                     VIDEO's
//   DATAGRAM body                     a DataDatagram (a forwarder's hop: a
//                                     Pipe or SealedAudio), never resent
//   PATH_CHALLENGE data(8) [zeros]    proves a path (below); the zeros pad
//   PATH_RESPONSE data(8) [zeros]     the datagram to the path's size (not
//                                     over a byte stream: any size passes)
//
// Paths (QUIC's connection migration, RFC 9000 sections 8 and 9, both ways):
// the keys, not the addresses, name the connection. A datagram that
// authenticates and is the peer's newest, not a probe (a datagram of a
// PATH_CHALLENGE or PATH_RESPONSE), from another address, is the peer on a
// new path (it changed networks, its NAT changed the mapping). The new
// address is proven first: a challenge goes there, which only the peer can
// answer, and traffic goes on over the last proven path meanwhile; until it
// is proven, no more than three times what came from it goes to it (no
// amplification of a forged source). Once proven, traffic moves there,
// reliable messages not yet acknowledged go again at once, the round trip
// and the adaptive bitrate start over (an address that kept its host: only
// the port changed, as NATs remap, keeps the bitrate). A challenge goes to
// the old path too, and newer traffic from it takes the connection back: a
// path an attacker on the way forwards loses to the real one. This end moves
// itself onto a socket of its own (addRoute, or one UdpOptions::reopen opens
// when its socket failed or the network changed under a silent path) the
// same way: proven first, unless the old one failed and the path is not
// lost (a lost path is back once the peer answered). Over TCP each TCP
// connection is such a path: the viewer connects anew and moves there, the
// sharer's port hands what comes over it to the connection its id names
// (tcp_demux.hpp), and tells it when the TCP connection a path took closed
// (transport::PeerClosed): the path stalls at once, or without migration the
// connection fails, as the TCP connection did. A path silent for
// pathStall is reported changing, and the video, sound and data stop (only
// pings, probes and control messages go on) until something arrives again;
// silent for idleTimeout it is lost: the connection stays for `hold` more,
// in case a path comes back, and then fails (NETWORK). Each change is a
// PathEvent (takePathEvents; the latest 64 wait for it). Either end without
// migration (0 in its pings) makes a lost path fail the connection at
// idleTimeout, as a change of address.
//
// Sound and unreliable data leave ahead of everything else, between any two
// datagrams of video. Video is paced: on a path the viewer's reports find
// clear, each picture within the frame spread (a millisecond by default) and
// never faster than the host's maximum, so that it arrives whole at once;
// where the viewer measures the path's bottleneck, what of a picture goes
// beyond pathBurst of the path's time at pathPacing percent of its rate at
// most (RateRules): a large picture arrives no later for it (the bottleneck
// spaces it anyway), and what would queue at the bottleneck queues here, where
// sound overtakes it, instead of on the path (a Wi-Fi access point), where
// sound waits behind it. Otherwise at about 1.5 times the bitrate, so that a
// keyframe leaves as a stream, not a burst a queue already filling drops.
// Fragments leave in order, so a receiver asks at once for
// the ones missing below the highest that arrived, and for the rest of a frame
// once a later frame's fragment (of the same stream) arrived or nothing came
// for longer than the arrival rate explains. It asks again while a frame is
// young; one it cannot complete becomes a StreamGap of its stream, whose
// decoding restarts at the next keyframe. A path that delivers in order and
// loses nothing while it works (Link::ordered: TCP) only arrives late, all at
// once after its own retransmission: there the rest of a frame counts as lost
// only once a later frame's fragment came or after orderedWait of silence,
// and requests and reliable messages go again after orderedWait at the
// soonest; what this end dropped itself (its queue full) and a path that
// broke (whose unacknowledged messages go again over the next one at once)
// are all it loses.
//
// What arrives waits for receive() up to UdpOptions::maxInbound bytes (one
// message always fits when none waits). Beyond it the session is behind (a
// host's callback holds its thread): the reliable streams wait as TCP's
// would (what is next in order stays here, with what came early up to
// maxOutOfOrder; later messages go unacknowledged and come again, so the
// peer's window stops it); a complete picture is given up on as a lost one
// is (a StreamGap before the stream's next picture); sound and unreliable
// data are dropped as lost datagrams are. The log says when the session
// falls behind and when it caught up, with what was dropped.
#pragma once

#include "protocol/connection.hpp"
#include "protocol/udp_demux.hpp"
#include "transport/handshake.hpp"
#include "transport/ice.hpp"
#include "transport/socket.hpp"
#include "transport/turn.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace rcsdk::protocol {

// The largest datagram sent over UDP paths: passes any path.
inline constexpr size_t udpDatagram = 1200;
// Over TCP, both ends: fewer, larger pieces of a picture.
inline constexpr size_t tcpDatagram = 16 * 1024;

// A datagram's first byte (see above).
enum DatagramKind : uint8_t {
    clientHelloKind = 0xD1,
    serverHelloKind = 0xD2,
    finishedKind = 0xD3,
    retryKind = 0xD4,
    versionKind = 0xD5,
    pinKind = 0xD6,
    unknownKind = 0xD7,
    dataKind = 0xD8
};
// Where a data datagram's connection id is (the receiver's).
inline constexpr size_t connectionIdOffset = 1;
// The ClientHello datagram of `hello` with the Retry's `cookie` (none yet: empty).
crypto::Bytes helloDatagram(std::span<const uint8_t> hello, std::span<const uint8_t> cookie = {});

// How datagrams reach the peer: directly, through a relay, or over the host's channel.
class Link {
public:
    virtual ~Link() = default;
    // To the peer's address (peer()); `dscp` (0: unmarked) as
    // DatagramSocket::sendMarked; true when it went out marked.
    virtual bool send(transport::DatagramSocket& socket, std::span<const uint8_t> datagram, uint8_t dscp = 0) = 0;
    // Datagrams to the peer in order, each as send() with its `dscp` (their
    // `to` is the link's to set): how many of the marked ones went out
    // marked. One by one, unless the link hands them to its socket at once
    // (a direct path, a TCP connection, a forwarder's stream entries:
    // DatagramSocket::sendBatch).
    virtual size_t sendBatch(transport::DatagramSocket& socket, std::span<transport::Outgoing> batch) {
        size_t marked = 0;
        for (const auto& datagram : batch) {
            marked += send(socket, datagram.datagram, datagram.dscp);
        }
        return marked;
    }
    // To another address of the peer's over this link: a path's probe.
    virtual void sendTo(transport::DatagramSocket& socket, const transport::Endpoint& to,
                        std::span<const uint8_t> datagram) {
        (void)to;
        send(socket, datagram);
    }
    // The payload of a datagram received from `from`, when it came over this
    // link, with the peer's address it came from in `from` (through a relay:
    // the address the server relayed it from). A link may answer link-level
    // datagrams (ICE checks) itself.
    virtual std::optional<std::span<const uint8_t>> unwrap(transport::DatagramSocket& socket, transport::Endpoint& from,
                                                           std::span<const uint8_t> datagram) = 0;
    // Bytes the link adds to each datagram now; it may shrink (a TURN channel
    // once bound), never grow.
    virtual size_t overhead() const noexcept = 0;
    virtual std::string describe() const = 0;
    // The peer's address; none over the host's channel.
    virtual std::optional<transport::Endpoint> peer() const { return std::nullopt; }
    // Where this end's datagrams go first: the peer, or the TURN server.
    virtual std::optional<transport::Endpoint> firstHop() const { return peer(); }
    // The peer moved there (a proven path); a relay only where it has a permission for.
    virtual void setPeer(const transport::Endpoint& peer) { (void)peer; }
    // Whether the peer may be reached at `address` over this link (a relay:
    // the addresses it has permissions for).
    virtual bool reaches(const transport::Endpoint& address) const {
        (void)address;
        return true;
    }
    // What it is, as a path's route (rcs_path_route), and how a relay is reached.
    virtual rcs_path_route route() const = 0;
    virtual rcs_turn_transport relayTransport() const { return RCS_TURN_UDP; }
    // Periodic upkeep (relay refreshes), and the end of the connection.
    virtual void tick(transport::DatagramSocket&, Clock::time_point) {}
    virtual void close(transport::DatagramSocket&) noexcept {}
    // The peer starts the handshake when it likes (the host's channel): a
    // server waits for it as long as it runs, as without a link.
    virtual bool awaitsPeer() const noexcept { return false; }
    // What it sends arrives in order, and nothing is lost on the way while it
    // works (a TCP connection): see above.
    virtual bool ordered() const noexcept { return false; }
    // Why a client's handshake may go unanswered.
    virtual std::string silence() const { return "nothing listens there, or a firewall drops UDP"; }
};

std::unique_ptr<Link> directLink(const transport::Endpoint& peer);
// A path chosen by ICE: direct to `peer` (of ICE type `type`: host,
// reflexive) from this end's `local`, still answering the peer's STUN checks
// (keeps NAT bindings and the peer's ICE state alive).
std::unique_ptr<Link> iceLink(const transport::Endpoint& peer, const std::string& localUfrag,
                              const std::string& localPassword, transport::ice::Type type,
                              const transport::Endpoint& local);
// The same through a TURN allocation (which the link keeps fresh and releases).
std::unique_ptr<Link> relayLink(std::unique_ptr<transport::TurnClient> relay, const transport::Endpoint& peer,
                                const std::string& localUfrag, const std::string& localPassword,
                                transport::ice::Type type, rcs_turn_transport transport);
// The host's own channel (transport/host_channel.hpp), whose only peer is the
// other end.
std::unique_ptr<Link> hostLink(std::string description);
// A TCP connection to the peer at `peer` (RCS_TRANSPORT_TCP): a viewer's
// FramedSocket to the sharer, or a sharer's port (tcp_demux.hpp) to the
// viewer's TCP connection from there. Ordered; sound goes unmarked.
std::unique_ptr<Link> tcpLink(const transport::Endpoint& peer);
// A channel to many peers, to the one at `to` (a route's stand-in,
// route_demux.hpp): a sharer's over the host's channel (`route` HOST) or
// through a forwarder (FORWARDER), and a viewer's through a forwarder.
std::unique_ptr<Link> routeLink(std::string description, const transport::Endpoint& to, rcs_path_route route);

struct UdpOptions {
    // Sharer: the video bitrate, its start in moveTo, and the pacing (rules).
    std::optional<Connection::VideoRate> rate;
    // Sharer, adaptive: each report of the viewer with what was measured, and
    // the rules' proposal in the decision, which it may change. On the
    // connection's thread, one at a time, without the connection's locks.
    std::function<void(const rcs_rate_report&, rcs_rate_decision&)> decideRate;
    // The largest datagram this end sends (from RCS_HOST_MIN_DATAGRAM), and
    // the largest it takes: a host's channel says what it carries.
    size_t maxDatagram = udpDatagram;
    size_t maxReceived = 2048;
    // What arrived and waits for receive() at most, in bytes (see above).
    size_t maxInbound = 32u << 20;
    // rcs_network_tuning_config: how long a client's handshake may take (a
    // server with a link: twice that), and the silence that ends a connection.
    std::chrono::milliseconds handshakeTimeout{5000};
    std::chrono::milliseconds idleTimeout{5000};
    // rcs_network_tuning_config.audio_dscp: what sound datagrams are marked (0: unmarked).
    uint8_t audioDscp = 0;
    // Paths (see above): whether this end follows the peer to a new path and
    // keeps a lost one (rcs_paths_config.migrate), the silence that makes a
    // path stall (path_stall_ms), and how long a lost path's connection is
    // kept after idleTimeout (0: it fails then).
    bool migrate = true;
    std::chrono::milliseconds pathStall{1000};
    std::chrono::milliseconds hold{0};
    // Opens another socket of this end's (of IPv6: `ipv6`), bound as this end's
    // sockets are to be now: for a direct path when its socket failed, the
    // network changed under it while it is silent, or moveRoute (a viewer's
    // direct UDP), and for a relay's allocation to move to. None: a new socket
    // is unbound, and a direct path stays on the one it has.
    std::function<std::unique_ptr<transport::DatagramSocket>(bool ipv6)> reopen;
    // A standby route's probes, this often while it waits (standby_probe_ms).
    std::chrono::milliseconds standbyProbe{15000};
    // Where both ends number their reliable messages from (tests: just below 2^32).
    uint64_t firstMessageId = 1;
    // Which end sends video: the server (a sharer; the forwarder to its
    // viewers' hops) by default, or the client (a sharer's hop to its
    // forwarder). The other end's video ends the connection
    // (UdpConnection::setVideoFrom changes it: the forwarder once an end said
    // it is the room's sharer).
    transport::Side videoFrom = transport::Side::server;
};

class UdpConnection final : public Connection {
public:
    using Side = transport::Side;
    using Rejected = std::function<void(const Failure&)>;

    // `socket`: a UDP socket, the TCP connection to a TURN server (ICE's
    // path), or a TCP connection to the sharer (a FramedSocket, tcpLink).
    // Client: handshakes with the peer behind `link`; throws when it does not
    // answer within options.handshakeTimeout or fails the handshake (an
    // answer that fails is reported a second later when no valid one came
    // meanwhile; one that fails the PIN at once). Server: waits for the peer behind `link` or, without one,
    // for the first datagram peer to complete the handshake; peers that fail
    // it are reported to `rejected` (at most 16 per 5 s, the rest summed up in
    // the log) and ignored. A server with a link gives up after twice the
    // handshake timeout, naming the last failure, unless the link awaits its
    // peer. A server takes its
    // credentials for each hello. The server sends video (a sharer), the
    // client receives it: video sent the other way ends the connection.
    static std::unique_ptr<UdpConnection> establish(Side side, std::unique_ptr<transport::DatagramSocket> socket,
                                                    std::unique_ptr<Link> link,
                                                    const transport::CredentialsSource& credentials,
                                                    const UdpOptions& options, CancelToken cancelled,
                                                    const Rejected& rejected = {});
    static std::unique_ptr<UdpConnection> establish(Side side, std::unique_ptr<transport::DatagramSocket> socket,
                                                    std::unique_ptr<Link> link,
                                                    const transport::Credentials& credentials,
                                                    const UdpOptions& options, CancelToken cancelled,
                                                    const Rejected& rejected = {}) {
        return establish(side, std::move(socket), std::move(link), transport::fixedCredentials(credentials), options,
                         cancelled, rejected);
    }
    ~UdpConnection() override;

    void send(const Message& message) override;
    void sendVideo(uint32_t stream, std::span<const uint8_t> encoded) override;
    void discardVideo() override;
    Message receive() override;
    void leaving(Goodbye::Reason reason) noexcept override;
    void cancel() noexcept override;

    std::optional<crypto::PublicKey> peerKey() const override;
    std::optional<transport::Endpoint> peerAddress() const override;
    uint64_t bytesTransferred() const noexcept override;
    uint64_t lastReceiveNs() const noexcept override;
    uint64_t lastDecryptNs() const noexcept override;

    PathStatus status() const override;
    std::vector<PathEvent> takePathEvents() override;
    uint64_t sendBacklogNs() const noexcept override;
    bool lossy() const noexcept override;
    size_t maxDataPartBytes() const noexcept override { return RCS_DATA_MAX_DATAGRAM; }
    size_t unackedBytes() const noexcept override;
    void waitForUnackedBelow(size_t bytes, std::chrono::milliseconds timeout) override;
    void setVideoRate(const VideoRate& rate) override;
    // Paths: another way to the peer this end found (a socket of its own on a
    // new network, a new relay allocation, a path ICE selected), which the
    // traffic moves to once the peer answered over it; `cause` and `signaled`
    // (the host's signalling found it) go to the change it makes. `standby`:
    // the route is kept proven beside the current one instead
    // (rcs_paths_config.standby), and takes the traffic when the current path
    // stalls, until that answers again; it replaces the standby route there
    // was. Any thread.
    void addRoute(std::unique_ptr<transport::DatagramSocket> socket, std::unique_ptr<Link> link,
                  rcs_path_cause cause = RCS_PATH_CAUSE_NETWORK, bool signaled = false, bool standby = false);
    // No standby route from now on (RCS_PATH_STANDBY, gone: `cause`, `result`
    // and `why`). Any thread.
    void dropStandby(rcs_path_cause cause, rcs_result result, std::string why);
    // This end is to use another network (rcs_paths_config): a direct path
    // moves to a new socket of its own (UdpOptions::reopen) once the peer
    // answered over it; `cause` goes to the change. Any thread.
    void moveRoute(rcs_path_cause cause);
    // No network is left for this end to use: nothing more goes over the
    // path, which is lost at once (RCS_PATH_LOST: `cause`, `why`) and kept as
    // a lost one is, until a route comes (addRoute, moveRoute). Any thread.
    void leaveRoute(rcs_path_cause cause, std::string why);
    // Whether migration is on now (rcs_paths_config.migrate; it applies at once).
    void setMigrate(bool migrate) override;
    // Which end sends video from now on (UdpOptions::videoFrom). Any thread.
    void setVideoFrom(Side from) noexcept;
    // The peer's signalling said it no longer knows the connection (an
    // unknown signal): taken while the path is silent, as 0xD7 is.
    void peerForgot();
    // The handshake's exporter (handshake.hpp): the keys of signals about the connection derive from it.
    const crypto::Key& exporter() const noexcept;

    struct Impl;

private:
    friend class UdpAcceptor;
    explicit UdpConnection(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

#ifdef RCSDK_FUZZING
// The fuzz targets' way in (tests/fuzz, RCSDK_BUILD_FUZZERS): a connection's
// receiving end without its thread, after a handshake that gave it `keys`.
// What it sends goes nowhere; datagrams come in by hand at the times given.
class UdpFuzzEnd {
public:
    UdpFuzzEnd(bool sendsVideo, transport::TrafficKeys keys, const UdpOptions& options = {});
    ~UdpFuzzEnd();
    // A datagram from the peer: as the network thread takes it; throws as
    // the connection would fail.
    void datagram(std::span<const uint8_t> datagram, Clock::time_point now);
    // What the network thread does for what arrived on each round.
    void tick(Clock::time_point now);
    // What receive() would return now.
    std::optional<Message> take();

private:
    std::unique_ptr<UdpConnection::Impl> impl_;
};
#endif

class ServerHandshakes;

// A server that takes connection after connection on one port shared through
// a demultiplexer (udp_demux.hpp; a TCP port: tcp_demux.hpp): the handshakes
// run side by side as a server's in establish(), the credentials taken for
// each hello, and each one that completes becomes a connection over a socket
// of its own for that peer; the others go on. Peers that fail are reported
// to `rejected` as there, and so is one whose own connection closed during
// its handshake (transport::PeerClosed).
class UdpAcceptor {
public:
    // How a connection reaches the peer whose handshake came from `from`.
    using LinkFor = std::function<std::unique_ptr<Link>(const transport::Endpoint& from)>;
    // A handshake that does not finish within `handshakeTimeout` of its hello
    // is dropped (reported). `linkFor`: direct UDP to the address by default.
    UdpAcceptor(Demux& demux, transport::CredentialsSource credentials, UdpConnection::Rejected rejected,
                std::chrono::milliseconds handshakeTimeout, LinkFor linkFor = directLink);
    ~UdpAcceptor();
    UdpAcceptor(const UdpAcceptor&) = delete;
    UdpAcceptor& operator=(const UdpAcceptor&) = delete;

    // The next connection, with `options()` as it completes, and in `from`
    // where given the address its handshake came from; throws CANCELLED once
    // `cancelled` is set. One thread at a time.
    std::unique_ptr<UdpConnection> accept(const std::function<UdpOptions()>& options, CancelToken cancelled,
                                          transport::Endpoint* from = nullptr);

private:
    Demux& demux_;
    std::unique_ptr<transport::DatagramSocket> socket_;
    UdpConnection::Rejected rejected_;
    LinkFor linkFor_;
    std::unique_ptr<ServerHandshakes> handshakes_;
    transport::Waiter waiter_;
};

} // namespace rcsdk::protocol
