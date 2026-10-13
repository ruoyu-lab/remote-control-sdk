// Where text typed now would go on this machine. Each platform has a reader
// (reader_<os>); a test puts readers of its own in their place (setFactory).
#pragma once

#include <rcsdk/text_caret.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace rcsdk::text_caret {

inline bool operator==(const rcs_text_caret& a, const rcs_text_caret& b) noexcept {
    return a.visible == b.visible && a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// Whether a caret's fields are what a report may hold.
inline bool valid(const rcs_text_caret& c) noexcept {
    if (!c.visible) {
        return !c.x && !c.y && !c.width && !c.height;
    }
    return c.visible == 1 && c.x <= 65535 && c.y <= 65535 && c.width <= 65535 - c.x && c.height <= 65535 - c.y;
}

// A rectangle on the screen, in the system's coordinates (rcs_module_shared):
// left, top, right, bottom, the right and bottom ones just outside.
struct ScreenRect {
    double left = 0, top = 0, right = 0, bottom = 0;
};

// A caret on the screen as input positions across what is shared (`area`),
// the way input positions map across it (0 its first pixel, 65535 its last):
// clipped to it, none when nothing of it is inside.
inline rcs_text_caret caretWithin(const ScreenRect& caret, const ScreenRect& area) noexcept {
    const double left = std::max(caret.left, area.left), right = std::min(caret.right, area.right);
    const double top = std::max(caret.top, area.top), bottom = std::min(caret.bottom, area.bottom);
    if (right <= left || bottom <= top) {
        return {};
    }
    const auto position = [](double at, double first, double last) {
        return uint32_t(std::clamp(std::lround((at - first) * 65535 / std::max(last - first - 1, 1.0)), 0L, 65535L));
    };
    rcs_text_caret result{1, position(left, area.left, area.right), position(top, area.top, area.bottom), 0, 0};
    result.width = position(right, area.left, area.right) - result.x;
    result.height = position(bottom, area.top, area.bottom) - result.y;
    return result;
}

inline ScreenRect areaOf(const rcs_module_shared& shared) noexcept {
    return {shared.left, shared.top, shared.right, shared.bottom};
}

// Looks for the caret on the thread that made it (Windows: UI Automation
// lives there), which uses and destroys it.
class Reader {
public:
    virtual ~Reader() = default;
    // Where typing goes now, across what `shared` says is shared; none where
    // it goes to another application than a shared window's or application's.
    virtual rcs_text_caret read(const rcs_module_shared& shared) = 0;
};

// The platform's; its warnings go to `module`'s log (may be NULL). Null where
// the system cannot tell.
std::unique_ptr<Reader> createPlatformReader(rcs_module module);
bool platformAvailable() noexcept;

// Readers are `factory`'s instead of the system's, and this machine counts as
// able to look; empty restores.
using Factory = std::function<std::unique_ptr<Reader>()>;
void setFactory(Factory factory);
std::unique_ptr<Reader> createReader(rcs_module module);
// rcs_text_caret_available.
bool available() noexcept;

} // namespace rcsdk::text_caret
