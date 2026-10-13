// Share role: the run's viewers (rcs_viewers_config). Each viewer has a
// connection of its own and what serves it alone: a sender (Outbox: the
// stream's messages and video in order, on a thread of its own, so that a
// viewer that is slow holds up no one), a reader
// (ViewerMessages: input, keyframe requests, its settings and offer), its
// sound, data and pointer, its remote control and its statistics. The run's
// streams (share.cpp) hand every viewer of an encoding the same packets.
#pragma once

#include "protocol/connection.hpp"
#include "session/audio_stream.hpp"
#include "session/connect.hpp"
#include "session/cursor_stream.hpp"
#include "session/data_channel.hpp"
#include "session/session.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace rcsdk::session {

class Viewer;

// A video packet of `stream` as the network carries it, encoded once for every viewer.
struct VideoMessage {
    uint32_t stream = 0;
    crypto::Bytes bytes;
    bool keyframe = false;
    uint32_t temporalId = 0;
};

// Sends one viewer what the streams have for it, in order, on its own thread.
// Video it cannot send within a second of its coming (it waits here, or in the
// connection's pacing) is dropped, and each stream waits for a keyframe,
// which it asks for; a viewer that joins starts at one too. Pictures of the
// temporal layers above those it gets are left out (setLayers). Sound and
// pointer moves late by a second are dropped as well.
class Outbox {
public:
    Outbox(Viewer& viewer, protocol::Connection& connection);
    ~Outbox();
    Outbox(const Outbox&) = delete;
    Outbox& operator=(const Outbox&) = delete;

    // Any thread; quick, never waits for the network.
    void message(protocol::Message message);
    void video(const std::shared_ptr<const VideoMessage>& packet);
    // Video waiting to be sent, as time: here and in the connection.
    uint64_t backlogNs() const;
    // Video it dropped unsent: how often, and the bytes.
    std::pair<uint64_t, uint64_t> drops() const;
    // The streams that wait for a keyframe (it dropped video), as
    // (1u << stream) bits, when its connection can take one now; once per wait.
    uint32_t takeKeyframeWanted();
    // The viewer gets another encoding: what waits of the one it had goes,
    // here and in the connection, and each stream waits for a keyframe of the
    // new one (asked for by the stream), counted as neither drop nor want.
    void switchVideo();
    // How many temporal layers of `stream` it gets from now on: fewer at
    // once, more from the next picture of layer 0 on (the pictures of the
    // layers taken up again refer to nothing before it but layer 0's). Any thread.
    void setLayers(uint32_t stream, uint32_t layers) noexcept { wantedLayers_[stream] = layers; }
    // Pictures handed to the connection, those of its encoding it did not get
    // (waiting for a keyframe, a second behind) and those of the layers it
    // does not get; everything it sent, in bytes.
    struct Sent {
        uint64_t frames = 0, skipped = 0, upper = 0, bytes = 0;
    };
    Sent sent() const;

private:
    struct Item {
        std::shared_ptr<const VideoMessage> video;
        std::optional<protocol::Message> message;
        uint64_t queuedNs = 0;
    };
    void run() noexcept;
    void dropVideo(size_t popped = 0); // Holding mutex_: until the next keyframe (`popped`: bytes taken out already).

    Viewer& viewer_;
    protocol::Connection& connection_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> items_;
    uint64_t queuedNs_ = 0; // When the oldest video waiting came; 0: none waits.
    // By stream: video passes from a keyframe on (a stream, or one asked for
    // it, starts with one); after dropping video it waits for a keyframe not
    // asked for yet ((1u << stream) bits).
    std::array<bool, RCS_MAX_STREAMS> open_{};
    uint32_t wanted_ = 0;
    // By stream: the temporal layers it is to get, and those it lets through now.
    std::array<std::atomic<uint32_t>, RCS_MAX_STREAMS> wantedLayers_{};
    std::array<uint32_t, RCS_MAX_STREAMS> layers_{};
    uint64_t drops_ = 0, droppedBytes_ = 0;
    Sent sent_;
    bool stopping_ = false;
    std::thread thread_; // Last.
};

// Reads one viewer's messages on its own thread: input (executed while its
// control is granted), keyframe requests, its settings (the stream it asks
// for, how it wants the pointer, whether it takes sound), its codec offer,
// which it may change while running, and data.
class ViewerMessages {
public:
    ViewerMessages(Session& session, Viewer& viewer, protocol::Connection& connection);
    ~ViewerMessages();

private:
    void run() noexcept;
    void apply(const protocol::Input& input);

    Session& session_;
    Viewer& viewer_;
    protocol::Connection& connection_;
    DataReceiver data_;
    std::thread thread_;
};

// What a viewer told the sharer before it joined: its offer and settings.
struct ViewerHello {
    protocol::CodecOffer offer;
    protocol::ViewerSettings settings;
};
// The viewer's first messages (share): its offer, then its settings.
ViewerHello receiveViewerHello(protocol::Connection& connection);

class Viewer {
public:
    // Starts its threads; nothing is sent until the stream hands it something.
    // `route`: the host's to it (Incoming); `peer`: its number in the
    // forwarder's room (FORWARDER), 0 otherwise; `video`: how it gets the
    // picture (rcs_viewer_decision).
    // `sounds`: through a forwarder, the sharer's sound for every viewer.
    Viewer(Session& session, AudioHub& audio, SoundGroups* sounds, rcs_viewer_id id, uint64_t route, uint64_t peer,
           std::unique_ptr<protocol::Connection> connection, ViewerHello hello, bool control,
           const rcs_viewer_video& video);
    // Stops its threads, then tells the viewer that this end leaves.
    ~Viewer();
    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;

    const rcs_viewer_id id;
    const uint64_t route, peer;
    const uint64_t joinedNs;
    // Ends this viewer's blocking calls (set first when it leaves).
    std::atomic<bool> cancelled{false};
    // Its path is lost (RCS_PATH_LOST): it is kept, but gives its place to a new viewer.
    std::atomic<bool> held{false};
    // The run's thread: this end's choice of network its connection follows (followNetworkChoice).
    NetworkSeen networkSeen;

    // Its settings and offer as they are now; a change moves the set's version on.
    ViewerHello hello() const;
    void setSettings(const protocol::ViewerSettings& settings);
    void setOffer(const protocol::CodecOffer& offer);
    rcs_viewer_info info(bool main) const;

    // A thread of the viewer's failed: the first failure is why it leaves.
    void fail(std::exception_ptr failure) noexcept;
    std::exception_ptr failure() const;
    // Why it is to leave, other than a failure (the host disconnected it, the
    // stream it can no longer decode): the loop removes it.
    void leave(rcs_result result, const std::string& message);
    bool leaving() const noexcept { return leaving_.load(); }

    // Whether it takes `stream` (its settings' streams; none while it paused video).
    bool takes(uint32_t stream) const noexcept {
        const uint32_t streams = streams_.load();
        return !(paused_.load() & RCS_PAUSE_VIDEO) && (!streams || (streams & (1u << stream)));
    }
    // The streams it asked keyframes for, as (1u << stream) bits.
    uint32_t takeKeyframeRequests() noexcept { return keyframeRequests_.exchange(0); }
    void requestKeyframe(uint32_t stream) noexcept { keyframeRequests_ |= 1u << stream; }
    // It misses a picture of `stream` after `after` (protocol::RefLost), of
    // `encoding`: the earliest since taken, and whether it gave up on one.
    // Pictures of an encoding it no longer gets are no matter.
    void lost(uint32_t stream, uint32_t encoding, uint64_t after, bool gaveUp) noexcept {
        if (encoding != this->encoding.load()) {
            return;
        }
        auto& lost = lost_[stream];
        uint64_t current = lost.after.load();
        while (after < current && !lost.after.compare_exchange_weak(current, after)) {
        }
        lost.gaveUp = lost.gaveUp || gaveUp;
    }
    struct Lost {
        uint64_t after = 0;
        bool gaveUp = false;
    };
    std::optional<Lost> takeLost(uint32_t stream) noexcept {
        auto& lost = lost_[stream];
        const uint64_t after = lost.after.exchange(noLoss);
        const bool gaveUp = lost.gaveUp.exchange(false);
        return after == noLoss ? std::nullopt : std::optional(Lost{after, gaveUp});
    }

    // Under the session's control mutex.
    bool control = false; // The host lets it control.
    uint64_t epoch = 1;   // Input of another epoch was produced under another grant.
    // The loop's: the grant as announced to it.
    std::optional<std::pair<bool, uint64_t>> announced;
    // The streams announced to it, as (1u << stream) bits (under the stream's mutex).
    uint32_t announcedStreams = 0;
    // The loop's: what it was told this end paused (protocol::SharerPaused).
    uint32_t pausedTold = 0;
    // FORWARDER: the newest stream key it says it has (protocol::MediaKeyAck,
    // generation << 32 | step; 0: none yet), and the loop's: when it was last
    // sent the key in use again.
    std::atomic<uint64_t> keyHeld{0};
    Clock::time_point keyResent{};
    // FORWARDER, the loop's: the stream key it was given when let in.
    protocol::MediaKeyAck keyGiven;

    // Its encodings (RCS_EXT_ENCODINGS). The loop's: where the group's
    // policy put it (session/group_plan.hpp). The one its packets are of
    // follows once that one runs, under the set's exclusive lock (the
    // agent's threads read it under the shared one).
    uint32_t assigned = 0;
    std::atomic<uint32_t> encoding{0};
    // The loop's: whether it decodes the main encodings' codec, format and
    // size, and a codec every lower encoding may be in.
    bool decodesMain = true, decodesLower = false;
    // The host's say (rcs_viewer_video), any thread.
    std::atomic<uint32_t> pinned{RCS_ENCODING_ANY};
    std::atomic<uint32_t> pinnedLayers{0};
    std::atomic<bool> counts{true};
    // The temporal layers of its encoding it does not get, from the top
    // (the loop's; read under the stream's mutex), and how many of each
    // stream's it was told it gets (under the stream's mutex).
    std::atomic<uint32_t> dropped{0};
    std::array<uint32_t, RCS_MAX_STREAMS> layersTold{};
    // The loop's: its keyframes by stream (the last it caused, whether a
    // request of its waits for the next), those it caused within a minute,
    // and its drops as last seen.
    std::array<Clock::time_point, RCS_MAX_STREAMS> lastCaused{};
    std::array<bool, RCS_MAX_STREAMS> waits{};
    std::deque<Clock::time_point> caused;
    uint64_t dropsSeen = 0;

    // Its statistics (the connection's sections).
    void updateStats(const std::function<void(Stats&)>& update);
    Stats stats() const;
    // Its sound as negotiated (rcs_stream_info's audio fields).
    void setAudio(rcs_audio_codec codec, uint32_t bitrate);
    std::pair<rcs_audio_codec, uint32_t> audioStream() const;
    protocol::Connection& connection() noexcept { return *connection_; }

private:
    friend class ViewerSet;
    Session& session_;
    std::unique_ptr<protocol::Connection> connection_;

public:
    // P2P and RELAY: restarts its path when it stalls or is lost (rcs_paths_config); none otherwise.
    const std::unique_ptr<PathRestart> restart;

private:
    mutable std::mutex mutex_; // Below.
    ViewerHello hello_;
    rcs_peer_info peer_ = RCS_PEER_INFO_INIT;
    Stats stats_;
    rcs_audio_codec audioCodec_ = 0;
    uint32_t audioBitrate_ = 0;
    std::exception_ptr failure_;
    std::atomic<bool> leaving_{false};
    std::atomic<uint32_t> streams_{0}; // Its settings' (0: all).
    std::atomic<uint32_t> paused_{0};  // Its settings' (RCS_PAUSE_* bits).
    std::atomic<uint32_t> keyframeRequests_{0};
    static constexpr uint64_t noLoss = ~uint64_t(0);
    struct LostAtomic {
        std::atomic<uint64_t> after{noLoss};
        std::atomic<bool> gaveUp{false};
    };
    std::array<LostAtomic, RCS_MAX_STREAMS> lost_;

public:
    // Its parts, in this order: each thread starts once what it uses exists,
    // and stops first.
    DataQueue data;
    Outbox out;
    CursorSender cursor;
    SoundOut audio; // This end's sound to it...
    SoundIn heard;  // ...and its to this end.

private:
    // Once the threads below ended (destroyed after them), no module is active with it.
    struct ModulePeer {
        ModulePeer(Session& s, rcs_viewer_id viewer) : session(s), id(viewer) { session.modules().peerConnected(id); }
        ~ModulePeer() { session.modules().peerGone(id); }
        Session& session;
        rcs_viewer_id id;
    } modulePeer_;
    DataSender dataSender_;
    ViewerMessages messages_;
};

// The run's viewers, in the order they joined. The stream hands them packets
// under the shared lock; a viewer is added and removed under the exclusive
// one, and destroyed after it was removed, by the loop.
class ViewerSet {
public:
    mutable std::shared_mutex mutex;
    std::vector<std::unique_ptr<Viewer>> list;
    rcs_viewer_id nextId = 1;            // Those below were given in this run.
    rcs_viewer_id chosenMain = 0;        // The host's choice (rcs_session_set_main_viewer).
    uint32_t joining = 0;                // Past the handshake, and not in the list yet (waiting ones too)...
    std::map<rcs_viewer_id, protocol::CodecOffer> joiningOffers; // ...and what each of them decodes.
    // The viewers whose host decides later (RCS_ADMIT_LATER), from the call
    // of on_viewer_join on, with the decision once it is made (by
    // rcs_session_admit_viewer); `decided` wakes their joiners.
    std::map<rcs_viewer_id, std::optional<rcs_viewer_decision>> waiting;
    std::condition_variable_any decided;
    rcs_codec streamCodec = 0;           // What the viewers watch now; 0 before a stream started...
    rcs_picture_format streamFormat = 0; // ...in which picture format...
    PictureSize streamSize;              // ...and the largest size of a stream.
    // Each lower encoding's codec and picture format while it runs
    // (RCS_EXT_ENCODINGS): what a viewer moved to it must decode.
    std::array<std::pair<rcs_codec, rcs_picture_format>, RCS_MAX_ENCODINGS> lowerStreams{};
    // Moves on with joins, leaves, the main viewer, a viewer's settings or offer.
    std::atomic<uint64_t> version{0};

    // Holding the mutex.
    Viewer* find(rcs_viewer_id id) const noexcept;
    Viewer* main() const noexcept;
    // Throws INVALID_ARGUMENT for an id never given, INVALID_STATE for one
    // that left; holding the mutex.
    Viewer& present(rcs_viewer_id id) const;
};

} // namespace rcsdk::session
