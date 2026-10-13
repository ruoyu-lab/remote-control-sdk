// RCS_TRANSPORT_FORWARDER, this end's side: the hop to the forwarder (the
// SDK's UDP connection under the magic RCSFWD, protocol/messages.hpp, over
// UDP, a TCP or WebSocket connection, or the host's channel, as a browser's
// page gives it: rcs_forwarder_config.link), and the end-to-end connections
// through it.
//
// The sharer (ForwarderLink, one per run): its hop carries the viewers' pipes,
// which a RouteDemux of their peer numbers hands to the run's UdpAcceptor as
// a host channel's routes are (each viewer an end-to-end connection of its
// own, admitted as any), and the pictures and its sound, each sealed once
// with the stream key (protocol/media_keys.hpp) and sent once, whatever the
// viewers. The hop's adaptive bitrate measures the path up to the forwarder,
// which the encodings keep within (uplinkBps). The forwarder says who came
// and went, each viewer's path, what keyframes viewers wait for, whom it
// moved; the run says what each viewer it let in and gave the key gets
// (Subscribe), and who it sent away (PeerLeft).
//
// A viewer (ForwardedConnection): one Connection over two, its end-to-end
// connection with the sharer (through its pipe) and its hop, from which the
// sealed pictures and sound come and are opened with the keys the sharer sent
// end to end; the session reads both as it reads any connection.
#pragma once

#include "protocol/connection.hpp"
#include "protocol/media_keys.hpp"
#include "protocol/route_demux.hpp"
#include "protocol/udp_connection.hpp"
#include "session/session.hpp"
#include "transport/host_channel.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace rcsdk::session {

// The hop, which outlives the link while the viewers' connections over it say goodbye.
struct ForwarderHop;

class ForwarderLink {
public:
    // Connects to the forwarder (rcs_forwarder_config: its link, address or
    // URL), checks its key and takes the room: Failure AUTHENTICATION for
    // another key (or a TLS proxy's certificate that fails), REFUSED or BUSY
    // with the forwarder's reason, NETWORK when it does not answer, CANCELLED
    // when the run ends.
    explicit ForwarderLink(Session& session);
    ~ForwarderLink();
    ForwarderLink(const ForwarderLink&) = delete;
    ForwarderLink& operator=(const ForwarderLink&) = delete;

    // The viewers' pipes, a route each (its peer number).
    protocol::RouteDemux& pipes() noexcept { return *pipes_; }
    // The route `peer`'s token names, as the forwarder said; 0 for one it did not.
    uint64_t routeOf(uint64_t peer) const;
    // What ended the hop (the forwarder went, or broke its rules), if it ended.
    void rethrowFailure() const;
    // The hop itself: it follows this end's choice of network.
    protocol::UdpConnection& hop() noexcept;
    std::string describe() const;

    // A picture as `message` (its encode()), sealed and sent: any thread.
    void publish(const EncodedPacket& packet, std::span<const uint8_t> message);
    // A packet of the sharer's sound `sound` (SoundGroups), sealed and sent: any thread.
    void publishSound(uint8_t sound, const AudioPacket& packet);
    // Pictures waiting to go to the forwarder, as time.
    uint64_t backlogNs() const noexcept;
    // Bytes over the hop so far.
    uint64_t bytes() const noexcept;
    // What the path up to the forwarder carries now, as the hop's adaptive
    // bitrate measures it; 0 until it measured, and without adaptive bitrate.
    uint32_t uplinkBps() const noexcept;

    // The stream key for a viewer let in now, which takes `streams` ((1u <<
    // stream) bits) of `encoding`: of the step of the newest keyframe of
    // those the forwarder still keeps (it starts on them at once), else of a
    // step on from now.
    protocol::MediaKey join(uint32_t streams, uint32_t encoding);
    // What `peer` gets from now on (sent when it changes): its streams of
    // `encoding`, sound `sound`, from the stream key `key` on; a viewer the
    // run dropped (it left, or was sent away) gets nothing more.
    void subscribe(uint64_t peer, uint32_t streams, uint32_t encoding, uint8_t sound, protocol::MediaKeyAck key);
    void drop(uint64_t peer);

    // The stream key: a keyframe steps it on; one that left, or the
    // rotation, makes a new generation, sealed with once advance() says so.
    protocol::MediaSealer& keys() noexcept { return sealer_; }

    // What the forwarder said since the last call: the keyframes viewers wait
    // for, the viewers it moved down.
    struct News {
        std::vector<protocol::NeedKeyframe> keyframes;
        std::vector<protocol::PeerMoved> moved;
    };
    News takeNews();
    // `peer`'s path as last reported.
    std::optional<protocol::PathReport::Peer> path(uint64_t peer) const;

private:
    void pump() noexcept;

    Session& session_;
    std::shared_ptr<ForwarderHop> hop_;
    std::shared_ptr<protocol::RouteDemux> pipes_;
    protocol::MediaSealer sealer_;
    uint32_t cacheMs_ = 0;                                  // How long the forwarder keeps keyframes.
    std::shared_ptr<std::atomic<uint64_t>> uplinkReports_;  // The hop's adaptive bitrate measured this often.
    mutable std::mutex mutex_;                              // Below.
    std::map<uint64_t, uint64_t> routes_;                   // By peer.
    std::map<uint64_t, protocol::Subscribe> subscriptions_; // As last sent, by peer.
    std::map<uint64_t, protocol::PathReport::Peer> paths_;  // By peer.
    // Each stream's encodings: their newest keyframe sealed (when, under
    // which key), and when a picture of theirs went last.
    struct Keyframe {
        Clock::time_point at{}, seen{};
        uint32_t generation = 0, step = 0;
    };
    std::array<std::array<Keyframe, RCS_MAX_ENCODINGS>, RCS_MAX_STREAMS> keyframes_{};
    News news_;
    std::exception_ptr failure_;
    std::thread thread_; // Last.
};

class ForwardedConnection final : public protocol::Connection {
public:
    // Connects through the forwarder to the sharer of the room the token
    // names; fails as ForwarderLink does, and as a connection to a sharer
    // that does not answer (NETWORK) when the room has none.
    static std::unique_ptr<protocol::Connection> connect(Session& session);
    ~ForwardedConnection() override;

    void send(const protocol::Message& message) override;
    void sendVideo(uint32_t stream, std::span<const uint8_t> encoded) override;
    protocol::Message receive() override;
    void leaving(protocol::Goodbye::Reason reason) noexcept override;
    void cancel() noexcept override;

    PathStatus status() const override;
    std::vector<protocol::PathEvent> takePathEvents() override;
    void setMigrate(bool migrate) override;
    bool lossy() const noexcept override { return true; }
    size_t maxDataPartBytes() const noexcept override;
    size_t unackedBytes() const noexcept override;
    void waitForUnackedBelow(size_t bytes, std::chrono::milliseconds timeout) override;
    std::optional<crypto::PublicKey> peerKey() const override;
    std::optional<transport::Endpoint> peerAddress() const override { return std::nullopt; }
    // The hop: it follows this end's choice of network (the pipe goes with it).
    protocol::UdpConnection& hop() noexcept { return *hop_; }
    uint64_t bytesTransferred() const noexcept override;
    uint64_t lastReceiveNs() const noexcept override;
    uint64_t lastDecryptNs() const noexcept override;

private:
    explicit ForwardedConnection(Session& session);
    void hopPump() noexcept;
    void innerPump() noexcept;
    void deliver(protocol::Message message);
    void failed(std::exception_ptr failure) noexcept;
    // A sealed picture or packet of sound, opened (or why not, counted).
    template <class Sealed>
    void opened(const Sealed& sealed);

    Session& session_;
    std::unique_ptr<protocol::UdpConnection> hop_;
    std::shared_ptr<transport::HostChannel> pipe_;
    std::unique_ptr<protocol::UdpConnection> inner_; // After the hop and the pipe: its goodbye goes through them.
    std::mutex mutex_; // Below.
    std::condition_variable arrived_;
    std::deque<protocol::Message> inbound_;
    std::exception_ptr failure_;
    std::atomic<bool> cancelled_{false};
    protocol::MediaOpener opener_; // The hop's thread's, keys added by the inner one: under mutex_.
    Clock::time_point askedKey_{};  // The last MediaKeyAck sent for a key it lacked.
    std::thread hopThread_, innerThread_;
};

} // namespace rcsdk::session
