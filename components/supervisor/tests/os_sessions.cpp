// rcs_list_os_sessions through the public interface: on Windows this
// process's session is listed under its own number, at most one session is
// the console's, and every entry is filled in; elsewhere there are no such
// sessions (UNSUPPORTED).
#include <rcsdk/supervisor.hpp>

#include "support/check.hpp"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

bool terminated(const char* text, size_t size) {
    return std::memchr(text, 0, size) != nullptr;
}

void listed() {
    uint32_t count = 0;
    CHECK(rcs_list_os_sessions(nullptr, sizeof(rcs_os_session_info) + 4, 0, &count) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_list_os_sessions(nullptr, sizeof(rcs_os_session_info), 0, nullptr) == RCS_ERROR_INVALID_ARGUMENT);
    rcs_os_session_info one = RCS_OS_SESSION_INFO_INIT;
    CHECK(rcs_list_os_sessions(nullptr, sizeof(rcs_os_session_info), 1, &count) == RCS_ERROR_INVALID_ARGUMENT);
#ifdef _WIN32
    DWORD own = 0;
    CHECK(ProcessIdToSessionId(GetCurrentProcessId(), &own));
    const DWORD console = WTSGetActiveConsoleSessionId();
    const auto sessions = rcsdk::osSessions();
    CHECK(!sessions.empty());
    // Too small a buffer for the second pass says so.
    if (sessions.size() > 1) {
        CHECK(rcs_list_os_sessions(&one, sizeof(one), 1, &count) == RCS_ERROR_BUFFER_TOO_SMALL && count > 1);
    }
    int consoles = 0;
    bool ownListed = false;
    for (const auto& s : sessions) {
        std::printf("session %u: state %u, console %u, remote %u, account \"%s\", client \"%s\" (%s)\n", s.os_session,
                    s.state, s.is_console, s.is_remote, s.account, s.client_name, s.client_address);
        CHECK(s.struct_size == sizeof(rcs_os_session_info));
        CHECK(s.state >= RCS_OS_SESSION_ACTIVE && s.state <= RCS_OS_SESSION_OTHER);
        CHECK(terminated(s.account, sizeof(s.account)) && terminated(s.client_name, sizeof(s.client_name)) &&
              terminated(s.client_address, sizeof(s.client_address)));
        CHECK(s.is_console == 0 || s.is_console == 1);
        consoles += s.is_console ? 1 : 0;
        if (s.is_console) {
            CHECK(s.os_session == console && !s.is_remote); // The console's protocol is no remote one.
            CHECK(s.state == RCS_OS_SESSION_ACTIVE || s.state == RCS_OS_SESSION_CONNECTED);
        }
        if (s.os_session == 0) {
            CHECK(s.state == RCS_OS_SESSION_NO_DESKTOP && !s.is_console); // Services' session: never shown.
        }
        if (s.os_session == own) {
            ownListed = true;
            // This process runs there: a user's session that is shown (as a test runs), or session 0 (a service).
            CHECK(own == 0 || s.state == RCS_OS_SESSION_ACTIVE || s.state == RCS_OS_SESSION_CONNECTED);
        }
    }
    CHECK(ownListed);
    CHECK(consoles <= 1);
    CHECK(consoles == 1 || console == 0xFFFFFFFF || console == 0); // A console with a session has it listed.
#else
    CHECK(rcs_list_os_sessions(nullptr, sizeof(rcs_os_session_info), 0, &count) == RCS_ERROR_UNSUPPORTED);
    CHECK(rcs_list_os_sessions(&one, sizeof(one), 1, &count) == RCS_ERROR_UNSUPPORTED);
    CHECK(rcsdk::lastCallError().find("Windows") != std::string::npos);
    try {
        rcsdk::osSessions();
        CHECK(false);
    } catch (const rcsdk::Error& error) {
        CHECK(error.result() == RCS_ERROR_UNSUPPORTED);
    }
#endif
}

} // namespace

int main() {
    return runTest("os_sessions", listed);
}
