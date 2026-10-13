// Windows' sessions as Remote Desktop Services lists them: the console's,
// Remote Desktop sessions, session 0 and the listener (rcs_list_os_sessions),
// and what the supervisor needs to know of them.
#pragma once

#include "windows.hpp"

#include <wtsapi32.h>

#include <optional>
#include <string>
#include <vector>

namespace rcsdk::supervisor::win {

struct SessionState {
    DWORD id = 0;
    WTS_CONNECTSTATE_CLASS state = WTSDown;
};
// Every session this process may query (LocalSystem: all of them); none when
// Remote Desktop Services cannot list them, GetLastError() says why.
std::optional<std::vector<SessionState>> sessionStates();
// As rcs_os_session_state groups it. A helper runs in ACTIVE and CONNECTED ones.
rcs_os_session_state groupOf(const SessionState& session);
// Why nothing can run in `session`, a user's (not session 0) that is not
// ACTIVE or CONNECTED, for people.
std::string notShown(const SessionState& session);

} // namespace rcsdk::supervisor::win
