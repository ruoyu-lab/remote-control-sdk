// What desktop duplication hands out for a turned display: its panel's image,
// unrotated, the desktop turned within it. Written from Microsoft's
// documentation ("Desktop Duplication API": a 768x1024 desktop at 90 degrees
// comes as a 1024x768 surface) and its DXGIDesktopDuplication sample, which
// maps the texture back to the desktop (DisplayManager.cpp, SetDirtyVert),
// independently of the SDK's shader, so that a test can hold the two against
// each other. A display turned `turns` quarter turns clockwise
// (DXGI_MODE_ROTATION_ROTATE90: 1) shows its W x H desktop's point (x, y)
// at the panel's (y, W - x), (W - x, H - y) or (H - y, x).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace rcsdk::test {

// The panel's size for a W x H desktop.
inline std::pair<uint32_t, uint32_t> panelSize(uint32_t turns, uint32_t width, uint32_t height) {
    return turns % 2 ? std::pair{height, width} : std::pair{width, height};
}

// Where the desktop's point (x, y), continuous, lands in the panel's image.
inline std::pair<double, double> panelPoint(uint32_t turns, double x, double y, uint32_t width, uint32_t height) {
    switch (turns % 4) {
    case 1:
        return {y, width - x};
    case 2:
        return {width - x, height - y};
    case 3:
        return {height - y, x};
    default:
        return {x, y};
    }
}

// The panel's image of a desktop image (row by row, one value per pixel):
// each desktop pixel at the texel its centre lands in.
inline std::vector<uint32_t> panelImage(uint32_t turns, const std::vector<uint32_t>& desktop, uint32_t width,
                                        uint32_t height) {
    const auto [panelWidth, panelHeight] = panelSize(turns, width, height);
    std::vector<uint32_t> panel(size_t(panelWidth) * panelHeight);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto [px, py] = panelPoint(turns, x + 0.5, y + 0.5, width, height);
            panel[size_t(py) * panelWidth + size_t(px)] = desktop[size_t(y) * width + x];
        }
    }
    return panel;
}

} // namespace rcsdk::test
