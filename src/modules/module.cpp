#include "modules/module.hpp"

#include <algorithm>

rcs_module_impl::~rcs_module_impl() {
    if (release) {
        release(state);
    }
}

namespace rcsdk::modules {
namespace {

thread_local std::vector<const rcs_module_impl*> calling; // This thread's calls into modules, innermost last.
// What this thread's calls into modules left for when they returned: only
// the thread that made the call runs them, once it is in no module's call (a
// module that calls another's public functions holds its own calls
// meanwhile), so they never come while a call they came from still runs.
thread_local std::vector<std::pair<const rcs_module_impl*, std::function<void()>>> after;

void runAfter() noexcept {
    while (!after.empty()) {
        auto [module, action] = std::move(after.front());
        after.erase(after.begin());
        try {
            action();
        } catch (const std::exception& error) {
            log::error("modules", std::string("After a call into ") + module->definition.id + ": " + error.what());
        }
    }
}

} // namespace

bool inCall(const rcs_module_impl& module) noexcept {
    return std::find(calling.begin(), calling.end(), &module) != calling.end();
}

void call(rcs_module_impl& module, const std::function<void()>& body) noexcept {
    {
        std::lock_guard lock(module.calls);
        const log::Scope scope(module.logSource);
        calling.push_back(&module);
        try {
            body();
        } catch (const std::exception& error) {
            log::error(module.definition.id.c_str(), std::string("A call into the module threw: ") + error.what());
        } catch (...) {
            log::error(module.definition.id.c_str(), "A call into the module threw");
        }
        calling.pop_back();
    }
    if (calling.empty()) {
        runAfter();
    }
}

void afterCall(rcs_module_impl& module, std::function<void()> action) {
    if (calling.empty()) {
        action();
        return;
    }
    after.emplace_back(&module, std::move(action));
}

} // namespace rcsdk::modules
