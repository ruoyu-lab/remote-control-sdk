// Audio encoders and decoders by codec: Opus (libopus) and PCM are the same
// on every platform; AAC comes from the platform (platform/platform.hpp).
#pragma once

#include "media/audio.hpp"
#include "media/audio_codec.hpp"

#include <memory>

namespace rcsdk::audio {

// Throws Failure(UNSUPPORTED or CODEC) when the codec cannot open.
std::unique_ptr<Encoder> createEncoder(rcs_audio_codec codec, uint32_t bitrate);
std::unique_ptr<Decoder> createDecoder(const AudioFormat& format);

// What the bitrate of `codec` costs on the wire, for the video's budget;
// `redundant`: each packet carries the one before (not PCM).
uint32_t wireBitrate(rcs_audio_codec codec, uint32_t bitrate, bool redundant = false) noexcept;

} // namespace rcsdk::audio
