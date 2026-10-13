#include "media/cursor.hpp"

#include "util/util.hpp"

#include <algorithm>
#include <cstring>

namespace rcsdk {
namespace {

constexpr size_t maxRun = 128;

bool samePixel(const uint8_t* a, const uint8_t* b) noexcept {
    return std::memcmp(a, b, 4) == 0;
}

} // namespace

CursorSize fittedCursorSize(uint32_t width, uint32_t height) noexcept {
    const uint32_t longest = std::max(width, height);
    if (longest <= maxCursorSize) {
        return {width, height};
    }
    return {std::max(1u, uint32_t(uint64_t(width) * maxCursorSize / longest)),
            std::max(1u, uint32_t(uint64_t(height) * maxCursorSize / longest))};
}

void fitCursor(CursorImage& image) {
    const auto [width, height] = fittedCursorSize(image.width, image.height);
    if (width == image.width && height == image.height) {
        return;
    }
    // Each pixel averages the source pixels it covers, colour weighted by alpha.
    const auto span = [](uint32_t i, uint32_t to, uint32_t from) {
        const auto first = uint32_t(uint64_t(i) * from / to);
        return std::pair{first, std::max(first + 1, uint32_t(uint64_t(i + 1) * from / to))};
    };
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const auto [top, bottom] = span(y, height, image.height);
        for (uint32_t x = 0; x < width; ++x) {
            const auto [left, right] = span(x, width, image.width);
            uint64_t colour[3] = {}, alpha = 0;
            for (uint32_t sy = top; sy < bottom; ++sy) {
                for (uint32_t sx = left; sx < right; ++sx) {
                    const uint8_t* p = image.pixels.data() + (size_t(sy) * image.width + sx) * 4;
                    for (int c = 0; c < 3; ++c) {
                        colour[c] += uint64_t(p[c]) * p[3];
                    }
                    alpha += p[3];
                }
            }
            const uint64_t count = uint64_t(bottom - top) * (right - left);
            uint8_t* q = pixels.data() + (size_t(y) * width + x) * 4;
            for (int c = 0; c < 3; ++c) {
                q[c] = alpha ? uint8_t((colour[c] + alpha / 2) / alpha) : 0;
            }
            q[3] = uint8_t((alpha + count / 2) / count);
        }
    }
    image.hotspotX = std::min(uint32_t(uint64_t(image.hotspotX) * width / image.width), width - 1);
    image.hotspotY = std::min(uint32_t(uint64_t(image.hotspotY) * height / image.height), height - 1);
    image.width = width;
    image.height = height;
    image.pixels = std::move(pixels);
}

std::vector<uint8_t> packPixels(std::span<const uint8_t> pixels) {
    std::vector<uint8_t> packed;
    const size_t count = pixels.size() / 4;
    const uint8_t* p = pixels.data();
    size_t i = 0;
    while (i < count) {
        size_t run = 1;
        while (i + run < count && run < maxRun && samePixel(p + (i + run) * 4, p + i * 4)) {
            ++run;
        }
        if (run > 1) {
            packed.push_back(uint8_t(0x80 | (run - 1)));
            packed.insert(packed.end(), p + i * 4, p + i * 4 + 4);
            i += run;
            continue;
        }
        // Literals until the next pair of equal pixels.
        size_t literal = 1;
        while (i + literal < count && literal < maxRun &&
               !(i + literal + 1 < count && samePixel(p + (i + literal) * 4, p + (i + literal + 1) * 4))) {
            ++literal;
        }
        packed.push_back(uint8_t(literal - 1));
        packed.insert(packed.end(), p + i * 4, p + (i + literal) * 4);
        i += literal;
    }
    return packed;
}

std::vector<uint8_t> unpackPixels(std::span<const uint8_t> packed, size_t count) {
    std::vector<uint8_t> pixels;
    pixels.reserve(count * 4);
    size_t offset = 0;
    while (offset < packed.size()) {
        const uint8_t header = packed[offset++];
        const size_t run = size_t(header & 0x7f) + 1;
        const size_t bytes = header & 0x80 ? 4 : run * 4;
        if (packed.size() - offset < bytes || pixels.size() / 4 + run > count) {
            fail(RCS_ERROR_PROTOCOL, "Invalid message: cursor image");
        }
        if (header & 0x80) {
            for (size_t k = 0; k < run; ++k) {
                pixels.insert(pixels.end(), packed.begin() + ptrdiff_t(offset), packed.begin() + ptrdiff_t(offset + 4));
            }
        } else {
            pixels.insert(pixels.end(), packed.begin() + ptrdiff_t(offset), packed.begin() + ptrdiff_t(offset + bytes));
        }
        offset += bytes;
    }
    if (pixels.size() != count * 4) {
        fail(RCS_ERROR_PROTOCOL, "Invalid message: cursor image");
    }
    return pixels;
}

} // namespace rcsdk
