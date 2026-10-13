// Sound on Android with AAudio: playback on the output the system routes media
// to, microphones as AAudio inputs (the voice communication preset: the
// system cancels echo and suppresses noise), each stream opened again when its
// device goes. The device list and the microphone permission come from Java
// (rcs_android_initialize). Android lets an application capture what others
// play only through MediaProjection in Java (AudioPlaybackCapture): the host
// pushes it (RCS_AUDIO_HOST). AAC is aac.cpp.
#include "platform/android/jni.hpp"
#include "platform/platform.hpp"
#include "util/log.hpp"

#include <aaudio/AAudio.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace rcsdk::platform {
namespace {

using namespace std::chrono_literals;

std::string aaudioText(aaudio_result_t result) {
    return std::string(AAudio_convertResultToText(result)) + " (" + std::to_string(result) + ")";
}

// The name of the device AAudio's `id` is, as the device list has it.
std::string deviceName(int32_t id, bool input) {
    try {
        for (const auto& device : audioDevices()) {
            if (device.id == uint32_t(id) && device.input == input) {
                return device.name;
            }
        }
    } catch (const Failure&) { // No Java: the device has no name here.
    }
    return std::string(input ? "Microphone " : "Output ") + std::to_string(id);
}

// An AAudio stream kept open: opened at start, opened again (on a thread of
// its own, as AAudio wants) when its device goes or opening failed, until stop.
class Stream {
public:
    virtual ~Stream() { halt(); }

protected:
    // Opens and starts the stream, or throws Failure saying why not.
    virtual AAudioStream* open() = 0;
    virtual void report(const AudioStatus& status) = 0;

    void begin() {
        running_ = true;
        thread_ = std::thread([this] { keep(); });
    }

    void halt() noexcept {
        {
            std::lock_guard lock(mutex_);
            running_ = false;
            changed_.notify_all();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Whether a stream is open now. Only the keep thread and the stream's own
    // callbacks use it: it may be closed at any moment.
    bool streaming() const noexcept { return stream_.load() != nullptr; }

    static void lost(AAudioStream*, void* self, aaudio_result_t error) {
        auto* stream = static_cast<Stream*>(self);
        std::lock_guard lock(stream->mutex_);
        stream->error_ = error;
        stream->changed_.notify_all();
    }

private:
    void keep() {
        std::unique_lock lock(mutex_);
        while (running_) {
            lock.unlock();
            AudioStatus status;
            try {
                stream_ = open();
                status = activeStatus();
            } catch (const Failure& failure) {
                status = {false, failure.result(), failure.what()};
            }
            reportChange(status);
            lock.lock();
            // Open: until its device goes. Not: again in a second.
            const auto ended = [&] {
                return !running_ || error_ != AAUDIO_OK;
            };
            if (stream_) {
                changed_.wait(lock, ended);
            } else {
                changed_.wait_for(lock, 1s, ended);
            }
            if (AAudioStream* open = stream_.exchange(nullptr)) {
                const bool running = running_;
                const aaudio_result_t error = error_;
                lock.unlock(); // Closing waits for the callbacks; the error callback takes the lock.
                AAudioStream_requestStop(open);
                AAudioStream_close(open);
                if (running) {
                    reportChange({false, RCS_ERROR_DEVICE, "The sound device went (" + aaudioText(error) + ")"});
                }
                lock.lock();
            }
            error_ = AAUDIO_OK;
        }
    }

    // A failure that stays is reported once.
    void reportChange(const AudioStatus& status) {
        if (status.active || status.result != reported_.result || status.message != reported_.message) {
            reported_ = status;
            report(status);
        }
    }

protected:
    virtual AudioStatus activeStatus() { return {true, RCS_OK, {}}; }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool running_ = false;
    aaudio_result_t error_ = AAUDIO_OK;
    AudioStatus reported_;
    std::atomic<AAudioStream*> stream_{nullptr};
    std::thread thread_;
};

struct BuilderDeleter {
    void operator()(AAudioStreamBuilder* builder) const noexcept { AAudioStreamBuilder_delete(builder); }
};
using Builder = std::unique_ptr<AAudioStreamBuilder, BuilderDeleter>;

Builder builder() {
    AAudioStreamBuilder* value = nullptr;
    if (const aaudio_result_t result = AAudio_createStreamBuilder(&value); result != AAUDIO_OK) {
        fail(RCS_ERROR_DEVICE, "AAudio cannot make streams: " + aaudioText(result));
    }
    return Builder(value);
}

// Opens and starts what `settings` describes, at 48 kHz float32 and the
// channels asked for, or throws Failure.
AAudioStream* openStream(AAudioStreamBuilder* settings, const char* what, int32_t channels) {
    AAudioStream* stream = nullptr;
    if (const aaudio_result_t result = AAudioStreamBuilder_openStream(settings, &stream); result != AAUDIO_OK) {
        fail(RCS_ERROR_DEVICE, std::string("Opening the ") + what + " failed: " + aaudioText(result));
    }
    if (AAudioStream_getSampleRate(stream) != audioRate || AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_FLOAT ||
        AAudioStream_getChannelCount(stream) != channels) {
        const std::string got = std::to_string(AAudioStream_getSampleRate(stream)) + " Hz, " +
                                std::to_string(AAudioStream_getChannelCount(stream)) + " channels";
        AAudioStream_close(stream);
        fail(RCS_ERROR_UNSUPPORTED, std::string("The ") + what + " runs at " + got + ", not 48000 Hz float32");
    }
    if (const aaudio_result_t result = AAudioStream_requestStart(stream); result != AAUDIO_OK) {
        AAudioStream_close(stream);
        fail(RCS_ERROR_DEVICE, std::string("Starting the ") + what + " failed: " + aaudioText(result));
    }
    return stream;
}

class AAudioPlayer final : public AudioPlayer, Stream {
public:
    ~AAudioPlayer() override { stop(); }

    void start(const PlaybackConfig& config, Fill fill, Status status) override {
        config_ = config;
        fill_ = std::move(fill);
        status_ = std::move(status);
        begin();
    }
    void stop() noexcept override { halt(); }

    Device device() const override {
        if (!streaming()) {
            return {.underruns = underruns_.load(std::memory_order_relaxed)};
        }
        std::lock_guard lock(mutex_);
        return {name_, burst_.load(std::memory_order_relaxed), buffer_.load(std::memory_order_relaxed),
                underruns_.load(std::memory_order_relaxed)};
    }

private:
    AAudioStream* open() override {
        const auto settings = builder();
        AAudioStreamBuilder_setDirection(settings.get(), AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(settings.get(), audioRate);
        AAudioStreamBuilder_setChannelCount(settings.get(), audioChannels);
        AAudioStreamBuilder_setFormat(settings.get(), AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setPerformanceMode(settings.get(), AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setUsage(settings.get(), AAUDIO_USAGE_MEDIA);
        AAudioStreamBuilder_setContentType(settings.get(), AAUDIO_CONTENT_TYPE_MOVIE);
        if (config_.periodFrames) {
            AAudioStreamBuilder_setFramesPerDataCallback(settings.get(), int32_t(config_.periodFrames));
        }
        AAudioStreamBuilder_setDataCallback(
            settings.get(),
            [](AAudioStream* stream, void* self, void* data, int32_t frames) {
                auto* player = static_cast<AAudioPlayer*>(self);
                player->tune(stream);
                player->fill_(static_cast<float*>(data), uint32_t(frames));
                return aaudio_data_callback_result_t(AAUDIO_CALLBACK_RESULT_CONTINUE);
            },
            this);
        AAudioStreamBuilder_setErrorCallback(settings.get(), &Stream::lost, static_cast<Stream*>(this));
        xruns_ = 0;
        AAudioStream* stream = openStream(settings.get(), "sound output", audioChannels);
        burst_ = uint32_t(AAudioStream_getFramesPerBurst(stream));
        buffer_ = uint32_t(AAudioStream_getBufferSizeInFrames(stream));
        std::lock_guard lock(mutex_);
        name_ = deviceName(AAudioStream_getDeviceId(stream), false);
        return stream;
    }

    // Low latency starts with the device's least buffer, a burst or two
    // (1-4 ms): each time a callback came too late for it (an underrun the
    // listener hears as a click), a burst more, as Oboe's latency tuner does.
    void tune(AAudioStream* stream) noexcept {
        const int32_t xruns = AAudioStream_getXRunCount(stream);
        if (xruns <= xruns_) {
            return;
        }
        underruns_.fetch_add(1, std::memory_order_relaxed); // Once per gap: AAudio counts each burst it missed.
        xruns_ = xruns;
        const int32_t size = AAudioStream_getBufferSizeInFrames(stream) + AAudioStream_getFramesPerBurst(stream);
        if (size <= AAudioStream_getBufferCapacityInFrames(stream)) {
            if (const int32_t set = AAudioStream_setBufferSizeInFrames(stream, size); set > 0) {
                buffer_ = uint32_t(set);
            }
        }
    }
    void report(const AudioStatus& status) override { status_(status); }

    PlaybackConfig config_;
    Fill fill_;
    Status status_;
    int32_t xruns_ = 0; // The open stream's underruns, as tune() last saw them...
    std::atomic<uint64_t> underruns_{0}; // ...and every stream's since start.
    // The open stream's, for device() (the stream itself may close meanwhile).
    std::atomic<uint32_t> burst_{0}, buffer_{0};
    mutable std::mutex mutex_;
    std::string name_; // The device's, as the device list says.
};

class AAudioMicrophone final : public AudioCapture, Stream {
public:
    explicit AAudioMicrophone(const MicrophoneConfig& config) : config_(config) {}
    ~AAudioMicrophone() override { stop(); }

    void start(const AudioCaptureConfig&, Deliver deliver, Status status) override {
        deliver_ = std::move(deliver);
        status_ = std::move(status);
        begin();
    }
    void stop() noexcept override { halt(); }
    uint32_t periodFrames() const noexcept override { return streaming() ? burst_.load() : 0; }

private:
    AAudioStream* open() override {
        if (android::hasJava() && microphonePermission() != RCS_PERMISSION_GRANTED) {
            fail(RCS_ERROR_PERMISSION_REQUIRED, "The application may not record (android.permission.RECORD_AUDIO)");
        }
        const auto settings = builder();
        AAudioStreamBuilder_setDirection(settings.get(), AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setDeviceId(settings.get(), int32_t(config_.device));
        AAudioStreamBuilder_setSampleRate(settings.get(), audioRate);
        AAudioStreamBuilder_setChannelCount(settings.get(), 1);
        AAudioStreamBuilder_setFormat(settings.get(), AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setPerformanceMode(settings.get(), AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        // Voice communication: the system's echo cancellation and noise
        // suppression. Voice recognition: the microphone with no processing
        // that changes the level.
        AAudioStreamBuilder_setInputPreset(settings.get(), config_.voiceProcessing
                                                               ? AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION
                                                               : AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
        AAudioStreamBuilder_setDataCallback(
            settings.get(),
            [](AAudioStream*, void* self, void* data, int32_t frames) {
                static_cast<AAudioMicrophone*>(self)->captured(static_cast<const float*>(data), uint32_t(frames));
                return aaudio_data_callback_result_t(AAUDIO_CALLBACK_RESULT_CONTINUE);
            },
            this);
        AAudioStreamBuilder_setErrorCallback(settings.get(), &Stream::lost, static_cast<Stream*>(this));
        AAudioStream* stream = openStream(settings.get(), "microphone", 1);
        burst_ = uint32_t(AAudioStream_getFramesPerBurst(stream));
        voiceProcessing_ = AAudioStream_getInputPreset(stream) == AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION;
        return stream;
    }
    AudioStatus activeStatus() override { return {true, RCS_OK, {}, voiceProcessing_}; }
    void report(const AudioStatus& status) override { status_(status); }

    // Mono to stereo; the first frame was taken a buffer's length ago.
    void captured(const float* samples, uint32_t frames) noexcept {
        if (stereo_.size() < size_t(frames) * 2) {
            stereo_.resize(size_t(frames) * 2); // Grows once, to the device's period.
        }
        for (uint32_t i = 0; i < frames; ++i) {
            stereo_[i * 2] = stereo_[i * 2 + 1] = samples[i];
        }
        deliver_(std::span<const float>(stereo_.data(), size_t(frames) * 2),
                 monotonicNs() - uint64_t(frames) * 1'000'000'000ull / audioRate);
    }

    const MicrophoneConfig config_;
    Deliver deliver_;
    Status status_;
    bool voiceProcessing_ = false;
    std::atomic<uint32_t> burst_{0}; // The open stream's, for periodFrames().
    std::vector<float> stereo_;
};

// AudioDeviceInfo's types this SDK lists: what plays or records sound for
// the user (not the telephony line, tuners or the system's echo reference).
const char* typeName(int type) noexcept {
    switch (type) {
    case 1:
        return "Earpiece";
    case 2:
        return "Speaker";
    case 3:
        return "Wired headset";
    case 4:
        return "Wired headphones";
    case 7:
    case 8:
    case 26:
    case 27:
    case 30:
        return "Bluetooth";
    case 9:
    case 10:
        return "HDMI";
    case 11:
    case 12:
    case 22:
        return "USB";
    case 13:
        return "Dock";
    case 15:
        return "Microphone";
    case 19:
        return "Line";
    default:
        return nullptr;
    }
}

} // namespace

std::unique_ptr<AudioCapture> createAudioCapture(rcs_audio_capture_backend) {
    fail(RCS_ERROR_UNSUPPORTED, "Android lets an application capture what others play only through "
                                "MediaProjection (AudioPlaybackCapture): push it as the host's sound "
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
    return std::make_unique<AAudioPlayer>();
}

// AudioManager.getDevices(GET_DEVICES_ALL): "Speaker (SM-S936B)".
std::vector<AudioDevice> audioDevices() {
    JNIEnv* env = android::env();
    const auto manager = android::systemService(env, "audio");
    const auto managerType = android::findClass(env, "android/media/AudioManager");
    const jmethodID getDevices =
        android::method(env, managerType.get(), "getDevices", "(I)[Landroid/media/AudioDeviceInfo;");
    android::Local<jobjectArray> devices(
        env, static_cast<jobjectArray>(env->CallObjectMethod(manager.get(), getDevices, jint(3))));
    android::check(env, RCS_ERROR_DEVICE, "AudioManager.getDevices");
    const auto infoType = android::findClass(env, "android/media/AudioDeviceInfo");
    const jmethodID getId = android::method(env, infoType.get(), "getId", "()I");
    const jmethodID isSource = android::method(env, infoType.get(), "isSource", "()Z");
    const jmethodID getType = android::method(env, infoType.get(), "getType", "()I");
    const jmethodID getProductName =
        android::method(env, infoType.get(), "getProductName", "()Ljava/lang/CharSequence;");
    const auto objectType = android::findClass(env, "java/lang/Object");
    const jmethodID toString = android::method(env, objectType.get(), "toString", "()Ljava/lang/String;");
    std::vector<AudioDevice> result;
    const jsize count = devices ? env->GetArrayLength(devices.get()) : 0;
    for (jsize i = 0; i < count; ++i) {
        android::Local<> device(env, env->GetObjectArrayElement(devices.get(), i));
        const char* type = typeName(env->CallIntMethod(device.get(), getType));
        if (!type) {
            continue;
        }
        android::Local<> product(env, env->CallObjectMethod(device.get(), getProductName));
        std::string name = type;
        if (product) {
            android::Local<jstring> text(env, static_cast<jstring>(env->CallObjectMethod(product.get(), toString)));
            if (const std::string productName = android::text(env, text.get()); !productName.empty()) {
                name += " (" + productName + ")";
            }
        }
        android::check(env, RCS_ERROR_DEVICE, "Reading AudioDeviceInfo");
        result.push_back({uint32_t(env->CallIntMethod(device.get(), getId)),
                          bool(env->CallBooleanMethod(device.get(), isSource)), false, std::move(name)});
    }
    return result;
}

rcs_permission microphonePermission() noexcept {
    try {
        JNIEnv* env = android::env();
        const auto type = android::findClass(env, "android/content/Context");
        const jmethodID check = android::method(env, type.get(), "checkSelfPermission", "(Ljava/lang/String;)I");
        const jint state = env->CallIntMethod(android::context(), check,
                                              android::javaString(env, "android.permission.RECORD_AUDIO").get());
        android::check(env, RCS_ERROR_INTERNAL, "Context.checkSelfPermission");
        return state == 0 ? RCS_PERMISSION_GRANTED : RCS_PERMISSION_REQUIRED;
    } catch (const Failure&) { // Before rcs_android_initialize.
        return RCS_PERMISSION_UNAVAILABLE;
    }
}

rcs_result requestMicrophonePermission() {
    if (!android::hasJava()) {
        fail(RCS_ERROR_UNSUPPORTED, "The host has not handed over its Java VM (rcs_android_initialize)");
    }
    // An activity asks the user (requestPermissions); the SDK has none.
    return microphonePermission() == RCS_PERMISSION_GRANTED ? RCS_OK : RCS_ERROR_PERMISSION_REQUIRED;
}

std::unique_ptr<AudioCapture> createMicrophoneCapture(const MicrophoneConfig& config) {
    return std::make_unique<AAudioMicrophone>(config);
}

} // namespace rcsdk::platform
