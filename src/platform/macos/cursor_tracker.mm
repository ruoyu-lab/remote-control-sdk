#include "platform/macos/cursor_tracker.hpp"
#include "platform/macos/window_server.hpp"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace rcsdk::platform::mac {
namespace {

// The cursor's image at `scale` (fitted into maxCursorSize), straight-alpha
// BGRA, and its FNV-1a hash.
struct Rendered {
    std::shared_ptr<CursorImage> image;
    uint64_t hash = 0;
};

Rendered render(NSCursor* cursor, double scale) {
    Rendered result;
    NSImage* picture = cursor.image;
    const NSSize size = picture.size;
    const auto full = CursorSize{uint32_t(size.width * scale + 0.5), uint32_t(size.height * scale + 0.5)};
    const auto [width, height] = fittedCursorSize(full.width, full.height);
    if (!width || !height) {
        return result;
    }
    scale *= double(width) / full.width; // For the hotspot.
    NSRect proposed = NSMakeRect(0, 0, width, height);
    CGImageRef source = [picture CGImageForProposedRect:&proposed context:nil hints:nil];
    if (!source) {
        return result;
    }
    auto image = std::make_shared<CursorImage>();
    image->width = width;
    image->height = height;
    image->pixels.assign(size_t(width) * height * 4, 0);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context =
        CGBitmapContextCreate(image->pixels.data(), width, height, 8, width * 4, space,
                              uint32_t(kCGImageAlphaPremultipliedFirst) | uint32_t(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(space);
    if (!context) {
        return result;
    }
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), source);
    CGContextRelease(context);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < image->pixels.size(); i += 4) {
        uint8_t* p = image->pixels.data() + i;
        if (p[3] && p[3] < 255) { // Premultiplied to straight alpha.
            for (int c = 0; c < 3; ++c) {
                p[c] = uint8_t(std::min(255, p[c] * 255 / p[3]));
            }
        }
        for (int c = 0; c < 4; ++c) {
            hash = (hash ^ p[c]) * 1099511628211ull;
        }
    }
    const NSPoint hotspot = cursor.hotSpot; // Points from the top left.
    image->hotspotX = std::min<uint32_t>(uint32_t(std::max(0.0, hotspot.x * scale)), width - 1);
    image->hotspotY = std::min<uint32_t>(uint32_t(std::max(0.0, hotspot.y * scale)), height - 1);
    result.image = std::move(image);
    result.hash = hash ^ width ^ (uint64_t(height) << 32);
    return result;
}

// Standard cursors by the hash of their image at `scale`. Trackers of several
// captures may ask at once.
rcs_cursor_type standardType(uint64_t hash, double scale) {
    static std::mutex mutex;
    static std::map<double, std::map<uint64_t, rcs_cursor_type>> known;
    std::lock_guard lock(mutex);
    auto& table = known[scale];
    if (table.empty()) {
        const std::pair<NSCursor*, rcs_cursor_type> standard[] = {
            {NSCursor.arrowCursor, RCS_CURSOR_ARROW},
            {NSCursor.IBeamCursor, RCS_CURSOR_TEXT},
            {NSCursor.pointingHandCursor, RCS_CURSOR_HAND},
            {NSCursor.crosshairCursor, RCS_CURSOR_CROSSHAIR},
            {NSCursor.openHandCursor, RCS_CURSOR_GRAB},
            {NSCursor.closedHandCursor, RCS_CURSOR_GRABBING},
            {NSCursor.resizeLeftRightCursor, RCS_CURSOR_RESIZE_EW},
            {NSCursor.resizeUpDownCursor, RCS_CURSOR_RESIZE_NS},
            {NSCursor.operationNotAllowedCursor, RCS_CURSOR_NOT_ALLOWED},
        };
        for (const auto& [cursor, type] : standard) {
            if (const auto rendered = render(cursor, scale); rendered.image) {
                table.emplace(rendered.hash, type);
            }
        }
    }
    const auto found = table.find(hash);
    return found == table.end() ? RCS_CURSOR_CUSTOM : found->second;
}

} // namespace

void CursorTracker::poll(CGRect bounds, CGDirectDisplayID display) {
    @autoreleasepool {
        CGEventRef event = CGEventCreate(nullptr);
        const CGPoint point = event ? CGEventGetLocation(event) : CGPointZero; // Global, top left, points.
        if (event) {
            CFRelease(event);
        }
        // Hidden by an application (a video playing full screen, a game):
        // CGCursorIsVisible is deprecated without a successor and is the only
        // way to tell.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const bool shown = CGCursorIsVisible();
#pragma clang diagnostic pop
        // macOS also hides it while one types, until the mouse moves, and says
        // the same. Only a real mouse brings it back, never a move a viewer
        // sends, so the viewer would lose its pointer for good. Hidden as a key
        // went down (typed here or sent) counts as shown; applications hide it
        // later than that (a video after the mouse rests, a game as it starts).
        if (shown_ && !shown) {
            hiddenByTyping_ =
                CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGEventKeyDown) < 0.25;
        }
        hiddenByTyping_ = hiddenByTyping_ && !shown;
        shown_ = shown;
        state_.visible = CGRectContainsPoint(bounds, point) && (shown || hiddenByTyping_);
        const auto across = [](double offset, double size) {
            return size > 1 ? uint32_t(std::clamp(offset / (size - 1) * 65535, 0.0, 65535.0)) : 0u;
        };
        state_.x = across(point.x - bounds.origin.x, bounds.size.width);
        state_.y = across(point.y - bounds.origin.y, bounds.size.height);
        if (std::chrono::steady_clock::now() >= nextImage_) {
            nextImage_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            readImage(display);
        }
    }
}

void CursorTracker::readImage(CGDirectDisplayID display) {
    NSCursor* cursor = NSCursor.currentSystemCursor;
    if (!cursor) {
        return;
    }
    const double scale = pixelsPerPoint(display);
    auto rendered = render(cursor, scale);
    if (!rendered.image || rendered.hash == imageHash_) {
        return;
    }
    imageHash_ = rendered.hash;
    rendered.image->type = standardType(rendered.hash, scale);
    state_.image = std::move(rendered.image);
    ++state_.imageSerial;
}

} // namespace rcsdk::platform::mac
