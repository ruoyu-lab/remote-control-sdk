// macOS system queries and permissions.
#include "media/picture_size.hpp"
#include "platform/apple/cf.hpp"
#include "platform/apple/codecs.hpp"
#include "platform/macos/window_server.hpp"
#include "platform/platform.hpp"

#include <rcsdk/gpu.h>

#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <pthread.h>

namespace rcsdk::platform {

std::shared_ptr<void> stayResponsive() {
    @autoreleasepool {
        id activity = [NSProcessInfo.processInfo
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
                              reason:@"Remote desktop session"];
        return std::shared_ptr<void>((__bridge_retained void*)activity, [](void* held) {
            @autoreleasepool {
                [NSProcessInfo.processInfo endActivity:(__bridge_transfer id)held];
            }
        });
    }
}

rcs_capabilities capabilities() {
    rcs_capabilities value = RCS_CAPABILITIES_INIT;
    value.platform = RCS_PLATFORM_MACOS;
    value.capture_sources = (1u << RCS_CAPTURE_DISPLAY) | (1u << RCS_CAPTURE_REGION) | (1u << RCS_CAPTURE_WINDOW) |
                            (1u << RCS_CAPTURE_APPLICATION) | (1u << RCS_CAPTURE_HOST) | (1u << RCS_CAPTURE_ENCODED);
    // Everything but touch (no touch screens) and Ctrl+Alt+Del (no such thing).
    for (const rcs_input_type type :
         {RCS_INPUT_MOVE, RCS_INPUT_MOVE_RELATIVE, RCS_INPUT_BUTTON, RCS_INPUT_WHEEL, RCS_INPUT_SCROLL, RCS_INPUT_KEY,
          RCS_INPUT_TEXT, RCS_INPUT_PEN, RCS_INPUT_RELEASE_ALL}) {
        value.input_types |= 1u << type;
    }
    // ScreenCaptureKit captures in the encoder's YCbCr layout (420v, x420, 444v, x444).
    value.picture_formats = sdrPictureFormats;
    value.software_decoding_gpu_frames = 1; // Software decoders output IOSurface pixel buffers too.
    value.pushed_gpu_frames = 1u << RCS_GPU_CVPIXELBUFFER;
    return value;
}

std::vector<CodecImplementation> codecImplementations() {
    auto result = apple::encoderImplementations();
    const auto decoders = apple::decoderImplementations();
    result.insert(result.end(), decoders.begin(), decoders.end());
    return result;
}

std::vector<rcs_display_info> displays() {
    uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess) {
        fail(RCS_ERROR_CAPTURE, "Could not enumerate displays");
    }
    std::vector<CGDirectDisplayID> ids(count);
    if (CGGetActiveDisplayList(count, ids.data(), &count) != kCGErrorSuccess) {
        fail(RCS_ERROR_CAPTURE, "Could not enumerate displays");
    }
    std::vector<rcs_display_info> result;
    for (uint32_t i = 0; i < count; ++i) {
        const auto geometry = mac::displayGeometry(ids[i]);
        rcs_display_info info{};
        info.display_id = ids[i];
        info.pixel_width = geometry.width ? geometry.width : uint32_t(CGDisplayPixelsWide(ids[i]));
        info.pixel_height = geometry.height ? geometry.height : uint32_t(CGDisplayPixelsHigh(ids[i]));
        info.refresh_hz = geometry.refreshHz;
        info.rotation_degrees = uint32_t(std::lround(CGDisplayRotation(ids[i]))) % 360;
        info.is_main = ids[i] == CGMainDisplayID();
        const CGRect bounds = CGDisplayBounds(ids[i]); // Global points, the main display's top left at 0,0.
        info.desktop_x = int32_t(std::lround(bounds.origin.x));
        info.desktop_y = int32_t(std::lround(bounds.origin.y));
        info.desktop_width = uint32_t(std::lround(bounds.size.width));
        info.desktop_height = uint32_t(std::lround(bounds.size.height));
        result.push_back(info);
    }
    return result;
}

// Ordinary windows on screen, front to back. The first is the one in front:
// macOS gives the focus to the frontmost window.
std::vector<rcs_window_info> windows() {
    @autoreleasepool {
        NSArray* list = CFBridgingRelease(CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
        std::vector<rcs_window_info> result;
        for (NSDictionary* window in list) {
            CGRect bounds = CGRectNull;
            if (NSDictionary* value = window[(__bridge NSString*)kCGWindowBounds]) {
                CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)value, &bounds);
            }
            if ([window[(__bridge NSString*)kCGWindowLayer] intValue] != 0 ||
                [window[(__bridge NSString*)kCGWindowAlpha] doubleValue] <= 0 || CGRectIsNull(bounds) ||
                bounds.size.width < 1 || bounds.size.height < 1) {
                continue;
            }
            rcs_window_info& item = result.emplace_back();
            item.window_id = [window[(__bridge NSString*)kCGWindowNumber] unsignedLongLongValue];
            item.process_id = [window[(__bridge NSString*)kCGWindowOwnerPID] unsignedIntValue];
            const CGDirectDisplayID display = mac::displayShowing(bounds);
            const double scale = display ? mac::pixelsPerPoint(display) : 1;
            const CGRect area = display ? CGDisplayBounds(display) : CGRectZero;
            item.display_id = display;
            item.x = int32_t(std::lround((bounds.origin.x - area.origin.x) * scale));
            item.y = int32_t(std::lround((bounds.origin.y - area.origin.y) * scale));
            item.width = uint32_t(std::lround(bounds.size.width * scale));
            item.height = uint32_t(std::lround(bounds.size.height * scale));
            item.focused = result.size() == 1;
            const auto copy = [](NSString* text, char* to, size_t size) {
                // Cut at a character, never inside one.
                const std::string value = text.length ? text.UTF8String : "";
                size_t length = std::min(value.size(), size - 1);
                while (length && (uint8_t(value[length]) & 0xC0) == 0x80) {
                    --length;
                }
                std::memcpy(to, value.data(), length);
                to[length] = 0;
            };
            copy(window[(__bridge NSString*)kCGWindowName], item.title, sizeof(item.title));
            copy(window[(__bridge NSString*)kCGWindowOwnerName], item.application, sizeof(item.application));
        }
        return result;
    }
}

rcs_permission screenPermission() noexcept {
    return CGPreflightScreenCaptureAccess() ? RCS_PERMISSION_GRANTED : RCS_PERMISSION_REQUIRED;
}

rcs_result requestScreenPermission() {
    if (!pthread_main_np()) {
        return RCS_ERROR_WRONG_THREAD;
    }
    return CGRequestScreenCaptureAccess() ? RCS_OK : RCS_ERROR_PERMISSION_REQUIRED;
}

// Accessibility, which the user grants the program in System Settings.
rcs_permission inputPermission() noexcept {
    return AXIsProcessTrusted() ? RCS_PERMISSION_GRANTED : RCS_PERMISSION_REQUIRED;
}

// macOS has no desktop that injected input cannot reach: the permission decides.
bool inputAvailable(bool) noexcept {
    return AXIsProcessTrusted();
}

rcs_result requestInputPermission() {
    if (!pthread_main_np()) {
        return RCS_ERROR_WRONG_THREAD;
    }
    NSDictionary* options = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt : @YES};
    return AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options) ? RCS_OK : RCS_ERROR_PERMISSION_REQUIRED;
}

} // namespace rcsdk::platform
