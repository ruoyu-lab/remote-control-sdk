// Secure desktops, Ctrl+Alt+Del and a supervisor's helper on systems that
// have none of them: macOS (launchd starts and restarts helpers there) and
// platforms without a native backend. (Windows: platform/windows/desktop.cpp
// and helper.cpp; the macOS desktop state: platform/macos/window_server.mm.)
#include "platform/platform.hpp"

namespace rcsdk::platform {

bool secureDesktopAvailable() noexcept {
    return false;
}

bool secureAttentionAvailable() noexcept {
    return false;
}

rcs_result secureAttention() {
    return RCS_ERROR_UNSUPPORTED;
}

// No supervisor starts processes here, so no process is its helper.
rcs_result waitForSupervisorStop(WaitLimit) noexcept {
    return RCS_ERROR_UNSUPPORTED;
}

std::array<uint8_t, 32> helperKey() {
    fail(RCS_ERROR_UNSUPPORTED, "This process was not started by an rcs_supervisor (there is none on this system)");
}

} // namespace rcsdk::platform
