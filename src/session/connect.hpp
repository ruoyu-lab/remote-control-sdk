// Connection setup for a session's transport (rcs_connect, rcs_ice_servers_config).
#pragma once

#include "protocol/connection.hpp"
#include "protocol/udp_connection.hpp"
#include "session/session.hpp"
#include "session/signals.hpp"
#include "transport/ice.hpp"

#include <atomic>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

namespace rcsdk::session {

class ForwarderLink;

// A socket of this end's for a connection's route: unbound, or bound to the
// network the settings choose (Session::boundInterface).
std::unique_ptr<transport::UdpSocket> openSocket(Session& session, bool ipv6);
// Pacing and adaptive bitrate for this end's UDP connection. Share: a new
// viewer's starts at `start` (the stream's bitrate now; 0: as configured);
// its reports name `viewer` once it joined.
protocol::UdpOptions optionsFor(Session& session, const SessionConfig& live, bool share, uint32_t start = 0,
                                std::shared_ptr<std::atomic<rcs_viewer_id>> viewer = {});
// This connection's credentials, from the latest settings (the identity and
// the device identity change from one connection to the next).
transport::Credentials credentialsOf(Session& session, const SessionConfig& live);
// The credentials of each handshake, as the settings are when it starts.
transport::CredentialsSource credentialsSource(Session& session);
// A peer that failed the handshake: logged and counted.
void reject(Session& session, const Failure& failure);

// A viewer's connection that passed the handshake (share).
struct Incoming {
    std::unique_ptr<protocol::Connection> connection;
    // Set once it joined: its connection's rate reports name it.
    std::shared_ptr<std::atomic<rcs_viewer_id>> viewer;
    // The host's route to it: of its signal (P2P and RELAY), of its channel
    // (HOST), its token's (FORWARDER); and its number in the forwarder's room.
    uint64_t route = 0, peer = 0;
    // What its transport does when it is not let in (FORWARDER: the forwarder drops it).
    std::function<void()> turnedAway = {};
};

// Share: takes the viewers' connections for a run and hands each that passed
// the handshake to `accepted`. TCP and UDP keep the port open for as long as
// the acceptor exists, on a thread of its own (several viewers; one that
// cannot be served is told so), and take the latest credentials for each
// handshake (UDP: udp_demux.hpp; TCP: tcp_demux.hpp, where a viewer's later
// TCP connections join its connection); HOST takes each viewer's handshake
// on its route of the host's channel the same way (route_demux.hpp). P2P and
// RELAY take what the session's IceListener completes (each viewer's own ICE
// exchange), and report an attempt that failed to `failed` (`peer`: the
// viewer or its path caused it, not this end). Peers that fail the handshake
// are counted and logged; listening goes on.
class Acceptor {
public:
    using Accepted = std::function<void(Incoming)>;
    using Failed = std::function<void(const FailureInfo& failure, bool peer)>;
    // Opens the port (Failure NETWORK when it cannot) and reports LISTENING;
    // `startBitrate`: where a new viewer's adaptive bitrate starts (0: as
    // configured).
    Acceptor(Session& session, std::function<uint32_t()> startBitrate, Accepted accepted, Failed failed);
    // Closes the port; the run is over (its token is set).
    ~Acceptor();
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    // What ended it: the port failed, or the hop to the forwarder.
    void rethrowFailure();
    // FORWARDER: the run's hop to the forwarder; none otherwise.
    std::shared_ptr<ForwarderLink> forwarder() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Share, P2P and RELAY: from the start of a run to its end, as the run's
// agent, answers each viewer's signal (rcs_session_signal) with an ICE
// exchange of its own, on a thread of its own (paced by SignalQueue): it
// gathers this end's candidates on the first free port (over RELAY with an
// allocation of its own), hands the host the answer with the signal's route
// (on_signal), checks the pairs and completes the handshake on the path
// found. The connection goes to the acceptor attached, or waits for the next
// one while none is (between a run's last viewer leaving and listening
// again): what is in progress goes on meanwhile. Its own token ends the
// attempts and the connections' blocking calls when the run ends (finish).
class IceListener {
public:
    class Sink {
    public:
        virtual void accepted(Incoming incoming) = 0;
        virtual void failed(const FailureInfo& failure, bool peer) = 0;
        virtual uint32_t startBitrate() = 0;

    protected:
        ~Sink() = default;
    };

    explicit IceListener(Session& session);
    // Cancels the attempts and waits for them; connections that wait for an
    // acceptor are told how the run leaves them (Session::leaveReason) and
    // go. Returns the signals no attempt answered to the end: those still
    // waiting, those whose attempt the end cut short and those that came
    // since, which the next run answers.
    std::vector<ViewerSignal> finish();
    ~IceListener(); // finish(), the signals dropped.
    IceListener(const IceListener&) = delete;
    IceListener& operator=(const IceListener&) = delete;

    void signal(ViewerSignal signal);
    // From now on hands what completes to `sink` (first what waited); null:
    // to none. No call reaches the previous sink once it returns.
    void attach(Sink* sink);

private:
    struct Failed {
        FailureInfo failure;
        bool peer = false;
    };
    using Outcome = std::variant<Incoming, Failed>;
    void start(ViewerSignal signal); // Holding mutex_.
    void attempt(const ViewerSignal& signal) noexcept;
    void deliver(Outcome outcome);
    void hand(Outcome& outcome); // To the sink, holding mutex_.
    uint32_t startBitrate();

    Session& session_;
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_; // Below.
    SignalQueue queue_;
    Sink* sink_ = nullptr;
    std::deque<Outcome> held_;              // For the next sink.
    std::vector<ViewerSignal> unanswered_; // Cut short by the end.
    struct Attempt {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Attempt> attempts_;
};

// View: connects to the sharer.
std::unique_ptr<protocol::Connection> connectToSharer(Session& session);

// What finds a running connection's new paths where its transport needs more
// than the connection itself (P2P and RELAY: PathRestart; a TCP viewer:
// TcpRedial). The session hands it the connection's path changes and its
// choice of network.
class PathFinder {
public:
    virtual ~PathFinder() = default;
    // The connection's path changed: one is looked for when it stalls or is
    // lost, until it is back.
    virtual void pathChanged(const protocol::PathEvent& event) = 0;
    // This end is to use another network (NetworkChoice): the path is found
    // anew over it, though the one in use may still work.
    virtual void move(rcs_path_cause cause) = 0;
    // rcs_paths_config.standby: a standby path is found over the standby
    // network NetworkChoice chose (again when it changed, and when the
    // standby path stops answering), or, `wanted` false, no more.
    virtual void setStandby(bool wanted) = 0;
};

// P2P and RELAY: a new path for a running connection whose path stalled or
// was lost, or that is to use another network, or a standby path beside the
// one in use (rcs_paths_config), found through the host's signalling. This
// end gathers candidates on the network it uses (over RELAY an allocation of
// its own), hands them to the host sealed with keys of the connection's
// (on_signal; a sharer's with its viewer's route); the other end answers so,
// both check the pairs (ICE), and the path found joins the connection, which
// proves it and moves its traffic there (signaled), or keeps it ready
// (standby). Never another kind of path: P2P among direct candidates only,
// RELAY through its TURN server only. One attempt at a time, on a thread of
// its own, the path in use before a standby one, another while the path
// stays lost; when both ends start one, the viewer's goes on and the
// sharer's yields. The session hands it the peer's restart signals
// (Session::signal).
class PathRestart final : public PathFinder {
public:
    PathRestart(Session& session, protocol::UdpConnection& connection, uint64_t route);
    ~PathRestart() override; // Cancels an attempt under way and waits for it.
    PathRestart(const PathRestart&) = delete;
    PathRestart& operator=(const PathRestart&) = delete;

    const transport::ice::SignalTag& tag() const noexcept { return tag_; }
    const crypto::Key& key() const noexcept { return key_; }
    // An attempt starts when the path stalls or is lost, unless one runs, and
    // ends when it is back.
    void pathChanged(const protocol::PathEvent& event) override;
    // A restart signal of the peer's for this connection (its seal verified).
    void signal(const transport::ice::Restart& restart);
    // The peer has no connection of this tag (an unknown signal).
    void unknown();
    // Through the host's signalling.
    void move(rcs_path_cause cause) override;
    void setStandby(bool wanted) override;

private:
    struct Attempt;
    struct Worker {
        std::thread thread;
        std::shared_ptr<Attempt> attempt;
    };
    // Holding mutex_: an attempt (answering the peer's restart, or this
    // end's own: for the path in use, or `standby`) begins; the one under way
    // is cancelled (its thread ends by itself).
    void start(std::optional<transport::ice::Restart> answering, rcs_path_cause cause, Clock::duration delay,
               bool standby = false);
    void stop();
    void run(Attempt& attempt) noexcept;

    Session& session_;
    protocol::UdpConnection& connection_;
    const uint64_t route_;
    const crypto::Key key_;
    transport::ice::SignalTag tag_{};
    std::mutex mutex_;                 // Below.
    std::shared_ptr<Attempt> attempt_; // The one under way, if any.
    std::vector<Worker> workers_;      // Joined once they ended, and at the end.
    uint32_t generation_ = 0;          // This end's restarts so far...
    uint32_t peerGeneration_ = 0;      // ...and the peer's latest.
    bool lost_ = false;                // The path is lost (RCS_PATH_LOST) until it is back.
    bool standbyWanted_ = false;       // setStandby.
};

// A TCP viewer (RCS_TRANSPORT_TCP): a new path for a running connection
// whose TCP connection broke, whose path stalled or was lost, or that is to
// use another network, or a standby path beside the one in use
// (rcs_paths_config): this end connects to the sharer again over TCP, from
// the network it is to use, and hands the TCP connection to the connection,
// which proves it with its keys (the sharer's port finds the connection by
// its id, tcp_demux.hpp) and moves its traffic there, or keeps it ready
// (standby). Nothing goes through the host. A path that only stalls gets a
// second first; one attempt at a time, on a thread of its own, the path in
// use before a standby one (which waits until the path is back); while the
// path stays away, another every second (a standby one's that failed: every
// standby_probe_ms).
class TcpRedial final : public PathFinder {
public:
    TcpRedial(Session& session, protocol::UdpConnection& connection);
    ~TcpRedial() override; // Cancels an attempt under way and waits for it.
    TcpRedial(const TcpRedial&) = delete;
    TcpRedial& operator=(const TcpRedial&) = delete;

    void pathChanged(const protocol::PathEvent& event) override;
    void move(rcs_path_cause cause) override;
    void setStandby(bool wanted) override;

private:
    struct Attempt {
        rcs_path_cause cause = RCS_PATH_CAUSE_STALLED; // For the change the path it finds makes.
        Clock::duration delay{};                       // Its wait first: the path may come back by itself.
        bool standby = false;                          // For a standby path, over the standby network.
        std::atomic<bool> cancelled{false}, done{false};
    };
    struct Worker {
        std::thread thread;
        std::shared_ptr<Attempt> attempt;
    };
    // Holding mutex_: an attempt begins; the one under way is cancelled (its thread ends by itself).
    void start(rcs_path_cause cause, Clock::duration delay, bool standby = false);
    void stop();
    void run(Attempt& attempt) noexcept;

    Session& session_;
    protocol::UdpConnection& connection_;
    const transport::Endpoint sharer_;
    std::mutex mutex_;                 // Below.
    std::shared_ptr<Attempt> attempt_; // The one under way, if any.
    std::vector<Worker> workers_;      // Joined once they ended, and at the end.
    bool away_ = false;                // The path stalled or is lost, until it is back (RCS_PATH_CHANGED).
    bool standbyWanted_ = false;       // setStandby...
    bool standbyOwed_ = false;         // ...while the path in use was looked for.
};

// A connection follows this end's choice of network (Session::networkChoice)
// when it changed since it last did (`seen`): its path moves to the network
// chosen (UDP's own socket, or `finder`'s: over the host's signalling, a TCP
// connection of its own), or, none left, is lost; its standby path
// (rcs_paths_config.standby) is found over the standby network, or goes.
struct NetworkSeen {
    uint64_t version = 0, standbyVersion = 0;
};
// What a connection whose sockets open now has seen of it: all but the
// standby path it is to find.
NetworkSeen networkSeenNow(Session& session);
void followNetworkChoice(Session& session, protocol::Connection& connection, PathFinder* finder, NetworkSeen& seen);

// Tells the connection how the run leaves it (Session::stop's reason) just
// before it is destroyed. Declare it after the connection.
class LeaveNotice {
public:
    LeaveNotice(Session& session, protocol::Connection& connection) : session_(session), connection_(connection) {}
    ~LeaveNotice() { connection_.leaving(session_.leaveReason()); }
    LeaveNotice(const LeaveNotice&) = delete;
    LeaveNotice& operator=(const LeaveNotice&) = delete;

private:
    Session& session_;
    protocol::Connection& connection_;
};

} // namespace rcsdk::session
