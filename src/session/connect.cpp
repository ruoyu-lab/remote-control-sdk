#include "session/connect.hpp"

#include "protocol/tcp_demux.hpp"
#include "protocol/udp_connection.hpp"
#include "protocol/udp_demux.hpp"
#include "session/forwarder.hpp"
#include "transport/framed_socket.hpp"
#include "transport/ice.hpp"
#include "transport/interfaces.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <mutex>

namespace rcsdk::session {

std::unique_ptr<transport::UdpSocket> openSocket(Session& session, bool ipv6) {
    const uint32_t index = session.boundInterface();
    auto socket = std::make_unique<transport::UdpSocket>(transport::Endpoint::parse(ipv6 ? "::" : "0.0.0.0", 0));
    if (index) {
        socket->bindToInterface(index);
    }
    return socket;
}

protocol::UdpOptions optionsFor(Session& session, const SessionConfig& live, bool share, uint32_t start,
                                std::shared_ptr<std::atomic<rcs_viewer_id>> viewer) {
    protocol::UdpOptions options;
    // UDP sockets of its own for new routes: not a listening sharer's (its
    // viewers reach it where it listens), nor a connection inside another
    // (the host's channel, a forwarder's pipe), nor TCP's (TcpRedial).
    const auto transport = live.connect.transport;
    if (transport != RCS_TRANSPORT_HOST && transport != RCS_TRANSPORT_FORWARDER && transport != RCS_TRANSPORT_TCP &&
        (!share || usesIce(transport))) {
        options.reopen = [&session](bool ipv6) {
            return openSocket(session, ipv6);
        };
    }
    options.standbyProbe = std::chrono::milliseconds(live.networkTuning.standby_probe_ms);
    if (live.connect.transport == RCS_TRANSPORT_HOST) { // The channel says what it carries; the peer's may differ.
        const auto& host = live.hostTransport;
        options.maxDatagram = host.host_max_datagram ? host.host_max_datagram : protocol::udpDatagram;
        options.maxReceived = RCS_HOST_MAX_DATAGRAM;
    }
    if (live.connect.transport == RCS_TRANSPORT_FORWARDER) {
        options.maxDatagram = RCS_HOST_MIN_DATAGRAM; // Inside a pipe datagram of the hop's (session/forwarder.hpp).
    }
    if (transport == RCS_TRANSPORT_TCP) {
        options.maxDatagram = options.maxReceived = protocol::tcpDatagram;
    }
    const auto& tuning = live.networkTuning;
    options.handshakeTimeout = std::chrono::milliseconds(tuning.connect_timeout_ms);
    options.idleTimeout = std::chrono::milliseconds(tuning.idle_timeout_ms);
    options.audioDscp = uint8_t(tuning.audio_dscp);
    // Migration once the connection runs (the session turns it on): one that
    // loses its path while it negotiates or waits to be admitted fails as
    // before, and is made anew.
    options.migrate = false;
    options.pathStall = std::chrono::milliseconds(tuning.path_stall_ms);
    options.hold = std::chrono::milliseconds(tuning.path_hold_ms);
    if (!share) {
        // A viewer keeps its connection while it would try to reconnect: for
        // the window since the path broke, which idleTimeout of silence took.
        const auto window = std::chrono::milliseconds(tuning.reconnect_window_ms);
        options.hold = !live.reconnect.reconnect ? std::chrono::milliseconds(0)
                       : window.count()          ? std::max(window - options.idleTimeout, std::chrono::milliseconds(0))
                                                 : std::chrono::milliseconds(std::chrono::hours(24 * 365));
        return options;
    }
    // The stream's own bitrate until the viewers' requests are known (share.cpp).
    const auto limits = rateLimits(live, live.shareVideo.bitrate_bps);
    options.rate = protocol::Connection::VideoRate{
        limits.adaptive, limits.minimum, limits.maximum,
        start ? std::clamp(start, limits.minimum, limits.maximum) : limits.start, limits.rules, limits.fec};
    options.decideRate = [&session, viewer](const rcs_rate_report& seen, rcs_rate_decision& decision) {
        rcs_rate_report report = seen;
        report.viewer_id = viewer ? viewer->load() : 0;
        session.rateReport(report, decision);
    };
    return options;
}

transport::Credentials credentialsOf(Session& session, const SessionConfig& live) {
    auto credentials = transport::Credentials::fromKey(std::span<const uint8_t, 32>(live.identity.key));
    credentials.secret = live.identity.secret;
    credentials.password = live.identity.password;
    credentials.passwordKeys = session.passwordKeys();
    credentials.pinAttempts = session.pinAttempts();
    if (live.deviceIdentity.has_device_key) {
        credentials.device = std::make_shared<const crypto::DeviceKey>(live.deviceIdentity.device_key);
    }
    credentials.trusted = live.trustedKeys;
    return credentials;
}

transport::CredentialsSource credentialsSource(Session& session) {
    return [&session] {
        return std::make_shared<const transport::Credentials>(credentialsOf(session, *session.liveConfig()));
    };
}

void reject(Session& session, const Failure& failure) {
    log::warning("network", std::string("Dropped a connection that failed the handshake: ") + failure.what());
    session.updateStats([](Stats& s) { ++s.network.rejected_connections; });
}

namespace {

// The STUN servers that resolve. One that does not is left out (and logged):
// the list is redundancy, and the run fails only when no path is found.
std::vector<transport::Endpoint> stunServers(const SessionConfig& config) {
    std::vector<transport::Endpoint> servers;
    for (const auto& server : config.iceServers.stun_servers) {
        if (!server[0]) {
            continue;
        }
        const auto resolved = transport::ice::resolve(server);
        if (resolved.empty()) {
            log::warning("network", std::string("STUN server \"") + server + "\" does not resolve; skipped");
        }
        servers.insert(servers.end(), resolved.begin(), resolved.end());
    }
    return servers;
}

// P2P and RELAY: the agent for one connection, from the latest settings (the
// servers and credentials may change from one connection to the next), its
// sockets on the network of `interfaceIndex` (0: unbound). Throws for what
// fails on this end: a TURN server that does not resolve.
transport::ice::Agent iceAgent(uint32_t interfaceIndex, bool controlling, const SessionConfig& live,
                               transport::ice::Ports ports, CancelToken cancelled) {
    const auto& servers = live.iceServers;
    const auto& tuning = live.networkTuning;
    std::optional<transport::ice::RelaySettings> relay;
    if (live.connect.transport == RCS_TRANSPORT_RELAY) {
        const bool tls = servers.turn_transport == RCS_TURN_TLS;
        const auto turn = transport::ice::resolve(servers.turn_server, tls ? transport::turnTlsPort : 3478);
        if (turn.empty()) {
            fail(RCS_ERROR_NETWORK, std::string("The TURN server \"") + servers.turn_server + "\" does not resolve");
        }
        relay = transport::ice::RelaySettings{turn.front(),
                                              servers.turn_username,
                                              servers.turn_password,
                                              servers.turn_transport,
                                              tls ? transport::tlsSettings(servers) : transport::TlsSettings{},
                                              std::chrono::milliseconds(tuning.connect_timeout_ms),
                                              std::chrono::milliseconds(tuning.turn_allocate_ms)};
    }
    return transport::ice::Agent(controlling, relay ? std::vector<transport::Endpoint>{} : stunServers(live), ports,
                                 cancelled, std::move(relay), interfaceIndex ? std::to_string(interfaceIndex) : "",
                                 std::chrono::milliseconds(tuning.ice_gather_ms), tuning.ice_addresses);
}

// The link over the path ICE selected: direct, or through the relay.
std::unique_ptr<protocol::Link> linkOver(transport::ice::Path& path) {
    return path.relay ? protocol::relayLink(std::move(path.relay), path.remote, path.localUfrag, path.localPassword,
                                            path.remoteType, path.relayTransport)
                      : protocol::iceLink(path.remote, path.localUfrag, path.localPassword, path.remoteType,
                                          path.socket->local());
}

// View, P2P and RELAY: this end's signal to the host, the sharer's answer to
// it, ICE, then the secure session over the chosen path.
std::unique_ptr<protocol::Connection> viewOverIce(Session& session, const SessionConfig& live,
                                                  const transport::Credentials& credentials) {
    auto agent = iceAgent(session.boundInterface(), true, live, {uint16_t(live.connect.port), 1}, session.cancelled());
    const auto signal = agent.gather(RCS_ROLE_VIEW);
    session.emitSignal(transport::ice::encode(signal), 0);
    session.setState(RCS_STATE_CONNECTING);
    const auto answer = session.waitForAnswer(signal.ufrag);
    // Another answer to this signal: the sharer started again meanwhile and
    // answered it anew; the agent uses the one the sharer's checks confirm.
    auto path = agent.connect(answer, std::chrono::milliseconds(live.networkTuning.ice_connect_ms),
                              [&] { return session.takeAnswer(signal.ufrag); });
    auto link = linkOver(path); // Before the socket goes: it describes the path.
    return protocol::UdpConnection::establish(protocol::UdpConnection::Side::client, std::move(path.socket),
                                              std::move(link), credentials, optionsFor(session, live, false),
                                              session.cancelled());
}

// TCP: a connection to the sharer at `sharer` (through interface `index`
// only, 0: any), as the datagram socket a connection's route takes.
std::unique_ptr<transport::DatagramSocket> tcpTo(const transport::Endpoint& sharer, uint32_t index,
                                                 CancelToken cancelled, std::chrono::milliseconds timeout) {
    const std::string what = "the sharer " + sharer.toString();
    return std::make_unique<transport::FramedSocket>(
        transport::tcpBytes(transport::connectStreamTcp(sharer, what + " over TCP", cancelled, timeout, index), what),
        sharer, transport::lengthFraming());
}

// The run's host channel, as a connection's path names it.
std::string hostChannelName(const SessionConfig& live) {
    return live.hostTransport.host_channel == RCS_HOST_CHANNEL_STREAM ? "host stream channel" : "host datagram channel";
}

// View, HOST: the connection over the run's channel.
std::unique_ptr<protocol::Connection> overHostChannel(Session& session, const SessionConfig& live,
                                                      const transport::Credentials& credentials) {
    auto link = protocol::hostLink(hostChannelName(live));
    log::info("network", "Connecting over the " + link->describe());
    return protocol::UdpConnection::establish(protocol::UdpConnection::Side::client,
                                              std::make_unique<transport::HostSocket>(session.hostChannel()),
                                              std::move(link), credentials, optionsFor(session, live, false),
                                              session.cancelled());
}

} // namespace

// ---- Share, P2P and RELAY: each viewer's own ICE exchange --------------------------------

namespace {

// The ports a sharer's paths take: one each, the first free from
// connect.port up, among as many as there may be paths and attempts at once.
transport::ice::Ports sharerPorts(const SessionConfig& live) {
    return {uint16_t(live.connect.port), uint32_t(maxViewers(live) + SignalQueue::maxRunning)};
}

} // namespace

IceListener::IceListener(Session& session) : session_(session) {}

IceListener::~IceListener() {
    finish();
}

std::vector<ViewerSignal> IceListener::finish() {
    cancelled_ = true;
    std::vector<Attempt> attempts;
    {
        std::lock_guard lock(mutex_);
        attempts.swap(attempts_); // None starts once cancelled.
    }
    for (auto& attempt : attempts) {
        attempt.thread.join();
    }
    std::lock_guard lock(mutex_);
    for (auto& outcome : held_) { // Connections no run took: told how this one leaves them.
        if (auto* incoming = std::get_if<Incoming>(&outcome)) {
            incoming->connection->leaving(session_.leaveReason());
        }
    }
    held_.clear();
    auto unanswered = std::move(unanswered_);
    unanswered_.clear();
    for (auto& waiting : queue_.takeWaiting()) {
        unanswered.push_back(std::move(waiting));
    }
    return unanswered;
}

void IceListener::signal(ViewerSignal signal) {
    std::lock_guard lock(mutex_);
    if (cancelled_) {
        unanswered_.push_back(std::move(signal));
        return;
    }
    const uint64_t route = signal.route;
    auto added = queue_.add(signal);
    if (added.repeat) {
        log::debug("network", "A viewer's signal came again (route " + std::to_string(route) + "); already answered");
        return;
    }
    if (added.dropped) {
        log::warning("network", "Too many viewers' signals wait: left out the oldest (route " +
                                    std::to_string(added.dropped->route) + ")");
    }
    if (added.start) {
        start(std::move(signal));
    } else {
        log::info("network", "A viewer's signal waits for one of " + std::to_string(SignalQueue::maxRunning) +
                                 " connection attempts to end (route " + std::to_string(route) + ")");
    }
}

void IceListener::start(ViewerSignal signal) {
    for (auto attempt = attempts_.begin(); attempt != attempts_.end();) { // Those that ended.
        if (*attempt->done) {
            attempt->thread.join();
            attempt = attempts_.erase(attempt);
        } else {
            ++attempt;
        }
    }
    if (cancelled_) {
        unanswered_.push_back(std::move(signal));
        return;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto held = std::make_shared<ViewerSignal>(std::move(signal)); // Still here when no thread takes it.
    try {
        attempts_.push_back({log::thread([this, done, held] {
                                 attempt(*held);
                                 *done = true;
                             }),
                             done});
    } catch (const std::system_error& error) { // The system has no thread for it.
        log::warning("network", "Could not start a connection attempt for a viewer's signal (route " +
                                    std::to_string(held->route) + "): " + error.what());
        if (auto next = queue_.finished()) {
            start(std::move(*next));
        }
    }
}

void IceListener::attempt(const ViewerSignal& signal) noexcept {
    try {
        try {
            const auto live = session_.liveConfig();
            auto agent = iceAgent(session_.boundInterface(), false, *live, sharerPorts(*live), cancelled_);
            auto answer = agent.gather(RCS_ROLE_SHARE);
            answer.answers = signal.description.ufrag;
            session_.emitSignal(transport::ice::encode(answer), signal.route);
            try {
                auto path = agent.connect(signal.description, std::chrono::milliseconds(live->networkTuning.ice_connect_ms));
                auto slot = std::make_shared<std::atomic<rcs_viewer_id>>(0);
                const auto options = optionsFor(session_, *session_.liveConfig(), true, startBitrate(), slot);
                auto link = linkOver(path); // Before the socket goes: it describes the path.
                auto connection = protocol::UdpConnection::establish(
                    protocol::UdpConnection::Side::server, std::move(path.socket), std::move(link),
                    credentialsSource(session_), options, cancelled_,
                    [this](const Failure& failure) { reject(session_, failure); });
                deliver(Incoming{std::move(connection), std::move(slot), signal.route});
            } catch (const Failure& failure) {
                if (failure.result() == RCS_ERROR_CANCELLED) {
                    throw;
                }
                // No path to this viewer, or it failed the handshake on the
                // one found: the viewer's attempt failed, not this end.
                deliver(Failed{{failure.result(), failure.what()}, true});
            }
        } catch (const Failure& failure) {
            if (failure.result() == RCS_ERROR_CANCELLED) { // The run ends: the next one answers it.
                std::lock_guard lock(mutex_);
                unanswered_.push_back(signal);
            } else { // This end's: its TURN server, its interface.
                deliver(Failed{{failure.result(), failure.what()}, false});
            }
        } catch (const std::exception& error) {
            deliver(Failed{{RCS_ERROR_INTERNAL, error.what()}, false});
        }
    } catch (...) {
        // Out of memory while telling of it: nothing more to tell.
    }
    std::lock_guard lock(mutex_);
    if (auto next = queue_.finished()) {
        start(std::move(*next));
    }
}

void IceListener::deliver(Outcome outcome) {
    std::lock_guard lock(mutex_);
    if (const auto* failed = std::get_if<Failed>(&outcome)) {
        log::info("network", std::string("A viewer's connection attempt failed (") +
                                 rcs_result_message(failed->failure.result) + "): " + failed->failure.message);
    }
    if (!sink_) {
        held_.push_back(std::move(outcome));
        return;
    }
    hand(outcome);
}

void IceListener::hand(Outcome& outcome) {
    if (auto* incoming = std::get_if<Incoming>(&outcome)) {
        sink_->accepted(std::move(*incoming));
    } else {
        const auto& failed = std::get<Failed>(outcome);
        sink_->failed(failed.failure, failed.peer);
    }
}

uint32_t IceListener::startBitrate() {
    std::lock_guard lock(mutex_);
    return sink_ ? sink_->startBitrate() : 0;
}

void IceListener::attach(Sink* sink) {
    std::lock_guard lock(mutex_);
    sink_ = sink;
    while (sink_ && !held_.empty()) {
        auto outcome = std::move(held_.front());
        held_.pop_front();
        hand(outcome);
    }
}

// ---- Acceptor ---------------------------------------------------------------------------

struct Acceptor::Impl final : IceListener::Sink {
    Session& session;
    const std::function<uint32_t()> start;
    const Accepted take;
    const Failed failure;
    const LiveConfig live; // As when the port opened.
    transport::CredentialsSource credentials;
    std::unique_ptr<protocol::Demux> port;        // UDP and TCP: where the viewers connect.
    std::shared_ptr<protocol::RouteDemux> routes; // HOST: the run's channel, a route to each viewer.
    std::shared_ptr<ForwarderLink> forwarder;      // FORWARDER: the hop, its pipes a route to each viewer.
    std::unique_ptr<protocol::UdpAcceptor> udp;
    IceListener* ice = nullptr; // P2P and RELAY.
    std::mutex mutex;
    std::exception_ptr ended; // What ended it.
    std::thread thread;       // Last.

    Impl(Session& s, std::function<uint32_t()> startBitrate, Accepted accepted, Failed failed)
        : session(s), start(std::move(startBitrate)), take(std::move(accepted)), failure(std::move(failed)),
          live(s.liveConfig()), credentials(credentialsSource(s)) {}

    void accepted(Incoming incoming) override { take(std::move(incoming)); }
    void failed(const FailureInfo& why, bool peer) override { failure(why, peer); }
    uint32_t startBitrate() override { return start(); }

    std::shared_ptr<std::atomic<rcs_viewer_id>> viewerSlot() { return std::make_shared<std::atomic<rcs_viewer_id>>(0); }

    void run() noexcept {
        try {
            const auto& token = session.cancelled();
            for (;;) {
                auto slot = viewerSlot();
                try {
                    transport::Endpoint from;
                    auto connection = udp->accept(
                        [&] { return optionsFor(session, *session.liveConfig(), true, start(), slot); }, token, &from);
                    // HOST: the route it came over is the host's for it; FORWARDER: its
                    // pipe's is its number in the room, and its token names the host's.
                    const auto route = protocol::RouteDemux::routeOf(from);
                    if (forwarder) {
                        const uint64_t peer = route.value_or(0);
                        take({std::move(connection), std::move(slot), forwarder->routeOf(peer), peer,
                              [link = forwarder, peer] { link->drop(peer); }});
                    } else {
                        take({std::move(connection), std::move(slot), route.value_or(0)});
                    }
                } catch (const std::system_error& error) { // No thread for its connection: it times out.
                    log::warning("network", std::string("Could not start a viewer's connection: ") + error.what());
                }
            }
        } catch (const Failure& failure) {
            if (failure.result() != RCS_ERROR_CANCELLED) {
                std::lock_guard lock(mutex);
                ended = std::current_exception();
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            ended = std::current_exception();
        }
    }
};

Acceptor::Acceptor(Session& session, std::function<uint32_t()> startBitrate, Accepted accepted, Failed failed)
    : impl_(std::make_unique<Impl>(session, std::move(startBitrate), std::move(accepted), std::move(failed))) {
    auto& i = *impl_;
    const auto& network = i.live->connect;
    if (i.live->identity.secret == RCS_SECRET_PASSWORD) { // Before listening: the first viewer does not wait for it.
        const auto own = i.credentials();
        own->passwordKeys->derive(own->password, own->passwordKeys->salt());
    }
    if (usesIce(network.transport)) {
        log::info("network", "Waiting for viewers' signals");
        session.setState(RCS_STATE_LISTENING);
        i.ice = &session.iceListener();
        i.ice->attach(&i); // What completed meanwhile comes at once.
        return;
    }
    const auto rejected = [&session](const Failure& failure) {
        reject(session, failure);
    };
    const auto handshakeTimeout = std::chrono::milliseconds(i.live->networkTuning.connect_timeout_ms);
    if (network.transport == RCS_TRANSPORT_TCP) {
        auto tcp = std::make_unique<protocol::TcpDemux>(
            transport::Endpoint::parse(network.address, uint16_t(network.port)), handshakeTimeout, rejected);
        log::info("network", "Listening on TCP " + tcp->local().toString());
        i.port = std::move(tcp);
        i.udp = std::make_unique<protocol::UdpAcceptor>(*i.port, i.credentials, rejected, handshakeTimeout,
                                                        protocol::tcpLink);
        session.setState(RCS_STATE_LISTENING);
    } else if (network.transport == RCS_TRANSPORT_UDP) {
        auto udp =
            std::make_unique<protocol::UdpDemux>(transport::Endpoint::parse(network.address, uint16_t(network.port)));
        log::info("network", "Listening on UDP " + udp->local().toString());
        i.port = std::move(udp);
        i.udp = std::make_unique<protocol::UdpAcceptor>(*i.port, i.credentials, rejected, handshakeTimeout);
        session.setState(RCS_STATE_LISTENING);
    } else if (network.transport == RCS_TRANSPORT_FORWARDER) {
        i.forwarder = std::make_shared<ForwarderLink>(session);
        i.udp = std::make_unique<protocol::UdpAcceptor>(
            i.forwarder->pipes(), i.credentials, rejected, handshakeTimeout,
            [name = i.forwarder->describe()](const transport::Endpoint& from) {
                return protocol::routeLink(name, from, RCS_ROUTE_FORWARDER);
            });
        session.setState(RCS_STATE_LISTENING);
    } else {
        i.routes = session.hostRoutes();
        i.udp = std::make_unique<protocol::UdpAcceptor>(
            *i.routes, i.credentials, rejected, handshakeTimeout,
            [name = hostChannelName(*i.live)](const transport::Endpoint& from) {
                return protocol::routeLink(name, from, RCS_ROUTE_HOST);
            });
        log::info("network", "Waiting for viewers over the " + hostChannelName(*i.live));
        session.setState(RCS_STATE_LISTENING);
    }
    i.thread = log::thread([&i] { i.run(); });
}

Acceptor::~Acceptor() {
    if (impl_->ice) {
        impl_->ice->attach(nullptr); // Attempts go on: the next acceptor takes what they complete.
        return;
    }
    impl_->session.cancel();
    impl_->thread.join();
}

void Acceptor::rethrowFailure() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->ended) {
            std::rethrow_exception(impl_->ended);
        }
    }
    if (impl_->forwarder) {
        impl_->forwarder->rethrowFailure();
    }
}

std::shared_ptr<ForwarderLink> Acceptor::forwarder() const {
    return impl_->forwarder;
}

namespace {

std::unique_ptr<protocol::Connection> connect(Session& session, const SessionConfig& live,
                                              const transport::Credentials& credentials) {
    const auto& network = live.connect;
    if (usesIce(network.transport)) {
        return viewOverIce(session, live, credentials);
    }
    if (network.transport == RCS_TRANSPORT_HOST) {
        return overHostChannel(session, live, credentials);
    }
    if (network.transport == RCS_TRANSPORT_FORWARDER) {
        return ForwardedConnection::connect(session);
    }
    const auto peer = transport::Endpoint::parse(network.address, uint16_t(network.port));
    // The network the settings choose (local_interface, rcs_paths_config), or
    // unbound: its datagrams leave by the system's default route, and another
    // socket like it takes over when this one fails or the network changed
    // under it (UdpOptions::reopen).
    const uint32_t interfaceIndex = session.boundInterface();
    const std::string through = interfaceIndex ? " through interface " + std::to_string(interfaceIndex) : "";
    if (network.transport == RCS_TRANSPORT_UDP) {
        log::info("network", "Connecting over UDP to " + peer.toString() + through);
        return protocol::UdpConnection::establish(protocol::UdpConnection::Side::client,
                                                  openSocket(session, peer.ipv6()), protocol::directLink(peer),
                                                  credentials, optionsFor(session, live, false), session.cancelled());
    }
    log::info("network", "Connecting over TCP to " + peer.toString() + through);
    auto socket = tcpTo(peer, interfaceIndex, session.cancelled(),
                        std::chrono::milliseconds(live.networkTuning.connect_timeout_ms));
    return protocol::UdpConnection::establish(protocol::UdpConnection::Side::client, std::move(socket),
                                              protocol::tcpLink(peer), credentials, optionsFor(session, live, false),
                                              session.cancelled());
}

} // namespace

NetworkSeen networkSeenNow(Session& session) {
    const auto choice = session.networkChoice();
    return {choice.version, session.liveConfig()->paths.standby ? 0 : choice.standbyVersion};
}

void followNetworkChoice(Session& session, protocol::Connection& connection, PathFinder* finder, NetworkSeen& seen) {
    const auto choice = session.networkChoice();
    auto* udp = dynamic_cast<protocol::UdpConnection*>(&connection);
    if (auto* forwarded = dynamic_cast<ForwardedConnection*>(&connection)) {
        udp = &forwarded->hop(); // A viewer's pipe goes with its hop.
    }
    if (!udp || (choice.version == seen.version && choice.standbyVersion == seen.standbyVersion)) {
        return;
    }
    if (choice.standbyVersion != seen.standbyVersion) { // First: a standby path on a network that went is no help.
        seen.standbyVersion = choice.standbyVersion;
        const auto live = session.liveConfig();
        if (!live->paths.standby || !choice.standbyIndex) {
            if (finder) {
                finder->setStandby(false);
            }
            udp->dropStandby(live->paths.standby ? RCS_PATH_CAUSE_NETWORK : RCS_PATH_CAUSE_SETTINGS,
                             live->paths.standby ? RCS_ERROR_NETWORK : RCS_OK,
                             live->paths.standby ? choice.standbyWhy : "Standby is off (rcs_paths_config)");
        } else if (finder) {
            finder->setStandby(true);
        } else {
            try {
                const auto peer = transport::Endpoint::parse(live->connect.address, uint16_t(live->connect.port));
                auto socket = std::make_unique<transport::UdpSocket>(
                    transport::Endpoint::parse(peer.ipv6() ? "::" : "0.0.0.0", 0));
                if (const uint32_t index = session.standbyInterface()) {
                    socket->bindToInterface(index);
                }
                udp->addRoute(std::move(socket), protocol::directLink(peer), RCS_PATH_CAUSE_NETWORK, false, true);
            } catch (const Failure& failure) {
                udp->dropStandby(RCS_PATH_CAUSE_NETWORK, failure.result(),
                                 std::string("Could not open a socket for a standby path: ") + failure.what());
            }
        }
    }
    if (choice.version != seen.version) {
        seen.version = choice.version;
        if (choice.lost) {
            udp->leaveRoute(choice.cause, choice.why);
        } else if (finder) {
            finder->move(choice.cause);
        } else {
            udp->moveRoute(choice.cause);
        }
    }
}

std::unique_ptr<protocol::Connection> connectToSharer(Session& session) {
    const auto live = session.liveConfig();
    const auto credentials = credentialsOf(session, *live);
    try {
        return connect(session, *live, credentials);
    } catch (const transport::UntrustedPeer& refused) {
        if (refused.key()) { // The host may show it, or decide to trust it.
            session.setRefusedPeerKey(*refused.key());
        }
        throw;
    }
}

// ---- Restarting a path (P2P and RELAY) ----------------------------------------------------

struct PathRestart::Attempt {
    std::optional<transport::ice::Restart> answering; // The peer's restart it answers; none: this end's own.
    rcs_path_cause cause = RCS_PATH_CAUSE_STALLED;    // Why: for the change the path it finds makes.
    Clock::duration delay{};                          // Its wait first: the path may come back by itself.
    bool standby = false;                             // For a standby path, over the standby network.
    std::atomic<bool> cancelled{false}, done{false};
    std::mutex mutex; // Below.
    std::condition_variable answered;
    std::string ufrag; // This end's, once gathered: the peer's answer names it.
    std::optional<transport::ice::Description> answer;
    bool unknown = false; // The peer answered that it has no such connection (or not yet).
};

namespace {

// A path that only stalls gets this long to come back by itself (a Wi-Fi
// radio's hiccup) or over a socket of this end's own (a relay's allocation
// moved: no signalling) before the host's signalling is asked.
constexpr auto stallGrace = 1s;

crypto::Key derived(const crypto::Key& exporter, std::string_view label) {
    return crypto::hmacSha256(exporter, std::span(reinterpret_cast<const uint8_t*>(label.data()), label.size()));
}

} // namespace

PathRestart::PathRestart(Session& session, protocol::UdpConnection& connection, uint64_t route)
    : session_(session), connection_(connection), route_(route),
      key_(derived(connection.exporter(), "rcsdk restart key")) {
    const auto tag = derived(connection.exporter(), "rcsdk restart tag");
    std::copy_n(tag.begin(), tag_.size(), tag_.begin());
    session_.addRestart(*this);
}

PathRestart::~PathRestart() {
    session_.removeRestart(*this); // No signal reaches it from now on.
    std::vector<Worker> workers;
    {
        std::lock_guard lock(mutex_);
        stop();
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        worker.thread.join();
    }
}

void PathRestart::pathChanged(const protocol::PathEvent& event) {
    std::lock_guard lock(mutex_);
    if (event.change == RCS_PATH_CHANGING || event.change == RCS_PATH_LOST) {
        lost_ = lost_ || event.change == RCS_PATH_LOST;
        if (!attempt_ || attempt_->standby) { // The path in use first.
            stop();
            start(std::nullopt, event.cause == RCS_PATH_CAUSE_NETWORK ? RCS_PATH_CAUSE_NETWORK : RCS_PATH_CAUSE_STALLED,
                  event.change == RCS_PATH_CHANGING ? stallGrace : Clock::duration::zero());
        }
    } else if (event.change == RCS_PATH_CHANGED) {
        lost_ = false;
        if (attempt_ && !attempt_->standby) {
            stop(); // Back by itself, or another way (the standby path): nothing more to find.
        }
    } else if (event.change == RCS_PATH_STANDBY && event.result != RCS_OK && event.cause == RCS_PATH_CAUSE_STALLED &&
               standbyWanted_ && !attempt_) { // It stopped answering: found anew, a while later.
        start(std::nullopt, RCS_PATH_CAUSE_NETWORK,
              std::chrono::milliseconds(session_.liveConfig()->networkTuning.standby_probe_ms), true);
    }
}

void PathRestart::signal(const transport::ice::Restart& restart) {
    std::lock_guard lock(mutex_);
    const auto& d = restart.description;
    if (!d.answers.empty()) { // An answer, to this end's attempt under way if it names it.
        if (attempt_ && !attempt_->answering) {
            std::lock_guard answered(attempt_->mutex);
            if (attempt_->ufrag == d.answers) {
                attempt_->answer = d;
                attempt_->answered.notify_all();
            }
        }
        return;
    }
    if (restart.generation <= peerGeneration_) {
        return; // The host delivered an earlier one again.
    }
    peerGeneration_ = restart.generation;
    if (attempt_ && !attempt_->answering && session_.config().role == RCS_ROLE_VIEW &&
        attempt_->standby <= restart.standby) {
        return; // Both ends started one: the viewer's goes on, and the sharer answers it.
    }
    if (attempt_ && restart.standby && !attempt_->standby) {
        return; // This end looks for the path in use: a standby one waits.
    }
    log::info("network", std::string("The peer restarts ") + (restart.standby ? "its standby path" : "the path") +
                             " (its restart " + std::to_string(restart.generation) + ")");
    stop();
    start(restart, RCS_PATH_CAUSE_PEER, Clock::duration::zero(), restart.standby);
}

void PathRestart::unknown() {
    {
        std::lock_guard lock(mutex_);
        if (attempt_ && !attempt_->answering) {
            std::lock_guard answered(attempt_->mutex);
            attempt_->unknown = true;
            attempt_->answered.notify_all();
        }
    }
    connection_.peerForgot();
}

void PathRestart::move(rcs_path_cause cause) {
    std::lock_guard lock(mutex_);
    stop();
    start(std::nullopt, cause, Clock::duration::zero());
}

void PathRestart::setStandby(bool wanted) {
    std::lock_guard lock(mutex_);
    standbyWanted_ = wanted;
    if (attempt_ && !attempt_->standby) {
        return; // The path in use first; the standby path once the stall that needs it is over.
    }
    stop();
    if (wanted) {
        start(std::nullopt, RCS_PATH_CAUSE_NETWORK, Clock::duration::zero(), true);
    }
}

void PathRestart::start(std::optional<transport::ice::Restart> answering, rcs_path_cause cause, Clock::duration delay,
                        bool standby) {
    std::erase_if(workers_, [](Worker& worker) { // Those that ended (they need no lock to end).
        if (!worker.attempt->done) {
            return false;
        }
        worker.thread.join();
        return true;
    });
    auto attempt = std::make_shared<Attempt>();
    attempt->answering = std::move(answering);
    attempt->cause = cause;
    attempt->delay = delay;
    attempt->standby = standby;
    attempt_ = attempt;
    workers_.push_back({log::thread([this, attempt] { run(*attempt); }), attempt});
}

void PathRestart::stop() {
    if (attempt_) {
        attempt_->cancelled = true;
        std::lock_guard lock(attempt_->mutex);
        attempt_->answered.notify_all();
        attempt_.reset();
    }
}

// One exchange: candidates gathered, sealed and signalled, the peer's in
// answer (or the restart this answers), ICE, and the path to the connection.
// While the path stays lost, this end's own tries again every second (a
// standby path's: every standby_probe_ms); an answer is not repeated (its
// peer starts anew if need be).
void PathRestart::run(Attempt& attempt) noexcept {
    const bool ownStandby = attempt.standby && !attempt.answering;
    const auto once = [&] {
        const auto live = session_.liveConfig();
        const bool viewer = live->role == RCS_ROLE_VIEW;
        auto agent = iceAgent(ownStandby ? session_.standbyInterface() : session_.boundInterface(), viewer, *live,
                              viewer ? transport::ice::Ports{uint16_t(live->connect.port), 1} : sharerPorts(*live),
                              attempt.cancelled);
        transport::ice::Restart mine;
        mine.tag = tag_;
        mine.standby = attempt.standby;
        mine.description = agent.gather(viewer ? RCS_ROLE_VIEW : RCS_ROLE_SHARE);
        if (attempt.answering) {
            mine.description.answers = attempt.answering->description.ufrag;
        }
        {
            std::lock_guard lock(mutex_);
            mine.generation = ++generation_;
        }
        {
            std::lock_guard lock(attempt.mutex);
            attempt.ufrag = mine.description.ufrag;
        }
        log::info("network", std::string(attempt.answering ? "Answering the peer's restart of "
                                                           : "Restarting through the host's signalling ") +
                                 (attempt.standby ? "a standby path" : "the path") + " (restart " +
                                 std::to_string(mine.generation) + ")");
        session_.emitSignal(transport::ice::encodeRestart(mine, key_), route_);
        const auto timeout = std::chrono::milliseconds(live->networkTuning.ice_connect_ms);
        transport::ice::Description remote;
        if (attempt.answering) {
            remote = attempt.answering->description;
        } else {
            std::unique_lock lock(attempt.mutex);
            attempt.answered.wait_for(lock, timeout,
                                      [&] { return attempt.answer || attempt.unknown || attempt.cancelled; });
            throwIfCancelled(attempt.cancelled);
            if (std::exchange(attempt.unknown, false)) {
                fail(RCS_ERROR_NETWORK, "The peer has no connection of this restart (it let it go, or does not have "
                                        "it yet)");
            }
            if (!attempt.answer) {
                fail(RCS_ERROR_NETWORK, "The peer did not answer within " + secondsText(timeout) +
                                            " (the host's signalling may be down as well)");
            }
            remote = *attempt.answer;
        }
        auto path = agent.connect(remote, timeout);
        auto link = linkOver(path); // Before the socket goes: it describes the path.
        connection_.addRoute(std::move(path.socket), std::move(link), attempt.cause, true, attempt.standby);
    };
    for (const auto until = Clock::now() + attempt.delay; Clock::now() < until && !attempt.cancelled;) {
        std::this_thread::sleep_for(20ms);
    }
    for (bool first = true; !attempt.cancelled; first = false) {
        if (!first) {
            const auto wait =
                ownStandby
                    ? Clock::duration(std::chrono::milliseconds(session_.liveConfig()->networkTuning.standby_probe_ms))
                    : Clock::duration(1s);
            for (const auto until = Clock::now() + wait; Clock::now() < until && !attempt.cancelled;) {
                std::this_thread::sleep_for(20ms);
            }
            std::lock_guard lock(mutex_);
            if (!(ownStandby ? standbyWanted_ : lost_) || attempt.cancelled) {
                break;
            }
        }
        try {
            once();
            break;
        } catch (const Failure& failure) {
            if (attempt.cancelled || failure.result() == RCS_ERROR_CANCELLED) {
                break;
            }
            log::info("network", std::string("Restarting the path failed: ") + failure.what());
            if (attempt.answering) {
                break;
            }
        } catch (const std::exception& error) {
            log::warning("network", std::string("Restarting the path failed: ") + error.what());
            break;
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (attempt_.get() == &attempt) {
            attempt_.reset(); // Done: the next change of the path may start another.
        }
    }
    attempt.done = true;
}

// ---- A TCP viewer's new TCP connections -------------------------------------------------

TcpRedial::TcpRedial(Session& session, protocol::UdpConnection& connection)
    : session_(session), connection_(connection),
      sharer_(transport::Endpoint::parse(session.liveConfig()->connect.address,
                                         uint16_t(session.liveConfig()->connect.port))) {}

TcpRedial::~TcpRedial() {
    std::vector<Worker> workers;
    {
        std::lock_guard lock(mutex_);
        stop();
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        worker.thread.join();
    }
}

void TcpRedial::pathChanged(const protocol::PathEvent& event) {
    std::lock_guard lock(mutex_);
    if (event.change == RCS_PATH_CHANGING || event.change == RCS_PATH_LOST) {
        away_ = true;
        if (!attempt_ || attempt_->standby) { // The path in use first.
            stop();
            const bool stalledOnly = event.change == RCS_PATH_CHANGING && event.cause == RCS_PATH_CAUSE_STALLED;
            start(event.cause == RCS_PATH_CAUSE_STALLED ? RCS_PATH_CAUSE_STALLED : RCS_PATH_CAUSE_NETWORK,
                  stalledOnly ? stallGrace : Clock::duration::zero());
        }
    } else if (event.change == RCS_PATH_CHANGED) {
        away_ = false;
        if (attempt_ && !attempt_->standby) {
            stop(); // Back by itself, or another way: nothing more to find.
        }
        if (std::exchange(standbyOwed_, false) && standbyWanted_ && !attempt_) {
            start(RCS_PATH_CAUSE_NETWORK, Clock::duration::zero(), true);
        }
    } else if (event.change == RCS_PATH_STANDBY && event.result != RCS_OK && standbyWanted_ && !attempt_) {
        // Its TCP connection broke, or it stopped answering: anew, a while later.
        start(RCS_PATH_CAUSE_NETWORK,
              std::chrono::milliseconds(session_.liveConfig()->networkTuning.standby_probe_ms), true);
    }
}

void TcpRedial::move(rcs_path_cause cause) {
    std::lock_guard lock(mutex_);
    stop();
    start(cause, Clock::duration::zero());
}

void TcpRedial::setStandby(bool wanted) {
    std::lock_guard lock(mutex_);
    standbyWanted_ = wanted;
    if (attempt_ && !attempt_->standby) {
        standbyOwed_ = wanted; // The path in use first; the standby path once it is back.
        return;
    }
    stop();
    if (wanted) {
        start(RCS_PATH_CAUSE_NETWORK, Clock::duration::zero(), true);
    }
}

void TcpRedial::start(rcs_path_cause cause, Clock::duration delay, bool standby) {
    std::erase_if(workers_, [](Worker& worker) { // Those that ended (they need no lock to end).
        if (!worker.attempt->done) {
            return false;
        }
        worker.thread.join();
        return true;
    });
    auto attempt = std::make_shared<Attempt>();
    attempt->cause = cause;
    attempt->delay = delay;
    attempt->standby = standby;
    attempt_ = attempt;
    workers_.push_back({log::thread([this, attempt] { run(*attempt); }), attempt});
}

void TcpRedial::stop() {
    if (attempt_) {
        attempt_->cancelled = true;
        attempt_.reset();
    }
}

// One TCP connection to the sharer, handed to the connection. While the path
// is away (stalled or lost), another every second, whether the last one
// connected or not (the path may still not answer over it: the network under
// it went too); a standby one that failed, again every standby_probe_ms while
// standby is wanted.
void TcpRedial::run(Attempt& attempt) noexcept {
    const auto pause = [&](Clock::duration wait) {
        for (const auto until = Clock::now() + wait; Clock::now() < until && !attempt.cancelled;) {
            std::this_thread::sleep_for(20ms);
        }
    };
    pause(attempt.delay);
    for (bool first = true; !attempt.cancelled; first = false) {
        if (!first) {
            const auto probe = std::chrono::milliseconds(session_.liveConfig()->networkTuning.standby_probe_ms);
            pause(attempt.standby ? Clock::duration(probe) : Clock::duration(1s));
            std::lock_guard lock(mutex_);
            if (!(attempt.standby ? standbyWanted_ : away_) || attempt.cancelled) {
                break;
            }
        }
        try {
            const auto live = session_.liveConfig();
            const uint32_t index = attempt.standby ? session_.standbyInterface() : session_.boundInterface();
            log::info("network", std::string("Connecting to the sharer again over TCP") +
                                     (attempt.standby ? " for a standby path" : "") +
                                     (index ? " through interface " + std::to_string(index) : ""));
            auto socket = tcpTo(sharer_, index, attempt.cancelled,
                                std::chrono::milliseconds(live->networkTuning.connect_timeout_ms));
            connection_.addRoute(std::move(socket), protocol::tcpLink(sharer_), attempt.cause, false, attempt.standby);
            if (attempt.standby) {
                break; // The connection proves it from now on, and says when it fails.
            }
        } catch (const Failure& failure) {
            if (attempt.cancelled || failure.result() == RCS_ERROR_CANCELLED) {
                break;
            }
            log::info("network", std::string("Connecting to the sharer again over TCP failed: ") + failure.what());
        } catch (const std::exception& error) {
            log::warning("network", std::string("Connecting to the sharer again over TCP failed: ") + error.what());
            break;
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (attempt_.get() == &attempt) {
            attempt_.reset(); // Done: the next change of the path may start another.
            if (!attempt.standby && !away_ && std::exchange(standbyOwed_, false) && standbyWanted_) {
                start(RCS_PATH_CAUSE_NETWORK, Clock::duration::zero(), true);
            }
        }
    }
    attempt.done = true;
}

} // namespace rcsdk::session
