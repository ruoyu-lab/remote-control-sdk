// Where the machine's halves of modules run (RCS_MODULE_MACHINE): a sharer's
// agent (MediaAgent, in the session's process or in an agent's), a viewer's
// own process (its ModuleSet). A half starts when its session's half asks,
// and each call into it comes on the host's thread, in the order asked; it
// answers through Output: whether it runs, and its messages to the session.
#pragma once

#include "modules/module.hpp"
#include "platform/platform.hpp"
#include "util/util.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace rcsdk::modules {

class MachineHost {
public:
    struct Output {
        // A half started (RCS_OK), or why it cannot (UNSUPPORTED: this
        // process has no such module, or another version).
        std::function<void(const std::string& id, rcs_result result, const std::string& message)> status;
        // A half's message to its session's half, in order (on its thread, or the host's).
        std::function<void(const std::string& id, Bytes bytes)> data;
        // What the session shares on this machine now (rcs_module_get_shared),
        // as remote input is set up there; none before that, and on a viewer's.
        std::function<std::optional<platform::CaptureTarget>()> shared;
    };

    // `role`: the session's whose machine this is.
    MachineHost(rcs_role role, Output output);
    // Stops every half that runs.
    ~MachineHost();
    MachineHost(const MachineHost&) = delete;
    MachineHost& operator=(const MachineHost&) = delete;

    // Queued for the host's thread; the first start starts it.
    void start(const std::string& id, uint32_t version);
    void stop(const std::string& id);
    void fromSession(const std::string& id, Bytes bytes);

    // rcs_module_send_machine of a half.
    void toSession(const rcs_module_impl& half, std::span<const uint8_t> bytes);
    // rcs_module_get_shared of a half.
    std::optional<platform::CaptureTarget> shared() const { return output_.shared ? output_.shared() : std::nullopt; }

private:
    struct Task {
        enum Kind { start, stop, data } kind = start;
        std::string id;
        uint32_t version = 0;
        Bytes bytes;
    };
    void queue(Task task);
    void run() noexcept;
    void handle(Task& task);

    const rcs_role role_;
    const Output output_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> tasks_;
    bool quit_ = false;
    std::map<std::string, std::unique_ptr<rcs_module_impl>> halves_; // The thread's.
    std::thread thread_;
};

} // namespace rcsdk::modules
