// The host's own picture, sound and input handling (capture source HOST,
// audio source HOST, input target HOST), and its own bitstream (capture source
// ENCODED). The host pushes frames and sound through the session; HostVideo
// and HostAudio hand them to the share loop and the audio sender behind the
// same interfaces as the platform's capture, so nothing downstream tells them
// apart; EncodedVideo hands packets to the share loop as an agent's would.
// HostInput "injects" by leaving the input to on_input.
#pragma once

#include "agent/video_pipeline.hpp"
#include "audio/mixer.hpp"
#include "platform/platform.hpp"
#include "util/api.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace rcsdk::session {

class HostVideo {
public:
    // A capture taking the pushed frames, of the size it is started with.
    std::unique_ptr<platform::ScreenCapture> capture();
    // Throws Failure: INVALID_STATE while no stream takes frames,
    // INVALID_ARGUMENT for another size or format.
    void push(const rcs_frame& frame);
    // The same for a GPU image of the host's (rcs_session_push_gpu_frame);
    // UNSUPPORTED where this system takes none of its kind.
    void push(const rcs_gpu_frame_info& frame);
    // Ready for the system's own images (platform::hostImages()): throws
    // Failure(UNSUPPORTED) on systems without them, before any check of the state.
    void prepareSamples();
    // Such an image of any size (a CMSampleBuffer's picture, an
    // AHardwareBuffer), turned upright and fitted into the stream
    // (rcs_session_push_sample_buffer, rcs_session_push_hardware_buffer):
    // where it lies and its own size go with it.
    void pushSample(const void* image, uint32_t orientation);

private:
    friend class HostVideoCapture;
    // The newest picture now (holding mutex_): where it lies in the frame and,
    // where known, its size before it was fitted.
    void keep(VideoFrame frame, const Rect& content, const platform::DisplayMode& display);
    std::mutex mutex_;
    std::condition_variable arrived_;
    uint32_t width_ = 0, height_ = 0; // Expected; 0 while no capture runs.
    std::optional<VideoFrame> latest_;
    Rect latestContent_;
    platform::DisplayMode latestDisplay_;
    std::mutex fitting_; // One sample buffer at a time.
    std::unique_ptr<platform::SamplePictures> samples_;
    bool interrupted_ = false;
    uint64_t pushed_ = 0, replaced_ = 0, sequence_ = 0;
};

class EncodedVideo {
public:
    using Deliver = std::function<void(EncodedPacket&&)>;

    // A stream starts: packets of its codec and size go to `deliver`, a
    // keyframe first. `backlog`: the video waiting in the connection, as time.
    void start(const agent::VideoStream& stream, Deliver deliver, std::function<uint64_t()> backlog);
    // No packet is taken after this returns, until the next start.
    void stop();
    // The target, and whether packets count against it (adaptive bitrate).
    void setRate(uint32_t bitrate, bool budgeted);
    // Throws Failure: INVALID_STATE while no stream runs or for a stream's
    // first packet that is no keyframe, INVALID_ARGUMENT naming what is wrong.
    void push(const rcs_encoded_packet& packet);
    // RCS_OK, or QUEUE_FULL while the path is behind or the budget spent;
    // throws Failure(INVALID_STATE) while no stream runs.
    rcs_result ready();

private:
    std::recursive_mutex mutex_; // Held while delivering: stop() waits for it (on_encoded may push).
    Deliver deliver_;            // Set while a stream runs.
    std::function<uint64_t()> backlog_;
    agent::VideoStream stream_;
    bool keyframeSeen_ = false;
    bool layerLogged_ = false; // Its packets said another temporal layer than their bitstream (logged once).
    std::vector<uint8_t> config_; // Standard codecs: the latest keyframe's.
    uint64_t sequence_ = 0;
    agent::EncodedBudget budget_;
};

class HostAudio {
public:
    std::unique_ptr<platform::AudioCapture> capture();
    // Throws Failure: INVALID_STATE while no sound is being sent (sound off,
    // or not running), INVALID_ARGUMENT for another format.
    void push(const rcs_audio_frame& frame);
    // As HostVideo's.
    void prepareSamples();
    // A CMSampleBuffer's sound (rcs_session_push_sample_buffer) of `type`,
    // AUDIO or MICROPHONE: each converted on its own and mixed with the
    // other at the volumes `settings` give (0 leaves one out).
    void pushSample(const void* sampleBuffer, rcs_sample_buffer_type type, const rcs_send_audio_config& settings);

private:
    friend class HostAudioCapture;
    std::mutex converting_; // One sample buffer at a time.
    std::array<std::unique_ptr<platform::SampleSound>, audio::Mixer::sources> samples_; // AUDIO, MICROPHONE.
    audio::Mixer mixer_;
    uint64_t mixing_ = 0; // The capture the mixer holds sound of.
    std::vector<float> converted_, mixed_;
    std::mutex mutex_;
    platform::AudioCapture::Deliver deliver_; // Set while a capture runs.
    uint64_t captures_ = 0;                   // Started, under mutex_.
    std::atomic<uint32_t> lastFrames_{0}; // Read while delivering, under mutex_.
};

class HostInput final : public platform::InputInjector {
public:
    // `released`: set when the host is to release what it holds; the session
    // tells it (Session::deliverHostRelease) outside its locks.
    explicit HostInput(std::atomic<bool>& released) : released_(released) {}
    // on_input already had the event: the host executed it.
    rcs_result apply(const rcs_input_event&) override { return RCS_OK; }
    void releaseAll() noexcept override { released_ = true; }

private:
    std::atomic<bool>& released_;
};

} // namespace rcsdk::session
