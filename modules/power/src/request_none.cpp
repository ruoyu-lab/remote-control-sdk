// Systems without power requests.
#include "power_request.hpp"

namespace rcsdk::power {

std::unique_ptr<Request> systemRequest(rcs_keep_awake, const std::string&) {
    throw Refused(RCS_ERROR_UNSUPPORTED, "This system has no power requests");
}

bool systemAvailable() {
    return false;
}

} // namespace rcsdk::power
