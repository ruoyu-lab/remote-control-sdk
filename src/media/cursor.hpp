// The sharer's mouse pointer, sent apart from the picture (RCS_CURSOR_SEPARATE):
// its image and where it is. Platform capture produces these; the protocol
// carries them with the image run-length packed.
#pragma once

#include "util/api.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace rcsdk {

// Pixels per side. A larger pointer is scaled down to fit, keeping its shape
// and its hotspot's place (both platforms).
constexpr uint32_t maxCursorSize = 256;

struct CursorImage {
    rcs_cursor_type type = RCS_CURSOR_CUSTOM;
    uint32_t width = 0, height = 0;
    uint32_t hotspotX = 0, hotspotY = 0;
    std::vector<uint8_t> pixels; // BGRA8, straight alpha, width * 4 bytes per row.
    bool operator==(const CursorImage&) const = default;
};

struct CursorState {
    bool visible = false;
    uint32_t x = 0, y = 0; // Hotspot, 0..65535 across the shared display.
    std::shared_ptr<const CursorImage> image;
    uint64_t imageSerial = 0; // Moves on when the image changes.
};

// The size a pointer of width x height pixels is sent at: its own up to
// maxCursorSize per side, scaled down to fit above that.
struct CursorSize {
    uint32_t width = 0, height = 0;
};
CursorSize fittedCursorSize(uint32_t width, uint32_t height) noexcept;
// Scales `image` down to fittedCursorSize(), averaging its pixels, and its
// hotspot with it.
void fitCursor(CursorImage& image);

// Run-length packing of 32-bit pixels: a header byte, then either one pixel
// repeated (header 0x80 | (count - 1)) or count literal pixels (count - 1),
// up to 128 per header. Cursors are mostly transparent or flat.
std::vector<uint8_t> packPixels(std::span<const uint8_t> pixels);
// Throws Failure(RCS_ERROR_NETWORK) unless `packed` is exactly `count` pixels.
std::vector<uint8_t> unpackPixels(std::span<const uint8_t> packed, size_t count);

} // namespace rcsdk
