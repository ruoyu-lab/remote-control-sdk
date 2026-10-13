#include "platform/macos/window_server.hpp"
#include "platform/platform.hpp"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sys/sysctl.h>
#include <thread>

namespace rcsdk::platform::mac {
namespace {

CGRect boundsOf(NSDictionary* window) {
    CGRect bounds = CGRectNull;
    if (NSDictionary* value = window[(__bridge NSString*)kCGWindowBounds]) {
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)value, &bounds);
    }
    return bounds;
}

pid_t processOf(NSDictionary* window) { return pid_t([window[(__bridge NSString*)kCGWindowOwnerPID] intValue]); }

NSArray* onScreenWindows() {
    return CFBridgingRelease(CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
}

// Windows that take part in what is in front: visible, of a layer that
// takes clicks (the desktop's are below 0).
bool counts(NSDictionary* window) {
    return [window[(__bridge NSString*)kCGWindowLayer] intValue] >= 0 &&
           [window[(__bridge NSString*)kCGWindowAlpha] doubleValue] > 0;
}

bool ordinary(NSDictionary* window) {
    return [window[(__bridge NSString*)kCGWindowLayer] intValue] == 0 && counts(window);
}

bool near(CGFloat a, CGFloat b) { return std::fabs(a - b) < 2; }

} // namespace

double pixelsPerPoint(CGDirectDisplayID display) {
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (!mode) {
        return 1;
    }
    const double points = double(CGDisplayModeGetWidth(mode));
    const double scale = points > 0 ? double(CGDisplayModeGetPixelWidth(mode)) / points : 1;
    CGDisplayModeRelease(mode);
    return std::clamp(scale, 1.0, 4.0);
}

DisplayGeometry displayGeometry(CGDirectDisplayID display) {
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (!mode) {
        return {};
    }
    const double points = double(CGDisplayModeGetWidth(mode));
    const double scale = points > 0 ? double(CGDisplayModeGetPixelWidth(mode)) / points : 1;
    const CGRect bounds = CGDisplayBounds(display);
    DisplayGeometry geometry{uint32_t(std::lround(bounds.size.width * scale)),
                             uint32_t(std::lround(bounds.size.height * scale)),
                             uint32_t(std::lround(CGDisplayModeGetRefreshRate(mode))),
                             uint32_t(std::lround(CGDisplayRotation(display))) % 360};
    CGDisplayModeRelease(mode);
    return geometry;
}

CGDirectDisplayID displayShowing(CGRect bounds) {
    CGDirectDisplayID displays[32];
    uint32_t count = 0;
    if (CGGetDisplaysWithRect(bounds, 32, displays, &count) != kCGErrorSuccess) {
        return 0;
    }
    CGDirectDisplayID best = 0;
    double most = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const CGRect shown = CGRectIntersection(bounds, CGDisplayBounds(displays[i]));
        if (const double area = shown.size.width * shown.size.height; area > most) {
            most = area;
            best = displays[i];
        }
    }
    return best;
}

std::optional<WindowState> windowState(CGWindowID window) {
    @autoreleasepool {
        const void* ids[] = {reinterpret_cast<const void*>(uintptr_t(window))};
        CFArrayRef array = CFArrayCreate(nullptr, ids, 1, nullptr);
        NSArray* described = CFBridgingRelease(CGWindowListCreateDescriptionFromArray(array));
        CFRelease(array);
        NSDictionary* found = described.firstObject;
        if (!found) {
            return std::nullopt;
        }
        // BOOL is signed char on x86-64: compared, not narrowed.
        return WindowState{boundsOf(found), [found[(__bridge NSString*)kCGWindowIsOnscreen] boolValue] != NO,
                           processOf(found)};
    }
}

bool processRuns(pid_t process) {
    // Not one that ended and waits for its parent to collect it.
    kinfo_proc info{};
    size_t size = sizeof(info);
    int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, process};
    return process > 0 && sysctl(name, 4, &info, &size, nullptr, 0) == 0 && size == sizeof(info) &&
           info.kp_proc.p_stat != SZOMB;
}

bool showsOn(pid_t process, CGDirectDisplayID display) {
    @autoreleasepool {
        const CGRect area = CGDisplayBounds(display);
        for (NSDictionary* window in onScreenWindows()) {
            if (processOf(window) == process && ordinary(window) &&
                !CGRectIsEmpty(CGRectIntersection(boundsOf(window), area))) {
                return true;
            }
        }
        return false;
    }
}

std::optional<WindowAt> windowAt(CGPoint point, pid_t owner) {
    @autoreleasepool {
        for (NSDictionary* window in onScreenWindows()) { // Front to back.
            const CGRect bounds = boundsOf(window);
            if (counts(window) && CGRectContainsPoint(bounds, point) &&
                (ordinary(window) || processOf(window) == owner)) {
                return WindowAt{[window[(__bridge NSString*)kCGWindowNumber] unsignedIntValue],
                                processOf(window),
                                bounds,
                                [window[(__bridge NSString*)kCGWindowLayer] intValue]};
            }
        }
        return std::nullopt;
    }
}

std::optional<WindowAt> windowOfAt(pid_t process, CGPoint point) {
    @autoreleasepool {
        for (NSDictionary* window in onScreenWindows()) {
            const CGRect bounds = boundsOf(window);
            if (processOf(window) == process && ordinary(window) && CGRectContainsPoint(bounds, point)) {
                return WindowAt{[window[(__bridge NSString*)kCGWindowNumber] unsignedIntValue], process, bounds};
            }
        }
        return std::nullopt;
    }
}

CGWindowID frontWindow(pid_t process) {
    @autoreleasepool {
        for (NSDictionary* window in onScreenWindows()) {
            if (processOf(window) == process && ordinary(window)) {
                return [window[(__bridge NSString*)kCGWindowNumber] unsignedIntValue];
            }
        }
        return 0;
    }
}

namespace {

// A window of this process: Accessibility would serve the request right here,
// on this thread, and AppKit works on the main thread only (it ends the
// process). The main thread raises it the next time its loop runs; where it
// runs none, the window does not come (and the press does not reach it).
void raiseOwnWindow(CGWindowID number) {
    dispatch_async(dispatch_get_main_queue(), ^{
        for (NSWindow* window in NSApp.windows) {
            if (window.windowNumber == NSInteger(number)) {
                [NSApp activate];
                [window orderFrontRegardless];
                return;
            }
        }
    });
}

// Another process's window, through Accessibility (which injecting input
// needs too), and its application to the front.
void raiseWindowOf(const WindowAt& target) {
    @autoreleasepool {
        // Accessibility names windows by their place, not by their number:
        // the one where the window server has this one is raised.
        AXUIElementRef application = AXUIElementCreateApplication(target.process);
        // An application that hangs holds up the input no longer than this per question.
        constexpr float timeoutSeconds = 0.25f;
        if (application) {
            AXUIElementSetMessagingTimeout(application, timeoutSeconds);
        }
        CFArrayRef windows = nullptr;
        if (application &&
            AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, (CFTypeRef*)&windows) == kAXErrorSuccess &&
            windows) {
            for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) {
                auto window = static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(windows, i));
                AXUIElementSetMessagingTimeout(window, timeoutSeconds);
                CFTypeRef position = nullptr, size = nullptr;
                CGPoint origin{};
                CGSize extent{};
                const bool placed =
                    AXUIElementCopyAttributeValue(window, kAXPositionAttribute, &position) == kAXErrorSuccess &&
                    AXUIElementCopyAttributeValue(window, kAXSizeAttribute, &size) == kAXErrorSuccess &&
                    AXValueGetValue(static_cast<AXValueRef>(position), kAXValueTypeCGPoint, &origin) &&
                    AXValueGetValue(static_cast<AXValueRef>(size), kAXValueTypeCGSize, &extent);
                if (position) {
                    CFRelease(position);
                }
                if (size) {
                    CFRelease(size);
                }
                if (placed && near(origin.x, target.bounds.origin.x) && near(origin.y, target.bounds.origin.y) &&
                    near(extent.width, target.bounds.size.width) && near(extent.height, target.bounds.size.height)) {
                    AXUIElementPerformAction(window, kAXRaiseAction);
                    break;
                }
            }
            CFRelease(windows);
        }
        if (application) {
            CFRelease(application);
        }
        [[NSRunningApplication runningApplicationWithProcessIdentifier:target.process] activateWithOptions:0];
    }
}

} // namespace

bool bringToFront(const WindowAt& target, CGPoint point,
                  const std::function<bool(const std::optional<WindowAt>&)>& reached) {
    if (target.process == getpid()) {
        raiseOwnWindow(target.window);
    } else {
        raiseWindowOf(target);
    }
    for (int wait = 0; wait < 15; ++wait) {
        if (reached(windowAt(point, target.process))) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::optional<WindowServerSession> windowServerSession() {
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session) {
        return std::nullopt;
    }
    const auto flag = [&](CFStringRef key) {
        const CFTypeRef value = CFDictionaryGetValue(session, key);
        if (value && CFGetTypeID(value) == CFBooleanGetTypeID()) {
            return CFBooleanGetValue(static_cast<CFBooleanRef>(value)) != 0;
        }
        int number = 0;
        return value && CFGetTypeID(value) == CFNumberGetTypeID() &&
               CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &number) && number != 0;
    };
    const WindowServerSession result{flag(kCGSessionLoginDoneKey), flag(kCGSessionOnConsoleKey),
                                     flag(CFSTR("CGSSessionScreenIsLocked"))};
    CFRelease(session);
    return result;
}

bool screenLocked() {
    const auto session = windowServerSession();
    return session && session->locked;
}

std::optional<CGRect> sharedArea(const CaptureTarget& target, CGDirectDisplayID display) {
    if (target.source == RCS_CAPTURE_WINDOW) {
        const auto state = windowState(CGWindowID(target.windowId));
        return state && state->onScreen ? std::optional<CGRect>(state->bounds) : std::nullopt;
    }
    const CGRect bounds = CGDisplayBounds(display);
    if (bounds.size.width <= 0 || bounds.size.height <= 0) {
        return std::nullopt;
    }
    if (target.source != RCS_CAPTURE_REGION) {
        return bounds;
    }
    const double scale = pixelsPerPoint(display);
    const auto& r = target.region;
    const CGRect region =
        CGRectIntersection(bounds, CGRectMake(bounds.origin.x + r.x / scale, bounds.origin.y + r.y / scale,
                                              r.width / scale, r.height / scale));
    return CGRectIsEmpty(region) ? std::nullopt : std::optional<CGRect>(region);
}

pid_t focusedProcess() {
    AXUIElementRef system = AXUIElementCreateSystemWide();
    AXUIElementRef application = nullptr;
    pid_t process = 0;
    if (AXUIElementCopyAttributeValue(system, kAXFocusedApplicationAttribute, (CFTypeRef*)&application) ==
            kAXErrorSuccess &&
        application) {
        AXUIElementGetPid(application, &process);
        CFRelease(application);
    }
    CFRelease(system);
    return process;
}

} // namespace rcsdk::platform::mac

namespace rcsdk::platform {

std::optional<SharedArea> sharedArea(const CaptureTarget& target) {
    const CGDirectDisplayID display = target.displayId ? target.displayId : CGMainDisplayID();
    const auto area = mac::sharedArea(target, display);
    if (!area) {
        return std::nullopt;
    }
    uint32_t process = target.source == RCS_CAPTURE_APPLICATION ? target.processId : 0;
    if (target.source == RCS_CAPTURE_WINDOW) {
        const auto state = mac::windowState(CGWindowID(target.windowId));
        process = state ? uint32_t(state->process) : 0;
    }
    return SharedArea{CGRectGetMinX(*area), CGRectGetMinY(*area), CGRectGetMaxX(*area), CGRectGetMaxY(*area), process};
}

// The login window (nobody has logged in to this session), a locked screen,
// or the user's desktop. Unknown outside a window-server session (a
// LaunchDaemon: capture belongs in a LaunchAgent) and while another user's
// session has the console (fast user switching): from here, what the machine
// shows cannot be seen.
DesktopState desktopState() {
    DesktopState state;
    const auto session = mac::windowServerSession();
    if (!session) {
        return state;
    }
    if (!session->loginDone) {
        state.kind = RCS_DESKTOP_SECURE;
        return state;
    }
    state.userLoggedOn = true;
    if (session->onConsole) {
        state.kind = session->locked ? RCS_DESKTOP_SECURE : RCS_DESKTOP_USER;
    }
    return state;
}

} // namespace rcsdk::platform
