// The supervisor and the session it keeps its helper in (Windows). Only
// LocalSystem may run a supervisor, so this skips (exit 77) in any other
// process: run it as a SYSTEM scheduled task, as a service would run. The
// helper is this program again ("helper"): it checks its Ctrl+Alt+Del pipe,
// waits for the supervisor's request to end and exits 0, so an exit code of
// 0 shows both.
//
// os_session naming the console's session behaves like 0; rcs_supervisor_update
// moves the helper away (a session that does not exist: NO_SESSION) and back,
// leaves it where it is when the target does not change, and refuses to change
// what is fixed for the supervisor's life. The user account runs the helper as
// the user signed in to the chosen session. What a supervisor logs names it,
// from its own threads too.
#include <rcsdk/agent.hpp>
#include <rcsdk/supervisor.hpp>

#include "support/check.hpp"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// What a supervisor reported, taken in order.
class Events {
public:
    void add(const rcs_supervisor_event& event) {
        std::lock_guard lock(mutex_);
        std::printf("event %u: session %u, process %u, exit code %u: %s\n", event.type, event.os_session,
                    event.process_id, event.exit_code, event.message);
        std::fflush(stdout);
        events_.push_back(event);
        changed_.notify_all();
    }
    // The next one of `type` after those taken, within `timeout`; the ones
    // before it are taken too.
    std::optional<rcs_supervisor_event> next(rcs_supervisor_event_type type, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        std::optional<rcs_supervisor_event> found;
        changed_.wait_for(lock, timeout, [&] {
            for (; !found && taken_ < events_.size(); ++taken_) {
                if (events_[taken_].type == type) {
                    found = events_[taken_];
                }
            }
            return found.has_value();
        });
        return found;
    }
    size_t count() const {
        std::lock_guard lock(mutex_);
        return events_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<rcs_supervisor_event> events_;
    size_t taken_ = 0;
};

// The records of supervisors (RCS_LOG_SOURCE_SUPERVISOR) the log took.
class Records {
public:
    void add(const rcs_log_record& record) {
        if (record.source != RCS_LOG_SOURCE_SUPERVISOR) {
            return;
        }
        std::lock_guard lock(mutex_);
        lines_.push_back({record.supervisor, std::string(record.module) + ": " + record.message});
    }
    bool has(rcs_supervisor supervisor, const char* part) const {
        std::lock_guard lock(mutex_);
        for (const auto& [from, line] : lines_) {
            if (from == supervisor && line.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<rcs_supervisor, std::string>> lines_;
};

Records& records() {
    static Records value;
    return value;
}

std::string selfPath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        CHECK(size != 0);
        if (size < path.size()) {
            path.resize(size);
            break;
        }
        path.resize(path.size() * 2);
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string text(size_t(bytes > 0 ? bytes - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, text.data(), bytes, nullptr, nullptr);
    return text;
}

// A session number that is not in use now.
uint32_t unusedSession() {
    const auto sessions = rcsdk::osSessions();
    for (uint32_t id = 0x7FFF0000u;; ++id) {
        bool used = false;
        for (const auto& s : sessions) {
            used = used || s.os_session == id;
        }
        if (!used) {
            return id;
        }
    }
}

bool contains(const char* text, const char* part) {
    return std::strstr(text, part) != nullptr;
}

// What a helper finds of the pipe through which it asks for Ctrl+Alt+Del
// (SA-08 of docs/SECURITY-ASSESSMENT.md): the end it was given answers it
// (a request that asks nothing, so nothing is sent), and the pipe's name,
// which any process can list, opens for nobody (as LocalSystem: its one
// instance is the helper's; as the user: not the user's to open), so no
// other process of its account asks in its place or keeps it from asking.
// Only the pipes of its supervisor (process `supervisor`) are looked at:
// another supervisor on the machine, of another version, has its own. 0 when
// so.
int sasPipe(DWORD supervisor) {
    wchar_t value[64] = {};
    const DWORD size = GetEnvironmentVariableW(L"RCSDK_SUPERVISOR_SAS", value, 64);
    const std::wstring text(value, size < 64 ? size : 0);
    const size_t colon = text.find(L':');
    const auto end = reinterpret_cast<HANDLE>(
        uintptr_t(colon == std::wstring::npos ? 0 : std::wcstoull(text.c_str() + colon + 1, nullptr, 10)));
    uint8_t request = 'X';
    int32_t answer = 0;
    DWORD read = 0;
    if (!end || !TransactNamedPipe(end, &request, 1, &answer, sizeof(answer), &read, nullptr) ||
        read != sizeof(answer) || answer != RCS_ERROR_INVALID_ARGUMENT) {
        return 5;
    }
    WIN32_FIND_DATAW found{};
    const std::wstring pattern = L"\\\\.\\pipe\\rcsdk-sas-" + std::to_wstring(supervisor) + L"-*";
    const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    int result = 0;
    for (BOOL more = search != INVALID_HANDLE_VALUE; more && !result; more = FindNextFileW(search, &found)) {
        const HANDLE opened = CreateFileW((std::wstring(L"\\\\.\\pipe\\") + found.cFileName).c_str(),
                                          GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (opened != INVALID_HANDLE_VALUE) {
            CloseHandle(opened);
            result = 6;
        }
    }
    if (search != INVALID_HANDLE_VALUE) {
        FindClose(search);
    }
    return result;
}

// The helper: ends when its supervisor asks.
int helper(DWORD supervisor) {
    if (const int sas = sasPipe(supervisor)) {
        return sas;
    }
    try {
        return rcsdk::waitForStopRequest(120000) ? 0 : 3;
    } catch (const rcsdk::Error&) {
        return 4; // Not started by a supervisor.
    }
}

// os_session 0 and os_session = the console's session start the helper in
// the same session, which ends when asked.
void followsConsole(const rcs_supervisor_config& base, DWORD console) {
    for (const uint32_t chosen : {uint32_t(0), uint32_t(console)}) {
        Events events;
        auto config = base;
        config.os_session = chosen;
        rcsdk::Supervisor supervisor(config, [&](const rcs_supervisor_event& e) { events.add(e); });
        CHECK(supervisor.start() == RCS_OK);
        const auto started = events.next(RCS_SUPERVISOR_HELPER_STARTED, 15s);
        CHECK(started && started->os_session == console && started->process_id != 0);
        const auto status = supervisor.status();
        CHECK(status.helper_running && status.os_session == console && status.process_id == started->process_id);
        CHECK(supervisor.stop() == RCS_OK); // Asks it to end, and joins: reported by now.
        const auto exited = events.next(RCS_SUPERVISOR_HELPER_EXITED, 1s);
        CHECK(exited && exited->process_id == started->process_id && exited->exit_code == 0);
        CHECK(records().has(supervisor.handle(), "supervisor: Helper ") && // Its thread's.
              records().has(supervisor.handle(), "ended: the supervisor stopped"));
    }
}

// rcs_supervisor_update while running.
void moves(const rcs_supervisor_config& base, DWORD console) {
    Events events;
    auto config = base;
    config.os_session = console;
    rcsdk::Supervisor supervisor(config, [&](const rcs_supervisor_event& e) { events.add(e); });
    CHECK(supervisor.start() == RCS_OK);
    const auto started = events.next(RCS_SUPERVISOR_HELPER_STARTED, 15s);
    CHECK(started && started->os_session == console);

    // Following the console instead: the helper is there already, and stays.
    auto changed = config;
    changed.os_session = 0;
    CHECK(supervisor.update(changed) == RCS_OK);
    std::this_thread::sleep_for(1500ms);
    CHECK(supervisor.status().starts == 1 && supervisor.status().helper_running);

    // A session that does not exist: the helper is asked to end at once, and
    // NO_SESSION names the session and says why.
    const uint32_t missing = unusedSession();
    changed.os_session = missing;
    CHECK(supervisor.update(changed) == RCS_OK);
    const auto ended = events.next(RCS_SUPERVISOR_HELPER_EXITED, 15s);
    CHECK(ended && ended->process_id == started->process_id && ended->exit_code == 0);
    const auto none = events.next(RCS_SUPERVISOR_NO_SESSION, 5s);
    CHECK(none && none->os_session == missing && contains(none->message, "does not exist"));
    CHECK(!supervisor.status().helper_running);

    // Back to the console's session: started there at once, not after a delay.
    const auto before = std::chrono::steady_clock::now();
    CHECK(supervisor.update(config) == RCS_OK);
    const auto again = events.next(RCS_SUPERVISOR_HELPER_STARTED, 5s);
    CHECK(again && again->os_session == console && std::chrono::steady_clock::now() - before < 2s);

    // What is fixed for the supervisor's life stays.
    auto other = config;
    std::strcpy(other.command_line, "\"C:\\Windows\\System32\\cmd.exe\" /c exit");
    CHECK(supervisor.update(other) == RCS_ERROR_INVALID_ARGUMENT &&
          rcsdk::lastCallError().find("fixed") != std::string::npos);
    other = config;
    other.account = RCS_HELPER_USER;
    CHECK(supervisor.update(other) == RCS_ERROR_INVALID_ARGUMENT);
    other = config;
    other.helper_key[0] ^= 1;
    CHECK(supervisor.update(other) == RCS_ERROR_INVALID_ARGUMENT);
    other = config;
    other.os_session = 0xFFFFFFFFu;
    CHECK(supervisor.update(other) == RCS_ERROR_INVALID_ARGUMENT &&
          rcsdk::lastCallError().find("os_session") != std::string::npos);
    other = config;
    other.restart_delay_ms = 10;
    CHECK(supervisor.update(other) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(supervisor.status().starts == 2 && supervisor.status().helper_running); // None of these moved it.

    CHECK(supervisor.stop() == RCS_OK);
    const auto last = events.next(RCS_SUPERVISOR_HELPER_EXITED, 1s);
    CHECK(last && last->process_id == again->process_id && last->exit_code == 0);
}

// The user account in a chosen session runs as whoever is signed in there,
// and waits while nobody is.
void asUser(const rcs_supervisor_config& base, DWORD console) {
    bool signedIn = false;
    for (const auto& s : rcsdk::osSessions()) {
        signedIn = signedIn || (s.os_session == console && s.account[0] != '\0');
    }
    Events events;
    auto config = base;
    config.account = RCS_HELPER_USER;
    config.os_session = console;
    rcsdk::Supervisor supervisor(config, [&](const rcs_supervisor_event& e) { events.add(e); });
    CHECK(supervisor.start() == RCS_OK);
    if (!signedIn) {
        const auto none = events.next(RCS_SUPERVISOR_NO_SESSION, 5s);
        CHECK(none && none->os_session == console && contains(none->message, "Nobody is signed in"));
        CHECK(supervisor.stop() == RCS_OK);
        return;
    }
    const auto started = events.next(RCS_SUPERVISOR_HELPER_STARTED, 15s);
    CHECK(started && started->os_session == console);
    CHECK(supervisor.stop() == RCS_OK);
    const auto exited = events.next(RCS_SUPERVISOR_HELPER_EXITED, 1s);
    CHECK(exited && exited->exit_code == 0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::strcmp(argv[1], "helper") == 0) {
        return helper(DWORD(std::strtoul(argv[2], nullptr, 10)));
    }
    auto config = rcsdk::supervisorConfig("\"" + selfPath() + "\" helper " + std::to_string(GetCurrentProcessId()),
                                          RCS_HELPER_SYSTEM,
                                          rcsdk::generateConnectionKey());
    config.stop_grace_ms = 10000;
    rcs_supervisor probe = nullptr;
    const rcs_result created = rcs_supervisor_create(&config, nullptr, &probe);
    rcs_supervisor_destroy(probe);
    if (created == RCS_ERROR_PERMISSION_REQUIRED) {
        std::printf("skipped: only LocalSystem may run a supervisor (run this as a SYSTEM scheduled task)\n");
        return 77;
    }
    CHECK(created == RCS_OK);
    const DWORD console = WTSGetActiveConsoleSessionId();
    if (console == 0xFFFFFFFF || console == 0) {
        std::printf("skipped: the console has no session\n");
        return 77;
    }
    CHECK(rcsdk::setLogCallback(RCS_LOG_INFO, [](const rcs_log_record& r) { records().add(r); }) == RCS_OK);
    return runTest("supervisor", [&] {
        followsConsole(config, console);
        moves(config, console);
        asUser(config, console);
    });
}
