// Windows' secure desktops and the secure attention sequence (Ctrl+Alt+Del).
#include "platform/windows/desktop.hpp"

#include "platform/platform.hpp"
#include "platform/windows/win.hpp"
#include "util/log.hpp"

#include <wtsapi32.h>

#include <mutex>
#include <vector>

namespace rcsdk::platform {
namespace win {

std::wstring nameOf(HDESK desktop) {
    wchar_t name[256]{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed)) {
        return {};
    }
    return name;
}

bool userDesktopHasInput() {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) {
        return false; // Closed to this process: a desktop of the system's.
    }
    const bool user = nameOf(input) == userDesktop;
    CloseDesktop(input);
    return user;
}

namespace {

// The name of the desktop this thread works on now, looked up once per thread.
const std::wstring& ownName() {
    thread_local const std::wstring name = nameOf(GetThreadDesktop(GetCurrentThreadId()));
    return name;
}

} // namespace

bool runningAsSystem() noexcept {
    static const bool system = [] {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return false;
        }
        const Handle owned(token);
        const auto user = tokenInformation(token, TokenUser);
        return !user.empty() && IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid,
                                               WinLocalSystemSid);
    }();
    return system;
}

DWORD sessionId() noexcept {
    static const DWORD session = [] {
        DWORD id = 0;
        return ProcessIdToSessionId(GetCurrentProcessId(), &id) ? id : 0;
    }();
    return session;
}

bool InputDesktop::attach() {
    HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!input) {
        error_ = GetLastError();
        return false;
    }
    const std::wstring name = nameOf(input);
    if (current_ ? name == name_ : name == ownName()) {
        CloseDesktop(input);
        return true;
    }
    if (!own_) {
        own_ = GetThreadDesktop(GetCurrentThreadId());
    }
    thread_ = GetCurrentThreadId();
    if (!SetThreadDesktop(input)) {
        error_ = GetLastError();
        CloseDesktop(input);
        return false;
    }
    if (current_) {
        CloseDesktop(current_); // The thread is on `input` now.
    }
    current_ = input;
    name_ = name;
    return true;
}

void InputDesktop::restore() noexcept {
    if (current_ && own_ && thread_ == GetCurrentThreadId() && SetThreadDesktop(own_)) {
        CloseDesktop(current_);
    }
    current_ = nullptr; // (A handle still in use is left to the system.)
    name_.clear();
}

} // namespace win

namespace {

// Whether anyone is signed in to this session: the sign-in screen has nobody.
bool sessionHasUser() {
    LPWSTR name = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSUserName, &name, &bytes)) {
        return false;
    }
    const bool signedIn = name && name[0] != L'\0';
    WTSFreeMemory(name);
    return signedIn;
}

} // namespace

namespace win {

std::optional<DWORD> remoteSession() {
    // 0 is the console's protocol (Microsoft's WTSQuerySessionInformation documentation).
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSClientProtocolType, &buffer,
                                     &bytes)) {
        return std::nullopt;
    }
    const bool console = bytes < sizeof(USHORT) || *reinterpret_cast<const USHORT*>(buffer) == 0;
    WTSFreeMemory(buffer);
    if (console) {
        return std::nullopt;
    }
    return sessionId();
}

} // namespace win

DesktopState desktopState() {
    DesktopState state;
    if (HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)) {
        const std::wstring name = win::nameOf(input);
        CloseDesktop(input);
        // Only the user's own desktop: a screen saver's ("Screen-saver") is one
        // apart, like Winlogon's, which this process captures and controls only
        // by following it (as inputAvailable says).
        state.kind = name == win::userDesktop ? RCS_DESKTOP_USER : RCS_DESKTOP_SECURE;
    } else if (GetLastError() == ERROR_ACCESS_DENIED) {
        state.kind = RCS_DESKTOP_SECURE; // Closed to this process: a desktop of the system's.
    }
    state.userLoggedOn = sessionHasUser();
    return state;
}

bool secureDesktopAvailable() noexcept {
    return win::runningAsSystem() && win::sessionId() != 0;
}

// ---- Ctrl+Alt+Del ----------------------------------------------------------------------------

namespace {

// The system policy that lets services simulate the secure attention sequence
// (1: services, 3: services and Ease of Access applications). Read, never set.
bool policyAllowsServices() {
    DWORD value = 0, size = sizeof(value);
    const LSTATUS status =
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                     L"SoftwareSASGeneration", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && (value == 1 || value == 3);
}

// This process's end of the supervisor's Ctrl+Alt+Del pipe, when one started it.
HANDLE relayEnd() {
    const auto* link = win::supervisorLink();
    return link ? link->sas : nullptr;
}

// Sends it as a service, to the console session; the call reports nothing.
// Impersonating another process's token first (Microsoft's SendSAS
// documentation says the sequence then goes to that token's session) made it
// reach no session on Windows 11: the thread no longer counts as a service.
rcs_result sendSas() {
    using SendSas = void(WINAPI*)(BOOL);
    static const SendSas function = [] {
        HMODULE module = LoadLibraryExW(L"sas.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<SendSas>(GetProcAddress(module, "SendSAS")) : nullptr;
    }();
    if (!function) {
        log::warning("input", "Ctrl+Alt+Del was not sent: this system has no sas.dll");
        return RCS_ERROR_UNSUPPORTED;
    }
    function(FALSE);
    return RCS_OK;
}

// Asks the supervisor that started this process to send it (the supervisor is
// a service in session 0), through this process's end of a pipe of its: one
// byte out, a 32-bit result back, one request at a time.
rcs_result relaySas(HANDLE end) {
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    uint8_t request = 'S';
    int32_t result = RCS_ERROR_INTERNAL;
    DWORD read = 0;
    if (!TransactNamedPipe(end, &request, 1, &result, sizeof(result), &read, nullptr) || read != sizeof(result)) {
        const DWORD error = GetLastError();
        log::warning("input",
                     "Ctrl+Alt+Del was not sent: the supervisor did not answer (" + win::errorText(error) + ")");
        return RCS_ERROR_NETWORK;
    }
    return rcs_result(result);
}

} // namespace

bool secureAttentionAvailable() noexcept {
    return policyAllowsServices() && ((win::sessionId() == 0 && win::runningAsSystem()) || relayEnd() != nullptr);
}

rcs_result secureAttention() {
    if (win::sessionId() == 0 && win::runningAsSystem()) {
        if (!policyAllowsServices()) { // This process sends it, so this is where the policy matters.
            log::warning("input",
                         "Ctrl+Alt+Del was not sent: the system policy SoftwareSASGeneration does not let "
                         "services simulate it (1 or 3 under HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
                         "\\Policies\\System allows it; the SDK never sets it)");
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
        return sendSas();
    }
    if (const HANDLE end = relayEnd()) {
        // The supervisor, a service in session 0, sends it to this process's session, or says why not.
        return relaySas(end);
    }
    log::warning("input", "Ctrl+Alt+Del was not sent: only a service in session 0 can send it; a helper process "
                          "asks the rcs_supervisor that started it, and this process was not started by one");
    return RCS_ERROR_UNSUPPORTED;
}

} // namespace rcsdk::platform
