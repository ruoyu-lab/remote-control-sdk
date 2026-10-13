// A frame of CPU BGRA pixels made smaller, for a stream's lower encodings
// (RCS_EXT_ENCODINGS): each pixel the mean of the pixels it covers, which
// keeps text legible where sampling would drop strokes. Native surfaces are
// the platform's to scale (platform::SurfaceScaler).
#pragma once

#include "media/video.hpp"

#include <algorithm>
#include <vector>

namespace rcsdk {

inline VideoFrame scaleBgra(const VideoFrame& frame, uint32_t width, uint32_t height) {
    auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * 4);
    const uint8_t* source = frame.pixels->data();
    // Each destination column's span of source columns, at least one.
    std::vector<uint32_t> left(width + 1);
    for (uint32_t x = 0; x <= width; ++x) {
        left[x] = uint32_t(uint64_t(x) * frame.width / width);
    }
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t top = uint32_t(uint64_t(y) * frame.height / height);
        const uint32_t bottom = std::max(top + 1, uint32_t(uint64_t(y + 1) * frame.height / height));
        uint8_t* out = pixels->data() + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t from = left[x], to = std::max(from + 1, left[x + 1]);
            uint32_t sum[4] = {};
            for (uint32_t row = top; row < bottom; ++row) {
                const uint8_t* in = source + size_t(row) * frame.stride + size_t(from) * 4;
                for (uint32_t column = from; column < to; ++column, in += 4) {
                    sum[0] += in[0], sum[1] += in[1], sum[2] += in[2], sum[3] += in[3];
                }
            }
            const uint32_t count = (bottom - top) * (to - from);
            for (int c = 0; c < 4; ++c) {
                out[x * 4 + c] = uint8_t((sum[c] + count / 2) / count);
            }
        }
    }
    VideoFrame scaled = frame;
    scaled.width = width;
    scaled.height = height;
    scaled.stride = width * 4;
    scaled.pixels = std::move(pixels);
    return scaled;
}

} // namespace rcsdk
