#include "session/forwarder.hpp"

#include "session/connect.hpp"
#include "transport/framed_socket.hpp"
#include "transport/ice.hpp"
#include "transport/websocket.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>

namespace rcsdk::session {

using protocol::Message;

struct ForwarderHop {
    std::unique_ptr<protocol::UdpConnection> connection;
    std::string description; // How it reaches the forwarder.
};

namespace {

// The hop over a TCP or WebSocket connection to the forwarder, or a TLS
// proxy in front of it: the stream's only peer.
class StreamLink final : public protocol::Link {
public:
    StreamLink(std::string description, const transport::Endpoint& server)
        : description_(std::move(description)), server_(server) {}
    bool send(transport::DatagramSocket& socket, std::span<const uint8_t> datagram, uint8_t) override {
        socket.send(server_, datagram);
        return false;
    }
    // In one write (FramedSocket::sendBatch).
    size_t sendBatch(transport::DatagramSocket& socket, std::span<transport::Outgoing> batch) override {
        for (auto& datagram : batch) {
            datagram.to = &server_;
        }
        return socket.sendBatch(batch);
    }
    std::optional<std::span<const uint8_t>> unwrap(transport::DatagramSocket&, transport::Endpoint&,
                                                   std::span<const uint8_t> datagram) override {
        return datagram;
    }
    size_t overhead() const noexcept override { return 0; }
    std::string describe() const override { return description_; }
    std::optional<transport::Endpoint> peer() const override { return server_; }
    rcs_path_route route() const override { return RCS_ROUTE_DIRECT; }
    std::string silence() const override { return "the forwarder does not answer over it"; }

private:
    std::string description_;
    transport::Endpoint server_;
};

// `host` (an address, or a name: its first address) and `port`.
transport::Endpoint resolved(const std::string& host, uint16_t port, const std::string& what) {
    if (const auto numeric = transport::Endpoint::tryParse(host, port)) {
        return *numeric;
    }
    const auto all = transport::ice::resolve(host + ":" + std::to_string(port), port);
    if (all.empty()) {
        fail(RCS_ERROR_NETWORK, what + " \"" + host + "\" does not resolve");
    }
    return all.front();
}

// The hop's handshake: no secret (the forwarder proves its key, which this
// end checks; the token, inside the hop, says who this end is), the
// forwarder's magic.
transport::Credentials hopCredentials() {
    transport::Credentials credentials;
    credentials.secret = RCS_SECRET_NONE;
    credentials.forwarder = true;
    return credentials;
}

// The hop's options: this end's UDP connection to the forwarder, which over
// UDP follows this end to new paths. A sharer sends the pictures and sound
// over it, with its parity, paced at what the path up to the forwarder
// carries as its adaptive bitrate measures it (every encoding of every
// stream goes up once: room for a few at their most), or at four times the
// most one encoding takes without adaptive bitrate; a viewer receives them.
protocol::UdpOptions hopOptions(Session& session, const SessionConfig& live, bool share,
                                const std::shared_ptr<std::atomic<uint64_t>>& reports) {
    auto options = optionsFor(session, live, false);
    options.maxDatagram = protocol::udpDatagram;
    options.maxReceived = 2048;
    const bool udp = live.forwarder.link == RCS_FORWARDER_UDP;
    if (udp) {
        options.reopen = [&session](bool ipv6) {
            return openSocket(session, ipv6);
        };
    }
    // Over the host's channel it goes on where the host replaced the channel
    // with one the forwarder sees as the same peer (rcs_session_transport_restarted).
    options.migrate = (udp || live.forwarder.link == RCS_FORWARDER_HOST) && live.paths.migrate != 0;
    if (share) {
        const auto limits = rateLimits(live, live.shareVideo.bitrate_bps);
        const uint32_t most = uint32_t(std::min<uint64_t>(4ull * limits.maximum, 1'000'000'000));
        options.videoFrom = transport::Side::client;
        options.rate = limits.adaptive ? protocol::Connection::VideoRate{true,         limits.minimum, most,
                                                                         limits.start, limits.rules,   limits.fec}
                                       : protocol::Connection::VideoRate{false, most, most, most, {}, limits.fec};
        options.decideRate = [reports](const rcs_rate_report&, rcs_rate_decision&) {
            ++*reports;
        };
        options.hold = std::chrono::milliseconds(0);
    }
    return options;
}

// The WebSocket's opening over `stream`, by `deadline`: what came after
// the server's answer (frames already).
Bytes openWebSocket(transport::ByteStream& stream, const transport::websocket::Url& url, const std::string& what,
                    CancelToken cancelled, Clock::time_point deadline) {
    const auto opening = transport::websocket::open(url.hostHeader, url.target);
    std::span<const uint8_t> rest(reinterpret_cast<const uint8_t*>(opening.request.data()), opening.request.size());
    const auto due = [&] {
        throwIfCancelled(cancelled);
        if (Clock::now() >= deadline) {
            fail(RCS_ERROR_NETWORK, what + " did not answer the WebSocket opening in time");
        }
    };
    while (!rest.empty()) {
        if (const size_t taken = stream.writeSome(rest)) {
            rest = rest.subspan(taken);
        } else {
            due();
            std::this_thread::sleep_for(5ms);
        }
    }
    Bytes answer;
    std::array<uint8_t, 4096> chunk;
    for (;;) {
        if (const size_t taken = stream.readSome(chunk)) {
            answer.insert(answer.end(), chunk.begin(), chunk.begin() + ptrdiff_t(taken));
            if (const auto consumed = transport::websocket::opened(answer, opening.key)) {
                return Bytes(answer.begin() + ptrdiff_t(*consumed), answer.end());
            }
        } else if (!stream.buffered()) {
            due();
            try {
                transport::waitReady(stream.handle(), false, cancelled, std::min(deadline, Clock::now() + 20ms));
            } catch (const Failure& failure) {
                if (failure.result() == RCS_ERROR_CANCELLED) {
                    throw;
                }
            }
        }
    }
}

// How the hop reaches the forwarder (rcs_forwarder_config.link): its socket, link and description.
struct HopPath {
    std::unique_ptr<transport::DatagramSocket> socket;
    std::unique_ptr<protocol::Link> link;
    std::string description;
};
HopPath hopPath(Session& session, const SessionConfig& live) {
    const auto& f = live.forwarder;
    const auto timeout = std::chrono::milliseconds(live.networkTuning.connect_timeout_ms);
    CancelToken cancelled = session.cancelled();
    if (f.link == RCS_FORWARDER_HOST) { // The run's channel; each attempt reads it anew.
        std::string description = live.hostTransport.host_channel == RCS_HOST_CHANNEL_STREAM
                                      ? "the host's stream channel to the forwarder"
                                      : "the host's datagram channel to the forwarder";
        return {std::make_unique<transport::HostSocket>(session.hostChannel()), protocol::hostLink(description),
                description};
    }
    if (f.link == RCS_FORWARDER_WEBSOCKET) {
        const auto url = transport::websocket::parseUrl(f.websocket_url);
        const auto server = resolved(url.host, url.port, "The forwarder's WebSocket host");
        const std::string what = (url.tls ? "the TLS proxy " : "the forwarder's WebSocket entry ") + server.toString();
        const uint32_t interfaceIndex = session.boundInterface();
        std::unique_ptr<transport::ByteStream> stream;
        if (url.tls) {
            transport::TlsSettings tls;
            tls.serverName = transport::Endpoint::tryParse(url.host, 0) ? "" : url.host;
            if (std::any_of(std::begin(f.tls_certificate_sha256), std::end(f.tls_certificate_sha256),
                            [](uint8_t b) { return b != 0; })) {
                tls.certificateSha256.emplace();
                std::copy(std::begin(f.tls_certificate_sha256), std::end(f.tls_certificate_sha256),
                          tls.certificateSha256->begin());
            }
            tls.alpn = transport::httpAlpn;
            tls.pinSetting = "tls_certificate_sha256 (rcs_forwarder_config)";
            tls.nameSetting = "the host of websocket_url";
            stream = transport::connectTls(server, tls, what, cancelled, timeout, interfaceIndex);
        } else {
            stream = transport::tcpBytes(transport::connectStreamTcp(server, what, cancelled, timeout, interfaceIndex),
                                         what);
        }
        const auto arrived = openWebSocket(*stream, url, what, cancelled, Clock::now() + timeout);
        std::string description = std::string("WebSocket ") + f.websocket_url;
        return {std::make_unique<transport::FramedSocket>(std::move(stream), server, transport::webSocketFraming(),
                                                          arrived),
                std::make_unique<StreamLink>(description, server), description};
    }
    const auto server = resolved(live.connect.address, uint16_t(live.connect.port), "The forwarder's address");
    if (f.link == RCS_FORWARDER_TCP) {
        const std::string what = "the forwarder " + server.toString();
        auto stream = transport::tcpBytes(
            transport::connectStreamTcp(server, what + " over TCP", cancelled, timeout, session.boundInterface()),
            what);
        std::string description = "TCP to the forwarder at " + server.toString();
        return {std::make_unique<transport::FramedSocket>(std::move(stream), server, transport::lengthFraming()),
                std::make_unique<StreamLink>(description, server), description};
    }
    return {openSocket(session, server.ipv6()), protocol::directLink(server),
            "UDP to the forwarder at " + server.toString()};
}

// The token the host gives before each connection but the session's first
// (rcs_forwarder_config.refresh_token): applied as an update of the
// settings, and the settings from then on.
LiveConfig refreshToken(Session& session, LiveConfig live) {
    const auto& f = live->forwarder;
    if (!session.forwarderAttempt() || !f.refresh_token) {
        return live;
    }
    auto next = f;
    if (!session.callHostWith(f.user_data, f.refresh_token, next.token, uint32_t(sizeof(next.token)))) {
        return live;
    }
    next.header.next = nullptr;
    session.update(&next.header); // A token that is none fails the run (INVALID_ARGUMENT).
    log::info("network", "The host gave a new token for the forwarder");
    return session.liveConfig();
}

// Cancels a connection's blocking calls unless disarmed within a timeout.
class Deadline {
public:
    Deadline(protocol::UdpConnection& connection, std::chrono::milliseconds timeout)
        : thread_([this, &connection, timeout] {
              std::unique_lock lock(mutex_);
              if (!done_.wait_for(lock, timeout, [this] { return disarmed_; })) {
                  fired_ = true;
                  connection.cancel();
              }
          }) {}
    ~Deadline() { disarm(); }
    Deadline(const Deadline&) = delete;
    Deadline& operator=(const Deadline&) = delete;
    // Whether it was too late: the connection is cancelled for good.
    bool disarm() {
        {
            std::lock_guard lock(mutex_);
            disarmed_ = true;
        }
        done_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        return fired_;
    }

private:
    std::mutex mutex_;
    std::condition_variable done_;
    bool disarmed_ = false, fired_ = false;
    std::thread thread_; // Last.
};

// The hop to the forwarder, its key checked and this end in its room: the
// connection, how it reaches the forwarder, and the forwarder's welcome.
// Failure AUTHENTICATION, REFUSED, BUSY, NETWORK, CANCELLED.
struct Reached {
    std::unique_ptr<protocol::UdpConnection> hop;
    std::string description;
    protocol::ForwarderWelcome welcome;
};
Reached reach(Session& session, protocol::ForwarderHello::Role role,
              const std::shared_ptr<std::atomic<uint64_t>>& reports) {
    const auto live = refreshToken(session, session.liveConfig());
    const auto options = hopOptions(session, *live, role == protocol::ForwarderHello::publish, reports);
    auto path = hopPath(session, *live);
    const std::string description = path.description;
    log::info("network", "Connecting to the forwarder: " + description);
    auto hop = protocol::UdpConnection::establish(protocol::UdpConnection::Side::client, std::move(path.socket),
                                                  std::move(path.link), hopCredentials(), options, session.cancelled());
    const auto& f = live->forwarder;
    const auto key = hop->peerKey();
    const crypto::Key expected = [&] {
        crypto::Key k{};
        std::copy(std::begin(f.forwarder_key_sha256), std::end(f.forwarder_key_sha256), k.begin());
        return k;
    }();
    if (!key || !crypto::equal(crypto::sha256(*key), expected)) {
        fail(RCS_ERROR_AUTHENTICATION,
             "The forwarder (" + description + ")" +
                 (key ? " proves another key (" + transport::fingerprint(*key) + ")" : " proves no key") +
                 " than rcs_forwarder_config.forwarder_key_sha256 names; the token was not sent");
    }
    hop->send(protocol::ForwarderHello{role, std::string(f.token)});
    // Its answer, within the connect timeout (it answers at once).
    const auto timeout = std::chrono::milliseconds(live->networkTuning.connect_timeout_ms);
    try {
        Deadline deadline(*hop, timeout);
        auto message = hop->receive();
        if (const auto* welcome = std::get_if<protocol::ForwarderWelcome>(&message)) {
            if (deadline.disarm()) {
                fail(RCS_ERROR_CANCELLED, "The welcome came as the time ran out"); // NETWORK below.
            }
            session.updateStats([&](Stats& s) { s.forwarder.link = f.link; });
            return {std::move(hop), description, *welcome};
        }
        if (const auto* refused = std::get_if<protocol::Refused>(&message)) {
            fail(refused->result == RCS_ERROR_BUSY ? RCS_ERROR_BUSY : RCS_ERROR_REFUSED,
                 "The forwarder (" + description + ") refused this end: " + refused->reason);
        }
        fail(RCS_ERROR_PROTOCOL, "The forwarder sent something else than its welcome");
    } catch (const Failure& failure) {
        if (failure.result() == RCS_ERROR_CANCELLED && !session.cancelled()) {
            fail(RCS_ERROR_NETWORK,
                 "The forwarder (" + description + ") did not answer the token within " + secondsText(timeout));
        }
        throw;
    }
}

} // namespace

// ---- The sharer's link ---------------------------------------------------------------------

ForwarderLink::ForwarderLink(Session& session)
    : session_(session), hop_(std::make_shared<ForwarderHop>()),
      uplinkReports_(std::make_shared<std::atomic<uint64_t>>(0)) {
    const auto live = session.liveConfig();
    // A sharer that keeps listening waits for its forwarder (restarting, or
    // not there yet) as a viewer reconnects; a refusal ends the run.
    const auto window = std::chrono::milliseconds(live->networkTuning.reconnect_window_ms);
    const auto until = Clock::now() + window;
    for (;;) {
        try {
            auto reached = reach(session, protocol::ForwarderHello::publish, uplinkReports_);
            hop_->connection = std::move(reached.hop);
            hop_->description = std::move(reached.description);
            cacheMs_ = reached.welcome.cacheMs;
            break;
        } catch (const Failure& failure) {
            const bool retry = (failure.result() == RCS_ERROR_NETWORK || failure.result() == RCS_ERROR_RESTARTING) &&
                               live->reconnect.keep_listening && (!window.count() || Clock::now() < until);
            if (!retry) {
                throw;
            }
            log::info("network", std::string("The forwarder is not there (") + failure.what() + "); trying again");
            for (const auto next = Clock::now() + std::chrono::milliseconds(live->networkTuning.reconnect_interval_ms);
                 Clock::now() < next;) {
                throwIfCancelled(session.cancelled());
                std::this_thread::sleep_for(20ms);
            }
        }
    }
    pipes_ = std::make_shared<protocol::RouteDemux>(
        protocol::RouteDemux::Kind::datagram, [hop = hop_](uint64_t peer, std::span<const uint8_t> bytes) {
            try {
                hop->connection->send(protocol::Pipe{peer, crypto::Bytes(bytes.begin(), bytes.end())});
                return true;
            } catch (const Failure&) {
                return false; // The hop failed: as a channel with no room (the link reports why).
            }
        });
    log::info("network", "In the forwarder's room (" + hop_->description + ")");
    thread_ = log::thread([this] { pump(); });
}

ForwarderLink::~ForwarderLink() {
    hop_->connection->leaving(session_.leaveReason());
    hop_->connection->cancel(); // The pump ends; the viewers' goodbyes still go out over the hop.
    thread_.join();
}

std::string ForwarderLink::describe() const {
    return "pipe through the forwarder (" + hop_->description + ")";
}

protocol::UdpConnection& ForwarderLink::hop() noexcept {
    return *hop_->connection;
}

uint64_t ForwarderLink::routeOf(uint64_t peer) const {
    std::lock_guard lock(mutex_);
    const auto found = routes_.find(peer);
    return found == routes_.end() ? 0 : found->second;
}

void ForwarderLink::rethrowFailure() const {
    std::lock_guard lock(mutex_);
    if (failure_) {
        try {
            std::rethrow_exception(failure_);
        } catch (const Failure& failure) {
            // The viewers' path: a sharer that keeps listening reaches the forwarder anew.
            throw PeerFailure(failure.result(), std::string("The hop to the forwarder ended: ") + failure.what());
        }
    }
}

void ForwarderLink::publish(const EncodedPacket& packet, std::span<const uint8_t> message) {
    auto sealed = sealer_.seal(packet, message);
    const uint64_t bytes = sealed.sealed.size();
    {
        std::lock_guard lock(mutex_);
        auto& keyframe = keyframes_[sealed.stream][sealed.encoding];
        keyframe.seen = Clock::now();
        if (packet.keyframe) {
            keyframe.at = keyframe.seen;
            keyframe.generation = sealed.generation;
            keyframe.step = sealed.step;
        }
    }
    try {
        hop_->connection->send(sealed);
    } catch (const Failure&) {
        return; // The hop failed: the loop finds out (rethrowFailure).
    }
    session_.updateStats([&](Stats& s) {
        ++s.forwarder.media_frames;
        s.forwarder.media_bytes += bytes;
    });
}

void ForwarderLink::publishSound(uint8_t sound, const AudioPacket& packet) {
    const auto message = protocol::encode(packet);
    try {
        hop_->connection->send(sealer_.seal(packet, message, sound));
    } catch (const Failure&) {
        return; // The hop failed: the loop finds out.
    }
    session_.updateStats([](Stats& s) { ++s.forwarder.sound_packets; });
}

uint64_t ForwarderLink::backlogNs() const noexcept {
    return hop_->connection->sendBacklogNs();
}

uint64_t ForwarderLink::bytes() const noexcept {
    return hop_->connection->status().bytes;
}

uint32_t ForwarderLink::uplinkBps() const noexcept {
    return *uplinkReports_ ? hop_->connection->status().targetBitrate : 0;
}

protocol::MediaKey ForwarderLink::join(uint32_t streams, uint32_t encoding) {
    // What the forwarder keeps must still be there once the key went to the
    // viewer and its subscription to the forwarder: a second's room.
    const auto keptFor = std::chrono::milliseconds(cacheMs_) - 1s;
    const auto generation = sealer_.current().generation;
    std::optional<uint32_t> from;
    {
        std::lock_guard lock(mutex_);
        const auto now = Clock::now();
        for (uint32_t s = 0; s < RCS_MAX_STREAMS && encoding < RCS_MAX_ENCODINGS; ++s) {
            const auto& keyframe = keyframes_[s][encoding];
            if ((streams & (1u << s)) && now - keyframe.seen < 2s && keyframe.generation == generation &&
                now - keyframe.at < keptFor) {
                from = std::min(from.value_or(keyframe.step), keyframe.step);
            }
        }
    }
    return sealer_.join(from);
}

void ForwarderLink::subscribe(uint64_t peer, uint32_t streams, uint32_t encoding, uint8_t sound,
                              protocol::MediaKeyAck key) {
    const protocol::Subscribe next{peer, streams, uint8_t(encoding), sound, key.generation, key.step};
    {
        std::lock_guard lock(mutex_);
        auto& sent = subscriptions_[peer];
        if (sent == next) {
            return;
        }
        sent = next;
    }
    try {
        hop_->connection->send(next);
    } catch (const Failure&) {
    }
}

void ForwarderLink::drop(uint64_t peer) {
    {
        std::lock_guard lock(mutex_);
        subscriptions_.erase(peer);
        paths_.erase(peer);
    }
    try {
        hop_->connection->send(protocol::PeerLeft{peer});
    } catch (const Failure&) {
    }
}

ForwarderLink::News ForwarderLink::takeNews() {
    std::lock_guard lock(mutex_);
    return std::exchange(news_, {});
}

std::optional<protocol::PathReport::Peer> ForwarderLink::path(uint64_t peer) const {
    std::lock_guard lock(mutex_);
    const auto found = paths_.find(peer);
    return found == paths_.end() ? std::nullopt : std::optional(found->second);
}

// What the forwarder sends: the viewers' pipes and its news.
void ForwarderLink::pump() noexcept {
    constexpr size_t maxPeers = 4 * RCS_MAX_VIEWERS; // What the forwarder may name at once, and some that went.
    try {
        for (;;) {
            auto message = hop_->connection->receive();
            if (const auto* pipe = std::get_if<protocol::Pipe>(&message)) {
                try {
                    pipes_->receive(pipe->peer, pipe->bytes);
                } catch (const Failure&) {
                    // Too many routes wait, or the session fell behind: dropped, as a network drops it.
                }
            } else if (const auto* joined = std::get_if<protocol::PeerJoined>(&message)) {
                std::lock_guard lock(mutex_);
                if (routes_.size() >= maxPeers) {
                    routes_.erase(routes_.begin());
                }
                routes_[joined->peer] = joined->route;
            } else if (const auto* left = std::get_if<protocol::PeerLeft>(&message)) {
                pipes_->closed(left->peer); // Its connection ends (NETWORK), and it leaves.
                std::lock_guard lock(mutex_);
                routes_.erase(left->peer);
                subscriptions_.erase(left->peer);
                paths_.erase(left->peer);
            } else if (const auto* report = std::get_if<protocol::PathReport>(&message)) {
                std::lock_guard lock(mutex_);
                for (const auto& peer : report->peers) {
                    if (routes_.count(peer.peer)) {
                        paths_[peer.peer] = peer;
                    }
                }
            } else if (const auto* keyframe = std::get_if<protocol::NeedKeyframe>(&message)) {
                std::lock_guard lock(mutex_);
                if (news_.keyframes.size() < RCS_MAX_STREAMS * RCS_MAX_ENCODINGS) {
                    news_.keyframes.push_back(*keyframe);
                }
            } else if (const auto* moved = std::get_if<protocol::PeerMoved>(&message)) {
                std::lock_guard lock(mutex_);
                if (news_.moved.size() < maxPeers) {
                    news_.moved.push_back(*moved);
                }
            } else {
                fail(RCS_ERROR_PROTOCOL, "The forwarder sent what a forwarder does not send a sharer");
            }
        }
    } catch (const Failure& failure) {
        if (failure.result() != RCS_ERROR_CANCELLED) {
            log::warning("network", std::string("The hop to the forwarder ended: ") + failure.what());
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
        }
    } catch (...) {
        std::lock_guard lock(mutex_);
        failure_ = std::current_exception();
    }
}

// ---- A viewer's connection -----------------------------------------------------------------

ForwardedConnection::ForwardedConnection(Session& session) : session_(session) {}

std::unique_ptr<protocol::Connection> ForwardedConnection::connect(Session& session) {
    std::unique_ptr<ForwardedConnection> made(new ForwardedConnection(session));
    auto reached = reach(session, protocol::ForwarderHello::subscribe, std::make_shared<std::atomic<uint64_t>>(0));
    made->hop_ = std::move(reached.hop);
    session.updateStats([peer = reached.welcome.peer](Stats& s) { s.forwarder.peer = peer; });
    if (!reached.welcome.sharer) {
        log::info("network", "The forwarder's room has no sharer yet");
    }
    auto* self = made.get();
    made->pipe_ = std::make_shared<transport::HostChannel>(
        transport::HostKind::datagram, [self](std::span<const uint8_t> bytes) {
            try {
                self->hop_->send(protocol::Pipe{0, crypto::Bytes(bytes.begin(), bytes.end())});
                return true;
            } catch (const Failure&) {
                return false;
            }
        });
    made->hopThread_ = log::thread([self] { self->hopPump(); });
    const auto live = session.liveConfig();
    auto link = protocol::routeLink("pipe through the forwarder (" + reached.description + ")",
                                    transport::HostSocket::placeholder(), RCS_ROUTE_FORWARDER);
    log::info("network", "Connecting to the sharer over the " + link->describe());
    auto inner = protocol::UdpConnection::establish(
        protocol::UdpConnection::Side::client, std::make_unique<transport::HostSocket>(made->pipe_), std::move(link),
        credentialsOf(session, *live), optionsFor(session, *live, false), session.cancelled());
    {
        std::lock_guard lock(made->mutex_); // The hop's thread asks for keys over it from now on.
        made->inner_ = std::move(inner);
    }
    made->innerThread_ = log::thread([self] { self->innerPump(); });
    return made;
}

ForwardedConnection::~ForwardedConnection() {
    cancel();
    if (innerThread_.joinable()) {
        innerThread_.join();
    }
    if (hopThread_.joinable()) {
        hopThread_.join();
    }
}

void ForwardedConnection::failed(std::exception_ptr failure) noexcept {
    std::lock_guard lock(mutex_);
    if (!failure_) {
        failure_ = std::move(failure);
    }
    arrived_.notify_all();
}

void ForwardedConnection::deliver(Message message) {
    std::lock_guard lock(mutex_);
    inbound_.push_back(std::move(message));
    arrived_.notify_all();
}

// The hop: the pipe's datagrams, and the sealed pictures and sound, opened.
void ForwardedConnection::hopPump() noexcept {
    try {
        for (;;) {
            auto message = hop_->receive();
            if (auto* pipe = std::get_if<protocol::Pipe>(&message)) {
                if (pipe->peer) {
                    fail(RCS_ERROR_PROTOCOL, "The forwarder's pipe names a peer");
                }
                try {
                    pipe_->receive(pipe->bytes);
                } catch (const Failure&) {
                    // The session fell behind: dropped, as a network drops it.
                }
            } else if (const auto* sealed = std::get_if<protocol::SealedVideo>(&message)) {
                opened(*sealed);
            } else if (const auto* sound = std::get_if<protocol::SealedAudio>(&message)) {
                opened(*sound);
            } else if (std::holds_alternative<protocol::StreamGap>(message) ||
                       std::holds_alternative<protocol::VideoMissing>(message)) {
                deliver(std::move(message)); // The hop lost a picture: as any connection's.
            } else {
                fail(RCS_ERROR_PROTOCOL, "The forwarder sent what a forwarder does not send a viewer");
            }
        }
    } catch (...) {
        failed(std::current_exception());
    }
}

// A sealed picture or packet of sound: opened with the keys the sharer sent,
// or dropped and counted. One under a key this end lacks makes it say which
// it has (at most once a second), and the sharer sends the one in use.
template <class Sealed>
void ForwardedConnection::opened(const Sealed& sealed) {
    constexpr bool picture = std::is_same_v<Sealed, protocol::SealedVideo>;
    std::conditional_t<picture, EncodedPacket, AudioPacket> packet;
    protocol::MediaOpener::Outcome outcome;
    protocol::MediaKeyAck have;
    protocol::UdpConnection* ask = nullptr; // The end-to-end connection, to ask over.
    {
        std::lock_guard lock(mutex_);
        const auto now = Clock::now();
        outcome = opener_.open(sealed, now, packet);
        have = opener_.newest();
        if (outcome == protocol::MediaOpener::Outcome::unkeyed && now - askedKey_ >= 1s && inner_) {
            askedKey_ = now;
            ask = inner_.get();
        }
    }
    protocol::Connection::PathStatus hop;
    if constexpr (picture) {
        hop = hop_->status();
    }
    session_.updateStats([&](Stats& s) {
        auto& f = s.forwarder;
        switch (outcome) {
        case protocol::MediaOpener::Outcome::opened:
            if constexpr (picture) {
                ++f.media_frames;
                f.media_bytes += sealed.sealed.size();
                f.hop_round_trip_ns = hop.roundTripNs;
                f.hop_bytes = hop.bytes;
            } else {
                ++f.sound_packets;
            }
            f.generation = sealed.generation;
            f.step = sealed.step;
            break;
        case protocol::MediaOpener::Outcome::rejected:
            ++f.media_rejected;
            break;
        case protocol::MediaOpener::Outcome::replayed:
            ++f.media_replayed;
            break;
        case protocol::MediaOpener::Outcome::unkeyed:
            ++f.media_unkeyed;
            break;
        }
    });
    if (ask) {
        try {
            ask->send(have);
        } catch (const Failure&) {
        }
    }
    if (outcome == protocol::MediaOpener::Outcome::opened) {
        deliver(std::move(packet));
    } else if constexpr (picture) {
        if (outcome != protocol::MediaOpener::Outcome::replayed) {
            deliver(protocol::StreamGap{sealed.stream}); // A picture lost: its dependents cannot decode.
        }
    }
}

// The end-to-end connection: the sharer's messages, its stream keys taken here.
void ForwardedConnection::innerPump() noexcept {
    try {
        for (;;) {
            auto message = inner_->receive();
            if (const auto* key = std::get_if<protocol::MediaKey>(&message)) {
                protocol::MediaKeyAck have;
                {
                    std::lock_guard lock(mutex_);
                    opener_.add(*key);
                    have = opener_.newest();
                }
                inner_->send(have);
                continue;
            }
            deliver(std::move(message));
        }
    } catch (...) {
        failed(std::current_exception());
    }
}

void ForwardedConnection::send(const Message& message) {
    inner_->send(message);
}

void ForwardedConnection::sendVideo(uint32_t, std::span<const uint8_t>) {
    fail(RCS_ERROR_INTERNAL, "A viewer sends no video");
}

Message ForwardedConnection::receive() {
    std::unique_lock lock(mutex_);
    for (;;) {
        if (!inbound_.empty()) {
            auto message = std::move(inbound_.front());
            inbound_.pop_front();
            return message;
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        throwIfCancelled(cancelled_);
        throwIfCancelled(session_.cancelled());
        arrived_.wait_for(lock, 20ms);
    }
}

void ForwardedConnection::leaving(protocol::Goodbye::Reason reason) noexcept {
    inner_->leaving(reason);
    hop_->leaving(reason);
}

void ForwardedConnection::cancel() noexcept {
    cancelled_ = true;
    if (inner_) {
        inner_->cancel();
    }
    if (hop_) {
        hop_->cancel();
    }
    std::lock_guard lock(mutex_);
    arrived_.notify_all();
}

// The pictures' path (the hop's: what the forwarder measures for this end
// and what arrives), with the end-to-end round trip.
protocol::Connection::PathStatus ForwardedConnection::status() const {
    auto status = hop_->status();
    const auto inner = inner_->status();
    status.roundTripNs = inner.roundTripNs;
    status.path = inner.path;
    status.current.route = RCS_ROUTE_FORWARDER;
    return status;
}

std::vector<protocol::PathEvent> ForwardedConnection::takePathEvents() {
    auto events = hop_->takePathEvents();
    for (auto& event : events) {
        event.from.route = event.to.route = RCS_ROUTE_FORWARDER;
    }
    return events;
}

void ForwardedConnection::setMigrate(bool migrate) {
    hop_->setMigrate(migrate);
}

size_t ForwardedConnection::maxDataPartBytes() const noexcept {
    return inner_->maxDataPartBytes();
}

size_t ForwardedConnection::unackedBytes() const noexcept {
    return inner_->unackedBytes();
}

void ForwardedConnection::waitForUnackedBelow(size_t bytes, std::chrono::milliseconds timeout) {
    inner_->waitForUnackedBelow(bytes, timeout);
}

std::optional<crypto::PublicKey> ForwardedConnection::peerKey() const {
    return inner_->peerKey();
}

uint64_t ForwardedConnection::bytesTransferred() const noexcept {
    return hop_->bytesTransferred();
}

uint64_t ForwardedConnection::lastReceiveNs() const noexcept {
    return hop_->lastReceiveNs();
}

uint64_t ForwardedConnection::lastDecryptNs() const noexcept {
    return hop_->lastDecryptNs();
}

} // namespace rcsdk::session
