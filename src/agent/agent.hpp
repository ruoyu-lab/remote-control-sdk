// The agent: the half of a sharer that belongs to the machine's screen, sound
// and input. It captures and encodes the picture, captures the sound the
// machine plays, follows the pointer and the display list, and executes the
// viewer's input. Everything else a sharer does (the connection, identity,
// negotiation, rate control, data channels, reconnecting) is the session's.
//
// A session works with its agent through Port, and hears from it through
// Events. The agent runs in the session's process by default (MediaAgent); a
// host can run it in another process, e.g. one in the user's login session
// beside a service that holds the connection, and the two talk over a local
// link (agent/remote.hpp). Both ways use the same MediaAgent and the same
// interface, so a stream behaves the same wherever its agent runs.
#pragma once

#include "media/cursor.hpp"
#include "media/video.hpp"
#include "platform/platform.hpp"
#include "transport/local_stream.hpp"
#include "util/api.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rcsdk::agent {

// What the machine offers, as its agent reports it: what the session needs to
// check a configuration against and to offer codecs from.
struct Machine {
    // The platform, screen capture, input injection, the audio capture backends
    // and exclusion, and the codec masks (derived from `implementations`).
    rcs_capabilities capabilities = RCS_CAPABILITIES_INIT;
    std::vector<platform::CodecImplementation> implementations;
    // The codecs the host's own encoder handles (RCS_EXT_HOST_ENCODER), as
    // (1u << codec) bits: offered whatever the acceleration and backend, in
    // the picture formats it encodes ((1u << rcs_picture_format) bits).
    uint32_t hostEncoders = 0;
    uint32_t hostEncoderFormats = 0;
    std::vector<rcs_display_info> displays;
    rcs_permission screenPermission = RCS_PERMISSION_UNAVAILABLE;
    // Whether the agent's process may inject input at all. Whether the system
    // takes it at a given moment (not on a secure desktop it does not follow)
    // is Events::inputAvailability.
    rcs_permission inputPermission = RCS_PERMISSION_UNAVAILABLE;
    rcs_audio_capture_backend defaultAudioBackend = RCS_AUDIO_CAPTURE_AUTO;
    uint32_t audioPeriodBackends = 0; // Backends that take a capture period, as (1u << backend) bits.
    // The agent's process, as the operating system reports it: for an agent in
    // another process the session reads it from the link's endpoint, never
    // from what the agent says. Not part of the link's hello.
    transport::LocalPeer process;
};

// This process as a machine: its platform's answers, as they are now.
Machine localMachine();
// A machine and its agent's process as the public interface shows them:
// attached, or why the last agent left (`result` RCS_OK: it closed the link).
rcs_agent_info agentInfo(const Machine& machine, bool attached, rcs_result result = RCS_OK,
                         const std::string& message = {});
// Probing codecs is slow on Windows and the hardware does not change while the
// process runs: probed once.
const std::vector<platform::CodecImplementation>& codecImplementations();

// What restarts the pipeline when it changes.
struct VideoStream {
    rcs_codec codec = RCS_CODEC_H264;
    Picture picture;
    uint32_t width = 0, height = 0, framesPerSecond = 60;
    // The host's own pictures (rcs_capture_config.source HOST): only for an
    // agent in the session's process, so the link does not carry it.
    bool hostFrames = false;
    platform::CaptureTarget target; // Otherwise what is captured.
    bool excludeCurrentProcess = false;
    rcs_acceleration acceleration = RCS_ACCELERATION_HARDWARE; // HARDWARE_FIRST: decided when the stream starts.
    rcs_backend backend = RCS_BACKEND_AUTO;
    rcs_encoder_speed speed = RCS_ENCODER_FASTEST;
    rcs_content_hint content = RCS_CONTENT_AUTO;
    uint32_t softwareThreads = 0;
    bool secureDesktop = false; // Follow the desktop that receives input, secure ones included (Windows).
    uint32_t temporalLayers = 1; // Its encoder makes up to this many (rcs_scalability).
    bool operator==(const VideoStream&) const = default;
};

// A stream's encodings (RCS_EXT_ENCODINGS): encoding 0 brings up the capture
// and the main encoder (stopping the stream's lower encodings, which the
// session starts again for it); a lower one is another encoder fed from that
// capture, at its own size and frame rate, whose capture fields are the main
// one's. A lower encoding that cannot start or fails does not end the
// stream: Events::encodingFailed says why.
struct VideoStart {
    uint32_t index = 0;    // Which stream (rcs_streams_config)...
    uint32_t encoding = 0; // ...and which of its encodings.
    VideoStream stream;
    uint32_t bitrate = 0; // The encoder's first target.
    // Adaptive bitrate: capture is skipped while the encoder's output is over
    // its budget, which turns an encoder that overshoots into a lower frame
    // rate instead of a flooded path.
    bool budgeted = false;
    bool cursorInVideo = true; // Encoding 0: the capture's...
    uint32_t inputBoostMs = 0; // ...and its boost.
};

// The pipeline's reply to a start, before its first packet.
struct VideoStarted {
    uint32_t index = 0;
    uint32_t encoding = 0;
    VideoStream stream;
    Rect content;                  // Where what is shared sits inside the frame.
    platform::DisplayMode display; // What is shared, as the capture sees it.
    // The encoder: the system's (the stream's acceleration, the platform's
    // backend) or the host's own (RCS_BACKEND_HOST, the acceleration it says).
    rcs_acceleration acceleration = RCS_ACCELERATION_HARDWARE;
    rcs_backend backend = RCS_BACKEND_AUTO;
    bool lowLatency = false;
    std::string encoder;
    // An HDR10 stream: the shared display's metadata, as the capture reads it (0 where unknown).
    rcs_hdr_metadata hdr{};
    bool refInvalidation = false; // The encoder drops pictures from its references (invalidateAfter)...
    uint32_t temporalLayers = 1;  // ...and makes these temporal layers.
};

struct CaptureStatus {
    rcs_capture_pause paused = RCS_CAPTURE_PAUSE_NONE;
    uint64_t captured = 0, overwritten = 0, interruptions = 0;
    // Each encoding's time pictures were held back (the bitrate's budget
    // spent, the path behind), in steps of 100 ms; since it started.
    std::array<uint64_t, RCS_MAX_ENCODINGS> limitedMs{};
    bool operator==(const CaptureStatus&) const = default;
};

struct AudioStart {
    rcs_audio_capture_backend backend = RCS_AUDIO_CAPTURE_AUTO;
    bool excludeCurrentProcess = false;
    uint32_t periodFrames = 0;
    // A microphone instead of the machine's sound (platform::MicrophoneConfig).
    bool microphone = false;
    uint32_t device = 0;
    bool voiceProcessing = true;
};

// What the agent tells its session. Each call comes from one of the agent's
// threads: the pipeline's (video, in order), the sound capture's, a watcher's;
// a handler may be called while the session calls the Port.
class Events {
public:
    virtual ~Events() = default;

    // Video, each stream's (`index`, EncodedPacket::stream) and each of its
    // encodings' (EncodedPacket::encoding) in order on a thread of its own:
    // an encoding starts (before any of its packets), its packets, and what
    // changes within it (the capture's content and status: the stream's).
    virtual void videoStarted(const VideoStarted& started) = 0;
    virtual void videoPacket(EncodedPacket&& packet) = 0;
    virtual void videoContent(uint32_t index, const Rect& content, const platform::DisplayMode& display) = 0;
    // The encoder changed how it works (VideoToolbox: to its pipelined encoder).
    virtual void encoderMode(uint32_t index, uint32_t encoding, rcs_codec codec, bool lowLatency, bool refInvalidation,
                             uint32_t temporalLayers, const std::string& name) = 0;
    // A lower encoding could not start, or failed: it ended, and the stream
    // goes on without it.
    virtual void encodingFailed(uint32_t index, uint32_t encoding, rcs_result result, const std::string& message) = 0;
    virtual void captureStatus(uint32_t index, const CaptureStatus& status) = 0;
    // The pointer as stream `index`'s capture sees it: its position on that
    // display and visibility, and its image when that changed.
    virtual void cursor(uint32_t index, const CursorState& state) = 0;

    // The machine.
    virtual void displays(const std::vector<rcs_display_info>& list) = 0;
    // What it shows (the user's desktop, or a secure one): at the first look
    // and whenever that changes.
    virtual void desktopState(const platform::DesktopState& state) = 0;
    // Whether the system accepts injected input (a lock screen or a UAC prompt
    // on the secure desktop does not), reported once input is configured and
    // whenever it changes.
    virtual void inputAvailability(bool available) = 0;
    // The results of input a remote agent executed (kInputDeferred): the last
    // one's, and how many succeeded since the previous report.
    virtual void inputResult(rcs_result last, uint64_t applied) = 0;
    // Modules (Port::startModule): a machine's half runs (RCS_OK), or why it
    // cannot; its messages to the session's half, in order.
    virtual void moduleStatus(const std::string& id, rcs_result result, const std::string& message) = 0;
    virtual void moduleData(const std::string& id, Bytes bytes) = 0;

    // Sound: PCM as captured, and the capture's status (active, or why not).
    virtual void audioPcm(std::span<const float> samples, uint64_t timestampNs, uint32_t periodFrames) = 0;
    virtual void audioStatus(const platform::AudioStatus& status) = 0;

    // The agent cannot go on (capture or encoder failed, input could not be
    // set up). An agent in the session's process ends the run with this
    // result; one in another process leaves with it instead (RemoteAgent's
    // Observer hears that), and the session's handler is not called.
    virtual void failed(rcs_result result, const std::string& message) = 0;
};

// What a session asks of its agent. Thread-safe; none of it blocks on the
// machine: starts and stops are queued, and failures come back as events.
class Port {
public:
    virtual ~Port() = default;

    // The machine as it is now.
    virtual Machine machine() const = 0;

    // Video, each stream (rcs_streams_config) on a pipeline of its own, each
    // of its encodings on a thread of its own (VideoStart). A start while an
    // encoding runs replaces it: what the old encoder still holds is sent
    // first, then videoStarted follows. The first packet of an encoding is a
    // keyframe. Stopping encoding 0 stops the stream.
    virtual void startVideo(const VideoStart& start) = 0;
    virtual void stopVideo(uint32_t index, uint32_t encoding) = 0;
    // The encoder's target from the next frame on, and whether it is budgeted.
    virtual void setRate(uint32_t index, uint32_t encoding, uint32_t bitrate, bool budgeted) = 0;
    // Every stream's.
    virtual void setCursorInVideo(bool inVideo) = 0;
    // For this long after each executed input, capture runs at twice the frame rate (every stream).
    virtual void setInputBoost(uint32_t ms) = 0;
    virtual void requestKeyframe(uint32_t index, uint32_t encoding) = 0;
    // A viewer lost a picture of an encoding of stream `index` after
    // `sequence`: its encoder drops the pictures after it from its
    // references, so that the next packet refers to none of them
    // (EncodedPacket::recoversAfter); one that cannot makes a keyframe instead.
    virtual void invalidateAfter(uint32_t index, uint32_t encoding, uint64_t sequence) = 0;
    // The session's send backlog of each encoding (its viewers'), in
    // nanoseconds of video at the current send rate. While it holds about
    // two frames, that encoding takes no new picture (as a blocking TCP send
    // makes it), and capture waits while every encoding does. Called from
    // the agent's threads; must be cheap.
    virtual void setBacklogSource(std::function<uint64_t(uint32_t encoding)> backlogNs) = 0;

    // Sound: captured while started, delivered as audioPcm.
    virtual void startAudio(const AudioStart& start) = 0;
    virtual void stopAudio() = 0;

    // Input: an injector for `target` and the displays of streams 1, 2, ...
    // (platform::createInputInjector; following secure desktops, or not)
    // replaces the previous one (its held keys are released), and
    // availability is reported at once.
    virtual void configureInput(const platform::CaptureTarget& target, const std::vector<uint32_t>& streamDisplays,
                                bool secureDesktop) = 0;
    // Executes one event. Returns its result; a remote agent returns
    // kInputDeferred and reports results with Events::inputResult.
    virtual rcs_result applyInput(const rcs_input_event& event) = 0;
    virtual void releaseInput() = 0;

    // Modules (rcsdk/module.h): the machine's half of module `id` (of the
    // session's `version`) runs from now on, Events::moduleStatus says
    // whether; it stops; the session's half's messages to it, in order. An
    // agent in another process runs them again when another attaches.
    virtual void startModule(const std::string& id, uint32_t version) = 0;
    virtual void stopModule(const std::string& id) = 0;
    virtual void moduleData(const std::string& id, Bytes bytes) = 0;

    // The calls that wait for the machine. An agent in another process is
    // asked; they throw Failure INVALID_STATE while none is attached (or it
    // leaves before it answers), TIMEOUT when it does not answer in time.
    // The machine's windows now (rcs_list_windows), within a second.
    virtual std::vector<rcs_window_info> windows() = 0;
};

// An executed-input result that follows as an event (a remote agent).
constexpr rcs_result kInputDeferred = 1;

} // namespace rcsdk::agent
