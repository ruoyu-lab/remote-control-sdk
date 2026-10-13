// Sound on iOS: players and microphones as parts of the application's one
// audio engine (audio_engine.mm), the sound devices of the current route,
// the microphone permission. iOS lets no application capture the sound of
// others; AAC is platform/apple/aac.mm.
#include "platform/apple/render_gaps.hpp"
#include "platform/ios/audio_engine.hpp"
#include "platform/platform.hpp"
#include "util/log.hpp"

#import <AVFoundation/AVFoundation.h>

#include <atomic>
#include <mutex>

namespace rcsdk::platform {
namespace {

// Plays 48 kHz stereo as a source node of the engine, on the current route
// (the speaker, headphones, AirPods, AirPlay), which follows the system's choice.
class EnginePlayback final : public AudioPlayer, ios::EnginePlayer {
public:
    ~EnginePlayback() override { stop(); }

    void start(const PlaybackConfig& config, Fill fill, Status status) override {
        config_ = config;
        fill_ = std::move(fill);
        status_ = std::move(status);
        started_ = true;
        ios::add(static_cast<ios::EnginePlayer*>(this));
    }
    void stop() noexcept override {
        if (std::exchange(started_, false)) {
            ios::remove(static_cast<ios::EnginePlayer*>(this));
        }
    }
    Device device() const override {
        Device device = active_ ? Device{ios::outputName(), ios::outputPeriod(), 0} : Device{};
        device.underruns = gaps_.count();
        return device;
    }

private:
    void status(const AudioStatus& status) override {
        gaps_.restart(); // The engine was set up again: a new node, perhaps a new route.
        active_ = status.active;
        status_(status);
    }
    void render(float* samples, uint32_t frames, const AudioTimeStamp* time) noexcept override {
        gaps_.rendered(time, frames);
        fill_(samples, frames);
    }
    uint32_t periodFrames() const noexcept override { return config_.periodFrames; }

    PlaybackConfig config_;
    Fill fill_;
    Status status_;
    bool started_ = false;
    std::atomic<bool> active_{false};
    apple::RenderGaps gaps_;
};

// A microphone: the engine's input, with voice processing as asked, 48 kHz stereo.
class EngineCapture final : public AudioCapture, ios::EngineMicrophone {
public:
    explicit EngineCapture(const MicrophoneConfig& config) : config_(config) {}
    ~EngineCapture() override { stop(); }

    void start(const AudioCaptureConfig&, Deliver deliver, Status status) override {
        deliver_ = std::move(deliver);
        status_ = std::move(status);
        started_ = true;
        ios::add(static_cast<ios::EngineMicrophone*>(this));
    }
    void stop() noexcept override {
        if (std::exchange(started_, false)) {
            ios::remove(static_cast<ios::EngineMicrophone*>(this));
        }
    }
    uint32_t periodFrames() const noexcept override { return ios::inputPeriod(); }

private:
    void status(const AudioStatus& status) override { status_(status); }
    void captured(std::span<const float> samples, uint64_t timestampNs) noexcept override {
        deliver_(samples, timestampNs);
    }
    const MicrophoneConfig& microphone() const noexcept override { return config_; }

    const MicrophoneConfig config_;
    Deliver deliver_;
    Status status_;
    bool started_ = false;
};

} // namespace

std::unique_ptr<AudioCapture> createAudioCapture(rcs_audio_capture_backend) {
    fail(RCS_ERROR_UNSUPPORTED, "iOS lets no application capture the sound of others; push the host's own "
                                "(RCS_AUDIO_HOST)");
}

uint32_t audioCaptureBackends() noexcept {
    return 0;
}

rcs_audio_capture_backend defaultAudioCaptureBackend() noexcept {
    return RCS_AUDIO_CAPTURE_AUTO;
}

bool audioCapturePeriodSelectable(rcs_audio_capture_backend) noexcept {
    return false;
}

bool audioExclusionAvailable() noexcept {
    return false;
}

std::unique_ptr<AudioPlayer> createAudioPlayer() {
    return std::make_unique<EnginePlayback>();
}

// The inputs the audio session's category lets the application choose from
// (while it records; with playback only, iOS lists none), and the current
// route's outputs.
std::vector<AudioDevice> audioDevices() {
    @autoreleasepool {
        AVAudioSession* session = AVAudioSession.sharedInstance;
        AVAudioSessionRouteDescription* route = session.currentRoute;
        std::vector<AudioDevice> result;
        for (AVAudioSessionPortDescription* port in session.availableInputs) {
            const bool current = [route.inputs containsObject:port];
            result.push_back({ios::deviceId(port.UID.UTF8String), true, current, port.portName.UTF8String});
        }
        for (AVAudioSessionPortDescription* port in route.outputs) {
            result.push_back({ios::deviceId(port.UID.UTF8String), false, true, port.portName.UTF8String});
        }
        return result;
    }
}

rcs_permission microphonePermission() noexcept {
    return AVAudioApplication.sharedInstance.recordPermission == AVAudioApplicationRecordPermissionGranted
               ? RCS_PERMISSION_GRANTED
               : RCS_PERMISSION_REQUIRED;
}

rcs_result requestMicrophonePermission() {
    switch (AVAudioApplication.sharedInstance.recordPermission) {
    case AVAudioApplicationRecordPermissionGranted:
        return RCS_OK;
    case AVAudioApplicationRecordPermissionUndetermined:
        // The prompt shows; the answer comes later (microphonePermission()).
        [AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL) {}];
        return RCS_ERROR_PERMISSION_REQUIRED;
    default: // Denied before: only Settings changes it.
        return RCS_ERROR_PERMISSION_REQUIRED;
    }
}

std::unique_ptr<AudioCapture> createMicrophoneCapture(const MicrophoneConfig& config) {
    return std::make_unique<EngineCapture>(config);
}

} // namespace rcsdk::platform
