// VP8 and VP9 bitstream helpers: the fixed fields of a frame's header (VP8:
// RFC 6386 section 9.1; VP9: the bitstream specification's uncompressed
// header, section 6.2), VP9's superframe index (Annex B), and the VP codec
// configuration record ('vpcC', VP Codec ISO Media File Format Binding 2.2)
// the SDK carries as a VP stream's config (rcs_encoded_packet). Only fixed
// positions and indexes are read, never entropy-coded data; malformed input
// throws Failure(RCS_ERROR_CODEC), and nothing reads outside what it is given.
#pragma once

#include "media/video.hpp"
#include "util/util.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rcsdk::vpx {

// VP9's color_space (6.2.2).
enum : unsigned {
    csUnknown = 0,
    csBt601 = 1,
    csBt709 = 2,
    csSmpte170 = 3,
    csSmpte240 = 4,
    csBt2020 = 5,
    csReserved = 6,
    csRgb = 7
};

// What a frame's header says. The size and colour are a keyframe's (VP9: an
// intra-only frame's too); other frames take them from the stream.
struct FrameHeader {
    bool keyframe = false;
    bool intraOnly = false;   // VP9: decodable without references, not a keyframe.
    bool showExisting = false; // VP9: shows a frame decoded before, carries none.
    bool shown = true;
    unsigned profile = 0; // VP8: its version.
    uint32_t width = 0, height = 0;
    unsigned bitDepth = 8;
    bool subsampled = true; // 4:2:0 (VP9 also has 4:2:2 and 4:4:0: not the SDK's).
    bool fullSubsampling = false; // 4:4:4.
    unsigned colorSpace = csUnknown;
    bool fullRange = false;
    // VP9: the reference slots the frame refreshes; 0 for a frame nothing
    // refers to (discardable). 0xff for keyframes.
    unsigned refreshFrameFlags = 0xff;
};

namespace detail {
class Bits {
public:
    explicit Bits(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    uint32_t read(unsigned count) {
        uint32_t value = 0;
        while (count--) {
            if (position_ >= bytes_.size() * 8) {
                fail(RCS_ERROR_CODEC, "Truncated VP9 frame header");
            }
            value = (value << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1);
            ++position_;
        }
        return value;
    }
    bool flag() { return read(1) != 0; }

private:
    std::span<const uint8_t> bytes_;
    size_t position_ = 0;
};
} // namespace detail

// A VP8 frame (RFC 6386 9.1): the frame tag, and a keyframe's start code and size.
inline FrameHeader vp8Header(std::span<const uint8_t> frame) {
    if (frame.size() < 3) {
        fail(RCS_ERROR_CODEC, "A VP8 frame shorter than its frame tag");
    }
    const uint32_t tag = frame[0] | (uint32_t(frame[1]) << 8) | (uint32_t(frame[2]) << 16);
    FrameHeader h;
    h.keyframe = !(tag & 1);
    h.profile = (tag >> 1) & 7;
    h.shown = (tag >> 4) & 1;
    if (h.profile > 3) {
        fail(RCS_ERROR_CODEC, "A VP8 frame of version " + std::to_string(h.profile) + " (0..3 are defined)");
    }
    if (h.keyframe) {
        if (frame.size() < 10 || frame[3] != 0x9d || frame[4] != 0x01 || frame[5] != 0x2a) {
            fail(RCS_ERROR_CODEC, "A VP8 keyframe without its start code");
        }
        h.width = (frame[6] | (uint32_t(frame[7]) << 8)) & 0x3fff;
        h.height = (frame[8] | (uint32_t(frame[9]) << 8)) & 0x3fff;
    }
    return h;
}

namespace detail {
inline void vp9ColorConfig(Bits& b, FrameHeader& h) {
    h.bitDepth = h.profile >= 2 ? (b.flag() ? 12 : 10) : 8;
    h.colorSpace = b.read(3);
    if (h.colorSpace != csRgb) {
        h.fullRange = b.flag();
        if (h.profile == 1 || h.profile == 3) {
            const bool x = b.flag(), y = b.flag();
            h.subsampled = x && y;
            h.fullSubsampling = !x && !y;
            if (b.flag()) {
                fail(RCS_ERROR_CODEC, "A VP9 colour configuration with its reserved bit set");
            }
            if (h.subsampled) {
                fail(RCS_ERROR_CODEC, "A VP9 profile " + std::to_string(h.profile) + " frame of 4:2:0 pictures");
            }
        }
    } else {
        h.fullRange = true;
        h.subsampled = false;
        h.fullSubsampling = true;
        if (h.profile == 1 || h.profile == 3) {
            b.flag();
        } else {
            fail(RCS_ERROR_CODEC, "A VP9 profile " + std::to_string(h.profile) + " frame of RGB pictures");
        }
    }
}

inline void vp9Size(Bits& b, FrameHeader& h) {
    h.width = b.read(16) + 1;
    h.height = b.read(16) + 1;
}

inline void vp9Sync(Bits& b) {
    if (b.read(8) != 0x49 || b.read(8) != 0x83 || b.read(8) != 0x42) {
        fail(RCS_ERROR_CODEC, "A VP9 frame without its sync code");
    }
}
} // namespace detail

// One VP9 frame's uncompressed header, as far as the fields above go.
inline FrameHeader vp9Header(std::span<const uint8_t> frame) {
    detail::Bits b(frame);
    if (b.read(2) != 2) {
        fail(RCS_ERROR_CODEC, "A VP9 frame without its frame marker");
    }
    FrameHeader h;
    const unsigned low = b.read(1);
    h.profile = (b.read(1) << 1) | low;
    if (h.profile == 3 && b.flag()) {
        fail(RCS_ERROR_CODEC, "A VP9 profile 3 frame with its reserved bit set");
    }
    h.showExisting = b.flag();
    if (h.showExisting) {
        b.read(3);
        h.refreshFrameFlags = 0;
        return h;
    }
    h.keyframe = !b.flag();
    h.shown = b.flag();
    const bool errorResilient = b.flag();
    if (h.keyframe) {
        detail::vp9Sync(b);
        detail::vp9ColorConfig(b, h);
        detail::vp9Size(b, h);
        return h;
    }
    h.intraOnly = !h.shown && b.flag();
    if (!errorResilient) {
        b.read(2); // reset_frame_context
    }
    if (h.intraOnly) {
        detail::vp9Sync(b);
        if (h.profile > 0) {
            detail::vp9ColorConfig(b, h);
        } else {
            h.colorSpace = csBt601;
        }
        h.refreshFrameFlags = b.read(8);
        detail::vp9Size(b, h);
        return h;
    }
    h.refreshFrameFlags = b.read(8);
    return h;
}

// The frames of a VP9 packet: those its superframe index lists (each
// non-empty, together exactly the data before the index), or the whole
// packet as one frame.
inline std::vector<std::span<const uint8_t>> vp9Frames(std::span<const uint8_t> data) {
    if (data.empty()) {
        fail(RCS_ERROR_CODEC, "An empty VP9 packet");
    }
    const uint8_t marker = data.back();
    if ((marker & 0xe0) != 0xc0) {
        return {data};
    }
    const size_t frames = (marker & 7) + 1, magnitude = ((marker >> 3) & 3) + 1;
    const size_t index = 2 + magnitude * frames;
    if (data.size() < index || data[data.size() - index] != marker) {
        return {data}; // The last byte of a frame merely looks like a marker.
    }
    std::vector<std::span<const uint8_t>> list;
    size_t at = 0;
    const uint8_t* sizes = data.data() + data.size() - index + 1;
    for (size_t i = 0; i < frames; ++i) {
        size_t size = 0;
        for (size_t b = 0; b < magnitude; ++b) {
            size |= size_t(sizes[i * magnitude + b]) << (8 * b);
        }
        if (!size || size > data.size() - index - at) {
            fail(RCS_ERROR_CODEC, "A VP9 superframe index whose frame sizes do not fit the packet");
        }
        list.push_back(data.subspan(at, size));
        at += size;
    }
    if (at != data.size() - index) {
        fail(RCS_ERROR_CODEC, "A VP9 superframe index that leaves bytes of the packet out");
    }
    return list;
}

// A packet's header: VP8's frame, VP9's first frame (a superframe's first
// frame decides whether the packet is a keyframe).
inline FrameHeader packetHeader(rcs_codec codec, std::span<const uint8_t> data) {
    return codec == RCS_CODEC_VP8 ? vp8Header(data) : vp9Header(vp9Frames(data).front());
}

// ---- The configuration record ('vpcC', version 1) ------------------------------

// vpcC's chromaSubsampling.
enum : uint8_t { subsampling420Vertical = 0, subsampling420 = 1, subsampling422 = 2, subsampling444 = 3 };

struct Config {
    unsigned profile = 0, level = 0, bitDepth = 8, chroma = subsampling420;
    bool fullRange = false;
    Cicp color; // ISO/IEC 23091-2 codes.
    bool operator==(const Config&) const = default;
};
inline constexpr size_t configSize = 12;

inline std::vector<uint8_t> writeConfig(const Config& c) {
    return {1, 0, 0, 0, // version 1, flags 0
            uint8_t(c.profile),
            uint8_t(c.level),
            uint8_t((c.bitDepth << 4) | (c.chroma << 1) | (c.fullRange ? 1 : 0)),
            c.color.primaries,
            c.color.transfer,
            c.color.matrix,
            0, 0}; // No codec initialization data.
}

inline Config readConfig(std::span<const uint8_t> bytes) {
    if (bytes.size() != configSize || bytes[0] != 1 || bytes[1] || bytes[2] || bytes[3] || bytes[10] || bytes[11]) {
        fail(RCS_ERROR_CODEC, "A VP codec configuration that is not 12 bytes of version 1 without initialization data");
    }
    Config c;
    c.profile = bytes[4];
    c.level = bytes[5];
    c.bitDepth = bytes[6] >> 4;
    c.chroma = (bytes[6] >> 1) & 7;
    c.fullRange = bytes[6] & 1;
    c.color.primaries = bytes[7];
    c.color.transfer = bytes[8];
    c.color.matrix = bytes[9];
    return c;
}

// VP9's level for pictures of this size at this rate (the specification's
// Annex A: luma samples per picture and per second), as vpcC writes it: 41
// is level 4.1.
inline unsigned vp9Level(uint32_t width, uint32_t height, uint32_t framesPerSecond) noexcept {
    struct Level {
        unsigned level;
        uint64_t rate, size;
    };
    static constexpr Level levels[] = {
        {10, 829'440, 36'864},           {11, 2'764'800, 73'728},         {20, 4'608'000, 122'880},
        {21, 9'216'000, 245'760},        {30, 20'736'000, 552'960},       {31, 36'864'000, 983'040},
        {40, 83'558'400, 2'228'224},     {41, 160'432'128, 2'228'224},    {50, 311'951'360, 8'912'896},
        {51, 588'251'136, 8'912'896},    {52, 1'176'502'272, 8'912'896},  {60, 1'176'502'272, 35'651'584},
        {61, 2'353'004'544, 35'651'584}, {62, 4'706'009'088, 35'651'584}};
    const uint64_t size = uint64_t(width) * height, rate = size * framesPerSecond;
    for (const auto& l : levels) {
        if (size <= l.size && rate <= l.rate) {
            return l.level;
        }
    }
    return 62;
}

// What a stream of `picture` writes: the profile its format takes in `codec`.
inline Config streamConfig(rcs_codec codec, const Picture& picture, uint32_t width, uint32_t height,
                           uint32_t framesPerSecond) {
    Config c;
    const bool full = chroma444(picture.format), deep = bitDepth(picture.format) > 8;
    c.profile = codec == RCS_CODEC_VP8 ? 0 : (deep ? 2 : 0) + (full ? 1 : 0);
    c.level = codec == RCS_CODEC_VP8 ? 0 : vp9Level(width, height, framesPerSecond);
    c.bitDepth = bitDepth(picture.format);
    c.chroma = full ? subsampling444 : subsampling420;
    c.fullRange = picture.range == RCS_RANGE_FULL;
    c.color = cicpOf(picture);
    return c;
}

// VP9's color_space for a picture's matrix.
inline unsigned colorSpace(const Picture& picture) noexcept {
    return picture.matrix == RCS_MATRIX_BT2020 ? csBt2020 : picture.matrix == RCS_MATRIX_BT601 ? csBt601 : csBt709;
}

// What is wrong with a VP8 or VP9 packet of `picture` and this size whose
// configuration record is `config`: as videoProblem says it; empty when
// nothing is. The record against the stream, its frames' boundaries, its
// keyframe flag, and a keyframe's size and sampling against the packet and
// the record.
inline std::string packetProblem(rcs_codec codec, const Picture& picture, uint32_t width, uint32_t height,
                                 bool keyframe, std::span<const uint8_t> data, std::span<const uint8_t> config) {
    try {
        const Config c = readConfig(config);
        const Config wanted = streamConfig(codec, picture, width, height, 1);
        if (c.bitDepth != wanted.bitDepth || c.chroma != wanted.chroma || c.fullRange != wanted.fullRange ||
            c.color != wanted.color ||
            (codec == RCS_CODEC_VP9 && c.profile != wanted.profile) || (codec == RCS_CODEC_VP8 && c.profile > 3)) {
            return "a configuration record that is not the stream's (" + pictureText(picture) + ")";
        }
        const FrameHeader h = packetHeader(codec, data);
        if (h.keyframe != keyframe) {
            return keyframe ? "a keyframe flag on a frame that is not one" : "a keyframe without the keyframe flag";
        }
        if (!h.keyframe) {
            return {};
        }
        if (h.width != width || h.height != height) {
            return "a keyframe of " + std::to_string(h.width) + "x" + std::to_string(h.height) + " in a packet of " +
                   std::to_string(width) + "x" + std::to_string(height);
        }
        // The colour the frame header writes is not checked: the stream's is
        // the packet's (hardware encoders write their own idea of it).
        if (codec == RCS_CODEC_VP9 &&
            (h.profile != c.profile || h.bitDepth != c.bitDepth || h.fullSubsampling != (c.chroma == subsampling444))) {
            return "a keyframe whose profile, bit depth or sampling is not the configuration's";
        }
        return {};
    } catch (const Failure& failure) {
        return failure.what();
    }
}

} // namespace rcsdk::vpx
