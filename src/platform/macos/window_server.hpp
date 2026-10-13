// Windows and displays as the window server sees them, for window and
// application capture, the input that stays inside what is shared, and
// rcs_list_windows. Positions are global points from the top left of the main
// display, as CoreGraphics gives them.
#pragma once

#include "platform/platform.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <functional>
#include <optional>
#include <unistd.h>

namespace rcsdk::platform::mac {

// Pixels per point of a display (2 on Retina displays).
double pixelsPerPoint(CGDirectDisplayID display);

// A display as the desktop shows it: its size in pixels (CGDisplayBounds, in
// the desktop's orientation, at the mode's pixels per point: a display turned
// to portrait is as tall as it looks, whichever way its mode counts), its
// refresh rate (0: the system does not say) and how far it is turned,
// clockwise (CGDisplayRotation). All 0 while it has no mode.
struct DisplayGeometry {
    uint32_t width = 0, height = 0, refreshHz = 0, rotation = 0;
};
DisplayGeometry displayGeometry(CGDirectDisplayID display);

// The display that shows most of `bounds`; 0 when none does.
CGDirectDisplayID displayShowing(CGRect bounds);

struct WindowState {
    CGRect bounds{};
    bool onScreen = false; // Off screen: minimized, hidden, ordered out or on another Space.
    pid_t process = 0;
};
// The window as it is now; none once it no longer exists.
std::optional<WindowState> windowState(CGWindowID window);

bool processRuns(pid_t process);

// Whether one of `process`'s windows shows on `display` now.
bool showsOn(pid_t process, CGDirectDisplayID display);

struct WindowAt {
    CGWindowID window = 0;
    pid_t process = 0;
    CGRect bounds{};
    int layer = 0; // 0: an ordinary window; above: menus, panels, the Dock, the menu bar.
};
// The window a press at `point` goes to, as far as the window list tells: the
// frontmost ordinary window there, or a window of `owner` above it (its menus,
// panels). Other processes' windows above the ordinary ones are left out: the
// Dock, the menu bar and notification panels keep large windows over whole
// displays that let presses through, and the list does not say which parts
// take them...
std::optional<WindowAt> windowAt(CGPoint point, pid_t owner);
// ...and the frontmost of `process`'s ordinary windows there.
std::optional<WindowAt> windowOfAt(pid_t process, CGPoint point);
// The frontmost of `process`'s ordinary windows anywhere: the one its keys go
// to while it has the focus; 0 while it shows none.
CGWindowID frontWindow(pid_t process);

// Brings `window` and its application to the front (Accessibility, which
// injecting input needs too). True once the window in front at `point` is one
// `reached` takes; waits for that a little while.
bool bringToFront(const WindowAt& window, CGPoint point,
                  const std::function<bool(const std::optional<WindowAt>&)>& reached);

// The process of the application with the keyboard focus; 0 when the system
// does not say (no Accessibility permission).
pid_t focusedProcess();

// Where what is shared shows now, in global points, as remote input maps
// across it (and modules: platform::sharedArea): `display` (the target's,
// resolved), the region of it, or the window; none while that does not show
// or the display is gone.
std::optional<CGRect> sharedArea(const CaptureTarget& target, CGDirectDisplayID display);

// The window-server session this process belongs to: whether someone has
// logged in to it (the login window's has nobody), whether it has the console
// (fast user switching gives it to another) and whether its screen is locked.
// None outside any (a LaunchDaemon).
struct WindowServerSession {
    bool loginDone = false, onConsole = false, locked = false;
};
std::optional<WindowServerSession> windowServerSession();

// Whether the screen is locked now.
bool screenLocked();

} // namespace rcsdk::platform::mac
