// MediaCodec (NDK AMediaCodec) as the encoders and decoders use it.
#pragma once

#include "platform/platform.hpp"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <memory>
#include <span>
#include <string>

namespace rcsdk::platform::android {

struct CodecDeleter {
    void operator()(AMediaCodec* codec) const noexcept { AMediaCodec_delete(codec); }
};
using Codec = std::unique_ptr<AMediaCodec, CodecDeleter>;
struct FormatDeleter {
    void operator()(AMediaFormat* format) const noexcept { AMediaFormat_delete(format); }
};
using Format = std::unique_ptr<AMediaFormat, FormatDeleter>;

// Has this process serve binder calls, as MediaCodec needs before its first
// codec: its codecs call back into the process for every buffer they pass
// (Android 16's Codec2: the buffer pool, a decoder's output buffers), and in
// a process that serves none they wait forever. Apps serve them; a native
// program starts serving here.
void serveBinderCalls();

// "video/avc", "video/hevc", "video/av01"; null for a codec MediaCodec has no type for.
const char* mimeOf(rcs_codec codec) noexcept;
// "AMEDIA_ERROR_UNKNOWN (-10000)".
std::string statusText(media_status_t status);
// Throws Failure(result) naming `what` unless `status` is AMEDIA_OK.
void check(media_status_t status, const char* what, rcs_result result = RCS_ERROR_CODEC);
// The picture's colour as MediaCodec's keys (color-standard, color-range,
// color-transfer): what an encoder writes into the stream and converts its
// RGB input with, and what a decoder takes for a stream that does not say.
void setColor(AMediaFormat* format, const Picture& picture);

// The codecs MediaCodec has for this SDK: per codec and direction, the
// system's choice for the type when it runs on the hardware, and Android's
// own software codec (c2.android., OMX.google.); H.264 only among software
// encoders, as on the other systems. Each is named as MediaCodec names it.
std::vector<CodecImplementation> implementations();
// The one `implementations()` lists first for that codec, direction and
// acceleration; Failure(UNSUPPORTED) when there is none.
const CodecImplementation& implementation(rcs_codec codec, rcs_codec_direction direction, rcs_acceleration acceleration,
                                          rcs_backend backend);
// Opens the codec MediaCodec names `name`.
Codec open(const std::string& name);
// Whether the decoder `name` decodes a keyframe of `format` (decode_probes.hpp)
// into a picture that keeps its bit depth and chroma (decoder.cpp): MediaCodec
// tells which profiles a decoder takes to Java only, and Android's software
// decoders draw 10-bit pictures into 8-bit buffers where the device has no
// 10-bit ones.
bool decodes(const std::string& name, rcs_codec codec, rcs_picture_format format, std::span<const uint8_t> config,
             std::span<const uint8_t> data);

} // namespace rcsdk::platform::android
