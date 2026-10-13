// A connected peer: messages over an authenticated, encrypted transport. The
// session works with Connection; the datagram connection (udp_connection.hpp:
// UDP, TCP, P2P, RELAY, HOST) and a viewer's through a forwarder
// (session/forwarder.hpp) implement it.
#pragma once

#include "protocol/messages.hpp"
#include "protocol/rate_control.hpp"
#include "protocol/stall_watch.hpp"
#include "transport/handshake.hpp"
#include "transport/socket.hpp"
#include "util/api.hpp"

#include <memory>
#include <optional>
#include <string>

namespace rcsdk::protocol {

// The peer no longer knows the connection (it restarted, or let it go): a
// viewer connects anew at once instead of waiting.
class UnknownConnection : public Failure {
public:
    using Failure::Failure;
};

// One end of a path, as this end sees it (rcs_path_info without the network,
// which the session works out).
struct PathEnd {
    rcs_path_route route = RCS_ROUTE_NONE;
    rcs_turn_transport relayTransport = RCS_TURN_UDP;
    // This end's socket (a wildcard where the system picks the address), the
    // peer's address, and where this end's datagrams go first (the peer, or
    // the TURN server): which network carries them.
    std::optional<transport::Endpoint> local, remote, hop;
    bool operator==(const PathEnd&) const = default;
};

// A change of the connection's path (rcs_path_event).
struct PathEvent {
    rcs_path_change change = RCS_PATH_CHANGED;
    rcs_path_cause cause = RCS_PATH_CAUSE_STALLED;
    PathEnd from, to;
    uint64_t interruptionNs = 0;
    bool signaled = false;
    rcs_result result = RCS_OK;
    std::string message;
};

class Connection {
public:
    virtual ~Connection() = default;

    // Thread-safe. Messages arrive in the order sent, except that video, data
    // parts and the other messages are separate streams (each in order): a
    // small message may overtake queued video or data. Video is paced and
    // may be lost (StreamGap).
    virtual void send(const Message& message) = 0;
    // A video packet of `stream` as encode() made it: one encoding for every
    // connection that sends it (a sharer's viewers). Sent as send() sends the
    // packet; each stream's video is in order on its own, so a lost picture
    // of one holds up no other.
    virtual void sendVideo(uint32_t stream, std::span<const uint8_t> encoded) = 0;
    // Drops the video queued and not sent yet (the pacing's): the viewer gets
    // another encoding, whose keyframe is not to wait behind it. The peer
    // gives up on those pictures.
    virtual void discardVideo() {}
    // Single reader. Throws Failure when the peer disconnects, its path is
    // lost for good, or the run is cancelled. The connection keeps itself alive.
    virtual Message receive() = 0;

    // What the transport measures about the path; zero where it cannot.
    struct PathStatus {
        uint64_t roundTripNs = 0;
        uint64_t receivedBps = 0;   // Video arriving at the viewer, as it reports.
        uint32_t lossPermille = 0;  // Datagrams the path lost.
        uint64_t queueDelayUs = 0;  // Queueing on the path.
        uint32_t targetBitrate = 0; // Adaptive: what the video should use now, less parity; 0: fixed.
        std::string path;           // Human-readable route.
        // Repairing lost video, this connection's so far.
        uint32_t fecPercent = 0;  // Sender: parity now, per 100 video fragments.
        uint64_t paritySent = 0;  // Sender.
        uint64_t fecRecovered = 0, resendRecovered = 0, framesLost = 0; // Receiver.
        uint64_t markedPackets = 0; // Sound sent with its DSCP (UdpOptions::audioDscp).
        uint64_t pathBps = 0;       // Sender: the path's rate as the receiver measures it; 0: unknown.
        // Sound, the most over the last one to two seconds: what this end
        // sent waited in it before it left; what it received queued on the path.
        uint64_t soundWaitUs = 0, soundQueueUs = 0;
        uint64_t backlogDrops = 0, backlogDroppedBytes = 0; // Video dropped unsent: the path is far too slow.
        PathEnd current;                                    // The path the traffic takes.
        bool lost = false;                                  // No path reaches the peer now (RCS_PATH_LOST).
        uint64_t pathChanges = 0;                           // RCS_PATH_CHANGED so far...
        uint64_t lastInterruptionNs = 0;                    // ...and the latest one's.
        PathEnd standby;                                    // The standby path ready now (route NONE: none).
        // Datagram bytes sent and received so far, and those of them over
        // standby routes (their probes).
        uint64_t bytes = 0, standbyBytes = 0;
        StallWatch::Summary stalls; // Short stalls of what this end receives (stall_watch.hpp).
    };
    virtual PathStatus status() const = 0;
    // The path's changes since the last call, in order, the latest 64 of
    // them (a thread that may call the host: never the connection's own).
    virtual std::vector<PathEvent> takePathEvents() = 0;
    // Whether this end follows its peer to new paths and keeps a lost one
    // (rcs_paths_config.migrate), from now on.
    virtual void setMigrate(bool migrate) = 0;
    // Video queued in this process and not yet sent, as time at the current
    // send rate.
    virtual uint64_t sendBacklogNs() const noexcept { return 0; }
    // Whether sound and unreliable data may be lost on the way (a path of
    // datagrams; over TCP only this end's own queue drops them, when the path
    // is far slower than what goes).
    virtual bool lossy() const noexcept = 0;
    // The sharer's video bitrate: adaptive between the limits by the rules,
    // or fixed at the maximum. moveTo, when not 0, sets the adaptive target
    // there at once.
    struct VideoRate {
        bool adaptive = false;
        uint32_t minimum = 0, maximum = 0;
        uint32_t moveTo = 0;
        RateRules rules;
        FecSetting fec;
    };
    virtual void setVideoRate(const VideoRate& rate) { (void)rate; }
    // Data messages (session/data_channel.hpp): the most one DataPart carries;
    // the DataPart bytes sent and not acknowledged yet, and a wait until that
    // is down to `bytes` (or the timeout passes).
    virtual size_t maxDataPartBytes() const noexcept = 0;
    virtual size_t unackedBytes() const noexcept = 0;
    virtual void waitForUnackedBelow(size_t bytes, std::chrono::milliseconds timeout) = 0;

    // The device key the peer proved in the handshake, if it has one.
    virtual std::optional<crypto::PublicKey> peerKey() const = 0;
    // The peer's address, where the transport has one (not over the host's channel).
    virtual std::optional<transport::Endpoint> peerAddress() const = 0;
    // Ends this connection's blocking calls, now and from now on, as the run's
    // cancellation does (they throw CANCELLED): one connection of several on
    // the same token.
    virtual void cancel() noexcept = 0;

    // How this end leaves when the connection is destroyed: the peer is told
    // (best effort) and reports it as RCS_ERROR_DISCONNECTED, or
    // RCS_ERROR_RESTARTING for a restart. Set before destroying it.
    virtual void leaving(Goodbye::Reason reason) noexcept = 0;

    virtual uint64_t bytesTransferred() const noexcept = 0;
    // The most recent video packet: receiving it (from its first byte on, not
    // the wait before it), decrypting it.
    virtual uint64_t lastReceiveNs() const noexcept = 0;
    virtual uint64_t lastDecryptNs() const noexcept = 0;
};

// The failure a peer's goodbye is: DISCONNECTED (it stopped) or RESTARTING.
[[noreturn]] void failGoodbye(Goodbye::Reason reason);

} // namespace rcsdk::protocol
