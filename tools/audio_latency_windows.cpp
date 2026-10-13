// Sound latency through the SDK on one Windows machine, measured on one clock.
//
// This process runs a sharer and a viewer over UDP on 127.0.0.1, plus a
// recorder: plain WASAPI loopback of the default output device, which hears
// everything the device plays. Another process (not a child: started by the
// Task Scheduler, so the sharer's process loopback includes it) plays short
// clicks. The recorder hears each click twice: as played, and as the viewer
// plays it after the SDK captured, encoded, sent, decoded and buffered it.
// The gap is the SDK's latency up to the device mix; the device's own output
// latency (DAC, speakers) comes on top for any application alike.
//
// The sharer leaves this process's sound out, so the viewer's playback is
// not captured again: no echo loop.
//
// usage: rcsdk_audio_latency CLICK_TASK [--seconds N] [--codec opus|aac|pcm]
//          [--playback-period N] [--queue N] [--margin-us N]
//   CLICK_TASK  a scheduled task that plays the clicks, e.g. RCS-Click
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/tuning.hpp>

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr uint32_t rate = 48000;

template <class T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// Mono (left) samples of everything the default output device plays.
class Recorder {
public:
    bool start() {
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator))) ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_)))) {
            return false;
        }
        release(device);
        release(enumerator);
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = rate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 8;
        format.nAvgBytesPerSec = rate * 8;
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                           AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                       1'000'000, 0, &format, nullptr)) ||
            FAILED(client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_))) ||
            FAILED(client_->Start())) {
            return false;
        }
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() {
        stopping_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
        if (client_) {
            client_->Stop();
        }
        release(capture_);
        release(client_);
    }
    const std::vector<float>& samples() const { return samples_; }

private:
    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uint64_t lastPosition = 0;
        while (!stopping_) {
            std::this_thread::sleep_for(2ms);
            UINT32 frames = 0;
            while (SUCCEEDED(capture_->GetNextPacketSize(&frames)) && frames) {
                BYTE* data = nullptr;
                DWORD flags = 0;
                UINT64 position = 0;
                if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &position, nullptr))) {
                    break;
                }
                // Loopback skips silent stretches: keep the timeline by device position.
                if (lastPosition && position > lastPosition) {
                    samples_.resize(samples_.size() + size_t(position - lastPosition), 0.0f);
                }
                const auto* stereo = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    samples_.push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : stereo[i * 2]);
                }
                lastPosition = position + frames;
                capture_->ReleaseBuffer(frames);
            }
        }
        CoUninitialize();
    }

    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::vector<float> samples_;
};

// Onsets: the first sample above `threshold` after at least `quiet` samples below it.
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
    if (argc < 2) {
        std::fprintf(stderr, "usage: rcsdk_audio_latency CLICK_TASK [SECONDS] [opus|aac|pcm]\n");
        return 2;
    }
    const std::string task = argv[1];
    int seconds = 15;
    std::string codecName = "opus";
    uint32_t playbackPeriod = 0, queue = 0, marginUs = 2000;
    for (int i = 2; i < argc; ++i) {
        const std::string name = argv[i];
        const std::string value = i + 1 < argc ? argv[i + 1] : "";
        ++i;
        if (name == "--seconds") {
            seconds = std::atoi(value.c_str());
        } else if (name == "--codec") {
            codecName = value;
        } else if (name == "--playback-period") {
            playbackPeriod = uint32_t(std::atoi(value.c_str()));
        } else if (name == "--queue") {
            queue = uint32_t(std::atoi(value.c_str()));
        } else if (name == "--margin-us") {
            marginUs = uint32_t(std::atoi(value.c_str()));
        }
    }
    const rcs_audio_codec codec = codecName == "aac" ? RCS_AUDIO_AAC : codecName == "pcm" ? RCS_AUDIO_PCM : RCS_AUDIO_OPUS;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    rcsdk::setLogCallback(RCS_LOG_DEBUG, [](const rcs_log_record& record) {
        if (std::string(record.module) == "audio") {
            std::fprintf(stderr, "[%s] %s\n", record.module, record.message);
        }
    });

    uint8_t key[32];
    rcs_generate_connection_key(key, sizeof(key));
    rcsdk::SessionConfig shareConfig(RCS_ROLE_SHARE), viewConfig(RCS_ROLE_VIEW);
    for (auto* c : {&shareConfig, &viewConfig}) {
        c->setAddress("127.0.0.1", 47960).setKey(key).setTransport(RCS_TRANSPORT_UDP).setAudioCodecs({codec});
    }
    shareConfig.setVideo(640, 360, 30, 2'000'000).setExcludeOwnSound(true);
    auto& playbackTuning = viewConfig.extension<rcs_audio_tuning_config>();
    playbackTuning.buffer_margin_us = marginUs;
    playbackTuning.playback_period_frames = playbackPeriod;
    playbackTuning.playback_queue_frames = queue;
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
    if (!share.stats<rcs_audio_send_stats>().active || !view.stats<rcs_audio_receive_stats>().active) {
        std::fprintf(stderr, "audio is not active (share %u, view %u)\n", share.stats<rcs_audio_send_stats>().active,
                     view.stats<rcs_audio_receive_stats>().active);
        return 1;
    }
    Recorder recorder;
    if (!recorder.start()) {
        std::fprintf(stderr, "the loopback recorder did not start\n");
        return 1;
    }
    std::this_thread::sleep_for(500ms);
    const std::string command = "schtasks /run /tn " + task;
    std::system((command + " > nul").c_str());
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const auto received = view.stats<rcs_audio_receive_stats>();
    recorder.stop();
    view.stop();
    share.stop();

    // Clicks come 1 s apart; each is heard first as played, then from the viewer.
    const auto& s = recorder.samples();
    const auto found = onsets(s, 0.01f, rate / 4);
    std::vector<double> latencies;
    for (const size_t click : found) {
        // The replay: the first loud sample after the click's own few milliseconds.
        const size_t from = click + rate / 200, to = std::min(s.size(), click + rate / 3);
        for (size_t i = from; i < to; ++i) {
            if (std::fabs(s[i]) > 0.01f) {
                latencies.push_back(double(i - click) * 1000 / rate);
                break;
            }
        }
    }
    std::printf("codec %s, playback period %u, queue %u, margin %u us: recorded %.1f s, %zu clicks, %zu replays\n",
                codecName.c_str(), received.playback_period_frames, received.playback_queue_frames, marginUs,
                double(s.size()) / rate, found.size(), latencies.size());
    if (latencies.empty()) {
        return 1;
    }
    std::sort(latencies.begin(), latencies.end());
    std::printf("latency to the device mix: min %.1f ms, median %.1f ms, max %.1f ms\n", latencies.front(),
                latencies[latencies.size() / 2], latencies.back());
    std::printf("viewer buffer %u ms (target %u ms), underruns %llu, dropped %llu ms, concealed %llu ms\n",
                received.buffer_ms, received.target_ms, (unsigned long long)received.underruns,
                (unsigned long long)received.dropped_ms, (unsigned long long)received.concealed_ms);
    return 0;
}
