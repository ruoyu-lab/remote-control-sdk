// Where the module knows no system clipboard: target HOST works, SYSTEM does not.
#include "system.hpp"

namespace rcsdk::clipboard {

std::unique_ptr<SystemClipboard> createPlatformClipboard() {
    fail(RCS_ERROR_UNSUPPORTED, "The clipboard is not available on this platform");
}

uint32_t platformFormats() noexcept {
    return 0;
}

bool platformCarriesFiles() noexcept {
    return false;
}

uint32_t platformFiles(std::string& why) {
    why = "The clipboard is not available on this platform";
    return 0;
}

} // namespace rcsdk::clipboard
