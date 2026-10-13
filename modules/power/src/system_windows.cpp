// Windows: restarting, shutting down and sleeping with the shutdown privilege
// (InitiateShutdownW, SetSuspendState), signing out the process's own Windows
// session (WTSLogoffSession, ExitWindowsEx), and the interfaces that can wake
// the machine (GetAdaptersAddresses; whether Device Manager lets each device
// wake it, from the power device list, as `powercfg /devicequery
// wake_armed` reads it).
#include "system.hpp"

#include "messages.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
// After winsock2.h:
#include <windows.h>

#include <iphlpapi.h>
#include <netioapi.h>
#include <powrprof.h>
#include <wtsapi32.h>

#include <algorithm>
#include <set>

#ifndef DEVICEPOWER_FILTER_WAKEPROGRAMMABLE // Windows SDK's powrprof.h has it; MinGW's does not.
#define DEVICEPOWER_FILTER_WAKEPROGRAMMABLE 0x04000000
#endif

namespace rcsdk::power {
namespace {

std::string errorText(DWORD error) {
    char* text = nullptr;
    const DWORD length =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = length ? std::string(text, length) : "error " + std::to_string(error);
    LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
}

std::string utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size > 0 ? size - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

class Token {
public:
    explicit Token(DWORD access) {
        if (!OpenProcessToken(GetCurrentProcess(), access, &handle_)) {
            handle_ = nullptr;
        }
    }
    ~Token() {
        if (handle_) {
            CloseHandle(handle_);
        }
    }
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

// Whether this process's token holds the shutdown privilege (enabled or not).
bool holdsShutdownPrivilege() {
    LUID shutdown{};
    const Token token(TOKEN_QUERY);
    if (!token.get() || !LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &shutdown)) {
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenPrivileges, nullptr, 0, &size);
    std::vector<uint8_t> buffer(size);
    if (!size || !GetTokenInformation(token.get(), TokenPrivileges, buffer.data(), size, &size)) {
        return false;
    }
    const auto* privileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(buffer.data());
    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
        const LUID& luid = privileges->Privileges[i].Luid;
        if (luid.LowPart == shutdown.LowPart && luid.HighPart == shutdown.HighPart) {
            return true;
        }
    }
    return false;
}

void enableShutdownPrivilege() {
    const Token token(TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY);
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!token.get() || !LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &privileges.Privileges[0].Luid) ||
        !AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr) ||
        GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        throw Refused(RCS_ERROR_UNSUPPORTED,
                      "This process's account has no shutdown privilege (SeShutdownPrivilege) to enable");
    }
}

bool runsAsLocalSystem() {
    const Token token(TOKEN_QUERY);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> buffer(size);
    if (!token.get() || !size || !GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
        return false;
    }
    return IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid, WinLocalSystemSid) != 0;
}

DWORD ownSession() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return session;
}

// Whether the machine has a sleep state a program can put it in (S1..S3):
// a Modern Standby machine (S0 low power idle) has none.
bool canSleep() {
    SYSTEM_POWER_CAPABILITIES capabilities{};
    return GetPwrCapabilities(&capabilities) &&
           (capabilities.SystemS1 || capabilities.SystemS2 || capabilities.SystemS3);
}

constexpr char noPrivilege[] = "This process's account has no shutdown privilege (SeShutdownPrivilege; LocalSystem "
                               "has it, as do signed-in users on a workstation)";
constexpr char noSleep[] = "This machine has no sleep state a program can put it in (S1..S3; a Modern Standby "
                           "machine sleeps when its display goes off)";
constexpr char sessionZero[] = "This process runs in session 0, which no user signs in to";
constexpr char systemSignsOut[] = "Without RCS_POWER_FORCE Windows signs a user out from that user's own process; "
                                  "this one runs as LocalSystem";

// The names of the devices present that Device Manager lets wake the machine
// (wake_armed), or that could be let (wake_programmable).
std::set<std::wstring> wakeDevices(ULONG filter) {
    std::set<std::wstring> names;
    for (ULONG index = 0;; ++index) {
        wchar_t name[512] = {};
        ULONG size = sizeof(name);
        if (!DevicePowerEnumDevices(index, DEVICEPOWER_FILTER_DEVICES_PRESENT | filter,
                                    PDCAP_WAKE_FROM_D0_SUPPORTED | PDCAP_WAKE_FROM_D1_SUPPORTED |
                                        PDCAP_WAKE_FROM_D2_SUPPORTED | PDCAP_WAKE_FROM_D3_SUPPORTED,
                                    reinterpret_cast<PBYTE>(name), &size)) {
            return names;
        }
        names.insert(name);
    }
}

class WindowsMachine final : public Machine {
public:
    Abilities abilities() override {
        Abilities a;
        std::string why;
        for (const rcs_power_action action :
             {RCS_POWER_RESTART, RCS_POWER_SHUT_DOWN, RCS_POWER_SLEEP, RCS_POWER_SIGN_OUT}) {
            const std::string problem = problemWith(action, RCS_POWER_FORCE);
            if (problem.empty()) {
                a.actions |= 1u << action;
            } else if (why.find(problem) == std::string::npos) {
                why += (why.empty() ? "" : "; ") + problem;
            }
        }
        a.why = why;
        return a;
    }

    void check(rcs_power_action action, uint32_t flags) override {
        if (const std::string problem = problemWith(action, flags); !problem.empty()) {
            throw Refused(RCS_ERROR_UNSUPPORTED, problem);
        }
    }

    void act(rcs_power_action action, uint32_t flags) override {
        const bool force = flags & RCS_POWER_FORCE;
        const DWORD reason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
        switch (action) {
        case RCS_POWER_RESTART:
        case RCS_POWER_SHUT_DOWN: {
            enableShutdownPrivilege();
            // A full shutdown, not Fast Startup's (SHUTDOWN_HYBRID): a machine
            // switched off fully is the one network cards wake.
            DWORD how = action == RCS_POWER_RESTART ? SHUTDOWN_RESTART : SHUTDOWN_POWEROFF;
            if (force) {
                how |= SHUTDOWN_FORCE_OTHERS | SHUTDOWN_FORCE_SELF;
            }
            if (const DWORD error = InitiateShutdownW(nullptr, nullptr, 0, how, reason); error != ERROR_SUCCESS) {
                throw Refused(RCS_ERROR_INTERNAL, "Windows did not start to " + std::string(actionName(action)) +
                                                      " (" + errorText(error) + ")");
            }
            return;
        }
        case RCS_POWER_SLEEP:
            enableShutdownPrivilege();
            if (!SetSuspendState(FALSE, FALSE, FALSE)) {
                throw Refused(RCS_ERROR_INTERNAL, "Windows did not go to sleep (" + errorText(GetLastError()) + ")");
            }
            return;
        case RCS_POWER_SIGN_OUT:
            if (force ? !WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, FALSE)
                      : !ExitWindowsEx(EWX_LOGOFF, reason)) {
                throw Refused(RCS_ERROR_INTERNAL, "Windows did not sign the user out (" + errorText(GetLastError()) +
                                                      ")");
            }
            return;
        default:
            throw Refused(RCS_ERROR_INVALID_ARGUMENT, "An unknown action");
        }
    }

    std::vector<rcs_power_interface> interfaces() override {
        ULONG size = 16 * 1024;
        std::vector<uint8_t> buffer;
        ULONG result = ERROR_BUFFER_OVERFLOW;
        for (int tries = 0; result == ERROR_BUFFER_OVERFLOW && tries < 4; ++tries) {
            buffer.resize(size);
            result = GetAdaptersAddresses(AF_INET,
                                          GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                          nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        }
        if (result == ERROR_NO_DATA) {
            return {};
        }
        if (result != ERROR_SUCCESS) {
            throw Refused(RCS_ERROR_INTERNAL, "Windows did not list the network adapters (" + errorText(result) + ")");
        }
        DevicePowerOpen(0);
        const auto armed = wakeDevices(DEVICEPOWER_FILTER_WAKEENABLED);
        const auto programmable = wakeDevices(DEVICEPOWER_FILTER_WAKEPROGRAMMABLE);
        DevicePowerClose();
        std::vector<rcs_power_interface> list;
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->PhysicalAddressLength != 6 ||
                (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211)) {
                continue;
            }
            MIB_IF_ROW2 row{};
            row.InterfaceLuid = a->Luid;
            if (GetIfEntry2(&row) != NO_ERROR || !row.InterfaceAndOperStatusFlags.HardwareInterface) {
                continue; // A virtual adapter (Hyper-V's, a VPN's).
            }
            const sockaddr_in* in = nullptr;
            uint32_t prefix = 0;
            for (auto* u = a->FirstUnicastAddress; u && !in; u = u->Next) {
                if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
                    in = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                    prefix = std::min<uint32_t>(u->OnLinkPrefixLength, 32);
                }
            }
            if (!in) {
                continue;
            }
            const std::wstring device = a->Description ? a->Description : L"";
            const rcs_power_wake_setting wake = armed.count(device)          ? RCS_POWER_WAKE_ENABLED
                                                : programmable.count(device) ? RCS_POWER_WAKE_DISABLED
                                                                             : RCS_POWER_WAKE_UNKNOWN;
            uint8_t mac[6];
            std::copy(a->PhysicalAddress, a->PhysicalAddress + 6, mac);
            list.push_back(makeInterface(mac,
                                         a->IfType == IF_TYPE_IEEE80211 ? RCS_POWER_LINK_WIFI
                                                                        : RCS_POWER_LINK_ETHERNET,
                                         wake, ntohl(in->sin_addr.s_addr), prefix,
                                         utf8(a->FriendlyName ? a->FriendlyName : L"")));
        }
        std::stable_sort(list.begin(), list.end(), [](const rcs_power_interface& x, const rcs_power_interface& y) {
            return x.link < y.link;
        });
        return list;
    }

    std::string noInterfaces() override { return {}; }

private:
    // Why this process cannot carry out `action` with `flags`; empty when it can.
    static std::string problemWith(rcs_power_action action, uint32_t flags) {
        switch (action) {
        case RCS_POWER_RESTART:
        case RCS_POWER_SHUT_DOWN:
            return holdsShutdownPrivilege() ? std::string() : noPrivilege;
        case RCS_POWER_SLEEP:
            return !holdsShutdownPrivilege() ? noPrivilege : !canSleep() ? noSleep : std::string();
        case RCS_POWER_SIGN_OUT:
            return ownSession() == 0                                      ? sessionZero
                   : !(flags & RCS_POWER_FORCE) && runsAsLocalSystem() ? systemSignsOut
                                                                          : std::string();
        default:
            return "An unknown action";
        }
    }
};

} // namespace

std::shared_ptr<Machine> systemMachine() {
    return std::make_shared<WindowsMachine>();
}

} // namespace rcsdk::power
