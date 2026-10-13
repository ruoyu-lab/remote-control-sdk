#include "audio/codec.hpp"

#include "platform/platform.hpp"
#include "util/util.hpp"

#include "opus/rcsdk_opus_names.h" // libopus's symbols as the SDK's copy names them.
#include <opus.h>

#include <algorithm>
#include <cmath>

namespace rcsdk {

const char* audioCodecName(rcs_audio_codec codec) noexcept {
    switch (codec) {
    case RCS_AUDIO_OPUS:
        return "Opus";
    case RCS_AUDIO_AAC:
        return "AAC";
    case RCS_AUDIO_PCM:
        return "PCM";
    default:
        return "none";
    }
}

namespace audio {
namespace {

// ---- PCM: 16-bit little-endian -------------------------------------------------

class PcmEncoder final : public Encoder {
public:
    uint32_t frames() const noexcept override { return pcmFrames; }
    std::vector<Bytes> encode(std::span<const float> samples) override {
        Bytes packet(samples.size() * 2);
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto value = int16_t(std::lrint(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
            packet[2 * i] = uint8_t(value);
            packet[2 * i + 1] = uint8_t(uint16_t(value) >> 8);
        }
        return {std::move(packet)};
    }
    std::string name() const override { return "PCM"; }
};

class PcmDecoder final : public Decoder {
public:
    void decode(std::span<const uint8_t> packet, std::vector<float>& out) override {
        if (packet.empty() || packet.size() % (2 * audioChannels)) {
            fail(RCS_ERROR_NETWORK, "Invalid PCM packet");
        }
        for (size_t i = 0; i + 1 < packet.size(); i += 2) {
            out.push_back(float(int16_t(packet[i] | packet[i + 1] << 8)) / 32768.0f);
        }
    }
    std::string name() const override { return "PCM"; }
};

// ---- Opus -------------------------------------------------------------------------

// The longest Opus packet duration (2.5, 5, 10 or 20 ms) within `frames`.
uint32_t opusDuration(uint32_t frames) noexcept {
    for (const uint32_t n : {960u, 480u, 240u, 120u}) {
        if (frames >= n) {
            return n;
        }
    }
    return 0;
}

std::string opusName() {
    std::string version = opus_get_version_string(); // "libopus 1.6.1"
    return version;
}

class LibopusEncoder final : public Encoder {
public:
    explicit LibopusEncoder(uint32_t bitrate) {
        int error = OPUS_OK;
        // Restricted low delay: CELT only, 2.5 ms look-ahead. Sound from a
        // desktop is music, games and speech alike; CELT handles all of it.
        encoder_ = opus_encoder_create(int(audioRate), int(audioChannels), OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                                       &error);
        if (error != OPUS_OK || !encoder_) {
            fail(RCS_ERROR_CODEC, std::string("Opus encoder: ") + opus_strerror(error));
        }
        error = opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(int(bitrate)));
        if (error == OPUS_OK) {
            error = opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(8));
        }
        if (error != OPUS_OK) {
            opus_encoder_destroy(encoder_);
            fail(RCS_ERROR_CODEC, std::string("Opus encoder settings: ") + opus_strerror(error));
        }
    }
    ~LibopusEncoder() override { opus_encoder_destroy(encoder_); }

    uint32_t frames() const noexcept override { return opusFrames; }
    uint32_t next(uint32_t pending) const noexcept override { return opusDuration(pending); }
    std::vector<Bytes> encode(std::span<const float> samples) override {
        // At most one datagram's worth: at high bitrates and 20 ms packets
        // (above about 400 kbit/s) libopus spends no more than that.
        Bytes packet(maxAudioPacketBytes);
        const int size = opus_encode_float(encoder_, samples.data(), int(samples.size() / audioChannels),
                                           packet.data(), opus_int32(packet.size()));
        if (size < 0) {
            fail(RCS_ERROR_CODEC, std::string("Opus encoder: ") + opus_strerror(size));
        }
        packet.resize(size_t(size));
        return {std::move(packet)};
    }
    std::string name() const override { return opusName(); }

private:
    ::OpusEncoder* encoder_ = nullptr;
};

class LibopusDecoder final : public Decoder {
public:
    LibopusDecoder() {
        int error = OPUS_OK;
        decoder_ = opus_decoder_create(int(audioRate), int(audioChannels), &error);
        if (error != OPUS_OK || !decoder_) {
            fail(RCS_ERROR_CODEC, std::string("Opus decoder: ") + opus_strerror(error));
        }
    }
    ~LibopusDecoder() override { opus_decoder_destroy(decoder_); }

    void decode(std::span<const uint8_t> packet, std::vector<float>& out) override {
        run(packet.data(), int(packet.size()), maxFrames, out);
    }
    void conceal(uint32_t frames, std::vector<float>& out) override {
        // Concealment works in Opus packet durations; a remainder below
        // 2.5 ms is silence.
        uint32_t done = 0;
        while (const uint32_t n = opusDuration(frames - done)) {
            run(nullptr, 0, int(n), out);
            done += n;
        }
        out.insert(out.end(), size_t(frames - done) * audioChannels, 0.0f);
    }
    std::string name() const override { return opusName(); }

private:
    static constexpr int maxFrames = 5760; // 120 ms, Opus's longest packet.

    void run(const uint8_t* data, int size, int frames, std::vector<float>& out) {
        const size_t start = out.size();
        out.resize(start + size_t(frames) * audioChannels);
        const int decoded = opus_decode_float(decoder_, data, size, out.data() + start, frames, 0);
        if (decoded < 0) {
            out.resize(start);
            fail(RCS_ERROR_NETWORK, std::string("Opus decoder: ") + opus_strerror(decoded));
        }
        out.resize(start + size_t(decoded) * audioChannels);
    }

    ::OpusDecoder* decoder_ = nullptr;
};

} // namespace

std::unique_ptr<Encoder> createEncoder(rcs_audio_codec codec, uint32_t bitrate) {
    switch (codec) {
    case RCS_AUDIO_OPUS:
        return std::make_unique<LibopusEncoder>(bitrate);
    case RCS_AUDIO_AAC:
        return platform::createAacEncoder(bitrate);
    case RCS_AUDIO_PCM:
        return std::make_unique<PcmEncoder>();
    default:
        fail(RCS_ERROR_UNSUPPORTED, "Unknown audio codec " + std::to_string(codec));
    }
}

std::unique_ptr<Decoder> createDecoder(const AudioFormat& format) {
    switch (format.codec) {
    case RCS_AUDIO_OPUS:
        return std::make_unique<LibopusDecoder>();
    case RCS_AUDIO_AAC:
        return platform::createAacDecoder(format.config);
    case RCS_AUDIO_PCM:
        return std::make_unique<PcmDecoder>();
    default:
        fail(RCS_ERROR_UNSUPPORTED, "Unknown audio codec " + std::to_string(format.codec));
    }
}

uint32_t wireBitrate(rcs_audio_codec codec, uint32_t bitrate, bool redundant) noexcept {
    constexpr uint32_t pcmBitrate = audioRate * audioChannels * 16;
    return codec == RCS_AUDIO_PCM ? pcmBitrate : codec ? bitrate * (redundant ? 2 : 1) : 0;
}

} // namespace audio
} // namespace rcsdk
