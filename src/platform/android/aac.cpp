// AAC-LC through MediaCodec's audio codecs (Android's own: c2.android.aac.*),
// 16-bit PCM in and out, converted from and to the SDK's float32.
#include "platform/android/codecs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rcsdk::platform {
namespace {

using namespace std::chrono_literals;
using android::check;

constexpr const char* aacType = "audio/mp4a-latm";
constexpr uint32_t configFlag = 2; // BUFFER_FLAG_CODEC_CONFIG.

android::Codec openAac(bool encoder, int32_t bitrate) {
    android::serveBinderCalls();
    android::Codec codec(encoder ? AMediaCodec_createEncoderByType(aacType) : AMediaCodec_createDecoderByType(aacType));
    if (!codec) {
        fail(RCS_ERROR_CODEC, std::string("This system has no AAC ") + (encoder ? "encoder" : "decoder"));
    }
    android::Format format(AMediaFormat_new());
    AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, aacType);
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_SAMPLE_RATE, int32_t(audioRate));
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_CHANNEL_COUNT, int32_t(audioChannels));
    if (encoder) {
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_AAC_PROFILE, 2); // AAC-LC.
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
        AMediaFormat_setInt32(format.get(), "bitrate-mode", 2); // Constant: packets stay within a datagram.
    } else {
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_IS_ADTS, 0);
        AMediaFormat_setBuffer(format.get(), "csd-0", aacConfig, sizeof(aacConfig));
    }
    check(AMediaCodec_configure(codec.get(), format.get(), nullptr, nullptr,
                                encoder ? AMEDIACODEC_CONFIGURE_FLAG_ENCODE : 0),
          encoder ? "Configuring the AAC encoder" : "Configuring the AAC decoder");
    check(AMediaCodec_start(codec.get()), "Starting the AAC codec");
    return codec;
}

// Queues `bytes` (or waits up to `wait` for room).
void queue(AMediaCodec* codec, std::span<const uint8_t> bytes, int64_t time, const char* what) {
    const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 20'000);
    if (index < 0) {
        fail(RCS_ERROR_CODEC, std::string(what) + " takes no input");
    }
    size_t capacity = 0;
    uint8_t* buffer = AMediaCodec_getInputBuffer(codec, size_t(index), &capacity);
    if (!buffer || capacity < bytes.size()) {
        fail(RCS_ERROR_CODEC, std::string(what) + " has no room for its input");
    }
    std::memcpy(buffer, bytes.data(), bytes.size());
    check(AMediaCodec_queueInputBuffer(codec, size_t(index), 0, bytes.size(), uint64_t(time), 0), what);
}

// Calls `take` with each output ready, waiting up to `wait` for the first.
template <class Take>
void outputs(AMediaCodec* codec, std::chrono::microseconds wait, Take&& take) {
    for (;;) {
        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, wait.count());
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        }
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            return;
        }
        if (index < 0) {
            fail(RCS_ERROR_CODEC, "The AAC codec failed: " + android::statusText(media_status_t(index)));
        }
        const struct Release { // Also when `take` throws.
            AMediaCodec* codec;
            size_t index;
            ~Release() { AMediaCodec_releaseOutputBuffer(codec, index, false); }
        } release{codec, size_t(index)};
        size_t capacity = 0;
        const uint8_t* buffer = AMediaCodec_getOutputBuffer(codec, size_t(index), &capacity);
        if (buffer && !(info.flags & configFlag) && info.size > 0) {
            take(std::span<const uint8_t>(buffer + info.offset, size_t(info.size)));
        }
        wait = 0us;
    }
}

class AacEncoder final : public audio::Encoder {
public:
    explicit AacEncoder(uint32_t bitrate) : codec_(openAac(true, int32_t(bitrate))) {}
    ~AacEncoder() override { AMediaCodec_stop(codec_.get()); }

    uint32_t frames() const noexcept override { return aacFrames; }

    std::vector<Bytes> encode(std::span<const float> samples) override {
        pcm_.resize(samples.size() * 2);
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto value = int16_t(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
            std::memcpy(pcm_.data() + i * 2, &value, 2);
        }
        queue(codec_.get(), pcm_, time_, "The AAC encoder");
        time_ += int64_t(samples.size() / audioChannels) * 1'000'000 / audioRate;
        std::vector<Bytes> packets;
        // Software AAC takes well under a millisecond a packet; the first ones come later (look-ahead).
        outputs(codec_.get(), 5ms, [&](std::span<const uint8_t> packet) {
            if (packet.size() > maxAudioPacketBytes) {
                fail(RCS_ERROR_CODEC, "The AAC encoder made a packet of " + std::to_string(packet.size()) +
                                          " bytes; one datagram takes " + std::to_string(maxAudioPacketBytes));
            }
            packets.emplace_back(packet.begin(), packet.end());
        });
        return packets;
    }

    Bytes config() const override { return {std::begin(aacConfig), std::end(aacConfig)}; }
    std::string name() const override { return "MediaCodec AAC encoder"; }

private:
    android::Codec codec_;
    Bytes pcm_;
    int64_t time_ = 0;
};

class AacDecoder final : public audio::Decoder {
public:
    // The decoder takes the one configuration every AAC stream has.
    explicit AacDecoder(std::span<const uint8_t> config) {
        if (!std::equal(config.begin(), config.end(), std::begin(aacConfig), std::end(aacConfig))) {
            fail(RCS_ERROR_UNSUPPORTED, "Only AAC-LC at 48 kHz in stereo is decoded");
        }
        codec_ = openAac(false, 0);
    }
    ~AacDecoder() override { AMediaCodec_stop(codec_.get()); }

    void decode(std::span<const uint8_t> packet, std::vector<float>& out) override {
        try {
            queue(codec_.get(), packet, time_, "The AAC decoder");
        } catch (const Failure& failure) {
            fail(RCS_ERROR_NETWORK, failure.what());
        }
        time_ += int64_t(aacFrames) * 1'000'000 / audioRate;
        outputs(codec_.get(), 5ms, [&](std::span<const uint8_t> pcm) {
            const size_t samples = pcm.size() / 2;
            const size_t start = out.size();
            out.resize(start + samples);
            for (size_t i = 0; i < samples; ++i) {
                int16_t value;
                std::memcpy(&value, pcm.data() + i * 2, 2);
                out[start + i] = float(value) / 32768.0f;
            }
        });
    }

    std::string name() const override { return "MediaCodec AAC decoder"; }

private:
    android::Codec codec_;
    int64_t time_ = 0;
};

} // namespace

std::unique_ptr<audio::Encoder> createAacEncoder(uint32_t bitrate) {
    return std::make_unique<AacEncoder>(bitrate);
}

std::unique_ptr<audio::Decoder> createAacDecoder(std::span<const uint8_t> config) {
    return std::make_unique<AacDecoder>(config);
}

std::vector<uint32_t> aacBitrates() {
    // Higher rates make packets beyond one datagram: Android's encoder has a
    // bit reservoir and no constant bitrate to turn it off.
    return {96000, 128000, 160000, 192000};
}

} // namespace rcsdk::platform
