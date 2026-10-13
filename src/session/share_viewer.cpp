#include "session/share_viewer.hpp"

#include "agent/agent.hpp"
#include "session/negotiation.hpp"
#include "util/log.hpp"

#include <algorithm>

namespace rcsdk::session {
namespace {

// Video that waited this long is dropped; the viewer resumes at a keyframe.
// Sound and pointer moves that waited this long are dropped too.
constexpr uint64_t maxBehindNs = 1'000'000'000;

// What may be dropped once it is late: the next sound packet and pointer
// position replace it.
bool stale(const protocol::Message& message) {
    return std::holds_alternative<AudioPacket>(message) ||
           std::holds_alternative<protocol::CursorPosition>(message);
}

} // namespace

// ---- Outbox -----------------------------------------------------------------------------

Outbox::Outbox(Viewer& viewer, protocol::Connection& connection) : viewer_(viewer), connection_(connection) {
    for (auto& layers : wantedLayers_) {
        layers = RCS_SCALABILITY_L1T3; // All there are.
    }
    layers_.fill(RCS_SCALABILITY_L1T3);
    thread_ = log::thread([this] { run(); });
}

Outbox::~Outbox() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        wake_.notify_all();
    }
    thread_.join();
}

void Outbox::message(protocol::Message message) {
    const uint64_t now = monotonicNs();
    std::lock_guard lock(mutex_);
    items_.push_back({nullptr, std::move(message), now});
    wake_.notify_one();
    const auto oldest = std::find_if(items_.begin(), items_.end(), [](const Item& i) { return i.message; });
    if (now - oldest->queuedNs <= maxBehindNs) {
        return;
    }
    // Late sound goes, and every pointer position but the newest.
    bool position = false;
    for (auto i = items_.rbegin(); i != items_.rend(); ++i) {
        if (!i->message || !stale(*i->message)) {
            continue;
        }
        if (std::holds_alternative<protocol::CursorPosition>(*i->message) ? std::exchange(position, true)
                                                                          : now - i->queuedNs > maxBehindNs) {
            i->message.reset();
        }
    }
    std::erase_if(items_, [](const Item& i) { return !i.video && !i.message; });
}

void Outbox::video(const std::shared_ptr<const VideoMessage>& packet) {
    const uint64_t now = monotonicNs();
    std::lock_guard lock(mutex_);
    if (queuedNs_ && now - queuedNs_ > maxBehindNs) {
        dropVideo(); // A second behind: what waits is stale.
    }
    auto& layers = layers_[packet->stream];
    if (const uint32_t wanted = wantedLayers_[packet->stream]; wanted < layers || !packet->temporalId) {
        layers = wanted;
    }
    if (packet->temporalId >= layers && !packet->keyframe) {
        ++sent_.upper;
        return;
    }
    if (!open_[packet->stream]) {
        if (!packet->keyframe) {
            ++sent_.skipped;
            return;
        }
        open_[packet->stream] = true;
    }
    if (!queuedNs_) {
        queuedNs_ = now;
    }
    items_.push_back({packet, std::nullopt, now});
    wake_.notify_one();
}

void Outbox::dropVideo(size_t popped) {
    ++drops_;
    droppedBytes_ += popped;
    sent_.skipped += popped != 0;
    std::erase_if(items_, [&](const Item& item) {
        droppedBytes_ += item.video ? item.video->bytes.size() : 0;
        sent_.skipped += item.video != nullptr;
        return item.video != nullptr;
    });
    queuedNs_ = 0;
    for (uint32_t stream = 0; stream < open_.size(); ++stream) {
        wanted_ |= open_[stream] ? 1u << stream : 0; // The streams it was getting.
        open_[stream] = false;
    }
}

uint32_t Outbox::takeKeyframeWanted() {
    {
        std::lock_guard lock(mutex_);
        if (!wanted_) {
            return 0;
        }
    }
    if (connection_.sendBacklogNs() > maxBehindNs / 2) {
        return 0; // It would wait behind what the connection holds.
    }
    std::lock_guard lock(mutex_);
    return std::exchange(wanted_, 0);
}

std::pair<uint64_t, uint64_t> Outbox::drops() const {
    std::lock_guard lock(mutex_);
    return {drops_, droppedBytes_};
}

void Outbox::switchVideo() {
    {
        std::lock_guard lock(mutex_);
        std::erase_if(items_, [](const Item& item) { return item.video != nullptr; });
        queuedNs_ = 0;
        open_.fill(false);
        wanted_ = 0;
        for (size_t stream = 0; stream < layers_.size(); ++stream) {
            layers_[stream] = wantedLayers_[stream]; // The new encoding starts at a keyframe.
        }
    }
    connection_.discardVideo(); // A send under way finishes; the rest of the old encoding goes.
}

Outbox::Sent Outbox::sent() const {
    std::lock_guard lock(mutex_);
    return sent_;
}

uint64_t Outbox::backlogNs() const {
    uint64_t waiting;
    {
        std::lock_guard lock(mutex_);
        waiting = queuedNs_ ? monotonicNs() - queuedNs_ : 0;
    }
    return waiting + connection_.sendBacklogNs();
}

void Outbox::run() noexcept {
    try {
        for (;;) {
            Item item;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || !items_.empty(); });
                if (stopping_) {
                    return;
                }
                item = std::move(items_.front());
                items_.pop_front();
                if (item.video) {
                    queuedNs_ = 0;
                    for (const auto& waiting : items_) { // The next video waiting.
                        if (waiting.video) {
                            queuedNs_ = waiting.queuedNs;
                            break;
                        }
                    }
                    if (connection_.sendBacklogNs() > maxBehindNs) {
                        // The connection itself holds a second of video (its
                        // pacing): resume at the next keyframe.
                        dropVideo(item.video->bytes.size());
                        continue;
                    }
                }
            }
            if (item.video) {
                connection_.sendVideo(item.video->stream, item.video->bytes);
            } else {
                connection_.send(*item.message);
            }
            {
                std::lock_guard lock(mutex_);
                sent_.frames += item.video != nullptr;
                sent_.bytes += item.video ? item.video->bytes.size() : 0;
            }
            const uint64_t bytes = connection_.bytesTransferred();
            viewer_.updateStats([&](Stats& s) { s.network.bytes = bytes; });
        }
    } catch (...) {
        viewer_.fail(std::current_exception());
    }
}

// ---- ViewerMessages ---------------------------------------------------------------------

ViewerMessages::ViewerMessages(Session& session, Viewer& viewer, protocol::Connection& connection)
    : session_(session), viewer_(viewer), connection_(connection), data_(session, viewer.data, viewer.id) {
    thread_ = log::thread([this] { run(); });
}

ViewerMessages::~ViewerMessages() {
    thread_.join(); // The viewer cancelled its connection first.
}

void ViewerMessages::run() noexcept {
    try {
        while (!viewer_.cancelled) {
            const auto message = connection_.receive();
            if (const auto* input = std::get_if<protocol::Input>(&message)) {
                apply(*input);
            } else if (const auto* keyframe = std::get_if<protocol::KeyframeRequest>(&message)) {
                viewer_.requestKeyframe(keyframe->stream);
            } else if (const auto* lost = std::get_if<protocol::RefLost>(&message)) {
                viewer_.lost(lost->stream, lost->encoding, lost->after, lost->gaveUp);
            } else if (const auto* settings = std::get_if<protocol::ViewerSettings>(&message)) {
                viewer_.setSettings(*settings);
            } else if (const auto* offer = std::get_if<protocol::CodecOffer>(&message)) {
                viewer_.setOffer(*offer);
                session_.setPeer(peerOf(*offer, connection_.peerKey()), viewer_.id);
            } else if (const auto* format = std::get_if<AudioFormat>(&message)) {
                viewer_.heard.format(*format);
            } else if (const auto* sound = std::get_if<AudioPacket>(&message)) {
                viewer_.heard.packet(*sound, monotonicNs());
            } else if (const auto* off = std::get_if<protocol::AudioOff>(&message)) {
                viewer_.heard.off(off->reason);
            } else if (const auto* part = std::get_if<protocol::DataPart>(&message)) {
                data_.part(*part);
            } else if (const auto* datagram = std::get_if<protocol::DataDatagram>(&message)) {
                data_.datagram(*datagram);
            } else if (const auto* stalls = std::get_if<protocol::PathStalls>(&message)) {
                session_.peerStalls(*stalls, viewer_.id);
            } else if (const auto* key = std::get_if<protocol::MediaKeyAck>(&message);
                       key && session_.config().connect.transport == RCS_TRANSPORT_FORWARDER) {
                viewer_.keyHeld = uint64_t(key->generation) << 32 | key->step;
            } else {
                fail(RCS_ERROR_PROTOCOL, "Unexpected message from the viewer");
            }
        }
    } catch (...) {
        viewer_.fail(std::current_exception());
    }
    // What the viewer holds is released when it leaves.
}

void ViewerMessages::apply(const protocol::Input& input) {
    viewer_.updateStats([](Stats& s) { ++s.input.received; });
    auto& control = session_.shareControl();
    const auto current = [&] {
        return input.epoch == viewer_.epoch && control.granted(viewer_.control);
    };
    bool byHost;
    {
        std::lock_guard lock(control.mutex);
        if (!current()) {
            return;
        }
        byHost = control.byHost(input.event.type);
    }
    // The host sees every event first, without the control lock held. With
    // the system executing the event it may drop it (only that one: what it
    // means for keys held is the host's to judge); release-all always goes on
    // to both. One the host executes is done once on_input returned.
    rcs_input_event event = input.event;
    event.viewer_id = viewer_.id;
    const bool hostSees = session_.callbacks().on_input != nullptr;
    const bool decided = session_.callOnInput(event);
    if (hostSees && !decided && event.type != RCS_INPUT_RELEASE_ALL && !byHost) {
        return;
    }
    rcs_result result;
    {
        std::lock_guard lock(control.mutex);
        if (!current()) { // Revoked, or the executors changed, while the guard ran.
            return;
        }
        // An OS refusal (an elevated window, or the desktop switched before
        // the next availability check) releases what is held in the agent;
        // the grant follows the OS's availability (the stream's loop).
        result = byHost && !control.hostExecutes ? RCS_OK : control.injector->apply(input.event);
    }
    if (result == agent::kInputDeferred) {
        return; // A remote agent executes it and reports the results, of every viewer together.
    }
    viewer_.updateStats([&](Stats& s) {
        s.input.last_result = result;
        s.input.applied += result == RCS_OK;
    });
}

ViewerHello receiveViewerHello(protocol::Connection& connection) {
    ViewerHello hello;
    auto message = connection.receive();
    auto* offer = std::get_if<protocol::CodecOffer>(&message);
    if (!offer) {
        throw PeerFailure(RCS_ERROR_PROTOCOL, "The viewer did not start with a codec offer");
    }
    hello.offer = std::move(*offer);
    message = connection.receive();
    auto* settings = std::get_if<protocol::ViewerSettings>(&message);
    if (!settings) {
        throw PeerFailure(RCS_ERROR_PROTOCOL, "The viewer did not send its settings after its offer");
    }
    hello.settings = *settings;
    return hello;
}

// ---- Viewer -----------------------------------------------------------------------------

Viewer::Viewer(Session& session, AudioHub& audioHub, SoundGroups* sounds, rcs_viewer_id viewerId, uint64_t hostRoute,
               uint64_t roomPeer, std::unique_ptr<protocol::Connection> connection, ViewerHello hello, bool allowed,
               const rcs_viewer_video& video)
    : id(viewerId), route(hostRoute), peer(roomPeer), joinedNs(monotonicNs()), control(allowed), pinned(video.encoding),
      pinnedLayers(video.layers), counts(video.counts_for_bitrate != 0), session_(session),
      connection_(std::move(connection)),
      restart(usesIce(session.config().connect.transport)
                  ? std::make_unique<PathRestart>(session, static_cast<protocol::UdpConnection&>(*connection_), route)
                  : nullptr),
      hello_(std::move(hello)), peer_(peerOf(hello_.offer, connection_->peerKey())), data(true),
      out(*this, *connection_), cursor([this](protocol::Message message) { out.message(std::move(message)); }),
      audio(
          session, *connection_, audioHub, id, [this](protocol::Message message) { out.message(std::move(message)); },
          sounds),
      heard(session, {session.liveConfig()->receiveAudio, session.liveConfig()->audioTuning}, id),
      modulePeer_{session, viewerId}, dataSender_(
                                          session, *connection_, data, cancelled,
                                          [this] {
                                              cancelled = true;
                                              connection_->cancel();
                                          },
                                          id),
      messages_(session, *this, *connection_) {
    networkSeen = networkSeenNow(session); // As its connection's sockets were opened, near enough.
    connection_->setMigrate(session.liveConfig()->paths.migrate != 0); // Joined: kept when its path goes.
    stats_.network.connections = 1;
    stats_.viewerVideo.counts_for_bitrate = video.counts_for_bitrate;
    streams_ = hello_.settings.streams;
    paused_ = hello_.settings.paused;
}

Viewer::~Viewer() {
    cancelled = true;
    connection_->cancel();                        // Its threads end (members, in reverse order)...
    connection_->leaving(session_.leaveReason()); // ...then the connection says goodbye.
}

ViewerHello Viewer::hello() const {
    std::lock_guard lock(mutex_);
    return hello_;
}

void Viewer::setSettings(const protocol::ViewerSettings& settings) {
    {
        std::lock_guard lock(mutex_);
        hello_.settings = settings;
        streams_ = settings.streams;
        paused_ = settings.paused;
    }
    ++session_.viewerSet().version;
}

void Viewer::setOffer(const protocol::CodecOffer& offer) {
    {
        std::lock_guard lock(mutex_);
        hello_.offer = offer;
        peer_ = peerOf(offer, connection_->peerKey());
    }
    ++session_.viewerSet().version;
}

rcs_viewer_info Viewer::info(bool main) const {
    rcs_viewer_info info = RCS_VIEWER_INFO_INIT;
    info.viewer_id = id;
    info.joined = !leaving_;
    info.joined_ns = joinedNs;
    if (const auto address = connection_->peerAddress()) { // Where it is now: it may have moved.
        copyText(info.address, address->address());
        info.port = address->port();
    }
    info.route = route;
    info.is_main = main;
    std::lock_guard lock(mutex_);
    info.peer = peer_;
    return info;
}

void Viewer::fail(std::exception_ptr failure) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (cancelled || failure_) {
            return; // It is leaving already: what fails meanwhile is part of that.
        }
        failure_ = std::move(failure);
    }
    leaving_ = true;
}

std::exception_ptr Viewer::failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
}

void Viewer::leave(rcs_result result, const std::string& message) {
    fail(std::make_exception_ptr(PeerFailure(result, message)));
}

void Viewer::updateStats(const std::function<void(Stats&)>& update) {
    std::lock_guard lock(mutex_);
    update(stats_);
}

Stats Viewer::stats() const {
    std::lock_guard lock(mutex_);
    Stats copy = stats_;
    copy.data.queued_bytes = data.bytes();
    session_.modules().addQueued(copy.modules, data);
    return copy;
}

void Viewer::setAudio(rcs_audio_codec codec, uint32_t bitrate) {
    std::lock_guard lock(mutex_);
    audioCodec_ = codec;
    audioBitrate_ = bitrate;
    stats_.audioSend.codec = codec;
}

std::pair<rcs_audio_codec, uint32_t> Viewer::audioStream() const {
    std::lock_guard lock(mutex_);
    return {audioCodec_, audioBitrate_};
}

// ---- ViewerSet --------------------------------------------------------------------------

Viewer* ViewerSet::find(rcs_viewer_id id) const noexcept {
    const auto found = std::find_if(list.begin(), list.end(), [&](const auto& v) { return v->id == id; });
    return found == list.end() ? nullptr : found->get();
}

Viewer* ViewerSet::main() const noexcept {
    if (Viewer* chosen = chosenMain ? find(chosenMain) : nullptr) {
        return chosen;
    }
    return list.empty() ? nullptr : list.front().get();
}

Viewer& ViewerSet::present(rcs_viewer_id id) const {
    if (!id || id >= nextId) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "Viewer " + std::to_string(id) + " was never given in this run");
    }
    Viewer* viewer = find(id);
    if (!viewer || viewer->leaving()) {
        fail(RCS_ERROR_INVALID_STATE, "Viewer " + std::to_string(id) + " left");
    }
    return *viewer;
}

} // namespace rcsdk::session
