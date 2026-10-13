// Picture sizes: the most a stream may be, what each codec takes, and fitting
// a size into a limit with its aspect ratio kept. A limit holds either way
// round where its sides are equal, so a portrait picture (a display turned to
// portrait) has the room of a landscape one.
#pragma once

#include "media/video.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace rcsdk {

struct PictureSize {
    uint32_t width = 0, height = 0;
    bool operator==(const PictureSize&) const = default;
};

// What a picture may be: at most `width` wide and `height` high, and at most
// `area` blocks of `block` x `block` pixels (its blocks rounded up).
struct PictureLimit {
    uint32_t width = 0, height = 0;
    uint64_t area = 0;
    uint32_t block = 1;
};

// The most any stream may be: 8192 a side and 35 651 584 pixels, HEVC's and
// AV1's level 6 (8192x4352, 4352x8192).
inline constexpr PictureLimit maxPicture{8192, 8192, 35'651'584, 1};

// What a codec takes. H.264: 4096 a side (what hardware encoders and decoders
// take) and 36 864 macroblocks (levels 5.1 and 5.2: 4096x2304, 2304x4096);
// HEVC, VP8, VP9, AV1 and the hosts' own formats: maxPicture (VP8's frame
// header takes 16 383 a side, VP9's 65 536).
inline PictureLimit codecLimit(rcs_codec codec) noexcept {
    return codec == RCS_CODEC_H264 ? PictureLimit{4096, 4096, 36'864, 16} : maxPicture;
}

// `limit`, no wider than `width` and no higher than `height`.
inline PictureLimit narrowed(PictureLimit limit, uint32_t width, uint32_t height) noexcept {
    limit.width = std::min(limit.width, width);
    limit.height = std::min(limit.height, height);
    return limit;
}

// Whether `limit` takes a picture of width x height.
inline bool takes(const PictureLimit& limit, uint32_t width, uint32_t height) noexcept {
    const uint64_t across = (uint64_t(width) + limit.block - 1) / limit.block;
    const uint64_t down = (uint64_t(height) + limit.block - 1) / limit.block;
    return width <= limit.width && height <= limit.height && across * down <= limit.area;
}

// A picture of `width` x `height` fitted into `intoWidth` x `intoHeight`,
// its shape kept, centred, even: where it lies, the rest black.
inline Rect letterbox(uint32_t width, uint32_t height, uint32_t intoWidth, uint32_t intoHeight) noexcept {
    const double scale = std::min(double(intoWidth) / width, double(intoHeight) / height);
    const auto w = std::min(intoWidth, uint32_t(std::lround(width * scale)) & ~1u);
    const auto h = std::min(intoHeight, uint32_t(std::lround(height * scale)) & ~1u);
    return {((intoWidth - w) / 2) & ~1u, ((intoHeight - h) / 2) & ~1u, w, h};
}

// A size a stream may have: even, at least 16x16, within maxPicture.
inline bool validPictureSize(uint32_t width, uint32_t height) noexcept {
    return width >= 16 && height >= 16 && width % 2 == 0 && height % 2 == 0 && takes(maxPicture, width, height);
}

// width x height when `limit` takes it; else the largest even size, at least
// 16x16, of its aspect ratio that it takes: scaled to the side that binds,
// then by the square root of how far the area is over.
inline PictureSize fitPicture(uint32_t width, uint32_t height, const PictureLimit& limit) noexcept {
    if (takes(limit, width, height)) {
        return {width, height};
    }
    const auto even = [](double value) {
        return std::max(16u, uint32_t(value) & ~1u);
    };
    uint64_t w = width, h = height;
    if (w > limit.width || h > limit.height) {
        if (w * limit.height >= h * limit.width) {
            h = h * limit.width / w;
            w = limit.width;
        } else {
            w = w * limit.height / h;
            h = limit.height;
        }
    }
    PictureSize size{even(double(w)), even(double(h))};
    if (!takes(limit, size.width, size.height)) {
        // Rounding up to whole blocks may still leave it over: a little less each time.
        double scale = std::sqrt(double(limit.area) * limit.block * limit.block / (double(w) * double(h)));
        do {
            size = {even(double(w) * scale), even(double(h) * scale)};
            scale *= 0.999;
        } while (!takes(limit, size.width, size.height) && size.width > 16 && size.height > 16);
    }
    return size;
}

// "4096 a side and 36 864 macroblocks (e.g. 4096x2304)", for reasons; the
// area only where it binds.
inline std::string limitText(const PictureLimit& limit) {
    const auto number = [](uint64_t value) { // 4096, 36 864, 35 651 584.
        std::string text = std::to_string(value);
        for (size_t at = text.size(); text.size() > 4 && at > 3; at -= 3) {
            text.insert(at - 3, " ");
        }
        return text;
    };
    std::string text = limit.width == limit.height
                           ? number(limit.width) + " a side"
                           : number(limit.width) + " wide and " + number(limit.height) + " high";
    const uint64_t across = (uint64_t(limit.width) + limit.block - 1) / limit.block;
    const uint64_t down = (uint64_t(limit.height) + limit.block - 1) / limit.block;
    if (across * down > limit.area) {
        const uint64_t height = std::min<uint64_t>(limit.height, limit.area / across * limit.block) & ~uint64_t(1);
        text += " and " + number(limit.area) + (limit.block == 1 ? " pixels" : " macroblocks") + " (e.g. " +
                std::to_string(limit.width) + "x" + std::to_string(height) + ")";
    }
    return text;
}

} // namespace rcsdk
