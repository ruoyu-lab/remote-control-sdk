// YCbCr to BGRA8: NV12's SIMD (NEON on ARM, SSE2 on x86-64) equals the scalar
// reference for every matrix and range; the coefficients are those of the
// standards (BT.709 and BT.601 limited are the integers the SDK always used);
// every layout (NV12, P010, NV24, P410) gives what the floating-point formula
// gives; strides and padding are kept.
#include "media/yuv.hpp"
#include "support/check.hpp"

#include <array>
#include <tuple>
#include <cmath>
#include <cstring>
#include <vector>

using namespace rcsdk;

namespace {

uint32_t seed = 17;
uint8_t random8() {
    seed = seed * 1664525u + 1013904223u;
    return uint8_t(seed >> 24);
}

// The formula itself, in floating point: Y, Cb, Cr at `bits` bits to 8-bit R, G, B.
std::array<double, 3> reference(int y, int cb, int cr, rcs_color_matrix matrix, rcs_color_range range, unsigned bits) {
    const double kr = matrix == RCS_MATRIX_BT601 ? 0.299 : matrix == RCS_MATRIX_BT2020 ? 0.2627 : 0.2126;
    const double kb = matrix == RCS_MATRIX_BT601 ? 0.114 : matrix == RCS_MATRIX_BT2020 ? 0.0593 : 0.0722;
    const double unit = double(1u << (bits - 8)), top = double((1u << bits) - 1);
    const bool full = range == RCS_RANGE_FULL;
    const double luma = full ? y / top : std::max(0.0, y - 16 * unit) / (219 * unit);
    const double half = double(1u << (bits - 1));
    const double u = (cb - half) / (full ? top : 224 * unit), v = (cr - half) / (full ? top : 224 * unit);
    const double kg = 1 - kr - kb;
    const double r = luma + 2 * (1 - kr) * v, b = luma + 2 * (1 - kb) * u;
    const double g = (luma - kr * r - kb * b) / kg;
    return {std::clamp(b * 255, 0.0, 255.0), std::clamp(g * 255, 0.0, 255.0), std::clamp(r * 255, 0.0, 255.0)};
}

} // namespace

int main() {
    return runTest("yuv", [] {
        // The integers the SDK used before (BT.709 and BT.601, limited range).
        const auto bt709 = yuvCoefficients(RCS_MATRIX_BT709, RCS_RANGE_LIMITED, 8);
        CHECK(bt709.yOffset == 16 && bt709.half == 128 && bt709.yScale == 298 && bt709.bU == 541 && bt709.gU == 55 &&
              bt709.gV == 136 && bt709.rV == 459 && bt709.shift == 8);
        const auto bt601 = yuvCoefficients(RCS_MATRIX_BT601, RCS_RANGE_LIMITED, 8);
        CHECK(bt601.bU == 516 && bt601.gU == 100 && bt601.gV == 208 && bt601.rV == 409);
        const auto full = yuvCoefficients(RCS_MATRIX_BT709, RCS_RANGE_FULL, 8);
        CHECK(full.yOffset == 0 && full.yScale == 256);
        const auto ten = yuvCoefficients(RCS_MATRIX_BT2020, RCS_RANGE_LIMITED, 10);
        CHECK(ten.yOffset == 64 && ten.half == 512 && ten.shift == 10);

        // SIMD equals scalar, for each matrix and range.
        for (uint32_t width : {16u, 18u, 32u, 62u, 1920u}) {
            const uint32_t height = 18;
            const size_t yStride = width + 32, uvStride = width + 16, outStride = width * 4 + 24;
            std::vector<uint8_t> y(yStride * height), uv(uvStride * height / 2);
            for (auto& byte : y) {
                byte = random8();
            }
            for (auto& byte : uv) {
                byte = random8();
            }
            for (const auto matrix : {RCS_MATRIX_BT709, RCS_MATRIX_BT601, RCS_MATRIX_BT2020}) {
                for (const auto range : {RCS_RANGE_LIMITED, RCS_RANGE_FULL}) {
                    const auto c = yuvCoefficients(matrix, range, 8);
                    std::vector<uint8_t> scalar(outStride * height + 32, 0x7b), simd(scalar);
                    nv12ToBgra(y.data(), yStride, uv.data(), uvStride, width, height, scalar.data() + 16, outStride,
                               c, false);
                    nv12ToBgra(y.data(), yStride, uv.data(), uvStride, width, height, simd.data() + 16, outStride, c,
                               true);
                    CHECK(scalar == simd);
                    for (size_t i = 0; i < 16; ++i) { // Guard bytes around the image are untouched.
                        CHECK(simd[i] == 0x7b && simd[simd.size() - 1 - i] == 0x7b);
                    }
                    for (uint32_t row = 0; row < height; ++row) { // Row padding is untouched.
                        for (size_t i = width * 4; i < outStride; ++i) {
                            CHECK(simd[16 + row * outStride + i] == 0x7b);
                        }
                    }
                }
            }
        }

        // Black and white at either range.
        std::array<uint8_t, 16> y{};
        std::array<uint8_t, 8> uv{};
        std::array<uint8_t, 64> pixels{};
        uv.fill(128);
        for (const auto [range, black, white] :
             {std::tuple{RCS_RANGE_LIMITED, 16, 235}, std::tuple{RCS_RANGE_FULL, 0, 255}}) {
            const auto c = yuvCoefficients(RCS_MATRIX_BT709, range, 8);
            y.fill(uint8_t(black));
            nv12ToBgra(y.data(), 8, uv.data(), 8, 8, 2, pixels.data(), 32, c);
            for (size_t i = 0; i < pixels.size(); i += 4) {
                CHECK(!pixels[i] && !pixels[i + 1] && !pixels[i + 2] && pixels[i + 3] == 255);
            }
            y.fill(uint8_t(white));
            nv12ToBgra(y.data(), 8, uv.data(), 8, 8, 2, pixels.data(), 32, c);
            for (auto byte : pixels) {
                CHECK(byte == 255);
            }
        }

        // Every layout against the formula, within rounding: random pixels,
        // each with its own chroma in 4:4:4 (a neighbour's differs).
        constexpr uint32_t width = 10, height = 4;
        for (const bool chroma444 : {false, true}) {
            for (const unsigned bits : {8u, 10u}) {
                const size_t sample = bits > 8 ? 2 : 1;
                const size_t yStride = width * sample + 6, uvStride = (chroma444 ? width : width / 2) * 2 * sample + 4;
                const uint32_t uvRows = chroma444 ? height : height / 2;
                std::vector<uint8_t> yPlane(yStride * height), uvPlane(uvStride * uvRows);
                const auto put = [&](std::vector<uint8_t>& plane, size_t offset, int value) {
                    if (bits > 8) {
                        const uint16_t stored = uint16_t(value << (16 - bits)); // In the top bits.
                        std::memcpy(plane.data() + offset, &stored, 2);
                    } else {
                        plane[offset] = uint8_t(value);
                    }
                };
                const int top = (1 << bits) - 1;
                const auto value = [&] { return int((uint32_t(random8()) << 8 | random8()) % uint32_t(top + 1)); };
                for (uint32_t row = 0; row < height; ++row) {
                    for (uint32_t x = 0; x < width; ++x) {
                        put(yPlane, row * yStride + x * sample, value());
                    }
                }
                for (uint32_t row = 0; row < uvRows; ++row) {
                    for (uint32_t x = 0; x < (chroma444 ? width : width / 2) * 2; ++x) {
                        put(uvPlane, row * uvStride + x * sample, value());
                    }
                }
                const auto read = [&](const std::vector<uint8_t>& plane, size_t offset) {
                    if (bits > 8) {
                        uint16_t stored;
                        std::memcpy(&stored, plane.data() + offset, 2);
                        return int(stored >> (16 - bits));
                    }
                    return int(plane[offset]);
                };
                for (const auto matrix : {RCS_MATRIX_BT709, RCS_MATRIX_BT601, RCS_MATRIX_BT2020}) {
                    for (const auto range : {RCS_RANGE_LIMITED, RCS_RANGE_FULL}) {
                        std::vector<uint8_t> out(width * 4 * height + 8, 0x7b);
                        yuvToBgra(yPlane.data(), yStride, uvPlane.data(), uvStride, width, height, {chroma444, bits},
                                  yuvCoefficients(matrix, range, bits), out.data(), width * 4);
                        for (uint32_t row = 0; row < height; ++row) {
                            for (uint32_t x = 0; x < width; ++x) {
                                const size_t uvAt = (chroma444 ? row : row / 2) * uvStride +
                                                    (chroma444 ? x * 2 : x & ~1u) * sample;
                                const auto expected =
                                    reference(read(yPlane, row * yStride + x * sample), read(uvPlane, uvAt),
                                              read(uvPlane, uvAt + sample), matrix, range, bits);
                                const uint8_t* got = out.data() + (row * width + x) * 4;
                                for (int channel = 0; channel < 3; ++channel) {
                                    CHECK(std::abs(got[channel] - expected[size_t(channel)]) <= 1.5);
                                }
                                CHECK(got[3] == 255);
                            }
                        }
                        CHECK(out[width * 4 * height] == 0x7b); // Nothing beyond the image.
                        if (!chroma444) {
                            continue;
                        }
                        // The same samples packed as Windows' decoders give
                        // them (AYUV: V, U, Y, A; Y410: U, Y, V in a word)
                        // convert to the same pixels.
                        const size_t packedStride = width * 4 + 12;
                        std::vector<uint8_t> packed(packedStride * height);
                        for (uint32_t row = 0; row < height; ++row) {
                            for (uint32_t x = 0; x < width; ++x) {
                                const size_t uvAt = row * uvStride + x * 2 * sample;
                                const int y = read(yPlane, row * yStride + x * sample), u = read(uvPlane, uvAt),
                                          v = read(uvPlane, uvAt + sample);
                                uint8_t* p = packed.data() + row * packedStride + x * 4;
                                if (bits == 8) {
                                    p[0] = uint8_t(v), p[1] = uint8_t(u), p[2] = uint8_t(y), p[3] = 255;
                                } else {
                                    const uint32_t word = uint32_t(u) | uint32_t(y) << 10 | uint32_t(v) << 20 | 3u << 30;
                                    std::memcpy(p, &word, 4);
                                }
                            }
                        }
                        std::vector<uint8_t> fromPacked(width * 4 * height);
                        packedYuvToBgra(packed.data(), packedStride, width, height,
                                        bits == 8 ? PackedYuv::ayuv : PackedYuv::y410,
                                        yuvCoefficients(matrix, range, bits), fromPacked.data(), width * 4);
                        CHECK(std::equal(fromPacked.begin(), fromPacked.end(), out.begin()));
                    }
                }
            }
        }
    });
}
