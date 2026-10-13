// The agent's side of the link (agent/remote.hpp): connects to the session's
// endpoint, checks which process serves it, says what its machine offers, and
// then serves: the session's commands drive a MediaAgent on this machine, and
// what that reports goes back over the link. When the link ends the MediaAgent
// goes with it (its capture, its sound, its held keys); the server connects
// again, or ends.
#pragma once

#include "agent/media_agent.hpp"
#include "crypto/crypto.hpp"
#include "transport/local_stream.hpp"
#include "util/util.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rcsdk::agent {

class AgentServer {
public:
    struct Options {
        std::string endpoint;
        crypto::Key key{};
        // Which sessions it serves (rcs_agent_config.access): privileged, one
        // whose process runs as this process's account, LocalSystem or root, or
        // an elevated administrator; anyUser, any that holds the key.
        transport::LocalAccess access = transport::LocalAccess::privileged;
        bool reconnect = true; // Connect again when the link ends, until stopped.
        std::chrono::milliseconds reconnectInterval{1000};
        // The host's own encoder (rcs_agent_config's RCS_EXT_HOST_ENCODER).
        std::optional<rcs_host_encoder_config> hostEncoder;
    };
    // CONNECTING (looking for the session), RUNNING (attached), RECONNECTING
    // (the link ended), STOPPED, FAILED. `result` says why where it matters.
    using StateHandler = std::function<void(rcs_state state, rcs_result result)>;

    AgentServer(Options options, StateHandler handler);
    ~AgentServer();
    AgentServer(const AgentServer&) = delete;
    AgentServer& operator=(const AgentServer&) = delete;

    // False while it runs; starts anew after it ended.
    bool start();
    // Not from the state handler (it joins the thread that calls it).
    void stop();
    rcs_state state() const;
    // Why the last link failed or the server ended, else RCS_OK.
    std::pair<rcs_result, std::string> failure() const;
    // Blocks until the server has ended (STOPPED or FAILED) and the state
    // handler has heard it; false at the timeout, none waits without limit.
    bool wait(std::optional<std::chrono::milliseconds> timeout);

private:
    class LinkEvents;
    void run();
    // One attachment: connects, says hello, serves until the link ends, and
    // throws why. `attached` is set once it served.
    [[noreturn]] void attachment(const std::shared_ptr<std::atomic<bool>>& cancel, bool& attached);
    // Throws Failure(AUTHENTICATION) for a session this agent does not serve.
    void checkSession(const transport::LocalPeer& session) const;
    void setState(rcs_state state, rcs_result result);
    bool ended() const; // Holding mutex_.

    const Options options_;
    const StateHandler handler_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    rcs_state state_ = RCS_STATE_IDLE;
    rcs_result result_ = RCS_OK;
    bool ended_ = false; // The run ended, and its handler heard it.
    std::string message_;
    bool stopRequested_ = false;
    std::shared_ptr<std::atomic<bool>> linkCancel_; // The current link's: set to end its blocked calls.
    std::thread thread_;
};

} // namespace rcsdk::agent
