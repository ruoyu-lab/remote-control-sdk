// Remote Control SDK - header-only helpers for a viewer's window: where the
// picture goes in it (fitted with letterboxing, or 1:1 with panning), and the
// conversions between window pixels, the encoded frame and the shared display
// that remote input (rcs_input_event) and the separate pointer
// (RCS_EVENT_CURSOR) use. Optional: nothing in the SDK depends on them.
#pragma once

#include <rcsdk/rcsdk.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rcsdk {

// How the picture fills the window: scaled to fit, or one image pixel per
// window pixel (cropped, panned).
enum class ViewScale { fit, nativePixels };

// Where the picture sits in the window. Coordinates are physical client
// pixels. Native mode crops at the requested source offset; painting and
// remote input must use the same geometry.
struct ViewGeometry {
    int x = 0, y = 0, width = 0, height = 0, sourceX = 0, sourceY = 0, sourceWidth = 0, sourceHeight = 0;
    bool position(double px, double py, uint32_t imageWidth, uint32_t imageHeight, uint32_t& normalizedX,
                  uint32_t& normalizedY, bool clamp = false) const {
        if (width <= 0 || height <= 0 || !imageWidth || !imageHeight) {
            return false;
        }
        if (!clamp && (px < x || py < y || px >= x + width || py >= y + height)) {
            return false;
        }
        const double sx =
            sourceX + std::clamp((px - x) / std::max(1, width - 1), 0.0, 1.0) * std::max(0, sourceWidth - 1);
        const double sy =
            sourceY + std::clamp((py - y) / std::max(1, height - 1), 0.0, 1.0) * std::max(0, sourceHeight - 1);
        normalizedX = static_cast<uint32_t>(std::lround(sx / std::max(1u, imageWidth - 1) * 65535));
        normalizedY = static_cast<uint32_t>(std::lround(sy / std::max(1u, imageHeight - 1) * 65535));
        return true;
    }
    // The inverse of position(): normalized image coordinates to client
    // pixels; false where native mode crops them away.
    bool point(uint32_t normalizedX, uint32_t normalizedY, uint32_t imageWidth, uint32_t imageHeight, double& px,
               double& py) const {
        if (width <= 0 || height <= 0 || !imageWidth || !imageHeight) {
            return false;
        }
        const double sx = double(normalizedX) / 65535 * std::max(1u, imageWidth - 1) - sourceX;
        const double sy = double(normalizedY) / 65535 * std::max(1u, imageHeight - 1) - sourceY;
        if (sx < 0 || sy < 0 || sx > sourceWidth - 1 || sy > sourceHeight - 1) {
            return false;
        }
        px = x + sx / std::max(1, sourceWidth - 1) * std::max(0, width - 1);
        py = y + sy / std::max(1, sourceHeight - 1) * std::max(0, height - 1);
        return true;
    }
};
inline ViewGeometry viewGeometry(int viewportWidth, int viewportHeight, uint32_t imageWidth, uint32_t imageHeight,
                                 ViewScale mode = ViewScale::fit, int panX = 0, int panY = 0) {
    ViewGeometry g;
    if (viewportWidth <= 0 || viewportHeight <= 0 || !imageWidth || !imageHeight) {
        return g;
    }
    g.sourceWidth = static_cast<int>(imageWidth);
    g.sourceHeight = static_cast<int>(imageHeight);
    if (mode == ViewScale::fit) {
        const double scale = std::min(double(viewportWidth) / imageWidth, double(viewportHeight) / imageHeight);
        g.width = std::max(1, int(imageWidth * scale));
        g.height = std::max(1, int(imageHeight * scale));
    } else {
        g.width = g.sourceWidth = std::min(viewportWidth, int(imageWidth));
        g.height = g.sourceHeight = std::min(viewportHeight, int(imageHeight));
        g.sourceX = std::clamp(panX, 0, int(imageWidth) - g.sourceWidth);
        g.sourceY = std::clamp(panY, 0, int(imageHeight) - g.sourceHeight);
    }
    g.x = (viewportWidth - g.width) / 2;
    g.y = (viewportHeight - g.height) / 2;
    return g;
}
// Convert encoded-frame coordinates to normalized shared-display coordinates.
// Drop clicks in encoder padding; clamp an existing drag to the display edge.
inline bool displayPosition(uint32_t& x, uint32_t& y, const rcs_stream_info& i, bool clamp = false) {
    if (!i.content_width || !i.content_height) {
        return true;
    }
    const double px = double(x) * std::max(1u, i.width - 1) / 65535,
                 py = double(y) * std::max(1u, i.height - 1) / 65535;
    if (!clamp && (px + 0.5 < i.content_x || py + 0.5 < i.content_y || px > i.content_x + i.content_width - 0.5 ||
                   py > i.content_y + i.content_height - 0.5)) {
        return false;
    }
    x = uint32_t(std::lround(std::clamp((px - i.content_x) / std::max(1u, i.content_width - 1), 0.0, 1.0) * 65535));
    y = uint32_t(std::lround(std::clamp((py - i.content_y) / std::max(1u, i.content_height - 1), 0.0, 1.0) * 65535));
    return true;
}

// The inverse of displayPosition: normalized shared-display coordinates (a
// cursor's hotspot, RCS_EVENT_CURSOR) to normalized encoded-frame coordinates.
inline void framePosition(uint32_t& x, uint32_t& y, const rcs_stream_info& i) {
    if (!i.content_width || !i.content_height) {
        return;
    }
    const double px = i.content_x + double(x) / 65535 * std::max(1u, i.content_width - 1);
    const double py = i.content_y + double(y) / 65535 * std::max(1u, i.content_height - 1);
    x = uint32_t(std::lround(std::clamp(px / std::max(1u, i.width - 1), 0.0, 1.0) * 65535));
    y = uint32_t(std::lround(std::clamp(py / std::max(1u, i.height - 1), 0.0, 1.0) * 65535));
}

// Client pixels per pixel of the shared display, e.g. to draw a cursor image
// (rcs_cursor_shape is in display pixels) as large as the picture shows it.
inline double displayScale(const ViewGeometry& g, const rcs_stream_info& i) {
    if (!g.sourceWidth || !i.display_width || !i.content_width) {
        return 1;
    }
    return double(g.width) / g.sourceWidth * double(i.content_width) / i.display_width;
}

} // namespace rcsdk
