// NAL unit containers and header fields shared by H.264 and HEVC. Malformed
// input throws Failure(RCS_ERROR_CODEC).
#pragma once

#include "media/video.hpp"
#include "util/util.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rcsdk::nal {

// ---- Containers --------------------------------------------------------------

// Length-prefixed (4-byte big-endian) NAL units, as used in EncodedPacket.
inline void appendUnit(std::vector<uint8_t>& out, std::span<const uint8_t> nal) {
    const auto size = static_cast<uint32_t>(nal.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(size >> shift));
    }
    out.insert(out.end(), nal.begin(), nal.end());
}

// Splits length-prefixed NAL units.
inline std::vector<std::span<const uint8_t>> units(std::span<const uint8_t> buffer) {
    std::vector<std::span<const uint8_t>> result;
    size_t offset = 0;
    while (offset < buffer.size()) {
        if (buffer.size() - offset < 4) {
            fail(RCS_ERROR_CODEC, "Truncated NAL length");
        }
        const uint32_t size = (uint32_t(buffer[offset]) << 24) | (uint32_t(buffer[offset + 1]) << 16) |
                              (uint32_t(buffer[offset + 2]) << 8) | buffer[offset + 3];
        offset += 4;
        if (!size || size > buffer.size() - offset) {
            fail(RCS_ERROR_CODEC, "Invalid NAL length");
        }
        result.push_back(buffer.subspan(offset, size));
        offset += size;
    }
    return result;
}

// Splits an Annex B byte stream (00 00 01 / 00 00 00 01 start codes).
inline std::vector<std::span<const uint8_t>> annexBUnits(std::span<const uint8_t> stream) {
    const auto startCode = [&](size_t p) -> size_t {
        if (p + 3 <= stream.size() && !stream[p] && !stream[p + 1] && stream[p + 2] == 1) {
            return 3;
        }
        if (p + 4 <= stream.size() && !stream[p] && !stream[p + 1] && !stream[p + 2] && stream[p + 3] == 1) {
            return 4;
        }
        return 0;
    };
    std::vector<std::span<const uint8_t>> result;
    size_t p = 0;
    while (p < stream.size() && !startCode(p)) {
        ++p;
    }
    while (p < stream.size()) {
        const size_t begin = p + startCode(p);
        size_t end = begin;
        while (end < stream.size() && !startCode(end)) {
            ++end;
        }
        size_t length = end - begin;
        while (length && !stream[begin + length - 1]) { // Trailing zero bytes belong to the next start code.
            --length;
        }
        if (length) {
            result.push_back(stream.subspan(begin, length));
        }
        p = end;
    }
    return result;
}

inline void appendAnnexB(std::vector<uint8_t>& out, std::span<const uint8_t> nal) {
    static constexpr uint8_t start[] = {0, 0, 0, 1};
    out.insert(out.end(), std::begin(start), std::end(start));
    out.insert(out.end(), nal.begin(), nal.end());
}

// ---- Header fields -----------------------------------------------------------

// nal_unit_type: 5 bits after one forbidden bit and two nal_ref_idc bits.
namespace h264 {
enum : unsigned { idr = 5, sps = 7, pps = 8 };
inline unsigned type(std::span<const uint8_t> nal) noexcept {
    return nal.empty() ? 0 : nal[0] & 31;
}
} // namespace h264

// nal_unit_type: 6 bits after the forbidden bit, in a two-byte header.
namespace hevc {
enum : unsigned { firstIrap = 16, lastIrap = 23, vps = 32, sps = 33, pps = 34 };
inline unsigned type(std::span<const uint8_t> nal) noexcept {
    return nal.empty() ? 0 : (nal[0] >> 1) & 63;
}
} // namespace hevc

[[noreturn]] inline void notNalCodec(rcs_codec codec) {
    fail(RCS_ERROR_INTERNAL, std::string(codecName(codec)) + " has no NAL units");
}

// Parameter sets travel in EncodedPacket::config rather than with the pictures.
inline bool parameterSet(rcs_codec codec, std::span<const uint8_t> nal) {
    switch (codec) {
    case RCS_CODEC_H264:
        return h264::type(nal) == h264::sps || h264::type(nal) == h264::pps;
    case RCS_CODEC_HEVC:
        return hevc::type(nal) >= hevc::vps && hevc::type(nal) <= hevc::pps;
    default:
        notNalCodec(codec);
    }
}

// A picture decodable without earlier pictures (IDR; HEVC IRAP).
inline bool randomAccess(rcs_codec codec, std::span<const uint8_t> nal) {
    switch (codec) {
    case RCS_CODEC_H264:
        return h264::type(nal) == h264::idr;
    case RCS_CODEC_HEVC:
        return hevc::type(nal) >= hevc::firstIrap && hevc::type(nal) <= hevc::lastIrap;
    default:
        notNalCodec(codec);
    }
}

} // namespace rcsdk::nal
