// Keeping an iPhone or iPad from locking while a session needs it: the
// application's idle timer, off while any request holds it (set on the main
// thread, as UIKit wants). iOS keeps an application running only while its
// display is on, so it has no request for the system alone. An app extension
// has no application, and so no idle timer.
#include "power_request.hpp"

#import <UIKit/UIKit.h>

#include <atomic>

namespace rcsdk::power {
namespace {

std::atomic<int> holding{0}; // Requests that hold the idle timer off.

void apply() {
    dispatch_async(dispatch_get_main_queue(), ^{
      // A process that is no application (no shared application) has nothing to keep on.
      UIApplication.sharedApplication.idleTimerDisabled = holding.load() > 0;
    });
}

class IdleTimer final : public Request {
public:
    IdleTimer() {
        ++holding;
        apply();
    }
    ~IdleTimer() override {
        --holding;
        apply();
    }
};

bool inExtension() {
    return [NSBundle.mainBundle.bundleURL.pathExtension isEqualToString:@"appex"];
}

} // namespace

std::unique_ptr<Request> systemRequest(rcs_keep_awake mode, const std::string&) {
    if (inExtension()) {
        throw Refused(RCS_ERROR_UNSUPPORTED, "An app extension cannot keep the device from locking; its application "
                                             "can (a session there with this module)");
    }
    if (mode != RCS_KEEP_AWAKE_DISPLAY) {
        throw Refused(RCS_ERROR_UNSUPPORTED, "iOS keeps an application running only while its display is on: "
                                             "RCS_KEEP_AWAKE_DISPLAY keeps the device from locking");
    }
    return std::make_unique<IdleTimer>();
}

bool systemAvailable() {
    return !inExtension();
}

} // namespace rcsdk::power
