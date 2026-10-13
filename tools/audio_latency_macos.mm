// Sound latency through the SDK on one Mac, measured on one clock.
//
// This process runs a sharer and a viewer over UDP on 127.0.0.1, and a
// recorder: a Core Audio tap on everything the Mac plays, this process
// included. A child process (afplay) plays quiet clicks. The recorder hears
// each click twice: as played, and as the viewer plays it after the SDK
// captured, encoded, sent, decoded and buffered it. The gap is the SDK's
// latency up to the system's mix; the speakers' own delay comes on top for
// any application alike. Clicks come every 1.0137 s, so they fall at
// different points of the capture and output periods: the spread is real.
//
// The sharer leaves this process's sound out, so the viewer's playback is not
// captured again: no echo loop.
//
// usage: rcsdk_audio_latency [--seconds N] [--codec opus|aac|pcm] [--capture tap|sck]
//          [--capture-period N] [--playback-period N] [--margin-us N]
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/tuning.hpp>

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr uint32_t rate = 48000;
constexpr double spacing = 1.0137; // Seconds between clicks.

// Everything the Mac plays, left channel, as a continuous timeline.
class Recorder {
public:
    explicit Recorder(double seconds) : samples_(size_t(seconds * rate) + rate) {}

    bool start() {
        CATapDescription* description = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
        description.privateTap = YES;
        if (AudioHardwareCreateProcessTap(description, &tap_) != noErr) {
            return false;
        }
        AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        AudioDeviceID output = 0;
        UInt32 size = sizeof(output);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &output);
        CFStringRef uid = nullptr;
        size = sizeof(uid);
        address.mSelector = kAudioDevicePropertyDeviceUID;
        AudioObjectGetPropertyData(output, &address, 0, nullptr, &size, &uid);
        NSString* outputUid = (__bridge_transfer NSString*)uid;
        NSDictionary* aggregate = @{
            @kAudioAggregateDeviceNameKey : @"latency recorder",
            @kAudioAggregateDeviceUIDKey : [NSUUID UUID].UUIDString,
            @kAudioAggregateDeviceMainSubDeviceKey : outputUid,
            @kAudioAggregateDeviceIsPrivateKey : @YES,
            @kAudioAggregateDeviceTapAutoStartKey : @YES,
            @kAudioAggregateDeviceSubDeviceListKey : @[ @{@kAudioSubDeviceUIDKey : outputUid} ],
            @kAudioAggregateDeviceTapListKey : @[ @{@kAudioSubTapUIDKey : description.UUID.UUIDString} ],
        };
        if (AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate, &device_) != noErr) {
            return false;
        }
        UInt32 period = 128;
        address = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                   kAudioObjectPropertyElementMain};
        AudioObjectSetPropertyData(device_, &address, 0, nullptr, sizeof(period), &period);
        auto* samples = &samples_;
        auto* count = &count_;
        if (AudioDeviceCreateIOProcIDWithBlock(&proc_, device_, nullptr,
                                               ^(const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp*,
                                                 AudioBufferList*, const AudioTimeStamp*) {
                                                 const auto* s = static_cast<const float*>(in->mBuffers[0].mData);
                                                 const bool planar = in->mNumberBuffers > 1;
                                                 const UInt32 frames =
                                                     in->mBuffers[0].mDataByteSize / 4 / (planar ? 1 : 2);
                                                 size_t n = count->load();
                                                 for (UInt32 i = 0; i < frames && n < samples->size(); ++i) {
                                                     (*samples)[n++] = planar ? s[i] : s[i * 2];
                                                 }
                                                 count->store(n);
                                               }) != noErr) {
            return false;
        }
        return AudioDeviceStart(device_, proc_) == noErr;
    }
    void stop() {
        AudioDeviceStop(device_, proc_);
        AudioDeviceDestroyIOProcID(device_, proc_);
        AudioHardwareDestroyAggregateDevice(device_);
        AudioHardwareDestroyProcessTap(tap_);
    }
    std::vector<float> samples() const { return {samples_.begin(), samples_.begin() + ptrdiff_t(count_.load())}; }

private:
    std::vector<float> samples_;
    std::atomic<size_t> count_{0};
    AudioObjectID tap_ = 0;
    AudioDeviceID device_ = 0;
    AudioDeviceIOProcID proc_ = nullptr;
};

void writeClicks(const std::string& path, int clicks) {
    const uint32_t frames = uint32_t((clicks + 1) * spacing * rate);
    std::vector<int16_t> pcm(size_t(frames) * 2, 0);
    for (int c = 0; c < clicks; ++c) {
        const auto start = size_t((c + 0.5) * spacing * rate);
        for (size_t i = 0; i < 96; ++i) { // 2 ms of 3 kHz.
            const auto v = int16_t(8000 * std::sin(2 * M_PI * 3000 * double(i) / rate));
            pcm[(start + i) * 2] = pcm[(start + i) * 2 + 1] = v;
        }
    }
    std::ofstream out(path, std::ios::binary);
    const auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = frames * 4;
    out.write("RIFF", 4), u32(36 + bytes), out.write("WAVEfmt ", 8), u32(16), u16(1), u16(2), u32(rate), u32(rate * 4),
        u16(4), u16(16), out.write("data", 4), u32(bytes);
    out.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(bytes));
}

std::vector<size_t> onsets(const std::vector<float>& s, float threshold, size_t quiet) {
    std::vector<size_t> result;
    size_t silent = quiet;
    for (size_t i = 0; i < s.size(); ++i) {
        if (std::fabs(s[i]) > threshold) {
            if (silent >= quiet) {
                result.push_back(i);
            }
            silent = 0;
        } else {
            ++silent;
        }
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    int seconds = 14;
    std::string codecName = "opus", capture = "tap";
    uint32_t capturePeriod = 0, playbackPeriod = 0, marginUs = 2000;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string name = argv[i], value = argv[i + 1];
        if (name == "--seconds") {
            seconds = std::stoi(value);
        } else if (name == "--codec") {
            codecName = value;
        } else if (name == "--capture") {
            capture = value;
        } else if (name == "--capture-period") {
            capturePeriod = uint32_t(std::stoul(value));
        } else if (name == "--playback-period") {
            playbackPeriod = uint32_t(std::stoul(value));
        } else if (name == "--margin-us") {
            marginUs = uint32_t(std::stoul(value));
        }
    }
    const rcs_audio_codec codec = codecName == "aac" ? RCS_AUDIO_AAC : codecName == "pcm" ? RCS_AUDIO_PCM : RCS_AUDIO_OPUS;
    rcsdk::setLogCallback(RCS_LOG_DEBUG, [](const rcs_log_record& record) {
        if (std::string(record.module) == "audio") {
            std::fprintf(stderr, "[audio] %s\n", record.message);
        }
    });

    uint8_t key[32];
    rcs_generate_connection_key(key, sizeof(key));
    rcsdk::SessionConfig shareConfig(RCS_ROLE_SHARE), viewConfig(RCS_ROLE_VIEW);
    for (auto* c : {&shareConfig, &viewConfig}) {
        c->setAddress("127.0.0.1", 47962).setKey(key).setTransport(RCS_TRANSPORT_UDP).setAudioCodecs({codec});
    }
    shareConfig.setVideo(640, 360, 30, 2'000'000).setExcludeOwnSound(true);
    auto& captureTuning = shareConfig.extension<rcs_audio_tuning_config>();
    captureTuning.capture_backend =
        capture == "sck" ? RCS_AUDIO_CAPTURE_SCREENCAPTUREKIT : RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP;
    captureTuning.capture_period_frames = capturePeriod;
    auto& playbackTuning = viewConfig.extension<rcs_audio_tuning_config>();
    playbackTuning.buffer_margin_us = marginUs;
    playbackTuning.playback_period_frames = playbackPeriod;
    rcsdk::Session share(shareConfig);
    rcsdk::Session view(viewConfig);
    if (share.start() != RCS_OK) {
        std::fprintf(stderr, "share: %s\n", share.failure().message.c_str());
        return 1;
    }
    std::this_thread::sleep_for(500ms);
    if (view.start() != RCS_OK) {
        std::fprintf(stderr, "view: %s\n", view.failure().message.c_str());
        return 1;
    }
    for (int i = 0; i < 100 && !view.stats<rcs_audio_receive_stats>().active; ++i) {
        std::this_thread::sleep_for(50ms);
    }
    Recorder recorder(seconds + 2);
    if (!recorder.start()) {
        std::fprintf(stderr, "the recorder did not start\n");
        return 1;
    }
    // A first sound makes the sharer's tap rebuild once this process uses
    // audio; the clicks start after that.
    std::this_thread::sleep_for(1500ms);
    const std::string clicks = "/tmp/rcsdk-clicks.wav";
    writeClicks(clicks, int(seconds / spacing) - 1);
    std::system(("afplay -v 0.3 " + clicks).c_str());
    std::this_thread::sleep_for(500ms);
    const auto sent = share.stats<rcs_audio_send_stats>();
    const auto received = view.stats<rcs_audio_receive_stats>();
    recorder.stop();
    view.stop();
    share.stop();
    std::remove(clicks.c_str());

    const auto s = recorder.samples();
    const auto found = onsets(s, 0.01f, rate / 3);
    std::vector<double> latencies;
    for (const size_t click : found) {
        const size_t from = click + rate / 200, to = std::min(s.size(), click + rate / 3);
        for (size_t i = from; i < to; ++i) {
            if (std::fabs(s[i]) > 0.01f) {
                latencies.push_back(double(i - click) * 1000 / rate);
                break;
            }
        }
    }
    std::printf("codec %s, capture %s (%u frames), playback period %u, margin %u us: %zu clicks, %zu replays\n",
                codecName.c_str(), capture.c_str(), sent.capture_period_frames, received.playback_period_frames,
                marginUs, found.size(), latencies.size());
    if (latencies.empty()) {
        return 1;
    }
    std::sort(latencies.begin(), latencies.end());
    std::printf("latency to the system mix: min %.1f ms, median %.1f ms, max %.1f ms\n", latencies.front(),
                latencies[latencies.size() / 2], latencies.back());
    std::printf("viewer buffer %u ms (target %u ms), underruns %llu, lost %llu\n", received.buffer_ms,
                received.target_ms, (unsigned long long)received.underruns, (unsigned long long)received.lost_packets);
    return 0;
}
