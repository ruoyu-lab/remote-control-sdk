// Keeping the Mac awake with power assertions, shown by `pmset -g assertions`
// under the session's reason.
#include "power_request.hpp"

#import <Foundation/Foundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

#include <vector>

namespace rcsdk::power {
namespace {

class Assertions final : public Request {
public:
    // The system stays awake; for DISPLAY the display too, as on Windows.
    Assertions(rcs_keep_awake mode, const std::string& reason) {
        CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, reason.c_str(), kCFStringEncodingUTF8);
        if (!name) {
            throw Refused(RCS_ERROR_INVALID_ARGUMENT, "The reason for a power assertion is not UTF-8");
        }
        std::vector<CFStringRef> types{kIOPMAssertionTypePreventUserIdleSystemSleep};
        if (mode == RCS_KEEP_AWAKE_DISPLAY) {
            types.push_back(kIOPMAssertionTypePreventUserIdleDisplaySleep);
        }
        for (CFStringRef type : types) {
            IOPMAssertionID id = kIOPMNullAssertionID;
            const IOReturn result = IOPMAssertionCreateWithName(type, kIOPMAssertionLevelOn, name, &id);
            if (result != kIOReturnSuccess) {
                CFRelease(name);
                release();
                throw Refused(RCS_ERROR_INTERNAL,
                              "The system refused a power assertion (IOKit error " + std::to_string(result) + ")");
            }
            ids_.push_back(id);
        }
        CFRelease(name);
    }
    ~Assertions() override { release(); }

private:
    void release() noexcept {
        for (const IOPMAssertionID id : ids_) {
            IOPMAssertionRelease(id);
        }
        ids_.clear();
    }

    std::vector<IOPMAssertionID> ids_;
};

} // namespace

std::unique_ptr<Request> systemRequest(rcs_keep_awake mode, const std::string& reason) {
    return std::make_unique<Assertions>(mode, reason);
}

bool systemAvailable() {
    return true;
}

} // namespace rcsdk::power
