// The host's two sample buffer sounds mixed into one (audio/mixer.hpp), in
// simulated time: sources of their own buffer lengths and paces mix in full,
// without breaking up; one alone goes out as it comes; one that stops holds
// the other up briefly and joins again; bursts do not make a source look
// stopped; one ahead for good (clocks apart, a lost buffer) is set back, so
// what is held stays bounded; volumes, full scale, leaving one out. On Apple
// systems the same with Core Media sample buffers as ReplayKit hands them
// over (the applications' sound at 44.1 kHz in big-endian 16-bit stereo, the
// microphone at 48 kHz in 16-bit mono, float32 not interleaved), each
// converted on its own (platform::SampleSound), and through the session's
// host sound (session::HostAudio) with rcs_send_audio_config's volumes.
#include "audio/mixer.hpp"
#include "platform/platform.hpp"
#include "session/host_sources.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <vector>

#ifdef __APPLE__
#include <CoreMedia/CoreMedia.h>
#include <libkern/OSByteOrder.h>
#endif

using namespace rcsdk;

namespace {

constexpr double pi = 3.14159265358979;
constexpr double appHz = 1000, micHz = 1500; // Whole cycles in every 10 ms at 48 kHz: they never leak into each other.

// The amplitude of `hz` in the left channel of `stereo` from frame `from`, over `frames` (Goertzel).
double amplitude(const std::vector<float>& stereo, size_t from, size_t frames, double hz) {
    const double coefficient = 2 * std::cos(2 * pi * hz / audioRate);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < frames; ++i) {
        const double s = stereo[(from + i) * audioChannels] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coefficient * s1 * s2)) / double(frames);
}

// The lowest and highest amplitude of `hz` over 100 ms windows from `from` to `to` (frames):
// a sound that broke up anywhere shows a dip.
std::pair<double, double> amplitudeRange(const std::vector<float>& stereo, size_t from, size_t to, double hz) {
    constexpr size_t window = audioRate / 10;
    double lowest = 1e9, highest = 0;
    for (size_t at = from; at + window <= to; at += window) {
        const double a = amplitude(stereo, at, window, hz);
        lowest = std::min(lowest, a);
        highest = std::max(highest, a);
    }
    return {lowest, highest};
}

// One simulated source: buffers of `frames` (at 48 kHz, as converted) every
// `periodNs`, its clock `ppm` fast, a tone of `hz`; nothing from `stopNs` to
// `resumeNs`; `burst` buffers at a time; `lose`: the buffer due then never comes.
struct Feed {
    uint32_t source = 0;
    double hz = appHz, level = 0.3, gain = 1;
    std::vector<uint32_t> frames{1024};
    double periodNs = 1024e9 / audioRate;
    double ppm = 0;
    uint64_t stopNs = UINT64_MAX, resumeNs = UINT64_MAX;
    uint32_t burst = 1;
    uint64_t loseNs = UINT64_MAX;
};

// ReplayKit's application sound: 1024 frames at 44.1 kHz, about 1114.6 at 48 kHz.
Feed appFeed() {
    Feed f;
    f.frames = {1115, 1114, 1115, 1114, 1115, 1114, 1115, 1114, 1115, 1115};
    f.periodNs = 1024e9 / 44100;
    return f;
}
Feed micFeed() {
    Feed f;
    f.source = 1;
    f.hz = micHz;
    f.level = 0.2;
    return f;
}

struct Run {
    std::vector<float> out;
    uint32_t mostHeld[audio::Mixer::sources] = {}; // After the first second.
    audio::Mixer::Counters counters;
};

// The feeds pushed in time order for `seconds`; `check` is called after each push.
Run simulate(std::vector<Feed> feeds, double seconds, uint32_t jitterUs = 0,
             const std::function<void(audio::Mixer&, uint64_t)>& each = {}) {
    audio::Mixer mixer;
    Run run;
    std::mt19937 random(11);
    std::uniform_int_distribution<uint32_t> jitter(0, jitterUs * 1000);
    struct State {
        double nextNs = 0;
        uint64_t phase = 0;
        size_t index = 0;
        bool lost = false;
    };
    std::vector<State> states(feeds.size());
    std::vector<float> samples;
    for (;;) {
        size_t next = 0;
        for (size_t i = 1; i < feeds.size(); ++i) {
            if (states[i].nextNs < states[next].nextNs) {
                next = i;
            }
        }
        auto& f = feeds[next];
        auto& s = states[next];
        if (s.nextNs >= seconds * 1e9) {
            break;
        }
        const auto dueNs = uint64_t(s.nextNs);
        const uint64_t arrivalNs = dueNs + jitter(random);
        const bool quiet = dueNs >= f.stopNs && dueNs < f.resumeNs;
        uint32_t frames = 0;
        for (uint32_t b = 0; b < f.burst; ++b) {
            frames += f.frames[s.index++ % f.frames.size()];
        }
        if (!quiet) {
            samples.resize(size_t(frames) * audioChannels);
            for (uint32_t i = 0; i < frames; ++i) {
                const auto v = float(f.level * std::sin(2 * pi * f.hz * double(s.phase + i) / audioRate));
                samples[i * 2] = samples[i * 2 + 1] = v;
            }
            if (dueNs >= f.loseNs && !s.lost) {
                s.lost = true; // Gone on the way: the next buffer goes on from where this one ended.
            } else {
                mixer.push(f.source, samples, float(f.gain), arrivalNs, run.out);
            }
        }
        s.phase += frames;
        s.nextNs += f.periodNs * f.burst / (1 + f.ppm * 1e-6);
        if (each) {
            each(mixer, arrivalNs);
        }
        if (arrivalNs > 1'000'000'000) {
            for (uint32_t i = 0; i < audio::Mixer::sources; ++i) {
                run.mostHeld[i] = std::max(run.mostHeld[i], mixer.counters().heldFrames[i]);
            }
        }
    }
    run.counters = mixer.counters();
    return run;
}

size_t frames(const Run& run) {
    return run.out.size() / audioChannels;
}

// One source goes out as it comes, at its volume, nothing held.
void alone() {
    audio::Mixer mixer;
    std::vector<float> in(1024 * audioChannels), out;
    for (size_t i = 0; i < in.size(); ++i) {
        in[i] = float(i % 200) / 400;
    }
    mixer.push(1, in, 0.5f, 1'000'000, out);
    CHECK(out.size() == in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        CHECK(out[i] == in[i] * 0.5f);
    }
    CHECK(mixer.counters().heldFrames[1] == 0);
    // Above full scale: held within it.
    out.clear();
    mixer.push(1, in, 4.0f, 22'000'000, out);
    CHECK(*std::max_element(out.begin(), out.end()) == 1.0f);
}

// ReplayKit's two sounds at their own lengths and paces, delivered up to 5 ms
// late: every frame of both, at its volume, nothing dropped, held about a buffer.
void together() {
    auto app = appFeed();
    auto mic = micFeed();
    mic.gain = 2;
    const auto run = simulate({app, mic}, 20, 5000);
    std::printf("together: %zu frames out, held at most %u / %u, %llu dropped\n", frames(run), run.mostHeld[0],
                run.mostHeld[1], (unsigned long long)run.counters.droppedFrames);
    CHECK(frames(run) > 19 * audioRate);
    CHECK(run.counters.droppedFrames == 0);
    CHECK(run.mostHeld[0] < 2400 && run.mostHeld[1] < 2400); // 50 ms.
    // From the second window: the applications' first buffer went out before the microphone's came.
    const auto [appLow, appHigh] = amplitudeRange(run.out, audioRate / 10, frames(run), appHz);
    const auto [micLow, micHigh] = amplitudeRange(run.out, audioRate / 10, frames(run), micHz);
    std::printf("together: application %.4f..%.4f, microphone %.4f..%.4f\n", appLow, appHigh, micLow, micHigh);
    CHECK(appLow > 0.297 && appHigh < 0.303);
    CHECK(micLow > 0.397 && micHigh < 0.403);
    // Loud ones: the mix stays within full scale.
    app.level = mic.level = 0.8;
    mic.gain = 1;
    const auto loud = simulate({app, mic}, 2);
    CHECK(*std::max_element(loud.out.begin(), loud.out.end()) <= 1.0f);
    CHECK(*std::min_element(loud.out.begin(), loud.out.end()) >= -1.0f);
}

// The microphone stops for 3 s, later the applications for 2 s: the other
// waits for it only briefly, then goes on alone; each comes back in full.
void stops() {
    auto app = appFeed();
    auto mic = micFeed();
    mic.stopNs = 5'000'000'000;
    mic.resumeNs = 8'000'000'000;
    app.stopNs = 12'000'000'000;
    app.resumeNs = 14'000'000'000;
    uint32_t heldAlone[2] = {};
    const auto run = simulate({app, mic}, 18, 2000, [&](audio::Mixer& mixer, uint64_t nowNs) {
        // A while after one stopped, the other is held no longer than its own buffer.
        if (nowNs > 5'100'000'000 && nowNs < 8'000'000'000) {
            heldAlone[0] = std::max(heldAlone[0], mixer.counters().heldFrames[0]);
        }
        if (nowNs > 12'100'000'000 && nowNs < 14'000'000'000) {
            heldAlone[1] = std::max(heldAlone[1], mixer.counters().heldFrames[1]);
        }
    });
    std::printf("stops: held at most %u / %u, alone %u / %u\n", run.mostHeld[0], run.mostHeld[1], heldAlone[0],
                heldAlone[1]);
    CHECK(heldAlone[0] == 0 && heldAlone[1] == 0);
    // Waiting for the one that stopped: at most its longest pause and 20 ms, and a buffer.
    CHECK(run.mostHeld[0] < 4000 && run.mostHeld[1] < 4000);
    const auto at = [](double s) { return size_t(s * audioRate); };
    CHECK(amplitudeRange(run.out, at(1), at(4.9), micHz).first > 0.19);
    CHECK(amplitudeRange(run.out, at(5.2), at(7.8), micHz).second < 0.001); // Gone, and nothing of it repeated.
    CHECK(amplitudeRange(run.out, at(5.2), at(7.8), appHz).first > 0.29);   // The other did not break up.
    CHECK(amplitudeRange(run.out, at(8.3), at(11.8), micHz).first > 0.19);  // Back.
    CHECK(amplitudeRange(run.out, at(12.2), at(13.8), appHz).second < 0.001);
    CHECK(amplitudeRange(run.out, at(12.2), at(13.8), micHz).first > 0.19);
    CHECK(amplitudeRange(run.out, at(14.3), at(17.5), appHz).first > 0.29);
}

// The microphone comes three buffers at a time (64 ms apart): after the first
// such pause it is not taken for stopped, and nothing of it is dropped.
void bursts() {
    auto mic = micFeed();
    mic.burst = 3;
    const auto run = simulate({appFeed(), mic}, 10, 1000);
    std::printf("bursts: held at most %u / %u, %llu dropped\n", run.mostHeld[0], run.mostHeld[1],
                (unsigned long long)run.counters.droppedFrames);
    CHECK(run.counters.droppedFrames == 0);
    const auto [low, high] = amplitudeRange(run.out, audioRate / 2, frames(run), micHz);
    CHECK(low > 0.19 && high < 0.21);
    CHECK(amplitudeRange(run.out, audioRate / 2, frames(run), appHz).first > 0.29);
}

// The microphone's clock 300 ppm fast for five minutes: it gets ahead of the
// applications' sound by 14 frames a second, and is set back now and then,
// so what it holds stays bounded. A lost buffer of the applications' sound
// leaves the microphone a buffer ahead: set back within two seconds.
void drift() {
    auto mic = micFeed();
    mic.ppm = 300;
    const auto run = simulate({appFeed(), mic}, 300, 3000);
    const double expected = 300e-6 * 300 * audioRate; // 4320 frames
    std::printf("drift: %llu frames dropped in %llu cuts (%.0f expected), held at most %u / %u\n",
                (unsigned long long)run.counters.droppedFrames, (unsigned long long)run.counters.drops, expected,
                run.mostHeld[0], run.mostHeld[1]);
    CHECK(run.counters.drops >= 4 && run.counters.drops <= 12);
    CHECK(std::abs(double(run.counters.droppedFrames) - expected) < 1100);
    CHECK(run.mostHeld[1] < 2400 + audioRate / 100);
    CHECK(amplitudeRange(run.out, audioRate, frames(run), appHz).first > 0.28);
    // The other way round: the applications' clock fast.
    auto app = appFeed();
    app.ppm = 300;
    const auto other = simulate({app, micFeed()}, 300, 3000);
    std::printf("drift the other way: %llu frames dropped in %llu cuts, held at most %u / %u\n",
                (unsigned long long)other.counters.droppedFrames, (unsigned long long)other.counters.drops,
                other.mostHeld[0], other.mostHeld[1]);
    CHECK(other.counters.drops >= 1 && other.mostHeld[0] < 2400 + audioRate / 100);

    auto lossy = appFeed();
    lossy.loseNs = 10'000'000'000;
    uint32_t held[3] = {}; // The most the microphone held before the loss, after it, and two seconds on.
    const auto lost = simulate({lossy, micFeed()}, 14, 2000, [&](audio::Mixer& mixer, uint64_t nowNs) {
        const int phase = nowNs < 8'000'000'000    ? -1
                          : nowNs < 10'000'000'000 ? 0
                          : nowNs < 10'900'000'000 ? 1
                          : nowNs > 12'000'000'000 ? 2
                                                   : -1;
        if (phase >= 0) {
            held[phase] = std::max(held[phase], mixer.counters().heldFrames[1]);
        }
    });
    std::printf("lost buffer: %llu frames dropped in %llu cuts; the microphone held at most %u, %u, then %u\n",
                (unsigned long long)lost.counters.droppedFrames, (unsigned long long)lost.counters.drops, held[0],
                held[1], held[2]);
    CHECK(lost.counters.drops >= 1 && lost.counters.drops <= 2);
    CHECK(lost.counters.droppedFrames >= 1000 && lost.counters.droppedFrames <= 1300);
    CHECK(held[1] > held[0] + 800 && held[2] < held[0] + 200);
}

// Left out: the other goes out at once, what it held is dropped; it joins again when pushed.
void leaving() {
    audio::Mixer mixer;
    std::vector<float> a(480 * audioChannels, 0.25f), b(960 * audioChannels, 0.5f), out;
    mixer.push(0, a, 1, 1'000'000, out); // Alone: out at once.
    CHECK(out.size() == a.size() && out.back() == 0.25f);
    mixer.push(1, b, 1, 2'000'000, out); // Joins: waits for the other.
    CHECK(out.size() == a.size() && mixer.counters().heldFrames[1] == 960);
    mixer.push(0, a, 1, 11'000'000, out);
    CHECK(out.size() == 2 * a.size() && out.back() == 0.75f && mixer.counters().heldFrames[1] == 480);
    mixer.push(1, b, 1, 12'000'000, out);
    CHECK(out.size() == 2 * a.size() && mixer.counters().heldFrames[1] == 1440);
    mixer.leave(0); // What the other holds goes with its next buffer.
    mixer.push(1, b, 1, 13'000'000, out);
    CHECK(out.size() == 2 * a.size() + 2400 * audioChannels && out.back() == 0.5f &&
          mixer.counters().heldFrames[1] == 0);
    mixer.push(0, a, 1, 14'000'000, out); // Joins again.
    mixer.push(1, b, 1, 15'000'000, out);
    CHECK(out.size() == 3 * a.size() + 2400 * audioChannels && out.back() == 0.75f &&
          mixer.counters().heldFrames[1] == 480);
    mixer.reset();
    CHECK(mixer.counters().mixedFrames == 0 && mixer.counters().heldFrames[1] == 0);
}

// Both stop, one with sound left waiting; the other comes back two seconds
// later: that stale sound is not mixed in.
void stale() {
    audio::Mixer mixer;
    std::vector<float> a(480 * audioChannels, 0.25f), b(960 * audioChannels, 0.5f), out;
    mixer.push(0, a, 1, 1'000'000, out);
    mixer.push(1, b, 1, 2'000'000, out); // Waits for the other, which stops.
    mixer.push(0, a, 1, 2'002'000'000, out);
    CHECK(out.size() == 2 * a.size() && out.back() == 0.25f && mixer.counters().heldFrames[1] == 0 &&
          mixer.counters().droppedFrames == 960);
}

#ifdef __APPLE__
// Linear PCM as Core Media carries it.
enum class Pcm { int16, int16BigEndian, float32 };

// A sample buffer of `frames` of a tone of `hz` at `level`: `rate`, `channels`,
// `pcm`, interleaved or not. `phase` counts on across buffers.
CMSampleBufferRef soundSample(double rate, uint32_t channels, Pcm pcm, bool interleaved, uint32_t frames, double hz,
                              double level, uint64_t& phase) {
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    const uint32_t bytes = pcm == Pcm::float32 ? 4 : 2;
    format.mFormatFlags = (pcm == Pcm::float32 ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) |
                          kAudioFormatFlagIsPacked | (pcm == Pcm::int16BigEndian ? kAudioFormatFlagIsBigEndian : 0) |
                          (interleaved ? 0 : kAudioFormatFlagIsNonInterleaved);
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = bytes * 8;
    format.mBytesPerFrame = format.mBytesPerPacket = interleaved ? bytes * channels : bytes;
    format.mFramesPerPacket = 1;
    CMAudioFormatDescriptionRef description = nullptr;
    CHECK(CMAudioFormatDescriptionCreate(nullptr, &format, 0, nullptr, 0, nullptr, nullptr, &description) == noErr);
    // Channel after channel when not interleaved: one block, each channel's buffer after the last.
    std::vector<uint8_t> data(size_t(frames) * channels * bytes);
    for (uint32_t i = 0; i < frames; ++i) {
        const double value = level * std::sin(2 * pi * hz * double(phase + i) / rate);
        for (uint32_t c = 0; c < channels; ++c) {
            uint8_t* at = data.data() + (interleaved ? (size_t(i) * channels + c) * bytes
                                                     : (size_t(c) * frames + i) * bytes);
            if (pcm == Pcm::float32) {
                const auto f = float(value);
                std::memcpy(at, &f, 4);
            } else {
                auto s = uint16_t(int16_t(std::lround(value * 32767)));
                s = pcm == Pcm::int16BigEndian ? OSSwapHostToBigInt16(s) : s;
                std::memcpy(at, &s, 2);
            }
        }
    }
    CMBlockBufferRef block = nullptr;
    CHECK(CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, data.size(), kCFAllocatorDefault, nullptr, 0,
                                             data.size(), 0, &block) == noErr);
    CHECK(CMBlockBufferReplaceDataBytes(data.data(), block, 0, data.size()) == noErr);
    CMSampleBufferRef sample = nullptr;
    CHECK(CMAudioSampleBufferCreateReadyWithPacketDescriptions(nullptr, block, description, frames,
                                                               CMTimeMake(int64_t(phase), int32_t(rate)), nullptr,
                                                               &sample) == noErr);
    CFRelease(block);
    CFRelease(description);
    phase += frames;
    return sample;
}

// What a host hands over: the format of each buffer of a sound.
struct Kind {
    const char* name;
    double rate;
    uint32_t channels;
    Pcm pcm;
    bool interleaved;
    uint32_t frames; // Per buffer.
};
constexpr Kind replayKitApp{"ReplayKit's applications' sound", 44100, 2, Pcm::int16BigEndian, true, 1024};
constexpr Kind replayKitMic{"ReplayKit's microphone", 48000, 1, Pcm::int16, true, 1024};
constexpr Kind captureKit{"ScreenCaptureKit's sound", 48000, 2, Pcm::float32, false, 960};
constexpr Kind narrowMic{"a 16 kHz microphone", 16000, 1, Pcm::float32, true, 320};

// Each kind converted on its own: 48 kHz stereo, the tone's pitch and level kept.
void conversions() {
    for (const auto& kind : {replayKitApp, replayKitMic, captureKit, narrowMic}) {
        auto sound = platform::createSampleSound();
        std::vector<float> out;
        uint64_t phase = 0;
        const uint32_t buffers = uint32_t(2 * kind.rate / kind.frames); // Two seconds.
        for (uint32_t i = 0; i < buffers; ++i) {
            CMSampleBufferRef sample = soundSample(kind.rate, kind.channels, kind.pcm, kind.interleaved, kind.frames,
                                                   appHz, 0.4, phase);
            sound->convert(sample, out);
            CFRelease(sample);
        }
        const double seconds = double(buffers) * kind.frames / kind.rate;
        const size_t converted = out.size() / audioChannels;
        const double a = amplitude(out, audioRate / 2, audioRate, appHz);
        const double right = [&] { // Mono on both channels.
            double sum = 0;
            for (size_t i = audioRate / 2; i < audioRate; ++i) {
                sum += std::abs(out[i * 2] - out[i * 2 + 1]);
            }
            return sum;
        }();
        std::printf("%s: %zu frames for %.3f s, amplitude %.4f\n", kind.name, converted, seconds, a);
        CHECK(std::abs(double(converted) - seconds * audioRate) < 64);
        CHECK(std::abs(a - 0.4) < 0.004);
        CHECK(right < 1e-3);
    }
}

// ReplayKit's two sounds in their own formats, converted each on its own and
// mixed in simulated time: the microphone's clock far off (1000 ppm fast),
// the microphone stopping for two seconds, the applications' for one.
void replayKit() {
    auto app = platform::createSampleSound(), mic = platform::createSampleSound();
    audio::Mixer mixer;
    std::vector<float> out, converted;
    uint64_t appPhase = 0, micPhase = 0;
    const double appPeriod = 1024e9 / 44100, micPeriod = 1024e9 / 48000 / (1 + 1000e-6);
    double appNext = 0, micNext = 3e6;
    uint32_t mostHeld = 0;
    const auto quiet = [](double ns, double from, double to) { return ns >= from * 1e9 && ns < to * 1e9; };
    while (appNext < 60e9 || micNext < 60e9) {
        const bool isApp = appNext <= micNext;
        double& next = isApp ? appNext : micNext;
        const auto now = uint64_t(next);
        const bool stopped = isApp ? quiet(next, 40, 41) : quiet(next, 20, 22);
        const Kind& kind = isApp ? replayKitApp : replayKitMic;
        CMSampleBufferRef sample = soundSample(kind.rate, kind.channels, kind.pcm, kind.interleaved, kind.frames,
                                               isApp ? appHz : micHz, isApp ? 0.3 : 0.2, isApp ? appPhase : micPhase);
        if (!stopped) {
            converted.clear();
            (isApp ? app : mic)->convert(sample, converted);
            mixer.push(isApp ? 0 : 1, converted, 1, now, out);
        }
        CFRelease(sample);
        next += isApp ? appPeriod : micPeriod;
        if (now > 1'000'000'000) {
            mostHeld = std::max({mostHeld, mixer.counters().heldFrames[0], mixer.counters().heldFrames[1]});
        }
    }
    const auto c = mixer.counters();
    std::printf("ReplayKit's sounds: %zu frames out, %llu dropped in %llu cuts, held at most %u\n",
                out.size() / audioChannels, (unsigned long long)c.droppedFrames, (unsigned long long)c.drops,
                mostHeld);
    CHECK(mostHeld < 4000);
    CHECK(c.drops >= 1);
    const auto at = [](double s) { return size_t(s * audioRate); };
    CHECK(amplitudeRange(out, at(1), at(19.9), appHz).first > 0.29);
    CHECK(amplitudeRange(out, at(1), at(19.9), micHz).first > 0.1); // A cut (its clock is far off) dips a window.
    CHECK(amplitudeRange(out, at(20.2), at(21.8), micHz).second < 0.002);
    CHECK(amplitudeRange(out, at(20.2), at(21.8), appHz).first > 0.29);
    CHECK(amplitudeRange(out, at(22.3), at(39.8), micHz).first > 0.1);
    CHECK(amplitudeRange(out, at(40.2), at(40.8), micHz).first > 0.1);
}

// The session's host sound: AUDIO and MICROPHONE buffers at the volumes of
// rcs_send_audio_config, 0 leaving one out (its buffers taken, nothing of it sent).
void hostAudio() {
    session::HostAudio host;
    rcs_send_audio_config settings = RCS_SEND_AUDIO_CONFIG_INIT;
    uint64_t appPhase = 0, micPhase = 0;
    // `seconds` of both in the order they would come (as fast as they can be pushed).
    const auto pushBoth = [&](double seconds) {
        const uint64_t appEnd = appPhase + uint64_t(seconds * 44100), micEnd = micPhase + uint64_t(seconds * 48000);
        while (appPhase < appEnd || micPhase < micEnd) {
            const bool isApp = double(appPhase) / 44100 <= double(micPhase) / 48000;
            CMSampleBufferRef sample =
                isApp ? soundSample(44100, 2, Pcm::int16BigEndian, true, 1024, appHz, 0.3, appPhase)
                      : soundSample(48000, 1, Pcm::int16, true, 1024, micHz, 0.2, micPhase);
            const std::unique_ptr<const void, decltype(&CFRelease)> held(sample, CFRelease);
            host.pushSample(sample, isApp ? RCS_SAMPLE_BUFFER_AUDIO : RCS_SAMPLE_BUFFER_MICROPHONE, settings);
        }
    };
    CHECK(throws<Failure>([&] { pushBoth(0.01); })); // No sound is being sent.
    std::vector<float> sent;
    auto capture = host.capture();
    capture->start(
        {}, [&](std::span<const float> samples, uint64_t) { sent.insert(sent.end(), samples.begin(), samples.end()); },
        [](const platform::AudioStatus&) {});
    pushBoth(2);
    const auto heard = [&](double hz) {
        return amplitude(sent, sent.size() / audioChannels - audioRate, audioRate, hz);
    };
    std::printf("host sound, both: %zu frames, application %.3f, microphone %.3f\n", sent.size() / audioChannels,
                heard(appHz), heard(micHz));
    CHECK(sent.size() / audioChannels > 2 * audioRate - 2400 && std::abs(heard(appHz) - 0.3) < 0.01 &&
          std::abs(heard(micHz) - 0.2) < 0.01);
    settings.sample_buffer_microphone_percent = 0; // The applications' sound alone.
    sent.clear();
    pushBoth(2);
    std::printf("host sound, application alone: %.3f, microphone %.4f\n", heard(appHz), heard(micHz));
    CHECK(std::abs(heard(appHz) - 0.3) < 0.01 && heard(micHz) < 0.001);
    settings.sample_buffer_microphone_percent = 200; // The microphone alone, twice as loud.
    settings.sample_buffer_audio_percent = 0;
    sent.clear();
    pushBoth(2);
    std::printf("host sound, microphone alone: %.3f, application %.4f\n", heard(micHz), heard(appHz));
    CHECK(std::abs(heard(micHz) - 0.4) < 0.01 && heard(appHz) < 0.001);
    capture->stop();
    CHECK(throws<Failure>([&] { pushBoth(0.01); }));
}
#endif

} // namespace

int main() {
    return runTest("mixer", [] {
        alone();
        together();
        stops();
        bursts();
        drift();
        leaving();
        stale();
#ifdef __APPLE__
        conversions();
        replayKit();
        hostAudio();
#endif
    });
}
