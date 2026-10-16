// Session: lifecycle and shared state behind an rcs_session handle. The share
// and view roles run on a worker thread (share.cpp, view.cpp).
#pragma once

#include "media/video.hpp"
#include "platform/platform.hpp"
#include "protocol/connection.hpp"
#include "protocol/route_demux.hpp"
#include "seams/seams.hpp"
#include "session/checks.hpp"
#include "session/config.hpp"
#include "session/data_channel.hpp"
#include "session/host_sources.hpp"
#include "session/input_queue.hpp"
#include "session/modules.hpp"
#include "session/path_info.hpp"
#include "session/receive_queue.hpp"
#include "session/signals.hpp"
#include "transport/credentials.hpp"
#include "transport/host_channel.hpp"
#include "util/api.hpp"
#include "util/log.hpp"
#include "util/util.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace rcsdk::session {

class IceListener;
class PathRestart;
class ShareAgent;
class ViewerSet;

// A failure the peer, or the path to it, caused, rather than this end: a
// sharer that keeps listening (rcs_reconnect_config) waits for the next viewer
// instead of ending its run.
class PeerFailure : public Failure {
public:
    using Failure::Failure;
};

// The settings as they are now; a new snapshot replaces it with each change.
using LiveConfig = std::shared_ptr<const SessionConfig>;

template <class T>
T sizedStruct() noexcept {
    T value{};
    value.struct_size = sizeof(T);
    return value;
}

// A stream's statistics (rcs_streams_config): `send` is its main
// encoding's, `lower` its lower encodings' (RCS_EXT_ENCODINGS) while they run
// (`encodings`: those that do, as (1u << encoding) bits).
struct VideoStats {
    rcs_capture_stats capture = sizedStruct<rcs_capture_stats>();
    rcs_video_send_stats send = sizedStruct<rcs_video_send_stats>();
    std::array<rcs_video_send_stats, RCS_MAX_ENCODINGS - 1> lower{sizedStruct<rcs_video_send_stats>(),
                                                             sizedStruct<rcs_video_send_stats>()};
    uint32_t encodings = 0;
    rcs_video_receive_stats receive = sizedStruct<rcs_video_receive_stats>();
    rcs_video_send_stats& sendOf(uint32_t encoding) noexcept { return encoding ? lower[encoding - 1] : send; }
};

// A run's statistics, one public structure per section.
struct Stats {
    std::array<VideoStats, RCS_MAX_STREAMS> video; // By stream.
    rcs_network_stats network = sizedStruct<rcs_network_stats>();
    rcs_input_stats input = sizedStruct<rcs_input_stats>();
    rcs_audio_send_stats audioSend = sizedStruct<rcs_audio_send_stats>();
    rcs_audio_receive_stats audioReceive = sizedStruct<rcs_audio_receive_stats>();
    rcs_data_stats data = sizedStruct<rcs_data_stats>();
    std::vector<rcs_module_stats> modules; // By the session's module (ModuleSet), as far as any counted.
    rcs_group_stats group = sizedStruct<rcs_group_stats>();                    // Share: the run's.
    rcs_viewer_video_stats viewerVideo = sizedStruct<rcs_viewer_video_stats>(); // Share: one viewer's.
    rcs_forwarder_stats forwarder = sizedStruct<rcs_forwarder_stats>();         // FORWARDER.
    // The connection's path (rcs_session_get_path_info), as last published,
    // and what it was worked out from (and the networks then: transport::networkGeneration).
    rcs_path_info path{}, standbyPath{};
    protocol::PathEnd pathEnd, standbyEnd;
    uint64_t pathNetworks = 0;
    // The connection's bytes over its path and over its standby path as last
    // published: what came since counts in metered_bytes when it went over a
    // metered network.
    uint64_t trafficSeen = 0, standbySeen = 0;
    uint32_t stallPace = 0; // The pace of the path's stalls as last reported (RCS_EVENT_STALLS); 0: none.
};

// Why the most recent start() or run failed.
struct FailureInfo {
    rcs_result result = RCS_OK;
    std::string message;
};

class Session {
public:
    // `logSource`: what this session's messages name (rcs_log_record).
    Session(SessionConfig config, const rcs_session_callbacks& callbacks, log::SourceRef logSource);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- The public interface (src/api/c_api.cpp) ---------------------------

    rcs_result start();
    // `reason`: what the peer is told (RCS_STOP_RESTART: it will be back).
    void stop(rcs_stop_reason reason = RCS_STOP_NORMAL);
    // rcs_session_update, rcs_session_set_preset and rcs_session_set_identity;
    // throw Failure naming the refused change.
    void update(const rcs_extension* changes);
    void setPreset(rcs_preset preset);
    void setIdentity(const rcs_identity& identity);
    // The settings now (rcs_session_get_settings).
    SessionConfig settings() const;

    rcs_state state() const noexcept { return state_.load(); }
    FailureInfo failure() const;
    Stats stats() const;
    // A stream as last announced (`stream` < RCS_MAX_STREAMS; one not
    // running: RCS_STREAM_INFO_INIT), the run's sound in its audio fields.
    rcs_stream_info streamInfo(uint32_t stream) const;
    rcs_peer_info peerInfo() const;
    rcs_control_info controlInfo() const;
    // Share: the viewers connected now, and the actions on one of them (they
    // throw Failure: INVALID_ARGUMENT for an id this run never gave,
    // INVALID_STATE for a viewer that left).
    std::vector<rcs_viewer_info> viewers() const;
    Stats viewerStats(rcs_viewer_id viewer) const;
    void disconnectViewer(rcs_viewer_id viewer);
    void setViewerControl(rcs_viewer_id viewer, bool allowed);
    void setMainViewer(rcs_viewer_id viewer);
    // rcs_session_set_viewer_video; INVALID_ARGUMENT also for an encoding the
    // settings do not have (RCS_EXT_ENCODINGS).
    void setViewerVideo(rcs_viewer_id viewer, const rcs_viewer_video& video);
    // Share: the decision for a viewer that waits for it (RCS_ADMIT_LATER);
    // INVALID_STATE for one that does not.
    void admitViewer(rcs_viewer_id viewer, const rcs_viewer_decision& decision);
    // Share: the machine the session captures (its agent's), and its displays.
    rcs_agent_info agentInfo() const;
    std::vector<rcs_display_info> displays() const;
    // Share: the windows of the agent's machine (rcs_session_list_windows).
    std::vector<rcs_window_info> windows();
    // What the shared machine shows (a viewer: once the sharer has said).
    rcs_desktop_info desktopInfo() const;
    // rcs_session_signal; throws Failure(INVALID_ARGUMENT).
    void signal(const std::string& text, uint64_t route);
    // rcs_session_transport_receive, rcs_session_transport_closed and
    // rcs_session_transport_restarted; throw Failure naming why.
    void transportReceive(uint64_t route, std::span<const uint8_t> bytes);
    void transportClosed(uint64_t route);
    void transportRestarted(uint64_t route);
    rcs_result sendInput(const rcs_input_event& event);
    rcs_result requestKeyframe();
    // rcs_session_pause and rcs_session_resume: the host's pause of `what`
    // (RCS_PAUSE_* bits) on or off.
    void pause(uint32_t what, bool paused);
    // View: what the sharer paused for its viewers (protocol::SharerPaused);
    // 0 again once the connection ended.
    void peerPaused(uint32_t what);
    // Throw Failure naming why the frame, the sound or the message was refused.
    void pushVideoFrame(const rcs_frame& frame);
    void pushGpuFrame(const rcs_gpu_frame_info& frame);
    void pushSampleBuffer(const void* sampleBuffer, const rcs_sample_buffer_info& info);
    void pushHardwareBuffer(const void* buffer, uint32_t rotation);
    // rcs_session_push_encoded_video and rcs_session_video_ready; throw Failure naming why.
    void pushEncodedVideo(const rcs_encoded_packet& packet);
    rcs_result videoReady();
    void pushAudio(const rcs_audio_frame& frame);
    void sendData(const rcs_data_message& message);
    // The modules the session uses (rcs_module_config).
    ModuleSet& modules() noexcept { return *modules_; }
    // rcs_session_get_module_stats: `viewer`'s (share), or the session's (0).
    rcs_module_stats moduleStats(rcs_viewer_id viewer, int module) const;

    // True while the calling thread runs one of this session's callbacks.
    bool onCallbackThread() const noexcept;
    // Calls one of the host's callbacks with `args` and its user_data. The
    // calling thread counts as this session's callback thread meanwhile, so
    // lifecycle calls the host makes from there are refused instead of
    // deadlocking (rcs_session_callbacks).
    template <class Callback, class... Args>
    auto callHost(Callback callback, Args... args) const {
        return callHostWith(callbacks_.user_data, callback, args...);
    }
    // The same for a callback of an extension, which has its own user_data.
    template <class Callback, class... Args>
    auto callHostWith(void* userData, Callback callback, Args... args) const {
        const CallbackThread mark(*this);
        return callback(args..., userData);
    }

    // ---- Used by the role runners ------------------------------------------

    // Fixed while a run lasts, except the runtime extensions (config.hpp):
    // read those through liveConfig() and the accessors below.
    const SessionConfig& config() const noexcept { return config_; }
    // The latest settings, and a counter that moves on with each change of
    // them during a run.
    LiveConfig liveConfig() const;
    uint64_t settingsVersion() const noexcept { return settingsVersion_.load(); }
    ReceiveLimits receiveLimits() const;
    const rcs_session_callbacks& callbacks() const noexcept { return callbacks_; }
    const log::SourceRef& logSource() const noexcept { return logSource_; }
    // Set by stop() and when a run ends; every blocking call observes it.
    const std::atomic<bool>& cancelled() const noexcept { return cancelled_; }
    // How the run's connection is left when the run ends: told to the peer.
    protocol::Goodbye::Reason leaveReason() const noexcept {
        return leave_ == RCS_STOP_RESTART ? protocol::Goodbye::restarting : protocol::Goodbye::stopped;
    }
    // What this end pauses now (RCS_PAUSE_* bits), the host's and the
    // background's together; each change moves settingsVersion() on. (A
    // viewer's sharer's pause is only reported: RCS_EVENT_PAUSE.)
    uint32_t paused() const noexcept { return paused_.load(); }
    // Ends the current run's blocking calls, e.g. before joining helper threads.
    void cancel() noexcept { cancelled_ = true; }
    // The host asked for a keyframe since the last call (rcs_session_request_keyframe).
    bool takeKeyframeRequest() noexcept { return keyframeRequested_.exchange(false); }
    HostVideo& hostVideo() noexcept { return hostVideo_; }
    EncodedVideo& encodedVideo() noexcept { return encodedVideo_; }
    HostAudio& hostAudio() noexcept { return hostAudio_; }
    // Share: this run's agent, which captures, encodes and executes input.
    ShareAgent& shareAgent() noexcept { return *shareAgent_; }
    // View: the connection's data queue (a sharer's viewers have their own).
    DataQueue& dataQueue() noexcept { return dataQueue_; }
    // A data message from the peer (`viewer`'s: share), to on_data (on the
    // connection's reader thread; calls never overlap).
    void deliverData(uint32_t channel, rcs_data_flags flags, const crypto::Bytes& bytes, rcs_viewer_id viewer = 0);
    // The send queue (`viewer`'s: share) took messages again after refusing one (RCS_EVENT_DATA).
    void dataRoom(uint64_t queuedBytes, rcs_viewer_id viewer = 0);
    // Share: the run's viewers.
    ViewerSet& viewerSet() noexcept { return *viewers_; }
    // A module's event (RCS_EVENT_MODULE), about `viewer`'s connection or none.
    void moduleEvent(rcs_viewer_id viewer, const rcs_module_event& event);

    // `reason`: why the previous connection ended (LISTENING again, RECONNECTING).
    void setState(rcs_state state, rcs_result reason = RCS_OK);
    void holdSystem(rcs_state state);
    // The connection passed the handshake: counted; losing it later may keep
    // the run going (rcs_reconnect_config).
    void connectionEstablished();
    template <class Update>
    void updateStats(Update&& update) {
        std::lock_guard lock(dataMutex_);
        update(stats_);
    }
    // One viewer's statistics (share), or the session's (`viewer` 0); a viewer
    // that left counts no more.
    void updateStats(rcs_viewer_id viewer, const std::function<void(Stats&)>& update);
    // Share: a viewer joined or left (RCS_EVENT_VIEWER).
    void viewerChanged(const rcs_viewer_info& info);
    // A viewer's admission (RCS_EVENT_ADMISSION): share, `viewer`'s; view,
    // this end's (viewer 0).
    void admission(rcs_admission_state state, rcs_viewer_id viewer, uint32_t timeoutMs = 0, rcs_result result = RCS_OK,
                   const std::string& message = {});
    // Share: a PIN handshake failed (RCS_EVENT_PIN, pin_failures).
    void pinFailed(uint32_t failures, uint32_t left);
    // A viewer left: its counters stay the run's, and its measurements while
    // it was the main viewer, and its final statistics stay readable
    // (viewerStats) (holding the set's exclusive lock).
    void viewerLeft(rcs_viewer_id viewer, const Stats& stats, bool main);
    // A stream's picture (info.stream; the audio fields are left as they
    // are), and the end of one (it stopped running), and the sound (share:
    // `viewer`'s, the main viewer's being the run's): kept for streamInfo()
    // and reported with what changed (RCS_EVENT_STREAM; the sound with stream 0).
    void setVideoStream(const rcs_stream_info& info);
    void videoStreamEnded(uint32_t stream);
    void setAudioStream(rcs_audio_codec codec, uint32_t bitrate, rcs_viewer_id viewer = 0);
    // The peer's platform and offers (share: `viewer`'s, the main viewer's
    // being peerInfo()); reported (RCS_EVENT_PEER).
    void setPeer(const rcs_peer_info& peer, rcs_viewer_id viewer = 0);
    // Share: the main viewer changed; its peer and sound are the session's now.
    void mainViewerChanged();
    // The device key a sharer proved before this end refused it (no event).
    void setRefusedPeerKey(const crypto::PublicKey& key);
    // Share: an agent attached with this machine, or left and why (RCS_OK: it
    // closed the link); its displays changed. Reported (RCS_EVENT_AGENT,
    // RCS_EVENT_DISPLAYS).
    void agentAttached(const agent::Machine& machine);
    void agentDetached(rcs_result result, const std::string& message);
    void agentDisplays(const std::vector<rcs_display_info>& list);
    // An agent's machine as the host sees it: with the input types the host
    // executes beside it (rcs_control_config.host_input_types). Not while
    // holding dataMutex_.
    rcs_agent_info agentInfoOf(const agent::Machine& machine, bool attached, rcs_result result = RCS_OK,
                               const std::string& message = {}) const;
    // What the shared machine shows changed (RCS_EVENT_DESKTOP); the same
    // value again is no news.
    void desktopChanged(const platform::DesktopState& state);
    // The machine the settings are checked against: this system's without a
    // remote agent, the attached agent's with one (none before it attached).
    std::optional<agent::Machine> machineForChecks() const;
    // Throws Failure naming the setting an attached agent's machine cannot carry out.
    void checkMachine(const agent::Machine& machine);
    // PASSWORD: this run's salt and derivations.
    const std::shared_ptr<transport::PasswordKeys>& passwordKeys() const noexcept { return passwordKeys_; }
    // PIN, share: this run's failed PIN handshakes, counted afresh when the
    // host sets RCS_EXT_IDENTITY.
    const std::shared_ptr<transport::PinAttempts>& pinAttempts() const noexcept { return pinAttempts_; }
    // FORWARDER: how many times this session set out to reach the forwarder
    // before (rcs_forwarder_config.refresh_token is asked before each but the first).
    uint64_t forwarderAttempt() noexcept { return forwarderAttempts_++; }
    // Hands a frame to the host's frame callback, if it set one.
    void deliver(const VideoFrame& frame);

    // ---- Events (on_event), never overlapping ------------------------------

    // An encoder or decoder opened, or the encoder changed its mode: recorded
    // in the statistics and reported.
    struct CodecInfo {
        uint32_t stream = 0;
        rcs_codec codec = 0;
        rcs_acceleration acceleration = RCS_ACCELERATION_HARDWARE;
        rcs_backend backend = RCS_BACKEND_AUTO;
        bool lowLatency = false;      // Encoders: one frame at a time...
        bool refInvalidation = false; // ...dropping missed pictures from their references...
        std::string name;
        uint32_t temporalLayers = 0;  // ...and the temporal layers they make (decoders: 0).
    };
    void codecChanged(const CodecInfo& info);
    // Share: a lower encoding of `stream` started, stopped or could not
    // (RCS_EVENT_ENCODING), and a viewer got another encoding
    // (RCS_EVENT_VIEWER_VIDEO); both logged.
    void encodingChanged(uint32_t stream, const rcs_encoding_event& encoding);
    void viewerVideoChanged(rcs_viewer_id viewer, const rcs_viewer_video_event& video);
    void captureChanged(uint32_t stream, rcs_capture_pause paused);
    void displaysChanged(uint32_t count);
    // View: reports controlInfo() as it is now.
    void controlChanged();
    // Share: one viewer's control as it was announced to it.
    void viewerControlChanged(rcs_viewer_id viewer, const rcs_control_info& info);
    // `stream`'s; `viewer`: the viewer that asked (share), 0 for the host or the interval.
    void recovering(uint32_t stream, rcs_recovery_reason reason, rcs_viewer_id viewer = 0,
                    rcs_recovery_method method = RCS_RECOVERY_KEYFRAME, uint64_t frozenNs = 0);
    // Capture source ENCODED: the host's next packet is to be a keyframe (stream 0).
    void keyframeDue(rcs_keyframe_reason reason, rcs_viewer_id viewer = 0);
    // Sound one way started, paused or stopped (RCS_EVENT_AUDIO): to or from
    // `viewer` (a sharer's; a viewer's sharer: 0), in `codec` while active.
    void audioChanged(const platform::AudioStatus& status, rcs_audio_direction direction, rcs_viewer_id viewer,
                      rcs_audio_codec codec);
    // Adaptive bitrate moved the encoder's target, to what `viewer`'s path
    // carries (`status`).
    void rateChanged(const protocol::Connection::PathStatus& status, rcs_viewer_id viewer);
    // Each report adaptive bitrate decides on (UdpOptions::decideRate), of
    // report.viewer_id's connection: the host's on_rate_report may decide
    // instead (one report at a time); both are counted.
    void rateReport(const rcs_rate_report& report, rcs_rate_decision& decision);
    // Publishes the transport's view of the path (share: `viewer`'s) in the
    // network statistics. When the pace of its short stalls began, changed by
    // a tenth or more, or stopped: reported (RCS_EVENT_STALLS), and what the
    // peer is to be told.
    std::optional<protocol::PathStalls> publishPath(const protocol::Connection::PathStatus& status,
                                                    rcs_viewer_id viewer = 0);
    // The peer (share: `viewer`) said what it sees of the path's stalls.
    void peerStalls(const protocol::PathStalls& stalls, rcs_viewer_id viewer = 0);
    // The connection's path changed (share: `viewer`'s): reported (RCS_EVENT_PATH).
    void pathChanged(const protocol::PathEvent& event, rcs_viewer_id viewer = 0);
    // rcs_session_get_path_info: the active and standby paths of `viewer`'s
    // connection (share; 0: the main viewer's) or this end's (view). Throws
    // Failure: INVALID_STATE without a connection, INVALID_ARGUMENT for an id
    // this run never gave.
    std::pair<rcs_path_info, rcs_path_info> pathInfo(rcs_viewer_id viewer) const;
    // View: the running connection lost its path (RCS_PATH_LOST) at
    // `brokeAt` and is kept: RECONNECTING, the reconnect window counted from
    // then; back (RUNNING) once a path is.
    void pathLost(Clock::time_point brokeAt);
    void pathBack();
    // The network this end's sockets use now (NetworkChoice), and the
    // interface a new socket binds to (0: none; the test's networks are no
    // interfaces). Throws Failure(NETWORK) when no network is left to use.
    NetworkChoice::Choice networkChoice();
    uint32_t boundInterface();
    // The interface a standby path's socket binds to (0: none).
    uint32_t standbyInterface();
    // View: the sharer's pointer moved, changed or hid (RCS_EVENT_CURSOR).
    void cursorChanged(const rcs_cursor_event& cursor);

    // P2P and RELAY: hands this end's signal to the host, for `route` (a
    // sharer's answer: the route of the viewer signal it answers); the calls
    // never overlap.
    void emitSignal(const std::string& text, uint64_t route);
    // View: waits (cancellable; while reconnecting, until the reconnect
    // window closes) for the sharer's answer to this connection's signal,
    // whose ufrag is `ufrag`; takes another answer to it the host delivered
    // since, if any.
    transport::ice::Description waitForAnswer(const std::string& ufrag);
    std::optional<transport::ice::Description> takeAnswer(const std::string& ufrag);
    // Share, P2P and RELAY: the run's answers to viewers' signals, from its
    // start to its end.
    IceListener& iceListener() noexcept { return *iceListener_; }
    // P2P and RELAY: the running connections' restarts of their paths, which
    // take the peer's restart signals (rcs_session_signal), from their start
    // to their end.
    void addRestart(PathRestart& restart);
    void removeRestart(PathRestart& restart);
    // HOST: the run's channel, from start() to the end of the run: a
    // viewer's to the sharer, a sharer's with a route to each viewer; and
    // FORWARDER's link HOST: either end's to the forwarder (its hop).
    std::shared_ptr<transport::HostChannel> hostChannel() const;
    std::shared_ptr<protocol::RouteDemux> hostRoutes() const;

    // Marks the calling thread as running one of this session's callbacks.
    class CallbackThread {
    public:
        explicit CallbackThread(const Session& session);
        ~CallbackThread();

    private:
        const Session* previous_;
    };

    // Share: remote-control state shared by the stream's loop and the viewers'
    // input threads. A viewer may control while the host allows it
    // (rcs_control_config, on_viewer_join, rcs_session_set_viewer_control:
    // Viewer::control) and, for input the system executes, the OS accepts
    // injected input; a lock screen or secure desktop pauses the grant without
    // touching the host's setting. Every change of a viewer's grant gets it a
    // new epoch (Viewer::epoch), under this mutex.
    struct ShareControl {
        mutable std::mutex mutex;
        bool allowed = false;  // The host's setting: what a viewer gets when it joins.
        bool available = true; // The executor accepts input right now.
        rcs_input_target target = RCS_INPUT_SYSTEM;
        uint32_t hostTypes = 0;     // Target SYSTEM: the types the host executes (host_input_types)...
        bool systemExecutes = true; // ...and whether the system executes any type.
        std::unique_ptr<platform::InputInjector> injector;
        bool hostExecutes = false; // The injector is the host's (target HOST applied).
        std::atomic<bool> hostRelease{false};     // The host executes input: a release-all is due...
        std::atomic<rcs_viewer_id> releaseFor{0}; // ...for this viewer (0: none in particular).
        bool granted(bool control) const noexcept { return control && available && injector; }
        // Whether the host executes this type of input (RELEASE_ALL goes to both).
        bool byHost(rcs_input_type type) const noexcept {
            return hostExecutes || (type != RCS_INPUT_RELEASE_ALL && type < 32 && (hostTypes & (1u << type)));
        }
        // Releases what the system and the host hold (they got input only
        // while there was an injector), for `viewer`: whose a held key is
        // cannot be told, so it is everything; holding the mutex.
        void release(rcs_viewer_id viewer) noexcept {
            if (!injector) {
                return;
            }
            releaseFor = viewer;
            injector->releaseAll();
            if (hostTypes) {
                hostRelease = true;
            }
        }
    };
    ShareControl& shareControl() noexcept { return shareControl_; }
    // on_input, if set (false without it), after a release-all that is due
    // (target HOST); calls never overlap. Without the control lock held.
    bool callOnInput(const rcs_input_event& event);
    // Target HOST: hands a due release-all to on_input.
    void deliverHostRelease();

    // View: remote-control state reported by the sharer and outgoing input.
    struct ViewControl {
        std::atomic<bool> granted{false};
        std::atomic<uint64_t> epoch{0};
        InputQueue inputs;
    };
    ViewControl& viewControl() noexcept { return viewControl_; }

private:
    void run() noexcept;
    // Whether the run goes on after `failure` ended its connection (the
    // sharer listens again, the viewer reconnects: rcs_reconnect_config), and
    // the state it reports once that connection is released and forgotten
    // (none: the state stays).
    struct Next {
        bool resume = false;
        std::optional<rcs_state> report;
        rcs_result result = RCS_OK;
    };
    Next resumeAfter(const Failure& failure);
    // Releases what the connection that ended held: input, control.
    void releaseConnection();
    void join();
    void emit(rcs_event& event);
    rcs_event streamEvent(uint32_t stream, uint32_t changes) const; // Holding dataMutex_.
    void clearStreams() noexcept;                                    // Holding dataMutex_.
    // Replaces the settings with `next` (holding liveMutex_ in `live`), whose
    // extensions of `changed` the host changed; running, as far as they may.
    void commit(std::unique_lock<std::mutex>& live, const SessionConfig& next, uint32_t changed);
    void applyControl();               // Holding liveMutex_.
    // The viewers' offers, for checking a change against (holding liveMutex_).
    PeerOffers peerOffers() const;
    void countDelivered(uint32_t stream);
    bool running() const noexcept;
    // Works out what is paused from the host's pause, the background and
    // rcs_background_config; reports a change while running (RCS_EVENT_PAUSE),
    // or what is paused when a run starts (`starting`). Not holding liveMutex_.
    void pauseChanged(bool starting = false);

    SessionConfig config_; // Changes while running only under liveMutex_, in the runtime extensions.
    const rcs_session_callbacks callbacks_;
    const log::SourceRef logSource_; // Its level follows config_.log.
    mutable std::mutex liveMutex_;
    LiveConfig live_;       // config_ as of its last change, under liveMutex_.
    std::atomic<uint64_t> settingsVersion_{0};
    std::mutex eventMutex_;
    // The thread inside on_event, and the events raised on it meanwhile (a
    // host's call from there, e.g. a module's function), delivered once that
    // on_event returned; both used under eventMutex_ by its holder only.
    std::atomic<std::thread::id> eventThread_{};
    struct HeldEvent {
        rcs_event event{};
        std::string module;     // MODULE: what event.module.module points to...
        Bytes payload;          // ...and its payload.
        rcs_cursor_shape shape{}; // CURSOR: the image, with its pixels.
        Bytes pixels;
    };
    std::deque<HeldEvent> heldEvents_;
    std::mutex inputCallbackMutex_;
    std::mutex dataCallbackMutex_; // on_data, from several viewers' readers.
    std::mutex rateCallbackMutex_; // on_rate_report, from several viewers' connections.
    std::mutex dataSendMutex_;     // Share: a message for every viewer is queued for all at once.

    std::mutex lifecycleMutex_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<rcs_stop_reason> leave_{RCS_STOP_NORMAL};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> keyframeRequested_{false};
    std::atomic<rcs_state> state_{RCS_STATE_IDLE};
    std::mutex systemMutex_;
    std::shared_ptr<void> wifi_;     // The system's Wi-Fi out of power saving (platform::lowLatencyWifi).
    std::shared_ptr<void> networks_; // The system's changes of its networks watched (platform::watchNetworks).
    std::shared_ptr<void> responsive_; // The process kept prompt (platform::stayResponsive).
    NetworkChoice networkChoice_;
    bool connected_ = false;            // Worker thread: this connection passed the handshake.
    Clock::time_point lostAt_{};        // Worker thread, view: when the running connection broke.
    std::optional<Clock::time_point> pathLostAt_; // Worker thread, view: when the kept connection's path broke.
    bool reconnectWait_ = true;         // Worker thread, view: wait reconnect_interval_ms before the next attempt.
    std::mutex reportedMutex_;
    rcs_control_info reportedControl_ = RCS_CONTROL_INFO_INIT; // Under reportedMutex_: as last reported.

    mutable std::mutex dataMutex_;
    Stats stats_;
    // The final statistics of the viewers that left this run, the latest
    // departedKept (rcs_session_get_viewer_stats answers for them too).
    static constexpr size_t departedKept = 256;
    std::deque<std::pair<rcs_viewer_id, Stats>> departed_;
    std::array<rcs_stream_info, RCS_MAX_STREAMS> streams_; // As announced; the sound in stream 0's.
    rcs_peer_info peer_ = RCS_PEER_INFO_INIT;
    rcs_desktop_info desktop_ = RCS_DESKTOP_INFO_INIT;
    std::optional<agent::Machine> agentMachine_; // The remote agent that attached last.
    bool agentAttached_ = false;
    FailureInfo agentLeft_; // Why it left.
    FailureInfo failure_;

    std::mutex signalMutex_;
    std::condition_variable signalArrived_;
    // Share: viewers' signals that came while no run listened, for the next
    // run (the latest maxEarlySignals), and the run's listener, which takes
    // the rest.
    static constexpr size_t maxEarlySignals = 64;
    std::deque<ViewerSignal> earlySignals_;
    std::unique_ptr<IceListener> iceListener_;
    Answers answers_;                                 // View.
    std::vector<PathRestart*> restarts_;              // Of the running connections.
    std::optional<Clock::time_point> signalDeadline_; // View, reconnecting: the window's end.
    std::mutex signalCallbackMutex_;                  // on_signal, from several attempts.

    // A sharer over HOST has a route per viewer; the others one channel.
    bool routedChannel() const noexcept {
        return config_.connect.transport == RCS_TRANSPORT_HOST && config_.role == RCS_ROLE_SHARE;
    }
    mutable std::mutex hostMutex_;
    std::shared_ptr<transport::HostChannel> hostChannel_; // View, and a forwarder's hop.
    std::shared_ptr<protocol::RouteDemux> hostRoutes_;    // Share.

    ShareControl shareControl_;
    std::unique_ptr<ViewerSet> viewers_; // Share.
    ViewControl viewControl_;
    HostVideo hostVideo_;
    EncodedVideo encodedVideo_;
    HostAudio hostAudio_;
    DataQueue dataQueue_;
    std::shared_ptr<transport::PasswordKeys> passwordKeys_; // Replaced at each start.
    std::shared_ptr<transport::PinAttempts> pinAttempts_;   // Replaced at each start, under liveMutex_.
    std::atomic<uint64_t> forwarderAttempts_{0};
    std::mutex agentMutex_;                  // shareAgent_, for the host's threads (windows()).
    std::unique_ptr<ShareAgent> shareAgent_; // Share: from the run's start to its end.
    std::unique_ptr<ModuleSet> modules_;     // Its machine's halves' threads call into the rest.

    // Pausing (rcs_session_pause, rcs_background_config, a viewer's sharer's).
    std::recursive_mutex pauseMutex_; // pauseChanged(); a host's call from on_event may enter it again.
    std::atomic<uint32_t> pausedByHost_{0};
    std::atomic<uint32_t> pausedByPeer_{0};
    std::atomic<bool> background_{false}; // The application is in the background.
    std::atomic<uint32_t> paused_{0};
    rcs_pause_event pauseReported_{}; // As last worked out, under pauseMutex_.
    // Last: the application's state calls backgroundChanged until it is released.
    std::unique_ptr<seams::AppStateWatch> appState_;
};

void runShare(Session& session);
void runView(Session& session);

} // namespace rcsdk::session
