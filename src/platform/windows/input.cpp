// Remote input execution with SendInput. Keys go in as scan codes (the place
// of the key; the layout decides what it types), see platform/keys.hpp; media
// keys as virtual keys; text as Unicode characters (VK_PACKET). Touch and pen
// go through synthetic pointer devices (platform/windows/pointer_input.hpp).
//
// Pointer positions map across what is shared: the display, the region, or
// the window where it is now. Sharing a window or an application, input stays
// inside it (rcs_capture_config): a press or a wheel turn where another
// window is in front first brings the shared one to the front, key presses
// and text go in only while its application has the focus and, sharing a
// window, that window or one it owns is the foreground window; media keys and
// the system's shortcuts (keys::windowsBeyondConfinement) are refused.
#include "platform/windows/d3d.hpp"
#include "platform/windows/desktop.hpp"
#include "platform/windows/display.hpp"
#include "platform/windows/pointer_input.hpp"
#include "platform/windows/window.hpp"

#include "platform/keys.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <tuple>
#include <utility>
#include <vector>

namespace rcsdk::platform {
namespace {

// Lets applications recognise SDK-injected events (GetMessageExtraInfo).
constexpr ULONG_PTR injectedMarker = 0x52435344; // 'RCSD'

// Lines a wheel notch scrolls: the user's setting of lines (of characters
// sideways). Where a notch scrolls a page, or nothing, three (Windows'
// default) stand for a notch.
int64_t linesPerNotch(bool horizontal) {
    UINT perNotch = 3;
    SystemParametersInfoW(horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &perNotch, 0);
    return !perNotch || perNotch == WHEEL_PAGESCROLL ? 3 : perNotch;
}

// Wheel units (WHEEL_DELTA a notch) for `sixtieths` 1/120 lines; what does not
// make a whole unit stays in `rest` for the next turn.
int32_t wheelDelta(int64_t sixtieths, bool horizontal, int64_t& rest) {
    const int64_t total = sixtieths + rest, perNotch = linesPerNotch(horizontal);
    const int64_t delta = total / perNotch; // (total / 120 lines) * WHEEL_DELTA / perNotch.
    rest = total - delta * perNotch;
    return int32_t(std::clamp<int64_t>(delta, INT32_MIN, INT32_MAX));
}

class SendInputInjector final : public InputInjector {
public:
    SendInputInjector(const CaptureTarget& target, const std::vector<uint32_t>& streamDisplays, bool secureDesktop)
        : target_(target), streamDisplays_(streamDisplays), secureDesktop_(secureDesktop) {
        if (target.source == RCS_CAPTURE_WINDOW) {
            window_ = win::windowOf(target.windowId);
            process_ = win::processOf(window_); // 0: gone already, and it takes no input.
            return;
        }
        process_ = target.processId;
        // The display must exist when control starts; 0 stays the display
        // that is main now, as capture keeps it.
        const auto monitor = win::findMonitor(target.displayId);
        if (!monitor) {
            fail(RCS_ERROR_UNSUPPORTED, "The input display is not available");
        }
        displayId_ = monitor->id;
    }
    ~SendInputInjector() override { releaseAll(); }

    rcs_result apply(const rcs_input_event& e) override {
        if (e.type == RCS_INPUT_SECURE_ATTENTION) {
            return RCS_ERROR_INVALID_ARGUMENT; // The session sends it (platform::secureAttention), never an injector.
        }
        win::DpiScope dpi; // Physical pixels for the virtual-desktop mapping.
        win::InputDesktop desktop; // Following secure desktops: this thread works on the one that has input.
        // Not following them: only while the user's own desktop has input,
        // looked at with each event, not only by the availability check (once
        // a second), which pauses control after a switch. Windows refuses
        // SendInput into a desktop the thread is not on as well.
        if (secureDesktop_ ? !desktop.attach() : !win::userDesktopHasInput()) {
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
        switch (e.type) {
        case RCS_INPUT_RELEASE_ALL:
            releaseAll();
            return RCS_OK;
        case RCS_INPUT_KEY:
            return (e.code & 0xFFFF0000u) == RCS_KEY_CONSUMER ? consumerKey(e.code & 0xFFFFu, e.down) : key(e);
        case RCS_INPUT_TEXT:
            return text(e.code);
        case RCS_INPUT_MOVE_RELATIVE:
            return relative(e);
        case RCS_INPUT_TOUCH:
        case RCS_INPUT_PEN:
            return touchOrPen(e);
        default:
            return pointer(e);
        }
    }

    void releaseAll() noexcept override {
        win::DpiScope dpi;         // Touch and pen lift where they are, in physical pixels.
        win::InputDesktop desktop; // Best effort on the desktop that has input now.
        if (secureDesktop_) {
            desktop.attach();
        }
        // Release ordinary keys before modifiers so no shortcut is triggered.
        for (const bool modifiers : {false, true}) {
            for (uint32_t usage = keys::firstUsage; usage < keys_.size(); ++usage) {
                if (keys_[usage] && (usage >= 224) == modifiers) {
                    sendKey(keys::windowsScanCode(usage), false);
                    keys_[usage] = false;
                }
            }
        }
        for (unsigned key = 0; key < consumer_.size(); ++key) {
            if (consumer_[key]) {
                sendVirtualKey(uint8_t(key), false);
                consumer_[key] = false;
            }
        }
        for (unsigned button = 0; button < buttons_.size(); ++button) {
            if (buttons_[button]) {
                INPUT input{};
                input.type = INPUT_MOUSE;
                setButton(input, button, false);
                send(input);
                buttons_[button] = false;
            }
        }
        pointers_.releaseAll();
    }

private:
    // SendInput fails when the input desktop is locked or belongs to a more
    // privileged application (UIPI).
    static bool send(INPUT& input) {
        if (input.type == INPUT_MOUSE) {
            input.mi.dwExtraInfo = injectedMarker;
        } else {
            input.ki.dwExtraInfo = injectedMarker;
        }
        return SendInput(1, &input, sizeof(input)) == 1;
    }

    // The mouse event for `button` (0: left .. 4: forward) going down or up.
    static void setButton(INPUT& input, unsigned button, bool down) {
        constexpr DWORD flags[][2] = {{MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN},
                                      {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
                                      {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN},
                                      {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN},
                                      {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}};
        input.mi.dwFlags |= flags[button][down];
        if (button >= 3) {
            input.mi.mouseData = button == 3 ? XBUTTON1 : XBUTTON2;
        }
    }

    static bool sendVirtualKey(uint8_t key, bool down) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = KEYEVENTF_EXTENDEDKEY | (down ? 0 : KEYEVENTF_KEYUP);
        return send(input);
    }

    static bool sendKey(uint16_t scan, bool down) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = WORD(scan & 0xff);
        input.ki.dwFlags =
            KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP) | ((scan & 0xff00) ? KEYEVENTF_EXTENDEDKEY : 0);
        return send(input);
    }

    rcs_result key(const rcs_input_event& e) {
        const uint16_t scan = keys::windowsScanCode(e.code);
        if (!scan) {
            return RCS_ERROR_UNSUPPORTED;
        }
        if (confined() && e.down && (keys::windowsBeyondConfinement(e.code, held()) || !focused())) {
            return RCS_ERROR_INVALID_STATE; // A system shortcut, or the keys would go to another window.
        }
        if (keys_[e.code] == bool(e.down) && !e.repeat) {
            return RCS_OK;
        }
        if (!sendKey(scan, e.down)) {
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
        keys_[e.code] = e.down;
        return RCS_OK;
    }

    // A media key: it acts on the machine or another application (mail, the
    // browser), not on a window, so none while input is confined to one.
    rcs_result consumerKey(uint32_t usage, bool down) {
        const uint8_t key = keys::windowsConsumerKey(usage);
        if (!key) {
            return RCS_ERROR_UNSUPPORTED;
        }
        if (confined() && down) {
            return RCS_ERROR_INVALID_STATE;
        }
        if (consumer_[key] == down) {
            return RCS_OK;
        }
        if (!sendVirtualKey(key, down)) {
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
        consumer_[key] = down;
        return RCS_OK;
    }

    // One character, as UTF-16 code units: each goes down (the window gets it
    // as WM_CHAR, a surrogate pair as two), then all go up, in one call so
    // nothing comes between.
    rcs_result text(uint32_t character) {
        if (confined() && !focused()) {
            return RCS_ERROR_INVALID_STATE;
        }
        std::vector<WORD> units;
        if (character == '\n') {
            units.push_back(WORD('\r')); // Enter, as Windows types it.
        } else if (character >= 0x10000) {
            units.push_back(WORD(0xD800 + ((character - 0x10000) >> 10)));
            units.push_back(WORD(0xDC00 + ((character - 0x10000) & 0x3FF)));
        } else {
            units.push_back(WORD(character));
        }
        std::vector<INPUT> inputs;
        for (const bool down : {true, false}) {
            for (const WORD unit : units) {
                INPUT input{};
                input.type = INPUT_KEYBOARD;
                input.ki.wScan = unit;
                input.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
                input.ki.dwExtraInfo = injectedMarker;
                inputs.push_back(input);
            }
        }
        return SendInput(UINT(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size()
                   ? RCS_OK
                   : RCS_ERROR_PERMISSION_REQUIRED;
    }

    bool confined() const { return target_.source == RCS_CAPTURE_WINDOW || target_.source == RCS_CAPTURE_APPLICATION; }
    // Input on `stream` stays inside what it shares (only stream 0 shares less than a display).
    bool confined(uint32_t stream) const { return !stream && confined(); }

    // Whether `window` is the shared window or one it owns, at any remove.
    bool sharedOrOwned(HWND window) const {
        for (; window; window = GetWindow(window, GW_OWNER)) {
            if (window == window_) {
                return true;
            }
        }
        return false;
    }

    // Whether keys go to what is shared: its application has the focus and,
    // sharing a window, the foreground window is that one or one it owns (a
    // dialog of it). Menus never take the foreground.
    bool focused() const {
        const HWND front = GetForegroundWindow();
        return front && win::processOf(front) == process_ &&
               (target_.source != RCS_CAPTURE_WINDOW || sharedOrOwned(front));
    }

    keys::Modifiers held() const {
        return {keys_[224] || keys_[228], keys_[225] || keys_[229], keys_[226] || keys_[230], keys_[227] || keys_[231]};
    }

    // What `stream` shares, where it is now. Looked up per event: a window
    // moves, a display mode or layout change moves the display.
    rcs_result area(uint32_t stream, RECT& shared) const {
        if (!stream) {
            return win::sharedArea(target_, displayId_, window_, process_, shared);
        }
        if (stream > streamDisplays_.size()) {
            return RCS_ERROR_INVALID_ARGUMENT;
        }
        const uint32_t display = streamDisplays_[stream - 1];
        return win::sharedArea({.displayId = display}, display, nullptr, 0, shared);
    }

    // Whether a press reaches what is shared: the window in front there is
    // the shared one, one it owns (a dialog, an owned popup) or an unowned
    // popup of its application (a menu) or, sharing an application, one of its
    // windows. A window another of the application's windows owns is none.
    bool reaches(HWND front) const {
        if (!front || win::processOf(front) != process_) {
            return false;
        }
        return target_.source == RCS_CAPTURE_APPLICATION || sharedOrOwned(front) ||
               (!GetWindow(front, GW_OWNER) && (GetWindowLongW(front, GWL_STYLE) & WS_POPUP));
    }

    // Brings what is shared to the front at `point` when another window is
    // there; INVALID_STATE when nothing shared is there or it cannot come.
    rcs_result reach(POINT point) const {
        if (reaches(win::windowAt(point))) {
            return RCS_OK;
        }
        const HWND wanted = target_.source == RCS_CAPTURE_WINDOW ? window_ : win::windowOfAt(process_, point);
        if (!wanted || !win::bringToFront(wanted, point, [this](HWND front) { return reaches(front); })) {
            return RCS_ERROR_INVALID_STATE;
        }
        return RCS_OK;
    }

    // Where an absolute position (0..65535 across what `stream` shares) is on
    // the screen, in physical pixels.
    rcs_result position(uint32_t stream, uint32_t px, uint32_t py, POINT& point) const {
        RECT shared{};
        if (const rcs_result found = area(stream, shared); found != RCS_OK) {
            return found;
        }
        point = {shared.left + pixelAt(px, shared.right - shared.left),
                 shared.top + pixelAt(py, shared.bottom - shared.top)};
        return RCS_OK;
    }

    // An absolute move there, to put in front of a button or wheel event.
    // Windows does not document how it turns a normalized position n into a
    // pixel of w: the smallest n at or past the pixel, x * 65535 / (w - 1)
    // rounded up, lands on it whether Windows takes n * w / 65536 or
    // n * (w - 1) / 65535, rounded down, or the latter rounded.
    static rcs_result moveTo(POINT point, INPUT& input) {
        const LONG vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const LONG vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw <= 1 || vh <= 1) {
            return RCS_ERROR_UNSUPPORTED;
        }
        input = {};
        input.type = INPUT_MOUSE;
        const auto normalized = [](LONG offset, LONG size) {
            return LONG((int64_t(offset) * 65535 + size - 2) / (size - 1));
        };
        input.mi.dx = normalized(point.x - vx, vw);
        input.mi.dy = normalized(point.y - vy, vh);
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        return RCS_OK;
    }

    rcs_result pointer(const rcs_input_event& e) {
        POINT point{};
        if (const rcs_result found = position(e.stream, e.x, e.y, point); found != RCS_OK) {
            return found;
        }
        const bool wheel = e.type == RCS_INPUT_WHEEL || e.type == RCS_INPUT_SCROLL;
        const bool press = wheel || (e.type == RCS_INPUT_BUTTON && e.down);
        if (confined(e.stream) && press && !(e.type == RCS_INPUT_BUTTON && buttons_[e.code - 1])) {
            if (const rcs_result reached = reach(point); reached != RCS_OK) {
                return reached;
            }
        }
        INPUT input{};
        if (const rcs_result moved = moveTo(point, input); moved != RCS_OK) {
            return moved;
        }
        if (e.type == RCS_INPUT_BUTTON) {
            const unsigned button = e.code - 1;
            if (buttons_[button] == bool(e.down)) {
                return RCS_OK;
            }
            setButton(input, button, e.down);
            if (!send(input)) {
                return RCS_ERROR_PERMISSION_REQUIRED;
            }
            buttons_[button] = e.down;
            return RCS_OK;
        }
        if (!send(input)) {
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
        if (!wheel) {
            return RCS_OK;
        }
        const int64_t scale = e.type == RCS_INPUT_WHEEL ? 120 : 1; // To 1/120 lines.
        for (const auto& [amount, flag, rest] : {std::tuple{e.wheel_x, MOUSEEVENTF_HWHEEL, &wheelRest_[0]},
                                                 std::tuple{e.wheel_y, MOUSEEVENTF_WHEEL, &wheelRest_[1]}}) {
            if (!amount) {
                continue;
            }
            const int32_t delta = wheelDelta(amount * scale, flag == MOUSEEVENTF_HWHEEL, *rest);
            if (delta) {
                INPUT turn{};
                turn.type = INPUT_MOUSE;
                turn.mi.dwFlags = flag;
                turn.mi.mouseData = DWORD(delta);
                if (!send(turn)) {
                    return RCS_ERROR_PERMISSION_REQUIRED;
                }
            }
        }
        return RCS_OK;
    }

    // Sharing a display, motion as the mouse reports it (programs that read
    // raw input get it as it is). Sharing a region, a window or an application,
    // the pointer moves by as much but stays inside it.
    rcs_result relative(const rcs_input_event& e) {
        INPUT input{};
        if (target_.source == RCS_CAPTURE_DISPLAY) {
            input.type = INPUT_MOUSE;
            input.mi.dx = e.dx;
            input.mi.dy = e.dy;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            return send(input) ? RCS_OK : RCS_ERROR_PERMISSION_REQUIRED;
        }
        RECT shared{};
        POINT point{};
        if (const rcs_result found = area(0, shared); found != RCS_OK) {
            return found;
        }
        if (!GetCursorPos(&point)) {
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
        point = {std::clamp<LONG>(point.x + e.dx, shared.left, shared.right - 1),
                 std::clamp<LONG>(point.y + e.dy, shared.top, shared.bottom - 1)};
        if (const rcs_result moved = moveTo(point, input); moved != RCS_OK) {
            return moved;
        }
        return send(input) ? RCS_OK : RCS_ERROR_PERMISSION_REQUIRED;
    }

    rcs_result touchOrPen(const rcs_input_event& e) {
        POINT point{};
        if (const rcs_result found = position(e.stream, e.x, e.y, point); found != RCS_OK) {
            return found;
        }
        const bool touch = e.type == RCS_INPUT_TOUCH;
        const bool pressing = touch ? e.down && !pointers_.touching(e.code)
                                    : e.down && !(e.pen & RCS_PEN_LEAVE) && !pointers_.penTouching();
        if (confined(e.stream) && pressing) {
            if (const rcs_result reached = reach(point); reached != RCS_OK) {
                return reached;
            }
        }
        return touch ? pointers_.touch(e, point) : pointers_.pen(e, point);
    }

    const CaptureTarget target_;
    const std::vector<uint32_t> streamDisplays_; // Streams 1, 2, ...: whole displays.
    uint32_t displayId_ = 0;
    HWND window_ = nullptr; // WINDOW: the shared window...
    DWORD process_ = 0;     // ...and (WINDOW, APPLICATION) the process whose windows input stays in.
    bool secureDesktop_ = false;
    std::array<bool, keys::lastUsage + 1> keys_{};
    std::array<bool, 256> consumer_{}; // Media keys held, by virtual key.
    std::array<bool, 5> buttons_{};
    std::array<int64_t, 2> wheelRest_{}; // 1/120 lines not yet turned, across and down.
    win::PointerInjector pointers_;
};

} // namespace

std::unique_ptr<InputInjector> createInputInjector(const CaptureTarget& target,
                                                   const std::vector<uint32_t>& streamDisplays,
                                                   const InputOptions& options) {
    return std::make_unique<SendInputInjector>(target, streamDisplays, options.secureDesktop);
}

} // namespace rcsdk::platform
