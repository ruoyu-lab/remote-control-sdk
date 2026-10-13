// One half of a module (rcs_module, rcsdk/module.h): the session's, owned by
// its session's ModuleSet for the session's life, or the machine's, owned by
// a MachineHost from machine_start to machine_stop. Calls into it never
// overlap (call()), and what it hands the host from inside one waits until
// that call returned, and any call into another module it was made from
// (afterCall()), so that a host waiting for a module in its own callback
// never waits for a module waiting for it.
#pragma once

#include "modules/registry.hpp"
#include "util/log.hpp"

#include <functional>
#include <mutex>
#include <vector>

namespace rcsdk::session {
class ModuleSet;
}
namespace rcsdk::modules {
class MachineHost;
}

struct rcs_module_impl {
    rcs_module_impl(const rcsdk::modules::Definition& definition, rcsdk::log::SourceRef logSource)
        : definition(definition), logSource(std::move(logSource)) {}
    ~rcs_module_impl();
    rcs_module_impl(const rcs_module_impl&) = delete;
    rcs_module_impl& operator=(const rcs_module_impl&) = delete;

    const rcsdk::modules::Definition& definition;
    const rcsdk::log::SourceRef logSource; // Whose its log lines are.
    // The session's half: its set, and its place there (its number in this
    // end's module list); the machine's half: its host.
    rcsdk::session::ModuleSet* set = nullptr;
    uint32_t number = 0;
    rcsdk::modules::MachineHost* host = nullptr;

    std::recursive_mutex calls; // Held during each call into it.
    std::mutex stateMutex;
    void* state = nullptr;
    void (*release)(void* state) = nullptr;
};

namespace rcsdk::modules {

// Whether this thread is inside one of `module`'s calls.
bool inCall(const rcs_module_impl& module) noexcept;

// Runs `body` (which calls into the module) as one of `module`'s calls: after
// the one that runs on another thread, nested in one of this thread's. Once
// the outermost returned, what it left for then runs. Nothing it throws gets
// out (a module's C++ that throws through a C callback): it is logged.
void call(rcs_module_impl& module, const std::function<void()>& body) noexcept;

// `action` once this thread's calls into modules returned (`module`'s, and
// those of a module that called its public functions); at once when it is in
// none.
void afterCall(rcs_module_impl& module, std::function<void()> action);

} // namespace rcsdk::modules
