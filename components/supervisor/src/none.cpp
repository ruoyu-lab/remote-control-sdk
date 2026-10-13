// Systems without a supervisor or sessions to choose from: everything but
// Windows (macOS starts and restarts helpers with launchd).
#include "supervisor.hpp"

namespace rcsdk::supervisor {

std::vector<rcs_os_session_info> osSessions() {
    fail(RCS_ERROR_UNSUPPORTED, "Sessions to choose from are Windows' (Remote Desktop Services); this system has none");
}

std::unique_ptr<Supervisor> create(const Config&, Supervisor::Handler) {
#ifdef __APPLE__
    fail(RCS_ERROR_UNSUPPORTED, "A supervisor is for Windows services; on macOS launchd starts and restarts helpers");
#else
    fail(RCS_ERROR_UNSUPPORTED, "A supervisor is for Windows services; this system has none");
#endif
}

} // namespace rcsdk::supervisor
