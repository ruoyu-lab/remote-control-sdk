#include "screen_broadcast.hpp"

#include <QPointer>

#import <ReplayKit/ReplayKit.h>
#import <UIKit/UIKit.h>

namespace {

UIWindow* keyWindow() {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:UIWindowScene.class]) {
            for (UIWindow* window in static_cast<UIWindowScene*>(scene).windows) {
                if (window.isKeyWindow) {
                    return window;
                }
            }
        }
    }
    return nil;
}

} // namespace

ScreenBroadcast::ScreenBroadcast(QObject* parent) : QObject(parent) {
    QPointer<ScreenBroadcast> self(this);
    id observer = [NSNotificationCenter.defaultCenter addObserverForName:UIScreenCapturedDidChangeNotification
                                                                  object:nil
                                                                   queue:NSOperationQueue.mainQueue
                                                              usingBlock:^(NSNotification*) {
                                                                if (self) {
                                                                    emit self->runningChanged();
                                                                }
                                                              }];
    observer_ = (__bridge_retained void*)observer;
}

ScreenBroadcast::~ScreenBroadcast() {
    [NSNotificationCenter.defaultCenter removeObserver:(__bridge_transfer id)observer_];
}

bool ScreenBroadcast::start(const BroadcastSettings& settings) {
    if (!saveBroadcastSettings(settings)) {
        return false;
    }
    showPicker();
    return true;
}

void ScreenBroadcast::showPicker() {
    // The system's picker for the extension; its button opens the sheet, tapped here for the user.
    RPSystemBroadcastPickerView* picker = [[RPSystemBroadcastPickerView alloc] initWithFrame:CGRectMake(0, 0, 1, 1)];
    picker.preferredExtension = @"local.rcsdk.qmldemo.broadcast";
    picker.showsMicrophoneButton = NO;
    picker.alpha = 0.01;
    [keyWindow() addSubview:picker];
    for (UIView* view in picker.subviews) {
        if ([view isKindOfClass:UIButton.class]) {
            [static_cast<UIButton*>(view) sendActionsForControlEvents:UIControlEventTouchUpInside];
        }
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      [picker removeFromSuperview];
    });
}

bool ScreenBroadcast::running() const {
    return UIScreen.mainScreen.isCaptured;
}
