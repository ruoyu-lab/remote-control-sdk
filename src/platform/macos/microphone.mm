// Microphones on macOS: the sound devices from Core Audio, the permission from
// AVFoundation (TCC), capture through AVAudioEngine's input node, with voice
// processing (echo cancellation of what this machine plays, noise
// suppression) when asked for and the system has it (reported either way). A supervisor thread opens the engine, and
// opens it again when its configuration changes (the device, or its format)
// or it cannot start, reporting why and trying again every two seconds.
#include "platform/platform.hpp"
#include "util/log.hpp"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace rcsdk::platform {
namespace {

constexpr auto retryInterval = 2s;

template <class T>
bool property(AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, T& value) {
    const AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) == noErr;
}

std::string nameOf(AudioDeviceID device) {
    CFStringRef name = nullptr;
    if (!property(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, name) || !name) {
        return {};
    }
    char buffer[256] = {};
    CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(name);
    return buffer;
}

bool hasStreams(AudioDeviceID device, AudioObjectPropertyScope scope) {
    const AudioObjectPropertyAddress address{kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    return AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) == noErr && size > 0;
}

std::string text(NSError* error) {
    const char* description = error ? error.localizedDescription.UTF8String : nullptr;
    return description ? description : "";
}

class MicrophoneCapture final : public AudioCapture {
public:
    explicit MicrophoneCapture(const MicrophoneConfig& config) : config_(config) {}
    ~MicrophoneCapture() override { stop(); }

    uint32_t periodFrames() const noexcept override { return period_.load(); }

    void start(const AudioCaptureConfig&, Deliver deliver, Status status) override {
        deliver_ = std::move(deliver);
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

private:
    void supervise() {
        std::optional<AudioStatus> reported;
        const auto publish = [&](const AudioStatus& now) {
            if (!reported || reported->active != now.active || reported->message != now.message ||
                reported->voiceProcessing != now.voiceProcessing) {
                reported = now;
                if (!now.active) {
                    log::warning("audio", now.message);
                }
                status_(now);
            }
        };
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            changed_ = false;
            lock.unlock();
            std::optional<AudioStatus> failed;
            @autoreleasepool { // Nothing thrown leaves it.
                try {
                    open();
                } catch (const Failure& failure) {
                    failed = AudioStatus{false, failure.result(), failure.what()};
                    close();
                }
            }
            publish(failed.value_or(AudioStatus{true, RCS_OK, {}, processing_}));
            const bool running = !failed;
            lock.lock();
            // Running: until the engine's configuration changes; else try again later.
            wake_.wait_for(lock, running ? std::chrono::hours(24 * 365) : retryInterval,
                           [&] { return stopping_ || (running && changed_); });
            const bool reopen = running && changed_;
            lock.unlock();
            if (running) {
                close();
                if (reopen) {
                    log::info("audio", "The microphone's configuration changed; opening it again");
                }
            }
            lock.lock();
        }
    }

    void open() {
        if (microphonePermission() != RCS_PERMISSION_GRANTED) {
            fail(RCS_ERROR_PERMISSION_REQUIRED,
                 "This process may not record from microphones (rcs_request_microphone_permission)");
        }
        {
            engine_ = [[AVAudioEngine alloc] init];
            AVAudioInputNode* input = engine_.inputNode;
            if (config_.device) {
                AudioDeviceID device = config_.device;
                if (AudioUnitSetProperty(input.audioUnit, kAudioOutputUnitProperty_CurrentDevice,
                                         kAudioUnitScope_Global, 0, &device, sizeof(device)) != noErr) {
                    fail(RCS_ERROR_DEVICE, "Sound device " + std::to_string(config_.device) +
                                               " cannot record (rcs_list_audio_devices lists the inputs)");
                }
            }
            NSError* error = nil;
            processing_ = config_.voiceProcessing && [input setVoiceProcessingEnabled:YES error:&error];
            if (config_.voiceProcessing && !processing_) {
                log::info("audio", "The microphone has no voice processing here (" + text(error) +
                                       "): it records without");
            }
            AVAudioFormat* native = [input outputFormatForBus:0];
            if (!native || native.sampleRate <= 0 || !native.channelCount) {
                fail(RCS_ERROR_DEVICE, "No microphone to record from");
            }
            AVAudioFormat* wanted = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                                     sampleRate:audioRate
                                                                       channels:audioChannels
                                                                    interleaved:YES];
            AVAudioConverter* converter = [[AVAudioConverter alloc] initFromFormat:native toFormat:wanted];
            if (!converter) {
                fail(RCS_ERROR_DEVICE, "The microphone's format cannot be converted to 48 kHz stereo");
            }
            if (native.channelCount == 1) {
                converter.channelMap = @[ @0, @0 ];
            }
            observer_ = [[NSNotificationCenter defaultCenter]
                addObserverForName:AVAudioEngineConfigurationChangeNotification
                            object:engine_
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          {
                              std::lock_guard lock(mutex_);
                              changed_ = true;
                          }
                          wake_.notify_all();
                        }];
            const double ratio = audioRate / native.sampleRate;
            [input installTapOnBus:0
                        bufferSize:AVAudioFrameCount(native.sampleRate / 100) // 10 ms
                            format:native
                             block:^(AVAudioPCMBuffer* buffer, AVAudioTime*) {
                               convert(converter, wanted, buffer, ratio);
                             }];
            [engine_ prepare];
            if (![engine_ startAndReturnError:&error]) {
                fail(RCS_ERROR_DEVICE, "The microphone cannot start: " + text(error));
            }
        }
    }

    // The tap's thread.
    void convert(AVAudioConverter* converter, AVAudioFormat* wanted, AVAudioPCMBuffer* buffer, double ratio) {
        const auto capacity = AVAudioFrameCount(buffer.frameLength * ratio) + 32;
        AVAudioPCMBuffer* out = [[AVAudioPCMBuffer alloc] initWithPCMFormat:wanted frameCapacity:capacity];
        __block bool given = false;
        NSError* error = nil;
        [converter convertToBuffer:out
                             error:&error
                withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount, AVAudioConverterInputStatus* status) {
                  if (given) {
                      *status = AVAudioConverterInputStatus_NoDataNow;
                      return nil;
                  }
                  given = true;
                  *status = AVAudioConverterInputStatus_HaveData;
                  return buffer;
                }];
        if (error || !out.frameLength) {
            return;
        }
        period_ = out.frameLength;
        const uint64_t duration = uint64_t(out.frameLength) * 1'000'000'000 / audioRate;
        deliver_({out.floatChannelData[0], size_t(out.frameLength) * audioChannels}, monotonicNs() - duration);
    }

    void close() {
        @autoreleasepool {
            if (observer_) {
                [[NSNotificationCenter defaultCenter] removeObserver:observer_];
                observer_ = nil;
            }
            if (engine_) {
                [engine_.inputNode removeTapOnBus:0];
                [engine_ stop];
                engine_ = nil;
            }
        }
    }

    const MicrophoneConfig config_;
    Deliver deliver_;
    Status status_;
    AVAudioEngine* engine_ = nil;
    id observer_ = nil;
    std::atomic<uint32_t> period_{0};
    bool processing_ = false; // The supervisor's: voice processing runs.
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false, changed_ = false;
    std::thread thread_;
};

} // namespace

std::vector<AudioDevice> audioDevices() {
    std::vector<AudioDevice> result;
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
        return result;
    }
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr) {
        return result;
    }
    AudioDeviceID defaultInput = kAudioObjectUnknown, defaultOutput = kAudioObjectUnknown;
    property(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
             defaultInput);
    property(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
             defaultOutput);
    for (const AudioDeviceID device : devices) {
        const std::string name = nameOf(device);
        if (hasStreams(device, kAudioObjectPropertyScopeInput)) {
            result.push_back({device, true, device == defaultInput, name});
        }
        if (hasStreams(device, kAudioObjectPropertyScopeOutput)) {
            result.push_back({device, false, device == defaultOutput, name});
        }
    }
    return result;
}

rcs_permission microphonePermission() noexcept {
    return [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] == AVAuthorizationStatusAuthorized
               ? RCS_PERMISSION_GRANTED
               : RCS_PERMISSION_REQUIRED;
}

rcs_result requestMicrophonePermission() {
    if (!pthread_main_np()) {
        return RCS_ERROR_WRONG_THREAD;
    }
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized:
        return RCS_OK;
    case AVAuthorizationStatusNotDetermined:
        // The prompt shows; the answer comes later (microphonePermission()).
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {}];
        return RCS_ERROR_PERMISSION_REQUIRED;
    default: // Denied before, or restricted: only System Settings changes it.
        return RCS_ERROR_PERMISSION_REQUIRED;
    }
}

std::unique_ptr<AudioCapture> createMicrophoneCapture(const MicrophoneConfig& config) {
    return std::make_unique<MicrophoneCapture>(config);
}

} // namespace rcsdk::platform
