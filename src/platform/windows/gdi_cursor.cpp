#include "platform/windows/gdi_cursor.hpp"

#include "platform/windows/d3d.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace rcsdk::platform::win {
namespace {

// A bitmap's pixels as 32-bit BGRA rows, top down.
std::vector<uint32_t> pixelsOf(HBITMAP bitmap, int width, int height) {
    std::vector<uint32_t> pixels(size_t(width) * height);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    const int rows = GetDIBits(screen, bitmap, 0, UINT(height), pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (rows != height) {
        pixels.clear();
    }
    return pixels;
}

// A cursor's image. Pixels that invert the screen beneath (monochrome AND 1
// XOR 1, masked-color pixels the mask lets through) have no straight
// equivalent: they become opaque black, or the colour they XOR with, as with
// Desktop Duplication's shapes.
std::shared_ptr<CursorImage> imageOf(HCURSOR cursor) {
    ICONINFO icon{};
    if (!GetIconInfo(cursor, &icon)) {
        return nullptr;
    }
    struct Owned {
        ICONINFO& icon;
        ~Owned() {
            if (icon.hbmColor) {
                DeleteObject(icon.hbmColor);
            }
            if (icon.hbmMask) {
                DeleteObject(icon.hbmMask);
            }
        }
    } owned{icon};
    BITMAP mask{};
    if (!GetObjectW(icon.hbmMask, sizeof(mask), &mask) || mask.bmWidth <= 0 || mask.bmHeight <= 0) {
        return nullptr;
    }
    const int width = mask.bmWidth;
    const int height = icon.hbmColor ? mask.bmHeight : mask.bmHeight / 2; // Monochrome: AND above XOR.
    if (height <= 0) {
        return nullptr;
    }
    const auto masks = pixelsOf(icon.hbmMask, width, mask.bmHeight);
    const auto colors = icon.hbmColor ? pixelsOf(icon.hbmColor, width, height) : std::vector<uint32_t>();
    if (masks.empty() || (icon.hbmColor && colors.empty())) {
        return nullptr;
    }
    const bool alpha = std::any_of(colors.begin(), colors.end(), [](uint32_t p) { return p >> 24; });
    auto image = std::make_shared<CursorImage>();
    image->width = uint32_t(width);
    image->height = uint32_t(height);
    image->pixels.resize(size_t(width) * height * 4);
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        uint8_t* p = image->pixels.data() + i * 4;
        const bool andBit = masks[i] & 0xFFFFFF;
        if (!icon.hbmColor) {
            const bool xorBit = masks[i + size_t(width) * height] & 0xFFFFFF;
            p[0] = p[1] = p[2] = !andBit && xorBit ? 255 : 0;
            p[3] = andBit && !xorBit ? 0 : 255;
            continue;
        }
        const uint32_t color = colors[i];
        p[0] = uint8_t(color);
        p[1] = uint8_t(color >> 8);
        p[2] = uint8_t(color >> 16);
        p[3] = alpha ? uint8_t(color >> 24) : !andBit || (color & 0xFFFFFF) ? 255 : 0;
    }
    image->hotspotX = std::min<uint32_t>(icon.xHotspot, image->width - 1);
    image->hotspotY = std::min<uint32_t>(icon.yHotspot, image->height - 1);
    image->type = standardType(cursor);
    fitCursor(*image);
    return image;
}

} // namespace

rcs_cursor_type standardType(HCURSOR cursor) {
    const auto id = [](auto resource) {
        return WORD(ULONG_PTR(resource));
    }; // IDC_* are resource numbers (A or W).
    static const std::pair<WORD, rcs_cursor_type> table[] = {{id(IDC_ARROW), RCS_CURSOR_ARROW},
                                                             {id(IDC_IBEAM), RCS_CURSOR_TEXT},
                                                             {id(IDC_HAND), RCS_CURSOR_HAND},
                                                             {id(IDC_WAIT), RCS_CURSOR_WAIT},
                                                             {id(IDC_APPSTARTING), RCS_CURSOR_PROGRESS},
                                                             {id(IDC_CROSS), RCS_CURSOR_CROSSHAIR},
                                                             {id(IDC_SIZEALL), RCS_CURSOR_MOVE},
                                                             {id(IDC_SIZEWE), RCS_CURSOR_RESIZE_EW},
                                                             {id(IDC_SIZENS), RCS_CURSOR_RESIZE_NS},
                                                             {id(IDC_SIZENWSE), RCS_CURSOR_RESIZE_NWSE},
                                                             {id(IDC_SIZENESW), RCS_CURSOR_RESIZE_NESW},
                                                             {id(IDC_NO), RCS_CURSOR_NOT_ALLOWED},
                                                             {id(IDC_HELP), RCS_CURSOR_HELP}};
    for (const auto& [resource, type] : table) {
        if (cursor && cursor == LoadCursorW(nullptr, MAKEINTRESOURCEW(resource))) {
            return type;
        }
    }
    return RCS_CURSOR_CUSTOM;
}

void CursorReader::poll(const RECT& area) {
    DpiScope dpi; // Physical pixels, as `area`.
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info)) {
        return;
    }
    const POINT& point = info.ptScreenPos; // The hotspot.
    state_.visible = (info.flags & CURSOR_SHOWING) && info.hCursor && PtInRect(&area, point);
    const auto across = [](LONG offset, LONG size) {
        return size > 1 ? uint32_t(std::clamp<int64_t>(int64_t(offset) * 65535 / (size - 1), 0, 65535)) : 0u;
    };
    state_.x = across(point.x - area.left, area.right - area.left);
    state_.y = across(point.y - area.top, area.bottom - area.top);
    if (info.hCursor && info.hCursor != shown_) {
        if (auto image = imageOf(info.hCursor)) {
            shown_ = info.hCursor;
            state_.image = std::move(image);
            ++state_.imageSerial;
        }
    }
}

} // namespace rcsdk::platform::win
