// The supervisor (rcs_supervisor_*): keeps one helper process running in the
// session at the machine's console, or in one the host chose (os_session)
// while that one is shown. A service lives in session 0, which has no desktop,
// so the helper that captures the screen has to be started in a user's session
// with the rights it needs: a duplicate of the service's own LocalSystem token
// moved to that session (it can then open the secure desktops), or the
// signed-in user's token.
//
// What the helper is given: an event set when it should end, its end of a
// pipe through which it asks for Ctrl+Alt+Del, and optionally a key, through
// an inherited pipe. Their handles are in environment variables that also
// name the supervisor's process, so that a process only takes them from its
// parent (the SDK's side of it: rcs_helper_*).
#include "sessions_windows.hpp"

#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace rcsdk::supervisor {
namespace {

constexpr const wchar_t* stopVariable = L"RCSDK_SUPERVISOR_STOP";
constexpr const wchar_t* sasVariable = L"RCSDK_SUPERVISOR_SAS";
constexpr const wchar_t* keyVariable = L"RCSDK_SUPERVISOR_KEY";
constexpr DWORD noSession = 0xFFFFFFFF;
// The console has no session for a moment while it switches; a helper is
// ended for that only once it lasted this long.
constexpr auto sessionGrace = 3s;
// A helper that ends sooner than this after starting is started again after a
// longer delay each time.
constexpr auto shortLived = 30s;

std::wstring decimal(uint64_t value) {
    return std::to_wstring(value);
}

// "Session 3 is ..." as part of a sentence.
std::string lowerFirst(std::string text) {
    if (!text.empty()) {
        text[0] = char(std::tolower(uint8_t(text[0])));
    }
    return text;
}

void enablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        warning("This process's privileges cannot be adjusted (" + win::errorText(GetLastError()) + ")");
        return;
    }
    const win::Handle owned(token);
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) ||
        !AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) ||
        GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        // Starting a helper says what is missing, when it is.
        warning("The privilege " + win::narrow(name) + " could not be enabled (" + win::errorText(GetLastError()) +
                ")");
    }
}

std::unique_ptr<void, decltype(&LocalFree)> descriptorOf(const std::wstring& sddl) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        fail(RCS_ERROR_INTERNAL, "Could not build an access control (" + win::errorText(GetLastError()) + ")");
    }
    return {descriptor, &LocalFree};
}

// ---- Ctrl+Alt+Del for the helpers --------------------------------------------------------------

// The system policy that lets services simulate the secure attention sequence
// (1: services, 3: services and Ease of Access applications). Read, never set.
bool policyAllowsServices() {
    DWORD value = 0, size = sizeof(value);
    const LSTATUS status =
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                     L"SoftwareSASGeneration", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && (value == 1 || value == 3);
}

// Sends it as `helper` (a process handle the supervisor holds: never another
// process that took its number): a service's SendSAS reaches the session of
// the token it impersonates (Microsoft's SendSAS documentation), so it
// reaches the helper's session wherever the console is. The call itself
// reports nothing.
rcs_result sendSas(HANDLE helper) {
    if (!policyAllowsServices()) {
        warning("Ctrl+Alt+Del was not sent: the system policy SoftwareSASGeneration does not let services simulate "
                "it (1 or 3 under HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System allows it; "
                "the SDK never sets it)");
        return RCS_ERROR_PERMISSION_REQUIRED;
    }
    using SendSas = void(WINAPI*)(BOOL);
    static const SendSas function = [] {
        HMODULE module = LoadLibraryExW(L"sas.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<SendSas>(GetProcAddress(module, "SendSAS")) : nullptr;
    }();
    if (!function) {
        warning("Ctrl+Alt+Del was not sent: this system has no sas.dll");
        return RCS_ERROR_UNSUPPORTED;
    }
    HANDLE raw = nullptr;
    const bool opened = OpenProcessToken(helper, TOKEN_QUERY | TOKEN_DUPLICATE, &raw);
    const win::Handle token(raw);
    if (!opened || !ImpersonateLoggedOnUser(token.get())) {
        const DWORD error = GetLastError();
        warning("Ctrl+Alt+Del was not sent: it could not be sent as helper " + std::to_string(GetProcessId(helper)) +
                ", for its session (" + win::errorText(error) + ")");
        return RCS_ERROR_INTERNAL;
    }
    function(FALSE);
    RevertToSelf();
    return RCS_OK;
}

// A pipe through which a helper (in the user's session, where the secure
// attention sequence cannot be sent from) asks this process (in session 0,
// where it can) to send it: one byte in, a 32-bit result out, as often as it
// likes. Each helper has one of its own, connected before the helper starts:
// the helper inherits its end, and nothing else reaches the pipe (only
// LocalSystem and administrators may open its name, and its one instance is
// taken), so a request comes through what the supervisor gave that helper.
// Another process of the helper's account can neither ask in its place nor
// keep it from asking. The sequence goes to the session of the helper's own
// token. It ends as the helper does (its end closes), and when it goes.
class SasRelay {
public:
    SasRelay() {
        stop_ = win::Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        event_ = win::Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop_ || !event_) {
            fail(RCS_ERROR_INTERNAL, "Could not create an event (" + win::errorText(GetLastError()) + ")");
        }
        std::random_device random; // A name nobody has made first.
        const std::wstring name =
            L"\\\\.\\pipe\\rcsdk-sas-" + decimal(GetCurrentProcessId()) + L"-" + decimal(random()) + decimal(random());
        const auto descriptor = descriptorOf(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.get(), FALSE};
        const DWORD open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
        const DWORD mode = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
        pipe_ = win::Handle(CreateNamedPipeW(name.c_str(), open, mode, 1, 64, 64, 0, &attributes));
        if (!pipe_) {
            fail(RCS_ERROR_INTERNAL,
                 "Could not create the Ctrl+Alt+Del endpoint (" + win::errorText(GetLastError()) + ")");
        }
        // The helper's end, opened here: the one instance is taken from now
        // on (one an administrator took first fails it, never serves them).
        helperEnd_ =
            win::Handle(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        DWORD readMode = PIPE_READMODE_MESSAGE;
        if (!helperEnd_ || !SetNamedPipeHandleState(helperEnd_.get(), &readMode, nullptr, nullptr)) {
            fail(RCS_ERROR_INTERNAL,
                 "Could not connect the helper's Ctrl+Alt+Del endpoint (" + win::errorText(GetLastError()) + ")");
        }
        OVERLAPPED connect = overlapped(); // Connected already: it says so at once.
        if (!ConnectNamedPipe(pipe_.get(), &connect) && GetLastError() != ERROR_PIPE_CONNECTED) {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                CancelIoEx(pipe_.get(), &connect);
                DWORD ignored = 0;
                GetOverlappedResult(pipe_.get(), &connect, &ignored, TRUE);
            }
            fail(RCS_ERROR_INTERNAL,
                 "Could not connect the helper's Ctrl+Alt+Del endpoint (" + win::errorText(error) + ")");
        }
    }
    ~SasRelay() {
        SetEvent(stop_.get());
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    SasRelay(const SasRelay&) = delete;
    SasRelay& operator=(const SasRelay&) = delete;

    // The end the helper inherits.
    HANDLE helperEnd() const { return helperEnd_.get(); }
    // Serves `helper`, started with helperEnd(), from now on; this process's
    // copy of its end closes (the pipe breaks when the helper's does). False
    // when its process could not be kept (GetLastError() says why).
    bool serve(HANDLE helper) {
        HANDLE copy = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), helper, GetCurrentProcess(), &copy,
                             PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) {
            return false;
        }
        helper_ = win::Handle(copy);
        helperEnd_.reset();
        thread_ = supervisor::thread([this] { run(); });
        return true;
    }

private:
    OVERLAPPED overlapped() {
        OVERLAPPED o{};
        o.hEvent = event_.get();
        ResetEvent(event_.get());
        return o;
    }

    // Waits for an operation on the pipe, `timeout` at most; the relay going
    // ends the wait too. True when the operation completed; otherwise it is
    // cancelled.
    bool await(OVERLAPPED& o, DWORD timeout) {
        const HANDLE waits[] = {stop_.get(), o.hEvent};
        if (WaitForMultipleObjects(2, waits, FALSE, timeout) == WAIT_OBJECT_0 + 1) {
            return true;
        }
        CancelIoEx(pipe_.get(), &o);
        DWORD ignored = 0;
        GetOverlappedResult(pipe_.get(), &o, &ignored, TRUE); // A cancelled pipe operation ends at once.
        return false;
    }

    // Each request in turn, until the helper's end closes or the relay goes;
    // then the pipe is let go, so that a request after that fails at once
    // instead of waiting for an answer.
    void run() noexcept {
        serveRequests();
        DisconnectNamedPipe(pipe_.get());
    }

    void serveRequests() {
        for (;;) {
            uint8_t request[4] = {};
            DWORD size = 0;
            OVERLAPPED read = overlapped();
            if (!ReadFile(pipe_.get(), request, sizeof(request), nullptr, &read) &&
                GetLastError() != ERROR_IO_PENDING) {
                return; // Its end closed.
            }
            if (!await(read, INFINITE)) {
                return; // The relay goes.
            }
            if (!GetOverlappedResult(pipe_.get(), &read, &size, FALSE)) {
                if (GetLastError() == ERROR_MORE_DATA) {
                    warning("The helper's Ctrl+Alt+Del endpoint closes: it sent what is no request");
                }
                return;
            }
            int32_t result = RCS_ERROR_INVALID_ARGUMENT; // One byte, 'S', asks.
            if (size == 1 && request[0] == 'S') {
                info("The helper asked for Ctrl+Alt+Del");
                result = sendSas(helper_.get());
            }
            OVERLAPPED write = overlapped();
            if ((!WriteFile(pipe_.get(), &result, sizeof(result), nullptr, &write) &&
                 GetLastError() != ERROR_IO_PENDING) ||
                !await(write, 2000)) {
                return;
            }
        }
    }

    win::Handle pipe_, helperEnd_, helper_, stop_, event_;
    std::thread thread_;
};

// ---- Starting a helper ---------------------------------------------------------------------------

// What CreateProcess needs listed apart: the attributes naming the only handles
// a helper inherits.
class InheritedHandles {
public:
    explicit InheritedHandles(std::vector<HANDLE> handles) : handles_(std::move(handles)) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size); // Says the size; fails by design.
        buffer_.resize(size);
        auto* list = this->list();
        if (!size || !InitializeProcThreadAttributeList(list, 1, 0, &size)) {
            failed();
        }
        initialized_ = true;
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_.data(),
                                       handles_.size() * sizeof(HANDLE), nullptr, nullptr)) {
            failed();
        }
    }
    ~InheritedHandles() {
        if (initialized_) {
            DeleteProcThreadAttributeList(list());
        }
    }
    InheritedHandles(const InheritedHandles&) = delete;
    InheritedHandles& operator=(const InheritedHandles&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST list() { return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer_.data()); }

private:
    [[noreturn]] static void failed() {
        const DWORD error = GetLastError();
        fail(RCS_ERROR_INTERNAL, "Could not list the handles the helper inherits (" + win::errorText(error) + ")");
    }

    std::vector<HANDLE> handles_;
    std::vector<uint8_t> buffer_;
    bool initialized_ = false;
};

// The account's environment without anything a supervisor sets, then what
// this one tells the helper.
std::vector<wchar_t> environment(HANDLE token, const std::vector<std::wstring>& added) {
    LPVOID block = nullptr;
    if (!CreateEnvironmentBlock(&block, token, FALSE)) {
        fail(RCS_ERROR_INTERNAL, "The helper's environment could not be built (" + win::errorText(GetLastError()) +
                                     "); without it the helper would lack SystemRoot and PATH");
    }
    std::vector<std::wstring> entries;
    for (const wchar_t* entry = static_cast<const wchar_t*>(block); *entry; entry += wcslen(entry) + 1) {
        if (_wcsnicmp(entry, L"RCSDK_SUPERVISOR_", 17) != 0) {
            entries.emplace_back(entry);
        }
    }
    DestroyEnvironmentBlock(block);
    entries.insert(entries.end(), added.begin(), added.end());
    std::sort(entries.begin(), entries.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> result;
    for (const auto& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

// ---- The supervisor ----------------------------------------------------------------------------

class WindowsSupervisor final : public Supervisor {
public:
    WindowsSupervisor(const Config& config, Handler handler)
        : config_(config), handler_(std::move(handler)), program_(win::wide(config.program, "The program")),
          command_(win::wide(config.commandLine, "The command line")), settings_(config.settings) {
        if (!win::runningAsSystem()) { // Either account: moving a token to a session and the user's token need it.
            fail(RCS_ERROR_PERMISSION_REQUIRED,
                 "A supervisor runs in a LocalSystem service: it starts helpers in another session, as LocalSystem or "
                 "as the signed-in user, which only LocalSystem may do");
        }
        // A drive letter a network share is mapped to: whoever can write the share would run as LocalSystem.
        if (GetDriveTypeW(program_.substr(0, 3).c_str()) == DRIVE_REMOTE) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_supervisor_config: command_line: the program is on a network "
                                             "drive: a supervisor starts only programs on this machine's own drives");
        }
        stopRequested_ = win::Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stopRequested_) {
            fail(RCS_ERROR_INTERNAL, "Could not create an event (" + win::errorText(GetLastError()) + ")");
        }
    }
    ~WindowsSupervisor() override { stop(); }

    void start() override {
        std::lock_guard lock(lifecycle_);
        if (thread_.joinable()) {
            if (running_) {
                return;
            }
            thread_.join(); // It ended on an error (RCS_SUPERVISOR_FAILED): it starts anew.
        }
        enablePrivilege(L"SeTcbPrivilege");
        ResetEvent(stopRequested_.get());
        {
            std::lock_guard status(statusMutex_);
            status_ = {};
        }
        running_ = true;
        thread_ = supervisor::thread([this] { run(); });
    }

    void stop() override {
        std::lock_guard lock(lifecycle_);
        if (!thread_.joinable()) {
            return;
        }
        SetEvent(stopRequested_.get());
        thread_.join();
    }

    void update(const Settings& settings) override {
        std::lock_guard lock(settingsMutex_);
        settings_ = settings;
        ++chosen_;
    }

    Status status() const override {
        std::lock_guard lock(statusMutex_);
        return status_;
    }

private:
    struct Helper {
        // Its own job (a job holds the processes of one Windows session only:
        // the first one's), which ends it and what it starts when the
        // supervisor lets go of it.
        win::Handle job;
        win::Handle process, stopEvent;
        std::unique_ptr<SasRelay> relay; // Its own: it asks for Ctrl+Alt+Del through it.
        DWORD processId = 0, sessionId = 0;
        Clock::time_point started;
    };

    // The settings now, and how many times the host chose (rcs_supervisor_update).
    std::pair<Settings, uint64_t> settings() const {
        std::lock_guard lock(settingsMutex_);
        return {settings_, chosen_};
    }

    // Where the helper is to run now: the console's session, or the chosen
    // one while it is shown (ACTIVE or CONNECTED). `ended`: the chosen session
    // does not exist, now or at a look since the host chose (`gone`); a later
    // session may get its number, and is not taken until the host chooses again.
    struct Target {
        DWORD session = noSession; // The one it concerns.
        bool shown = false;
        bool ended = false;
        std::string why; // Not shown: why.
    };
    static Target target(uint32_t chosen, bool gone) {
        if (!chosen) {
            const DWORD console = WTSGetActiveConsoleSessionId();
            const bool shown = console != noSession && console != 0;
            return {console, shown, false, shown ? std::string() : "The console has no session yet"};
        }
        const auto ended = [&] {
            return Target{chosen, false, true,
                          "Session " + std::to_string(chosen) +
                              " does not exist (it ended); a later session given its number is not taken until "
                              "rcs_supervisor_update chooses again"};
        };
        if (gone) {
            return ended();
        }
        const auto sessions = win::sessionStates();
        if (!sessions) {
            return {chosen, false, false,
                    "Remote Desktop Services did not list the sessions (" + win::errorText(GetLastError()) + ")"};
        }
        const auto found = std::find_if(sessions->begin(), sessions->end(),
                                        [&](const win::SessionState& s) { return s.id == chosen; });
        if (found == sessions->end()) {
            return ended();
        }
        const rcs_os_session_state state = win::groupOf(*found);
        if (state == RCS_OS_SESSION_ACTIVE || state == RCS_OS_SESSION_CONNECTED) {
            return {chosen, true, false, {}};
        }
        return {chosen, false, false, win::notShown(*found)};
    }

    void report(rcs_supervisor_event_type type, DWORD session, DWORD process, DWORD exitCode, rcs_result result,
                const std::string& message) {
        rcs_supervisor_event event{};
        event.struct_size = sizeof(event);
        event.type = type;
        event.timestamp_ns = monotonicNs();
        event.os_session = session;
        event.process_id = process;
        event.exit_code = exitCode;
        event.result = result;
        copyText(event.message, message);
        if (handler_) {
            handler_(event);
        }
    }

    // The token the helper runs with, for `session`; none when the user
    // account has nobody signed in there.
    win::Handle token(DWORD session) {
        HANDLE primary = nullptr;
        if (config_.account == RCS_HELPER_USER) {
            HANDLE user = nullptr;
            if (!WTSQueryUserToken(session, &user)) {
                const DWORD error = GetLastError();
                if (error == ERROR_NO_TOKEN) {
                    return win::Handle();
                }
                fail(RCS_ERROR_PERMISSION_REQUIRED,
                     "The signed-in user's token could not be taken (" + win::errorText(error) + ")");
            }
            const win::Handle owned(user);
            if (!DuplicateTokenEx(user, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
                fail(RCS_ERROR_PERMISSION_REQUIRED,
                     "The user's token could not be duplicated (" + win::errorText(GetLastError()) + ")");
            }
            return win::Handle(primary);
        }
        HANDLE self = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_SESSIONID |
                                  TOKEN_ADJUST_DEFAULT,
                              &self)) {
            fail(RCS_ERROR_PERMISSION_REQUIRED,
                 "This process's token could not be opened (" + win::errorText(GetLastError()) + ")");
        }
        const win::Handle owned(self);
        if (!DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
            fail(RCS_ERROR_PERMISSION_REQUIRED,
                 "This process's token could not be duplicated (" + win::errorText(GetLastError()) + ")");
        }
        win::Handle moved(primary);
        if (!SetTokenInformation(moved.get(), TokenSessionId, &session, sizeof(session))) {
            fail(RCS_ERROR_PERMISSION_REQUIRED, "The token could not be moved to session " + std::to_string(session) +
                                                    " (" + win::errorText(GetLastError()) +
                                                    "); the supervisor needs SeTcbPrivilege (LocalSystem)");
        }
        return moved;
    }

    // Starts the helper in `session`. False when nobody is signed in there to
    // run it as (the user account); throws Failure why it could not start.
    bool startHelper(DWORD session) {
        const win::Handle primary = token(session);
        if (!primary) {
            return false;
        }

        // What the helper inherits: the stop event, its end of the pipe that
        // asks for Ctrl+Alt+Del, and the key in a pipe it reads once.
        // Inheritable only while CreateProcess runs: a process the host
        // starts meanwhile with every inheritable handle gets none of them.
        Helper helper;
        helper.job = win::Handle(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!helper.job ||
            !SetInformationJobObject(helper.job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            fail(RCS_ERROR_INTERNAL, "Could not create the job that keeps the helper from outliving the supervisor (" +
                                         win::errorText(GetLastError()) + ")");
        }
        helper.stopEvent = win::Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!helper.stopEvent) {
            fail(RCS_ERROR_INTERNAL,
                 "Could not create the helper's stop event (" + win::errorText(GetLastError()) + ")");
        }
        helper.relay = std::make_unique<SasRelay>();
        const std::wstring self = decimal(GetCurrentProcessId()) + L":";
        std::vector<HANDLE> inherited{helper.stopEvent.get(), helper.relay->helperEnd()};
        std::vector<std::wstring> variables{
            std::wstring(stopVariable) + L"=" + self + decimal(reinterpret_cast<uintptr_t>(helper.stopEvent.get())),
            std::wstring(sasVariable) + L"=" + self + decimal(reinterpret_cast<uintptr_t>(helper.relay->helperEnd()))};
        win::Handle keyPipe;
        if (config_.helperKey) {
            HANDLE read = nullptr, write = nullptr;
            if (!CreatePipe(&read, &write, nullptr, 0)) {
                fail(RCS_ERROR_INTERNAL, "Could not create the helper's key pipe (" + win::errorText(GetLastError()) +
                                             ")");
            }
            keyPipe = win::Handle(read);
            const win::Handle writer(write);
            DWORD written = 0;
            if (!WriteFile(writer.get(), config_.helperKey->data(), DWORD(config_.helperKey->size()), &written,
                           nullptr) ||
                written != config_.helperKey->size()) {
                fail(RCS_ERROR_INTERNAL, "Could not hand the helper its key (" + win::errorText(GetLastError()) + ")");
            }
            inherited.push_back(keyPipe.get());
            variables.push_back(std::wstring(keyVariable) + L"=" + self +
                                decimal(reinterpret_cast<uintptr_t>(keyPipe.get())));
        }
        auto env = environment(primary.get(), variables);
        InheritedHandles handles(inherited); // Only these, not whatever else this process holds inheritable.

        std::wstring command = command_; // CreateProcess may write to it.
        std::wstring desktop = L"winsta0\\default";
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.lpDesktop = desktop.data();
        startup.lpAttributeList = handles.list();
        PROCESS_INFORMATION process{};
        const auto inheritable = [&](bool on) {
            for (const HANDLE handle : inherited) {
                SetHandleInformation(handle, HANDLE_FLAG_INHERIT, on ? HANDLE_FLAG_INHERIT : 0);
            }
        };
        inheritable(true);
        // The program is named apart: CreateProcess starts exactly it and never guesses from the command line.
        const BOOL started = CreateProcessAsUserW(primary.get(), program_.c_str(), command.data(), nullptr, nullptr,
                                                  TRUE,
                                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                                                      CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                                  env.data(), nullptr, &startup.StartupInfo, &process);
        const DWORD startError = GetLastError();
        inheritable(false);
        if (!started) {
            const DWORD error = startError;
            fail(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_BAD_EXE_FORMAT
                     ? RCS_ERROR_INVALID_ARGUMENT
                     : RCS_ERROR_PERMISSION_REQUIRED,
                 "The helper could not be started in session " + std::to_string(session) + " (" +
                     win::errorText(error) + "): " + config_.program);
        }
        helper.process = win::Handle(process.hProcess);
        const win::Handle thread(process.hThread);
        // Ends the suspended helper that cannot go on: it never ran.
        const auto abandon = [&](const std::string& why) {
            const DWORD error = GetLastError();
            const bool ended = TerminateProcess(helper.process.get(), 1) != 0;
            fail(RCS_ERROR_INTERNAL, why + " (" + win::errorText(error) + "); it " +
                                         (ended ? "was ended" : "could not be ended and stays suspended"));
        };
        // In the job before it runs: whatever it starts ends with it, and with
        // this process. A helper outside it could outlive the service.
        if (!AssignProcessToJobObject(helper.job.get(), helper.process.get())) {
            abandon("The helper could not be put in the job that ends it with the supervisor");
        }
        if (!helper.relay->serve(helper.process.get())) {
            abandon("The helper's Ctrl+Alt+Del endpoint could not keep its process");
        }
        if (ResumeThread(thread.get()) == DWORD(-1)) {
            abandon("The helper could not be resumed");
        }
        helper.processId = process.dwProcessId;
        helper.sessionId = session;
        helper.started = Clock::now();
        {
            std::lock_guard lock(statusMutex_);
            status_.helperRunning = true;
            status_.sessionId = session;
            status_.processId = helper.processId;
            ++status_.starts;
        }
        info("Helper " + std::to_string(helper.processId) + " started in session " + std::to_string(session));
        report(RCS_SUPERVISOR_HELPER_STARTED, session, helper.processId, 0, RCS_OK, "The helper started");
        helper_ = std::move(helper);
        return true;
    }

    // Once for each reason, until a helper starts or the session to run in changes.
    void reportNoSession(DWORD session, const std::string& why) {
        if (why == noSessionReported_) {
            return;
        }
        noSessionReported_ = why;
        info(why);
        report(RCS_SUPERVISOR_NO_SESSION, session, 0, 0, RCS_OK, why);
    }

    // The helper has ended (or is to): its record is closed and reported.
    void finished(const std::string& how) {
        DWORD code = 0;
        GetExitCodeProcess(helper_->process.get(), &code);
        const DWORD process = helper_->processId, session = helper_->sessionId;
        const bool early = Clock::now() - helper_->started < shortLived;
        helper_.reset();
        {
            std::lock_guard lock(statusMutex_);
            status_.helperRunning = false;
            ++status_.exits;
        }
        info("Helper " + std::to_string(process) + " " + how + " (exit code " + std::to_string(code) + ")");
        report(RCS_SUPERVISOR_HELPER_EXITED, session, process, code, RCS_OK, how);
        shortRuns_ = early ? shortRuns_ + 1 : 0;
    }

    // Asks the helper to end, waits for the grace period, ends it.
    void endHelper(const std::string& how, std::chrono::milliseconds grace) {
        SetEvent(helper_->stopEvent.get());
        if (WaitForSingleObject(helper_->process.get(), DWORD(grace.count())) != WAIT_OBJECT_0) {
            TerminateProcess(helper_->process.get(), 1);
            WaitForSingleObject(helper_->process.get(), 5000);
        }
        finished(how);
    }

    // restart_delay_ms, doubled for each earlier one in a row, up to the maximum.
    static std::chrono::milliseconds delayAfter(const Settings& settings, uint32_t inRow) {
        auto delay = settings.restartDelay;
        for (uint32_t i = 1; i < inRow && delay < settings.restartDelayMax; ++i) {
            delay *= 2;
        }
        return std::min(delay, settings.restartDelayMax);
    }

    bool stopping() const { return WaitForSingleObject(stopRequested_.get(), 0) == WAIT_OBJECT_0; }

    void run() noexcept {
        shortRuns_ = failures_ = 0;
        noSessionReported_.clear();
        Clock::time_point nextStart{};
        DWORD shownIn = noSession;                  // The session a helper could run in at the last look.
        std::optional<Clock::time_point> noneSince; // Since when there has been none.
        uint64_t chosen = 0;                        // The host's choice acted on...
        bool gone = false;                          // ...and whether its session has ended since.
        try {
            for (bool first = true; !stopping(); first = false) {
                const auto [current, choice] = settings();
                if (first || choice != chosen) { // Started, or chosen anew: an ended session's number is taken.
                    chosen = choice;
                    gone = false;
                }
                const Target where = target(current.osSession, gone);
                gone = where.ended;
                const auto now = Clock::now();
                if (!where.shown) {
                    shownIn = noSession;
                    noneSince = noneSince.value_or(now);
                    // The console is without a session for a moment while it switches; a chosen session that
                    // shows nothing holds no helper, and the client that connects next gets a new one.
                    if (helper_ && (current.osSession || now - *noneSince >= sessionGrace)) {
                        endHelper("ended: " + lowerFirst(where.why), current.stopGrace);
                    }
                    if (!helper_) {
                        reportNoSession(where.session, where.why);
                    }
                } else {
                    noneSince.reset();
                    if (helper_ && helper_->sessionId != where.session) {
                        endHelper(current.osSession ? "ended: it moves to session " + std::to_string(where.session)
                                                    : std::string("ended: the console moved to another session"),
                                  current.stopGrace);
                    }
                    if (where.session != shownIn) { // A session appeared or shows again, or the helper moves.
                        shownIn = where.session;
                        shortRuns_ = failures_ = 0;
                        nextStart = {};
                        noSessionReported_.clear();
                    }
                    if (!helper_ && now >= nextStart) {
                        try {
                            if (startHelper(where.session)) {
                                failures_ = 0;
                                noSessionReported_.clear();
                            } else { // Nobody signed in yet: not a failure, tried again as it polls.
                                reportNoSession(where.session,
                                                current.osSession
                                                    ? "Nobody is signed in to session " + std::to_string(where.session)
                                                    : std::string("Nobody is signed in to the console session"));
                            }
                        } catch (const Failure& failure) {
                            ++failures_;
                            nextStart = now + delayAfter(current, failures_);
                            report(RCS_SUPERVISOR_HELPER_FAILED, where.session, 0, 0, failure.result(), failure.what());
                            warning(failure.what());
                        }
                    }
                }
                if (helper_) {
                    const HANDLE waits[] = {stopRequested_.get(), helper_->process.get()};
                    if (WaitForMultipleObjects(2, waits, FALSE, 250) == WAIT_OBJECT_0 + 1) {
                        finished("ended by itself");
                        nextStart = Clock::now() + delayAfter(current, std::max<uint32_t>(shortRuns_, 1));
                    }
                } else {
                    WaitForSingleObject(stopRequested_.get(), 250);
                }
            }
            if (helper_) {
                endHelper("ended: the supervisor stopped", settings().first.stopGrace);
            }
        } catch (const std::exception& error) {
            const std::string why = std::string("The supervisor stopped on an error: ") + error.what();
            supervisor::error(why);
            if (helper_) {
                TerminateProcess(helper_->process.get(), 1);
                WaitForSingleObject(helper_->process.get(), 5000);
                finished("ended: the supervisor stopped on an error");
            }
            report(RCS_SUPERVISOR_FAILED, 0, 0, 0, RCS_ERROR_INTERNAL, why);
        }
        running_ = false;
    }

    const Config config_; // The program, account and key; its settings are settings_'s first value.
    const Handler handler_;
    const std::wstring program_, command_;
    mutable std::mutex settingsMutex_;
    Settings settings_; // As rcs_supervisor_update left them...
    uint64_t chosen_ = 0;         // ...and how many times it did.
    win::Handle stopRequested_;
    std::mutex lifecycle_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex statusMutex_;
    Status status_;

    // The supervisor thread's.
    std::optional<Helper> helper_;
    uint32_t shortRuns_ = 0;        // Helpers in a row that ended soon after starting.
    uint32_t failures_ = 0;         // Starts in a row that failed.
    std::string noSessionReported_; // The reason last reported.
};

} // namespace

std::unique_ptr<Supervisor> create(const Config& config, Supervisor::Handler handler) {
    return std::make_unique<WindowsSupervisor>(config, std::move(handler));
}

} // namespace rcsdk::supervisor
