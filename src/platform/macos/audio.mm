// Sound on macOS: capture with ScreenCaptureKit (macOS 13; the Core Audio tap
// in audio_tap.mm is the default from macOS 14.2) and playback through the
// default output unit (AAC: platform/apple/aac.mm).
//
// ScreenCaptureKit capture runs a stream of its own that only delivers sound,
// so it neither depends on nor disturbs the screen capture's stream. A
// supervisor thread (re)starts it: when the stream cannot start, stops with
// an error or delivers sound it cannot convert, it reports that and tries
// again every two seconds.
#include "platform/apple/audio_format.hpp"
#include "platform/apple/cf.hpp"
#include "platform/apple/render_gaps.hpp"
#include "platform/platform.hpp"
#include "util/log.hpp"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>

namespace rcsdk::platform {
namespace mac {
std::unique_ptr<AudioCapture> createCoreAudioTapCapture(); // audio_tap.mm
}
namespace {

constexpr auto retryInterval = 2s;

using apple::floatFormat;
using apple::osStatusText;

} // namespace
} // namespace rcsdk::platform

// ---- Capture ------------------------------------------------------------------------

namespace rcsdk::platform::mac {
struct AudioCaptureState {
    std::mutex mutex;
    AudioCapture::Deliver deliver;
    bool stopped = false; // Under mutex: no delivery after stop().
    std::string problem;  // Why the stream cannot go on; the supervisor reports it and starts again.
    std::vector<float> interleaved;
    std::atomic<uint32_t> period{0};
};

// "44100 Hz, 1 channel, 16-bit integer" for a status message.
inline std::string describeFormat(const AudioStreamBasicDescription* f) {
    if (!f) {
        return "no format";
    }
    if (f->mFormatID != kAudioFormatLinearPCM) {
        return "not linear PCM";
    }
    return std::to_string(uint32_t(f->mSampleRate)) + " Hz, " + std::to_string(f->mChannelsPerFrame) + " channels, " +
           std::to_string(f->mBitsPerChannel) + "-bit " +
           ((f->mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "integer");
}
} // namespace rcsdk::platform::mac

using rcsdk::platform::mac::AudioCaptureState;

API_AVAILABLE(macos(13.0))
@interface RCSAudioSink : NSObject <SCStreamOutput, SCStreamDelegate>
- (instancetype)initWithState:(std::shared_ptr<AudioCaptureState>)state;
@end

@implementation RCSAudioSink {
    std::shared_ptr<AudioCaptureState> _state;
}

- (instancetype)initWithState:(std::shared_ptr<AudioCaptureState>)state {
    if ((self = [super init])) {
        _state = std::move(state);
    }
    return self;
}

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeAudio || !CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample)) {
        return; // The stream's minimal screen output is not used.
    }
    const AudioStreamBasicDescription* format =
        CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample));
    if (!format || format->mFormatID != kAudioFormatLinearPCM || !(format->mFormatFlags & kAudioFormatFlagIsFloat) ||
        format->mBitsPerChannel != 32 || format->mSampleRate != rcsdk::audioRate || !format->mChannelsPerFrame) {
        [self fail:"ScreenCaptureKit delivers sound this SDK does not convert (" +
                   rcsdk::platform::mac::describeFormat(format) + ")"];
        return;
    }
    size_t needed = 0;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &needed, nullptr, 0, nullptr, nullptr, 0, nullptr);
    std::vector<uint8_t> storage(needed);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    CMBlockBufferRef block = nullptr;
    if (const OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sample, nullptr, list, needed, nullptr, nullptr, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            &block);
        status != noErr) {
        [self fail:"Reading the captured sound failed (OSStatus " + std::to_string(int(status)) + ")"];
        return;
    }
    const auto frames = uint32_t(CMSampleBufferGetNumSamples(sample));
    const bool planar = format->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
    const uint32_t channels = format->mChannelsPerFrame;
    std::lock_guard lock(_state->mutex);
    if (_state->stopped) {
        CFRelease(block);
        return;
    }
    auto& out = _state->interleaved;
    out.resize(size_t(frames) * rcsdk::audioChannels);
    for (uint32_t i = 0; i < frames; ++i) {
        for (uint32_t c = 0; c < rcsdk::audioChannels; ++c) {
            const uint32_t from = std::min(c, channels - 1); // Mono plays on both sides.
            const float* data = planar ? static_cast<const float*>(list->mBuffers[from].mData)
                                       : static_cast<const float*>(list->mBuffers[0].mData);
            out[size_t(i) * rcsdk::audioChannels + c] = planar ? data[i] : data[size_t(i) * channels + from];
        }
    }
    CFRelease(block);
    _state->period = frames;
    const uint64_t now = rcsdk::monotonicNs();
    _state->deliver(out, now - uint64_t(frames) * 1'000'000'000 / rcsdk::audioRate);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    const char* text = error.localizedDescription.UTF8String;
    [self fail:std::string("The audio stream stopped: ") + (text ? text : "no reason given")];
}

// The first problem stands until the supervisor reports it and starts again.
- (void)fail:(const std::string&)problem {
    std::lock_guard lock(_state->mutex);
    if (_state->problem.empty()) {
        _state->problem = problem;
    }
}
@end

namespace rcsdk::platform {
namespace {

// Leaving this application out (excludesCurrentProcessAudio) leaves out this
// process's own sound only: what its helper processes play is captured
// (Chromium and Electron play sound from a utility process). The Core Audio
// tap leaves out every process this one started.
class ScreenCaptureKitAudioCapture final : public AudioCapture {
public:
    ~ScreenCaptureKitAudioCapture() override { stop(); }

    uint32_t periodFrames() const noexcept override { return state_ ? state_->period.load() : 0; }

    void start(const AudioCaptureConfig& config, Deliver deliver, Status status) override {
        config_ = config;
        status_ = std::move(status);
        state_ = std::make_shared<mac::AudioCaptureState>();
        state_->deliver = std::move(deliver);
        thread_ = log::thread([this] { supervise(); });
    }

    void stop() noexcept override {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || !thread_.joinable()) {
                return;
            }
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

private:
    void supervise() {
        bool running = false;
        std::optional<AudioStatus> reported;
        for (;;) {
            AudioStatus now{true, RCS_OK, {}};
            if (!running) {
                now = open();
                running = now.active;
            } else {
                std::lock_guard lock(state_->mutex);
                if (!state_->problem.empty()) {
                    now = {false, RCS_ERROR_CAPTURE, state_->problem};
                    state_->problem.clear();
                    running = false;
                }
            }
            if (!running) {
                close();
            }
            if (!reported || reported->active != now.active || reported->message != now.message) {
                reported = now;
                if (!now.active) {
                    log::warning("audio", now.message);
                }
                status_(now);
            }
            std::unique_lock lock(mutex_);
            if (wake_.wait_for(lock, running ? 250ms : retryInterval, [&] { return stopping_; })) {
                break;
            }
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->stopped = true;
        }
        close();
    }

    // Starts the stream: active, or why not.
    AudioStatus open() {
        const auto problem = [](rcs_result result, std::string message) {
            return AudioStatus{false, result, std::move(message)};
        };
        if (!CGPreflightScreenCaptureAccess()) {
            return problem(RCS_ERROR_PERMISSION_REQUIRED, "Screen recording permission is required to capture sound");
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->problem.clear(); // A late report of the stream that stopped.
        }
        __block SCShareableContent* content = nil;
        __block NSError* error = nil;
        dispatch_semaphore_t listed = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* value, NSError* failure) {
            content = value;
            error = failure;
            dispatch_semaphore_signal(listed);
        }];
        if (dispatch_semaphore_wait(listed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
            return problem(RCS_ERROR_CAPTURE, "Listing shareable content timed out");
        }
        if (error || content.displays.count == 0) {
            return problem(RCS_ERROR_CAPTURE,
                           error ? std::string("Listing shareable content: ") + error.localizedDescription.UTF8String
                                 : "No display is available to capture sound with");
        }
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:content.displays.firstObject
                                                     excludingApplications:@[]
                                                          exceptingWindows:@[]];
        SCStreamConfiguration* settings = [SCStreamConfiguration new];
        settings.capturesAudio = YES;
        settings.excludesCurrentProcessAudio = config_.excludeCurrentProcess;
        settings.sampleRate = audioRate;
        settings.channelCount = audioChannels;
        // The picture is not used: as small and rare as allowed.
        settings.width = 2;
        settings.height = 2;
        settings.minimumFrameInterval = CMTimeMake(1, 1);
        settings.queueDepth = 3;
        sink_ = [[RCSAudioSink alloc] initWithState:state_];
        stream_ = [[SCStream alloc] initWithFilter:filter configuration:settings delegate:sink_];
        queue_ = dispatch_queue_create("rcsdk.audio-capture", DISPATCH_QUEUE_SERIAL);
        NSError* attachError = nil;
        if (![stream_ addStreamOutput:sink_ type:SCStreamOutputTypeAudio sampleHandlerQueue:queue_ error:&attachError] ||
            ![stream_ addStreamOutput:sink_ type:SCStreamOutputTypeScreen sampleHandlerQueue:queue_ error:&attachError]) {
            return problem(RCS_ERROR_CAPTURE, std::string("Attaching the audio output: ") +
                                                  (attachError ? attachError.localizedDescription.UTF8String
                                                               : "failed"));
        }
        __block NSError* startError = nil;
        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        [stream_ startCaptureWithCompletionHandler:^(NSError* failure) {
            startError = failure;
            dispatch_semaphore_signal(started);
        }];
        if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
            return problem(RCS_ERROR_CAPTURE, "Starting audio capture timed out");
        }
        if (startError) {
            return problem(RCS_ERROR_CAPTURE,
                           std::string("Starting audio capture: ") + startError.localizedDescription.UTF8String);
        }
        return {true, RCS_OK, {}};
    
    }

    void close() {
        if (stream_) {
            dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
            [stream_ stopCaptureWithCompletionHandler:^(NSError*) {
                dispatch_semaphore_signal(stopped);
            }];
            dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
            stream_ = nil;
            sink_ = nil;
        }
    
    }

    AudioCaptureConfig config_;
    Status status_;
    std::shared_ptr<mac::AudioCaptureState> state_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    SCStream* stream_ API_AVAILABLE(macos(13.0)) = nil;
    RCSAudioSink* sink_ API_AVAILABLE(macos(13.0)) = nil;
    dispatch_queue_t queue_ = nil;
};

// ---- Playback ----------------------------------------------------------------------

struct OutputDevice {
    AudioDeviceID id = kAudioObjectUnknown;
    std::string name;
};

std::optional<OutputDevice> defaultOutput() {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    OutputDevice device;
    UInt32 size = sizeof(device.id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device.id) != noErr ||
        device.id == kAudioObjectUnknown) {
        return std::nullopt;
    }
    CFStringRef name = nullptr;
    size = sizeof(name);
    address.mSelector = kAudioObjectPropertyName;
    if (AudioObjectGetPropertyData(device.id, &address, 0, nullptr, &size, &name) == noErr && name) {
        char buffer[256] = {};
        CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
        CFRelease(name);
        device.name = buffer;
    }
    return device;
}

// The default output unit plays on the system's default device. When that
// changes, the unit opens again: the period is a setting of the device and
// must be applied to the new one.
class CoreAudioPlayer final : public AudioPlayer {
public:
    ~CoreAudioPlayer() override { stop(); }

    void start(const PlaybackConfig& config, Fill fill, Status status) override {
        config_ = config;
        fill_ = std::move(fill);
        status_ = std::move(status);
        thread_ = log::thread([this] { supervise(); });
    }

    void stop() noexcept override {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || !thread_.joinable()) {
                return;
            }
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    Device device() const override {
        std::lock_guard lock(mutex_);
        Device device = device_;
        device.underruns = gaps_.count();
        return device;
    }

private:
    static OSStatus render(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp* time, UInt32,
                           UInt32 frames, AudioBufferList* data) {
        auto* self = static_cast<CoreAudioPlayer*>(context);
        self->gaps_.rendered(time, frames);
        self->fill_(static_cast<float*>(data->mBuffers[0].mData), frames);
        return noErr;
    }

    void supervise() {
        std::optional<AudioStatus> reported;
        for (;;) {
            const auto output = defaultOutput();
            const std::string name = output ? output->name : std::string();
            AudioStatus now{true, RCS_OK, {}};
            if (!output) {
                now = {false, RCS_ERROR_DEVICE, "No output device"};
                close();
            } else if (!unit_ || output->id != playing_) {
                if (unit_) {
                    log::info("audio", "The default output device changed to " + name);
                }
                close();
                now = open();
                playing_ = now.active ? output->id : kAudioObjectUnknown;
            }
            {
                std::lock_guard lock(mutex_);
                device_ = now.active ? Device{name, period(), 0} : Device{};
            }
            if (!reported || reported->active != now.active || reported->message != now.message) {
                reported = now;
                if (now.active) {
                    log::info("audio", "Playing on " + name + ", " + std::to_string(period()) + " frames per period");
                } else {
                    log::warning("audio", now.message);
                }
                status_(now);
            }
            const std::string problem = now.message;
            std::unique_lock lock(mutex_);
            if (wake_.wait_for(lock, problem.empty() ? 500ms : retryInterval, [&] { return stopping_; })) {
                break;
            }
        }
        close();
    }

    UInt32 period() const {
        UInt32 frames = 0, size = sizeof(frames);
        if (unit_) {
            AudioUnitGetProperty(unit_, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &frames, &size);
        }
        return frames;
    }

    AudioStatus open() {
        const auto problem = [](rcs_result result, std::string message) {
            return AudioStatus{false, result, std::move(message)};
        };
        AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                              kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        AudioUnit unit = nullptr;
        if (!component || AudioComponentInstanceNew(component, &unit) != noErr) {
            return problem(RCS_ERROR_DEVICE, "The default output unit is not available");
        }
        // Default 256 frames (5.3 ms) per device period instead of the
        // system's 512: half the output delay, and a smaller playback buffer.
        // A device that does not take it keeps its own; a period the host
        // asked for is required.
        const bool asked = config_.periodFrames != 0;
        UInt32 cycle = asked ? config_.periodFrames : 256;
        AudioValueRange range{};
        UInt32 size = sizeof(range);
        AudioUnitGetProperty(unit, kAudioDevicePropertyBufferFrameSizeRange, kAudioUnitScope_Global, 0, &range, &size);
        const bool inRange = !range.mMaximum || (cycle >= range.mMinimum && cycle <= range.mMaximum);
        if (!inRange && asked) {
            AudioComponentInstanceDispose(unit);
            return problem(RCS_ERROR_UNSUPPORTED, "A playback period of " + std::to_string(cycle) +
                                                      " frames is outside this device's range " +
                                                      std::to_string(UInt32(range.mMinimum)) + ".." +
                                                      std::to_string(UInt32(range.mMaximum)));
        }
        if (inRange && AudioUnitSetProperty(unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0,
                                            &cycle, sizeof(cycle)) != noErr &&
            asked) {
            AudioComponentInstanceDispose(unit);
            return problem(RCS_ERROR_UNSUPPORTED, "The output device does not take a period of " +
                                                      std::to_string(cycle) + " frames");
        }
        const auto format = floatFormat();
        AURenderCallbackStruct callback{&CoreAudioPlayer::render, this};
        OSStatus status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                               sizeof(format));
        if (status == noErr) {
            status = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                          &callback, sizeof(callback));
        }
        bool initialized = false;
        if (status == noErr) {
            status = AudioUnitInitialize(unit);
            initialized = status == noErr;
        }
        if (status == noErr) {
            gaps_.restart();
            status = AudioOutputUnitStart(unit);
        }
        if (status != noErr) {
            if (initialized) {
                AudioUnitUninitialize(unit);
            }
            AudioComponentInstanceDispose(unit);
            return problem(RCS_ERROR_DEVICE, "Opening the output device failed (OSStatus " + osStatusText(status) + ")");
        }
        unit_ = unit;
        // The default may settle on what the device runs at; a period the
        // host asked for must be the one in use.
        if (const UInt32 applied = period(); asked && applied != cycle) {
            close();
            return problem(RCS_ERROR_UNSUPPORTED, "The output device runs at " + std::to_string(applied) +
                                                      " frames per period, not the " + std::to_string(cycle) +
                                                      " asked for");
        }
        return {true, RCS_OK, {}};
    }

    void close() {
        if (unit_) {
            AudioOutputUnitStop(unit_);
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
    }

    PlaybackConfig config_;
    Fill fill_;
    Status status_;
    AudioUnit unit_ = nullptr;
    AudioDeviceID playing_ = kAudioObjectUnknown; // The device unit_ opened on.
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    Device device_;
    apple::RenderGaps gaps_;
};

} // namespace

std::unique_ptr<AudioCapture> createAudioCapture(rcs_audio_capture_backend backend) {
    if (backend == RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP) {
        return mac::createCoreAudioTapCapture();
    }
    return std::make_unique<ScreenCaptureKitAudioCapture>();
}

uint32_t audioCaptureBackends() noexcept {
    uint32_t backends = 0;
    backends |= 1u << RCS_AUDIO_CAPTURE_SCREENCAPTUREKIT;
    if (@available(macOS 14.2, *)) {
        backends |= 1u << RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP;
    }
    return backends;
}

rcs_audio_capture_backend defaultAudioCaptureBackend() noexcept {
    const uint32_t backends = audioCaptureBackends();
    return (backends & (1u << RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP))     ? RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP
           : (backends & (1u << RCS_AUDIO_CAPTURE_SCREENCAPTUREKIT)) ? RCS_AUDIO_CAPTURE_SCREENCAPTUREKIT
                                                                     : RCS_AUDIO_CAPTURE_AUTO;
}

bool audioCapturePeriodSelectable(rcs_audio_capture_backend backend) noexcept {
    return backend == RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP;
}

bool audioExclusionAvailable() noexcept {
    return audioCaptureBackends() != 0; // Both backends leave this application out (see each for what that covers).
}

std::unique_ptr<AudioPlayer> createAudioPlayer() {
    return std::make_unique<CoreAudioPlayer>();
}

} // namespace rcsdk::platform
