// The text caret on macOS, through Accessibility (which remote input needs
// too): the focused element's selected text range, its end taken as the
// caret, and the bounds of the character before it (after it at the start of
// a line). Many applications answer no bounds for an empty range, so a
// character's are asked for. A secure text field (a password) has no caret to
// tell, and neither has an application that says nothing of its text.
//
// Never for this process's own windows: Accessibility would serve the request
// on the reading thread, and AppKit works on the main thread only. Each
// request waits at most a quarter of a second for an application that hangs.
#include "reader.hpp"

#import <ApplicationServices/ApplicationServices.h>

#include <optional>
#include <unistd.h>
#include <utility>

namespace rcsdk::text_caret {
namespace {

// Owns one CoreFoundation reference (released with CFRelease).
template <class T>
class CFRef {
public:
    T value = nullptr;
    CFRef() = default;
    explicit CFRef(T owned) : value(owned) {}
    ~CFRef() {
        if (value) {
            CFRelease(value);
        }
    }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    CFRef(CFRef&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    explicit operator bool() const noexcept { return value != nullptr; }
};

constexpr float requestTimeoutSeconds = 0.25f;

template <class T> CFRef<T> attribute(AXUIElementRef element, CFStringRef name) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess) {
        return {};
    }
    return CFRef<T>(static_cast<T>(value));
}

// What a range of the element's text holds, and where it shows (global points).
struct Character {
    bool lineBreak = false;
    CGRect bounds = CGRectNull;
};
std::optional<Character> character(AXUIElementRef element, CFIndex index) {
    const CFRange range{index, 1};
    CFRef<AXValueRef> value(AXValueCreate(kAXValueTypeCFRange, &range));
    CFTypeRef text = nullptr, bounds = nullptr;
    if (!value || AXUIElementCopyParameterizedAttributeValue(element, kAXBoundsForRangeParameterizedAttribute,
                                                             value.value, &bounds) != kAXErrorSuccess) {
        return std::nullopt;
    }
    CFRef<CFTypeRef> ownedBounds(bounds);
    Character result;
    if (CFGetTypeID(bounds) != AXValueGetTypeID() ||
        !AXValueGetValue(static_cast<AXValueRef>(bounds), kAXValueTypeCGRect, &result.bounds) ||
        result.bounds.size.height <= 0) {
        return std::nullopt;
    }
    if (AXUIElementCopyParameterizedAttributeValue(element, kAXStringForRangeParameterizedAttribute, value.value,
                                                   &text) == kAXErrorSuccess) {
        CFRef<CFTypeRef> ownedText(text);
        if (CFGetTypeID(text) == CFStringGetTypeID() && CFStringGetLength(static_cast<CFStringRef>(text)) > 0) {
            const UniChar first = CFStringGetCharacterAtIndex(static_cast<CFStringRef>(text), 0);
            result.lineBreak = first == '\n' || first == '\r' || first == 0x2028 || first == 0x2029;
        }
    }
    return result;
}

class AccessibilityReader final : public Reader {
  public:
    rcs_text_caret read(const rcs_module_shared& shared) override {
        CFRef<AXUIElementRef> system(AXUIElementCreateSystemWide());
        const auto application = attribute<AXUIElementRef>(system.value, kAXFocusedApplicationAttribute);
        pid_t process = 0;
        if (!application || AXUIElementGetPid(application.value, &process) != kAXErrorSuccess || process == getpid()) {
            return {};
        }
        if ((shared.source == RCS_CAPTURE_APPLICATION || shared.source == RCS_CAPTURE_WINDOW) &&
            process != pid_t(shared.process_id)) {
            return {}; // Typing goes to another application, which is not shared.
        }
        AXUIElementSetMessagingTimeout(application.value, requestTimeoutSeconds);
        const auto focused = attribute<AXUIElementRef>(application.value, kAXFocusedUIElementAttribute);
        if (!focused) {
            return {};
        }
        AXUIElementSetMessagingTimeout(focused.value, requestTimeoutSeconds);
        if (const auto subrole = attribute<CFStringRef>(focused.value, kAXSubroleAttribute);
            subrole && CFEqual(subrole.value, kAXSecureTextFieldSubrole)) {
            return {};
        }
        const auto selected = attribute<AXValueRef>(focused.value, kAXSelectedTextRangeAttribute);
        CFRange range{};
        if (!selected || !AXValueGetValue(selected.value, kAXValueTypeCFRange, &range)) {
            return {};
        }
        const CFIndex at = range.location + range.length;
        std::optional<ScreenRect> caret;
        if (at > 0) { // After the character before it, unless that ends a line.
            if (const auto before = character(focused.value, at - 1); before && !before->lineBreak) {
                const double x = CGRectGetMaxX(before->bounds);
                caret = ScreenRect{x, CGRectGetMinY(before->bounds), x + 1, CGRectGetMaxY(before->bounds)};
            }
        }
        if (!caret) { // Before the character after it.
            if (const auto after = character(focused.value, at)) {
                const double x = CGRectGetMinX(after->bounds);
                caret = ScreenRect{x, CGRectGetMinY(after->bounds), x + 1, CGRectGetMaxY(after->bounds)};
            }
        }
        return caret ? caretWithin(*caret, areaOf(shared)) : rcs_text_caret{};
    }
};

} // namespace

std::unique_ptr<Reader> createPlatformReader(rcs_module) { return std::make_unique<AccessibilityReader>(); }

bool platformAvailable() noexcept { return true; }

} // namespace rcsdk::text_caret
