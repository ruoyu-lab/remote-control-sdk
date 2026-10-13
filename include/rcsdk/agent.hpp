// Remote Control SDK - C++ wrapper of the agent in another process and the
// supervisor's helpers (rcsdk/agent.h).
#pragma once

#include <rcsdk/agent.h>
#include <rcsdk/host_codec.hpp>
#include <rcsdk/rcsdk.hpp>

namespace rcsdk {

template <>
inline rcs_agent_link_config defaults<rcs_agent_link_config>() noexcept {
    return RCS_AGENT_LINK_CONFIG_INIT;
}

// Share with an agent link: decides which agents attach (see
// rcs_agent_link_config.on_agent_attach). It must outlive the session; attach
// must not throw (an escaping exception terminates the process).
class AgentGate {
public:
    virtual ~AgentGate() = default;
    // True takes the agent.
    virtual bool attach(const rcs_agent_info& agent) = 0;
};

// The capture, encoding, sound and input run in an agent of another process
// that attaches through this endpoint with this key (see rcs_agent_link_config
// and Agent); `gate` decides who attaches (none: every agent holding the key).
//   config.use(rcsdk::agentLink("myapp-agent", key));
inline rcs_agent_link_config agentLink(const std::string& endpoint, const ConnectionKey& key,
                                       rcs_agent_access access = RCS_AGENT_ACCESS_PRIVILEGED,
                                       AgentGate* gate = nullptr) {
    rcs_agent_link_config c = RCS_AGENT_LINK_CONFIG_INIT;
    detail::setText(c.endpoint, endpoint, "endpoint");
    std::copy(key.begin(), key.end(), c.key);
    c.access = access;
    if (gate) {
        c.user_data = gate;
        c.on_agent_attach = [](const rcs_agent_info* agent, void* data) noexcept -> int32_t {
            return static_cast<AgentGate*>(data)->attach(*agent) ? 1 : 0;
        };
    }
    return c;
}

// Share: the machine the session captures, as its agent reports it (all zero
// for a viewing session; see rcs_session_get_agent_info).
inline rcs_agent_info agentInfo(const Session& session) noexcept {
    rcs_agent_info value = RCS_AGENT_INFO_INIT;
    rcs_session_get_agent_info(session.handle(), &value);
    return value;
}

// ---- Agent -----------------------------------------------------------------

// Builds an rcs_agent_config and its extension chain: the session's endpoint
// and key, then what differs from the defaults.
//
//   auto config = rcsdk::AgentConfig("myapp-agent", key).setLogLevel(RCS_LOG_DEBUG);
class AgentConfig {
public:
    AgentConfig(const std::string& endpoint, const ConnectionKey& key) {
        detail::setText(config_.endpoint, endpoint, "endpoint");
        std::copy(key.begin(), key.end(), config_.key);
    }

    // Which sessions it serves (rcs_agent_access).
    AgentConfig& setAccess(rcs_agent_access access) noexcept {
        config_.access = access;
        return *this;
    }
    // Whether it attaches again when the link ends, and how often it tries.
    AgentConfig& setReconnect(bool reconnect, uint32_t intervalMs = 1000) noexcept {
        config_.reconnect = reconnect ? 1u : 0u;
        config_.reconnect_interval_ms = intervalMs;
        return *this;
    }
    // How much it logs: 0 as the process (setLogCallback), or its own level.
    AgentConfig& setLogLevel(rcs_log_level level) {
        log().max_level = level;
        return *this;
    }
    // The host's own encoder (see hostEncoder()): it encodes here, in the
    // agent's process, and must outlive the agent.
    AgentConfig& setHostEncoder(const rcs_host_encoder_config& encoder) noexcept {
        hostEncoder_ = encoder;
        return *this;
    }

    rcs_agent_config& core() noexcept { return config_; }
    rcs_log_config& log() {
        if (!log_) {
            log_ = defaults<rcs_log_config>();
        }
        return *log_;
    }

    // The configuration with its chain, pointing into this object: valid until
    // it is changed, copied from or destroyed.
    const rcs_agent_config& get() const noexcept {
        const rcs_extension* head = nullptr;
        if (hostEncoder_) {
            hostEncoder_->header.next = head;
            head = &hostEncoder_->header;
        }
        if (log_) {
            log_->header.next = head;
            head = &log_->header;
        }
        config_.extensions = head;
        return config_;
    }

private:
    mutable rcs_agent_config config_ = RCS_AGENT_CONFIG_INIT;
    mutable std::optional<rcs_log_config> log_;
    mutable std::optional<rcs_host_encoder_config> hostEncoder_;
};

// An agent in a process of its own (see rcs_agent): attaches to the session
// that listens on `endpoint` with `key` (and that `access` takes), captures and
// encodes for it, and attaches again when the link ends. Not copyable or
// movable.
//
//   rcsdk::Agent agent(rcsdk::AgentConfig("myapp-agent", key), [](const rcs_agent_event& event) { ... });
//   agent.start();
//   agent.wait();
class Agent final {
public:
    using EventCallback = std::function<void(const rcs_agent_event& event)>;

    explicit Agent(const AgentConfig& config, EventCallback onEvent = {}) : Agent(config.get(), std::move(onEvent)) {}
    explicit Agent(const rcs_agent_config& config, EventCallback onEvent = {})
        : onEvent_(std::make_unique<EventCallback>(std::move(onEvent))) {
        rcs_agent_callbacks table = RCS_AGENT_CALLBACKS_INIT;
        table.user_data = onEvent_.get();
        if (*onEvent_) {
            table.on_event = [](const rcs_agent_event* event, void* data) noexcept {
                (*static_cast<EventCallback*>(data))(*event);
            };
        }
        check(rcs_agent_create(&config, &table, &handle_));
    }
    ~Agent() noexcept {
        if (rcs_agent_destroy(handle_) != RCS_OK) {
            std::terminate(); // Destroyed from its own callback.
        }
    }
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    rcs_result start() noexcept { return rcs_agent_start(handle_); }
    rcs_result stop() noexcept { return rcs_agent_stop(handle_); }
    // Until the agent has ended (STOPPED or FAILED) and onEvent heard it; TIMEOUT at the limit.
    rcs_result wait(uint32_t timeoutMs = UINT32_MAX) noexcept { return rcs_agent_wait(handle_, timeoutMs); }
    rcs_state state() const noexcept {
        rcs_state value = RCS_STATE_FAILED;
        rcs_agent_get_state(handle_, &value);
        return value;
    }
    SessionFailure failure() const {
        SessionFailure value;
        char message[1024] = {};
        rcs_agent_get_failure(handle_, &value.result, message, sizeof(message));
        value.message = message;
        return value;
    }
    rcs_agent handle() const noexcept { return handle_; }

private:
    std::unique_ptr<EventCallback> onEvent_;
    rcs_agent handle_ = nullptr;
};

// ---- A supervisor's helper (Windows) --------------------------------------

// In a helper the supervisor started: waits until it asks the helper to end
// (see rcs_helper_wait_for_stop); true when it did, false at the timeout.
// Throws Error(UNSUPPORTED) in a process no supervisor started.
inline bool waitForStopRequest(uint32_t timeoutMs = UINT32_MAX) {
    const rcs_result result = rcs_helper_wait_for_stop(timeoutMs);
    if (result == RCS_ERROR_TIMEOUT) {
        return false;
    }
    check(result);
    return true;
}

// In a helper the supervisor started: the key it was handed (see
// rcs_helper_get_key). Throws Error(UNSUPPORTED) where there is none.
inline ConnectionKey helperKey() {
    ConnectionKey key{};
    check(rcs_helper_get_key(key.data(), uint32_t(key.size())));
    return key;
}

} // namespace rcsdk
