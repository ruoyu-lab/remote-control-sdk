// AAC-LC through AudioConverter (macOS and iOS).
#include "platform/apple/audio_format.hpp"
#include "platform/apple/cf.hpp"
#include "platform/platform.hpp"

#import <AudioToolbox/AudioToolbox.h>

#include <cstring>

namespace rcsdk::platform {
namespace {

using apple::aacFormat;
using apple::floatFormat;
using apple::osStatusText;

constexpr OSStatus noMoreInput = 'rcs!';

class AudioConverterAac {
protected:
    ~AudioConverterAac() {
        if (converter_) {
            AudioConverterDispose(converter_);
        }
    }
    AudioConverterRef converter_ = nullptr;
};

// A constant bitrate: what the video's budget sets aside, and packets that
// stay within a datagram (with the default average rate control, loud sound
// at 320 kbit/s makes packets of 1.2 KB). AudioToolbox applies any rate in
// its range exactly; one it does not apply is refused, never replaced.
class AacEncoder final : public audio::Encoder, AudioConverterAac {
public:
    explicit AacEncoder(uint32_t bitrate) {
        const auto in = floatFormat(), out = aacFormat();
        apple::check(AudioConverterNew(&in, &out, &converter_), "AAC encoder (AudioConverter)", RCS_ERROR_CODEC);
        UInt32 mode = kAudioCodecBitRateControlMode_Constant;
        apple::check(AudioConverterSetProperty(converter_, kAudioCodecPropertyBitRateControlMode, sizeof(mode), &mode),
                   "AAC constant bitrate", RCS_ERROR_CODEC);
        const auto [lowest, highest] = bitrateRange();
        if (bitrate < lowest || bitrate > highest) {
            fail(RCS_ERROR_UNSUPPORTED, "This system's AAC encoder takes " + std::to_string(lowest) + ".." +
                                            std::to_string(highest) + " bit/s, not " + std::to_string(bitrate));
        }
        UInt32 rate = bitrate, applied = 0, size = sizeof(applied);
        apple::check(AudioConverterSetProperty(converter_, kAudioConverterEncodeBitRate, sizeof(rate), &rate),
                   "AAC bitrate", RCS_ERROR_CODEC);
        apple::check(AudioConverterGetProperty(converter_, kAudioConverterEncodeBitRate, &size, &applied), "AAC bitrate",
                   RCS_ERROR_CODEC);
        if (applied != bitrate) {
            fail(RCS_ERROR_UNSUPPORTED, "This system's AAC encoder runs at " + std::to_string(applied) +
                                            " bit/s when asked for " + std::to_string(bitrate));
        }
        size = sizeof(maxPacket_);
        apple::check(
            AudioConverterGetProperty(converter_, kAudioConverterPropertyMaximumOutputPacketSize, &size, &maxPacket_),
            "AAC packet size", RCS_ERROR_CODEC);
    }

    uint32_t frames() const noexcept override { return aacFrames; }

    std::vector<Bytes> encode(std::span<const float> samples) override {
        Input input{samples, false};
        Bytes packet(maxPacket_);
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0] = {audioChannels, maxPacket_, packet.data()};
        UInt32 packets = 1;
        AudioStreamPacketDescription description{};
        const OSStatus status = AudioConverterFillComplexBuffer(converter_, &AacEncoder::supply, &input, &packets,
                                                                &list, &description);
        if (status != noErr && status != noMoreInput) {
            fail(RCS_ERROR_CODEC, "AAC encoder: OSStatus " + osStatusText(status));
        }
        if (!packets) {
            return {}; // Look-ahead: the first packets come later.
        }
        packet.resize(list.mBuffers[0].mDataByteSize);
        if (packet.size() > maxAudioPacketBytes) {
            fail(RCS_ERROR_CODEC, "The AAC encoder made a packet of " + std::to_string(packet.size()) +
                                      " bytes; one datagram takes " + std::to_string(maxAudioPacketBytes));
        }
        return {std::move(packet)};
    }

    Bytes config() const override { return {std::begin(aacConfig), std::end(aacConfig)}; }
    std::string name() const override { return "AudioToolbox AAC encoder"; }

private:
    // The lowest and the highest rate the encoder lists for this format.
    std::pair<uint32_t, uint32_t> bitrateRange() const {
        UInt32 size = 0;
        apple::check(AudioConverterGetPropertyInfo(converter_, kAudioConverterApplicableEncodeBitRates, &size, nullptr),
                   "AAC bitrates", RCS_ERROR_CODEC);
        std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
        apple::check(AudioConverterGetProperty(converter_, kAudioConverterApplicableEncodeBitRates, &size, ranges.data()),
                   "AAC bitrates", RCS_ERROR_CODEC);
        uint32_t lowest = UINT32_MAX, highest = 0;
        for (const auto& range : ranges) {
            if (range.mMaximum > 0) { // The list ends in empty entries.
                lowest = std::min(lowest, uint32_t(range.mMinimum));
                highest = std::max(highest, uint32_t(range.mMaximum));
            }
        }
        if (!highest) {
            fail(RCS_ERROR_CODEC, "This system's AAC encoder lists no bitrates");
        }
        return {lowest, highest};
    }

    struct Input {
        std::span<const float> samples;
        bool consumed;
    };
    static OSStatus supply(AudioConverterRef, UInt32* packets, AudioBufferList* data, AudioStreamPacketDescription**,
                           void* context) {
        auto* input = static_cast<Input*>(context);
        if (input->consumed) {
            *packets = 0;
            return noMoreInput;
        }
        input->consumed = true;
        *packets = UInt32(input->samples.size() / audioChannels);
        data->mBuffers[0].mData = const_cast<float*>(input->samples.data());
        data->mBuffers[0].mDataByteSize = UInt32(input->samples.size_bytes());
        data->mBuffers[0].mNumberChannels = audioChannels;
        return noErr;
    }

    UInt32 maxPacket_ = 0;
};

class AacDecoder final : public audio::Decoder, AudioConverterAac {
public:
    // The converter decodes the one configuration every AAC stream has.
    explicit AacDecoder(std::span<const uint8_t> config) {
        if (!std::equal(config.begin(), config.end(), std::begin(aacConfig), std::end(aacConfig))) {
            fail(RCS_ERROR_UNSUPPORTED, "Only AAC-LC at 48 kHz in stereo is decoded");
        }
        const auto in = aacFormat(), out = floatFormat();
        apple::check(AudioConverterNew(&in, &out, &converter_), "AAC decoder (AudioConverter)", RCS_ERROR_CODEC);
    }

    void decode(std::span<const uint8_t> packet, std::vector<float>& out) override {
        Input input{packet, {0, 0, UInt32(packet.size())}, false};
        const size_t start = out.size();
        out.resize(start + size_t(aacFrames) * audioChannels);
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0] = {audioChannels, UInt32(aacFrames * audioChannels * sizeof(float)), out.data() + start};
        UInt32 frames = aacFrames;
        const OSStatus status = AudioConverterFillComplexBuffer(converter_, &AacDecoder::supply, &input, &frames, &list,
                                                                nullptr);
        if (status != noErr && status != noMoreInput) {
            out.resize(start);
            fail(RCS_ERROR_NETWORK, "AAC decoder: OSStatus " + osStatusText(status));
        }
        out.resize(start + size_t(frames) * audioChannels);
    }

    std::string name() const override { return "AudioToolbox AAC decoder"; }

private:
    struct Input {
        std::span<const uint8_t> packet;
        AudioStreamPacketDescription description;
        bool consumed;
    };
    static OSStatus supply(AudioConverterRef, UInt32* packets, AudioBufferList* data,
                           AudioStreamPacketDescription** description, void* context) {
        auto* input = static_cast<Input*>(context);
        if (input->consumed) {
            *packets = 0;
            return noMoreInput;
        }
        input->consumed = true;
        *packets = 1;
        data->mBuffers[0].mData = const_cast<uint8_t*>(input->packet.data());
        data->mBuffers[0].mDataByteSize = UInt32(input->packet.size());
        data->mBuffers[0].mNumberChannels = audioChannels;
        if (description) {
            *description = &input->description;
        }
        return noErr;
    }
};

} // namespace

std::unique_ptr<audio::Encoder> createAacEncoder(uint32_t bitrate) {
    return std::make_unique<AacEncoder>(bitrate);
}

std::unique_ptr<audio::Decoder> createAacDecoder(std::span<const uint8_t> config) {
    return std::make_unique<AacDecoder>(config);
}

std::vector<uint32_t> aacBitrates() {
    return {}; // Any rate in its range, applied exactly (AacEncoder refuses one it does not apply).
}

} // namespace rcsdk::platform
