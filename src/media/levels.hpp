// The picture rate a codec level allows, in pixels a second: what an encoder
// whose highest level is known (NVENC: NV_ENC_CAPS_LEVEL_MAX) can encode at
// most (CodecImplementation::maxPixelRate). H.264: the level's macroblocks a
// second (Table A-1) of 256 pixels; HEVC: its luma samples a second (Table
// A.8); AV1: its MaxDisplayRate (Annex A.3). 0 for a level the codec does not
// define.
#pragma once

#include "rcsdk/rcsdk.h"

#include <cstdint>
#include <iterator>

namespace rcsdk {

// `levelIdc` as the bitstream says it: H.264 10 × the level (9: 1b); HEVC
// 30 × the level; AV1 seq_level_idx (0: 2.0, 1: 2.1, ..., 4: 3.0, ... 23: 7.3).
inline uint64_t levelPixelRate(rcs_codec codec, uint32_t levelIdc) noexcept {
    switch (codec) {
    case RCS_CODEC_H264: {
        uint64_t macroblocks = 0;
        switch (levelIdc) {
        case 9: case 10: macroblocks = 1'485; break;
        case 11: macroblocks = 3'000; break;
        case 12: macroblocks = 6'000; break;
        case 13: case 20: macroblocks = 11'880; break;
        case 21: macroblocks = 19'800; break;
        case 22: macroblocks = 20'250; break;
        case 30: macroblocks = 40'500; break;
        case 31: macroblocks = 108'000; break;
        case 32: macroblocks = 216'000; break;
        case 40: case 41: macroblocks = 245'760; break;
        case 42: macroblocks = 522'240; break;
        case 50: macroblocks = 589'824; break;
        case 51: macroblocks = 983'040; break;
        case 52: macroblocks = 2'073'600; break;
        case 60: macroblocks = 4'177'920; break;
        case 61: macroblocks = 8'355'840; break;
        case 62: macroblocks = 16'711'680; break;
        default: break;
        }
        return macroblocks * 256;
    }
    case RCS_CODEC_HEVC:
        switch (levelIdc) {
        case 30: return 552'960;
        case 60: return 3'686'400;
        case 63: return 7'372'800;
        case 90: return 16'588'800;
        case 93: return 33'177'600;
        case 120: return 66'846'720;
        case 123: return 133'693'440;
        case 150: return 267'386'880;
        case 153: return 534'773'760;
        case 156: case 180: return 1'069'547'520;
        case 183: return 2'139'095'040;
        case 186: return 4'278'190'080;
        default: return 0;
        }
    case RCS_CODEC_AV1: {
        // 2.0 2.1 (2.2 2.3) 3.0 3.1 (3.2 3.3) 4.0 4.1 (4.2 4.3) 5.0 5.1 5.2
        // 5.3 6.0 6.1 6.2 6.3; those in brackets and 7.x are reserved.
        static constexpr uint64_t rates[] = {
            4'423'680,   8'363'520,     0,             0,             19'975'680,    31'950'720,    0,
            0,           70'778'880,    141'557'760,   0,             0,             267'386'880,   534'773'760,
            1'069'547'520, 1'069'547'520, 1'069'547'520, 2'139'095'040, 4'278'190'080, 4'278'190'080};
        return levelIdc < std::size(rates) ? rates[levelIdc] : 0;
    }
    default:
        return 0;
    }
}

// The pixels of a width x height picture as levels count them: H.264 whole
// 16x16 macroblocks, the others the picture's own.
inline uint64_t levelPixels(rcs_codec codec, uint32_t width, uint32_t height) noexcept {
    if (codec == RCS_CODEC_H264) {
        return uint64_t((width + 15) / 16) * ((height + 15) / 16) * 256;
    }
    return uint64_t(width) * height;
}

} // namespace rcsdk
