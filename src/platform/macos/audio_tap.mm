// Sound capture with a Core Audio process tap (macOS 14.2): a global stereo
// tap, this application left out when asked, on a private aggregate device
// whose period the host chooses (default 240 frames, 5 ms), against
// ScreenCaptureKit's fixed 20 ms.
//
// The aggregate device's I/O callback runs on Core Audio's real-time thread:
// it only copies into a lock-free ring and signals. A thread of this class
// takes the sound from there, converts it to 48 kHz stereo float where the
// device runs at another rate, and delivers it. The same thread supervises:
// it rebuilds the tap when the default output device changes and after
// errors, reporting each gap, and keeps the processes left out up to date.
#include "platform/platform.hpp"
#include "util/log.hpp"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <libproc.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

namespace rcsdk::platform::mac {
namespace {

constexpr uint32_t defaultPeriod = 240; // 5 ms at 48 kHz
constexpr auto retryInterval = 2s;

AudioObjectPropertyAddress global(AudioObjectPropertySelector selector) {
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}

struct OutputDevice {
    AudioDeviceID id = kAudioObjectUnknown;
    NSString* uid = nil;
};

std::optional<OutputDevice> defaultOutput() {
    auto address = global(kAudioHardwarePropertyDefaultOutputDevice);
    OutputDevice device;
    UInt32 size = sizeof(device.id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device.id) != noErr ||
        device.id == kAudioObjectUnknown) {
        return std::nullopt;
    }
    CFStringRef uid = nullptr;
    size = sizeof(uid);
    address.mSelector = kAudioDevicePropertyDeviceUID;
    if (AudioObjectGetPropertyData(device.id, &address, 0, nullptr, &size, &uid) != noErr || !uid) {
        return std::nullopt;
    }
    device.uid = (__bridge_transfer NSString*)uid;
    return device;
}

// The channels of each input buffer `device` hands its I/O callback.
std::vector<UInt32> inputBuffers(AudioObjectID device) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size < sizeof(UInt32)) {
        return {};
    }
    std::vector<uint8_t> storage(size);
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, storage.data()) != noErr) {
        return {};
    }
    const auto* list = reinterpret_cast<const AudioBufferList*>(storage.data());
    std::vector<UInt32> channels;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
        channels.push_back(list->mBuffers[i].mNumberChannels);
    }
    return channels;
}

// Whether `pid` is `ancestor` or was started by it, directly or not.
bool descends(pid_t pid, pid_t ancestor) {
    for (int depth = 0; depth < 64 && pid > 1; ++depth) {
        if (pid == ancestor) {
            return true;
        }
        proc_bsdshortinfo info{};
        if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &info, sizeof(info)) != int(sizeof(info))) {
            return false;
        }
        pid = pid_t(info.pbsi_ppid);
    }
    return false;
}

// The Core Audio process objects of this application: this process and every
// process it started (Chromium and Electron hosts play sound from helper
// processes). Processes launchd starts on its behalf (XPC services such as
// WebKit's) are not its descendants and are not among them. A process has an
// object once it uses Core Audio. Nil, with `status` saying why, when Core
// Audio does not list its processes (the list may grow between asking its
// size and reading it: tried again).
NSArray<NSNumber*>* ownProcesses(OSStatus& status) {
    auto address = global(kAudioHardwarePropertyProcessObjectList);
    std::vector<AudioObjectID> objects;
    for (int attempt = 0;; ++attempt) {
        UInt32 size = 0;
        status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size);
        if (status != noErr) {
            return nil;
        }
        objects.resize(size / sizeof(AudioObjectID));
        status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, objects.data());
        if (status == noErr) {
            objects.resize(size / sizeof(AudioObjectID));
            break;
        }
        if (status != kAudioHardwareBadPropertySizeError || attempt == 2) {
            return nil;
        }
    }
    NSMutableArray<NSNumber*>* own = [NSMutableArray array];
    const pid_t self = getpid();
    for (const AudioObjectID object : objects) {
        pid_t pid = -1;
        UInt32 size = sizeof(pid);
        address.mSelector = kAudioProcessPropertyPID;
        if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &pid) == noErr && descends(pid, self)) {
            [own addObject:@(object)];
        }
    }
    return own;
}

// Where the tap's sound arrives in the aggregate device's I/O callback: after
// the input buffers of the output device itself (a headset's microphone, a
// virtual device's loopback), one buffer per stream.
struct TapLayout {
    UInt32 first = 0, buffers = 0, channels = 0; // The tap's buffers and the channels in each.
    UInt32 total = 0;                            // Every input buffer of the device.
};

// Interleaved float frames from the real-time thread to the delivering one.
class Ring {
public:
    explicit Ring(size_t frames, uint32_t channels) : data_(frames * channels), frames_(frames), channels_(channels) {}
    // Real-time: no locks, no allocation. `buffers` are the tap's: one
    // interleaved, or one per channel. Frames that do not fit are dropped.
    void write(const AudioBuffer* buffers, uint32_t frames, bool planar) noexcept {
        const uint64_t w = written_.load(std::memory_order_relaxed);
        const uint64_t free = frames_ - (w - read_.load(std::memory_order_acquire));
        frames = uint32_t(std::min<uint64_t>(frames, free));
        for (uint32_t i = 0; i < frames; ++i) {
            for (uint32_t c = 0; c < channels_; ++c) {
                const auto* source = static_cast<const float*>(buffers[planar ? c : 0].mData);
                data_[((w + i) % frames_) * channels_ + c] = planar ? source[i] : source[size_t(i) * channels_ + c];
            }
        }
        written_.store(w + frames, std::memory_order_release);
    }
    // Moves what is there into `out` (interleaved); returns the frames.
    uint32_t read(std::vector<float>& out) {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        const auto frames = uint32_t(written_.load(std::memory_order_acquire) - r);
        out.resize(size_t(frames) * channels_);
        for (uint32_t i = 0; i < frames; ++i) {
            for (uint32_t c = 0; c < channels_; ++c) {
                out[size_t(i) * channels_ + c] = data_[((r + i) % frames_) * channels_ + c];
            }
        }
        read_.store(r + frames, std::memory_order_release);
        return frames;
    }

private:
    std::vector<float> data_;
    const size_t frames_;
    const uint32_t channels_;
    std::atomic<uint64_t> written_{0}, read_{0};
};

class CoreAudioTapCapture final : public AudioCapture {
public:
    ~CoreAudioTapCapture() override { stop(); }

    void start(const AudioCaptureConfig& config, Deliver deliver, Status status) override {
        config_ = config;
        deliver_ = std::move(deliver);
        status_ = std::move(status);
        thread_ = log::thread([this] { run(); });
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
        dispatch_semaphore_signal(ready_);
        thread_.join();
    }

    uint32_t periodFrames() const noexcept override { return period_.load(); }

private:
    bool stopping() {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

    void run() {
        std::optional<AudioStatus> reported;
        const auto publish = [&](const AudioStatus& now) {
            if (!reported || reported->active != now.active || reported->message != now.message) {
                reported = now;
                if (!now.active) {
                    log::warning("audio", now.message);
                }
                status_(now);
            }
        };
        // Processes come and go: the ones of this application are left out
        // as soon as Core Audio knows them.
        const auto processes = global(kAudioHardwarePropertyProcessObjectList);
        dispatch_queue_t queue = dispatch_queue_create("rcsdk.audio-tap", DISPATCH_QUEUE_SERIAL);
        AudioObjectPropertyListenerBlock listener = ^(UInt32, const AudioObjectPropertyAddress*) {
          processesChanged_ = true;
          dispatch_semaphore_signal(ready_);
        };
        bool listening = false;
        if (config_.excludeCurrentProcess) {
            const OSStatus status =
                AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &processes, queue, listener);
            listening = status == noErr;
            if (!listening) {
                log::warning("audio", "Core Audio does not report new processes (OSStatus " +
                                          std::to_string(int(status)) +
                                          "): processes this application starts later are captured too");
            }
        }
        while (!stopping()) {
            const auto opened = open();
            publish(opened);
            if (opened.active) {
                serve();
            }
            close();
            if (!opened.active) {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, retryInterval, [&] { return stopping_; });
            }
        }
        if (listening) {
            AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &processes, queue, listener);
            dispatch_sync(queue, ^{
                          }); // A notification under way finishes before this object goes.
        }
    }

    AudioStatus open() {
        if (@available(macOS 14.2, *)) {
            const auto problem = [](rcs_result result, std::string message) {
                return AudioStatus{false, result, std::move(message)};
            };
            const auto output = defaultOutput();
            if (!output) {
                return problem(RCS_ERROR_DEVICE, "No output device to capture sound from");
            }
            output_ = output->uid;
            processesChanged_ = false;
            excluded_ = @[];
            if (config_.excludeCurrentProcess) {
                OSStatus listed = noErr;
                excluded_ = ownProcesses(listed);
                if (!excluded_) {
                    excluded_ = @[];
                    return problem(RCS_ERROR_CAPTURE, "Core Audio does not list this application's processes "
                                                      "(OSStatus " +
                                                          std::to_string(int(listed)) + "), which it is to leave out");
                }
            }
            description_ = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:excluded_];
            description_.privateTap = YES;
            description_.name = @"Remote Control SDK";
            OSStatus status = AudioHardwareCreateProcessTap(description_, &tap_);
            if (status != noErr) {
                return problem(RCS_ERROR_CAPTURE, "Creating the audio tap failed (OSStatus " +
                                                      std::to_string(int(status)) +
                                                      "); system audio recording may not be permitted");
            }
            auto address = global(kAudioTapPropertyFormat);
            UInt32 size = sizeof(format_);
            format_ = {};
            if (AudioObjectGetPropertyData(tap_, &address, 0, nullptr, &size, &format_) != noErr) {
                return problem(RCS_ERROR_CAPTURE, "The audio tap does not say its format");
            }
            if (format_.mFormatID != kAudioFormatLinearPCM || !(format_.mFormatFlags & kAudioFormatFlagIsFloat) ||
                format_.mBitsPerChannel != 32 || format_.mChannelsPerFrame != audioChannels) {
                return problem(RCS_ERROR_CAPTURE, "The audio tap delivers a format this SDK does not convert");
            }
            NSDictionary* aggregate = @{
                @kAudioAggregateDeviceNameKey : @"Remote Control SDK",
                @kAudioAggregateDeviceUIDKey : [NSUUID UUID].UUIDString,
                @kAudioAggregateDeviceMainSubDeviceKey : output_,
                @kAudioAggregateDeviceIsPrivateKey : @YES,
                @kAudioAggregateDeviceIsStackedKey : @NO,
                // Not started with the tapped sound: the device runs, and so
                // delivers, through silence too. A stream that never stops
                // keeps the viewer's buffer primed; one that starts with the
                // sound arrives late by the device's start.
                @kAudioAggregateDeviceTapAutoStartKey : @NO,
                @kAudioAggregateDeviceSubDeviceListKey : @[ @{@kAudioSubDeviceUIDKey : output_} ],
                @kAudioAggregateDeviceTapListKey :
                    @[ @{@kAudioSubTapDriftCompensationKey : @YES, @kAudioSubTapUIDKey : description_.UUID.UUIDString} ],
            };
            status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate, &device_);
            if (status != noErr) {
                return problem(RCS_ERROR_CAPTURE,
                               "Creating the capture device failed (OSStatus " + std::to_string(int(status)) + ")");
            }
            // The output device's own inputs come first, then the tap's: one
            // buffer with both channels, or one buffer per channel.
            const auto buffers = inputBuffers(device_);
            TapLayout layout{UInt32(inputBuffers(output->id).size()), 0, 0, UInt32(buffers.size())};
            layout.buffers = layout.total > layout.first ? layout.total - layout.first : 0;
            layout.channels = layout.buffers == 1 ? audioChannels : 1;
            const bool laidOut =
                (layout.buffers == 1 || layout.buffers == audioChannels) &&
                std::all_of(buffers.begin() + layout.first, buffers.end(), [&](UInt32 c) { return c == layout.channels; });
            if (!laidOut) {
                return problem(RCS_ERROR_CAPTURE, "The capture device's inputs are not the output device's " +
                                                      std::to_string(layout.first) + " buffers followed by the " +
                                                      "tap's stereo sound (" + std::to_string(layout.total) +
                                                      " buffers)");
            }
            const bool planar = layout.buffers > 1;
            const UInt32 requested = config_.periodFrames ? config_.periodFrames : defaultPeriod;
            UInt32 period = requested; // The device's period is in its own frames.
            if (format_.mSampleRate != audioRate) {
                period = UInt32(uint64_t(period) * uint64_t(format_.mSampleRate) / audioRate);
            }
            address = global(kAudioDevicePropertyBufferFrameSizeRange);
            AudioValueRange range{};
            size = sizeof(range);
            AudioObjectGetPropertyData(device_, &address, 0, nullptr, &size, &range);
            if (period < range.mMinimum || period > range.mMaximum) {
                return problem(RCS_ERROR_UNSUPPORTED, "The capture period of " + std::to_string(period) +
                                                          " frames is outside this device's range " +
                                                          std::to_string(UInt32(range.mMinimum)) + ".." +
                                                          std::to_string(UInt32(range.mMaximum)));
            }
            address = global(kAudioDevicePropertyBufferFrameSize);
            status = AudioObjectSetPropertyData(device_, &address, 0, nullptr, sizeof(period), &period);
            UInt32 applied = 0;
            size = sizeof(applied);
            if (status == noErr) {
                status = AudioObjectGetPropertyData(device_, &address, 0, nullptr, &size, &applied);
            }
            if (status != noErr) {
                return problem(RCS_ERROR_UNSUPPORTED, "The capture device refused a period of " +
                                                          std::to_string(period) + " frames (OSStatus " +
                                                          std::to_string(int(status)) + ")");
            }
            if (config_.periodFrames && applied != period) { // The default may settle on what the device runs at.
                return problem(RCS_ERROR_UNSUPPORTED, "The capture device runs at " + std::to_string(applied) +
                                                          " frames per period, not the " + std::to_string(period) +
                                                          " asked for");
            }
            if (format_.mSampleRate != audioRate && !openConverter()) {
                return problem(RCS_ERROR_CAPTURE, "Converting the tap's sound to 48 kHz failed");
            }
            ring_ = std::make_unique<Ring>(size_t(format_.mSampleRate), audioChannels);
            Ring* ring = ring_.get();
            std::atomic<bool>* misplaced = &misplaced_;
            misplaced_ = false;
            dispatch_semaphore_t ready = ready_;
            status = AudioDeviceCreateIOProcIDWithBlock(
                &proc_, device_, nullptr,
                ^(const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*, AudioBufferList*,
                  const AudioTimeStamp*) {
                  if (!input->mNumberBuffers) {
                      return; // No input this cycle.
                  }
                  if (input->mNumberBuffers != layout.total ||
                      input->mBuffers[layout.first].mNumberChannels != layout.channels) {
                      misplaced->store(true, std::memory_order_relaxed); // The device changed under the tap.
                  } else {
                      const AudioBuffer* tap = input->mBuffers + layout.first;
                      ring->write(tap, tap[0].mDataByteSize / 4 / layout.channels, planar);
                  }
                  dispatch_semaphore_signal(ready);
                });
            if (status == noErr) {
                status = AudioDeviceStart(device_, proc_);
            }
            if (status != noErr) {
                return problem(RCS_ERROR_CAPTURE,
                               "Starting audio capture failed (OSStatus " + std::to_string(int(status)) + ")");
            }
            period_ = uint32_t(uint64_t(applied) * audioRate / uint64_t(format_.mSampleRate));
            log::debug("audio", "Core Audio tap: " + std::to_string(uint32_t(format_.mSampleRate)) + " Hz, " +
                                    std::to_string(applied) + " frames per period, tap after " +
                                    std::to_string(layout.first) + " input buffers of the output device" +
                                    (excluded_.count ? ", " + std::to_string(excluded_.count) +
                                                           " processes of this application left out"
                                                     : std::string()));
            return {true, RCS_OK, {}};
        }
        return {false, RCS_ERROR_UNSUPPORTED, "Core Audio process taps need macOS 14.2 or later"};
    }

    // Delivers until the tap must be rebuilt or capture stops.
    void serve() {
        auto nextCheck = Clock::now() + 500ms;
        std::vector<float> raw, converted;
        while (!stopping()) {
            dispatch_semaphore_wait(ready_, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC));
            if (const uint32_t frames = ring_->read(raw)) {
                const uint64_t now = monotonicNs();
                if (converter_) {
                    convert(raw, frames, converted);
                    if (!converted.empty()) {
                        deliver_(converted, now - uint64_t(converted.size() / audioChannels) * 1'000'000'000 / audioRate);
                    }
                } else {
                    deliver_(raw, now - uint64_t(frames) * 1'000'000'000 / audioRate);
                }
            }
            if (misplaced_) {
                log::warning("audio", "The capture device's inputs changed; rebuilding the audio tap");
                return;
            }
            if (processesChanged_.exchange(false)) {
                OSStatus listed = noErr;
                NSArray<NSNumber*>* processes = ownProcesses(listed);
                if (!processes) { // The tap is built again, with the list or with the reason it failed.
                    log::warning("audio", "Core Audio does not list this application's processes (OSStatus " +
                                              std::to_string(int(listed)) + "); rebuilding the audio tap");
                    return;
                }
                if (!leaveOut(processes)) {
                    return;
                }
            }
            if (Clock::now() >= nextCheck) {
                nextCheck = Clock::now() + 500ms;
                const auto output = defaultOutput();
                if (!output || ![output->uid isEqualToString:output_]) {
                    log::info("audio", "The default output device changed; rebuilding the audio tap");
                    return;
                }
            }
        }
    }

    // Leaves `processes` out of the running tap; false when the tap must be
    // built again for that.
    bool leaveOut(NSArray<NSNumber*>* processes) {
        if (@available(macOS 14.2, *)) {
            if ([processes isEqualToArray:excluded_]) {
                return true;
            }
            description_.processes = processes;
            const void* description = (__bridge const void*)description_;
            const auto address = global(kAudioTapPropertyDescription);
            const OSStatus status =
                AudioObjectSetPropertyData(tap_, &address, 0, nullptr, sizeof(description), &description);
            if (status != noErr) {
                log::warning("audio", "Updating the processes the audio tap leaves out failed (OSStatus " +
                                          std::to_string(int(status)) + "); rebuilding the tap");
                return false;
            }
            log::debug("audio", "The audio tap leaves out " + std::to_string(processes.count) +
                                    " processes of this application");
            excluded_ = processes;
        }
        return true;
    }

    void close() {
        if (@available(macOS 14.2, *)) {
            if (proc_) {
                AudioDeviceStop(device_, proc_);
                AudioDeviceDestroyIOProcID(device_, proc_);
                proc_ = nullptr;
            }
            if (device_ != kAudioObjectUnknown) {
                AudioHardwareDestroyAggregateDevice(device_);
                device_ = kAudioObjectUnknown;
            }
            if (tap_ != kAudioObjectUnknown) {
                AudioHardwareDestroyProcessTap(tap_);
                tap_ = kAudioObjectUnknown;
            }
            description_ = nil;
        }
        if (converter_) {
            AudioConverterDispose(converter_);
            converter_ = nullptr;
        }
        ring_.reset();
        period_ = 0;
    }

    // ---- Another device rate to 48 kHz ---------------------------------------------

    bool openConverter() {
        AudioStreamBasicDescription in = format_;
        in.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked; // The ring interleaves.
        in.mBytesPerFrame = in.mBytesPerPacket = audioChannels * 4;
        in.mFramesPerPacket = 1;
        AudioStreamBasicDescription out = in;
        out.mSampleRate = audioRate;
        return AudioConverterNew(&in, &out, &converter_) == noErr;
    }

    struct Input {
        const std::vector<float>* samples;
        uint32_t frames;
        bool consumed;
    };
    static OSStatus supply(AudioConverterRef, UInt32* packets, AudioBufferList* data, AudioStreamPacketDescription**,
                           void* context) {
        auto* input = static_cast<Input*>(context);
        if (input->consumed) {
            *packets = 0;
            return 'rcs!';
        }
        input->consumed = true;
        *packets = input->frames;
        data->mBuffers[0].mData = const_cast<float*>(input->samples->data());
        data->mBuffers[0].mDataByteSize = input->frames * audioChannels * 4;
        data->mBuffers[0].mNumberChannels = audioChannels;
        return noErr;
    }
    void convert(const std::vector<float>& raw, uint32_t frames, std::vector<float>& out) {
        Input input{&raw, frames, false};
        UInt32 produced = UInt32(uint64_t(frames) * audioRate / uint64_t(format_.mSampleRate)) + 16;
        out.resize(size_t(produced) * audioChannels);
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0] = {audioChannels, UInt32(out.size() * 4), out.data()};
        AudioConverterFillComplexBuffer(converter_, &CoreAudioTapCapture::supply, &input, &produced, &list, nullptr);
        out.resize(size_t(produced) * audioChannels);
    }

    AudioCaptureConfig config_;
    Deliver deliver_;
    Status status_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    dispatch_semaphore_t ready_ = dispatch_semaphore_create(0);
    std::atomic<uint32_t> period_{0};
    std::atomic<bool> processesChanged_{false};
    std::atomic<bool> misplaced_{false}; // The I/O callback found the tap's buffers elsewhere.

    NSString* output_ = nil;
    CATapDescription* description_ API_AVAILABLE(macos(14.2)) = nil;
    NSArray<NSNumber*>* excluded_ = @[];
    AudioObjectID tap_ = kAudioObjectUnknown;
    AudioDeviceID device_ = kAudioObjectUnknown;
    AudioDeviceIOProcID proc_ = nullptr;
    AudioStreamBasicDescription format_{};
    AudioConverterRef converter_ = nullptr;
    std::unique_ptr<Ring> ring_;
};

} // namespace

std::unique_ptr<AudioCapture> createCoreAudioTapCapture() {
    return std::make_unique<CoreAudioTapCapture>();
}

} // namespace rcsdk::platform::mac
