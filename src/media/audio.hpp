// Sound as the SDK moves it: 48 kHz stereo, float32 samples interleaved while
// raw, and packets of one codec frame each on the wire.
#pragma once

#include "util/api.hpp"

#include <cstdint>
#include <vector>

namespace rcsdk {

inline constexpr uint32_t audioRate = 48000;
inline constexpr uint32_t audioChannels = 2;

struct AudioPacket {
    uint32_t sequence = 0;    // Consecutive per run: gaps are lost packets.
    uint32_t frames = 0;      // Sample frames it decodes to.
    uint64_t timestampNs = 0; // The sharer's clock: when its first sample was captured.
    std::vector<uint8_t> data;
    // The packet before (sequence - 1) once more, where both fit in one
    // datagram (rcs_audio_tuning_config.redundancy): one lost alone comes with
    // the next. Empty: none.
    uint32_t previousFrames = 0;
    std::vector<uint8_t> previous;
};

// The AudioSpecificConfig of every AAC stream: AAC-LC, 48 kHz, stereo.
inline constexpr uint8_t aacConfig[] = {0x11, 0x90};

// What the viewer needs to decode the packets that follow.
struct AudioFormat {
    rcs_audio_codec codec = 0;
    uint32_t bitrate = 0;
    uint32_t framesPerPacket = 0;
    std::vector<uint8_t> config; // AAC: the AudioSpecificConfig. Otherwise empty.
};

// Packet sizes, in sample frames. Opus packets follow what capture delivers
// (10 ms on Windows, 5 ms from the Core Audio tap by default, 20 ms from
// ScreenCaptureKit): smaller packets would not leave sooner, they would only
// leave in bursts.
inline constexpr uint32_t opusFrames = 960; // At most 20 ms.
inline constexpr uint32_t aacFrames = 1024;
inline constexpr uint32_t pcmFrames = 240; // 5 ms.

// The most bytes one encoded packet may take: it travels in one datagram,
// never split. Opus is held to it; AAC packets are checked against it; PCM's
// fixed packets fit. A packet carrying the one before keeps both within it.
inline constexpr size_t maxAudioPacketBytes = 1024;
static_assert(size_t(pcmFrames) * audioChannels * sizeof(int16_t) <= maxAudioPacketBytes);

const char* audioCodecName(rcs_audio_codec codec) noexcept;

} // namespace rcsdk
