// Pictures between the codec libraries built into the SDK and the rest of
// it: the libraries read and write planar YCbCr (I420, I444; 16-bit samples
// with the value in the low bits), the SDK's surfaces are bi-planar (NV12,
// P010, NV24, P410: YuvImage). Conversions both ways, from the host's BGRA8
// pictures (capture source HOST) to a stream's layout, and to BGRA8 for the
// host's CPU frames.
#pragma once

#include "media/video.hpp"
#include "media/yuv.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace rcsdk {

// A picture on the CPU in one of the SDK's YCbCr layouts (rcs_pixel_format):
// NV12 and NV24 (8 bits), P010 and P410 (16-bit samples, the value in the top
// 10 bits); luma, then chroma with Cb and Cr interleaved, at half width and
// height (4:2:0) or full size (4:4:4). Strides in bytes.
struct YuvImage {
    rcs_pixel_format layout = RCS_PIXEL_NV12;
    uint32_t width = 0, height = 0;
    const uint8_t* luma = nullptr;
    size_t lumaStride = 0;
    const uint8_t* chroma = nullptr;
    size_t chromaStride = 0;
};

// The layout of a stream of `format`.
inline rcs_pixel_format yuvLayoutOf(rcs_picture_format format) noexcept {
    const bool full = chroma444(format), deep = bitDepth(format) > 8;
    return full ? (deep ? RCS_PIXEL_P410 : RCS_PIXEL_NV24) : (deep ? RCS_PIXEL_P010 : RCS_PIXEL_NV12);
}

inline bool wideLayout(rcs_pixel_format layout) noexcept {
    return layout == RCS_PIXEL_P010 || layout == RCS_PIXEL_P410;
}
inline bool fullChromaLayout(rcs_pixel_format layout) noexcept {
    return layout == RCS_PIXEL_NV24 || layout == RCS_PIXEL_P410;
}

// Three planes as a library reads or writes them: 8-bit samples, or 16-bit
// ones holding `bits` low bits.
struct Planes {
    const uint8_t* plane[3] = {};
    size_t stride[3] = {};
    unsigned bits = 8;
};

// A bi-planar picture the SDK owns.
class YuvBuffer {
public:
    void resize(rcs_pixel_format layout, uint32_t width, uint32_t height) {
        layout_ = layout;
        width_ = width;
        height_ = height;
        const size_t sample = wideLayout(layout) ? 2 : 1;
        lumaStride_ = size_t(width) * sample;
        chromaStride_ = fullChromaLayout(layout) ? size_t(width) * 2 * sample : size_t((width + 1) / 2) * 2 * sample;
        chromaRows_ = fullChromaLayout(layout) ? height : (height + 1) / 2;
        bytes_.resize(lumaStride_ * height + chromaStride_ * chromaRows_);
    }
    YuvImage view() const noexcept {
        return {layout_, width_, height_, bytes_.data(), lumaStride_, bytes_.data() + lumaStride_ * height_,
                chromaStride_};
    }
    uint8_t* luma() noexcept { return bytes_.data(); }
    uint8_t* chroma() noexcept { return bytes_.data() + lumaStride_ * height_; }
    size_t lumaStride() const noexcept { return lumaStride_; }
    size_t chromaStride() const noexcept { return chromaStride_; }
    uint32_t chromaRows() const noexcept { return chromaRows_; }

private:
    std::vector<uint8_t> bytes_;
    rcs_pixel_format layout_ = RCS_PIXEL_NV12;
    uint32_t width_ = 0, height_ = 0, chromaRows_ = 0;
    size_t lumaStride_ = 0, chromaStride_ = 0;
};

namespace detail {
inline uint16_t load16(const uint8_t* at) noexcept {
    uint16_t value;
    std::memcpy(&value, at, 2);
    return value;
}
inline void store16(uint8_t* at, uint16_t value) noexcept {
    std::memcpy(at, &value, 2);
}
} // namespace detail

// A library's decoded planes (`planes.bits` 8 or 10) as `layout`.
inline void fromPlanar(const Planes& planes, rcs_pixel_format layout, uint32_t width, uint32_t height,
                       YuvBuffer& out) {
    out.resize(layout, width, height);
    const bool wide = wideLayout(layout);
    const unsigned shift = wide ? 16 - planes.bits : 0;
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* from = planes.plane[0] + planes.stride[0] * row;
        uint8_t* to = out.luma() + out.lumaStride() * row;
        if (!wide) {
            std::memcpy(to, from, width);
        } else {
            for (uint32_t x = 0; x < width; ++x) {
                detail::store16(to + x * 2, uint16_t(detail::load16(from + x * 2) << shift));
            }
        }
    }
    const uint32_t chromaWidth = fullChromaLayout(layout) ? width : (width + 1) / 2;
    for (uint32_t row = 0; row < out.chromaRows(); ++row) {
        const uint8_t* u = planes.plane[1] + planes.stride[1] * row;
        const uint8_t* v = planes.plane[2] + planes.stride[2] * row;
        uint8_t* to = out.chroma() + out.chromaStride() * row;
        if (!wide) {
            for (uint32_t x = 0; x < chromaWidth; ++x) {
                to[x * 2] = u[x];
                to[x * 2 + 1] = v[x];
            }
        } else {
            for (uint32_t x = 0; x < chromaWidth; ++x) {
                detail::store16(to + x * 4, uint16_t(detail::load16(u + x * 2) << shift));
                detail::store16(to + x * 4 + 2, uint16_t(detail::load16(v + x * 2) << shift));
            }
        }
    }
}

// Planes a library encodes, owned: from a bi-planar image (16-bit samples
// shifted to the low `bits`).
class PlanarBuffer {
public:
    Planes from(const YuvImage& image, unsigned bits) {
        const bool wide = wideLayout(image.layout), full = fullChromaLayout(image.layout);
        const uint32_t cw = full ? image.width : (image.width + 1) / 2, ch = full ? image.height : (image.height + 1) / 2;
        const size_t sample = wide ? 2 : 1;
        const size_t lumaSize = size_t(image.width) * image.height * sample, chromaSize = size_t(cw) * ch * sample;
        bytes_.resize(lumaSize + 2 * chromaSize);
        uint8_t* y = bytes_.data();
        uint8_t* u = y + lumaSize;
        uint8_t* v = u + chromaSize;
        const unsigned shift = wide ? 16 - bits : 0;
        for (uint32_t row = 0; row < image.height; ++row) {
            const uint8_t* from = image.luma + image.lumaStride * row;
            uint8_t* to = y + size_t(image.width) * sample * row;
            if (!wide) {
                std::memcpy(to, from, image.width);
            } else {
                for (uint32_t x = 0; x < image.width; ++x) {
                    detail::store16(to + x * 2, uint16_t(detail::load16(from + x * 2) >> shift));
                }
            }
        }
        for (uint32_t row = 0; row < ch; ++row) {
            const uint8_t* from = image.chroma + image.chromaStride * row;
            uint8_t* toU = u + size_t(cw) * sample * row;
            uint8_t* toV = v + size_t(cw) * sample * row;
            if (!wide) {
                for (uint32_t x = 0; x < cw; ++x) {
                    toU[x] = from[x * 2];
                    toV[x] = from[x * 2 + 1];
                }
            } else {
                for (uint32_t x = 0; x < cw; ++x) {
                    detail::store16(toU + x * 2, uint16_t(detail::load16(from + x * 4) >> shift));
                    detail::store16(toV + x * 2, uint16_t(detail::load16(from + x * 4 + 2) >> shift));
                }
            }
        }
        Planes p;
        p.plane[0] = y;
        p.plane[1] = u;
        p.plane[2] = v;
        p.stride[0] = size_t(image.width) * sample;
        p.stride[1] = p.stride[2] = size_t(cw) * sample;
        p.bits = wide ? bits : 8;
        return p;
    }

private:
    std::vector<uint8_t> bytes_;
};

// The host's BGRA8 picture (capture source HOST: SDR) as `picture`'s layout,
// written with its matrix and range; chroma of 4:2:0 averaged over 2x2.
inline void fromBgra(const uint8_t* pixels, size_t stride, uint32_t width, uint32_t height, const Picture& picture,
                     YuvBuffer& out) {
    const rcs_pixel_format layout = yuvLayoutOf(picture.format);
    out.resize(layout, width, height);
    const double kr = picture.matrix == RCS_MATRIX_BT601 ? 0.299 : picture.matrix == RCS_MATRIX_BT2020 ? 0.2627 : 0.2126;
    const double kb = picture.matrix == RCS_MATRIX_BT601 ? 0.114 : picture.matrix == RCS_MATRIX_BT2020 ? 0.0593 : 0.0722;
    const double kg = 1 - kr - kb;
    const bool wide = wideLayout(layout), full = fullChromaLayout(layout);
    const double scale = wide ? 4 : 1; // 10-bit codes are the 8-bit ones times four.
    const bool fullRange = picture.range == RCS_RANGE_FULL;
    const double yScale = (fullRange ? 255 : 219) * scale, yOffset = (fullRange ? 0 : 16) * scale;
    const double cScale = (fullRange ? 255 : 224) * scale, cOffset = 128 * scale;
    const double maxCode = wide ? 1023 : 255;
    const unsigned shift = wide ? 6 : 0;
    const auto put = [&](uint8_t* at, double value) {
        const auto code = uint16_t(std::clamp(std::lround(value), 0l, long(maxCode)));
        if (wide) {
            detail::store16(at, uint16_t(code << shift));
        } else {
            *at = uint8_t(code);
        }
    };
    const size_t sample = wide ? 2 : 1;
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* from = pixels + stride * row;
        uint8_t* to = out.luma() + out.lumaStride() * row;
        for (uint32_t x = 0; x < width; ++x) {
            const double b = from[x * 4] / 255.0, g = from[x * 4 + 1] / 255.0, r = from[x * 4 + 2] / 255.0;
            put(to + x * sample, yOffset + yScale * (kr * r + kg * g + kb * b));
        }
    }
    const uint32_t step = full ? 1 : 2;
    for (uint32_t row = 0; row < out.chromaRows(); ++row) {
        uint8_t* to = out.chroma() + out.chromaStride() * row;
        for (uint32_t x = 0; x * step < width; ++x) {
            double r = 0, g = 0, b = 0;
            unsigned count = 0;
            for (uint32_t dy = 0; dy < step && row * step + dy < height; ++dy) {
                const uint8_t* from = pixels + stride * (row * step + dy);
                for (uint32_t dx = 0; dx < step && x * step + dx < width; ++dx) {
                    const uint8_t* p = from + (x * step + dx) * 4;
                    b += p[0];
                    g += p[1];
                    r += p[2];
                    ++count;
                }
            }
            r /= 255.0 * count;
            g /= 255.0 * count;
            b /= 255.0 * count;
            const double y = kr * r + kg * g + kb * b;
            put(to + x * 2 * sample, cOffset + cScale * (b - y) / (2 * (1 - kb)));
            put(to + x * 2 * sample + sample, cOffset + cScale * (r - y) / (2 * (1 - kr)));
        }
    }
}

// A decoded picture as the host reads it on the CPU: BGRA8 (VideoFrame::pixels).
inline void toBgra(const YuvImage& image, const Picture& picture, VideoFrame& frame) {
    auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(image.width) * image.height * 4);
    const YuvLayout layout{fullChromaLayout(image.layout), wideLayout(image.layout) ? 10u : 8u};
    yuvToBgra(image.luma, image.lumaStride, image.chroma, image.chromaStride, image.width, image.height, layout,
              yuvCoefficients(picture.matrix, picture.range, layout.bits), pixels->data(), size_t(image.width) * 4);
    frame.width = image.width;
    frame.height = image.height;
    frame.stride = image.width * 4;
    frame.pixels = std::move(pixels);
    frame.color = rgbColor(colorOf(picture));
}

} // namespace rcsdk
