// What every audio codec implements: Opus and PCM in audio/codec.cpp, AAC in
// the platform (platform/platform.hpp). Packets are raw codec frames, one per
// datagram (media/audio.hpp).
#pragma once

#include "media/audio.hpp"
#include "util/util.hpp"

#include <span>
#include <string>
#include <vector>

namespace rcsdk::audio {

class Encoder {
public:
    virtual ~Encoder() = default;
    // The longest packet, in sample frames.
    virtual uint32_t frames() const noexcept = 0;
    // How many of `pending` frames to encode now; 0 waits for more. Fixed-size
    // codecs take frames() at a time; Opus takes what capture delivered.
    virtual uint32_t next(uint32_t pending) const noexcept { return pending >= frames() ? frames() : 0; }
    // next() interleaved stereo frames in; the packets that are finished out
    // (codecs with look-ahead may hold the first ones back).
    virtual std::vector<Bytes> encode(std::span<const float> samples) = 0;
    virtual Bytes config() const { return {}; }
    virtual std::string name() const = 0;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    // Appends the packet's sound to `out` (interleaved stereo). Throws
    // Failure(NETWORK) for a packet it cannot decode.
    virtual void decode(std::span<const uint8_t> packet, std::vector<float>& out) = 0;
    // Appends `frames` of made-up sound for packets that never arrived: Opus
    // continues the sound; the others are silent.
    virtual void conceal(uint32_t frames, std::vector<float>& out) {
        out.insert(out.end(), size_t(frames) * audioChannels, 0.0f);
    }
    virtual std::string name() const = 0;
};

} // namespace rcsdk::audio
