#include "display_rate.hpp"

#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <algorithm>

// A display link's target that only asks for the rate: Qt Quick draws on its own.
@interface DisplayRateTarget : NSObject
- (void)tick:(CADisplayLink*)link;
@end

@implementation DisplayRateTarget
- (void)tick:(CADisplayLink*)link {
}
@end

DisplayRate::~DisplayRate() {
    setHighest(false);
}

void DisplayRate::setHighest(bool highest) {
    if (highest == (link_ != nullptr)) {
        return;
    }
    if (!highest) {
        CADisplayLink* link = (__bridge_transfer CADisplayLink*)link_;
        [link invalidate];
        link_ = nullptr;
        return;
    }
    CADisplayLink* link = [CADisplayLink displayLinkWithTarget:[DisplayRateTarget new] selector:@selector(tick:)];
    const float most = float(UIScreen.mainScreen.maximumFramesPerSecond);
    link.preferredFrameRateRange = CAFrameRateRangeMake(std::min(80.0f, most), most, most);
    [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    link_ = (__bridge_retained void*)link;
}
