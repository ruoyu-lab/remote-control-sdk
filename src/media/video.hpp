// Codec-neutral video types passed between capture, codecs, network and delivery.
#pragma once

#include "util/api.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <optional>
#include <tuple>
#include <vector>

namespace rcsdk {

// The standard formats, in rcs_codec's order.
inline constexpr rcs_codec standardCodecs[] = {RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_VP8, RCS_CODEC_VP9,
                                               RCS_CODEC_AV1};
inline bool standardCodec(uint32_t codec) noexcept {
    return codec >= RCS_CODEC_H264 && codec <= RCS_CODEC_AV1;
}
// The standard formats, and every format this version defines (the hosts'
// own too), as (1u << rcs_codec) bits.
static_assert(RCS_CODEC_HOST_LAST == 31, "the hosts' formats end at the top bit");
inline constexpr uint32_t standardCodecBits = ((1u << (RCS_CODEC_AV1 + 1)) - 1) & ~((1u << RCS_CODEC_H264) - 1);
inline constexpr uint32_t knownCodecBits = standardCodecBits | (~0u << RCS_CODEC_HOST_FIRST);
// One of the host's own formats (RCS_CODEC_HOST_FIRST..LAST): opaque to the SDK.
inline bool hostCodec(uint32_t codec) noexcept {
    return codec >= RCS_CODEC_HOST_FIRST && codec <= RCS_CODEC_HOST_LAST;
}
// A format this version defines: a standard one, or one of the host's.
inline bool knownCodec(uint32_t codec) noexcept {
    return standardCodec(codec) || hostCodec(codec);
}
// How a standard format's packets are written (rcs_encoded_packet): NAL units
// with parameter sets, one VP frame with a 'vpcC' record, or AV1's OBUs.
inline bool nalCodec(rcs_codec codec) noexcept {
    return codec == RCS_CODEC_H264 || codec == RCS_CODEC_HEVC;
}
inline bool vpCodec(rcs_codec codec) noexcept {
    return codec == RCS_CODEC_VP8 || codec == RCS_CODEC_VP9;
}

inline const char* codecName(rcs_codec codec) noexcept {
    static const char* const host[] = {"host codec 16", "host codec 17", "host codec 18", "host codec 19",
                                       "host codec 20", "host codec 21", "host codec 22", "host codec 23",
                                       "host codec 24", "host codec 25", "host codec 26", "host codec 27",
                                       "host codec 28", "host codec 29", "host codec 30", "host codec 31"};
    switch (codec) {
    case RCS_CODEC_H264:
        return "H.264";
    case RCS_CODEC_HEVC:
        return "HEVC";
    case RCS_CODEC_VP8:
        return "VP8";
    case RCS_CODEC_VP9:
        return "VP9";
    case RCS_CODEC_AV1:
        return "AV1";
    default:
        return hostCodec(codec) ? host[codec - RCS_CODEC_HOST_FIRST] : "Unknown codec";
    }
}

// ---- Backends (rcs_backend) -----------------------------------------------------

// A codec library built into the SDK (src/codecs/), as opposed to the system's.
inline bool libraryBackend(uint32_t backend) noexcept {
    return backend >= RCS_BACKEND_OPENH264 && backend <= RCS_BACKEND_DAV1D;
}
// One a session may choose (rcs_encoder_config.backend): AUTO, the system's or a
// library; HOST is reported only.
inline bool choosableBackend(uint32_t backend) noexcept {
    return backend < RCS_BACKEND_HOST;
}

inline const char* backendName(rcs_backend backend) noexcept {
    switch (backend) {
    case RCS_BACKEND_AUTO:
        return "automatic";
    case RCS_BACKEND_MEDIA_FOUNDATION:
        return "Media Foundation";
    case RCS_BACKEND_NVENC:
        return "NVENC";
    case RCS_BACKEND_VIDEOTOOLBOX:
        return "VideoToolbox";
    case RCS_BACKEND_MEDIACODEC:
        return "MediaCodec";
    case RCS_BACKEND_WEBCODECS:
        return "WebCodecs";
    case RCS_BACKEND_NVDEC:
        return "NVDEC";
    case RCS_BACKEND_ONEVPL:
        return "oneVPL";
    case RCS_BACKEND_OPENH264:
        return "OpenH264";
    case RCS_BACKEND_LIBVPX:
        return "libvpx";
    case RCS_BACKEND_LIBAOM:
        return "libaom";
    case RCS_BACKEND_DAV1D:
        return "dav1d";
    case RCS_BACKEND_HOST:
        return "host";
    default:
        return "unknown";
    }
}

// Whether the host's own encoder or decoder (RCS_EXT_HOST_ENCODER, _DECODER)
// handles `codec`.
template <class HostCodec>
bool handles(const HostCodec& config, rcs_codec codec) noexcept {
    return codec && std::find(std::begin(config.codecs), std::end(config.codecs), codec) != std::end(config.codecs);
}

// What a host's codec callback returned, as the run's failure: an error this
// version defines, or RCS_ERROR_CODEC for any other value.
inline rcs_result codecFailure(rcs_result result) noexcept {
    return result <= RCS_ERROR_INVALID_ARGUMENT && result >= RCS_ERROR_REFUSED ? result : RCS_ERROR_CODEC;
}

// ---- Picture formats (rcs_picture_format) ---------------------------------------

// Every format some version defines, as (1u << rcs_picture_format) bits.
inline constexpr uint32_t allPictureFormats = ((1u << (RCS_PICTURE_HDR10_444 + 1)) - 1) & ~1u;
inline constexpr uint32_t defaultPictureFormats = 1u << RCS_PICTURE_420_8;
inline bool knownPictureFormat(uint32_t format) noexcept {
    return format >= RCS_PICTURE_420_8 && format <= RCS_PICTURE_HDR10_444;
}
inline bool chroma444(rcs_picture_format format) noexcept {
    return format == RCS_PICTURE_444_8 || format == RCS_PICTURE_444_10 || format == RCS_PICTURE_HDR10_444;
}
inline bool hdr(rcs_picture_format format) noexcept {
    return format == RCS_PICTURE_HDR10_420 || format == RCS_PICTURE_HDR10_444;
}
inline unsigned bitDepth(rcs_picture_format format) noexcept {
    return format == RCS_PICTURE_420_8 || format == RCS_PICTURE_444_8 ? 8 : 10;
}
// The SDR formats, as (1u << rcs_picture_format) bits.
inline constexpr uint32_t sdrPictureFormats = (1u << RCS_PICTURE_420_8) | (1u << RCS_PICTURE_420_10) |
                                              (1u << RCS_PICTURE_444_8) | (1u << RCS_PICTURE_444_10);

// "4:4:4 8-bit", "HDR10 4:2:0", for messages.
inline const char* pictureFormatName(uint32_t format) noexcept {
    switch (format) {
    case RCS_PICTURE_420_8:
        return "4:2:0 8-bit";
    case RCS_PICTURE_420_10:
        return "4:2:0 10-bit";
    case RCS_PICTURE_444_8:
        return "4:4:4 8-bit";
    case RCS_PICTURE_444_10:
        return "4:4:4 10-bit";
    case RCS_PICTURE_HDR10_420:
        return "HDR10 4:2:0";
    case RCS_PICTURE_HDR10_444:
        return "HDR10 4:4:4";
    default:
        return "an unknown picture format";
    }
}

// What a stream's pictures are: their format, and how they are written as
// YCbCr (rcs_encoder_config.matrix, range). The matrix is the one in use:
// BT.2020's for HDR10 whatever the sharer's SDR matrix.
struct Picture {
    rcs_picture_format format = RCS_PICTURE_420_8;
    rcs_color_matrix matrix = RCS_MATRIX_BT709;
    rcs_color_range range = RCS_RANGE_LIMITED;
    bool operator==(const Picture&) const = default;
};

// A stream of `format` written with the sharer's SDR `matrix` and `range`.
inline Picture picture(rcs_picture_format format, rcs_color_matrix sdrMatrix, rcs_color_range range) noexcept {
    return {format, hdr(format) ? rcs_color_matrix(RCS_MATRIX_BT2020) : sdrMatrix, range};
}

// Whether `p` is a picture some stream may be: a known format, the matrix
// its dynamic range takes, a known range.
inline bool validPicture(const Picture& p) noexcept {
    const bool matrix = hdr(p.format) ? p.matrix == RCS_MATRIX_BT2020
                                      : p.matrix == RCS_MATRIX_BT709 || p.matrix == RCS_MATRIX_BT601;
    return knownPictureFormat(p.format) && matrix && (p.range == RCS_RANGE_LIMITED || p.range == RCS_RANGE_FULL);
}

// What a picture's values mean, as the public interface reports it.
inline rcs_color colorOf(const Picture& p) noexcept {
    const bool high = hdr(p.format);
    return {rcs_color_primaries(high ? RCS_PRIMARIES_BT2020 : RCS_PRIMARIES_BT709),
            rcs_color_transfer(high ? RCS_TRANSFER_PQ : RCS_TRANSFER_BT709), p.matrix, p.range};
}

// A picture's colour as ITU-T H.273 code points, as VP9's 'vpcC' record and
// AV1's sequence header carry it.
enum : uint8_t {
    cicpBt709 = 1,      // Primaries, transfer and matrix.
    cicpBt601 = 6,      // Matrix (SMPTE 170M).
    cicpBt2020 = 9,     // Primaries; matrix (non-constant luminance).
    cicpPq = 16         // Transfer (SMPTE ST 2084).
};
struct Cicp {
    uint8_t primaries = cicpBt709, transfer = cicpBt709, matrix = cicpBt709;
    bool operator==(const Cicp&) const = default;
};
inline Cicp cicpOf(const Picture& p) noexcept {
    const bool high = hdr(p.format);
    return {high ? cicpBt2020 : cicpBt709, high ? cicpPq : cicpBt709,
            p.matrix == RCS_MATRIX_BT2020 ? cicpBt2020 : p.matrix == RCS_MATRIX_BT601 ? cicpBt601 : cicpBt709};
}

// The colour of an RGB picture of these primaries and transfer: no matrix, no range.
inline rcs_color rgbColor(const rcs_color& color) noexcept {
    return {color.primaries, color.transfer, 0, 0};
}
inline bool rgbLayout(rcs_pixel_format format) noexcept {
    return format == RCS_PIXEL_BGRA8 || format == RCS_PIXEL_RGB10A2;
}

inline bool sameColor(const rcs_color& a, const rcs_color& b) noexcept {
    return a.primaries == b.primaries && a.transfer == b.transfer && a.matrix == b.matrix && a.range == b.range;
}
inline bool sameHdr(const rcs_hdr_metadata& a, const rcs_hdr_metadata& b) noexcept {
    const auto fields = [](const rcs_hdr_metadata& m) {
        return std::tie(m.red_x, m.red_y, m.green_x, m.green_y, m.blue_x, m.blue_y, m.white_x, m.white_y,
                        m.max_luminance, m.min_luminance, m.max_content_light, m.max_frame_average_light);
    };
    return fields(a) == fields(b);
}

// The picture a public description names (rcs_encoded_packet's format and
// color); valid only where the colour is what the format and matrix make.
inline bool pictureOf(rcs_picture_format format, const rcs_color& color, Picture& picture) noexcept {
    picture = {format, color.matrix, color.range};
    return validPicture(picture) && sameColor(colorOf(picture), color);
}

// "4:4:4 8-bit (BT.709, full range)", for messages and logs.
inline std::string pictureText(const Picture& p) {
    const char* matrix = p.matrix == RCS_MATRIX_BT601 ? "BT.601" : p.matrix == RCS_MATRIX_BT2020 ? "BT.2020" : "BT.709";
    return std::string(pictureFormatName(p.format)) + " (" + matrix + ", " +
           (p.range == RCS_RANGE_FULL ? "full" : "limited") + " range)";
}

// "4:4:4 8-bit, 4:2:0 8-bit" for a mask of formats; "none" when empty.
inline std::string pictureFormatList(uint32_t formats) {
    std::string list;
    for (uint32_t format = RCS_PICTURE_420_8; format <= RCS_PICTURE_HDR10_444; ++format) {
        if (formats & (1u << format)) {
            list += (list.empty() ? "" : ", ") + std::string(pictureFormatName(format));
        }
    }
    return list.empty() ? "none" : list;
}

struct Rect {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};

struct VideoFrame {
    uint32_t stream = 0; // Which of a sharer's streams (rcs_streams_config).
    uint32_t width = 0, height = 0;
    uint64_t sequence = 0;
    uint64_t timestampNs = 0;

    // Native GPU surface: CVPixelBufferRef on macOS, ID3D11Texture2D on Windows
    // (a whole texture of its own, never one slice of an array), AHardwareBuffer
    // on Android.
    std::shared_ptr<void> surface;
    // The surface's layout where its own format does not say (an Android
    // decoder's AIMAGE_FORMAT_PRIVATE buffers): the picture's; 0 otherwise.
    rcs_pixel_format layout = 0;
    std::shared_ptr<void> device; // ID3D11Device owning the surface (Windows).
    // What a YCbCr surface's values mean (its matrix and range too); RGB
    // pictures (BGRA8, CPU pixels) have matrix and range 0.
    rcs_color color{RCS_PRIMARIES_BT709, RCS_TRANSFER_BT709, RCS_MATRIX_BT709, RCS_RANGE_LIMITED};
    // Optional CPU wait for the GPU work that produced the surface.
    std::function<rcs_result(uint32_t timeoutMs)> waitGpu;

    // CPU BGRA image, used when there is no native surface.
    std::shared_ptr<const std::vector<uint8_t>> pixels;
    uint32_t stride = 0;
};

// H.264/HEVC: data and config are NAL units with 4-byte big-endian lengths;
// config carries the parameter sets of the current stream.
struct EncodedPacket {
    uint32_t stream = 0;   // Which of a sharer's streams (rcs_streams_config)...
    uint32_t encoding = 0; // ...and which of its encodings (RCS_EXT_ENCODINGS; 0 the main one).
    // Its temporal layer, and whether no later picture of its layer or a
    // lower one refers to it (media/layers.hpp reads both from the bitstream).
    uint32_t temporalId = 0;
    bool discardable = false;
    rcs_codec codec = RCS_CODEC_H264;
    Picture picture;
    uint32_t width = 0, height = 0;
    bool keyframe = false;
    uint64_t sequence = 0;
    uint64_t timestampNs = 0;
    // Encoded after the pictures following this sequence left the encoder's
    // references (reference invalidation): it refers to none of them, so a
    // viewer that lost one of them goes on from here.
    std::optional<uint64_t> recoversAfter;
    std::vector<uint8_t> data;
    std::vector<uint8_t> config;
    uint64_t encodeNs = 0; // Local: submit to output. Not transmitted.
};

// A packet as the host gets it (rcs_encoded_packet), borrowing its bytes;
// `afterGap`: packets before it were lost or dropped.
inline rcs_encoded_packet publicPacket(const EncodedPacket& p, bool afterGap = false) noexcept {
    rcs_encoded_packet view{};
    view.struct_size = sizeof(view);
    view.stream = p.stream;
    view.encoding = p.encoding;
    view.temporal_id = p.temporalId;
    view.discardable = p.discardable;
    view.codec = p.codec;
    view.format = p.picture.format;
    view.color = colorOf(p.picture);
    view.width = p.width;
    view.height = p.height;
    view.keyframe = p.keyframe;
    view.after_gap = afterGap;
    view.sequence = p.sequence;
    view.timestamp_ns = p.timestampNs;
    view.data = p.data.data();
    view.data_size = p.data.size();
    view.config = p.config.empty() ? nullptr : p.config.data();
    view.config_size = uint32_t(p.config.size());
    return view;
}

} // namespace rcsdk
