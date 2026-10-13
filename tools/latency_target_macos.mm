// Full-screen latency target for macOS: a moving bar redrawn as a small dirty
// rectangle at 240 Hz (so capture always has new content, cheaply) and the
// visual marker, which advances when Space is pressed.
//
//   rcsdk_latency_target [seconds=60]
#include "visual_marker.hpp"

#import <AppKit/AppKit.h>

// Borderless windows cannot become key by default, so Space would never arrive.
@interface RCSTargetWindow : NSWindow
@end

@implementation RCSTargetWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
@end

@interface RCSTargetView : NSView
@property(nonatomic) unsigned counter;
@property(nonatomic) CGFloat barX;
@end

@implementation RCSTargetView
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)isFlipped {
    return YES; // Same top-down layout as the Windows target.
}
- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == 49 && !event.isARepeat) { // Space.
        self.counter = (self.counter + 1) & 255;
        [self setNeedsDisplay:YES];
    }
}
- (void)drawRect:(NSRect)dirty {
    const CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
    [[NSColor colorWithSRGBRed:0x20 / 255.0 green:0x2d / 255.0 blue:0x42 / 255.0 alpha:1] setFill];
    NSRectFill(dirty);
    const uint32_t value = marker::bits(uint8_t(self.counter));
    for (int i = 0; i < 32; ++i) {
        [((value & (1u << (31 - i))) ? NSColor.whiteColor : NSColor.blackColor) setFill];
        const CGFloat left = std::floor(w * (.2 + i * .6 / 32)), right = std::floor(w * (.2 + (i + 1) * .6 / 32));
        NSRectFill(NSIntersectionRect(dirty, NSMakeRect(left, h * .42, right - left, h * .16)));
    }
    [NSColor.cyanColor setFill];
    NSRectFill(NSIntersectionRect(dirty, NSMakeRect(self.barX, h * .8, 120, 40)));
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        const double seconds = argc > 1 ? std::atof(argv[1]) : 60;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        const NSRect frame = NSScreen.mainScreen.frame;
        NSWindow* window = [[RCSTargetWindow alloc] initWithContentRect:frame
                                                              styleMask:NSWindowStyleMaskBorderless
                                                                backing:NSBackingStoreBuffered
                                                                  defer:NO];
        window.level = NSScreenSaverWindowLevel;
        RCSTargetView* view = [[RCSTargetView alloc] initWithFrame:frame];
        window.contentView = view;
        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:view];
        [NSApp activateIgnoringOtherApps:YES];

        [NSTimer
            scheduledTimerWithTimeInterval:1.0 / 240
                                   repeats:YES
                                     block:^(NSTimer*) {
                                         const CGFloat h = view.bounds.size.height;
                                         const CGFloat width = std::max<CGFloat>(1, view.bounds.size.width - 120);
                                         const CGFloat previous = view.barX;
                                         view.barX = std::fmod(previous + 7, width);
                                         const CGFloat left = std::min(previous, view.barX);
                                         [view setNeedsDisplayInRect:NSMakeRect(left, h * .8,
                                                                                std::fabs(view.barX - previous) + 120,
                                                                                40)];
                                     }];
        [NSTimer scheduledTimerWithTimeInterval:seconds
                                        repeats:NO
                                          block:^(NSTimer*) {
                                              [NSApp terminate:nil];
                                          }];
        [NSApp run];
    }
    return 0;
}
