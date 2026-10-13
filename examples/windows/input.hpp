// Win32 mouse/keyboard capture for a viewer window, sent through the SDK.
#pragma once

#include <rcsdk/rcsdk.hpp>
#include <rcsdk/view_geometry.hpp>

#include "../common/keyboard.hpp"

#include <windows.h>
#include <windowsx.h>

#include <utility>

namespace viewer {

// The key's place (examples/common/keyboard.hpp): the scan code in the key
// message, so the shared machine's layout decides what it types; 0 when it is
// not a key the SDK sends. When controlling a Mac, Ctrl acts as Command so
// Ctrl+C/V/A behave as Windows users expect.
inline uint32_t hidUsage(LPARAM flags, bool remoteIsMac) {
    const uint32_t scan = uint32_t((flags >> 16) & 0xff) | ((flags & (1 << 24)) ? 0xE000u : 0u);
    const uint32_t usage = demo::hidFromWindowsScanCode(scan);
    if (remoteIsMac && (usage == 224 || usage == 228)) {
        return usage + 3; // Left / right Control to left / right Command.
    }
    return usage;
}

// Wheel turns as the SDK's smooth scrolling takes them (RCS_INPUT_SCROLL,
// 1/120 lines): WHEEL_DELTA a notch, times the lines (or characters, sideways)
// a notch scrolls here, so the small steps of precision touchpads go as they are.
inline int scrollUnits(int delta, UINT setting) {
    UINT perNotch = 3;
    SystemParametersInfoW(setting, 0, &perNotch, 0);
    if (perNotch == WHEEL_PAGESCROLL) { // "One screen": the SDK scrolls lines, so a screenful of them.
        perNotch = 20;
    }
    return delta * int(perNotch);
}

// Forwards input while enabled. F8 toggles control; losing focus or mouse
// capture releases everything held on the remote side.
class InputForwarder {
public:
    bool enabled() const noexcept { return enabled_; }

    void disable(rcsdk::Session& session) {
        releaseAll(session);
        enabled_ = false;
    }

    // Returns true when the message was consumed.
    bool handle(HWND window, UINT message, WPARAM w, LPARAM l, rcsdk::Session& session, const rcsdk::ViewGeometry& view,
                uint32_t frameWidth, uint32_t frameHeight) {
        if (message == WM_KILLFOCUS || message == WM_CANCELMODE || (message == WM_ACTIVATEAPP && !w) ||
            (message == WM_CAPTURECHANGED && !releasingCapture_)) {
            releaseAll(session);
            return false;
        }
        if ((message == WM_KEYDOWN || message == WM_KEYUP) && w == VK_F8) {
            if (message == WM_KEYDOWN && !(l & (1 << 30))) {
                if (enabled_) {
                    disable(session);
                } else {
                    enabled_ = session.controlInfo().granted;
                }
            }
            return true;
        }
        const auto control = session.controlInfo();
        if (!enabled_ || !control.granted) {
            return false;
        }
        rcs_input_event event = RCS_INPUT_EVENT_INIT;
        switch (message) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            event.type = RCS_INPUT_KEY;
            event.code = hidUsage(l, session.peerInfo().platform == RCS_PLATFORM_MACOS);
            event.down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            event.repeat = event.down && (l & (1 << 30));
            if (!event.code) {
                return true;
            }
            break;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (!pointer(window, message, w, l, session, view, frameWidth, frameHeight, event)) {
                return message != WM_MOUSEMOVE;
            }
            break;
        default:
            return false;
        }
        if (session.sendInput(event) != RCS_OK) {
            disable(session);
        }
        return true;
    }

private:
    bool pointer(HWND window, UINT message, WPARAM w, LPARAM l, rcsdk::Session& session,
                 const rcsdk::ViewGeometry& view, uint32_t width, uint32_t height, rcs_input_event& event) {
        if (!width || !height) {
            return false;
        }
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
            ScreenToClient(window, &point); // Wheel messages carry screen coordinates.
        }
        const bool dragging = buttons_ != 0;
        if (!view.position(point.x, point.y, width, height, event.x, event.y, dragging) ||
            !rcsdk::displayPosition(event.x, event.y, session.streamInfo(), dragging)) {
            return false;
        }
        switch (message) {
        case WM_MOUSEMOVE:
            event.type = RCS_INPUT_MOVE;
            return true;
        case WM_MOUSEWHEEL:
            event.type = RCS_INPUT_SCROLL;
            event.wheel_y = scrollUnits(GET_WHEEL_DELTA_WPARAM(w), SPI_GETWHEELSCROLLLINES);
            return event.wheel_y != 0;
        case WM_MOUSEHWHEEL:
            event.type = RCS_INPUT_SCROLL;
            event.wheel_x = scrollUnits(GET_WHEEL_DELTA_WPARAM(w), SPI_GETWHEELSCROLLCHARS);
            return event.wheel_x != 0;
        }
        event.type = RCS_INPUT_BUTTON;
        event.code = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)   ? RCS_BUTTON_LEFT
                     : (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) ? RCS_BUTTON_RIGHT
                     : (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) ? RCS_BUTTON_MIDDLE
                     : GET_XBUTTON_WPARAM(w) == XBUTTON1                      ? RCS_BUTTON_BACK
                                                                              : RCS_BUTTON_FORWARD;
        event.down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
                     message == WM_XBUTTONDOWN;
        if (event.down) {
            SetFocus(window);
            SetCapture(window); // Keep receiving the drag outside the window.
            buttons_ |= 1u << event.code;
        } else {
            buttons_ &= ~(1u << event.code);
            if (!buttons_) {
                releasingCapture_ = true;
                ReleaseCapture();
                releasingCapture_ = false;
            }
        }
        return true;
    }

    void releaseAll(rcsdk::Session& session) {
        rcs_input_event event = RCS_INPUT_EVENT_INIT;
        event.type = RCS_INPUT_RELEASE_ALL;
        session.sendInput(event);
        buttons_ = 0;
    }

    bool enabled_ = false;
    bool releasingCapture_ = false;
    unsigned buttons_ = 0;
};

} // namespace viewer
