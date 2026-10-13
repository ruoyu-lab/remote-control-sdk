// Share role: the session's side of a sharer. A run serves its viewers the
// same streams (rcs_viewers_config; one, or one per display with
// rcs_streams_config): it plans each stream from this end's settings, the
// viewers' offers and the main viewer's request, tells the agent
// (agent/agent.hpp) to capture it once and encode it once for each of its
// encodings (RCS_EXT_ENCODINGS: the main one, and lower ones for the viewers
// that cannot take it), and hands each viewer the packets of the encoding it
// gets; it follows the paths (adaptive bitrate, shared among the streams;
// session/group_plan.hpp moves viewers between encodings and their temporal
// layers) and the viewers' requests, lets each viewer's input reach the
// machine through the agent while its control is granted, and tells each
// viewer that asks and holds control where text goes.
//
// ShareRun runs the loop: the acceptor (connect.hpp) takes connections, a
// joiner thread per connection decides whether its viewer can be served,
// viewers (share_viewer.hpp) join and leave, and the run ends when the last
// one leaves. ShareStream is the streams: when the settings or the viewers
// change it plans each encoding of each stream again and has the agent start
// a new capture and encoder where the plan, the capture or the encoder
// settings differ, starts the streams and encodings some viewer takes and
// stops the others; every viewer gets the announcements and keyframes of its
// encoding, the connections stay.
#include "protocol/connection.hpp"
#include "session/audio_stream.hpp"
#include "session/connect.hpp"
#include "session/forwarder.hpp"
#include "session/group_plan.hpp"
#include "session/negotiation.hpp"
#include "session/share_agent.hpp"
#include "session/share_video.hpp"
#include "session/share_viewer.hpp"
#include "seams/seams.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>

namespace rcsdk::session {
namespace {

using protocol::Connection;

// Whether `offer` takes `codec` in picture format `format`.
bool takes(const protocol::CodecOffer& offer, rcs_codec codec, rcs_picture_format format) {
    return formatsOffered(offer, codec) & (1u << format);
}

// Why two offers have no stream in common, as the sharer says it.
std::string noCommonStream(const protocol::CodecOffer& sharer, const protocol::CodecOffer& viewers, bool several) {
    if (negotiatedCodec(sharer.codecs, viewers.codecs)) {
        return formatMismatch(sharer, viewers);
    }
    return std::string(several ? "The viewers cannot decode " : "The viewer cannot decode ") +
           codecList(sharer.codecs) + (several ? "; they all offer " : "; it offers ") + codecList(viewers.codecs);
}

// Whether a viewer, or the path to it, caused `failure`: it leaves, the run
// goes on. Anything else is this end's and ends the run.
bool peerCaused(const std::exception_ptr& failure, FailureInfo& why) {
    try {
        std::rethrow_exception(failure);
    } catch (const Failure& f) {
        why = {f.result(), f.what()};
        const rcs_result r = f.result();
        return dynamic_cast<const PeerFailure*>(&f) || r == RCS_ERROR_NETWORK || r == RCS_ERROR_DISCONNECTED ||
               r == RCS_ERROR_RESTARTING || r == RCS_ERROR_PROTOCOL;
    } catch (const std::exception& e) {
        why = {RCS_ERROR_INTERNAL, e.what()};
    } catch (...) {
        why = {RCS_ERROR_INTERNAL, "Unexpected internal error"};
    }
    return false;
}

// How many encodings a stream may have with these settings (RCS_EXT_ENCODINGS):
// one with SINGLE, and for the host's own bitstream.
uint32_t encodingCount(const SessionConfig& config) noexcept {
    const auto& e = config.encodings;
    return e.mode == RCS_ENCODINGS_SINGLE || config.capture.source == RCS_CAPTURE_ENCODED ? 1 : e.count;
}

// This end's offer for a lower encoding: the codecs its setting lists, in that
// order, or all of them.
protocol::CodecOffer lowerOffer(const SessionConfig& config, const protocol::CodecOffer& offer, uint32_t encoding) {
    const auto& listed = config.encodings.lower[std::max(encoding, 1u) - 1].codecs;
    if (!listed[0]) {
        return offer;
    }
    protocol::CodecOffer lower = offer;
    lower.codecs.clear();
    lower.largest.clear();
    lower.formats.clear();
    for (const rcs_codec codec : listed) {
        const auto at = std::find(offer.codecs.begin(), offer.codecs.end(), codec);
        if (codec && at != offer.codecs.end()) {
            const size_t i = size_t(at - offer.codecs.begin());
            lower.codecs.push_back(codec);
            lower.largest.push_back(i < offer.largest.size() ? offer.largest[i] : PictureSize{});
            lower.formats.push_back(i < offer.formats.size() ? offer.formats[i] : 0);
        }
    }
    return lower;
}

// Whether a viewer offering `viewer` decodes every lower encoding: the codec
// and format of one that runs (ViewerSet::lowerStreams; its other viewers
// keep it), else some codec it may be in.
bool decodesLower(const SessionConfig& config, const protocol::CodecOffer& offer, const protocol::CodecOffer& viewer,
                  const ViewerSet& viewers) {
    for (uint32_t encoding = 1; encoding < encodingCount(config); ++encoding) {
        const auto [codec, format] = viewers.lowerStreams[encoding];
        if (codec ? !takes(viewer, codec, format) : !negotiatedCodec(lowerOffer(config, offer, encoding), viewer)) {
            return false;
        }
    }
    return encodingCount(config) > 1;
}

// A lower encoding's frame rate and the picture rate it may take (rcs_encoding_setting).
constexpr uint64_t defaultPixelRate = 1920ull * 1080 * 30;
constexpr uint32_t sizeSteps[] = {100, 75, 50, 38, 25}; // rcs_degradation's, of each side.
constexpr auto refusedFor = 30s;                        // AUTO tries an encoding the system refused again after this.

class ShareStream final : public VideoSink {
public:
    // `bitrate`: the main encodings' bitrate as it moves, for the acceptor's
    // threads; `forwarder`: the run's hop (FORWARDER), which the pictures go
    // to once for all viewers.
    ShareStream(Session& session, ShareAgent& agent, std::atomic<uint32_t>& bitrate,
                std::shared_ptr<ForwarderLink> forwarder)
        : session_(session), agent_(agent), viewers_(session.viewerSet()), currentBitrate_(bitrate),
          forwarder_(std::move(forwarder)) {
        agent_.attachVideo(this);
        agent_.port().setBacklogSource([this](uint32_t encoding) { return backlogNs(encoding); });
        live_ = *session_.liveConfig();
        offer_ = offerOf(live_, agent_.machine()); // As the first viewer got it.
        for (const auto& viewer : viewers_.list) {
            place(*viewer);
        }
        try {
            applySettings();
        } catch (...) {
            release(); // The destructor does not run for a stream that did not get built.
            throw;
        }
        // The first viewer learns what the machine shows now; changes follow.
        if (const auto now = session_.desktopInfo(); now.kind != RCS_DESKTOP_UNKNOWN) {
            forEach(
                [&](Viewer& viewer) { viewer.out.message(protocol::DesktopState{now.kind, now.user_logged_on != 0}); });
        }
    }

    ~ShareStream() override { release(); }

    // The loop's turn: failures first, then the settings, control, the paths and the viewers' encodings, keyframes.
    void step() {
        for (const auto& [viewer, event] : std::exchange(placed_, {})) {
            session_.viewerVideoChanged(viewer, event);
        }
        tick();
        forwarderNews();
        followControl();
        session_.deliverHostRelease();
        followPath();
        keyframes();
    }

    // A viewer joins (holding the set's exclusive lock, so that no packet
    // reaches it first): the encoding it starts with, what the streams it
    // takes are, what the machine shows, the pointer; its video starts at the
    // keyframes asked for it (through a forwarder: at what the forwarder kept,
    // or the keyframes it asks for).
    void joined(Viewer& viewer) {
        place(viewer);
        {
            std::lock_guard lock(mutex_);
            viewer.announcedStreams = 0;
            const uint32_t encoding = viewer.encoding;
            for (uint32_t index = 0; index < lanes_.size(); ++index) {
                auto& e = lanes_[index].encodings[encoding];
                if (e.announced && viewer.takes(index)) {
                    announceTo(viewer, index, e);
                    e.joiner = !forwarder_;
                }
            }
            viewer.cursor.update(pointerStream_, pointerFor(viewer));
        }
        if (const auto now = session_.desktopInfo(); now.kind != RCS_DESKTOP_UNKNOWN) {
            viewer.out.message(protocol::DesktopState{now.kind, now.user_logged_on != 0});
        }
        viewer.cursor.setActive(!cursorFollowsVideo_);
    }

    // A viewer left (holding the set's exclusive lock).
    void left(Viewer& viewer) { plan_.forget(viewer.id); }

private:
    // One encoding of a stream (RCS_EXT_ENCODINGS): its plan and encoder as
    // the agent was told, its share of its encoding's bitrate, its keyframes
    // and its announcement. Encoding 0 is the main one and owns the capture.
    struct Encoding {
        // The loop's.
        bool running = false;           // Started, and not stopped since.
        uint32_t share = 0;             // Its bitrate now...
        uint32_t sentBitrate = 0;       // ...what its encoder was last given...
        bool sentBudgeted = false;
        Clock::time_point lastKeyframe = Clock::now();
        rcs_viewer_id asking = 0; // A viewer asked for a keyframe that did not come yet...
        bool lossAsked = false;   // ...for a lost picture.
        bool joiner = false;      // A viewer joined it, or now takes the stream...
        bool switcher = false;    // ...or moved to it: a keyframe soon.
        std::optional<Clock::time_point> idleSince; // A lower one no viewer of its stream gets.
        // Degradation SIZE and BALANCED: when it last changed its size, its
        // share then (a step down), the 2 s being measured with the counters
        // as they began, and since when it held back next to nothing.
        Clock::time_point sizedAt{};
        uint32_t downShare = 0;
        Clock::time_point windowAt{};
        uint64_t windowLimitedMs = 0, windowBytes = 0;
        std::optional<Clock::time_point> calmSince;

        // Shared with its thread, under mutex_.
        StreamPlan plan;               // The encoding last started...
        agent::VideoStream stream;     // ...its settings...
        uint32_t startBitrate = 0;     // ...and the bitrate it started at.
        bool cursorInVideo = true;
        Session::CodecInfo encoderInfo; // The encoder of the encoding started.
        bool starting = false;          // A start waits for its videoStarted.
        bool settingsDirty = false;     // Settings to apply once it came.
        uint32_t refits = 0;
        // Encoding 0: what ran before the latest start, and a start its
        // encoder did not take (VideoPipeline::startOrKeep: the stream runs
        // as it was), not tried again while the settings ask for it.
        StreamPlan previousPlan, refusedPlan;
        agent::VideoStream previousStream, refusedStream;
        rcs_stream_info announcement = RCS_STREAM_INFO_INIT; // As its viewers last heard it.
        std::atomic<bool> announced{false};                  // Packets go out (set under mutex_).

        // Its thread's.
        std::atomic<rcs_viewer_id> lostBy{0};       // A viewer gave up on a picture: the packet that goes on says how.
        std::atomic<bool> refInvalidation{false};   // Its encoder drops pictures from its references.
        std::atomic<uint64_t> lastPacketNs{0};      // Stills leave their bitrate to the others.
        std::atomic<uint64_t> limitedMs{0};         // Pictures held back, as the pipeline says...
        std::atomic<uint64_t> encodedBytes{0};      // ...and what it encoded...
        std::array<std::atomic<uint64_t>, 3> layerBytes{}; // ...of each temporal layer.
        std::atomic<uint32_t> sizePercent{100};     // Each side's, of the plan's (rcs_degradation).
        std::atomic<uint32_t> layers{1};            // The temporal layers its encoder makes.

        // As before its first start (holding mutex_; its encoder stopped).
        void reset() {
            running = false;
            share = sentBitrate = 0;
            sentBudgeted = false;
            asking = 0;
            lossAsked = joiner = switcher = false;
            idleSince.reset();
            plan = {};
            stream = {};
            startBitrate = 0;
            cursorInVideo = true;
            encoderInfo = {};
            starting = settingsDirty = false;
            refits = 0;
            previousPlan = refusedPlan = {};
            previousStream = refusedStream = {};
            announcement = RCS_STREAM_INFO_INIT;
            announced = false;
            lostBy = 0;
            refInvalidation = false;
            lastPacketNs = 0;
            sizedAt = windowAt = {};
            downShare = 0;
            windowLimitedMs = windowBytes = 0;
            calmSince.reset();
            limitedMs = encodedBytes = 0;
            for (auto& bytes : layerBytes) {
                bytes = 0;
            }
            sizePercent = 100;
            layers = 1;
        }
    };

    // One stream (rcs_streams_config): what it shares and how it is
    // captured, and its encodings. Stream 0 shares RCS_EXT_CAPTURE's
    // target, the others whole displays.
    struct Lane {
        // The loop's.
        platform::CaptureTarget target; // What it shares, as started.
        rcs_capture_config capture{};   // Stream 0: the settings it started with.
        rcs_encoder_config encoder{};
        rcs_encoder_speed speed = 0;
        rcs_content_hint content = RCS_CONTENT_AUTO;
        uint32_t softwareThreads = 0;

        // Shared with its threads, under mutex_.
        platform::DisplayMode display; // As the capture reported it.
        bool displayKnown = false;
        rcs_display_info desktop{};    // Where its display sits on the desktop (a display's).
        CursorState pointer;           // As its capture last saw it.
        rcs_capture_pause paused = RCS_CAPTURE_PAUSE_NONE; // As its capture last said.
        Rect area;                     // Where the shared picture sits in the main encoding's frame.

        std::array<Encoding, RCS_MAX_ENCODINGS> encodings;
        Encoding& main() noexcept { return encodings[0]; }
        const Encoding& main() const noexcept { return encodings[0]; }
        bool running() const noexcept { return encodings[0].running; }
        // How many of its encodings its viewers hear of now.
        uint32_t announcedCount() const noexcept {
            return uint32_t(std::count_if(encodings.begin(), encodings.end(),
                                          [](const Encoding& e) { return e.announced.load(); }));
        }

        // As before its first start (holding mutex_; its pipeline stopped).
        void reset() {
            target = {};
            capture = {};
            encoder = {};
            speed = 0;
            displayKnown = false;
            display = {};
            desktop = {};
            pointer = {};
            paused = RCS_CAPTURE_PAUSE_NONE;
            area = {};
            for (auto& e : encodings) {
                e.reset();
            }
        }
    };

    void release() noexcept {
        session_.encodedVideo().stop(); // No packet of the host's is handed over after this.
        agent_.port().setBacklogSource({});
        for (uint32_t index = 0; index < lanes_.size(); ++index) {
            if (lanes_[index].running()) {
                agent_.port().stopVideo(index, 0);
            }
        }
        agent_.detachVideo(); // No handler runs after this.
    }

    // Each viewer, from the agent's threads (the loop's reads the list as it is).
    template <class Each>
    void forEach(Each&& each) {
        std::shared_lock lock(viewers_.mutex);
        for (const auto& viewer : viewers_.list) {
            each(*viewer);
        }
    }

    // The video of an encoding waiting for its fastest viewer, as time:
    // that encoding takes nothing new while even that one is behind; slower
    // viewers skip to keyframes instead, or move down. Through a forwarder:
    // what waits to go to it.
    uint64_t backlogNs(uint32_t encoding) const {
        if (forwarder_) {
            return forwarder_->backlogNs();
        }
        std::shared_lock lock(viewers_.mutex);
        std::optional<uint64_t> fastest;
        for (const auto& viewer : viewers_.list) {
            if (viewer->encoding == encoding) {
                fastest = std::min(fastest.value_or(~0ull), viewer->out.backlogNs());
            }
        }
        return fastest.value_or(0);
    }

    // ---- Viewers and their encodings ------------------------------------------------------

    // Whether a viewer decodes the main encodings and some codec of the
    // lower ones (held in it for the loop and the group's policy).
    void judgeDecoder(Viewer& viewer, const protocol::CodecOffer& offer) const {
        const rcs_codec watched = viewers_.streamCodec;
        viewer.decodesMain =
            !watched || (takes(offer, watched, viewers_.streamFormat) &&
                         sizeProblem(watched, viewers_.streamSize.width, viewers_.streamSize.height, {},
                                     largestOffered(offer, watched))
                             .empty());
        viewer.decodesLower = decodesLower(live_, offer_, offer, viewers_);
    }

    // Where a viewer starts (holding the set's exclusive lock): the main
    // encoding, unless the host or its decoder say otherwise.
    void place(Viewer& viewer) {
        judgeDecoder(viewer, viewer.hello().offer);
        const uint32_t count = encodingCount(live_);
        uint32_t encoding = 0;
        if (const uint32_t pinned = viewer.pinned; pinned != RCS_ENCODING_ANY && pinned < count) {
            encoding = pinned;
        }
        if (encoding == 0 && !viewer.decodesMain && viewer.decodesLower) {
            encoding = 1;
        }
        viewer.assigned = encoding;
        viewer.encoding = encoding;
        viewer.dropped = 0; // All its layers (the host's say applies at the next turn).
        if (encoding) { // Reported at the loop's next turn, holding no lock.
            placed_.push_back({viewer.id,
                               {encoding,
                                rcs_viewer_video_reason(viewer.decodesMain ? RCS_VIEWER_VIDEO_JOINED
                                                                           : RCS_VIEWER_VIDEO_DECODER),
                                0, encodingLayers(encoding)}});
        }
        viewer.updateStats([&](Stats& s) { s.viewerVideo.encoding = encoding; });
    }

    // The temporal layers an encoding has: the most any stream's encoder of it makes.
    uint32_t encodingLayers(uint32_t encoding) const {
        uint32_t layers = 1;
        for (const auto& lane : lanes_) {
            const auto& e = lane.encodings[encoding];
            layers = std::max(layers, e.running ? e.layers.load() : 1u);
        }
        return layers;
    }

    // How many of an encoding's temporal layers a viewer gets: all but those
    // it does not get from the top, one at least.
    static uint32_t layersFor(const Viewer& viewer, const Encoding& e) {
        const uint32_t layers = e.layers;
        return layers - std::min(layers - 1, viewer.dropped.load());
    }

    // Tells a viewer of stream `index` (holding the viewers' mutex and
    // mutex_): its encoding's announcement, with the layers it gets, which its
    // sender lets through from then on.
    void announceTo(Viewer& viewer, uint32_t index, const Encoding& e) {
        auto info = e.announcement;
        info.received_layers = layersFor(viewer, e);
        viewer.out.setLayers(index, info.received_layers);
        viewer.out.message(protocol::StreamAnnouncement{info});
        viewer.announcedStreams |= 1u << index;
        viewer.layersTold[index] = info.received_layers;
    }

    // Each viewer gets the layers of each stream the group's policy gave it
    // (it dropped some, or its encoder makes more or fewer); one whose layers
    // change hears so. Not while it switches encodings: the new one's come
    // with its announcement.
    void syncLayers() {
        std::shared_lock viewers(viewers_.mutex);
        std::lock_guard lock(mutex_);
        for (const auto& viewer : viewers_.list) {
            if (viewer->assigned != viewer->encoding) {
                continue;
            }
            for (uint32_t index = 0; index < lanes_.size(); ++index) {
                const auto& e = lanes_[index].encodings[viewer->encoding];
                if (e.announced && (viewer->announcedStreams & (1u << index)) &&
                    layersFor(*viewer, e) != viewer->layersTold[index]) {
                    announceTo(*viewer, index, e);
                }
            }
        }
    }

    // ---- Settings ------------------------------------------------------------------------

    // What stream `index` shares with these settings; none for a stream they do not have.
    std::optional<platform::CaptureTarget> targetOf(uint32_t index) const {
        if (!index) {
            return captureTarget(live_.capture);
        }
        const uint32_t display = live_.streams.displays[index - 1];
        if (!display) {
            return std::nullopt;
        }
        platform::CaptureTarget target;
        target.displayId = display;
        return target;
    }

    // The streams some viewer takes, as (1u << stream) bits; with `encoding`,
    // of the viewers that get or are to get that one.
    uint32_t takenStreams(std::optional<uint32_t> encoding = std::nullopt) const {
        uint32_t taken = 0;
        for (const auto& viewer : viewers_.list) {
            if (encoding && viewer->assigned != *encoding && viewer->encoding != *encoding) {
                continue;
            }
            for (uint32_t index = 0; index < lanes_.size(); ++index) {
                taken |= viewer->takes(index) ? 1u << index : 0;
            }
        }
        return taken;
    }

    // The lower encodings that may run now (RCS_EXT_ENCODINGS, those the
    // system did not refuse lately), as (1u << encoding) bits, 0 included.
    uint32_t availableEncodings(Clock::time_point now) const {
        uint32_t available = 1;
        for (uint32_t encoding = 1; encoding < encodingCount(live_); ++encoding) {
            available |= now >= refusedUntil_[encoding] ? 1u << encoding : 0;
        }
        return available;
    }

    // This end's latest settings and the viewers': plans each stream and
    // encoding and, where it or the capture or encoder settings changed, has
    // the agent start capture and encoder again; starts the streams and
    // encodings viewers take and stops the others.
    void applySettings() {
        seenSettings_ = session_.settingsVersion();
        seenViewers_ = viewers_.version;
        planDirty_ = false;
        const SessionConfig previous = live_;
        live_ = *session_.liveConfig();
        const bool first = !planned_;
        planned_ = true;
        const agent::Machine machine = agent_.machine();
        if (auto offer = offerOf(live_, machine); offer != offer_) {
            offer_ = std::move(offer); // The viewers learn this end's new codecs (rcs_peer_info).
            forEach([&](Viewer& viewer) { viewer.out.message(offer_); });
        }
        // The main encoding of each stream: a codec every viewer of it
        // decodes, the main viewer's request. A viewer that stopped offering
        // the codec the others watch gets a lower encoding, or where none can
        // run leaves, instead of changing it under them.
        protocol::ViewerSettings settings;
        protocol::CodecOffer common;
        std::array<std::vector<protocol::CodecOffer>, RCS_MAX_ENCODINGS> each;
        {
            std::shared_lock lock(viewers_.mutex);
            const Viewer* main = viewers_.main();
            const StreamPlan watched = runningPlan();
            for (const auto& viewer : viewers_.list) {
                const auto hello = viewer->hello();
                judgeDecoder(*viewer, hello.offer);
                if (viewers_.list.size() > 1 && watched.codec &&
                    !takes(hello.offer, watched.codec, watched.picture.format) && !viewer->decodesLower) {
                    viewer->leave(RCS_ERROR_BUSY, std::string("The viewer no longer decodes ") +
                                                      codecName(watched.codec) + " in " +
                                                      pictureFormatName(watched.picture.format) +
                                                      ", which the other viewers watch");
                    continue;
                }
                if (viewer->decodesMain) {
                    each[0].push_back(hello.offer);
                }
                for (uint32_t encoding = 1; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                    if (viewer->assigned == encoding || viewer->encoding == encoding) {
                        each[encoding].push_back(hello.offer);
                    }
                }
                if (viewer.get() == main) {
                    settings = hello.settings;
                }
            }
            if (each[0].empty()) { // None decodes the main encoding as it is: it changes for them all.
                for (const auto& viewer : viewers_.list) {
                    viewer->decodesMain = true;
                    each[0].push_back(viewer->hello().offer);
                }
            }
            common = commonOffer(each[0]);
        }
        {
            std::lock_guard lock(mutex_);
            inputs_ = {live_, offer_, common, settings.request, machine};
        }
        const bool cursorInVideo = settings.cursor == RCS_CURSOR_IN_VIDEO;
        const auto& c = live_.capture;
        const auto& e = live_.encoder;
        // Paused (rcs_session_pause, the background): no stream runs.
        const uint32_t paused = session_.paused();
        uint32_t taken = 0;
        std::array<uint32_t, RCS_MAX_ENCODINGS> takenBy{};
        const auto now = Clock::now();
        const uint32_t available = availableEncodings(now);
        if (!(paused & RCS_PAUSE_VIDEO)) {
            std::shared_lock lock(viewers_.mutex);
            taken = takenStreams();
            for (uint32_t encoding = 1; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                const bool fixed = live_.encodings.mode == RCS_ENCODINGS_FIXED && encoding < encodingCount(live_);
                takenBy[encoding] = !(available & (1u << encoding)) ? 0 : fixed ? taken : takenStreams(encoding);
            }
        }
        for (uint32_t index = 0; index < lanes_.size(); ++index) {
            auto& lane = lanes_[index];
            const auto target = targetOf(index);
            if (!target || !(taken & (1u << index))) {
                stop(index, 0);
                continue;
            }
            platform::DisplayMode display;
            {
                std::lock_guard lock(mutex_);
                display = lane.displayKnown && lane.target == *target ? lane.display : displayGuess(*target, machine);
            }
            const StreamPlan whole = planStream(live_, offer_, common, settings.request, display, &machine);
            const StreamPlan plan = scaled(whole, lane.main().sizePercent);
            if (!plan.codec) {
                throw PeerFailure(RCS_ERROR_UNSUPPORTED, noCommonStream(offer_, common, true));
            }
            if (!plan.refusal.empty()) {
                // This end's size, sent as it is, is more than the codec takes
                // between these ends: each viewer hears why, as one viewer would.
                forEach([&](Viewer& viewer) {
                    viewer.out.message(protocol::Refused{RCS_ERROR_UNSUPPORTED, plan.refusal});
                });
                throw PeerFailure(RCS_ERROR_UNSUPPORTED, plan.refusal);
            }
            const agent::VideoStream stream{plan.codec,
                                            plan.picture,
                                            plan.width,
                                            plan.height,
                                            plan.framesPerSecond,
                                            !index && c.source == RCS_CAPTURE_HOST,
                                            *target,
                                            c.exclude_current_process != 0,
                                            e.acceleration,
                                            e.backend,
                                            live_.videoTuning.speed,
                                            live_.videoTuning.content,
                                            live_.videoTuning.software_threads,
                                            c.secure_desktop != 0,
                                            live_.encodings.scalability};
            const bool captureChanged = !lane.running() || *target != lane.target ||
                                        (!index && (c.source != lane.capture.source ||
                                                    c.exclude_current_process != lane.capture.exclude_current_process ||
                                                    c.secure_desktop != lane.capture.secure_desktop));
            const bool encoderChanged = e.acceleration != lane.encoder.acceleration ||
                                        e.backend != lane.encoder.backend || stream.speed != lane.speed ||
                                        stream.content != lane.content ||
                                        stream.softwareThreads != lane.softwareThreads;
            std::optional<rcs_display_info> moved;
            {
                std::lock_guard lock(mutex_);
                lane.desktop = desktopOf(*target, machine);
                if (lane.main().announced && !sameDesktop(lane.main().announcement, lane.desktop)) {
                    moved = lane.desktop; // The display moved.
                }
            }
            if (moved) {
                for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                    std::optional<rcs_stream_info> info;
                    {
                        std::lock_guard lock(mutex_);
                        if (lane.encodings[encoding].announced) {
                            info = withDesktop(lane.encodings[encoding].announcement, *moved);
                        }
                    }
                    if (info) {
                        publish(index, encoding, *info);
                    }
                }
            }
            bool restarted = false;
            bool refused;
            {
                std::lock_guard lock(mutex_);
                refused = plan == lane.main().refusedPlan && stream == lane.main().refusedStream;
            }
            if (!refused && (captureChanged || encoderChanged || plan != lane.main().plan ||
                             stream.temporalLayers != lane.main().stream.temporalLayers)) {
                bool wait;
                {
                    std::lock_guard lock(mutex_);
                    wait = lane.main().starting;
                    lane.main().settingsDirty = wait; // Applied once the stream that is starting has.
                }
                if (!wait) {
                    restart(index, 0, plan, stream, cursorInVideo);
                    restarted = true;
                }
            }
            // Its lower encodings: those the viewers of this stream are on or
            // going to, and that the system did not refuse lately.
            for (uint32_t encoding = 1; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                auto& lower = lane.encodings[encoding];
                const bool wanted = c.source != RCS_CAPTURE_ENCODED && (takenBy[encoding] & (1u << index));
                const bool idle = live_.encodings.mode == RCS_ENCODINGS_AUTO && lower.idleSince &&
                                  now - *lower.idleSince >= std::chrono::milliseconds(live_.videoTuning.lower_idle_ms);
                if (!wanted || idle) {
                    stop(index, encoding);
                    continue;
                }
                // FIXED runs it before any viewer gets it: for the main encoding's viewers.
                const auto viewers = each[encoding].empty() ? common : commonOffer(each[encoding]);
                auto lowerPlan = planLower(whole, encoding, viewers, lower.sizePercent);
                if (!lowerPlan.codec) {
                    stop(index, encoding);
                    continue;
                }
                agent::VideoStream lowerStream = stream;
                lowerStream.codec = lowerPlan.codec;
                lowerStream.picture = lowerPlan.picture;
                lowerStream.width = lowerPlan.width;
                lowerStream.height = lowerPlan.height;
                lowerStream.framesPerSecond = lowerPlan.framesPerSecond;
                lowerStream.temporalLayers = live_.encodings.lower[encoding - 1].scalability;
                const bool changed =
                    lowerPlan != lower.plan || lowerStream.temporalLayers != lower.stream.temporalLayers;
                bool wait;
                {
                    std::lock_guard lock(mutex_);
                    wait = lower.starting || lane.main().starting;
                    lower.settingsDirty = wait && (restarted || changed || !lower.running);
                }
                if (!wait && (restarted || !lower.running || encoderChanged || changed)) {
                    restart(index, encoding, lowerPlan, lowerStream, cursorInVideo);
                }
            }
        }
        if (cursorInVideo != cursorInVideo_) {
            cursorInVideo_ = cursorInVideo;
            agent_.port().setCursorInVideo(cursorInVideo); // A stream started meanwhile has it already.
        }
        const bool captureMoved = first || captureTarget(c) != captureTarget(previous.capture) ||
                                  streamDisplays(live_) != streamDisplays(previous);
        if (captureMoved || cursorInVideo != cursorFollowsVideo_) {
            forEach([&](Viewer& viewer) { viewer.cursor.setActive(!cursorInVideo); });
        }
        if (cursorInVideo != cursorFollowsVideo_) {
            announceCursor(cursorInVideo);
        }
        cursorFollowsVideo_ = cursorInVideo;
        if (captureMoved || live_.control.target != previous.control.target ||
            c.secure_desktop != previous.capture.secure_desktop) {
            useInjector();
        }
        agent_.port().setInputBoost(live_.videoTuning.input_boost_ms);
        reannounce();
        for (const auto& viewer : viewers_.list) { // Each in the format it negotiated, and what this end paused.
            if (viewer->pausedTold != paused) {
                viewer->pausedTold = paused;
                viewer->out.message(protocol::SharerPaused{paused});
            }
            const auto hello = viewer->hello();
            const bool takesSound = hello.offer.receivesAudio && !(hello.settings.paused & RCS_PAUSE_AUDIO) &&
                                    !(paused & RCS_PAUSE_AUDIO);
            viewer->audio.apply(live_, takesSound, hello.offer.receiveAudio, &hello.settings.request);
            viewer->heard.apply({live_.receiveAudio, live_.audioTuning});
        }
        {
            std::unique_lock lock(viewers_.mutex); // What a viewer moved to a lower encoding must decode.
            for (uint32_t encoding = 1; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                viewers_.lowerStreams[encoding] = {};
                for (const auto& lane : lanes_) {
                    if (const auto& e = lane.encodings[encoding]; e.running) {
                        viewers_.lowerStreams[encoding] = {e.plan.codec, e.plan.picture.format};
                    }
                }
            }
        }
        // A new start bitrate moves the adaptive target there; otherwise it keeps adapting.
        applyRate(!first && live_.videoTuning.start_bitrate_bps != previous.videoTuning.start_bitrate_bps);
    }

    // A lower encoding of a stream whose main encoding is `main` (before its
    // own degradation), for viewers offering `viewers`: a codec they all
    // decode (this end's or its setting's, the main one's picture format where
    // they take it), the frame rate of its setting, the main encoding's size
    // made smaller until the picture rate fits (and the decoders take it),
    // then `percent` of that (its own degradation). Codec 0: none can be made.
    StreamPlan planLower(const StreamPlan& main, uint32_t encoding, const protocol::CodecOffer& viewers,
                         uint32_t percent) const {
        const auto& setting = live_.encodings.lower[encoding - 1];
        const auto sharer = lowerOffer(live_, offer_, encoding);
        StreamPlan plan = main;
        plan.refusal.clear();
        plan.codec = negotiatedCodec(sharer, viewers);
        if (!plan.codec) {
            return plan;
        }
        const uint32_t formats = formatsOffered(sharer, plan.codec) & formatsOffered(viewers, plan.codec);
        if (plan.codec != main.codec || !(formats & (1u << main.picture.format))) {
            const auto& listed = live_.encoder.formats;
            const auto format = std::find_if(std::begin(listed), std::end(listed),
                                             [&](uint32_t f) { return f && (formats & (1u << f)); });
            plan.picture = picture(format != std::end(listed) ? *format : rcs_picture_format(RCS_PICTURE_420_8),
                                   live_.encoder.matrix, live_.encoder.range);
        }
        plan.framesPerSecond = setting.frames_per_second
                                   ? std::min(setting.frames_per_second, main.framesPerSecond)
                                   : std::max(1u, main.framesPerSecond >> encoding);
        const uint64_t most = setting.max_pixel_rate ? setting.max_pixel_rate : defaultPixelRate;
        std::optional<StreamPlan> fitted;
        for (const uint32_t step : sizeSteps) {
            const StreamPlan sized = scaled(main, step);
            const bool fits = uint64_t(sized.width) * sized.height * plan.framesPerSecond <= most || step == 25;
            if (fits && sizeProblem(plan.codec, sized.width, sized.height, largestOffered(sharer, plan.codec),
                                    largestOffered(viewers, plan.codec))
                            .empty()) {
                fitted = sized;
                break;
            }
        }
        if (!fitted) {
            plan.codec = 0;
            return plan;
        }
        plan.width = fitted->width;
        plan.height = fitted->height;
        return scaled(plan, percent);
    }

    // What a stream shares before the capture says: a display as the system
    // lists it, a region its size on it. A window's size is known once it is captured.
    static platform::DisplayMode displayGuess(const platform::CaptureTarget& target, const agent::Machine& machine) {
        const bool display = target.source == RCS_CAPTURE_DISPLAY || target.source == RCS_CAPTURE_APPLICATION;
        if ((!display && target.source != RCS_CAPTURE_REGION) || seams::captureOverridden()) {
            return {};
        }
        for (const auto& d : machine.displays) {
            if (target.displayId ? d.display_id == target.displayId : d.is_main) {
                return display ? platform::DisplayMode{d.pixel_width, d.pixel_height, d.refresh_hz}
                               : platform::DisplayMode{target.region.width, target.region.height, d.refresh_hz};
            }
        }
        return {};
    }

    // The display a stream shares as the machine lists it (desktop_* tell
    // where it sits); all 0 for what is not a whole display.
    static rcs_display_info desktopOf(const platform::CaptureTarget& target, const agent::Machine& machine) {
        if (target.source == RCS_CAPTURE_DISPLAY) {
            for (const auto& d : machine.displays) {
                if (target.displayId ? d.display_id == target.displayId : d.is_main) {
                    return d;
                }
            }
        }
        return {};
    }
    static bool sameDesktop(const rcs_stream_info& info, const rcs_display_info& d) noexcept {
        return info.desktop_x == d.desktop_x && info.desktop_y == d.desktop_y &&
               info.desktop_width == d.desktop_width && info.desktop_height == d.desktop_height;
    }
    static rcs_stream_info withDesktop(rcs_stream_info info, const rcs_display_info& d) noexcept {
        info.desktop_x = d.desktop_x;
        info.desktop_y = d.desktop_y;
        info.desktop_width = d.desktop_width;
        info.desktop_height = d.desktop_height;
        return info;
    }

    // Encoding `encoding` of stream `index` starts (again) with `plan`; the
    // main one with its capture, which ends the stream's lower ones (they
    // start again at the next turn, from the new capture).
    void restart(uint32_t index, uint32_t encoding, const StreamPlan& plan, const agent::VideoStream& stream,
                 bool cursorInVideo) {
        auto& lane = lanes_[index];
        auto& e = lane.encodings[encoding];
        if (plan.encoderLimited && (plan.encoderLimited != e.plan.encoderLimited || plan.codec != e.plan.codec ||
                                    plan.picture.format != e.plan.picture.format || plan.width != e.plan.width)) {
            log::info("session", "Stream " + std::to_string(index) + ": the encoder takes " + codecName(plan.codec) +
                                     " " + pictureFormatName(plan.picture.format) + " at " +
                                     std::to_string(plan.width) + "x" + std::to_string(plan.height) + " up to " +
                                     std::to_string(plan.framesPerSecond) + " FPS (" +
                                     std::to_string(plan.encoderLimited) + " asked)");
        }
        const auto limits = videoLimits(plan);
        const uint32_t share = e.share ? e.share : startShare(index, encoding, plan, limits);
        {
            std::lock_guard lock(mutex_);
            if (!encoding) {
                e.previousPlan = e.plan;
                e.previousStream = e.stream;
                e.refusedPlan = {};
                e.refusedStream = {};
            }
            e.plan = plan;
            e.stream = stream;
            e.startBitrate = share;
            e.cursorInVideo = cursorInVideo;
            e.starting = true;
            e.announced = false; // A viewer joining meanwhile hears of the new encoding, not the old one.
            e.settingsDirty = false;
            if (!encoding) {
                lane.displayKnown = lane.displayKnown && lane.target == stream.target;
                for (uint32_t lower = 1; lower < RCS_MAX_ENCODINGS; ++lower) {
                    auto& l = lane.encodings[lower];
                    l.settingsDirty = l.running; // The capture they took pictures from ends, and they with it.
                    l.running = l.announced = l.starting = false;
                }
            }
        }
        if (encoding) {
            std::unique_lock lock(viewers_.mutex);
            viewers_.lowerStreams[encoding] = {plan.codec, plan.picture.format}; // Viewers that join it must decode it.
        } else {
            std::unique_lock lock(viewers_.mutex);
            viewers_.streamCodec = plan.codec;           // Viewers that join must decode it...
            viewers_.streamFormat = plan.picture.format; // ...in this format...
            viewers_.streamSize = largestSize(index, plan); // ...at these sizes.
            lane.target = stream.target;
            lane.capture = live_.capture;
            lane.encoder = live_.encoder;
            lane.speed = live_.videoTuning.speed;
            lane.content = live_.videoTuning.content;
            lane.softwareThreads = live_.videoTuning.software_threads;
        }
        e.running = true;
        e.idleSince.reset();
        e.share = share;
        e.sentBitrate = share;
        e.sentBudgeted = limits.adaptive;
        session_.updateStats([&](Stats& s) {
            auto& send = s.video[index].sendOf(encoding);
            if (encoding) {
                send = sizedStruct<rcs_video_send_stats>(); // A lower encoding's start afresh.
            }
            send.bitrate_bps = share;
            send.size_percent = e.sizePercent;
            s.video[index].encodings |= 1u << encoding;
        });
        e.lastKeyframe = Clock::now();
        e.lastPacketNs = monotonicNs(); // A new encoding counts as moving.
        if (live_.capture.source != RCS_CAPTURE_ENCODED) {
            if (!index && !encoding) {
                session_.encodedVideo().stop();
            }
            agent_.port().startVideo(
                {index, encoding, stream, share, limits.adaptive, cursorInVideo, live_.videoTuning.input_boost_ms});
            return;
        }
        // The host's own bitstream (stream 0 only): no capture, no encoder; the
        // stream starts here and the host is told to begin with a keyframe.
        agent_.port().stopVideo(0, 0);
        session_.encodedVideo().start(
            stream, [this](EncodedPacket&& packet) { videoPacket(std::move(packet)); }, [this] { return backlogNs(0); });
        session_.encodedVideo().setRate(share, limits.adaptive);
        // The host's encoder as its settings say; HARDWARE_FIRST chose nothing the viewers could hear of.
        const rcs_acceleration acceleration = stream.acceleration == RCS_ACCELERATION_HARDWARE_FIRST
                                                  ? rcs_acceleration(RCS_ACCELERATION_HARDWARE)
                                                  : stream.acceleration;
        videoStarted({0, 0, stream, Rect{0, 0, stream.width, stream.height}, {}, acceleration, RCS_BACKEND_HOST, false,
                      {}, {}, false, stream.temporalLayers});
        if (e.announced) {
            session_.keyframeDue(RCS_KEYFRAME_START);
        }
    }

    // Encoding `encoding` of stream `index` stops; the main one stops the
    // stream, and its viewers hear so. A lower one is reported
    // (RCS_EVENT_ENCODING) unless its failure was.
    void stop(uint32_t index, uint32_t encoding, bool report = true) {
        auto& lane = lanes_[index];
        auto& e = lane.encodings[encoding];
        if (!e.running) {
            return;
        }
        if (index || encoding || live_.capture.source != RCS_CAPTURE_ENCODED) {
            agent_.port().stopVideo(index, encoding);
        } else {
            session_.encodedVideo().stop();
        }
        if (encoding) {
            const uint32_t count = lane.announcedCount();
            rcs_encoding_event ended{encoding, 0, e.plan.codec, 0, 0, 0, 0, 0, RCS_OK, {}};
            {
                std::lock_guard lock(mutex_);
                e.reset();
            }
            if (std::none_of(lanes_.begin(), lanes_.end(),
                             [&](const Lane& l) { return l.encodings[encoding].running; })) {
                std::unique_lock lock(viewers_.mutex);
                viewers_.lowerStreams[encoding] = {};
            }
            session_.updateStats([&](Stats& s) {
                s.video[index].encodings &= ~(1u << encoding);
                s.video[index].sendOf(encoding) = sizedStruct<rcs_video_send_stats>();
            });
            if (count > 1) {
                reannounceCount(index); // The others hear how many run now.
            }
            if (report) {
                session_.encodingChanged(index, ended);
            }
            log::info("session", "Stream " + std::to_string(index) + ": encoding " + std::to_string(encoding) +
                                     " stopped");
            return;
        }
        {
            std::shared_lock viewers(viewers_.mutex);
            std::lock_guard lock(mutex_);
            for (const auto& viewer : viewers_.list) {
                if (viewer->announcedStreams & (1u << index)) {
                    viewer->announcedStreams &= ~(1u << index);
                    viewer->out.message(protocol::StreamEnded{uint8_t(index)});
                }
            }
            lane.reset();
        }
        session_.updateStats([&](Stats& s) { s.video[index].encodings = 0; });
        session_.videoStreamEnded(index);
        log::info("session", "Stream " + std::to_string(index) + " stopped");
    }
    // The largest picture of the streams, with stream `index` at `plan`'s: what a viewer that joins must take.
    PictureSize largestSize(uint32_t index, const StreamPlan& plan) const {
        PictureSize largest{plan.width, plan.height};
        for (uint32_t other = 0; other < lanes_.size(); ++other) {
            if (other != index && lanes_[other].running()) {
                largest.width = std::max(largest.width, lanes_[other].main().plan.width);
                largest.height = std::max(largest.height, lanes_[other].main().plan.height);
            }
        }
        return largest;
    }

    // ---- The bitrate ---------------------------------------------------------------------

    // Stream `index`'s weight with `config` (rcs_streams_config.weights; all 0: its pixels).
    static uint64_t weightOf(const SessionConfig& config, uint32_t index, const StreamPlan& plan) {
        const auto& w = config.streams.weights;
        const bool weighted = std::any_of(std::begin(w), std::end(w), [](uint32_t value) { return value != 0; });
        return weighted ? w[index] : uint64_t(plan.width) * plan.height;
    }

    // An encoding's shares of `total` among the streams that run it,
    // together `total`: each a fiftieth of it first, so that a stream whose
    // weight is 0 can still show a picture, and the rest by weight, a still
    // stream (nothing new for a second) counting a quarter of its weight.
    void share(uint32_t encoding, uint32_t total) {
        const uint64_t now = monotonicNs();
        std::array<uint64_t, RCS_MAX_STREAMS> weights{};
        uint64_t sum = 0;
        uint32_t running = 0;
        for (uint32_t index = 0; index < lanes_.size(); ++index) {
            const auto& e = lanes_[index].encodings[encoding];
            if (!e.running) {
                continue;
            }
            const bool still = now - e.lastPacketNs.load() > 1'000'000'000;
            weights[index] = std::max<uint64_t>(1, weightOf(live_, index, e.plan) / (still ? 4 : 1));
            sum += weights[index];
            ++running;
        }
        const uint32_t floor = total / 50;
        const uint64_t rest = total - uint64_t(floor) * running;
        for (uint32_t index = 0; index < lanes_.size(); ++index) {
            auto& e = lanes_[index].encodings[encoding];
            if (e.running) {
                e.share = floor + uint32_t(rest * weights[index] / std::max<uint64_t>(sum, 1));
            }
        }
    }

    // The share an encoding starts with, before the next turn shares the bitrate again.
    uint32_t startShare(uint32_t index, uint32_t encoding, const StreamPlan& plan, const RateLimits& limits) const {
        const uint32_t bitrate = bitrates_[encoding] ? bitrates_[encoding] : bitrates_[0];
        const uint32_t total = limits.adaptive ? std::clamp(bitrate ? bitrate : limits.start, limits.minimum,
                                                            limits.maximum)
                                               : plan.bitrate;
        uint64_t sum = weightOf(live_, index, plan);
        for (uint32_t other = 0; other < lanes_.size(); ++other) {
            if (other != index && lanes_[other].encodings[encoding].running) {
                sum += weightOf(live_, other, lanes_[other].encodings[encoding].plan);
            }
        }
        return uint32_t(uint64_t(total) * std::max<uint64_t>(weightOf(live_, index, plan), 1) /
                        std::max<uint64_t>(sum, 1));
    }

    // Each encoding's encoder target, where it is not what it has.
    void sendRates(bool budgeted) {
        for (uint32_t index = 0; index < lanes_.size(); ++index) {
            for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                auto& e = lanes_[index].encodings[encoding];
                if (!e.running || (e.share == e.sentBitrate && budgeted == e.sentBudgeted)) {
                    continue;
                }
                e.sentBitrate = e.share;
                e.sentBudgeted = budgeted;
                if (!index && !encoding && live_.capture.source == RCS_CAPTURE_ENCODED) {
                    session_.encodedVideo().setRate(e.share, budgeted);
                } else {
                    agent_.port().setRate(index, encoding, e.share, budgeted);
                }
                session_.updateStats([&](Stats& s) { s.video[index].sendOf(encoding).bitrate_bps = e.share; });
            }
        }
    }

    // The video bitrate for a plan, leaving room for the sound actually sent
    // (the most any viewer's takes).
    RateLimits videoLimits(const StreamPlan& plan) const {
        auto value = rateLimits(live_, plan.bitrate);
        uint32_t sound = 0;
        for (const auto& viewer : viewers_.list) {
            sound = std::max(sound, viewer->audio.wireBitrate());
        }
        value.maximum = std::max(value.minimum, value.maximum - std::min(value.maximum, sound));
        value.start = std::clamp(value.start, value.minimum, value.maximum);
        return value;
    }

    // Hands the limits to each viewer's connection when they changed (or a
    // viewer joined), or when the adaptive target is to move to the start
    // bitrate; shares each encoding's bitrate among the streams.
    void applyRate(bool moveToStart) {
        const StreamPlan plan = runningPlan();
        if (!plan.codec) {
            return; // No stream runs.
        }
        const auto limits = videoLimits(plan);
        if (limits != limits_ || moveToStart || viewers_.list.size() != rated_) {
            limits_ = limits;
            rated_ = viewers_.list.size();
            for (const auto& viewer : viewers_.list) {
                viewer->connection().setVideoRate(
                    {limits.adaptive, limits.minimum, limits.maximum, moveToStart ? limits.start : 0, limits.rules,
                     limits.fec});
            }
            for (auto& bitrate : bitrates_) {
                bitrate = !limits.adaptive ? limits.maximum
                          : moveToStart     ? limits.start
                                            : bitrate ? std::clamp(bitrate, limits.minimum, limits.maximum) : 0;
            }
            if (!bitrates_[0]) {
                bitrates_[0] = limits.start;
            }
            currentBitrate_ = bitrates_[0];
        }
        for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
            share(encoding, bitrates_[encoding] ? bitrates_[encoding] : bitrates_[0]);
        }
        sendRates(limits.adaptive);
    }

    // The main plan of some running stream (the bitrate settings are the same for all).
    StreamPlan runningPlan() const {
        for (const auto& lane : lanes_) {
            if (lane.running()) {
                return lane.main().plan;
            }
        }
        return {};
    }

    // Remote input goes to the displays now shared, or to the host; input of
    // the old executor is released and discarded.
    void useInjector() {
        std::unique_ptr<platform::InputInjector> injector;
        if (live_.control.target == RCS_INPUT_HOST) {
            injector = std::make_unique<HostInput>(session_.shareControl().hostRelease);
        } else {
            injector = std::make_unique<AgentInjector>(agent_.port(), captureTarget(live_.capture),
                                                       streamDisplays(live_), live_.capture.secure_desktop != 0);
        }
        auto& control = session_.shareControl();
        std::lock_guard lock(control.mutex);
        control.release(0);
        control.injector = std::move(injector);
        control.hostExecutes = live_.control.target == RCS_INPUT_HOST;
        for (const auto& viewer : viewers_.list) {
            ++viewer->epoch;
        }
    }

    // ---- The loop ------------------------------------------------------------------------

    // The paths, the viewers' encodings and the adaptive bitrate: a few
    // times a second, not per frame.
    void followPath() {
        const auto now = Clock::now();
        if (now < nextPathCheck_) {
            return;
        }
        nextPathCheck_ = now + 100ms;
        applyRate(false);
        std::vector<GroupViewer> group;
        std::vector<Connection::PathStatus> paths;
        const Viewer* main;
        {
            std::shared_lock lock(viewers_.mutex);
            main = viewers_.main();
        }
        uint64_t bytes = forwarder_ ? forwarder_->bytes() : 0;
        for (const auto& viewer : viewers_.list) {
            auto status = viewer->connection().status();
            const auto [drops, dropped] = viewer->out.drops(); // Its own, and the connection's.
            status.backlogDrops += drops;
            status.backlogDroppedBytes += dropped;
            // Through a forwarder its pictures' path is the forwarder's to it, as reported.
            const auto report = forwarder_ ? forwarder_->path(viewer->peer) : std::nullopt;
            if (report) {
                status.targetBitrate = report->targetBps;
                status.lossPermille = report->lossPermille;
                status.queueDelayUs = report->queueUs;
                status.backlogDrops = report->drops;
            }
            if (const auto said = session_.publishPath(status, viewer->id)) {
                viewer->out.message(*said);
            }
            const auto stats = viewer->stats();
            bytes += forwarder_ ? 0 : stats.network.bytes;
            while (!viewer->caused.empty() && now - viewer->caused.front() > 60s) {
                viewer->caused.pop_front();
            }
            GroupViewer v;
            v.id = viewer->id;
            v.encoding = viewer->assigned;
            const uint32_t layers = encodingLayers(v.encoding);
            v.layers = layers - std::min(layers - 1, viewer->dropped.load());
            v.pinned = viewer->pinned;
            v.pinnedLayers = viewer->pinnedLayers;
            v.counts = viewer->counts;
            v.main = viewer.get() == main;
            v.target = forwarder_ ? status.targetBitrate : stats.network.rate_reports ? status.targetBitrate : 0;
            v.behind = (report ? uint64_t(report->backlogUs) * 1000 : viewer->out.backlogNs()) > 500'000'000;
            v.dropped = status.backlogDrops > viewer->dropsSeen;
            viewer->dropsSeen = status.backlogDrops;
            v.lossKeyframes = uint32_t(viewer->caused.size());
            v.decodesMain = viewer->decodesMain;
            v.decodesLower = viewer->decodesLower;
            group.push_back(v);
            paths.push_back(std::move(status));
            viewer->updateStats([&](Stats& s) {
                s.viewerVideo.target_bps = v.target;
                s.viewerVideo.counts_for_bitrate = v.counts;
                s.viewerVideo.layers = v.layers;
                const auto sent = viewer->out.sent();
                s.viewerVideo.frames_sent = sent.frames;
                s.viewerVideo.frames_skipped = sent.skipped;
                s.viewerVideo.upper_layer_frames = sent.upper;
            });
        }
        auto turn = plan_.turn(group, groupEncodings(now), groupSettings(now), now);
        for (const auto& move : turn.moves) {
            moveViewer(move, group);
        }
        const bool uplinkHolds = forwarder_ && limits_.adaptive && fitUplink(turn.bitrate);
        // Each encoding's bitrate, where adaptive bitrate decides; the main
        // one's moves are reported (RCS_EVENT_RATE) with the viewer whose path holds it.
        if (limits_.adaptive) {
            bool changed = false;
            for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                const uint32_t bitrate = turn.bitrate[encoding];
                if (!bitrate || bitrate == bitrates_[encoding]) {
                    continue;
                }
                bitrates_[encoding] = bitrate;
                share(encoding, bitrate);
                changed = true;
                if (!encoding && uplinkHolds) { // The path up to the forwarder holds it.
                    currentBitrate_ = bitrate;
                    auto hop = forwarder_->hop().status();
                    hop.targetBitrate = bitrate;
                    session_.rateChanged(hop, 0);
                } else if (!encoding) {
                    currentBitrate_ = bitrate;
                    for (size_t i = 0; i < group.size(); ++i) {
                        if (group[i].id == turn.limiting[0]) {
                            session_.rateChanged(paths[i], group[i].id);
                        }
                    }
                }
            }
            if (changed) {
                sendRates(true);
            }
        }
        // The encodings to run: started (and stopped) by the settings' next turn.
        for (uint32_t encoding = 1; encoding < RCS_MAX_ENCODINGS; ++encoding) {
            const bool wanted = turn.wanted & (1u << encoding);
            for (uint32_t index = 0; index < lanes_.size(); ++index) {
                auto& e = lanes_[index].encodings[encoding];
                if (wanted && !e.running && lanes_[index].running() && !(session_.paused() & RCS_PAUSE_VIDEO)) {
                    planDirty_ = planDirty_ || takenStreams(encoding) & (1u << index);
                } else if (e.running && !(takenStreams(encoding) & (1u << index)) &&
                           live_.encodings.mode == RCS_ENCODINGS_AUTO) {
                    e.idleSince = e.idleSince.value_or(now);
                    planDirty_ = planDirty_ ||
                                 now - *e.idleSince >= std::chrono::milliseconds(live_.videoTuning.lower_idle_ms);
                } else {
                    e.idleSince.reset();
                }
            }
        }
        switchViewers();
        syncLayers();
        groupStats(now, bytes, turn.uplinkLimited);
        for (uint32_t index = 0; index < lanes_.size(); ++index) {
            for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                adaptSize(index, encoding, now);
            }
        }
    }

    // FORWARDER: the encodings that run, all their streams together, within
    // what the path up to the forwarder carries (its hop's adaptive bitrate,
    // less the sound that goes up too): each cut by the same share, none
    // below the minimum. True when it cut them. The turn's statistics.
    bool fitUplink(std::array<uint32_t, RCS_MAX_ENCODINGS>& bitrate) {
        const uint32_t uplink = forwarder_->uplinkBps();
        const auto hop = forwarder_->hop().status();
        session_.updateStats([&](Stats& s) {
            s.forwarder.uplink_bps = uplink;
            s.forwarder.hop_round_trip_ns = hop.roundTripNs;
            s.forwarder.hop_bytes = hop.bytes;
        });
        if (!uplink) {
            return false;
        }
        uint32_t sound = 0;
        for (const auto& viewer : viewers_.list) {
            sound = std::max(sound, viewer->audio.wireBitrate());
        }
        const uint64_t budget = uplink - std::min(uplink, sound);
        uint64_t total = 0;
        std::array<uint32_t, RCS_MAX_ENCODINGS> running{};
        for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
            const bool runs = std::any_of(lanes_.begin(), lanes_.end(),
                                          [&](const Lane& lane) { return lane.encodings[encoding].running; });
            running[encoding] = runs ? (bitrate[encoding] ? bitrate[encoding] : bitrates_[encoding]) : 0;
            total += running[encoding];
        }
        if (total <= budget) {
            return false;
        }
        for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
            if (running[encoding]) {
                bitrate[encoding] = std::max(limits_.minimum, uint32_t(uint64_t(running[encoding]) * budget / total));
            }
        }
        session_.updateStats([](Stats& s) { ++s.forwarder.uplink_limited; });
        return true;
    }

    // Each encoding's bitrate, its temporal layers and what it sent over the
    // last 2 s, by layer, all streams together.
    std::array<GroupEncoding, RCS_MAX_ENCODINGS> groupEncodings(Clock::time_point now) {
        std::array<GroupEncoding, RCS_MAX_ENCODINGS> encodings{};
        LayerBytes bytes{};
        for (const auto& lane : lanes_) {
            for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                for (size_t layer = 0; layer < 3; ++layer) {
                    bytes[encoding][layer] += lane.encodings[encoding].layerBytes[layer];
                }
            }
        }
        sent_.push_back({now, bytes});
        while (sent_.size() > 2 && now - sent_[1].first >= 2s) {
            sent_.pop_front();
        }
        const auto& [then, before] = sent_.front();
        const double seconds = std::max(0.1, std::chrono::duration<double>(now - then).count());
        for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
            auto& e = encodings[encoding];
            e.bitrate = bitrates_[encoding];
            e.layers = encodingLayers(encoding);
            uint64_t sent = 0;
            for (size_t layer = 0; layer < 3; ++layer) {
                const uint64_t total = bytes[encoding][layer], earlier = before[encoding][layer];
                sent += total - std::min(total, earlier);
                e.layerBps[layer] = uint64_t(double(sent) * 8 / seconds);
            }
            e.sentBps = e.layerBps[2];
        }
        return encodings;
    }

    GroupSettings groupSettings(Clock::time_point now) const {
        const auto& t = live_.videoTuning;
        GroupSettings s;
        s.from = live_.viewers.bitrate_from;
        s.quorumPercent = live_.viewers.quorum_percent;
        s.count = encodingCount(live_);
        s.available = availableEncodings(now);
        s.fixed = live_.encodings.mode == RCS_ENCODINGS_FIXED;
        s.moveDownMs = t.move_down_ms;
        s.moveUpMs = t.move_up_ms;
        s.lossKeyframesMove = t.loss_keyframes_move;
        s.minimum = limits_.minimum;
        s.maximum = limits_.maximum;
        return s;
    }

    // The group's policy moved a viewer: it gets the new encoding once that
    // one runs (switchViewers), fewer or more of its layers from now on
    // (syncLayers), and the host hears why.
    void moveViewer(const GroupMove& move, std::span<const GroupViewer> group) {
        Viewer* viewer = viewers_.find(move.viewer);
        if (!viewer) {
            return;
        }
        viewer->dropped = encodingLayers(move.to) - std::min(encodingLayers(move.to), move.toLayers);
        uint32_t target = 0;
        for (const auto& v : group) {
            target = v.id == move.viewer ? v.target : target;
        }
        session_.viewerVideoChanged(viewer->id, {move.to, move.reason, target, move.toLayers});
        if (move.to == move.from) { // Its layers alone.
            const bool fewer = move.toLayers < move.fromLayers;
            viewer->updateStats([&](Stats& s) {
                s.viewerVideo.layer_moves_down += fewer;
                s.viewerVideo.layer_moves_up += !fewer;
            });
            session_.updateStats([&](Stats& s) {
                s.group.layer_moves_down += fewer;
                s.group.layer_moves_up += !fewer;
            });
            return;
        }
        viewer->assigned = move.to;
        viewer->caused.clear(); // Its losses on the encoding it had are no matter on the next.
        const bool down = move.to > move.from;
        viewer->updateStats([&](Stats& s) {
            s.viewerVideo.moves_down += down;
            s.viewerVideo.moves_up += !down;
        });
        session_.updateStats([&](Stats& s) {
            s.group.moves_down += down;
            s.group.moves_up += !down;
        });
        planDirty_ = true; // The encoding it goes to may need starting, the one it leaves no longer be used.
    }

    // Viewers whose encoding is to change get it once it runs on every
    // stream they take: the new announcements, and its keyframe.
    void switchViewers() {
        std::unique_lock viewers(viewers_.mutex, std::defer_lock);
        for (const auto& viewer : viewers_.list) {
            const uint32_t to = viewer->assigned;
            if (to == viewer->encoding) {
                continue;
            }
            bool ready = true;
            for (uint32_t index = 0; index < lanes_.size(); ++index) {
                ready = ready && (!viewer->takes(index) || !lanes_[index].running() ||
                                  lanes_[index].encodings[to].announced);
            }
            if (!ready) {
                continue;
            }
            if (!viewers.owns_lock()) {
                viewers.lock(); // No packet of either encoding reaches it meanwhile.
            }
            std::lock_guard lock(mutex_);
            viewer->encoding = to;
            viewer->out.switchVideo();
            viewer->waits.fill(false);
            for (uint32_t index = 0; index < lanes_.size(); ++index) {
                auto& e = lanes_[index].encodings[to];
                if (viewer->takes(index) && e.announced) {
                    announceTo(*viewer, index, e);
                    // Through a forwarder it starts the viewer on what it kept, or asks for the keyframe
                    // (NeedKeyframe), maybe already: the switch asks for none, and cancels none asked.
                    e.switcher = e.switcher || !forwarder_;
                }
            }
            viewer->updateStats([&](Stats& s) { s.viewerVideo.encoding = to; });
        }
    }

    // The run's group statistics (RCS_STATS_GROUP), from this turn.
    void groupStats(Clock::time_point now, uint64_t bytes, bool uplinkLimited) {
        std::array<uint32_t, RCS_MAX_ENCODINGS> on{};
        for (const auto& viewer : viewers_.list) {
            ++on[std::min(viewer->encoding.load(), RCS_MAX_ENCODINGS - 1)];
        }
        if (now - sendWindow_.first >= 1s) {
            sendBps_ = sendWindow_.second && bytes >= sendWindow_.second
                           ? uint64_t(double(bytes - sendWindow_.second) * 8 /
                                      std::chrono::duration<double>(now - sendWindow_.first).count())
                           : sendBps_;
            sendWindow_ = {now, bytes};
        }
        const uint32_t encodings = uint32_t(std::count_if(lanes_[0].encodings.begin(), lanes_[0].encodings.end(),
                                                          [](const Encoding& e) { return e.running; }));
        session_.updateStats([&](Stats& s) {
            auto& g = s.group;
            g.viewers = uint32_t(viewers_.list.size());
            std::copy(on.begin(), on.end(), g.viewers_on);
            g.encodings = encodings;
            g.main_bitrate_bps = bitrates_[0];
            g.send_bps = sendBps_;
            g.uplink_limited += uplinkLimited;
        });
    }

    // ---- What gives way (rcs_degradation) ------------------------------------------------

    // Each side of a plan at `percent` of its own (even, at least 16).
    static StreamPlan scaled(StreamPlan plan, uint32_t percent) {
        if (percent < 100) {
            plan.width = std::max(16u, (plan.width * percent / 100) & ~1u);
            plan.height = std::max(16u, (plan.height * percent / 100) & ~1u);
        }
        return plan;
    }

    // SIZE and BALANCED: an encoding held back much of the time takes a step
    // smaller, and a step back up once its bitrate grew, or once it is held
    // back next to nothing while its encoder uses little of the bitrate;
    // FRAME_RATE keeps the plan's size. Each encoding on its own.
    void adaptSize(uint32_t index, uint32_t encoding, Clock::time_point now) {
        static constexpr auto window = 2s, calm = 10s;
        auto& e = lanes_[index].encodings[encoding];
        const auto& t = live_.videoTuning;
        const uint32_t percent = e.sizePercent;
        if (!e.running) {
            return;
        }
        if (t.degradation == RCS_DEGRADE_FRAME_RATE) {
            if (percent != 100) {
                resize(index, encoding, 100, now, "the frame rate gives way again");
            }
            return;
        }
        const uint64_t limitedMs = e.limitedMs, bytes = e.encodedBytes;
        // Measured from 2 s after a step: not the new stream's keyframe.
        if (e.windowAt == Clock::time_point{} || now - e.sizedAt < window) {
            e.windowAt = now;
            e.windowLimitedMs = limitedMs;
            e.windowBytes = bytes;
            return;
        }
        const auto elapsed = now - e.windowAt;
        if (elapsed < window) {
            return;
        }
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const double held = double(limitedMs - std::min(limitedMs, e.windowLimitedMs)) / 1000 / seconds;
        const double encodedBps = double(bytes - std::min(bytes, e.windowBytes)) * 8 / seconds;
        e.windowAt = now;
        e.windowLimitedMs = limitedMs;
        e.windowBytes = bytes;
        if (held >= 0.05) {
            e.calmSince.reset();
        } else if (!e.calmSince) {
            e.calmSince = now - elapsed;
        }
        const auto at = std::find(std::begin(sizeSteps), std::end(sizeSteps), percent);
        const double down = t.degradation == RCS_DEGRADE_SIZE ? 0.3 : 0.5;
        if (held > down && at + 1 != std::end(sizeSteps) && at[1] >= t.min_size_percent) {
            e.downShare = e.share;
            resize(index, encoding, at[1], now,
                   "pictures were held back " + std::to_string(int(held * 100)) + "% of the time at " +
                       std::to_string(e.share / 1000) + " kbit/s");
        } else if (percent < 100 && now - e.sizedAt >= calm &&
                   (e.share >= e.downShare * 1.8 ||
                    (e.calmSince && now - *e.calmSince >= calm && encodedBps < 0.4 * e.share))) {
            // Content that is always heavy is held back at any size: the
            // bitrate grown alone brings the step back. Held back again, it
            // steps down at the new share, which the bitrate must then outgrow.
            resize(index, encoding, at[-1], now,
                   "the bitrate carries it again (" + std::to_string(e.share / 1000) + " kbit/s, " +
                       std::to_string(int64_t(encodedBps / 1000)) + " used)");
        }
    }

    // The encoding starts again at `percent` of its plan's size.
    void resize(uint32_t index, uint32_t encoding, uint32_t percent, Clock::time_point now, const std::string& why) {
        auto& e = lanes_[index].encodings[encoding];
        log::info("session", "Stream " + std::to_string(index) +
                                 (encoding ? ", encoding " + std::to_string(encoding) : std::string()) + " at " +
                                 std::to_string(percent) + "% of its size: " + why);
        e.sizePercent = percent;
        e.sizedAt = now;
        e.calmSince.reset();
        session_.updateStats([&](Stats& s) { s.video[index].sendOf(encoding).size_percent = percent; });
        std::lock_guard lock(mutex_);
        e.settingsDirty = true; // Planned again at the next turn.
    }

    // Whether each viewer may control now, as the agent says the system
    // takes input; each viewer hears its own grant when it changes.
    void followControl() {
        auto& control = session_.shareControl();
        std::vector<std::pair<Viewer*, rcs_control_info>> changed;
        {
            std::lock_guard lock(control.mutex);
            // Input the host executes needs nothing from the system.
            const auto said = control.systemExecutes ? agent_.inputAvailable() : std::optional<bool>(true);
            const bool available = said.value_or(false);
            if (available != control.available) {
                control.available = available;
                for (const auto& viewer : viewers_.list) {
                    ++viewer->epoch; // Input produced before the change is discarded.
                }
                if (!available) {
                    control.release(0);
                }
            }
            for (const auto& viewer : viewers_.list) {
                if (viewer->control && !said) {
                    continue; // Until the agent says, there is no grant to announce (and no refusal either).
                }
                const bool granted = control.granted(viewer->control);
                const std::pair announcement{granted, viewer->epoch};
                if (viewer->announced != announcement) {
                    const bool news = !viewer->announced || viewer->announced->first != granted;
                    viewer->announced = announcement;
                    viewer->out.message(protocol::ControlGrant{granted, viewer->epoch});
                    if (news) { // A new grant epoch alone is no news for the host.
                        changed.push_back(
                            {viewer.get(), rcs_control_info{sizeof(rcs_control_info), viewer->control, granted}});
                    }
                }
            }
        }
        for (const auto& [viewer, info] : changed) {
            session_.viewerControlChanged(viewer->id, info);
        }
    }

    // Keyframes for each encoding of each stream: the host asked (every
    // one), the interval passed, a viewer joined it, moved to it or now
    // takes the stream, or its viewers asked (lost a picture, fell behind).
    // While others watch the encoding these wait keyframe_min_interval_ms
    // since its last; with several viewers, a keyframe one viewer's loss or
    // backlog causes comes at most every viewer_keyframe_interval_ms (its
    // requests meanwhile wait for the next there is); each keyframe serves
    // every viewer of its encoding that waits.
    void keyframes() {
        const bool host = session_.takeKeyframeRequest();
        const auto now = Clock::now();
        const auto& t = live_.videoTuning;
        using Grid = std::array<std::array<rcs_viewer_id, RCS_MAX_ENCODINGS>, RCS_MAX_STREAMS>;
        std::array<std::array<std::optional<uint64_t>, RCS_MAX_ENCODINGS>, RCS_MAX_STREAMS> missing;
        Grid lostBy{}, asked{};
        std::array<std::array<uint32_t, RCS_MAX_ENCODINGS>, RCS_MAX_STREAMS> watching{};
        for (const auto& viewer : viewers_.list) {
            const uint32_t encoding = viewer->encoding;
            for (uint32_t index = 0; index < lanes_.size(); ++index) {
                watching[index][encoding] += viewer->takes(index);
            }
        }
        std::array<std::array<bool, RCS_MAX_ENCODINGS>, RCS_MAX_STREAMS> lossAsked{};
        for (const auto& viewer : viewers_.list) {
            const uint32_t encoding = viewer->encoding;
            const uint32_t wanted = viewer->takeKeyframeRequests() | viewer->out.takeKeyframeWanted();
            for (uint32_t index = 0; index < lanes_.size(); ++index) {
                // Missing pictures: where the encoder can, it drops them from
                // its references at once (from the earliest its viewers miss),
                // whether they may still come or not; otherwise the one given
                // up on is a keyframe asked for.
                const auto& e = lanes_[index].encodings[encoding];
                const bool invalidates = t.ref_invalidation && e.refInvalidation &&
                                         !(!index && !encoding && live_.capture.source == RCS_CAPTURE_ENCODED);
                bool gaveUp = false;
                if (const auto lost = viewer->takeLost(index)) {
                    if (invalidates) {
                        auto& earliest = missing[index][encoding];
                        earliest = std::min(lost->after, earliest.value_or(lost->after));
                    }
                    auto& by = lostBy[index][encoding];
                    by = lost->gaveUp && !by ? viewer->id : by;
                    gaveUp = lost->gaveUp && !invalidates;
                }
                const bool wants = (wanted & (1u << index)) || viewer->waits[index] || gaveUp;
                if (!wants) {
                    continue;
                }
                // With others in the session its own limit, alone on its
                // encoding too: its keyframes take the uplink they share, and
                // one its path lost again is not got over by asking for more.
                const bool limited =
                    viewers_.list.size() > 1 &&
                    now - viewer->lastCaused[index] < std::chrono::milliseconds(t.viewer_keyframe_interval_ms);
                if (limited) {
                    if (!viewer->waits[index]) {
                        viewer->waits[index] = true;
                        viewer->updateStats([](Stats& s) { ++s.viewerVideo.keyframes_waited; });
                    }
                } else if (!asked[index][encoding]) {
                    asked[index][encoding] = viewer->id;
                    lossAsked[index][encoding] = gaveUp;
                }
            }
        }
        for (uint32_t index = 0; index < lanes_.size(); ++index) {
            auto& lane = lanes_[index];
            for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                auto& e = lane.encodings[encoding];
                if (!e.running) {
                    continue;
                }
                const bool encoded = !index && !encoding && live_.capture.source == RCS_CAPTURE_ENCODED;
                const bool merged = watching[index][encoding] > 1;
                if (lostBy[index][encoding] && !e.lostBy) {
                    e.lostBy = lostBy[index][encoding]; // Reported with the packet that goes on from it.
                }
                if (missing[index][encoding]) {
                    agent_.port().invalidateAfter(index, encoding, *missing[index][encoding]);
                }
                if (asked[index][encoding] && !e.asking) {
                    e.asking = asked[index][encoding];
                    e.lossAsked = lossAsked[index][encoding];
                }
                const bool due = t.keyframe_interval_ms &&
                                 now - e.lastKeyframe >= std::chrono::milliseconds(t.keyframe_interval_ms);
                const bool spaced =
                    !merged || now - e.lastKeyframe >= std::chrono::milliseconds(t.keyframe_min_interval_ms);
                const bool joining = (e.joiner || e.switcher) && spaced;
                const bool viewer = e.asking && spaced;
                if (!viewer && !host && !due && !joining) {
                    continue;
                }
                const rcs_viewer_id who = viewer ? e.asking : 0;
                if (encoded) {
                    session_.keyframeDue(who || host ? RCS_KEYFRAME_REQUESTED : RCS_KEYFRAME_INTERVAL, who);
                } else {
                    agent_.port().requestKeyframe(index, encoding);
                }
                e.lastKeyframe = now;
                if ((who && !e.lossAsked) || host) {
                    session_.recovering(index, RCS_RECOVERY_REQUESTED, who);
                }
                countKeyframe(index, encoding, e.joiner, e.switcher, who, merged, now);
                e.asking = 0; // This keyframe serves whoever asked.
                e.lossAsked = false;
                e.joiner = e.switcher = false;
            }
        }
    }

    // A keyframe of an encoding of stream `index` is on its way: why, in the
    // group's statistics; who caused it, and the requests it serves.
    void countKeyframe(uint32_t index, uint32_t encoding, bool joined, bool switched, rcs_viewer_id who, bool merged,
                       Clock::time_point now) {
        session_.updateStats([&](Stats& s) {
            auto& g = s.group;
            g.keyframes_joined += joined;
            g.keyframes_switched += !joined && switched;
            g.keyframes_lost += !joined && !switched && who;
            g.keyframes_host += !joined && !switched && !who;
        });
        for (const auto& viewer : viewers_.list) {
            if (viewer->encoding != encoding) {
                continue;
            }
            viewer->waits[index] = false;
            if (viewer->id == who && !joined && !switched) {
                viewer->lastCaused[index] = now;
                if (merged) { // What it cost the others.
                    viewer->caused.push_back(now);
                    viewer->updateStats([](Stats& s) { ++s.viewerVideo.keyframes_caused; });
                }
            }
        }
    }

    // FORWARDER: what the forwarder did: viewers it moved down on the spot
    // (each gets the new encoding as a move of the group's policy does), and
    // keyframes viewers it moved wait for.
    void forwarderNews() {
        if (!forwarder_) {
            return;
        }
        auto news = forwarder_->takeNews();
        for (const auto& moved : news.moved) {
            for (const auto& viewer : viewers_.list) {
                if (viewer->peer != moved.peer || viewer->assigned == moved.encoding) {
                    continue;
                }
                const uint32_t target = forwarder_->path(moved.peer).value_or(protocol::PathReport::Peer{}).targetBps;
                viewer->assigned = moved.encoding;
                viewer->caused.clear();
                viewer->updateStats([](Stats& s) { ++s.viewerVideo.moves_down; });
                session_.updateStats([](Stats& s) { ++s.group.moves_down; });
                // All its layers: the forwarder sends them all.
                session_.viewerVideoChanged(viewer->id, {moved.encoding, RCS_VIEWER_VIDEO_PATH, target,
                                                         encodingLayers(moved.encoding)});
                planDirty_ = true;
            }
        }
        for (const auto& keyframe : news.keyframes) {
            if (keyframe.stream < lanes_.size() && lanes_[keyframe.stream].encodings[keyframe.encoding].running) {
                lanes_[keyframe.stream].encodings[keyframe.encoding].switcher = true;
            }
        }
    }

    void tick() {
        agent_.rethrowFailure();
        bool dirty = false;
        std::deque<std::pair<std::pair<uint32_t, uint32_t>, FailureInfo>> failed;
        {
            std::lock_guard lock(mutex_);
            if (sendFailure_) {
                std::rethrow_exception(sendFailure_);
            }
            for (const auto& lane : lanes_) {
                for (const auto& e : lane.encodings) { // A lower one waits for the main one it takes pictures from.
                    dirty = dirty || (e.settingsDirty && !e.starting && !lane.main().starting);
                }
            }
            failed.swap(encodingFailures_);
        }
        for (const auto& [where, why] : failed) {
            if (where.second) {
                dropEncoding(where.first, where.second, why);
            } else {
                refuseChange(where.first, why);
            }
        }
        const bool machine = machineChanged_.exchange(false);
        if (dirty || machine || planDirty_ || session_.settingsVersion() != seenSettings_ ||
            viewers_.version != seenViewers_) {
            applySettings();
        }
    }

    // A change of stream `index` its encoder did not take: it runs as it
    // was (encodingFailed put its plan back), viewers that join get that,
    // and the host hears what was asked and why (RCS_EVENT_ENCODING of
    // encoding 0, running).
    void refuseChange(uint32_t index, const FailureInfo& why) {
        StreamPlan refused, kept;
        {
            std::lock_guard lock(mutex_);
            refused = lanes_[index].main().refusedPlan;
            kept = lanes_[index].main().plan;
        }
        {
            std::unique_lock lock(viewers_.mutex);
            viewers_.streamCodec = kept.codec;
            viewers_.streamFormat = kept.picture.format;
            viewers_.streamSize = largestSize(index, kept);
        }
        rcs_encoding_event event{0, 1, refused.codec, refused.width, refused.height, refused.framesPerSecond,
                                 refused.bitrate, 1, why.result, {}};
        copyText(event.message, why.message);
        session_.encodingChanged(index, event);
    }

    // A lower encoding could not start or failed: FIXED ends the run (the
    // host asked for it); AUTO reports it, stops it on every stream, moves
    // its viewers up and tries it again later.
    void dropEncoding(uint32_t index, uint32_t encoding, const FailureInfo& why) {
        const std::string what = "Stream " + std::to_string(index) + ", encoding " + std::to_string(encoding) + ": ";
        if (live_.encodings.mode == RCS_ENCODINGS_FIXED) {
            fail(why.result, what + why.message);
        }
        log::warning("session", what + why.message + " (tried again in " + std::to_string(refusedFor.count()) + " s)");
        refusedUntil_[encoding] = Clock::now() + refusedFor;
        rcs_encoding_event event{encoding, 0, lanes_[index].encodings[encoding].plan.codec, 0, 0, 0, 0, 0, why.result,
                                 {}};
        copyText(event.message, why.message);
        session_.updateStats([](Stats& s) { ++s.group.encodings_refused; });
        session_.encodingChanged(index, event);
        for (uint32_t lane = 0; lane < lanes_.size(); ++lane) {
            stop(lane, encoding, false); // Its viewers move up at the next turn.
        }
    }

    // ---- What goes to the viewers ---------------------------------------------------------

    // An encoding of stream `index` as it is announced: the main one's
    // recorded as the run's (RCS_EVENT_STREAM, no lock held), and sent to
    // each viewer of it that takes the stream before any of its packets.
    void publish(uint32_t index, uint32_t encoding, rcs_stream_info info) {
        uint32_t count;
        {
            std::lock_guard lock(mutex_);
            lanes_[index].encodings[encoding].announced = true;
            count = lanes_[index].announcedCount();
        }
        info.stream = index;
        info.encoding = encoding;
        info.encodings = count;
        if (!encoding) {
            session_.setVideoStream(info);
        }
        std::shared_lock viewers(viewers_.mutex);
        std::lock_guard lock(mutex_);
        announce(index, encoding, info);
    }

    // The announcement to the viewers of the encoding (holding the viewers' mutex, then mutex_).
    void announce(uint32_t index, uint32_t encoding, const rcs_stream_info& info) {
        auto& e = lanes_[index].encodings[encoding];
        e.announcement = info;
        for (const auto& viewer : viewers_.list) {
            if (viewer->takes(index) && viewer->encoding == encoding) {
                announceTo(*viewer, index, e);
            }
        }
    }

    // How many encodings a stream runs changed: each encoding's viewers hear
    // it (but those of `except`, which was just announced).
    void reannounceCount(uint32_t index, uint32_t except = RCS_MAX_ENCODINGS) {
        for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
            if (encoding == except) {
                continue;
            }
            std::optional<rcs_stream_info> info;
            {
                std::lock_guard lock(mutex_);
                if (lanes_[index].encodings[encoding].announced) {
                    info = lanes_[index].encodings[encoding].announcement;
                }
            }
            if (info) {
                publish(index, encoding, *info);
            }
        }
    }

    // The viewers that now take a stream they did not (or no longer take
    // one) hear of it, and its keyframe follows (holding no lock).
    void reannounce() {
        std::shared_lock viewers(viewers_.mutex);
        std::lock_guard lock(mutex_);
        for (const auto& viewer : viewers_.list) {
            for (uint32_t index = 0; index < lanes_.size(); ++index) {
                auto& lane = lanes_[index];
                auto& e = lane.encodings[viewer->encoding];
                const uint32_t bit = 1u << index;
                // A stream that restarts is not announced until it started: no end.
                const bool takes = lane.running() && viewer->takes(index);
                if (takes && e.announced && !(viewer->announcedStreams & bit)) {
                    announceTo(*viewer, index, e);
                    e.joiner = !forwarder_;
                } else if (!takes && (viewer->announcedStreams & bit)) {
                    viewer->out.message(protocol::StreamEnded{uint8_t(index)});
                    viewer->announcedStreams &= ~bit;
                }
            }
        }
    }

    // The pointer moved into the pictures or out of them: one picture
    // serves every viewer of an encoding, so each learns what the streams do.
    void announceCursor(bool inVideo) {
        for (uint32_t index = 0; index < lanes_.size(); ++index) {
            for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                std::optional<rcs_stream_info> info;
                {
                    std::lock_guard lock(mutex_);
                    auto& e = lanes_[index].encodings[encoding];
                    if (e.announced) { // An encoding that starts announces it.
                        info = e.announcement;
                        info->cursor = inVideo ? RCS_CURSOR_IN_VIDEO : RCS_CURSOR_SEPARATE;
                    }
                }
                if (info) {
                    publish(index, encoding, *info);
                }
            }
        }
    }

    // The pointer as `viewer` gets it (holding mutex_): hidden while it is
    // on a stream the viewer does not take.
    CursorState pointerFor(const Viewer& viewer) const {
        CursorState state = lanes_[pointerStream_].pointer;
        if (!viewer.takes(pointerStream_)) {
            state.visible = false;
        }
        return state;
    }

    // ---- The agent's events (its pipelines' threads) ----------------------------------------

    // What a handler throws is the loop's to rethrow: it ends the run.
    template <class Handler>
    void guard(Handler&& handler) noexcept {
        try {
            handler();
        } catch (...) {
            std::lock_guard lock(mutex_);
            if (!sendFailure_) {
                sendFailure_ = std::current_exception();
            }
        }
    }

    // Where the shared picture sits in an encoding's frame, from where it
    // sits in the main encoding's (holding mutex_).
    Rect contentOf(const Lane& lane, const Encoding& e) const {
        const auto& main = lane.main().plan;
        const auto& c = lane.area;
        if (!main.width || !main.height || !c.width) {
            return {0, 0, e.plan.width, e.plan.height};
        }
        const auto x = [&](uint32_t v) { return uint32_t(uint64_t(v) * e.plan.width / main.width); };
        const auto y = [&](uint32_t v) { return uint32_t(uint64_t(v) * e.plan.height / main.height); };
        const Rect content{x(c.x), y(c.y), std::max(1u, x(c.width)), std::max(1u, y(c.height))};
        return {content.x, content.y, std::min(content.width, e.plan.width - std::min(content.x, e.plan.width)),
                std::min(content.height, e.plan.height - std::min(content.y, e.plan.height))};
    }

    void videoStarted(const agent::VideoStarted& event) override {
        guard([&] {
            auto& lane = lanes_.at(event.index);
            auto& e = lane.encodings.at(event.encoding);
            StreamPlan plan;
            uint32_t announced;
            bool cursorInVideo;
            rcs_display_info desktop;
            Rect content;
            {
                std::lock_guard lock(mutex_);
                if (!e.plan.codec || (event.encoding && !lane.main().announced)) {
                    return; // It stopped while it started, or its capture did: nothing to announce or start again.
                }
                e.starting = false;
                if (!event.encoding) {
                    lane.display = event.display;
                    lane.displayKnown = true;
                    lane.area = event.content;
                    // The display as the capture sees it may change the plan (a
                    // request beyond the display's own size or refresh rate): the
                    // stream starts again for the new plan, which is announced instead.
                    const auto fitted = scaled(planStream(inputs_.live, inputs_.offer, inputs_.viewerOffer,
                                                          inputs_.request, event.display, &inputs_.machine),
                                               e.sizePercent);
                    if (fitted != e.plan && fitted != e.refusedPlan && e.refits < 1) {
                        ++e.refits;
                        e.settingsDirty = true;
                        e.announced = false;
                        return;
                    }
                    e.refits = 0;
                }
                plan = e.plan;
                cursorInVideo = e.cursorInVideo;
                desktop = lane.desktop;
                announced = announcedBitrate(event.index, event.encoding, plan);
                content = event.encoding ? contentOf(lane, e) : event.content;
            }
            // The encoder's acceleration and layers as it opened (HARDWARE_FIRST decided).
            e.layers = event.temporalLayers;
            auto info = withDesktop(streamInfo(plan, event.hdr, event.acceleration, content, event.display,
                                               cursorInVideo, event.temporalLayers),
                                    desktop);
            info.bitrate_bps = announced;
            publish(event.index, event.encoding, info);
            if (event.encoding) {
                reannounceCount(event.index, event.encoding); // The others hear one more runs.
            }
            e.refInvalidation = event.refInvalidation;
            const Session::CodecInfo encoder{event.index,      plan.codec,            event.acceleration,
                                             event.backend,    event.lowLatency,      event.refInvalidation,
                                             event.encoder,    event.temporalLayers};
            {
                std::lock_guard lock(mutex_);
                e.encoderInfo = encoder;
            }
            if (event.encoding) {
                session_.updateStats([&](Stats& s) {
                    auto& send = s.video[event.index].sendOf(event.encoding);
                    send.low_latency = event.lowLatency;
                    send.temporal_layers = event.temporalLayers;
                    copyText(send.encoder, event.encoder);
                });
                rcs_encoding_event started{event.encoding,       1,       plan.codec,           plan.width, plan.height,
                                           plan.framesPerSecond, e.share, event.temporalLayers, RCS_OK,     {}};
                session_.encodingChanged(event.index, started);
            } else if (!encoder.name.empty()) { // The host's own bitstream has no encoder of the SDK's.
                session_.codecChanged(encoder);
            }
            log::info("session", "Stream " + std::to_string(event.index) +
                                     (event.encoding ? ", encoding " + std::to_string(event.encoding) : std::string()) +
                                     ": " + codecName(plan.codec) + " " + pictureText(plan.picture) + " " +
                                     std::to_string(plan.width) + "x" + std::to_string(plan.height) + " at " +
                                     std::to_string(plan.framesPerSecond) + " FPS, " +
                                     std::to_string(info.bitrate_bps / 1000) + " kbit/s");
        });
    }

    // An encoding's share of the plan's bitrate by weight alone (holding mutex_): what it announces.
    uint32_t announcedBitrate(uint32_t index, uint32_t encoding, const StreamPlan& plan) const {
        uint64_t sum = 0;
        for (uint32_t other = 0; other < lanes_.size(); ++other) {
            if (lanes_[other].encodings[encoding].plan.codec) {
                sum += weightOf(inputs_.live, other, lanes_[other].encodings[encoding].plan);
            }
        }
        return sum ? uint32_t(uint64_t(plan.bitrate) * weightOf(inputs_.live, index, plan) / sum) : plan.bitrate;
    }

    // Each packet once: the host sees it (the encodings it asked for) and it
    // is counted, then every viewer of its encoding that takes its stream
    // gets the same message.
    void videoPacket(EncodedPacket&& packet) override {
        guard([&] {
            const uint32_t index = packet.stream, encoding = packet.encoding;
            auto& e = lanes_.at(index).encodings.at(encoding);
            if (!e.announced) {
                return;
            }
            e.lastPacketNs = monotonicNs();
            e.encodedBytes += packet.data.size();
            e.layerBytes[std::min<uint32_t>(packet.temporalId, 2)] += packet.data.size();
            if (const auto& output = session_.config().encodedOutput;
                output.on_video && (output.encodings & (1u << encoding))) {
                const auto view = publicPacket(packet);
                session_.callHostWith(output.user_data, output.on_video, &view);
            }
            const auto message = std::make_shared<const VideoMessage>(
                VideoMessage{index, protocol::encode(packet), packet.keyframe, packet.temporalId});
            if (forwarder_) { // Once for all: the forwarder gives each viewer its copy, of every layer.
                forwarder_->publish(packet, message->bytes);
            } else {
                forEach([&](Viewer& viewer) {
                    if (viewer.takes(index) && viewer.encoding == encoding) {
                        viewer.out.video(message);
                    }
                });
            }
            const uint64_t now = monotonicNs();
            session_.updateStats([&](Stats& s) {
                auto& send = s.video[index].sendOf(encoding);
                ++send.encoded_frames;
                send.keyframes += packet.keyframe;
                send.invalidations += packet.recoversAfter.has_value();
                send.encoded_bytes += packet.data.size();
                send.base_layer_bytes += packet.temporalId ? 0 : packet.data.size();
                send.last_encode_ns = packet.encodeNs;
                send.last_pipeline_ns = now - packet.timestampNs;
            });
            if (packet.keyframe || packet.recoversAfter) {
                if (const rcs_viewer_id lost = e.lostBy.exchange(0)) {
                    session_.recovering(index, RCS_RECOVERY_LOSS, lost,
                                        packet.keyframe ? RCS_RECOVERY_KEYFRAME : RCS_RECOVERY_REFERENCES);
                }
            }
        });
    }

    void videoContent(uint32_t index, const Rect& content, const platform::DisplayMode& display) override {
        guard([&] {
            auto& lane = lanes_.at(index);
            std::array<std::optional<rcs_stream_info>, RCS_MAX_ENCODINGS> infos;
            {
                std::lock_guard lock(mutex_);
                lane.display = display;
                lane.displayKnown = true;
                lane.area = content;
                for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                    auto& e = lane.encodings[encoding];
                    if (!e.announced) {
                        continue;
                    }
                    auto info = withDesktop(streamInfo(e.plan, e.announcement.hdr, e.announcement.encoder_acceleration,
                                                       encoding ? contentOf(lane, e) : content, display,
                                                       e.announcement.cursor == RCS_CURSOR_IN_VIDEO, e.layers),
                                            lane.desktop);
                    info.bitrate_bps = e.announcement.bitrate_bps;
                    infos[encoding] = info;
                }
            }
            for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                if (infos[encoding]) {
                    publish(index, encoding, *infos[encoding]);
                }
            }
        });
    }

    void encoderMode(uint32_t index, uint32_t encoding, rcs_codec codec, bool lowLatency, bool refInvalidation,
                     uint32_t temporalLayers, const std::string& name) override {
        guard([&] {
            Session::CodecInfo encoder;
            std::optional<rcs_stream_info> announcement; // With the layers it makes now.
            {
                std::lock_guard lock(mutex_);
                auto& e = lanes_.at(index).encodings.at(encoding);
                if (!e.plan.codec) {
                    return; // An encoding that stopped.
                }
                e.refInvalidation = refInvalidation;
                if (e.layers.exchange(temporalLayers) != temporalLayers && e.announced) {
                    announcement = e.announcement;
                    announcement->layers = announcement->received_layers = temporalLayers;
                }
                auto& info = e.encoderInfo;
                info.codec = codec;
                info.lowLatency = lowLatency;
                info.refInvalidation = refInvalidation;
                info.temporalLayers = temporalLayers;
                info.name = name;
                encoder = info;
            }
            if (announcement) {
                publish(index, encoding, *announcement);
            }
            if (encoding) {
                session_.updateStats([&](Stats& s) {
                    auto& send = s.video[index].sendOf(encoding);
                    send.low_latency = lowLatency;
                    send.temporal_layers = temporalLayers;
                    copyText(send.encoder, name);
                });
            } else {
                session_.codecChanged(encoder);
            }
        });
    }

    void encodingFailed(uint32_t index, uint32_t encoding, rcs_result result, const std::string& message) override {
        std::lock_guard lock(mutex_);
        if (!encoding) { // The stream runs as it was: its start, which comes next, is announced as such.
            auto& e = lanes_.at(index).main();
            e.refusedPlan = std::exchange(e.plan, e.previousPlan);
            e.refusedStream = std::exchange(e.stream, e.previousStream);
        }
        encodingFailures_.push_back({{index, encoding}, {result, message}}); // The loop's to handle.
    }

    void captureStatus(uint32_t index, const agent::CaptureStatus& status) override {
        guard([&] {
            bool changed;
            {
                std::lock_guard lock(mutex_);
                auto& lane = lanes_.at(index);
                if (!lane.main().plan.codec) {
                    return; // The last words of a stream that stopped.
                }
                changed = std::exchange(lane.paused, status.paused) != status.paused;
                for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                    lane.encodings[encoding].limitedMs = status.limitedMs[encoding];
                }
            }
            session_.updateStats([&](Stats& s) {
                for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                    if (s.video[index].encodings & (1u << encoding)) {
                        s.video[index].sendOf(encoding).limited_ms = status.limitedMs[encoding];
                    }
                }
                auto& capture = s.video[index].capture;
                capture.captured_frames = status.captured;
                capture.overwritten_frames = status.overwritten;
                capture.paused = status.paused;
                capture.interruptions = status.interruptions;
            });
            if (changed) {
                session_.captureChanged(index, status.paused);
            }
        });
    }

    // Each stream's capture sees the pointer on its own display: the stream
    // it shows on is the one whose capture last saw it, until another does.
    void cursor(uint32_t index, const CursorState& state) override {
        guard([&] {
            std::shared_lock viewers(viewers_.mutex);
            std::lock_guard lock(mutex_);
            lanes_.at(index).pointer = state; // For viewers that join.
            if (!state.visible && index != pointerStream_) {
                return; // Not on this display, nor was it.
            }
            pointerStream_ = index;
            for (const auto& viewer : viewers_.list) {
                viewer->cursor.update(pointerStream_, pointerFor(*viewer));
            }
        });
    }

    // The agent's machine changed (what it executes, its displays): offered again, the streams' places checked.
    void machineChanged() override { machineChanged_ = true; }

    void desktop(const platform::DesktopState& state) override {
        guard([&] {
            forEach(
                [&](Viewer& viewer) { viewer.out.message(protocol::DesktopState{state.kind, state.userLoggedOn}); });
        });
    }

    // ---- State ---------------------------------------------------------------------------

    Session& session_;
    ShareAgent& agent_;
    ViewerSet& viewers_;

    // The loop's thread.
    SessionConfig live_{};
    protocol::CodecOffer offer_; // This end's, as the viewers last received it.
    uint64_t seenSettings_ = 0, seenViewers_ = 0;
    bool planned_ = false;
    bool planDirty_ = false;         // The group's policy wants encodings started or stopped.
    bool cursorInVideo_ = true;      // What the agent was last told.
    bool cursorFollowsVideo_ = true; // The cursor senders are off: the pointer is in the picture.
    RateLimits limits_;
    size_t rated_ = 0; // The viewers the limits were handed to.
    // Each encoding's bitrate, its streams together (0: the main one's until its viewers are measured).
    std::array<uint32_t, RCS_MAX_ENCODINGS> bitrates_{};
    std::atomic<uint32_t>& currentBitrate_;          // bitrates_[0], for the acceptor's threads (the run's).
    const std::shared_ptr<ForwarderLink> forwarder_; // FORWARDER: the run's hop.
    Clock::time_point nextPathCheck_ = Clock::now();
    GroupPlan plan_;
    std::array<Clock::time_point, RCS_MAX_ENCODINGS> refusedUntil_{}; // AUTO: encodings the system refused lately.
    using LayerBytes = std::array<std::array<uint64_t, 3>, RCS_MAX_ENCODINGS>; // By encoding and temporal layer.
    std::deque<std::pair<Clock::time_point, LayerBytes>> sent_;                // Encoded bytes by turn.
    std::pair<Clock::time_point, uint64_t> sendWindow_{}; // The viewers' bytes a second ago...
    uint64_t sendBps_ = 0;                                // ...and the rate since.
    // Viewers that joined on another encoding than the main one (place()),
    // reported at the loop's next turn.
    std::vector<std::pair<rcs_viewer_id, rcs_viewer_video_event>> placed_;

    // Shared with the agent's threads, under mutex_ (and each encoding's fields as marked).
    struct PlanInputs {
        SessionConfig live;
        protocol::CodecOffer offer, viewerOffer;
        protocol::StreamRequest request;
        agent::Machine machine;
    };
    std::mutex mutex_;
    PlanInputs inputs_;
    std::array<Lane, RCS_MAX_STREAMS> lanes_;
    uint32_t pointerStream_ = 0; // The stream whose display the pointer is on.
    std::exception_ptr sendFailure_;
    std::deque<std::pair<std::pair<uint32_t, uint32_t>, FailureInfo>> encodingFailures_; // (stream, encoding), why.
    std::atomic<bool> machineChanged_{false}; // The agent's machine changed: offer it again.
};

// A viewer that passed the handshake, its offer and settings taken and the
// host's decision made: it joins at the loop's next turn.
struct Joined {
    std::unique_ptr<Connection> connection;
    ViewerHello hello;
    rcs_viewer_id id = 0;
    bool control = false;
    uint64_t route = 0;
    rcs_viewer_video video = RCS_VIEWER_VIDEO_INIT;
    uint64_t peer = 0; // FORWARDER: its number in the room.
};

// Refuses a viewer that passed the handshake: it hears why and ends its run
// with `result`; the connection closes once it did, or after 2 s of anything
// else it sends (or 5 s of silence).
void refuse(Connection& connection, rcs_result result, const std::string& reason) {
    log::info("session", "Refused a viewer (" + std::string(rcs_result_message(result)) + "): " + reason);
    try {
        connection.send(protocol::Refused{result, reason});
        for (const auto deadline = Clock::now() + 2s; Clock::now() < deadline;) {
            connection.receive();
        }
    } catch (const Failure&) {
    }
}

class ShareRun {
public:
    ShareRun(Session& session, ShareAgent& agent)
        : session_(session), agent_(agent), viewers_(session.viewerSet()), audio_(session) {
        acceptor_ = std::make_unique<Acceptor>(
            session, [this] { return bitrate_.load(); }, // Where a new viewer's adaptive bitrate starts.
            [this](Incoming incoming) { startJoiner(std::move(incoming)); },
            [this](const FailureInfo& failure, bool peer) {
                std::lock_guard lock(mutex_);
                failedJoiners_.push_back({failure, RCS_PEER_INFO_INIT, peer});
            });
        forwarder_ = acceptor_->forwarder();
        forwarderSeen_ = networkSeenNow(session_);
        if (forwarder_) { // The sound once per format for every viewer, through the forwarder.
            sounds_ = std::make_unique<SoundGroups>(
                session_, audio_,
                [link = forwarder_](uint8_t sound, const AudioPacket& packet) { link->publishSound(sound, packet); });
        }
    }

    ~ShareRun() {
        session_.cancel(); // The acceptor, the joiners and their blocking calls end...
        viewers_.decided.notify_all();
        acceptor_.reset();
        {
            std::lock_guard lock(mutex_); // ...also those of connections ICE found, which have a token of their own.
            for (auto* connection : joining_) {
                connection->cancel();
            }
        }
        for (auto& joiner : joiners_) {
            joiner.thread.join();
        }
        // Every viewer says goodbye at once: stopping takes one goodbye's time.
        std::vector<std::unique_ptr<Viewer>> leaving;
        {
            std::unique_lock lock(viewers_.mutex);
            leaving.swap(viewers_.list);
            viewers_.joining = 0;
        }
        for (auto& viewer : leaving) {
            auto info = viewer->info(false);
            info.joined = 0;
            info.result = ended_.result;
            copyText(info.message, ended_.message);
            session_.viewerChanged(info);
            release(*viewer);
            reap(std::move(viewer));
        }
        for (auto& reaper : reapers_) {
            reaper.thread.join();
        }
        stream_.reset();
    }

    // Until the last viewer leaves: throws why (PeerFailure: a sharer that
    // keeps listening waits for the next one), or what failed on this end.
    void run() {
        try {
            loop();
        } catch (const Failure& failure) {
            ended_ = {failure.result(), failure.what()};
            throw;
        }
    }

private:
    // A thread joined once it says it is done, not only when the run ends.
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    // A viewer that could not join, and whether it or its path caused it.
    struct FailedJoin {
        FailureInfo failure;
        rcs_peer_info peer = RCS_PEER_INFO_INIT;
        bool peerCaused = true;
    };

    // A joiner's connection (in joining_ from before its thread starts),
    // which the run cancels when it ends; out once the joiner is done with it.
    class Joining {
    public:
        Joining(ShareRun& run, Connection& connection) : run_(run), connection_(connection) {}
        ~Joining() {
            std::lock_guard lock(run_.mutex_);
            run_.joining_.erase(&connection_);
        }
        Joining(const Joining&) = delete;
        Joining& operator=(const Joining&) = delete;

    private:
        ShareRun& run_;
        Connection& connection_;
    };

    static void joinDone(std::vector<Worker>& workers) {
        for (auto worker = workers.begin(); worker != workers.end();) {
            if (*worker->done) {
                worker->thread.join();
                worker = workers.erase(worker);
            } else {
                ++worker;
            }
        }
    }

    void loop() {
        while (!session_.cancelled()) {
            agent_.rethrowFailure();
            acceptor_->rethrowFailure();
            std::deque<Joined> joined;
            std::deque<FailedJoin> failed;
            {
                std::lock_guard lock(mutex_);
                joined.swap(joined_);
                failed.swap(failedJoiners_);
            }
            for (const auto& failure : failed) {
                firstFailed(failure);
            }
            for (auto& viewer : joined) {
                admit(std::move(viewer));
            }
            if (stream_) {
                stream_->step();
            }
            forwarderKeys();
            followPaths();
            leave();
            finishThreads();
            // Keyframe requests, bitrates, settings, joins and the rest are
            // picked up here, every 2 ms (a timed wait would take a 15.6 ms
            // tick on Windows).
            std::unique_lock lock(mutex_);
            if (joined_.empty() && failedJoiners_.empty()) {
                lock.unlock();
                sleepPrecise(2ms);
            }
        }
    }

    // ---- Joining ---------------------------------------------------------------------------

    void startJoiner(Incoming incoming) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto held = std::make_shared<Incoming>(std::move(incoming)); // Still here when no thread takes it.
        std::lock_guard lock(mutex_);
        joining_.insert(held->connection.get());
        try {
            joiners_.push_back({log::thread([this, done, held] {
                                    join(std::move(*held));
                                    *done = true;
                                }),
                                done});
        } catch (const std::system_error& error) {
            // The system has no thread for it: it hears so, and goes.
            joining_.erase(held->connection.get());
            const std::string reason = std::string("The sharer cannot start another viewer: ") + error.what();
            log::warning("session", reason);
            try {
                held->connection->send(protocol::Refused{RCS_ERROR_BUSY, reason});
            } catch (const Failure&) {
            }
        }
    }

    void finishThreads() {
        {
            std::lock_guard lock(mutex_);
            joinDone(joiners_);
        }
        joinDone(reapers_);
    }

    // A joiner's place among max_viewers, given back once (`reserved` false after).
    void release(bool& reserved, rcs_viewer_id id) {
        if (reserved) {
            std::unique_lock lock(viewers_.mutex);
            --viewers_.joining;
            viewers_.joiningOffers.erase(id);
            reserved = false;
        }
    }

    // A viewer the host may decide on (rcs_session_admit_viewer), from the
    // call of on_viewer_join to the end of its wait.
    class Waiting {
    public:
        Waiting(ViewerSet& viewers, rcs_viewer_id id) : viewers_(viewers), id_(id) {
            std::unique_lock lock(viewers_.mutex);
            viewers_.waiting.emplace(id, std::nullopt);
        }
        ~Waiting() {
            std::unique_lock lock(viewers_.mutex);
            viewers_.waiting.erase(id_);
        }
        Waiting(const Waiting&) = delete;
        Waiting& operator=(const Waiting&) = delete;

    private:
        ViewerSet& viewers_;
        const rcs_viewer_id id_;
    };

    // A viewer whose host decides later: the decision, or none when the time
    // ran out. Meanwhile both ends say they are there (protocol::Admission)
    // about once a second; a viewer that leaves, goes silent or breaks the
    // protocol, and the run's end, throw Failure (RCS_EVENT_ADMISSION GONE).
    std::optional<rcs_viewer_decision> waitForDecision(Connection& connection, rcs_viewer_id id, uint32_t timeoutMs) {
        using namespace std::chrono;
        const auto deadline = timeoutMs ? Clock::now() + milliseconds(timeoutMs) : Clock::time_point::max();
        const auto decided = [&] { return viewers_.waiting.at(id).has_value(); };
        try {
            for (;;) {
                const auto now = Clock::now();
                if (now >= deadline) {
                    return std::nullopt;
                }
                const auto left = timeoutMs ? std::max<int64_t>(1, duration_cast<milliseconds>(deadline - now).count()) : 0;
                connection.send(protocol::Admission{uint32_t(left)});
                if (!std::holds_alternative<protocol::Admission>(connection.receive())) {
                    fail(RCS_ERROR_PROTOCOL, "The viewer sent something else while it waited to be admitted");
                }
                std::unique_lock lock(viewers_.mutex);
                viewers_.decided.wait_until(lock, std::min(deadline, Clock::now() + 1s),
                                            [&] { return decided() || session_.cancelled(); });
                if (decided()) {
                    return viewers_.waiting.at(id);
                }
                if (session_.cancelled()) {
                    fail(RCS_ERROR_CANCELLED, "The run ended");
                }
            }
        } catch (const Failure& failure) {
            log::info("session", "Viewer " + std::to_string(id) + " left while it waited for the host's decision (" +
                                     rcs_result_message(failure.result()) + "): " + failure.what());
            session_.admission(RCS_ADMISSION_GONE, id, 0, failure.result(), failure.what());
            throw;
        }
    }

    // A viewer's way in (its own thread): its offer and settings, whether it
    // can be served next to the viewers there are, the host's decision.
    void join(Incoming incoming) noexcept {
        auto& connection = *incoming.connection;
        const Joining joining(*this, connection);
        // One not let in leaves its transport's place too (a forwarder's room).
        struct TurnedAway {
            std::function<void()> away;
            ~TurnedAway() {
                if (away) {
                    away();
                }
            }
        } turnedAway{std::move(incoming.turnedAway)};
        bool reserved = false;
        rcs_viewer_id id = 0;
        rcs_peer_info peer = RCS_PEER_INFO_INIT;
        session_.updateStats([](Stats& s) { ++s.network.connections; }); // It passed the handshake.
        try {
            auto hello = receiveViewerHello(connection);
            peer = peerOf(hello.offer, connection.peerKey());
            const auto live = session_.liveConfig();
            const auto offer = offerOf(*live, agent_.machine());
            bool alone;
            {
                std::unique_lock lock(viewers_.mutex);
                // Viewers whose path was lost give way to this one once it is let in (admit).
                const size_t held = size_t(std::count_if(viewers_.list.begin(), viewers_.list.end(),
                                                         [](const auto& v) { return v->held.load(); }));
                const size_t present = viewers_.list.size() + viewers_.joining;
                alone = !present;
                if (present - held >= maxViewers(*live)) {
                    lock.unlock();
                    refuse(connection, RCS_ERROR_BUSY,
                           "The sharer serves as many viewers as it takes (" + std::to_string(maxViewers(*live)) +
                               ")");
                    return;
                }
                const rcs_codec watched = viewers_.streamCodec;
                const rcs_picture_format watchedFormat = viewers_.streamFormat;
                // One that cannot take the main encoding may get a lower one in a codec it decodes.
                const bool lower = decodesLower(*live, offer, hello.offer, viewers_);
                if (!alone && watched && !takes(hello.offer, watched, watchedFormat) && !lower) {
                    lock.unlock();
                    refuse(connection, RCS_ERROR_BUSY,
                           std::string("The viewer does not decode ") + codecName(watched) + " in " +
                               pictureFormatName(watchedFormat) + ", which the other viewers watch; it offers " +
                               codecList(hello.offer.codecs));
                    return;
                }
                const PictureSize watchedSize = viewers_.streamSize;
                if (!alone && watched && !lower) {
                    if (const auto problem = sizeProblem(watched, watchedSize.width, watchedSize.height, {},
                                                         largestOffered(hello.offer, watched));
                        !problem.empty()) {
                        lock.unlock();
                        refuse(connection, RCS_ERROR_BUSY,
                               "The other viewers watch a picture this viewer cannot "
                               "take: " +
                                   problem);
                        return;
                    }
                }
                if (!alone && !watched) {
                    // No stream yet, while others join or have just joined:
                    // the one stream for all must suit them and this viewer.
                    std::vector<protocol::CodecOffer> each{hello.offer};
                    for (const auto& viewer : viewers_.list) {
                        each.push_back(viewer->hello().offer);
                    }
                    for (const auto& [joiner, joinerOffer] : viewers_.joiningOffers) {
                        each.push_back(joinerOffer);
                    }
                    const auto common = commonOffer(each);
                    std::string problem;
                    if (!negotiatedCodec(offer, common)) {
                        problem = "The viewer and those joining with it share no stream: " +
                                  noCommonStream(offer, common, true);
                    } else if (const auto plan = planStream(*live, offer, common, hello.settings.request, {});
                               !plan.refusal.empty()) {
                        problem = "The viewers joining with this one take no picture it can: " + plan.refusal;
                    }
                    if (!problem.empty()) {
                        lock.unlock();
                        refuse(connection, RCS_ERROR_BUSY, problem);
                        return;
                    }
                }
                if (!alone && offer.sendsAudio && hello.offer.receivesAudio &&
                    !negotiatedCodec(offer.sendAudio, hello.offer.receiveAudio)) {
                    lock.unlock();
                    refuse(connection, RCS_ERROR_BUSY,
                           "The viewer decodes none of " + audioCodecList(offer.sendAudio) +
                               " audio, which the sharer sends the other viewers; it offers " +
                               audioCodecList(hello.offer.receiveAudio));
                    return;
                }
                id = viewers_.nextId++;
                ++viewers_.joining;
                viewers_.joiningOffers.emplace(id, hello.offer);
                reserved = true;
            }
            if (alone) {
                // The first viewer: one this end cannot serve fails as a
                // single viewer always has, both ends naming the offers.
                const rcs_codec codec = negotiatedCodec(offer, hello.offer);
                if (!codec) {
                    connection.send(offer);
                    throw PeerFailure(RCS_ERROR_UNSUPPORTED, noCommonStream(offer, hello.offer, false));
                }
                if (offer.sendsAudio && hello.offer.receivesAudio &&
                    !negotiatedCodec(offer.sendAudio, hello.offer.receiveAudio)) {
                    connection.send(offer);
                    throw PeerFailure(RCS_ERROR_UNSUPPORTED, "The viewer cannot decode " +
                                                                 audioCodecList(offer.sendAudio) + " audio; it offers " +
                                                                 audioCodecList(hello.offer.receiveAudio));
                }
                if (hello.offer.sendsAudio && offer.receivesAudio &&
                    !negotiatedCodec(hello.offer.sendAudio, offer.receiveAudio)) {
                    connection.send(offer);
                    throw PeerFailure(RCS_ERROR_UNSUPPORTED, "This end cannot decode the viewer's " +
                                                                 audioCodecList(hello.offer.sendAudio) +
                                                                 " audio; it offers " + audioCodecList(offer.receiveAudio));
                }
                // A stream whose size the negotiated codec does not take is
                // refused as part of the negotiation.
                if (const auto plan = planStream(*live, offer, hello.offer, hello.settings.request, {});
                    !plan.refusal.empty()) {
                    connection.send(protocol::Refused{RCS_ERROR_UNSUPPORTED, plan.refusal});
                    throw PeerFailure(RCS_ERROR_UNSUPPORTED, plan.refusal);
                }
                log::info("session", std::string("Negotiated ") + codecName(codec) + " (this end offers " +
                                         codecList(offer.codecs) + ", the viewer " + codecList(hello.offer.codecs) +
                                         ")");
            }
            rcs_viewer_decision decision = RCS_VIEWER_DECISION_INIT;
            decision.control = live->control.allowed;
            decision.video.struct_size = sizeof(decision.video);
            bool waited = false;
            if (const auto decide = session_.callbacks().on_viewer_join) {
                rcs_viewer_info info = RCS_VIEWER_INFO_INIT;
                info.viewer_id = id;
                info.joined_ns = monotonicNs();
                info.route = incoming.route;
                info.peer = peer;
                if (const auto address = connection.peerAddress()) {
                    copyText(info.address, address->address());
                    info.port = address->port();
                }
                info.control.allowed = decision.control;
                const Waiting waiting(viewers_, id);
                {
                    std::lock_guard lock(joinCallbackMutex_);
                    session_.callHost(decide, &info, &decision);
                }
                if (decision.admission == RCS_ADMIT_LATER) {
                    const uint32_t timeoutMs = live->viewers.admission_timeout_ms;
                    log::info("session", "Viewer " + std::to_string(id) + " waits for the host's decision");
                    session_.admission(RCS_ADMISSION_WAITING, id, timeoutMs);
                    const auto decided = waitForDecision(connection, id, timeoutMs);
                    if (!decided) {
                        const std::string reason =
                            "The sharer's host did not decide within " + secondsText(std::chrono::milliseconds(timeoutMs));
                        release(reserved, id);
                        session_.admission(RCS_ADMISSION_REFUSED, id, 0, RCS_ERROR_TIMEOUT, reason);
                        refuse(connection, RCS_ERROR_TIMEOUT, reason);
                        return;
                    }
                    decision = *decided;
                    waited = true;
                } else if (decision.admission > RCS_ADMIT_LATER) {
                    log::warning("session", "on_viewer_join decided an admission no version defines (" +
                                                std::to_string(decision.admission) + "); viewer " +
                                                std::to_string(id) + " is refused");
                    decision.admission = RCS_REFUSE;
                }
            }
            if (decision.admission != RCS_ADMIT) {
                const std::string_view given(decision.reason, strnlen(decision.reason, sizeof(decision.reason)));
                const std::string reason = given.empty() ? "The host refused this viewer" : std::string(given);
                release(reserved, id);
                if (waited) {
                    session_.admission(RCS_ADMISSION_REFUSED, id, 0, RCS_ERROR_REFUSED, reason);
                }
                refuse(connection, RCS_ERROR_REFUSED, reason);
                return;
            }
            if (waited) {
                session_.admission(RCS_ADMISSION_ADMITTED, id);
                connection.send(offerOf(*session_.liveConfig(), agent_.machine())); // As it is after the wait.
            } else {
                connection.send(offer);
            }
            const auto& v = decision.video;
            if (v.struct_size != sizeof(v) || v.counts_for_bitrate > 1 ||
                (v.encoding != RCS_ENCODING_ANY && v.encoding >= RCS_MAX_ENCODINGS) ||
                v.layers > RCS_SCALABILITY_L1T3) {
                log::warning("session", "The host decided an rcs_viewer_video no version defines for viewer " +
                                            std::to_string(id) + "; it gets the picture as without one");
                decision.video = RCS_VIEWER_VIDEO_INIT;
            }
            incoming.viewer->store(id);
            turnedAway.away = nullptr;
            std::lock_guard lock(mutex_);
            joined_.push_back({std::move(incoming.connection), std::move(hello), id, decision.control != 0,
                               incoming.route, decision.video, incoming.peer});
            return;
        } catch (const Failure& failure) {
            release(reserved, id);
            if (failure.result() == RCS_ERROR_CANCELLED) {
                return;
            }
            log::info("session", std::string("A viewer could not join (") + rcs_result_message(failure.result()) +
                                     "): " + failure.what());
            std::lock_guard lock(mutex_);
            failedJoiners_.push_back({{failure.result(), failure.what()}, peer, true});
        } catch (const std::exception& exception) {
            release(reserved, id);
            log::error("session", std::string("A viewer could not join: ") + exception.what());
        }
    }

    // A viewer could not join. Before any joined, that is how the run's
    // single viewer always failed: a sharer that keeps listening goes on (and
    // says why), one that does not ends the run; and a failure of this end's
    // (P2P and RELAY: its TURN server, its interface) ends it. While viewers
    // watch it was logged.
    void firstFailed(const FailedJoin& failed) {
        if (everJoined_) {
            return;
        }
        if (!failed.peerCaused) {
            fail(failed.failure.result, failed.failure.message);
        }
        if (!session_.liveConfig()->reconnect.keep_listening) {
            if (failed.peer.platform) {
                session_.setPeer(failed.peer); // What it offered, as a host asks after the run failed.
            }
            throw PeerFailure(failed.failure.result, failed.failure.message);
        }
        session_.setState(RCS_STATE_LISTENING, failed.failure.result);
    }

    // A viewer that joined: added to the set, the stream starts with the first.
    void admit(Joined joined) {
        std::unique_ptr<Viewer> made;
        try {
            made = std::make_unique<Viewer>(session_, audio_, sounds_.get(), joined.id, joined.route, joined.peer,
                                            std::move(joined.connection), std::move(joined.hello), joined.control,
                                            joined.video);
        } catch (const std::exception& failure) { // Failure, or a thread the system could not start.
            std::unique_lock lock(viewers_.mutex);
            --viewers_.joining;
            viewers_.joiningOffers.erase(joined.id);
            log::warning("session", std::string("A viewer could not join: ") + failure.what());
            return;
        }
        Viewer& viewer = *made;
        const bool first = !stream_;
        {
            std::unique_lock lock(viewers_.mutex);
            --viewers_.joining;
            viewers_.joiningOffers.erase(joined.id);
            viewers_.list.push_back(std::move(made));
            if (stream_) {
                stream_->joined(viewer);
            }
        }
        ++viewers_.version;
        everJoined_ = true;
        log::info("network", "Viewer " + std::to_string(viewer.id) + " joined");
        if (forwarder_) { // The stream key of what the forwarder kept for it, or one step on.
            uint32_t streams = 0;
            for (uint32_t index = 0; index < RCS_MAX_STREAMS && !session_.liveConfig()->forwarder.from_admission;
                 ++index) {
                streams |= viewer.takes(index) ? 1u << index : 0;
            }
            const auto key = forwarder_->join(streams, viewer.encoding);
            viewer.keyGiven = {key.generation, key.step};
            viewer.out.message(protocol::MediaKey{key});
            session_.updateStats([&](Stats& s) {
                s.forwarder.generation = key.generation;
                s.forwarder.step = key.step;
            });
        }
        // Its place was one a viewer whose path was lost held: the oldest such goes.
        for (size_t over = viewers_.list.size(); over > maxViewers(*session_.liveConfig());) {
            const auto held = std::find_if(viewers_.list.begin(), viewers_.list.end(),
                                           [](const auto& v) { return v->held.load() && !v->leaving(); });
            if (held == viewers_.list.end()) {
                break;
            }
            (*held)->leave(RCS_ERROR_NETWORK, "Its path was lost, and a new viewer took its place");
            --over;
        }
        if (first) {
            // RUNNING before the stream's events (stream, codec, sound, control),
            // as on the viewer's side.
            session_.setState(RCS_STATE_RUNNING);
        }
        bool main;
        {
            std::shared_lock lock(viewers_.mutex);
            main = viewers_.main() == &viewer;
        }
        auto info = viewer.info(main);
        {
            auto& control = session_.shareControl();
            std::lock_guard lock(control.mutex);
            info.control = {sizeof(rcs_control_info), viewer.control, control.granted(viewer.control)};
        }
        session_.viewerChanged(info);
        session_.setPeer(info.peer, viewer.id);
        if (first) {
            stream_.emplace(session_, agent_, bitrate_, forwarder_);
        }
    }

    // ---- Paths -----------------------------------------------------------------------------

    // What happened to each viewer's path (rcs_paths_config): reported; a
    // path that stalls releases what the viewer holds and drops the input it
    // sent before; one that is lost keeps the viewer (holds its place until a
    // new viewer needs it) and revokes its control unless the host restores
    // it; the sound starts afresh on the path that takes over.
    void followPaths() {
        if (const uint64_t version = session_.settingsVersion(); version != pathSettings_) {
            pathSettings_ = version;
            const bool migrate = session_.liveConfig()->paths.migrate != 0;
            for (const auto& viewer : viewers_.list) {
                viewer->connection().setMigrate(migrate);
            }
        }
        if (forwarder_) { // The viewers' pipes go with the hop to the forwarder, whose changes are this end's own.
            followNetworkChoice(session_, forwarder_->hop(), nullptr, forwarderSeen_);
            for (const auto& event : forwarder_->hop().takePathEvents()) {
                session_.pathChanged(event, 0);
            }
        }
        for (const auto& viewer : viewers_.list) {
            if (!forwarder_) {
                followNetworkChoice(session_, viewer->connection(), viewer->restart.get(), viewer->networkSeen);
            }
            for (const auto& event : viewer->connection().takePathEvents()) {
                pathChanged(*viewer, event);
            }
        }
    }

    void pathChanged(Viewer& viewer, const protocol::PathEvent& event) {
        session_.pathChanged(event, viewer.id);
        if (viewer.restart) {
            viewer.restart->pathChanged(event);
        }
        auto& control = session_.shareControl();
        if (event.change == RCS_PATH_CHANGING || event.change == RCS_PATH_LOST) {
            std::lock_guard lock(control.mutex);
            if (control.granted(viewer.control)) {
                control.release(viewer.id);
            }
            ++viewer.epoch; // Its input from before reaches this end later, if at all: dropped.
            if (event.change == RCS_PATH_LOST && !session_.liveConfig()->paths.restore_control) {
                viewer.control = false;
            }
        }
        if (event.change == RCS_PATH_LOST) {
            viewer.held = true;
        } else if (event.change == RCS_PATH_CHANGED) {
            viewer.held = false;
            viewer.heard.discontinuity();
        }
    }

    // ---- Leaving ---------------------------------------------------------------------------

    // Viewers that failed or are to leave go; the run ends with the last.
    void leave() {
        for (size_t i = 0; i < viewers_.list.size();) {
            Viewer& viewer = *viewers_.list[i];
            if (!viewer.leaving()) {
                ++i;
                continue;
            }
            FailureInfo why;
            const bool peer = peerCaused(viewer.failure(), why);
            std::unique_ptr<Viewer> gone;
            bool wasMain;
            {
                std::unique_lock lock(viewers_.mutex);
                wasMain = viewers_.main() == &viewer;
                gone = std::move(viewers_.list[i]);
                viewers_.list.erase(viewers_.list.begin() + ptrdiff_t(i));
                if (stream_) {
                    stream_->left(*gone);
                }
                if (viewers_.chosenMain == gone->id) {
                    viewers_.chosenMain = 0;
                }
                session_.viewerLeft(gone->id, gone->stats(), wasMain);
            }
            ++viewers_.version;
            auto info = gone->info(wasMain);
            info.joined = 0;
            info.result = why.result;
            copyText(info.message, why.message);
            log::info("network", "Viewer " + std::to_string(gone->id) + " left (" + rcs_result_message(why.result) +
                                     "): " + why.message);
            release(*gone);
            if (forwarder_) { // It gets nothing more, and holds no key of what comes.
                forwarder_->drop(gone->peer);
                if (!viewers_.list.empty()) {
                    rekey();
                }
            }
            if (gone->data.clear()) {
                session_.dataRoom(0, gone->id); // A host waiting for its room has it now.
            }
            session_.viewerChanged(info);
            if (wasMain && !viewers_.list.empty()) {
                session_.mainViewerChanged();
            }
            reap(std::move(gone));
            if (!peer) {
                fail(why.result, why.message);
            }
            if (viewers_.list.empty()) {
                throw PeerFailure(why.result, why.message);
            }
        }
    }

    // ---- The stream key and what each viewer gets (FORWARDER) -----------------------------

    // A new generation of the stream key for the viewers that stay (one left,
    // or key_rotation_ms passed); sealed with once they all have it, or a
    // second on (forwarderKeys).
    void rekey() {
        const auto next = forwarder_->keys().rekey();
        for (const auto& viewer : viewers_.list) {
            viewer->out.message(protocol::MediaKey{next});
        }
        rekeyAt_ = lastGeneration_ = Clock::now();
        session_.updateStats([](Stats& s) { ++s.forwarder.rekeys; });
    }

    // Each turn: the rotation; a waiting generation used once every viewer
    // has it, or a second on; the key in use again to a viewer that said it
    // lacks it (at most once a second); and what each viewer gets: its
    // streams of its encoding once it has a key, nothing before.
    void forwarderKeys() {
        if (!forwarder_) {
            return;
        }
        const auto now = Clock::now();
        auto& keys = forwarder_->keys();
        auto pending = keys.pending();
        const uint32_t rotation = session_.liveConfig()->forwarder.key_rotation_ms;
        if (rotation && !pending && now - lastGeneration_ >= std::chrono::milliseconds(rotation)) {
            rekey();
            pending = keys.pending();
        }
        const auto generationOf = [](const Viewer& viewer) { return uint32_t(viewer.keyHeld.load() >> 32); };
        if (pending) {
            const bool all = std::all_of(viewers_.list.begin(), viewers_.list.end(), [&](const auto& viewer) {
                return generationOf(*viewer) >= pending->generation;
            });
            if (all || now - rekeyAt_ >= 1s) {
                keys.advance();
                const auto current = keys.current();
                session_.updateStats([&](Stats& s) {
                    s.forwarder.generation = current.generation;
                    s.forwarder.step = current.step;
                });
                pending.reset();
            }
        }
        const auto current = keys.current();
        for (const auto& viewer : viewers_.list) {
            const uint32_t held = generationOf(*viewer);
            if (held && held < current.generation && (!pending || held < pending->generation) &&
                now - viewer->keyResent >= 1s) {
                viewer->keyResent = now;
                viewer->out.message(protocol::MediaKey{current});
                session_.updateStats([](Stats& s) { ++s.forwarder.key_resends; });
            }
            uint32_t streams = 0;
            for (uint32_t index = 0; held && index < RCS_MAX_STREAMS; ++index) {
                streams |= viewer->takes(index) ? 1u << index : 0;
            }
            forwarder_->subscribe(viewer->peer, streams, viewer->encoding, held ? viewer->audio.sound() : 0,
                                  viewer->keyGiven);
        }
    }

    // What a viewer that leaves held is released: everything held, as whose a
    // key is cannot be told.
    void release(Viewer& viewer) {
        auto& control = session_.shareControl();
        std::lock_guard lock(control.mutex);
        if (control.granted(viewer.control)) {
            control.release(viewer.id);
        }
        viewer.control = false;
    }

    // A viewer is destroyed on a thread of its own: its goodbye does not hold
    // up the loop, nor the others' at the end.
    void reap(std::unique_ptr<Viewer> viewer) {
        viewer->cancelled = true;
        auto done = std::make_shared<std::atomic<bool>>(false);
        reapers_.push_back({log::thread([viewer = std::move(viewer), done]() mutable {
                                viewer.reset();
                                *done = true;
                            }),
                            done});
    }

    Session& session_;
    ShareAgent& agent_;
    ViewerSet& viewers_;
    AudioHub audio_;                      // Before the viewers' senders and after them.
    std::unique_ptr<SoundGroups> sounds_; // FORWARDER: the viewers' sound, through the forwarder.
    std::atomic<uint32_t> bitrate_{0};    // The streams' bitrate now (the stream's), read by the acceptor's threads.
    std::optional<ShareStream> stream_;
    bool everJoined_ = false;
    uint64_t pathSettings_ = 0;    // The settings' version the viewers' connections follow (paths.migrate).
    FailureInfo ended_;            // Why the run ended (RCS_OK: this end stopped).
    std::mutex joinCallbackMutex_; // on_viewer_join calls never overlap.

    std::mutex mutex_; // Below.
    std::vector<Worker> joiners_;
    std::set<Connection*> joining_; // The joiners' connections.
    std::deque<Joined> joined_;
    std::deque<FailedJoin> failedJoiners_;

    std::vector<Worker> reapers_;        // The loop's thread.
    std::shared_ptr<ForwarderLink> forwarder_;      // FORWARDER: the acceptor's hop, until the viewers' connections went.
    Clock::time_point rekeyAt_{}, lastGeneration_ = Clock::now(); // The stream key's latest generation.
    NetworkSeen forwarderSeen_;                                      // The hop's, as its sockets were opened.
    std::unique_ptr<Acceptor> acceptor_; // Last: its thread hands viewers to the above.
};

} // namespace

void runShare(Session& session) {
    // With an agent in another process the port opens once it attached: until
    // then there is nothing to offer a viewer.
    auto& agent = session.shareAgent();
    agent.waitAttached(session.cancelled());
    ShareRun run(session, agent);
    run.run();
}

} // namespace rcsdk::session
