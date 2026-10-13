// Sound in a browser: a thread of the SDK's fills a ring in the module's
// shared memory from the player's `fill` (the playout's jitter buffer and
// drift correction, as on every system), and web/rcsdk_audio.js, an
// AudioWorklet of the page's AudioContext, plays from it 128 frames at a
// time. The ring adds about 10 ms. The page's context plays once the user
// has interacted with it (browsers' autoplay rule); until then the sound is
// paused with that reason.
#include "platform/web/web.hpp"

#include <emscripten.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace rcsdk::platform {
namespace {

EM_JS(void, jsAudioStart, (int id, void* ring, int capacity), { Module.rcsdk.audioStart(id, ring, capacity); });
EM_JS(void, jsAudioStop, (int id), { Module.rcsdk.audioStop(id); });
EM_JS(void, jsMicrophoneStart, (int id, void* ring, int capacity, int voice),
      { Module.rcsdk.microphoneStart(id, ring, capacity, voice); });
EM_JS(void, jsMicrophoneStop, (int id), { Module.rcsdk.microphoneStop(id); });

constexpr uint32_t capacity = 4096;    // Frames the ring holds (85 ms).
constexpr uint32_t level = 512;        // Frames kept in it (10.7 ms): four of the worklet's turns.
constexpr uint32_t chunk = 128;        // What the worklet takes at a time.

// The ring as both sides see it: positions in frames (they wrap at 2^32),
// the worklet's underruns, then stereo samples.
struct Ring {
    std::atomic<uint32_t> write{0}, read{0}, underruns{0}, pad{0};
    float samples[capacity * 2];
};
static_assert(sizeof(std::atomic<uint32_t>) == 4);

std::mutex playersMutex;
std::map<int, AudioPlayer::Status> players; // For the page's word on its context.
int nextPlayer = 1;

class WebAudioPlayer final : public AudioPlayer {
public:
    ~WebAudioPlayer() override { stop(); }

    void start(const PlaybackConfig&, Fill fill, Status status) override {
        stop();
        ring_ = new Ring;
        {
            std::lock_guard lock(playersMutex);
            id_ = nextPlayer++;
            players[id_] = std::move(status);
        }
        onPage([id = id_, ring = ring_] { jsAudioStart(id, ring, int(capacity)); });
        stopping_ = false;
        feeder_ = std::thread([this, fill = std::move(fill)] {
            std::vector<float> block(capacity * 2);
            while (!stopping_) {
                const uint32_t written = ring_->write.load(), held = written - ring_->read.load();
                if (held < level) {
                    const uint32_t frames = (level - held + chunk - 1) / chunk * chunk;
                    fill(block.data(), frames);
                    for (uint32_t i = 0; i < frames; ++i) {
                        const uint32_t at = (written + i) % capacity * 2;
                        ring_->samples[at] = block[i * 2];
                        ring_->samples[at + 1] = block[i * 2 + 1];
                    }
                    ring_->write.store(written + frames);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }

    void stop() noexcept override {
        if (!ring_) {
            return;
        }
        stopping_ = true;
        if (feeder_.joinable()) {
            feeder_.join();
        }
        {
            std::lock_guard lock(playersMutex);
            players.erase(id_);
        }
        // The worklet lets go of the ring on the page's thread; it is freed there, after.
        onPage([id = id_, ring = ring_] {
            jsAudioStop(id);
            delete ring;
        });
        ring_ = nullptr;
    }

    Device device() const override {
        Device d;
        d.name = ring_ ? "Web Audio (AudioWorklet)" : "";
        d.periodFrames = chunk;
        d.queueFrames = level;
        d.underruns = ring_ ? ring_->underruns.load() : 0;
        return d;
    }

private:
    int id_ = 0;
    Ring* ring_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::thread feeder_;
};

// A microphone: the page's getUserMedia (the browser's echo cancellation and
// noise suppression with voiceProcessing) into rcsdk_audio.js's recorder,
// which fills a ring the SDK's thread takes 5 ms at a time from.
std::map<int, AudioCapture::Status> microphones;
std::atomic<int> microphonePermissionState{RCS_PERMISSION_REQUIRED};
constexpr uint32_t captureFrames = 240;

class WebMicrophone final : public AudioCapture {
public:
    explicit WebMicrophone(const MicrophoneConfig& config) : voice_(config.voiceProcessing) {}
    ~WebMicrophone() override { stop(); }

    void start(const AudioCaptureConfig&, Deliver deliver, Status status) override {
        stop();
        ring_ = new Ring;
        {
            std::lock_guard lock(playersMutex);
            id_ = nextPlayer++;
            microphones[id_] = std::move(status);
        }
        onPage([id = id_, ring = ring_, voice = voice_] { jsMicrophoneStart(id, ring, int(capacity), voice); });
        stopping_ = false;
        taker_ = std::thread([this, deliver = std::move(deliver)] {
            std::vector<float> block(captureFrames * 2);
            while (!stopping_) {
                uint32_t read = ring_->read.load();
                while (ring_->write.load() - read >= captureFrames) {
                    for (uint32_t i = 0; i < captureFrames; ++i) {
                        const uint32_t at = (read + i) % capacity * 2;
                        block[i * 2] = ring_->samples[at];
                        block[i * 2 + 1] = ring_->samples[at + 1];
                    }
                    read += captureFrames;
                    ring_->read.store(read);
                    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                      std::chrono::steady_clock::now().time_since_epoch())
                                                      .count());
                    deliver(block, now - uint64_t(captureFrames) * 1'000'000'000 / 48'000);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }

    void stop() noexcept override {
        if (!ring_) {
            return;
        }
        stopping_ = true;
        if (taker_.joinable()) {
            taker_.join();
        }
        {
            std::lock_guard lock(playersMutex);
            microphones.erase(id_);
        }
        onPage([id = id_, ring = ring_] {
            jsMicrophoneStop(id);
            delete ring;
        });
        ring_ = nullptr;
    }

    uint32_t periodFrames() const noexcept override { return ring_ ? captureFrames : 0; }

private:
    bool voice_;
    int id_ = 0;
    Ring* ring_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::thread taker_;
};

} // namespace

std::unique_ptr<AudioPlayer> createAudioPlayer() {
    return std::make_unique<WebAudioPlayer>();
}

std::unique_ptr<AudioCapture> createMicrophoneCapture(const MicrophoneConfig& config) {
    if (config.device > 1) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "A browser offers its default microphone only (device 0 or 1)");
    }
    return std::make_unique<WebMicrophone>(config);
}

std::vector<AudioDevice> audioDevices() {
    return {{1, true, true, "The browser's microphone"}};
}

rcs_permission microphonePermission() noexcept {
    return rcs_permission(microphonePermissionState.load());
}

// The page asks the user when the microphone starts (getUserMedia); here is only the answer so far.
rcs_result requestMicrophonePermission() {
    return microphonePermission() == RCS_PERMISSION_GRANTED ? RCS_OK : RCS_ERROR_PERMISSION_REQUIRED;
}

} // namespace rcsdk::platform

// A microphone of the page's: capturing (1), or not (0) and why: result and
// message as the page found them (the user's refusal, no device).
extern "C" EMSCRIPTEN_KEEPALIVE void rcsdk_web_microphone_state(int id, int capturing, int result, const char* message) {
    using namespace rcsdk::platform;
    AudioCapture::Status status;
    {
        std::lock_guard lock(playersMutex);
        const auto found = microphones.find(id);
        if (found == microphones.end()) {
            return;
        }
        status = found->second;
    }
    AudioStatus s;
    s.active = capturing != 0;
    s.voiceProcessing = s.active;
    if (!s.active) {
        s.result = result;
        s.message = message ? message : "";
    }
    status(s);
}

// What the browser says of the microphone permission (navigator.permissions), as rcs_permission.
extern "C" EMSCRIPTEN_KEEPALIVE void rcsdk_web_microphone_permission(int permission) {
    rcsdk::platform::microphonePermissionState = permission;
}

// The page's AudioContext for player `id`: playing, or held back until the
// user interacts with the page (on the page's main thread).
extern "C" EMSCRIPTEN_KEEPALIVE void rcsdk_web_audio_state(int id, int playing) {
    using namespace rcsdk::platform;
    AudioPlayer::Status status;
    {
        std::lock_guard lock(playersMutex);
        const auto found = players.find(id);
        if (found == players.end()) {
            return;
        }
        status = found->second;
    }
    AudioStatus s;
    s.active = playing != 0;
    if (!s.active) {
        s.result = RCS_ERROR_PERMISSION_REQUIRED;
        s.message = "The browser plays sound once the user has interacted with the page";
    }
    status(s);
}
