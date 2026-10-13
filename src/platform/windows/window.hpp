// Top-level windows as window capture, application capture, the input that
// stays inside what is shared, and rcs_list_windows see them. Places and
// sizes are physical pixels on the virtual desktop, whatever the calling
// thread's DPI awareness.
#pragma once

#include "platform/platform.hpp"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace rcsdk::platform::win {

inline HWND windowOf(uint64_t id) noexcept {
    return reinterpret_cast<HWND>(uintptr_t(id));
}

DWORD processOf(HWND window) noexcept;

struct WindowState {
    RECT frame{};       // What capture shows: its visible bounds, without the invisible resize borders.
    bool shown = false; // Visible, not minimized, not cloaked (another virtual desktop, a suspended app).
};
// None once the window is gone, or the handle names a window of another
// process (handles are used again).
std::optional<WindowState> windowState(HWND window, DWORD process);

// The parts of `display` (a rectangle of the virtual desktop) where windows of
// `process` show, nothing of another process in front: rectangles relative to
// the display's top left. Click-through overlays do not count as in front.
std::vector<RECT> visibleParts(DWORD process, const RECT& display);
// Whether a window of `process` shows on `display`, covered or not.
bool showsOn(DWORD process, const RECT& display);

// The top-level window in front at `point`...
HWND windowAt(POINT point);
// ...and the frontmost of `process`'s shown windows there.
HWND windowOfAt(DWORD process, POINT point);
// Brings `window` to the front; true once the window in front at `point` is
// one `reached` takes (it waits for that a little while).
bool bringToFront(HWND window, POINT point, const std::function<bool(HWND)>& reached);

// The process whose window has the keyboard focus; 0 when none does.
DWORD foregroundProcess();

// Where what is shared shows now, as remote input maps across it (and
// modules: platform::sharedArea): the display `displayId` (sharing a region,
// that part of it), or
// (WINDOW) the visible frame of `window` of `process`. INVALID_STATE while the
// window does not show or the region is off the display, UNSUPPORTED when the
// display is gone.
rcs_result sharedArea(const CaptureTarget& target, uint32_t displayId, HWND window, DWORD process, RECT& area);

} // namespace rcsdk::platform::win
