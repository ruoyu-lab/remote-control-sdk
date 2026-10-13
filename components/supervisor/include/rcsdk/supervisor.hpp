// Remote Control SDK: the supervisor's C++ wrapper (rcsdk/supervisor.h),
// beside rcsdk.hpp's; header-only. Its helper's side is rcsdk/agent.hpp's
// (waitForStopRequest, helperKey).
#pragma once

#include <rcsdk/rcsdk.hpp>
#include <rcsdk/supervisor.h>

namespace rcsdk {

// The sessions of this Windows machine (rcs_list_os_sessions): where a
// supervisor can run its helper. Throws Error(UNSUPPORTED) elsewhere.
inline std::vector<rcs_os_session_info> osSessions() {
    return detail::list<rcs_os_session_info>([](rcs_os_session_info* items, uint32_t capacity, uint32_t* count) {
        return rcs_list_os_sessions(items, sizeof(rcs_os_session_info), capacity, count);
    });
}

// A supervisor's configuration with the defaults (rcs_supervisor_config);
// `helperKey` is handed to each helper (helperKey()).
inline rcs_supervisor_config supervisorConfig(const std::string& commandLine,
                                              rcs_helper_account account = RCS_HELPER_SYSTEM,
                                              const std::optional<ConnectionKey>& helperKey = std::nullopt) {
    rcs_supervisor_config config = RCS_SUPERVISOR_CONFIG_INIT;
    detail::setText(config.command_line, commandLine, "command_line");
    config.account = account;
    if (helperKey) {
        std::copy(helperKey->begin(), helperKey->end(), config.helper_key);
    }
    return config;
}

// Keeps a helper process running in the machine's console session, or in a
// session the host chooses (see rcs_supervisor_*); run it from the host's
// service. Not copyable or movable.
//
//   auto config = rcsdk::supervisorConfig("\"C:\\Program Files\\MyApp\\helper.exe\" --helper");
//   rcsdk::Supervisor supervisor(config, [](const rcs_supervisor_event& e) { /* log it */ });
//   supervisor.start();
//   config.os_session = 3; // A Remote Desktop session (osSessions()); 0: the console's again.
//   supervisor.update(config);
class Supervisor final {
public:
    using EventCallback = std::function<void(const rcs_supervisor_event&)>;

    // `helperKey`: handed to each helper (rcs_supervisor_config.helper_key, helperKey()).
    Supervisor(const std::string& commandLine, rcs_helper_account account = RCS_HELPER_SYSTEM,
               EventCallback onEvent = {}, const std::optional<ConnectionKey>& helperKey = std::nullopt)
        : Supervisor(supervisorConfig(commandLine, account, helperKey), std::move(onEvent)) {}
    explicit Supervisor(const rcs_supervisor_config& config, EventCallback onEvent = {})
        : onEvent_(std::make_unique<EventCallback>(std::move(onEvent))) {
        rcs_supervisor_callbacks table = RCS_SUPERVISOR_CALLBACKS_INIT;
        table.user_data = onEvent_.get();
        if (*onEvent_) {
            table.on_event = [](const rcs_supervisor_event* event, void* data) noexcept {
                (*static_cast<EventCallback*>(data))(*event);
            };
        }
        check(rcs_supervisor_create(&config, &table, &handle_));
    }
    ~Supervisor() noexcept {
        if (rcs_supervisor_destroy(handle_) != RCS_OK) {
            std::terminate(); // Destroyed from its own callback.
        }
    }
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    rcs_result start() noexcept { return rcs_supervisor_start(handle_); }
    rcs_result stop() noexcept { return rcs_supervisor_stop(handle_); }
    // rcs_supervisor_update: the session (os_session), the restart delays and
    // the grace, also while running; the rest of `config` must be what the
    // supervisor was made with.
    rcs_result update(const rcs_supervisor_config& config) noexcept { return rcs_supervisor_update(handle_, &config); }
    rcs_supervisor_status status() const noexcept {
        rcs_supervisor_status value = RCS_SUPERVISOR_STATUS_INIT;
        rcs_supervisor_get_status(handle_, &value);
        return value;
    }
    rcs_supervisor handle() const noexcept { return handle_; }

private:
    std::unique_ptr<EventCallback> onEvent_;
    rcs_supervisor handle_ = nullptr;
};

} // namespace rcsdk
