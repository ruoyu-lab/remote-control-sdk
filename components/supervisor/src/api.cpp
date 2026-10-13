// The supervisor's C functions (rcsdk/supervisor.h): arguments are checked
// here, and failures become results with their reasons.
#include "supervisor.hpp"

#include <cctype>

struct rcs_supervisor_impl {
    rcs_supervisor_callbacks callbacks;
    rcsdk::supervisor::LogSource log; // Outlives the supervisor's threads.
    // What rcs_supervisor_update may not change.
    std::string commandLine;
    rcs_helper_account account = RCS_HELPER_SYSTEM;
    std::array<uint8_t, 32> helperKey{};
    std::unique_ptr<rcsdk::supervisor::Supervisor> supervisor;
    static thread_local const rcs_supervisor_impl* current; // The supervisor whose callback this thread is in.
};
thread_local const rcs_supervisor_impl* rcs_supervisor_impl::current = nullptr;

namespace rcsdk::supervisor {
namespace {

thread_local const LogSource* threadSource = nullptr;

} // namespace

LogScope::LogScope(const LogSource* source) noexcept : previous_(std::exchange(threadSource, source)) {}

LogScope::~LogScope() {
    threadSource = previous_;
}

const LogSource* logSource() noexcept {
    return threadSource;
}

void log(rcs_log_level level, const std::string& message) noexcept {
    rcs_log_record record{};
    record.struct_size = sizeof(record);
    record.level = level;
    record.module = "supervisor";
    record.message = message.c_str();
    if (threadSource) {
        record.source = RCS_LOG_SOURCE_SUPERVISOR;
        record.supervisor = threadSource->handle;
        record.user_data = threadSource->userData;
    }
    rcs_log_write(&record);
}

} // namespace rcsdk::supervisor

namespace {

using rcsdk::supervisor::Failure;

// Every call starts without a reason, so a failure never shows an older one.
void beginCall() noexcept {
    rcs_set_last_call_error(RCS_OK, nullptr);
}

rcs_result refuse(rcs_result result, const std::string& reason) noexcept {
    return rcs_set_last_call_error(result, reason.c_str());
}

template <class Function>
rcs_result guarded(Function&& function) noexcept {
    try {
        return function();
    } catch (const Failure& failure) {
        return refuse(failure.result(), failure.what());
    } catch (const std::exception& exception) {
        return refuse(RCS_ERROR_INTERNAL, exception.what());
    } catch (...) {
        return refuse(RCS_ERROR_INTERNAL, "Unexpected internal error");
    }
}

template <class T>
bool sized(const T* value) noexcept {
    return value && value->struct_size == sizeof(T);
}

std::string initialise(const char* structure, const char* macro) {
    return std::string("Initialise ") + structure + " with " + macro + " from the headers of this library (" +
           RCSDK_VERSION_STRING + ")";
}

// From the supervisor's own callback, or from the log callback, which holds up
// the threads it would wait for.
bool fromCallback(rcs_supervisor supervisor) noexcept {
    return rcs_supervisor_impl::current == supervisor || rcs_log_in_callback();
}

rcsdk::supervisor::LogScope logAs(rcs_supervisor supervisor) noexcept {
    return rcsdk::supervisor::LogScope(&supervisor->log);
}

// The program a supervisor's command line names, as CreateProcess reads the
// line: between leading quotes, or up to the first space or tab. Empty, with
// `problem` set, when it is no full path on a drive to a program file with
// its extension; that catches a path with spaces left unquoted too, which
// CreateProcess would try piece by piece ("C:\Program" first), and a network
// share's or a device's path (\\server\share\..., \\?\..., \\.\...): whoever
// can write the share would run as the supervisor's account.
std::string commandLineProgram(const std::string& line, std::string& problem) {
    std::string program;
    const bool quoted = !line.empty() && line[0] == '"';
    if (quoted) {
        const size_t end = line.find('"', 1);
        if (end == std::string::npos || (end + 1 < line.size() && line[end + 1] != ' ' && line[end + 1] != '\t')) {
            problem = "the program's quotes are not closed, or not followed by a space";
            return {};
        }
        program = line.substr(1, end - 1);
    } else {
        program = line.substr(0, line.find_first_of(" \t"));
    }
    const bool drive = program.size() > 2 && std::isalpha(uint8_t(program[0])) && program[1] == ':' &&
                       (program[2] == '\\' || program[2] == '/');
    const size_t name = program.find_last_of("\\/");
    const size_t dot = program.find('.', name == std::string::npos ? 0 : name + 1);
    if (program.rfind("\\\\", 0) == 0 || program.rfind("//", 0) == 0) {
        problem = "the program is on a network share or a device path (\\\\...): a supervisor starts only programs on "
                  "this machine's own drives, e.g. \"C:\\Program Files\\MyApp\\helper.exe\"";
    } else if (!drive) {
        problem = "the program is no full path, e.g. \"C:\\Program Files\\MyApp\\helper.exe\"";
    } else if (dot == std::string::npos || dot + 1 == program.size()) {
        problem = quoted ? "the program has no extension; name it with its .exe"
                         : "the program \"" + program +
                               "\" has no extension: quote a path with spaces (CreateProcess would try that piece "
                               "first) and name the program with its .exe";
    }
    return problem.empty() ? program : std::string();
}

// Why a supervisor's config is refused before its fields are read: built with
// other headers, or not initialised. Empty when it is neither.
std::string configProblem(const rcs_supervisor_config& config) {
    if (config.struct_size != sizeof(rcs_supervisor_config) || !config.version) {
        return initialise("rcs_supervisor_config", "RCS_SUPERVISOR_CONFIG_INIT");
    }
    if (config.version != RCSDK_VERSION) {
        return "rcs_supervisor_config: built with other headers than this library (" +
               std::string(RCSDK_VERSION_STRING) + "); use the headers of the library you ship";
    }
    return {};
}

// Why the settings rcs_supervisor_update may change are out of range; empty
// when they are not.
std::string settingsProblem(const rcs_supervisor_config& config) {
    if (config.os_session == 0xFFFFFFFFu) {
        return "rcs_supervisor_config: os_session 0xFFFFFFFF names no session (0: the console's)";
    }
    if (config.restart_delay_ms < 100 || config.restart_delay_ms > 60'000 ||
        config.restart_delay_max_ms < config.restart_delay_ms || config.restart_delay_max_ms > 600'000 ||
        config.stop_grace_ms > 60'000) {
        return "rcs_supervisor_config: restart_delay_ms is 100..60 000, restart_delay_max_ms at least that (at most "
               "600 000), stop_grace_ms 0..60 000";
    }
    return {};
}

rcsdk::supervisor::Settings settingsOf(const rcs_supervisor_config& config) {
    rcsdk::supervisor::Settings settings;
    settings.osSession = config.os_session;
    settings.restartDelay = std::chrono::milliseconds(config.restart_delay_ms);
    settings.restartDelayMax = std::chrono::milliseconds(config.restart_delay_max_ms);
    settings.stopGrace = std::chrono::milliseconds(config.stop_grace_ms);
    return settings;
}

} // namespace

extern "C" {

rcs_result rcs_list_os_sessions(rcs_os_session_info* os_sessions, uint32_t item_size, uint32_t capacity,
                                uint32_t* out_count) {
    beginCall();
    if (!out_count || item_size != sizeof(rcs_os_session_info) || (!os_sessions && capacity)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass item_size = sizeof(rcs_os_session_info) and out_count");
    }
    return guarded([&] {
        const auto sessions = rcsdk::supervisor::osSessions();
        *out_count = uint32_t(sessions.size());
        if (!os_sessions) {
            return RCS_OK;
        }
        if (capacity < sessions.size()) {
            return RCS_ERROR_BUFFER_TOO_SMALL;
        }
        std::copy(sessions.begin(), sessions.end(), os_sessions);
        return RCS_OK;
    });
}

rcs_result rcs_supervisor_create(const rcs_supervisor_config* config, const rcs_supervisor_callbacks* callbacks,
                                 rcs_supervisor* out_supervisor) {
    beginCall();
    if (!config || !out_supervisor) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "config and out_supervisor are required");
    }
    *out_supervisor = nullptr;
    if (const auto problem = configProblem(*config); !problem.empty()) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, problem);
    }
    rcs_supervisor_callbacks table = RCS_SUPERVISOR_CALLBACKS_INIT;
    if (callbacks) {
        if (!sized(callbacks)) {
            return refuse(RCS_ERROR_INVALID_ARGUMENT,
                          initialise("rcs_supervisor_callbacks", "RCS_SUPERVISOR_CALLBACKS_INIT"));
        }
        table = *callbacks;
    }
    if (!std::memchr(config->command_line, 0, sizeof(config->command_line)) || !config->command_line[0]) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_supervisor_config: command_line is empty or not NUL-terminated");
    }
    std::string why;
    const std::string program = commandLineProgram(config->command_line, why);
    if (program.empty()) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_supervisor_config: command_line: " + why);
    }
    if (config->account > RCS_HELPER_USER) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_supervisor_config: unknown account");
    }
    if (const auto problem = settingsProblem(*config); !problem.empty()) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, problem);
    }
    return guarded([&] {
        auto impl = std::make_unique<rcs_supervisor_impl>();
        impl->callbacks = table;
        impl->log = {impl.get(), table.user_data};
        impl->commandLine = config->command_line;
        impl->account = config->account;
        std::copy(std::begin(config->helper_key), std::end(config->helper_key), impl->helperKey.begin());
        const auto scope = logAs(impl.get());
        rcsdk::supervisor::Config settings;
        settings.commandLine = config->command_line;
        settings.program = program;
        settings.account = config->account;
        if (std::any_of(impl->helperKey.begin(), impl->helperKey.end(), [](uint8_t b) { return b != 0; })) {
            settings.helperKey = impl->helperKey;
        }
        settings.settings = settingsOf(*config);
        rcs_supervisor_impl* self = impl.get();
        impl->supervisor = rcsdk::supervisor::create(settings, [self](const rcs_supervisor_event& event) {
            if (!self->callbacks.on_event) {
                return;
            }
            const rcs_supervisor_impl* previous = rcs_supervisor_impl::current;
            rcs_supervisor_impl::current = self;
            self->callbacks.on_event(&event, self->callbacks.user_data);
            rcs_supervisor_impl::current = previous;
        });
        *out_supervisor = impl.release();
        return RCS_OK;
    });
}

rcs_result rcs_supervisor_start(rcs_supervisor supervisor) {
    beginCall();
    if (!supervisor) {
        return RCS_ERROR_INVALID_ARGUMENT;
    }
    if (fromCallback(supervisor)) {
        return RCS_ERROR_CALLBACK_THREAD;
    }
    const auto scope = logAs(supervisor);
    return guarded([&] {
        supervisor->supervisor->start();
        return RCS_OK;
    });
}

rcs_result rcs_supervisor_stop(rcs_supervisor supervisor) {
    beginCall();
    if (!supervisor) {
        return RCS_ERROR_INVALID_ARGUMENT;
    }
    if (fromCallback(supervisor)) {
        return RCS_ERROR_CALLBACK_THREAD;
    }
    const auto scope = logAs(supervisor);
    return guarded([&] {
        supervisor->supervisor->stop();
        return RCS_OK;
    });
}

rcs_result rcs_supervisor_destroy(rcs_supervisor supervisor) {
    beginCall();
    if (!supervisor) {
        return RCS_OK;
    }
    if (fromCallback(supervisor)) {
        return RCS_ERROR_CALLBACK_THREAD;
    }
    return guarded([&] {
        {
            const auto scope = logAs(supervisor);
            supervisor->supervisor.reset(); // Its threads end, and with them its records.
        }
        delete supervisor;
        return RCS_OK;
    });
}

rcs_result rcs_supervisor_update(rcs_supervisor supervisor, const rcs_supervisor_config* config) {
    beginCall();
    if (!supervisor || !config) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "supervisor and config are required");
    }
    if (const auto problem = configProblem(*config); !problem.empty()) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, problem);
    }
    const bool sameCommandLine = std::memchr(config->command_line, 0, sizeof(config->command_line)) &&
                                 supervisor->commandLine == config->command_line;
    if (!sameCommandLine || config->account != supervisor->account ||
        !std::equal(supervisor->helperKey.begin(), supervisor->helperKey.end(), std::begin(config->helper_key),
                    std::end(config->helper_key))) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      "rcs_supervisor_update: command_line, account and helper_key are fixed for the supervisor's "
                      "life; create another supervisor for other ones");
    }
    if (const auto problem = settingsProblem(*config); !problem.empty()) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, problem);
    }
    const auto scope = logAs(supervisor);
    return guarded([&] {
        supervisor->supervisor->update(settingsOf(*config));
        return RCS_OK;
    });
}

rcs_result rcs_supervisor_get_status(rcs_supervisor supervisor, rcs_supervisor_status* status) {
    beginCall();
    if (!supervisor) {
        return RCS_ERROR_INVALID_ARGUMENT;
    }
    if (!sized(status)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_supervisor_status", "RCS_SUPERVISOR_STATUS_INIT"));
    }
    return guarded([&] {
        const auto now = supervisor->supervisor->status();
        status->helper_running = now.helperRunning;
        status->os_session = now.sessionId;
        status->process_id = now.processId;
        status->starts = now.starts;
        status->exits = now.exits;
        return RCS_OK;
    });
}

} // extern "C"
