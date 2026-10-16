// Whether the application is in the background (iOS): it enters it when no
// scene of it is in front, and leaves it once it is active again, from when
// its pictures may be decoded and drawn again. An app extension (e.g. a
// broadcast upload extension sharing the whole screen) has no application
// and never is.
#include "platform/platform.hpp"

#import <UIKit/UIKit.h>

#include <memory>

namespace rcsdk::platform {

std::shared_ptr<void> stayResponsive() {
    return nullptr;
}

void observeAppState(std::function<void(bool background)> changed) {
    if ([NSBundle.mainBundle.bundleURL.pathExtension isEqualToString:@"appex"]) {
        changed(false);
        return;
    }
    auto tell = std::make_shared<std::function<void(bool)>>(std::move(changed));
    // The observers stay for the rest of the process (seams::watchAppState asks once).
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:UIApplicationDidEnterBackgroundNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      (*tell)(true);
                    }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      (*tell)(false);
                    }];
    // The state now, read where UIKit wants it read. A process that is no
    // application (no shared application) is never in the background.
    dispatch_async(dispatch_get_main_queue(), ^{
      UIApplication* application = UIApplication.sharedApplication;
      (*tell)(application && application.applicationState == UIApplicationStateBackground);
    });
}

} // namespace rcsdk::platform
