// A Windows service that shares the machine around the clock, from the sign-in
// screen on: a test program for unattended access (rcs_supervisor_* of
// RemoteControl::Supervisor, rcs_capture_config.secure_desktop, rcs_agent_*). It is not part of the SDK
// and stands for no product; a host writes its own service, with its own
// installer, key storage and consent.
//
// Two ways to split the work between the service (session 0, no desktop) and a
// helper the supervisor keeps running in the console session:
//   a  (default) The service only supervises. The helper IS the sharing
//      session. When the console moves to another session (sign-out, fast user
//      switching) the helper restarts there; a viewer is told ("restarting")
//      and reconnects by itself.
//   b  The service holds the sharing session, and the helper is its agent
//      (capture, encoding, input). The viewer stays connected when the
//      helper moves.
//
// Only the service reads files with keys: it checks that SYSTEM and
// administrators alone can change them (and the program itself), and hands a
// helper what it needs through the supervisor (rcs_helper_get_key): mode a the
// connection key, mode b a link key made anew for each run of the service.
//
//   rcsdk_unattended install   [--mode a|b] [--port N] [--os-session N] [--auto-start]   (administrator)
//                              [--power-actions LIST] [--share-wake-info]
//   rcsdk_unattended uninstall
//   rcsdk_unattended service   [--console]    what the service runs; --console: in the foreground
//   rcsdk_unattended session | agent          the helpers, started by the supervisor
//   rcsdk_unattended sessions                 the Windows sessions a helper can run in
#include <rcsdk/agent.hpp>
#include <rcsdk/power.h>
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/supervisor.hpp>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

struct Options {
    std::string command;
    char mode = 'a';
    std::string address = "0.0.0.0";
    uint32_t port = 47995;
    fs::path directory;
    std::string endpoint = "rcsdk-unattended-agent";
    rcs_helper_account account = RCS_HELPER_SYSTEM;
    uint32_t osSession = 0; // Where the helper runs: 0 follows the console.
    std::string serviceName = "RcsdkUnattended";
    rcs_keep_awake keepAwake = RCS_KEEP_AWAKE_SYSTEM;
    uint32_t powerActions = 0; // What viewers may ask the machine to do (rcs_power_settings.actions).
    bool console = false, autoStart = false, secureDesktop = true, shareWakeInfo = false;

    fs::path key() const { return directory / "connection.key"; }
};

std::atomic<bool> interrupted{false};

fs::path defaultDirectory() {
    wchar_t data[MAX_PATH]{};
    const DWORD size = GetEnvironmentVariableW(L"ProgramData", data, MAX_PATH);
    return fs::path(size && size < MAX_PATH ? data : L"C:\\ProgramData") / "RcsdkUnattended";
}

std::string utf8(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size_t(size > 0 ? size - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(size_t(size > 0 ? size - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    return result;
}

std::string quoted(const std::wstring& text) {
    return "\"" + utf8(text) + "\"";
}

fs::path selfPath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (size == 0) {
            throw std::runtime_error("This program's path cannot be read (Windows error " +
                                     std::to_string(GetLastError()) + ")");
        }
        if (size < path.size()) {
            path.resize(size);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// ---- Options --------------------------------------------------------------------------------

// --power-actions: restart, shut-down, sleep, sign-out (comma-separated), all or none.
uint32_t powerActions(const std::string& list) {
    if (list == "all") {
        return RCS_POWER_ALL_ACTIONS;
    }
    const char* names[] = {"", "restart", "shut-down", "sleep", "sign-out"};
    uint32_t bits = 0;
    size_t start = 0;
    while (list != "none" && start <= list.size()) {
        const size_t end = std::min(list.find(',', start), list.size());
        const std::string name = list.substr(start, end - start);
        uint32_t action = RCS_POWER_RESTART;
        while (action <= RCS_POWER_SIGN_OUT && name != names[action]) {
            ++action;
        }
        if (action > RCS_POWER_SIGN_OUT) {
            throw std::invalid_argument("--power-actions takes restart, shut-down, sleep, sign-out, all or none");
        }
        bits |= 1u << action;
        start = end + 1;
    }
    return bits;
}

std::string powerActionList(uint32_t bits) {
    if (bits == RCS_POWER_ALL_ACTIONS) {
        return "all";
    }
    const char* names[] = {"", "restart", "shut-down", "sleep", "sign-out"};
    std::string list;
    for (uint32_t action = RCS_POWER_RESTART; action <= RCS_POWER_SIGN_OUT; ++action) {
        if (bits & (1u << action)) {
            list += std::string(list.empty() ? "" : ",") + names[action];
        }
    }
    return list.empty() ? "none" : list;
}

void logLine(const std::string& text);

// The power module's steps, for the log.
void logPower(const rcs_event& event) {
    const auto& m = event.module;
    if (std::strcmp(m.module, RCS_POWER_MODULE) != 0 || m.kind != RCS_POWER_ACTION ||
        m.size != sizeof(rcs_power_action_event)) {
        return;
    }
    const auto& a = *static_cast<const rcs_power_action_event*>(m.payload);
    const char* actions[] = {"?", "restart", "shut down", "sleep", "sign out"};
    const char* states[] = {"?", "refused", "accepted", "started", "failed"};
    logLine("viewer " + std::to_string(event.viewer_id) + " asked to " + actions[a.action <= 4 ? a.action : 0] +
            (a.flags & RCS_POWER_FORCE ? " (force)" : "") + ": " + states[a.state <= 4 ? a.state : 0] +
            (a.result == RCS_OK ? std::string() : std::string(" (") + a.message + ")"));
}

[[noreturn]] void usage(int code) {
    std::cout
        << "rcsdk_unattended install   [--mode a|b] [--port N] [--address A] [--account system|user]\n"
           "                           [--os-session N] [--keep-awake none|system|display] [--no-secure-desktop]\n"
           "                           [--auto-start] [--name NAME] [--dir DIR]  register the service (administrator)\n"
           "                           [--power-actions restart,shut-down,sleep,sign-out|all] [--share-wake-info]\n"
           "rcsdk_unattended uninstall [--name NAME]                   stop and remove it\n"
           "rcsdk_unattended service   [same options] [--console]      what the service runs (--console: in the\n"
           "                                                           foreground, e.g. as a SYSTEM scheduled task)\n"
           "rcsdk_unattended session   [same options]                  mode a's helper: the sharing session\n"
           "rcsdk_unattended agent     [same options]                  mode b's helper: the agent\n"
           "rcsdk_unattended sessions                                  the Windows sessions (SYSTEM sees every\n"
           "                                                           user's in full; others may not)\n"
           "\n"
           "mode a (default): the service supervises; the helper is the session. mode b: the service holds the\n"
           "session; the helper is its agent. --account user runs the helper as the signed-in user (with\n"
           "--no-secure-desktop: only LocalSystem follows secure desktops). --os-session N keeps the helper in\n"
           "session N while it is shown (e.g. a Remote Desktop session; see sessions), not the console's.\n"
           "The program and its directory must be\n"
           "writable by SYSTEM, administrators and TrustedInstaller only (e.g. under Program Files). Keys and logs\n"
           "live in --dir (default %ProgramData%\\RcsdkUnattended), which the service creates for SYSTEM and\n"
           "administrators only: connection.key is the one a viewer needs (rcsdk_cli --role view --key-file ...).\n"
           "A helper running as the user logs to %LOCALAPPDATA%\\RcsdkUnattended.\n"
           "--power-actions lets a viewer holding control restart, shut down, sleep or sign out this machine\n"
           "(for real, 3 s after the answer); --share-wake-info tells viewers its MAC addresses for Wake-on-LAN.\n"
           "For the service to come back after a restart, install it with --auto-start.\n";
    std::exit(code);
}

Options parse(int argc, char** argv) {
    if (argc < 2) {
        usage(2);
    }
    Options o;
    o.command = argv[1];
    o.directory = defaultDirectory();
    std::map<std::string, std::string> values;
    for (int i = 2; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--console") {
            o.console = true;
        } else if (name == "--auto-start") {
            o.autoStart = true;
        } else if (name == "--no-secure-desktop") {
            o.secureDesktop = false;
        } else if (name == "--share-wake-info") {
            o.shareWakeInfo = true;
        } else if (name.rfind("--", 0) == 0 && i + 1 < argc) {
            values[name] = argv[++i];
        } else {
            std::cerr << "Unexpected argument: " << name << "\n";
            usage(2);
        }
    }
    const auto get = [&](const char* name, const std::string& fallback) {
        const auto found = values.find(name);
        return found == values.end() ? fallback : found->second;
    };
    const std::string mode = get("--mode", "a");
    if (mode != "a" && mode != "b") {
        std::cerr << "--mode takes a or b\n";
        usage(2);
    }
    o.mode = mode[0];
    o.address = get("--address", o.address);
    o.port = uint32_t(std::stoul(get("--port", std::to_string(o.port))));
    o.directory = fs::path(wide(get("--dir", utf8(o.directory.wstring()))));
    o.endpoint = get("--endpoint", o.endpoint);
    o.serviceName = get("--name", o.serviceName);
    o.osSession = uint32_t(std::stoul(get("--os-session", "0")));
    const std::string account = get("--account", "system");
    if (account != "system" && account != "user") {
        std::cerr << "--account takes system or user\n";
        usage(2);
    }
    o.account = account == "user" ? RCS_HELPER_USER : RCS_HELPER_SYSTEM;
    if (o.account == RCS_HELPER_USER && o.secureDesktop) {
        std::cerr << "--account user needs --no-secure-desktop: only a LocalSystem helper follows secure desktops\n";
        usage(2);
    }
    const std::string awake = get("--keep-awake", "system");
    if (awake != "none" && awake != "system" && awake != "display") {
        std::cerr << "--keep-awake takes none, system or display\n";
        usage(2);
    }
    o.keepAwake = awake == "display"  ? RCS_KEEP_AWAKE_DISPLAY
                  : awake == "system" ? RCS_KEEP_AWAKE_SYSTEM
                                      : RCS_KEEP_AWAKE_OFF;
    try {
        o.powerActions = powerActions(get("--power-actions", "none"));
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << "\n";
        usage(2);
    }
    return o;
}

// What the service's helpers and the installed service are given.
std::string optionArguments(const Options& o) {
    std::string text = " --mode " + std::string(1, o.mode) + " --address " + o.address + " --port " +
                       std::to_string(o.port) + " --dir " + quoted(o.directory.wstring()) + " --endpoint " +
                       o.endpoint + " --account " + (o.account == RCS_HELPER_USER ? "user" : "system") +
                       " --keep-awake " +
                       (o.keepAwake == RCS_KEEP_AWAKE_DISPLAY  ? "display"
                        : o.keepAwake == RCS_KEEP_AWAKE_SYSTEM ? "system"
                                                               : "none");
    if (!o.secureDesktop) {
        text += " --no-secure-desktop";
    }
    if (o.osSession) {
        text += " --os-session " + std::to_string(o.osSession);
    }
    text += " --power-actions " + powerActionList(o.powerActions);
    if (o.shareWakeInfo) {
        text += " --share-wake-info";
    }
    return text;
}

// ---- Who may change what --------------------------------------------------------------------

// The accounts that may change the program and the keys: LocalSystem,
// administrators and (the program, e.g. under Program Files) TrustedInstaller.
bool trustedWriter(PSID sid, bool trustedInstaller) {
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) {
        return true;
    }
    if (!trustedInstaller) {
        return false;
    }
    PSID installer = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &installer)) {
        return false;
    }
    const bool same = EqualSid(sid, installer) != 0;
    LocalFree(installer);
    return same;
}

std::string accountName(PSID sid) {
    wchar_t name[256]{}, domain[256]{};
    DWORD nameSize = 256, domainSize = 256;
    SID_NAME_USE use{};
    if (LookupAccountSidW(nullptr, sid, name, &nameSize, domain, &domainSize, &use)) {
        return utf8(std::wstring(domain) + L"\\" + name);
    }
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(sid, &text)) {
        const std::string result = utf8(text);
        LocalFree(text);
        return result;
    }
    return "an unknown account";
}

// Why `path` is not protected as `what` must be, or empty when it is: its
// owner and every account its access list lets in must be trusted. With
// `onlyWrites`, accounts that may only read and run it are fine (the program).
std::string protectionProblem(const fs::path& path, bool onlyWrites) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status =
        GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &owner, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        return "its access cannot be read (Windows error " + std::to_string(status) + ")";
    }
    const std::unique_ptr<void, decltype(&LocalFree)> owned(descriptor, &LocalFree);
    if (!owner || !trustedWriter(owner, onlyWrites)) {
        return "it belongs to " + (owner ? accountName(owner) : std::string("nobody"));
    }
    if (!dacl) {
        return "it has no access list: everyone may change it";
    }
    constexpr ACCESS_MASK writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD |
                                   FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE |
                                   GENERIC_ALL | MAXIMUM_ALLOWED;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER* header = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<void**>(&header))) {
            return "its access list cannot be read";
        }
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            continue;
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            return "its access list has an entry of a kind this program does not check";
        }
        const auto* ace = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(header);
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (onlyWrites && (header->AceFlags & INHERIT_ONLY_ACE)) {
            continue; // For what is created inside it, not for it.
        }
        if (!trustedWriter(sid, onlyWrites) && (!onlyWrites || (ace->Mask & writes))) {
            return "it lets " + accountName(sid) + (onlyWrites ? " change it" : " in");
        }
    }
    return {};
}

void requireProtected(const fs::path& path, bool onlyWrites, const char* what) {
    if (const auto problem = protectionProblem(path, onlyWrites); !problem.empty()) {
        throw std::runtime_error(std::string(what) + " " + utf8(path.wstring()) + " is not protected: " + problem +
                                 (onlyWrites ? ". Install the program where only SYSTEM, administrators and "
                                               "TrustedInstaller can change it, e.g. under Program Files."
                                             : ". Delete it as an administrator; this program creates it for SYSTEM "
                                               "and administrators only."));
    }
}

// The program the supervisor starts with LocalSystem's rights, and the
// directory it is loaded from: nobody else may change either.
void requireProtectedProgram() {
    const fs::path self = selfPath();
    requireProtected(self, true, "The program");
    requireProtected(self.parent_path(), true, "The program's directory");
}

constexpr const wchar_t* protectedAccess = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

// The directory of keys and logs: created for SYSTEM and administrators only,
// or, when it exists, checked to be so (a user who made it first could
// otherwise plant keys or read them).
void ensureDirectory(const Options& o) {
    if (!fs::exists(o.directory)) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(protectedAccess, SDDL_REVISION_1, &descriptor,
                                                                  nullptr)) {
            throw std::runtime_error("Could not build the directory's access control");
        }
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        const BOOL created = CreateDirectoryW(o.directory.c_str(), &attributes);
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        if (!created && error != ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("Could not create " + utf8(o.directory.wstring()) + " (Windows error " +
                                     std::to_string(error) + (error == ERROR_INVALID_OWNER
                                                                  ? ": run this as an administrator or as SYSTEM"
                                                                  : error == ERROR_PATH_NOT_FOUND
                                                                        ? ": its parent does not exist"
                                                                        : "") +
                                     ")");
        }
    }
    requireProtected(o.directory, false, "The directory");
}

// A key file only LocalSystem and administrators can read, created that way.
void createKeyFile(const fs::path& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1,
                                                              &descriptor, nullptr)) {
        throw std::runtime_error("Could not build the key file's access control");
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD error = GetLastError();
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Could not create " + utf8(path.wstring()) + " (Windows error " +
                                 std::to_string(error) + ")");
    }
    const auto key = rcsdk::generateConnectionKey();
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, key.data(), DWORD(key.size()), &written, nullptr);
    CloseHandle(file);
    if (!wrote || written != key.size()) {
        DeleteFileW(path.c_str());
        throw std::runtime_error("Could not write " + utf8(path.wstring()));
    }
}

void ensureKeys(const Options& o) {
    ensureDirectory(o);
    if (!fs::exists(o.key())) {
        createKeyFile(o.key());
        std::cout << "Created " << utf8(o.key().wstring()) << "\n";
    }
}

rcsdk::ConnectionKey readKey(const fs::path& path) {
    requireProtected(path, false, "The key");
    rcsdk::ConnectionKey key{};
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(key.data()), key.size())) {
        throw std::runtime_error("Cannot read the key " + utf8(path.wstring()) + " (create it with: install)");
    }
    return key;
}

// ---- Logs ------------------------------------------------------------------------------------

// The SDK's log, and what this program does, in one file per role.
std::mutex logMutex;
std::ofstream logFile;

void logLine(const std::string& text) {
    std::lock_guard lock(logMutex);
    if (!logFile.is_open()) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    logFile << stamp << ' ' << text << std::endl;
}

// The service and a LocalSystem helper log to the directory of keys and logs
// (protected); a helper running as the user may not write there, and logs to
// its own %LOCALAPPDATA%.
void openLog(const Options& o, const char* name, bool asUser) {
    fs::path directory = o.directory;
    if (asUser) {
        wchar_t local[MAX_PATH]{};
        const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (!size || size >= MAX_PATH) {
            return; // No log, then.
        }
        directory = fs::path(local) / "RcsdkUnattended";
        std::error_code ignored;
        fs::create_directories(directory, ignored);
    }
    logFile.open(directory / name, std::ios::app);
    rcsdk::setLogCallback(RCS_LOG_INFO, [](const rcs_log_record& record) {
        static const char* levels[] = {"", "error", "warning", "info", "debug"};
        static const char* sources[] = {"process", "session", "agent", "supervisor"};
        logLine(std::string("[") + levels[record.level] + "] " + sources[record.source] + " " + record.module + ": " +
                record.message);
    });
    logLine(std::string("--- ") + name + " started (process " + std::to_string(GetCurrentProcessId()) + ") ---");
}

// ---- The helpers -----------------------------------------------------------------------------

// Waits until the supervisor asks this helper to end (or, run by hand, Ctrl+C).
// Returns false while it should go on; `tick` runs every 200 ms.
template <class Tick>
void untilStopped(Tick&& tick) {
    bool supervised = true;
    while (!interrupted) {
        if (supervised) {
            try {
                if (rcsdk::waitForStopRequest(200)) {
                    logLine("The supervisor asked this helper to end");
                    return;
                }
            } catch (const rcsdk::Error&) { // Started by hand, not by a supervisor.
                supervised = false;
            }
        } else {
            std::this_thread::sleep_for(200ms);
        }
        if (!tick()) {
            return;
        }
    }
}

rcsdk::SessionConfig sessionConfig(const Options& o, const rcsdk::ConnectionKey& key) {
    rcsdk::SessionConfig config(RCS_ROLE_SHARE);
    config.setAddress(o.address, o.port).setKey(key).setControl(true).setSecureDesktop(o.secureDesktop);
    if (o.keepAwake != RCS_KEEP_AWAKE_OFF || o.powerActions || o.shareWakeInfo) {
        // Reachable at any hour, not only while someone is connected: the agent's machine holds the request.
        rcs_power_settings power = RCS_POWER_SETTINGS_INIT;
        power.keep_awake = o.keepAwake;
        power.while_running = 1;
        // A viewer holding control may restart (...) the machine; the helper carries it out (SYSTEM, or the
        // signed-in user: both have the shutdown privilege). After a restart the service starts again
        // (--auto-start) and so does the helper: the session listens again.
        power.actions = o.powerActions;
        power.share_wake_info = o.shareWakeInfo ? 1 : 0;
        config.useModule(RCS_POWER_MODULE, power);
    }
    return config;
}

rcsdk::Callbacks sessionCallbacks() {
    rcsdk::Callbacks callbacks;
    callbacks.onEvent = [](const rcs_event& event) {
        switch (event.type) {
        case RCS_EVENT_STATE:
            logLine("state " + std::to_string(event.state.value) + " (" + rcs_result_message(event.state.result) + ")");
            break;
        case RCS_EVENT_PEER:
            logLine("a viewer connected" + std::string(event.peer.has_public_key ? " (with a device key)" : ""));
            break;
        case RCS_EVENT_DESKTOP:
            logLine(std::string("the machine shows ") +
                    (event.desktop.kind == RCS_DESKTOP_USER ? "the user's desktop"
                     : event.desktop.kind == RCS_DESKTOP_SECURE
                         ? (event.desktop.user_logged_on ? "a lock screen or prompt" : "the sign-in screen")
                         : "an unknown desktop"));
            break;
        case RCS_EVENT_CAPTURE:
            logLine(event.capture.paused ? "capture paused (reason " + std::to_string(event.capture.paused) + ")"
                                         : "capture resumed");
            break;
        case RCS_EVENT_MODULE:
            logPower(event);
            break;
        case RCS_EVENT_AGENT:
            logLine(std::string("agent ") + (event.agent.attached ? "attached" : "left") + " (process " +
                    std::to_string(event.agent.process_id) + ", session " + std::to_string(event.agent.os_session) +
                    ", " + event.agent.account + ")" +
                    (event.agent.attached ? std::string()
                                          : std::string(": ") + rcs_result_message(event.agent.result) + " " +
                                                event.agent.message));
            break;
        default:
            break;
        }
    };
    return callbacks;
}

// Mode b: the session takes only the agent the supervisor started now.
class SupervisorGate final : public rcsdk::AgentGate {
public:
    explicit SupervisorGate(const rcsdk::Supervisor& supervisor) : supervisor_(supervisor) {}

    bool attach(const rcs_agent_info& agent) override {
        const auto status = supervisor_.status();
        const bool ours = status.helper_running && status.process_id == agent.process_id;
        if (!ours) {
            logLine("refused an agent: process " + std::to_string(agent.process_id) + " (" + agent.account +
                    ") is not the supervisor's helper");
        }
        return ours;
    }

private:
    const rcsdk::Supervisor& supervisor_;
};

// What a viewer's input comes to, every 5 s while one is sending.
void logInput(rcsdk::Session& session, std::chrono::steady_clock::time_point& next) {
    if (std::chrono::steady_clock::now() < next) {
        return;
    }
    next += 5s;
    const auto input = session.stats<rcs_input_stats>();
    if (input.received) {
        logLine("input: " + std::to_string(input.received) + " received, " + std::to_string(input.applied) +
                " applied, last result " + rcs_result_message(input.last_result));
    }
}

// Mode a's helper: the sharing session itself, with the connection key the
// supervisor handed it.
int runSession(const Options& o) {
    openLog(o, "session.log", o.account == RCS_HELPER_USER);
    try {
        rcsdk::Session session(sessionConfig(o, rcsdk::helperKey()), sessionCallbacks());
        if (const auto result = session.start(); result != RCS_OK) {
            logLine(std::string("start failed: ") + rcs_result_message(result) + ": " + session.failure().message);
            return 1;
        }
        int code = 0;
        auto nextStats = std::chrono::steady_clock::now();
        untilStopped([&] {
            if (session.state() == RCS_STATE_FAILED) {
                logLine("the session failed: " + session.failure().message);
                code = 1;
                return false;
            }
            logInput(session, nextStats);
            return true;
        });
        // Whoever is connected hears that this is a restart, not the end, and reconnects.
        session.stop(RCS_STOP_RESTART);
        return code;
    } catch (const std::exception& error) {
        logLine(std::string("error: ") + error.what());
        return 1;
    }
}

// Mode b's helper: the agent of the session the service holds, with the link
// key the supervisor handed it.
int runAgent(const Options& o) {
    openLog(o, "agent.log", o.account == RCS_HELPER_USER);
    try {
        rcsdk::Agent agent(rcsdk::AgentConfig(o.endpoint, rcsdk::helperKey()), [](const rcs_agent_event& event) {
            logLine("agent state " + std::to_string(event.state.value) + " (" + rcs_result_message(event.state.result) +
                    ")");
        });
        if (const auto result = agent.start(); result != RCS_OK) {
            logLine(std::string("start failed: ") + rcs_result_message(result));
            return 1;
        }
        int code = 0;
        untilStopped([&] {
            const auto state = agent.state();
            if (state == RCS_STATE_FAILED || state == RCS_STATE_STOPPED) { // Another version; another agent took over.
                logLine("the agent ended: " + agent.failure().message);
                code = state == RCS_STATE_FAILED;
                return false;
            }
            return true;
        });
        agent.stop();
        return code;
    } catch (const std::exception& error) {
        logLine(std::string("error: ") + error.what());
        return 1;
    }
}

// ---- The service -----------------------------------------------------------------------------

// What the service does until `stop` is set: supervise the helper, and in mode b hold the session.
int serviceBody(const Options& o, HANDLE stop) {
    try {
        ensureKeys(o); // Run without install (e.g. as a SYSTEM scheduled task), the key is made here.
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n"; // Nowhere safe to log yet.
        return 1;
    }
    openLog(o, "service.log", false);
    try {
        requireProtectedProgram();
        const rcsdk::ConnectionKey connectionKey = readKey(o.key());
        const std::string exe = quoted(selfPath().wstring());
        const std::string helper = exe + (o.mode == 'a' ? " session" : " agent") + optionArguments(o);
        // Mode a hands the helper the connection key; mode b a link key made for this run only, so no
        // key of the link is ever stored.
        const rcsdk::ConnectionKey helperKey = o.mode == 'a' ? connectionKey : rcsdk::generateConnectionKey();
        auto supervisorConfig = rcsdk::supervisorConfig(helper, o.account, helperKey);
        supervisorConfig.os_session = o.osSession;
        rcsdk::Supervisor supervisor(supervisorConfig, [](const rcs_supervisor_event& e) {
            static const char* names[] = {
                "", "no session", "helper started", "helper exited", "helper failed", "supervisor failed"};
            logLine(std::string("supervisor: ") + names[e.type < 6 ? e.type : 0] + " (session " +
                    std::to_string(e.os_session) + ", process " + std::to_string(e.process_id) + ") " + e.message);
        });
        SupervisorGate gate(supervisor);
        std::unique_ptr<rcsdk::Session> hub;
        if (o.mode == 'b') {
            auto config = sessionConfig(o, connectionKey);
            // A helper running as the user must be able to open the endpoint; which process may attach is
            // decided by the gate either way: the one the supervisor started now.
            config.use(rcsdk::agentLink(o.endpoint, helperKey,
                                        o.account == RCS_HELPER_USER ? RCS_AGENT_ACCESS_ANY_USER
                                                                     : RCS_AGENT_ACCESS_PRIVILEGED,
                                        &gate));
            hub = std::make_unique<rcsdk::Session>(config, sessionCallbacks());
            if (const auto result = hub->start(); result != RCS_OK) {
                logLine(std::string("the session could not start: ") + rcs_result_message(result) + ": " +
                        hub->failure().message);
                return 1;
            }
        }
        if (const auto result = supervisor.start(); result != RCS_OK) {
            logLine(std::string("the supervisor could not start: ") + rcs_result_message(result) + ": " +
                    rcsdk::lastCallError());
            return 1;
        }
        logLine("running in mode " + std::string(1, o.mode) + ", the helper in " +
                (o.osSession ? "session " + std::to_string(o.osSession) : std::string("the console's session")) + ": " +
                helper);
        // In the foreground (--console) the service also ends when a file `stop` appears next to the
        // keys, which only SYSTEM and administrators can create there.
        const fs::path stopFile = o.directory / "stop";
        std::error_code ignored;
        fs::remove(stopFile, ignored);
        auto nextStats = std::chrono::steady_clock::now();
        while (!interrupted && WaitForSingleObject(stop, 200) == WAIT_TIMEOUT) {
            if (o.console && fs::exists(stopFile)) {
                break;
            }
            if (hub && hub->state() == RCS_STATE_FAILED) {
                logLine("the session failed: " + hub->failure().message);
                supervisor.stop();
                return 1;
            }
            if (hub) {
                logInput(*hub, nextStats);
            }
        }
        logLine("stopping");
        supervisor.stop();
        if (hub) {
            hub->stop(RCS_STOP_RESTART); // A viewer reconnects when the service is back.
        }
        return 0;
    } catch (const std::exception& error) {
        logLine(std::string("error: ") + error.what());
        return 1;
    }
}

Options serviceOptions;
SERVICE_STATUS_HANDLE serviceHandle = nullptr;
HANDLE serviceStop = nullptr;

void reportStatus(DWORD state, DWORD exitCode = NO_ERROR) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode = exitCode;
    status.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
    SetServiceStatus(serviceHandle, &status);
}

DWORD WINAPI serviceControl(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        reportStatus(SERVICE_STOP_PENDING);
        SetEvent(serviceStop);
        return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    serviceHandle = RegisterServiceCtrlHandlerExW(L"", serviceControl, nullptr);
    serviceStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    reportStatus(SERVICE_START_PENDING);
    reportStatus(SERVICE_RUNNING);
    const int code = serviceBody(serviceOptions, serviceStop);
    reportStatus(SERVICE_STOPPED, code ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR);
}

int runService(const Options& o) {
    if (o.console) {
        HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return serviceBody(o, stop);
    }
    serviceOptions = o;
    wchar_t name[] = L"";
    SERVICE_TABLE_ENTRYW table[] = {{name, serviceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        std::cerr << "This is what the service control manager runs; to run it in the foreground use --console, to "
                     "register it use install.\n";
        return 1;
    }
    return 0;
}

// ---- Sessions --------------------------------------------------------------------------------

// What --os-session can name: Windows' sessions and what each shows now.
int listSessions() {
    static const char* states[] = {"", "active", "connected", "disconnected", "no desktop", "other"};
    for (const auto& s : rcsdk::osSessions()) {
        std::cout << "session " << s.os_session << ": " << states[s.state <= RCS_OS_SESSION_OTHER ? s.state : 0]
                  << (s.is_console ? ", console" : "") << (s.is_remote ? ", remote" : "")
                  << (s.account[0] ? std::string(", ") + s.account : std::string())
                  << (s.client_name[0] ? std::string(", client ") + s.client_name : std::string())
                  << (s.client_address[0] ? std::string(" (") + s.client_address + ")" : std::string()) << "\n";
    }
    return 0;
}

// ---- Installing ------------------------------------------------------------------------------

int install(const Options& o) {
    requireProtectedProgram();
    ensureKeys(o);
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!manager) {
        std::cerr << "Cannot open the service control manager (Windows error " << GetLastError()
                  << "); install needs an administrator.\n";
        return 1;
    }
    const std::wstring command = wide(quoted(selfPath().wstring()) + " service" + optionArguments(o));
    SC_HANDLE service =
        CreateServiceW(manager, wide(o.serviceName).c_str(), L"Remote control SDK unattended demo", SERVICE_ALL_ACCESS,
                       SERVICE_WIN32_OWN_PROCESS, o.autoStart ? SERVICE_AUTO_START : SERVICE_DEMAND_START,
                       SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!service) {
        std::cerr << "Cannot create the service (Windows error " << GetLastError() << ")\n";
        CloseServiceHandle(manager);
        return 1;
    }
    wchar_t description[] = L"Test program of the remote control SDK: shares this machine, sign-in screen included.";
    SERVICE_DESCRIPTIONW text{description};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &text);
    SC_ACTION restart{SC_ACTION_RESTART, 5000};
    SERVICE_FAILURE_ACTIONSW failure{86400, nullptr, nullptr, 1, &restart};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
    std::cout << "Installed service " << o.serviceName << " ("
              << (o.autoStart ? "starts with Windows" : "start it with: sc start " + o.serviceName) << "), mode "
              << o.mode << ".\nA viewer needs " << utf8(o.key().wstring())
              << " (on this machine only SYSTEM and administrators can read it).\n";
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return 0;
}

int uninstall(const Options& o) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service =
        manager ? OpenServiceW(manager, wide(o.serviceName).c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)
                : nullptr;
    if (!service) {
        std::cerr << "Cannot open the service " << o.serviceName << " (Windows error " << GetLastError()
                  << "); uninstall needs an administrator, and the service must exist.\n";
        if (manager) {
            CloseServiceHandle(manager);
        }
        return 1;
    }
    SERVICE_STATUS status{};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        for (int i = 0; i < 100 && QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED;
             ++i) {
            Sleep(100);
        }
    }
    const bool deleted = DeleteService(service) != 0;
    std::cout << (deleted ? "Removed " : "Could not remove ") << o.serviceName
              << (deleted ? ". The keys and logs in " + utf8(o.directory.wstring()) + " stay; delete them when done.\n"
                          : "\n");
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return deleted ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, [](int) { interrupted = true; });
    std::signal(SIGBREAK, [](int) { interrupted = true; });
    try {
        const Options options = parse(argc, argv);
        // The session's keep-awake (--keep-awake); its machine's half runs where the agent is.
        rcsdk::registerModule(*rcs_power_module());
        if (options.command == "install") {
            return install(options);
        }
        if (options.command == "uninstall") {
            return uninstall(options);
        }
        if (options.command == "service") {
            return runService(options);
        }
        if (options.command == "session") {
            return runSession(options);
        }
        if (options.command == "agent") {
            return runAgent(options);
        }
        if (options.command == "sessions") {
            return listSessions();
        }
        usage(2);
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 2;
    }
}
