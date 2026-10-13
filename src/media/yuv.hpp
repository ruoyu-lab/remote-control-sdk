// YCbCr to BGRA8 on the CPU, for frames the host reads on the CPU where the
// platform did not convert them already. Any matrix (BT.709, BT.601, BT.2020)
// and range, from bi-planar layouts: NV12 and NV24 (8 bits), P010 and P410
// (10 bits in the top of 16-bit samples); and from the packed 4:4:4 ones of
// Windows' decoders: AYUV (8 bits) and Y410 (10 bits). 8-bit 4:2:0 limited or full range,
// the common case, runs on NEON on ARM and SSE2 on x86-64 (part of its
// baseline), both bit-exact with the scalar loop; the rest is scalar.
#pragma once

#include "util/api.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define RCSDK_ARM_NEON 1
#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define RCSDK_X86_SSE2 1
#endif

namespace rcsdk {

// The integer conversion of one range and matrix:
//   B = (yScale * max(0, Y - yOffset) + bU * (Cb - half)                     + round) >> shift
//   G = (yScale * max(0, Y - yOffset) - gU * (Cb - half) - gV * (Cr - half) + round) >> shift
//   R = (yScale * max(0, Y - yOffset)                    + rV * (Cr - half) + round) >> shift
// clamped to 0..255, with Y, Cb and Cr at `bits` bits.
struct YuvCoefficients {
    int yOffset = 16, half = 128, yScale = 298, bU = 541, gU = 55, gV = 136, rV = 459, shift = 8;
};

// BT.709 limited 8-bit is {16, 128, 298, 541, 55, 136, 459, 8}, BT.601 {.., 516, 100, 208, 409}.
inline YuvCoefficients yuvCoefficients(rcs_color_matrix matrix, rcs_color_range range, unsigned bits) noexcept {
    // Kr, Kb of each matrix (H.273 MatrixCoefficients 1, 6, 9).
    const double kr = matrix == RCS_MATRIX_BT601 ? 0.299 : matrix == RCS_MATRIX_BT2020 ? 0.2627 : 0.2126;
    const double kb = matrix == RCS_MATRIX_BT601 ? 0.114 : matrix == RCS_MATRIX_BT2020 ? 0.0593 : 0.0722;
    const double kg = 1 - kr - kb;
    const double top = double((1u << bits) - 1), unit = double(1u << (bits - 8)); // 1023 and 4 at 10 bits.
    const bool limited = range != RCS_RANGE_FULL;
    // Y' = (Y - yOffset) / luma and C' = (C - half) / chroma, Y' in 0..1 and C' in -0.5..0.5.
    const double luma = limited ? 219 * unit : top, chroma = limited ? 224 * unit : top;
    YuvCoefficients c;
    c.shift = int(bits);
    c.yOffset = limited ? int(16 * unit) : 0;
    c.half = int(1u << (bits - 1));
    const double scale = 255.0 * double(1u << c.shift);
    const auto fixed = [&](double value) { return int(std::lround(value * scale)); };
    c.yScale = fixed(1 / luma);
    c.bU = fixed(2 * (1 - kb) / chroma);
    c.gU = fixed(2 * kb * (1 - kb) / kg / chroma);
    c.gV = fixed(2 * kr * (1 - kr) / kg / chroma);
    c.rV = fixed(2 * (1 - kr) / chroma);
    return c;
}

#ifdef RCSDK_X86_SSE2
namespace detail {
// Eight pixels of one channel: (yScale * luma + first * u-or-v + second * v-or-one + 128) >> 8,
// clamped to 0..255, with 16-bit pairs multiplied and summed into 32 bits.
inline __m128i sse2Channel(__m128i lumaU, __m128i lumaUHigh, __m128i vOne, __m128i vOneHigh, int yScale, int u,
                           int v) {
    const __m128i first = _mm_set1_epi32(int((uint32_t(uint16_t(int16_t(u))) << 16) | uint32_t(yScale)));
    const __m128i second = _mm_set1_epi32(int((128u << 16) | uint32_t(uint16_t(int16_t(v)))));
    const __m128i low = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(lumaU, first), _mm_madd_epi16(vOne, second)), 8);
    const __m128i high =
        _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(lumaUHigh, first), _mm_madd_epi16(vOneHigh, second)), 8);
    return _mm_packs_epi32(low, high); // Saturated to 16 bits; packus clamps to 0..255.
}
} // namespace detail
#endif

// NV12 (8-bit 4:2:0) with independent plane strides; `c` of 8 bits.
// simd=false forces the scalar path, the reference for tests.
inline void nv12ToBgra(const uint8_t* yPlane, size_t yStride, const uint8_t* uvPlane, size_t uvStride, uint32_t width,
                       uint32_t height, uint8_t* output, size_t outputStride, const YuvCoefficients& c,
                       bool simd = true) {
    for (uint32_t row = 0; row < height; ++row) {
        const auto* y = yPlane + static_cast<size_t>(row) * yStride;
        const auto* uv = uvPlane + static_cast<size_t>(row / 2) * uvStride;
        auto* dst = output + static_cast<size_t>(row) * outputStride;
        uint32_t x = 0;
#ifdef RCSDK_ARM_NEON
        if (simd) {
            for (; x + 8 <= width; x += 8) {
                const auto luma = vmaxq_s16(
                    vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(y + x))), vdupq_n_s16(int16_t(c.yOffset))),
                    vdupq_n_s16(0));
                const auto unpacked = vuzp_u8(vld1_u8(uv + x), vld1_u8(uv + x));
                const auto us = vzip_u8(unpacked.val[0], unpacked.val[0]).val[0];
                const auto vs = vzip_u8(unpacked.val[1], unpacked.val[1]).val[0];
                const auto u = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(us)), vdupq_n_s16(128));
                const auto v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vs)), vdupq_n_s16(128));
                const auto channel = [&](int uCoefficient, int vCoefficient) {
                    auto low = vmull_n_s16(vget_low_s16(luma), static_cast<int16_t>(c.yScale));
                    auto high = vmull_n_s16(vget_high_s16(luma), static_cast<int16_t>(c.yScale));
                    low = vmlal_n_s16(low, vget_low_s16(u), static_cast<int16_t>(uCoefficient));
                    high = vmlal_n_s16(high, vget_high_s16(u), static_cast<int16_t>(uCoefficient));
                    low = vmlal_n_s16(low, vget_low_s16(v), static_cast<int16_t>(vCoefficient));
                    high = vmlal_n_s16(high, vget_high_s16(v), static_cast<int16_t>(vCoefficient));
                    low = vaddq_s32(low, vdupq_n_s32(128));
                    high = vaddq_s32(high, vdupq_n_s32(128));
                    return vqmovn_u16(vcombine_u16(vqshrun_n_s32(low, 8), vqshrun_n_s32(high, 8)));
                };
                const uint8x8x4_t pixels{{channel(c.bU, 0), channel(-c.gU, -c.gV), channel(0, c.rV), vdup_n_u8(255)}};
                vst4_u8(dst + static_cast<size_t>(x) * 4, pixels);
            }
        }
#elif defined(RCSDK_X86_SSE2)
        if (simd) {
            const __m128i zero = _mm_setzero_si128();
            for (; x + 8 <= width; x += 8) {
                const __m128i luma = _mm_max_epi16(
                    _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(y + x)), zero),
                                  _mm_set1_epi16(int16_t(c.yOffset))),
                    zero);
                // u0 v0 u1 v1 u2 v2 u3 v3 as 16-bit values; each pair serves two pixels.
                const __m128i chroma =
                    _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(uv + x)), zero),
                                  _mm_set1_epi16(128));
                const __m128i us = _mm_and_si128(chroma, _mm_set1_epi32(0xffff));
                const __m128i u = _mm_or_si128(us, _mm_slli_epi32(us, 16));
                const __m128i vs = _mm_srli_epi32(chroma, 16);
                const __m128i v = _mm_or_si128(vs, _mm_slli_epi32(vs, 16));
                const __m128i one = _mm_set1_epi16(1);
                const __m128i lumaU = _mm_unpacklo_epi16(luma, u), lumaUHigh = _mm_unpackhi_epi16(luma, u);
                const __m128i vOne = _mm_unpacklo_epi16(v, one), vOneHigh = _mm_unpackhi_epi16(v, one);
                const __m128i b = detail::sse2Channel(lumaU, lumaUHigh, vOne, vOneHigh, c.yScale, c.bU, 0);
                const __m128i g = detail::sse2Channel(lumaU, lumaUHigh, vOne, vOneHigh, c.yScale, -c.gU, -c.gV);
                const __m128i r = detail::sse2Channel(lumaU, lumaUHigh, vOne, vOneHigh, c.yScale, 0, c.rV);
                const __m128i bg = _mm_unpacklo_epi8(_mm_packus_epi16(b, zero), _mm_packus_epi16(g, zero));
                const __m128i ra = _mm_unpacklo_epi8(_mm_packus_epi16(r, zero), _mm_set1_epi8(-1) /* alpha 255 */);
                auto* out = reinterpret_cast<__m128i*>(dst + static_cast<size_t>(x) * 4);
                _mm_storeu_si128(out, _mm_unpacklo_epi16(bg, ra));
                _mm_storeu_si128(out + 1, _mm_unpackhi_epi16(bg, ra));
            }
        }
#else
        (void)simd;
#endif
        const int round = 1 << (c.shift - 1);
        for (; x < width; ++x) {
            const int luma = c.yScale * std::max(0, y[x] - c.yOffset);
            const int u = uv[x & ~1u] - c.half, v = uv[(x & ~1u) + 1] - c.half;
            dst[x * 4] = static_cast<uint8_t>(std::clamp((luma + c.bU * u + round) >> c.shift, 0, 255));
            dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp((luma - c.gU * u - c.gV * v + round) >> c.shift, 0, 255));
            dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp((luma + c.rV * v + round) >> c.shift, 0, 255));
            dst[x * 4 + 3] = 255;
        }
    }
}

// The bi-planar YCbCr layouts: chroma at half width and height (4:2:0) or full
// size (4:4:4), samples of 8 bits or 16 bits with the value in the top
// `bits` (P010, P410: rcs_pixel_format).
struct YuvLayout {
    bool chroma444 = false;
    unsigned bits = 8;
};

// Any layout, any matrix and range (`c` of layout.bits); strides in bytes.
// 8-bit 4:2:0 takes nv12ToBgra's SIMD path.
inline void yuvToBgra(const uint8_t* yPlane, size_t yStride, const uint8_t* uvPlane, size_t uvStride, uint32_t width,
                      uint32_t height, const YuvLayout& layout, const YuvCoefficients& c, uint8_t* output,
                      size_t outputStride) {
    if (layout.bits == 8 && !layout.chroma444) {
        nv12ToBgra(yPlane, yStride, uvPlane, uvStride, width, height, output, outputStride, c);
        return;
    }
    const bool wide = layout.bits > 8;
    const unsigned drop = wide ? 16 - layout.bits : 0;
    const auto sample = [&](const uint8_t* row, size_t index) {
        if (!wide) {
            return int(row[index]);
        }
        uint16_t value;
        std::copy_n(row + index * 2, 2, reinterpret_cast<uint8_t*>(&value)); // Little-endian, as both systems store it.
        return int(value >> drop);
    };
    const int round = 1 << (c.shift - 1);
    for (uint32_t row = 0; row < height; ++row) {
        const auto* y = yPlane + static_cast<size_t>(row) * yStride;
        const auto* uv = uvPlane + static_cast<size_t>(layout.chroma444 ? row : row / 2) * uvStride;
        auto* dst = output + static_cast<size_t>(row) * outputStride;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t at = layout.chroma444 ? size_t(x) * 2 : size_t(x & ~1u);
            const int luma = c.yScale * std::max(0, sample(y, x) - c.yOffset);
            const int u = sample(uv, at) - c.half, v = sample(uv, at + 1) - c.half;
            dst[x * 4] = static_cast<uint8_t>(std::clamp((luma + c.bU * u + round) >> c.shift, 0, 255));
            dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp((luma - c.gU * u - c.gV * v + round) >> c.shift, 0, 255));
            dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp((luma + c.rV * v + round) >> c.shift, 0, 255));
            dst[x * 4 + 3] = 255;
        }
    }
}

// The packed 4:4:4 layouts (rcs_pixel_format): AYUV, the bytes V, U, Y, A;
// Y410, a little-endian 32-bit word of U, Y and V (10 bits each, U lowest)
// and 2 bits of A. `c` of 8 bits for AYUV, 10 for Y410.
enum class PackedYuv { ayuv, y410 };
inline void packedYuvToBgra(const uint8_t* pixels, size_t stride, uint32_t width, uint32_t height, PackedYuv layout,
                            const YuvCoefficients& c, uint8_t* output, size_t outputStride) {
    const int round = 1 << (c.shift - 1);
    for (uint32_t row = 0; row < height; ++row) {
        const auto* src = pixels + static_cast<size_t>(row) * stride;
        auto* dst = output + static_cast<size_t>(row) * outputStride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p = src + size_t(x) * 4;
            int y, u, v;
            if (layout == PackedYuv::ayuv) {
                v = p[0], u = p[1], y = p[2];
            } else {
                const uint32_t word = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
                u = int(word & 0x3ff), y = int(word >> 10 & 0x3ff), v = int(word >> 20 & 0x3ff);
            }
            const int luma = c.yScale * std::max(0, y - c.yOffset);
            u -= c.half;
            v -= c.half;
            dst[x * 4] = static_cast<uint8_t>(std::clamp((luma + c.bU * u + round) >> c.shift, 0, 255));
            dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp((luma - c.gU * u - c.gV * v + round) >> c.shift, 0, 255));
            dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp((luma + c.rV * v + round) >> c.shift, 0, 255));
            dst[x * 4 + 3] = 255;
        }
    }
}

} // namespace rcsdk
