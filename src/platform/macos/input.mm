// Remote input execution with CoreGraphics events. Keys go in as virtual key
// codes (the place of the key; the layout decides what it types), see
// platform/keys.hpp. Caps Lock is a lock, not a key: a press toggles the HID
// system's Caps Lock state, which posted key events cannot change. Media keys
// are system-defined auxiliary key events, text goes in as the Unicode string
// of a key event, and the pen as tablet events (proximity, then mouse events
// carrying pressure and tilt). macOS has no touch screens: touch is
// UNSUPPORTED.
//
// Pointer positions map across what is shared: the display, the region, or
// the window where it is now. Sharing a window or an application, input stays
// inside it (rcs_capture_config): a press or a wheel turn where another
// window is in front first brings the shared one to the front, key presses
// and text go in only while its application has the focus and, sharing a
// window, that window is the application's frontmost; media keys and the
// system's shortcuts (keys::macBeyondConfinement) are refused.
#include "platform/keys.hpp"
#include "platform/macos/window_server.hpp"
#include "platform/platform.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hidsystem/IOHIDLib.h>
#import <IOKit/hidsystem/IOHIDParameter.h>
#import <IOKit/hidsystem/IOHIDShared.h>
#import <IOKit/hidsystem/IOLLEvent.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace rcsdk::platform {
namespace {

constexpr uint32_t capsLock = 57;
constexpr CGEventFlags modifierFlags =
    kCGEventFlagMaskControl | kCGEventFlagMaskShift | kCGEventFlagMaskAlternate | kCGEventFlagMaskCommand;

class CoreGraphicsInjector final : public InputInjector {
  public:
    CoreGraphicsInjector(const CaptureTarget& target, const std::vector<uint32_t>& streamDisplays)
        : source_(CGEventSourceCreate(kCGEventSourceStatePrivate)), target_(target), streamDisplays_(streamDisplays),
          display_(target.displayId ? target.displayId : CGMainDisplayID()) {
        if (target.source == RCS_CAPTURE_WINDOW) {
            if (const auto state = mac::windowState(window())) {
                process_ = state->process;
            }
        } else if (target.source == RCS_CAPTURE_APPLICATION) {
            process_ = pid_t(target.processId);
        }
        if (CGEventRef current = CGEventCreate(nullptr)) {
            point_ = CGEventGetLocation(current);
            CFRelease(current);
        }
    }
    ~CoreGraphicsInjector() override {
        releaseAll();
        if (source_) {
            CFRelease(source_);
        }
        if (hid_) {
            IOServiceClose(hid_);
        }
    }

    rcs_result apply(const rcs_input_event& e) override {
        switch (e.type) {
        case RCS_INPUT_RELEASE_ALL:
            releaseAll();
            return RCS_OK;
        case RCS_INPUT_KEY:
            if ((e.code & 0xFFFF0000u) == RCS_KEY_CONSUMER) {
                return auxiliaryKey(e.code & 0xFFFFu, e.down);
            }
            if (const uint16_t usage = keys::consumerOfKeyboard(e.code)) {
                return auxiliaryKey(usage, e.down); // Volume keys have no key code here.
            }
            return key(e);
        case RCS_INPUT_TEXT:
            return text(e.code);
        case RCS_INPUT_MOVE_RELATIVE:
            return relative(e);
        case RCS_INPUT_PEN:
            return pen(e);
        case RCS_INPUT_TOUCH:
            return RCS_ERROR_UNSUPPORTED;
        case RCS_INPUT_SECURE_ATTENTION:
            return RCS_ERROR_INVALID_ARGUMENT; // The session handles it (platform::secureAttention), never an injector.
        default:
            return pointer(e);
        }
    }

    void releaseAll() noexcept override {
        // Release ordinary keys before modifiers so no shortcut is triggered.
        for (const bool modifiers : {false, true}) {
            for (uint32_t usage = keys::firstUsage; usage < keys_.size(); ++usage) {
                if (keys_[usage] && (usage >= 224) == modifiers) {
                    keys_[usage] = false;
                    post(CGEventCreateKeyboardEvent(source_, CGKeyCode(keys::macKeyCode(usage)), false));
                }
            }
        }
        for (unsigned button = 0; button < buttons_.size(); ++button) {
            if (buttons_[button]) {
                buttons_[button] = false;
                post(CGEventCreateMouseEvent(source_, buttonEvent(button, false), point_, CGMouseButton(button)));
            }
        }
        for (int auxiliary = 0; auxiliary < int(auxiliary_.size()); ++auxiliary) {
            if (auxiliary_[auxiliary]) {
                auxiliary_[auxiliary] = false;
                postAuxiliary(auxiliary, false);
            }
        }
        if (penInRange_) { // Lifted and out of range where it is, whether or not what is shared shows now.
            if (penDown_) {
                CGEventRef up =
                    CGEventCreateMouseEvent(source_, buttonEvent(penButton_, false), point_, CGMouseButton(penButton_));
                if (up) {
                    CGEventSetIntegerValueField(up, kCGMouseEventSubtype, kCGEventMouseSubtypeTabletPoint);
                    CGEventSetIntegerValueField(up, kCGTabletEventDeviceID, penDevice);
                }
                post(up);
                penDown_ = false;
            }
            proximity(false, penEraser_);
            penInRange_ = false;
        }
    }

  private:
    static CGEventType buttonEvent(unsigned button, bool down) {
        switch (button) {
        case 0:
            return down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        case 1:
            return down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        default:
            return down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        }
    }

    // The modifiers this injector holds, and Caps Lock as the system has it.
    CGEventFlags modifiers() const {
        CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) & kCGEventFlagMaskAlphaShift;
        flags |= (keys_[224] || keys_[228]) ? kCGEventFlagMaskControl : 0;
        flags |= (keys_[225] || keys_[229]) ? kCGEventFlagMaskShift : 0;
        flags |= (keys_[226] || keys_[230]) ? kCGEventFlagMaskAlternate : 0;
        flags |= (keys_[227] || keys_[231]) ? kCGEventFlagMaskCommand : 0;
        return flags;
    }

    // Keeps what the event says of its key (keypad, function key) and sets
    // the modifiers.
    void post(CGEventRef event) {
        if (event) {
            CGEventSetFlags(event,
                            (CGEventGetFlags(event) & ~(modifierFlags | kCGEventFlagMaskAlphaShift)) | modifiers());
            CGEventPost(kCGHIDEventTap, event);
            CFRelease(event);
        }
    }

    rcs_result key(const rcs_input_event& e) {
        const int code = keys::macKeyCode(e.code);
        if (code < 0) {
            return RCS_ERROR_UNSUPPORTED;
        }
        if (confined() && e.down && (keys::macBeyondConfinement(e.code, held()) || !focused())) {
            return RCS_ERROR_INVALID_STATE; // A system shortcut, or the keys would go to another window.
        }
        if (e.code == capsLock) {
            return e.down && !e.repeat ? toggleCapsLock() : RCS_OK;
        }
        if (keys_[e.code] == bool(e.down) && !e.repeat) {
            return RCS_OK;
        }
        keys_[e.code] = e.down;
        CGEventRef event = CGEventCreateKeyboardEvent(source_, CGKeyCode(code), e.down);
        if (event) {
            CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, e.repeat);
        }
        post(event);
        return RCS_OK;
    }

    // A system-defined auxiliary key event (NX_SUBTYPE_AUX_CONTROL_BUTTONS):
    // the key in the high half of data1, down (0xA) or up (0xB) below it.
    void postAuxiliary(int auxiliary, bool down) {
        @autoreleasepool {
            const NSEventModifierFlags flags = down ? 0xA00 : 0xB00;
            const NSInteger data = (auxiliary << 16) | ((down ? 0xA : 0xB) << 8);
            NSEvent* event = [NSEvent otherEventWithType:NSEventTypeSystemDefined
                                                location:NSZeroPoint
                                           modifierFlags:flags
                                               timestamp:0
                                            windowNumber:0
                                                 context:nil
                                                 subtype:NX_SUBTYPE_AUX_CONTROL_BUTTONS
                                                   data1:data
                                                   data2:-1];
            if (CGEventRef posted = event.CGEvent) {
                CGEventPost(kCGHIDEventTap, posted);
            }
        }
    }

    // A media key: it acts on the machine, not on a window, so none while
    // input is confined to one.
    rcs_result auxiliaryKey(uint32_t usage, bool down) {
        const int auxiliary = keys::macConsumerKey(usage);
        if (auxiliary < 0) {
            return RCS_ERROR_UNSUPPORTED;
        }
        if (confined() && down) {
            return RCS_ERROR_INVALID_STATE;
        }
        if (auxiliary_[auxiliary] != down) {
            auxiliary_[auxiliary] = down;
            postAuxiliary(auxiliary, down);
        }
        return RCS_OK;
    }

    // One character as the Unicode string of a key press, with no modifiers
    // (held keys must not make it a shortcut). Tab and line feed are the keys.
    rcs_result text(uint32_t character) {
        if (confined() && !focused()) {
            return RCS_ERROR_INVALID_STATE;
        }
        if (character == '\t' || character == '\n') {
            const CGKeyCode code = character == '\t' ? 0x30 : 0x24; // kVK_Tab, kVK_Return.
            for (const bool down : {true, false}) {
                if (CGEventRef event = CGEventCreateKeyboardEvent(source_, code, down)) {
                    CGEventSetFlags(event, 0);
                    CGEventPost(kCGHIDEventTap, event);
                    CFRelease(event);
                }
            }
            return RCS_OK;
        }
        UniChar units[2];
        UniCharCount count = 1;
        if (character >= 0x10000) {
            units[0] = UniChar(0xD800 + ((character - 0x10000) >> 10));
            units[1] = UniChar(0xDC00 + ((character - 0x10000) & 0x3FF));
            count = 2;
        } else {
            units[0] = UniChar(character);
        }
        for (const bool down : {true, false}) {
            if (CGEventRef event = CGEventCreateKeyboardEvent(source_, 0, down)) {
                CGEventKeyboardSetUnicodeString(event, count, units);
                CGEventSetFlags(event, 0);
                CGEventPost(kCGHIDEventTap, event);
                CFRelease(event);
            }
        }
        return RCS_OK;
    }

    rcs_result toggleCapsLock() {
        if (!hid_) {
            const io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(kIOHIDSystemClass));
            const kern_return_t opened =
                service ? IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &hid_) : KERN_FAILURE;
            if (service) {
                IOObjectRelease(service);
            }
            if (opened != KERN_SUCCESS) {
                hid_ = 0;
                return RCS_ERROR_PERMISSION_REQUIRED;
            }
        }
        bool on = false;
        if (IOHIDGetModifierLockState(hid_, kIOHIDCapsLockState, &on) != KERN_SUCCESS ||
            IOHIDSetModifierLockState(hid_, kIOHIDCapsLockState, !on) != KERN_SUCCESS) {
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
        return RCS_OK;
    }

    CGWindowID window() const { return CGWindowID(target_.windowId); }
    bool confined() const { return target_.source == RCS_CAPTURE_WINDOW || target_.source == RCS_CAPTURE_APPLICATION; }

    // Whether keys go to what is shared: its application has the focus and,
    // sharing a window, that window is the application's frontmost.
    bool focused() const {
        return mac::focusedProcess() == process_ &&
               (target_.source != RCS_CAPTURE_WINDOW || mac::frontWindow(process_) == window());
    }

    keys::Modifiers held() const {
        return {keys_[224] || keys_[228], keys_[225] || keys_[229], keys_[226] || keys_[230], keys_[227] || keys_[231]};
    }
    // Input on `stream` stays inside what it shares (only stream 0 shares less than a display).
    bool confined(uint32_t stream) const { return !stream && confined(); }

    // What `stream` shares (rcs_streams_config: stream 0 the target, the
    // others whole displays), in global points; none while it does not show.
    std::optional<CGRect> area(uint32_t stream, bool fresh) {
        if (!stream) {
            return area(fresh);
        }
        const CGDirectDisplayID display = streamDisplays_.at(stream - 1);
        return mac::sharedArea({.displayId = display}, display);
    }

    // What stream 0 shares, in global points; none while it does not show. A
    // window's place is looked up again for presses, and every 100 ms for moves.
    std::optional<CGRect> area(bool fresh) {
        if (target_.source != RCS_CAPTURE_WINDOW) {
            return mac::sharedArea(target_, display_);
        }
        const auto now = Clock::now();
        if (fresh || now >= windowLooked_ + 100ms) {
            windowLooked_ = now;
            windowArea_ = mac::sharedArea(target_, display_);
        }
        return windowArea_;
    }

    // Whether a press at `point` reaches what is shared: the window in front
    // there is the shared one (or a menu of its application) or, sharing an
    // application, one of its windows.
    bool reaches(const std::optional<mac::WindowAt>& front) const {
        if (!front || front->process != process_) {
            return false;
        }
        return target_.source == RCS_CAPTURE_APPLICATION || front->window == window() || front->layer != 0;
    }

    // Brings what is shared to the front at `point` when another window is
    // there; INVALID_STATE when nothing shared is there or it cannot come.
    rcs_result reach(CGPoint point, bool wheel) {
        // A scroll gesture's turns come many a second at about one place:
        // what was in front there a moment ago still is.
        const auto now = Clock::now();
        if (wheel && now < reached_ + 250ms && std::abs(point.x - reachedPoint_.x) < 8 &&
            std::abs(point.y - reachedPoint_.y) < 8) {
            return RCS_OK;
        }
        const rcs_result result = bring(point);
        if (result == RCS_OK) {
            reached_ = now;
            reachedPoint_ = point;
        }
        return result;
    }

    rcs_result bring(CGPoint point) {
        if (reaches(mac::windowAt(point, process_))) {
            return RCS_OK;
        }
        std::optional<mac::WindowAt> wanted;
        if (target_.source == RCS_CAPTURE_WINDOW) {
            if (const auto state = mac::windowState(window()); state && state->onScreen) {
                wanted = mac::WindowAt{window(), state->process, state->bounds};
            }
        } else {
            wanted = mac::windowOfAt(process_, point);
        }
        if (!wanted || !mac::bringToFront(*wanted, point, [this](const auto& front) { return reaches(front); })) {
            return RCS_ERROR_INVALID_STATE;
        }
        return RCS_OK;
    }

    rcs_result pointer(const rcs_input_event& e) {
        const bool wheel = e.type == RCS_INPUT_WHEEL || e.type == RCS_INPUT_SCROLL;
        const bool press = wheel || (e.type == RCS_INPUT_BUTTON && e.down);
        if (e.stream > streamDisplays_.size()) {
            return RCS_ERROR_INVALID_ARGUMENT;
        }
        const auto bounds = area(e.stream, press);
        if (!bounds) {
            // A window that does not show takes no input; a display that is gone takes none either.
            return !e.stream && target_.source == RCS_CAPTURE_WINDOW ? RCS_ERROR_INVALID_STATE : RCS_ERROR_UNSUPPORTED;
        }
        point_ = {bounds->origin.x + e.x / 65535.0 * (bounds->size.width - 1),
                  bounds->origin.y + e.y / 65535.0 * (bounds->size.height - 1)};
        if (confined(e.stream) && press && !(e.type == RCS_INPUT_BUTTON && buttons_[e.code - 1])) {
            if (const rcs_result reached = reach(point_, wheel); reached != RCS_OK) {
                return reached;
            }
        }
        if (e.type == RCS_INPUT_WHEEL) {
            CGEventRef event = CGEventCreateScrollWheelEvent(source_, kCGScrollEventUnitLine, 2, e.wheel_y, -e.wheel_x);
            if (event) {
                CGEventSetLocation(event, point_);
            }
            post(event);
            return RCS_OK;
        }
        if (e.type == RCS_INPUT_SCROLL) {
            // 1/120 lines in points: macOS scrolls 10 points a line. What does
            // not make a whole point waits for the next turn.
            const int64_t across = int64_t(e.wheel_x) * 10 + scrollRest_[0],
                          down = int64_t(e.wheel_y) * 10 + scrollRest_[1];
            const auto x = int32_t(across / 120), y = int32_t(down / 120);
            scrollRest_ = {across - int64_t(x) * 120, down - int64_t(y) * 120};
            if (x || y) {
                CGEventRef event = CGEventCreateScrollWheelEvent(source_, kCGScrollEventUnitPixel, 2, y, -x);
                if (event) {
                    CGEventSetLocation(event, point_);
                }
                post(event);
            }
            return RCS_OK;
        }
        if (e.type == RCS_INPUT_BUTTON) {
            const unsigned button = e.code - 1;
            if (buttons_[button] == bool(e.down)) {
                return RCS_OK;
            }
            buttons_[button] = e.down;
            CGEventRef event =
                CGEventCreateMouseEvent(source_, buttonEvent(button, e.down), point_, CGMouseButton(button));
            if (event) {
                CGEventSetIntegerValueField(event, kCGMouseEventClickState, clickCount(button, e.down));
            }
            post(event);
            return RCS_OK;
        }
        post(moveEvent(point_));
        return RCS_OK;
    }

    // A move to `point`: while a button is held a drag, which window moves and
    // selections rely on.
    CGEventRef moveEvent(CGPoint point) const {
        CGEventType type = kCGEventMouseMoved;
        unsigned button = 0;
        if (buttons_[0]) {
            type = kCGEventLeftMouseDragged;
        } else if (buttons_[1]) {
            type = kCGEventRightMouseDragged;
            button = 1;
        } else if (const auto held = std::find(buttons_.begin() + 2, buttons_.end(), true); held != buttons_.end()) {
            type = kCGEventOtherMouseDragged;
            button = unsigned(held - buttons_.begin());
        }
        return CGEventCreateMouseEvent(source_, type, point, CGMouseButton(button));
    }

    // From where the pointer is now, by the motion in pixels, carrying the
    // motion itself (what programs that hide the pointer read). Sharing a
    // display it may go on to the next display, as the mouse would; sharing
    // a region, a window or an application it stays inside.
    rcs_result relative(const rcs_input_event& e) {
        const auto bounds = area(false);
        if (!bounds) {
            return target_.source == RCS_CAPTURE_WINDOW ? RCS_ERROR_INVALID_STATE : RCS_ERROR_UNSUPPORTED;
        }
        CGRect limits = *bounds;
        if (target_.source == RCS_CAPTURE_DISPLAY) {
            uint32_t count = 0;
            CGDirectDisplayID displays[16];
            if (CGGetActiveDisplayList(16, displays, &count) == kCGErrorSuccess) {
                for (uint32_t i = 0; i < count; ++i) {
                    limits = CGRectUnion(limits, CGDisplayBounds(displays[i]));
                }
            }
        }
        if (CGEventRef current = CGEventCreate(nullptr)) {
            point_ = CGEventGetLocation(current);
            CFRelease(current);
        }
        const double scale = mac::pixelsPerPoint(display_);
        point_ = {std::clamp(point_.x + e.dx / scale, limits.origin.x, limits.origin.x + limits.size.width - 1),
                  std::clamp(point_.y + e.dy / scale, limits.origin.y, limits.origin.y + limits.size.height - 1)};
        CGEventRef event = moveEvent(point_);
        if (event) {
            CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, e.dx);
            CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, e.dy);
        }
        post(event);
        return RCS_OK;
    }

    // The pen comes into range (a proximity event naming the pen or its
    // eraser), hovers and touches as mouse events of the tablet subtype with
    // its pressure and tilt (the tip the left button, with the barrel button
    // held the right), and leaves (proximity again).
    rcs_result pen(const rcs_input_event& e) {
        const bool leave = e.pen & RCS_PEN_LEAVE;
        const bool down = e.down && !leave;
        if (e.stream > streamDisplays_.size()) {
            return RCS_ERROR_INVALID_ARGUMENT;
        }
        const auto bounds = area(e.stream, down && !penDown_);
        if (!bounds) {
            return !e.stream && target_.source == RCS_CAPTURE_WINDOW ? RCS_ERROR_INVALID_STATE : RCS_ERROR_UNSUPPORTED;
        }
        point_ = {bounds->origin.x + e.x / 65535.0 * (bounds->size.width - 1),
                  bounds->origin.y + e.y / 65535.0 * (bounds->size.height - 1)};
        if (confined(e.stream) && down && !penDown_) {
            if (const rcs_result reached = reach(point_, false); reached != RCS_OK) {
                return reached;
            }
        }
        const bool eraser = e.pen & RCS_PEN_ERASER;
        if (!penInRange_ && !leave) {
            proximity(true, eraser);
            penInRange_ = true;
            penEraser_ = eraser;
        }
        if (penInRange_) {
            CGEventType type = kCGEventMouseMoved;
            if (down != penDown_) {
                if (down) {
                    penButton_ = (e.pen & RCS_PEN_BARREL) ? 1 : 0;
                }
                type = buttonEvent(penButton_, down);
            } else if (down) {
                type = penButton_ ? kCGEventRightMouseDragged : kCGEventLeftMouseDragged;
            }
            CGEventRef event = CGEventCreateMouseEvent(source_, type, point_, CGMouseButton(penButton_));
            if (event) {
                const double pressure = down ? (e.pressure ? e.pressure / 1024.0 : 1.0) : 0.0;
                CGEventSetIntegerValueField(event, kCGMouseEventSubtype, kCGEventMouseSubtypeTabletPoint);
                CGEventSetDoubleValueField(event, kCGMouseEventPressure, pressure);
                CGEventSetDoubleValueField(event, kCGTabletEventPointPressure, pressure);
                CGEventSetDoubleValueField(event, kCGTabletEventTiltX, e.tilt_x / 90.0);
                CGEventSetDoubleValueField(event, kCGTabletEventTiltY, e.tilt_y / 90.0);
                CGEventSetDoubleValueField(event, kCGTabletEventRotation, e.rotation);
                CGEventSetIntegerValueField(event, kCGTabletEventDeviceID, penDevice);
                if (down != penDown_) {
                    CGEventSetIntegerValueField(event, kCGMouseEventClickState, 1);
                }
            }
            post(event);
        }
        penDown_ = down;
        if (leave && penInRange_) {
            proximity(false, penEraser_);
            penInRange_ = false;
        }
        return RCS_OK;
    }

    void proximity(bool enter, bool eraser) {
        CGEventRef event = CGEventCreate(source_);
        if (!event) {
            return;
        }
        CGEventSetType(event, kCGEventTabletProximity);
        CGEventSetLocation(event, point_);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventEnterProximity, enter);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventPointerType,
                                    eraser ? NX_TABLET_POINTER_ERASER : NX_TABLET_POINTER_PEN);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventDeviceID, penDevice);
        CGEventSetIntegerValueField(event, kCGTabletProximityEventCapabilityMask,
                                    NX_TABLET_CAPABILITY_DEVICEIDMASK | NX_TABLET_CAPABILITY_ABSXMASK |
                                        NX_TABLET_CAPABILITY_ABSYMASK | NX_TABLET_CAPABILITY_BUTTONSMASK |
                                        NX_TABLET_CAPABILITY_TILTXMASK | NX_TABLET_CAPABILITY_TILTYMASK |
                                        NX_TABLET_CAPABILITY_PRESSUREMASK | NX_TABLET_CAPABILITY_ROTATIONMASK);
        post(event);
    }

    // macOS needs an explicit click count for double and triple clicks.
    int clickCount(unsigned button, bool down) {
        if (down) {
            const uint64_t now = monotonicNs();
            const double dx = point_.x - clickPoint_[button].x, dy = point_.y - clickPoint_[button].y;
            const bool repeated = now - lastClick_[button] < 500'000'000 && dx * dx + dy * dy < 16;
            clicks_[button] = repeated ? std::min(3, clicks_[button] + 1) : 1;
            lastClick_[button] = now;
            clickPoint_[button] = point_;
        }
        return std::max(1, clicks_[button]);
    }

    CGEventSourceRef source_;
    const CaptureTarget target_;
    const std::vector<uint32_t> streamDisplays_; // Streams 1, 2, ...: whole displays.
    CGDirectDisplayID display_;
    pid_t process_ = 0; // WINDOW, APPLICATION: whose windows input stays in.
    Clock::time_point windowLooked_{};
    std::optional<CGRect> windowArea_;
    Clock::time_point reached_{}; // The last press or turn that reached what is shared, and where.
    CGPoint reachedPoint_{};
    io_connect_t hid_ = 0; // For Caps Lock, opened when first needed.
    CGPoint point_{};
    std::array<bool, keys::lastUsage + 1> keys_{};
    std::array<bool, 32> auxiliary_{}; // Media keys held, by NX_KEYTYPE.
    std::array<bool, 5> buttons_{};
    std::array<uint64_t, 5> lastClick_{};
    std::array<CGPoint, 5> clickPoint_{};
    std::array<int, 5> clicks_{};
    std::array<int64_t, 2> scrollRest_{}; // SCROLL: 1/120 points not yet scrolled, across and down.
    static constexpr int64_t penDevice = 1;
    bool penInRange_ = false, penDown_ = false;
    bool penEraser_ = false; // It came into range as the eraser.
    unsigned penButton_ = 0; // The button the pen's tip is: left, or right with the barrel button.
};

} // namespace

std::unique_ptr<InputInjector> createInputInjector(const CaptureTarget& target,
                                                   const std::vector<uint32_t>& streamDisplays, bool) {
    return std::make_unique<CoreGraphicsInjector>(target, streamDisplays);
}

} // namespace rcsdk::platform
