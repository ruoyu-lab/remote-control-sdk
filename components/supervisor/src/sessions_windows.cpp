// Windows' sessions (rcs_list_os_sessions). Remote Desktop Services lists
// every session this process may query with its connection state; the rest
// is asked of each: who is signed in, and for a remote one its client's name
// and address, as the client itself reported them.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "sessions_windows.hpp"

#include <algorithm>
#include <memory>

namespace rcsdk::supervisor {
namespace {

// What Remote Desktop Services says of one session in one class; null when it
// does not say (the session ended, or this process may not query it).
std::unique_ptr<void, decltype(&WTSFreeMemory)> query(DWORD session, WTS_INFO_CLASS what, DWORD minimum) {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    const bool queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, what, &buffer, &bytes);
    std::unique_ptr<void, decltype(&WTSFreeMemory)> value(queried ? buffer : nullptr, &WTSFreeMemory);
    if (bytes < minimum) {
        value.reset();
    }
    return value;
}

std::wstring text(DWORD session, WTS_INFO_CLASS what) {
    const auto value = query(session, what, sizeof(wchar_t));
    return value ? std::wstring(static_cast<const wchar_t*>(value.get())) : std::wstring();
}

// The client's address as text; empty when the client did not report one. The
// layout is WTS_CLIENT_ADDRESS's as Microsoft documents it: an IPv4 address
// starts two bytes into Address, an IPv6 address at its start.
std::string clientAddress(DWORD session) {
    const auto value = query(session, WTSClientAddress, sizeof(WTS_CLIENT_ADDRESS));
    if (!value) {
        return {};
    }
    const auto& address = *static_cast<const WTS_CLIENT_ADDRESS*>(value.get());
    const BYTE* bytes = address.Address;
    size_t size = 0;
    if (address.AddressFamily == AF_INET) {
        bytes += 2;
        size = 4;
    } else if (address.AddressFamily == AF_INET6) {
        size = 16;
    }
    if (!size || std::all_of(bytes, bytes + size, [](BYTE b) { return b == 0; })) {
        return {};
    }
    char textual[INET6_ADDRSTRLEN] = {};
    // Needs no Winsock initialisation (Microsoft's InetNtop documentation).
    return inet_ntop(int(address.AddressFamily), bytes, textual, sizeof(textual)) ? textual : std::string();
}

} // namespace

namespace win {

std::optional<std::vector<SessionState>> sessionStates() {
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        return std::nullopt;
    }
    const std::unique_ptr<WTS_SESSION_INFOW, decltype(&WTSFreeMemory)> owned(sessions, &WTSFreeMemory);
    std::vector<SessionState> states;
    for (DWORD i = 0; i < count; ++i) {
        states.push_back({sessions[i].SessionId, sessions[i].State});
    }
    return states;
}

rcs_os_session_state groupOf(const SessionState& session) {
    if (session.id == 0 || session.state == WTSListen) {
        return RCS_OS_SESSION_NO_DESKTOP; // Session 0 is where services run: Windows lists it as disconnected.
    }
    switch (session.state) {
    case WTSActive:
    case WTSShadow: // Shown, and shadowing another session.
        return RCS_OS_SESSION_ACTIVE;
    case WTSConnected:
        return RCS_OS_SESSION_CONNECTED;
    case WTSDisconnected:
        return RCS_OS_SESSION_DISCONNECTED;
    default: // Idle, connecting, being reset, down, initialising.
        return RCS_OS_SESSION_OTHER;
    }
}

std::string notShown(const SessionState& session) {
    const std::string name = "Session " + std::to_string(session.id);
    switch (session.state) {
    case WTSDisconnected:
        return name + " is disconnected: it shows nothing until a client, or the console, connects to it again";
    case WTSListen:
        return name + " is a listener for Remote Desktop connections: it never shows a desktop";
    case WTSIdle:
        return name + " is idle: no client has connected to it";
    case WTSConnectQuery:
        return name + " is connecting";
    case WTSReset:
        return name + " is being reset";
    case WTSInit:
        return name + " is initialising";
    default:
        return name + " is down";
    }
}

} // namespace win

std::vector<rcs_os_session_info> osSessions() {
    const auto states = win::sessionStates();
    if (!states) {
        fail(RCS_ERROR_INTERNAL,
             "Remote Desktop Services did not list the sessions (" + win::errorText(GetLastError()) + ")");
    }
    const DWORD console = WTSGetActiveConsoleSessionId();
    std::vector<rcs_os_session_info> list;
    for (const auto& session : *states) {
        rcs_os_session_info& info = list.emplace_back();
        info.struct_size = sizeof(info);
        info.os_session = session.id;
        info.state = win::groupOf(session);
        info.is_console = session.id == console;
        const auto protocol = query(session.id, WTSClientProtocolType, sizeof(USHORT));
        info.is_remote = protocol && *static_cast<const USHORT*>(protocol.get()) != 0;
        const std::wstring user = text(session.id, WTSUserName);
        if (!user.empty()) {
            const std::wstring domain = text(session.id, WTSDomainName);
            copyText(info.account, win::narrow(domain.empty() ? user : domain + L"\\" + user));
        }
        copyText(info.client_name, win::narrow(text(session.id, WTSClientName)));
        copyText(info.client_address, clientAddress(session.id));
    }
    return list;
}

} // namespace rcsdk::supervisor
