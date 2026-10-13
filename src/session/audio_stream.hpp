// Sound within a run, both ways: the sharer's machine to each viewer, and a
// viewer's (its microphone, or its host's sound) to the sharer. The sending
// end captures, encodes and sends; the receiving end decodes, conceals what
// was lost, and plays it or hands it to the host. Problems with sound never
// end the run: they pause that direction and are reported
// (Session::audioChanged), while video and control go on.
//
// SoundOut and SoundIn own one direction of a connection's sound, each end
// one of each per connection: a direction flows while its sender sends and
// its receiver receives, and restarts (a new AudioFormat) when either end's
// settings change; AudioOff tells the receiver it stopped and why. An end
// captures once for all its connections (AudioHub) and encodes for each in the
// format it negotiated; through a forwarder, a sharer encodes once per format
// for every viewer that takes it (SoundGroups).
#pragma once

#include "audio/codec.hpp"
#include "audio/playout.hpp"
#include "protocol/connection.hpp"
#include "session/session.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace rcsdk::session {

// The sound captured once for every viewer's sender: the first sender that
// starts starts the capture (the agent's, or the host's pushed sound), the
// last that stops stops it, and each gets what is captured while it runs. A
// start with other capture settings restarts the capture for every sender
// (they restart too: the settings are the run's).
class AudioHub {
public:
    explicit AudioHub(Session& session) : session_(session) {}
    ~AudioHub();
    AudioHub(const AudioHub&) = delete;
    AudioHub& operator=(const AudioHub&) = delete;

    // What a sender captures: the host's pushed sound, the machine's (the
    // agent's, by `backend`), or a microphone (a sharer's agent's; a viewer's own).
    struct Source {
        rcs_audio_source source = RCS_AUDIO_SYSTEM;
        rcs_audio_capture_backend backend = RCS_AUDIO_CAPTURE_AUTO;
        uint32_t device = 0;
        bool voiceProcessing = true;
        bool operator==(const Source&) const = default;
    };
    // One sender's capture.
    std::unique_ptr<platform::AudioCapture> capture(const Source& source);

private:
    class Share;
    struct Settings {
        Source source;
        bool excludeCurrentProcess = false;
        uint32_t periodFrames = 0;
        bool operator==(const Settings&) const = default;
    };
    struct Subscriber {
        const Share* share;
        platform::AudioCapture::Deliver deliver;
        platform::AudioCapture::Status status;
    };
    void subscribe(const Share& share, const Settings& settings, platform::AudioCapture::Deliver deliver,
                   platform::AudioCapture::Status status);
    void unsubscribe(const Share& share) noexcept;
    uint32_t periodFrames() const noexcept;
    void deliver(std::span<const float> samples, uint64_t timestampNs);
    void status(const platform::AudioStatus& status);

    Session& session_;
    mutable std::mutex captureMutex_; // Starting and stopping the capture.
    std::unique_ptr<platform::AudioCapture> capture_;
    Settings running_;
    std::mutex mutex_; // The subscribers; held while delivering to them.
    std::vector<Subscriber> subscribers_;
    std::optional<platform::AudioStatus> status_; // The capture's latest, for a sender that joins.
};

// Encodes and sends what capture delivers, on a thread of its own: capture
// threads (often the system's real-time audio threads, or the host's in
// rcs_session_push_audio) only hand the samples over and never wait for the
// encoder or the network. A backlog beyond maxBacklogMs (the path or the
// encoder cannot keep up) drops the oldest sound. An encoder that fails pauses
// the sound with the reason (RCS_EVENT_AUDIO) until the sound restarts.
class AudioSender {
public:
    using Send = std::function<void(protocol::Message)>;
    // Announces the format, then captures until destroyed: each message to
    // `send` (on the sender's thread; it throws as a connection's send does),
    // whose path may lose packets (`lossy`: they then carry the one before,
    // as the tuning says); of `viewer` (a sharer's; a viewer's sharer, or a
    // sharer's sound for several viewers: 0).
    AudioSender(Session& session, Send send, bool lossy, AudioHub& hub, rcs_audio_codec codec, uint32_t bitrate,
                const rcs_send_audio_config& settings, const rcs_audio_tuning_config& tuning, rcs_viewer_id viewer);
    ~AudioSender();
    AudioSender(const AudioSender&) = delete;
    AudioSender& operator=(const AudioSender&) = delete;

private:
    static constexpr uint32_t maxBacklogMs = 200;
    void captured(std::span<const float> samples, uint64_t timestampNs); // Capture threads.
    void run() noexcept;                                                  // The sender's thread.
    void encode(std::vector<float>& pending, uint64_t& startNs);

    Session& session_;
    const Send send_;
    const rcs_viewer_id viewer_;
    std::unique_ptr<audio::Encoder> encoder_;
    AudioFormat format_; // Announced first, by the sender's thread.
    std::unique_ptr<platform::AudioCapture> capture_;
    uint32_t sequence_ = 0;
    const bool redundant_; // Each packet carries the one before (rcs_audio_tuning_config.redundancy).
    uint32_t previousFrames_ = 0;
    std::vector<uint8_t> previous_;

    std::mutex mutex_;
    std::condition_variable arrived_;
    std::vector<float> input_; // Delivered, not yet taken by the sender.
    uint64_t inputStartNs_ = 0;
    uint64_t droppedFrames_ = 0;
    bool gap_ = false; // Sound was dropped since the sender last took any.
    bool stopping_ = false;
    std::thread thread_;
};

// This end's settings of the sound it receives.
struct ReceivedSound {
    rcs_receive_audio_config audio;
    rcs_audio_tuning_config tuning;
};

class AudioReceiver {
public:
    // Decodes and plays `format` from `viewer` (a sharer's; a viewer's
    // sharer: 0) with this end's `settings`, after handing each packet to the
    // host (RCS_EXT_ENCODED_OUTPUT); with a viewer's decode 0, only hands them.
    AudioReceiver(Session& session, const AudioFormat& format, const ReceivedSound& settings, rcs_viewer_id viewer);
    ~AudioReceiver();
    AudioReceiver(const AudioReceiver&) = delete;
    AudioReceiver& operator=(const AudioReceiver&) = delete;

    void packet(const AudioPacket& packet, uint64_t arrivalNs);
    // The path changed, or came back after a gap (audio::Playout::discontinuity).
    void discontinuity() noexcept;

private:
    void deliver(std::span<const float> samples, uint64_t captureNs, uint64_t arrivalNs);
    void publish();

    Session& session_;
    const AudioFormat format_;
    const rcs_viewer_id viewer_;
    const bool decode_;
    std::unique_ptr<audio::Decoder> decoder_;
    std::unique_ptr<audio::Playout> playout_;
    std::unique_ptr<platform::AudioPlayer> player_;
    std::optional<uint32_t> expected_; // The next sequence number.
    uint32_t lastFrames_ = 0;          // Opus packets vary with what capture delivers.
    std::vector<float> decoded_;
    uint64_t delivered_ = 0;
    uint64_t lost_ = 0, recovered_ = 0, concealedFrames_ = 0;
};

// What one direction's sound is: its codec and bitrate, whether each packet
// carries the one before, and what is captured.
struct SoundPlan {
    rcs_audio_codec codec = 0; // 0: no sound.
    uint32_t bitrate = 0;
    bool redundant = false;
    AudioHub::Source source;
    uint32_t excludeCurrentProcess = 0, capturePeriodFrames = 0;
    protocol::AudioOff::Reason reason = protocol::AudioOff::senderOff; // Without sound: why.
    bool operator==(const SoundPlan&) const = default;
};

// Through a forwarder (RCS_TRANSPORT_FORWARDER), a sharer's sound to its
// viewers: one sender per plan for every viewer that takes it, which hands
// each packet to `publish` with the plan's number (1..255): sealed with the
// stream key and sent once (session/forwarder.hpp); the forwarder sends the
// packets to the viewers subscribed to that number. Each viewer hears the
// format end to end first. A plan's sender starts with its first viewer and
// stops with its last.
class SoundGroups {
public:
    using Publish = std::function<void(uint8_t sound, const AudioPacket& packet)>;
    SoundGroups(Session& session, AudioHub& hub, Publish publish);
    ~SoundGroups();
    SoundGroups(const SoundGroups&) = delete;
    SoundGroups& operator=(const SoundGroups&) = delete;

    class Group;
    // A viewer takes `plan`: `announce` hears its format (at once when it is
    // known), on the sender's thread or this one; the viewer is in the group
    // while it holds the result. Throws BUSY when 255 plans run at once.
    struct Member {
        std::shared_ptr<Group> group;
        uint64_t id = 0;
        ~Member();
    };
    std::unique_ptr<Member> join(const SoundPlan& plan, const SessionConfig& live,
                                 std::function<void(const AudioFormat&)> announce);
    static uint8_t number(const Member& member) noexcept;

private:
    Session& session_;
    AudioHub& hub_;
    const Publish publish_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<Group>> groups_;
};

// Sends this end's sound to the other end of a connection while this end
// sends and the other end receives, in a codec both have (a viewer may ask
// its sharer for one, and a bitrate). `viewer`: the connection's (a sharer's;
// a viewer's: 0). `post`: the connection's ordered messages (AudioOff).
// `groups`: a sharer's through a forwarder, which encode for several viewers.
class SoundOut {
public:
    SoundOut(Session& session, protocol::Connection& connection, AudioHub& hub, rcs_viewer_id viewer,
             std::function<void(protocol::Message)> post, SoundGroups* groups = nullptr);
    // This end's settings, and the other end's: whether it receives and the
    // codecs it takes, and what a viewer asks for (a sharer's only): starts,
    // restarts or stops the sound.
    void apply(const SessionConfig& live, bool receives, const std::vector<rcs_audio_codec>& codecs,
               const protocol::StreamRequest* request = nullptr);
    // What the sound takes on the path now; 0 while none flows.
    uint32_t wireBitrate() const noexcept;
    // Through a forwarder: the number of the sound it takes (0: none).
    uint8_t sound() const noexcept { return sound_.load(); }

private:
    SoundPlan plan(const SessionConfig& live, bool receives, const std::vector<rcs_audio_codec>& codecs,
                   const protocol::StreamRequest* request) const;

    Session& session_;
    protocol::Connection& connection_;
    AudioHub& hub_;
    const rcs_viewer_id viewer_;
    const std::function<void(protocol::Message)> post_;
    SoundGroups* const groups_;
    std::optional<SoundPlan> current_;
    std::optional<AudioSender> sender_;
    std::unique_ptr<SoundGroups::Member> member_;
    std::atomic<uint8_t> sound_{0};
};

// Plays the other end's sound while this end receives, for each format it
// announces; from `viewer` (a sharer's; a viewer's sharer: 0). Decoding, the
// output device and on_audio run on the sound's own thread, so neither the
// connection's reader nor the input sender waits for a device to open or for
// the host's on_audio.
class SoundIn {
public:
    SoundIn(Session& session, const ReceivedSound& settings, rcs_viewer_id viewer);
    ~SoundIn();
    SoundIn(const SoundIn&) = delete;
    SoundIn& operator=(const SoundIn&) = delete;
    // The other end's messages (reader thread). Throws Failure(PROTOCOL) for
    // a format of a codec this end never offered during the run.
    void format(const AudioFormat& format);
    void packet(const AudioPacket& packet, uint64_t arrivalNs);
    void off(protocol::AudioOff::Reason reason);
    // This end's settings changed while running.
    void apply(const ReceivedSound& settings);
    // The connection's path changed, or came back after a gap (rcs_paths_config).
    void discontinuity();

private:
    struct Arrived {
        AudioPacket packet;
        uint64_t arrivalNs = 0;
    };
    struct Discontinuity {};
    using Command = std::variant<AudioFormat, Arrived, protocol::AudioOff::Reason, ReceivedSound, Discontinuity>;
    // Packets beyond this wait behind a stalled thread are dropped (and concealed).
    static constexpr size_t maxQueuedPackets = 100;

    void post(Command command);
    void run() noexcept;
    void handle(Command& command);
    void open(); // The sound's thread.
    void stream(rcs_audio_codec codec, uint32_t bitrate); // A viewer's: the stream's sound (rcs_stream_info).

    Session& session_;
    const rcs_viewer_id viewer_;
    std::atomic<uint32_t> offered_; // Audio codecs this end offered during the run, as (1u << codec) bits.

    std::mutex mutex_;
    std::condition_variable posted_;
    std::deque<Command> commands_;
    size_t queuedPackets_ = 0;
    bool stopping_ = false;

    // The sound's thread.
    ReceivedSound settings_;
    std::optional<AudioFormat> format_;
    std::unique_ptr<AudioReceiver> receiver_;
    std::thread thread_;
};

} // namespace rcsdk::session
