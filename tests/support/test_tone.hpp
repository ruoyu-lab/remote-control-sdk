// An audio capture replacement producing a sine tone in real time, installed
// through rcsdk::seams::setAudioCaptureFactory: tests never touch the
// system's sound.
#pragma once

#include "platform/platform.hpp"
#include "seams/seams.hpp"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace rcsdk::test {

inline constexpr double toneHz = 1000;
inline constexpr float toneAmplitude = 0.5f;

// `hz`; `voiceProcessing`: what a microphone says of its processing.
class ToneCapture final : public platform::AudioCapture {
public:
    explicit ToneCapture(double hz = toneHz, bool voiceProcessing = false) : hz_(hz), processing_(voiceProcessing) {}
    ~ToneCapture() override { stop(); }

    void start(const platform::AudioCaptureConfig&, Deliver deliver, Status status) override {
        thread_ = std::thread([this, deliver = std::move(deliver), status = std::move(status)] {
            status({true, RCS_OK, {}, processing_});
            constexpr uint32_t block = 480; // 10 ms, as system capture delivers.
            std::vector<float> samples(block * audioChannels);
            uint64_t produced = 0;
            auto next = std::chrono::steady_clock::now();
            while (!stopping_) {
                for (uint32_t i = 0; i < block; ++i) {
                    const auto value = float(toneAmplitude * std::sin(2 * 3.14159265358979 * hz_ *
                                                                      double(produced + i) / audioRate));
                    samples[i * 2] = samples[i * 2 + 1] = value;
                }
                deliver(samples, monotonicNs());
                produced += block;
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
            }
        });
    }
    uint32_t periodFrames() const noexcept override { return 480; }
    void stop() noexcept override {
        stopping_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    const double hz_;
    const bool processing_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
};

struct UseTestTone {
    UseTestTone() {
        seams::setAudioCaptureFactory([] { return std::make_unique<ToneCapture>(); });
    }
    ~UseTestTone() { seams::setAudioCaptureFactory({}); }
};

// Microphones are the test's too: a tone of `hz`, with voice processing.
struct UseTestMicrophone {
    explicit UseTestMicrophone(double hz) {
        seams::setMicrophoneFactory([hz] { return std::make_unique<ToneCapture>(hz, true); });
    }
    ~UseTestMicrophone() { seams::setMicrophoneFactory({}); }
};

// An output device that makes no sound: it pulls 256 frames (5.3 ms, as
// macOS's default period) in real time and drops them, so that the playback
// buffer runs as with a real device (its underruns, its target) and nothing
// is heard.
class SilentPlayer final : public platform::AudioPlayer {
public:
    ~SilentPlayer() override { stop(); }
    void start(const platform::PlaybackConfig&, Fill fill, Status status) override {
        thread_ = std::thread([this, fill = std::move(fill), status = std::move(status)] {
            status({true, RCS_OK, {}});
            std::vector<float> samples(period * audioChannels);
            auto next = std::chrono::steady_clock::now();
            while (!stopping_) {
                fill(samples.data(), period);
                next += std::chrono::nanoseconds(uint64_t(period) * 1'000'000'000 / audioRate);
                std::this_thread::sleep_until(next);
            }
        });
    }
    void stop() noexcept override {
        stopping_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    Device device() const override { return {"silent test device", period, 0, 0}; }

private:
    static constexpr uint32_t period = 256;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
};

// Viewers play into silent devices for the lifetime of this object.
struct UseSilentPlayer {
    UseSilentPlayer() {
        seams::setAudioPlayerFactory([] { return std::make_unique<SilentPlayer>(); });
    }
    ~UseSilentPlayer() { seams::setAudioPlayerFactory({}); }
};

// The dominant frequency of a mono signal from its zero crossings, and its RMS.
struct ToneMeasure {
    double hz = 0, rms = 0;
};
inline ToneMeasure measureTone(const std::vector<float>& samples) {
    ToneMeasure m;
    if (samples.size() < 2) {
        return m;
    }
    size_t crossings = 0;
    double energy = 0;
    for (size_t i = 1; i < samples.size(); ++i) {
        crossings += (samples[i - 1] < 0) != (samples[i] < 0);
        energy += double(samples[i]) * samples[i];
    }
    m.hz = double(crossings) / 2 / (double(samples.size()) / audioRate);
    m.rms = std::sqrt(energy / double(samples.size()));
    return m;
}

} // namespace rcsdk::test
