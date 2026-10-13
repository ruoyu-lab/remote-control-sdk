// The supervisor's own pieces (RemoteControl::Supervisor). It is built on the
// SDK's public headers alone: what it logs goes through rcs_log_write, why a
// call fails through rcs_set_last_call_error.
#pragma once

#include <rcsdk/supervisor.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace rcsdk::supervisor {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

inline uint64_t monotonicNs() noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// An error carrying the public result; the C functions return it.
class Failure : public std::runtime_error {
public:
    Failure(rcs_result result, const std::string& message) : std::runtime_error(message), result_(result) {}
    rcs_result result() const noexcept { return result_; }

private:
    rcs_result result_;
};

[[noreturn]] inline void fail(rcs_result result, const std::string& message) {
    throw Failure(result, message);
}

// UTF-8 text into a field, NUL-terminated, cut short at a character's start.
template <size_t N>
void copyText(char (&field)[N], std::string_view text) noexcept {
    size_t size = std::min(text.size(), N - 1);
    while (size > 0 && size < text.size() && (uint8_t(text[size]) & 0xC0) == 0x80) {
        --size;
    }
    std::memcpy(field, text.data(), size);
    field[size] = '\0';
}

// ---- The log ----------------------------------------------------------------------------------

// A supervisor as its log records name it.
struct LogSource {
    rcs_supervisor handle = nullptr;
    void* userData = nullptr; // Of its callbacks.
};

// Makes `source` the one this thread's records name while it lives: each C
// call on a supervisor, and the threads a supervisor starts (thread()). The
// source outlives them: it is the handle's, destroyed after its threads ended.
class LogScope {
public:
    explicit LogScope(const LogSource* source) noexcept;
    ~LogScope();
    LogScope(const LogScope&) = delete;
    LogScope& operator=(const LogScope&) = delete;

private:
    const LogSource* previous_;
};

const LogSource* logSource() noexcept;
// A record of this thread's source, the process's without one.
void log(rcs_log_level level, const std::string& message) noexcept;
inline void error(const std::string& message) noexcept {
    log(RCS_LOG_ERROR, message);
}
inline void warning(const std::string& message) noexcept {
    log(RCS_LOG_WARNING, message);
}
inline void info(const std::string& message) noexcept {
    log(RCS_LOG_INFO, message);
}

// A thread whose records name the source of the thread starting it.
template <class Function>
std::thread thread(Function&& function) {
    return std::thread([source = logSource(), function = std::forward<Function>(function)]() mutable {
        const LogScope scope(source);
        function();
    });
}

// ---- The supervisor ---------------------------------------------------------------------------

// What rcs_supervisor_update may change while the supervisor runs.
struct Settings {
    uint32_t osSession = 0; // 0: the console's session.
    std::chrono::milliseconds restartDelay{2000}, restartDelayMax{30000}, stopGrace{3000};
};
struct Config {
    std::string commandLine; // UTF-8
    std::string program;     // The command line's program, a full path: what is started, never searched for.
    rcs_helper_account account = RCS_HELPER_SYSTEM;
    std::optional<std::array<uint8_t, 32>> helperKey; // Handed to each helper (rcs_helper_get_key).
    Settings settings;
};
struct Status {
    bool helperRunning = false;
    uint32_t sessionId = 0, processId = 0;
    uint64_t starts = 0, exits = 0;
};

// Keeps one helper process running in the console's or a chosen session.
class Supervisor {
public:
    using Handler = std::function<void(const rcs_supervisor_event&)>;
    virtual ~Supervisor() = default;
    virtual void start() = 0;
    // Asks the helper to end, ends it after the grace period, and joins.
    virtual void stop() = 0;
    // From any thread, the handler's too; a running supervisor acts on it
    // within its next look at the sessions.
    virtual void update(const Settings& settings) = 0;
    virtual Status status() const = 0;
};

// Throws Failure: UNSUPPORTED off Windows, PERMISSION_REQUIRED when this
// process is not LocalSystem (either account needs it).
std::unique_ptr<Supervisor> create(const Config& config, Supervisor::Handler handler);

// rcs_list_os_sessions. Throws Failure: UNSUPPORTED off Windows.
std::vector<rcs_os_session_info> osSessions();

} // namespace rcsdk::supervisor
