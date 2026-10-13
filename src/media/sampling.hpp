// The chroma format and bit depth a bitstream says it has: H.264's and HEVC's
// sequence parameter sets, a VP8 or VP9 keyframe's header, AV1's sequence
// header. What a stream's picture
// format (rcs_picture_format) must match; the SDK checks its encoders'
// output against it, so that none of them hands out another format than the
// one asked for. Malformed input throws Failure(RCS_ERROR_CODEC).
#pragma once

#include "media/av1.hpp"
#include "media/nal.hpp"
#include "media/video.hpp"
#include "media/vpx.hpp"
#include "util/util.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rcsdk {

// chroma: H.273's chroma_format_idc (0 monochrome, 1 4:2:0, 2 4:2:2, 3 4:4:4).
struct Sampling {
    unsigned chroma = 1, bitDepth = 8;
    bool operator==(const Sampling&) const = default;
};

// The sampling a picture format has.
inline Sampling samplingOf(rcs_picture_format format) noexcept {
    return {chroma444(format) ? 3u : 1u, bitDepth(format)};
}

// "4:2:0 8-bit", "4:2:2 10-bit", for messages.
inline std::string samplingText(const Sampling& s) {
    const char* chroma = s.chroma == 0 ? "monochrome" : s.chroma == 1 ? "4:2:0" : s.chroma == 2 ? "4:2:2" : "4:4:4";
    return std::string(chroma) + " " + std::to_string(s.bitDepth) + "-bit";
}

namespace detail {
// Bits of an RBSP: the NAL unit's payload without its emulation prevention bytes.
class RbspReader {
public:
    RbspReader(std::span<const uint8_t> nal, size_t header) {
        for (size_t i = header, zeros = 0; i < nal.size(); ++i) {
            if (zeros >= 2 && nal[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = nal[i] ? 0 : zeros + 1;
            bytes_.push_back(nal[i]);
        }
    }
    uint32_t read(unsigned count) {
        uint32_t value = 0;
        while (count--) {
            if (position_ >= bytes_.size() * 8) {
                fail(RCS_ERROR_CODEC, "Truncated parameter set");
            }
            value = (value << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1);
            ++position_;
        }
        return value;
    }
    uint32_t ue() {
        unsigned zeros = 0;
        while (!read(1)) {
            if (++zeros > 31) {
                fail(RCS_ERROR_CODEC, "Exp-Golomb overflow in a parameter set");
            }
        }
        return uint32_t((uint64_t{1} << zeros) - 1 + read(zeros));
    }

private:
    std::vector<uint8_t> bytes_;
    size_t position_ = 0;
};
} // namespace detail

// An H.264 SPS NAL unit (with its header byte): H.264 7.3.2.1.1.
inline Sampling h264Sampling(std::span<const uint8_t> sps) {
    if (sps.size() < 4 || (sps[0] & 0x1f) != 7) {
        fail(RCS_ERROR_CODEC, "Not an H.264 SPS");
    }
    detail::RbspReader r(sps, 1);
    const uint32_t profile = r.read(8);
    r.read(16); // Constraint flags, level.
    r.ue();     // seq_parameter_set_id
    Sampling s;
    switch (profile) { // The profiles whose SPS says its chroma format and depth.
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135:
        s.chroma = r.ue();
        if (s.chroma > 3) {
            fail(RCS_ERROR_CODEC, "Invalid H.264 chroma format");
        }
        if (s.chroma == 3) {
            r.read(1); // separate_colour_plane_flag
        }
        s.bitDepth = 8 + r.ue();
        if (s.bitDepth > 14) {
            fail(RCS_ERROR_CODEC, "Invalid H.264 bit depth");
        }
        break;
    default:
        break; // 4:2:0, 8 bits.
    }
    return s;
}

// An HEVC SPS NAL unit (with its two header bytes): H.265 7.3.2.2.
inline Sampling hevcSampling(std::span<const uint8_t> sps) {
    if (sps.size() < 4 || ((sps[0] >> 1) & 0x3f) != 33) {
        fail(RCS_ERROR_CODEC, "Not an HEVC SPS");
    }
    detail::RbspReader r(sps, 2);
    r.read(4); // sps_video_parameter_set_id
    const unsigned subLayers = r.read(3);
    r.read(1); // sps_temporal_id_nesting_flag
    // profile_tier_level(1, sps_max_sub_layers_minus1): the general part...
    r.read(8);  // profile space, tier, profile idc
    r.read(32); // compatibility flags
    r.read(32); // progressive..., 43 + 1 bits of constraint flags,
    r.read(16);
    r.read(8); // general_level_idc
    // ...and the sub-layers'.
    bool profilePresent[8]{}, levelPresent[8]{};
    for (unsigned i = 0; i < subLayers; ++i) {
        profilePresent[i] = r.read(1);
        levelPresent[i] = r.read(1);
    }
    if (subLayers > 0) {
        for (unsigned i = subLayers; i < 8; ++i) {
            r.read(2); // reserved_zero_2bits
        }
    }
    for (unsigned i = 0; i < subLayers; ++i) {
        if (profilePresent[i]) {
            r.read(32);
            r.read(32);
            r.read(24);
        }
        if (levelPresent[i]) {
            r.read(8);
        }
    }
    r.ue(); // sps_seq_parameter_set_id
    Sampling s;
    s.chroma = r.ue();
    if (s.chroma > 3) {
        fail(RCS_ERROR_CODEC, "Invalid HEVC chroma format");
    }
    if (s.chroma == 3) {
        r.read(1); // separate_colour_plane_flag
    }
    r.ue(); // pic_width_in_luma_samples
    r.ue(); // pic_height_in_luma_samples
    if (r.read(1)) { // conformance_window_flag
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    s.bitDepth = 8 + r.ue();
    if (s.bitDepth > 16) {
        fail(RCS_ERROR_CODEC, "Invalid HEVC bit depth");
    }
    return s;
}

// An AV1 sequence header OBU, header included.
inline Sampling av1Sampling(std::span<const uint8_t> sequenceHeader) {
    const auto info = av1::sequenceInfo(sequenceHeader);
    Sampling s;
    s.chroma = info.monochrome ? 0 : info.subsamplingX ? (info.subsamplingY ? 1 : 2) : 3;
    s.bitDepth = info.twelveBit ? 12 : info.highBitDepth ? 10 : 8;
    return s;
}

// A VP8 or VP9 keyframe's header.
inline Sampling vpSampling(const vpx::FrameHeader& h) noexcept {
    return {h.fullSubsampling ? 3u : h.subsampled ? 1u : 2u, h.bitDepth};
}

// The sampling of a standard codec's keyframe: its configuration's SPS or
// sequence header (rcs_encoded_packet.config), a VP frame's own header.
inline Sampling keyframeSampling(rcs_codec codec, std::span<const uint8_t> config, std::span<const uint8_t> data) {
    if (vpCodec(codec)) {
        return vpSampling(vpx::packetHeader(codec, data));
    }
    if (codec == RCS_CODEC_AV1) {
        return av1Sampling(config);
    }
    for (const auto unit : nal::units(config)) {
        if (codec == RCS_CODEC_H264 && !unit.empty() && (unit[0] & 0x1f) == 7) {
            return h264Sampling(unit);
        }
        if (codec == RCS_CODEC_HEVC && !unit.empty() && ((unit[0] >> 1) & 0x3f) == 33) {
            return hevcSampling(unit);
        }
    }
    fail(RCS_ERROR_CODEC, std::string("The ") + codecName(codec) + " configuration has no sequence parameter set");
}

// What is wrong with a keyframe of a standard codec whose configuration is
// not of `packet.picture`'s format, as "a configuration of ..."; empty when
// nothing is (host codecs, other packets: not looked at).
inline std::string samplingProblem(const EncodedPacket& packet) {
    if (hostCodec(packet.codec) || !packet.keyframe) {
        return {};
    }
    const Sampling wanted = samplingOf(packet.picture.format);
    try {
        const Sampling found = keyframeSampling(packet.codec, packet.config, packet.data);
        if (found == wanted) {
            return {};
        }
        return "a configuration of " + samplingText(found) + " pictures; the stream is " + samplingText(wanted);
    } catch (const Failure& failure) {
        return std::string("a configuration whose chroma format and bit depth cannot be read (") + failure.what() +
               ")";
    }
}

} // namespace rcsdk
