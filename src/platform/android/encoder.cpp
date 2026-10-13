// H.264, HEVC, VP8, VP9 and AV1 encoding with MediaCodec (H.264 also in software).
// The encoder reads its pictures from an input surface: the GL thread draws
// each frame into it (gl.cpp), the SDK's hardware buffers and CPU pictures
// alike, and the encoder turns the RGB into the stream's YCbCr as the colour
// keys say. Packets come out in order; the parameter sets come first as a
// buffer of their own and travel with every packet, as on the other systems.
// A thread of the encoder's takes each packet as soon as MediaCodec has it
// and wakes the pipeline (onOutputReady), as VideoToolbox's callback does: a
// picture never waits for the next capture to be taken from the encoder.
// Temporal layers (EncoderConfig::temporalLayers): Android's generic schema
// (KEY_TEMPORAL_LAYERING, "android.generic.N"), as many as the encoder's
// output format says it took; two at most where the bitstream does not say a
// picture's layer (H.264, VP9: the upper one is what nothing refers to) and
// none for VP8, whose bitstream says neither. Qualcomm's H.264 and HEVC
// encoders (S25+) leave the schema out of their output format: one layer.
#include "media/encoder_output.hpp"
#include "media/layers.hpp"
#include "platform/android/codecs.hpp"
#include "platform/android/gl.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace rcsdk::platform {
namespace {

using android::check;

constexpr int32_t surfaceInput = 0x7F000789; // MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface.
constexpr uint32_t keyframeFlag = 1;         // BUFFER_FLAG_KEY_FRAME.
constexpr uint32_t configFlag = 2;           // BUFFER_FLAG_CODEC_CONFIG.

class MediaCodecEncoder final : public VideoEncoder {
public:
    ~MediaCodecEncoder() override {
        window_.reset(); // Before the codec its surface belongs to.
        stopping_ = true;
        if (watcher_.joinable()) {
            watcher_.join();
        }
        if (codec_) {
            AMediaCodec_stop(codec_.get());
        }
        if (input_) {
            ANativeWindow_release(input_);
        }
    }

    void open(const EncoderConfig& config) override {
        if (config.picture.format != RCS_PICTURE_420_8) {
            fail(RCS_ERROR_UNSUPPORTED, "Android encodes 4:2:0 8-bit pictures");
        }
        const auto& entry =
            android::implementation(config.codec, RCS_CODEC_ENCODER, config.acceleration, config.backend);
        name_ = entry.name;
        config_ = config;
        bitrate_ = config.bitrate;
        output_.emplace(config.codec, config.picture, config.width, config.height, config.framesPerSecond);
        const std::string creating = "Creating " + name_ + " for " + std::to_string(config.width) + "x" +
                                     std::to_string(config.height) + " pictures";
        android::Format format(AMediaFormat_new());
        AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, android::mimeOf(config.codec));
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, int32_t(config.width));
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, int32_t(config.height));
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_COLOR_FORMAT, surfaceInput);
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_BIT_RATE, int32_t(config.bitrate));
        AMediaFormat_setInt32(format.get(), "bitrate-mode", 2); // Constant: packets fit the path's budget.
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_FRAME_RATE, int32_t(config.framesPerSecond));
        // Keyframes only when the session asks (rcs_encoder_config.keyframe_interval_ms).
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 3600);
        AMediaFormat_setInt32(format.get(), "max-bframes", 0);
        // H.264's and HEVC's whole quantizer range (Android 12 on): with the
        // encoders' own ceiling, rate control cannot shrink grainy pictures
        // to a lowered target (Android's H.264 encoder stayed near 1.4 Mbit/s
        // when asked for 0.6), and the pacing then drops two frames in three.
        // The other codecs' ranges differ; their encoders keep their own.
        if (config.codec == RCS_CODEC_H264 || config.codec == RCS_CODEC_HEVC) {
            AMediaFormat_setInt32(format.get(), "video-qp-max", 51);
            AMediaFormat_setInt32(format.get(), "video-qp-i-max", 51);
            AMediaFormat_setInt32(format.get(), "video-qp-p-max", 51);
        }
        AMediaFormat_setInt32(format.get(), "priority", 0); // Real time.
        AMediaFormat_setInt32(format.get(), "latency", 1);  // Android 11: a packet per picture, at once.
        if (config.speed == RCS_ENCODER_FASTEST) {
            // As fast as the hardware goes, not paced to the frame rate:
            // Qualcomm's encoder otherwise spends most of a frame interval on
            // each picture (12-14 ms at 1080p60 on the S25+; 5 ms this way).
            AMediaFormat_setInt32(format.get(), "operating-rate", INT16_MAX);
        }
        if (config.acceleration == RCS_ACCELERATION_HARDWARE) {
            if (config.codec == RCS_CODEC_H264) {
                AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_PROFILE, 0x08); // AVCProfileHigh.
            } else if (config.codec == RCS_CODEC_HEVC) {
                AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_PROFILE, 0x01); // HEVCProfileMain.
            }
        }
        android::setColor(format.get(), config.picture);
        const bool tells = config.codec == RCS_CODEC_HEVC || config.codec == RCS_CODEC_AV1; // Each picture's layer.
        const uint32_t layers = config.codec == RCS_CODEC_VP8 ? 1 : std::min(config.temporalLayers, tells ? 3u : 2u);
        if (layers > 1) {
            AMediaFormat_setString(format.get(), "ts-schema", ("android.generic." + std::to_string(layers)).c_str());
        }
        codec_ = android::open(name_);
        check(AMediaCodec_configure(codec_.get(), format.get(), nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE),
              creating.c_str());
        layers_ = 1;
        if (layers > 1) { // What the encoder took, as its output format says.
            android::Format taken(AMediaCodec_getOutputFormat(codec_.get()));
            const char* schema = nullptr;
            unsigned count = 0;
            if (taken && AMediaFormat_getString(taken.get(), "ts-schema", &schema) && schema &&
                std::sscanf(schema, "android.generic.%u", &count) == 1) {
                layers_ = std::clamp(count, 1u, layers);
            }
        }
        check(AMediaCodec_createInputSurface(codec_.get(), &input_), creating.c_str());
        check(AMediaCodec_start(codec_.get()), creating.c_str());
        window_ = std::make_unique<android::gl::Window>(input_);
        watcher_ = log::thread([this] { watch(); });
    }

    void onOutputReady(std::function<void()> callback) override {
        std::lock_guard lock(outputMutex_);
        outputReady_ = std::move(callback);
    }

    void submit(const VideoFrame& frame) override {
        if (const uint32_t bitrate = pendingBitrate_.exchange(0)) {
            applyBitrate(bitrate);
        }
        if (keyframeRequested_.exchange(false)) {
            android::Format request(AMediaFormat_new());
            AMediaFormat_setInt32(request.get(), "request-sync", 0);
            check(AMediaCodec_setParameters(codec_.get(), request.get()), "Asking for a keyframe");
        }
        if (frame.width != config_.width || frame.height != config_.height) {
            fail(RCS_ERROR_CODEC, "Frame size does not match the encoder");
        }
        // The input's presentation time names the frame in the output: strictly increasing.
        const int64_t time = std::max(lastTime_ + 1, int64_t(frame.timestampNs / 1000));
        lastTime_ = time;
        {
            std::lock_guard lock(mutex_);
            EncodedPacket header;
            header.width = frame.width;
            header.height = frame.height;
            header.sequence = frame.sequence;
            header.timestampNs = frame.timestampNs;
            submitted_.push_back({time, monotonicNs(), std::move(header)});
        }
        try {
            if (frame.surface) {
                window_->draw(static_cast<AHardwareBuffer*>(frame.surface.get()), uint64_t(time) * 1000);
            } else if (frame.pixels) {
                window_->draw(frame.pixels->data(), frame.stride, frame.width, frame.height, uint64_t(time) * 1000);
            } else {
                fail(RCS_ERROR_CODEC, "The frame has no picture");
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            submitted_.pop_back();
            throw;
        }
    }

    bool receive(EncodedPacket& packet, std::chrono::milliseconds timeout) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            Output output;
            {
                std::unique_lock lock(outputMutex_);
                if (!outputArrived_.wait_until(lock, deadline, [&] { return !outputs_.empty(); })) {
                    return false;
                }
                output = outputs_.front();
                outputs_.pop_front();
            }
            const ssize_t index = output.index;
            const AMediaCodecBufferInfo& info = output.info;
            if (index < 0) {
                fail(RCS_ERROR_CODEC, name_ + " output failed (" + std::to_string(index) + ")");
            }
            struct Release {
                AMediaCodec* codec;
                size_t index;
                ~Release() { AMediaCodec_releaseOutputBuffer(codec, index, false); }
            } release{codec_.get(), size_t(index)};
            size_t capacity = 0;
            const uint8_t* buffer = AMediaCodec_getOutputBuffer(codec_.get(), size_t(index), &capacity);
            if (!buffer || size_t(info.offset) + size_t(info.size) > capacity) {
                fail(RCS_ERROR_CODEC, name_ + " produced no bitstream");
            }
            const std::span<const uint8_t> bytes(buffer + info.offset, size_t(info.size));
            if (info.flags & configFlag) {
                output_->configuration(bytes);
                continue;
            }
            if (bytes.empty() || !take(info.presentationTimeUs, packet)) {
                continue; // A picture the encoder skipped, or nothing of one.
            }
            packet.codec = config_.codec;
            packet.keyframe = info.flags & keyframeFlag;
            packet.data.clear();
            output_->split(bytes, packet);
            if (layers_ > 1) {
                inferTemporalLayer(packet);
            }
            return true;
        }
    }

    size_t inFlight() const noexcept override {
        std::lock_guard lock(mutex_);
        return submitted_.size();
    }
    void requestKeyframe() noexcept override { keyframeRequested_ = true; }
    void setBitrate(uint32_t bitsPerSecond) noexcept override { pendingBitrate_ = bitsPerSecond; }
    uint32_t bitrate() const noexcept override { return bitrate_; }
    bool lowLatency() const noexcept override { return true; }
    uint32_t temporalLayers() const noexcept override { return layers_; }
    std::string name() const override { return name_; }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_MEDIACODEC; }

private:
    struct Submitted {
        int64_t time = 0; // Presentation time, microseconds.
        uint64_t startedNs = 0;
        EncodedPacket header;
    };
    struct Output {
        ssize_t index = 0; // Below 0: the codec failed so.
        AMediaCodecBufferInfo info{};
    };

    // The watcher thread: each output buffer as MediaCodec has it, for receive().
    void watch() {
        while (!stopping_) {
            AMediaCodecBufferInfo info{};
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_.get(), &info, 10'000);
            if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
                index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            std::function<void()> ready;
            {
                std::lock_guard lock(outputMutex_);
                outputs_.push_back({index, info});
                ready = outputReady_;
            }
            outputArrived_.notify_all();
            if (ready) {
                ready();
            }
            if (index < 0) {
                return; // A codec that failed gives nothing more.
            }
        }
    }

    // The submitted frame the packet of `time` encodes; frames before it the encoder skipped.
    bool take(int64_t time, EncodedPacket& packet) {
        std::lock_guard lock(mutex_);
        while (!submitted_.empty() && submitted_.front().time < time) {
            submitted_.pop_front();
        }
        if (submitted_.empty() || submitted_.front().time != time) {
            return false;
        }
        packet = std::move(submitted_.front().header);
        packet.encodeNs = monotonicNs() - submitted_.front().startedNs;
        submitted_.pop_front();
        return true;
    }

    // A new target from the next frame on; one MediaCodec refuses leaves the previous.
    void applyBitrate(uint32_t bitrate) {
        android::Format parameters(AMediaFormat_new());
        AMediaFormat_setInt32(parameters.get(), "video-bitrate", int32_t(bitrate));
        if (const media_status_t status = AMediaCodec_setParameters(codec_.get(), parameters.get());
            status == AMEDIA_OK) {
            bitrate_ = bitrate;
        } else if (!bitrateRefused_) {
            bitrateRefused_ = true;
            log::warning("encoder", name_ + " refused a bitrate of " + std::to_string(bitrate) + " bit/s (" +
                                        android::statusText(status) + "); it stays at " +
                                        std::to_string(bitrate_.load()));
        }
    }

    EncoderConfig config_;
    std::string name_;
    android::Codec codec_;
    ANativeWindow* input_ = nullptr;
    std::unique_ptr<android::gl::Window> window_;
    std::atomic<uint32_t> pendingBitrate_{0};
    std::atomic<uint32_t> bitrate_{0};
    bool bitrateRefused_ = false;
    std::atomic<bool> keyframeRequested_{false};
    int64_t lastTime_ = 0;
    uint32_t layers_ = 1; // The temporal layers it makes.
    std::optional<EncoderOutput> output_;

    mutable std::mutex mutex_;
    std::deque<Submitted> submitted_;

    std::mutex outputMutex_;
    std::condition_variable outputArrived_;
    std::deque<Output> outputs_;
    std::function<void()> outputReady_;
    std::atomic<bool> stopping_{false};
    std::thread watcher_;
};

} // namespace

std::unique_ptr<VideoEncoder> createVideoEncoder(const EncoderConfig&) {
    return std::make_unique<MediaCodecEncoder>();
}

} // namespace rcsdk::platform
