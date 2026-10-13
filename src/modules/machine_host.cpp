#include "modules/machine_host.hpp"

#include "util/log.hpp"

namespace rcsdk::modules {

MachineHost::MachineHost(rcs_role role, Output output) : role_(role), output_(std::move(output)) {}

MachineHost::~MachineHost() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        if (!thread_.joinable()) {
            return;
        }
    }
    wake_.notify_one();
    thread_.join();
}

void MachineHost::start(const std::string& id, uint32_t version) {
    queue({Task::start, id, version, {}});
}

void MachineHost::stop(const std::string& id) {
    queue({Task::stop, id, 0, {}});
}

void MachineHost::fromSession(const std::string& id, Bytes bytes) {
    queue({Task::data, id, 0, std::move(bytes)});
}

void MachineHost::toSession(const rcs_module_impl& half, std::span<const uint8_t> bytes) {
    output_.data(half.definition.id, Bytes(bytes.begin(), bytes.end()));
}

void MachineHost::queue(Task task) {
    {
        std::lock_guard lock(mutex_);
        if (quit_) {
            return;
        }
        tasks_.push_back(std::move(task));
        if (!thread_.joinable()) {
            thread_ = log::thread([this] { run(); });
        }
    }
    wake_.notify_one();
}

void MachineHost::run() noexcept {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !tasks_.empty(); });
            if (quit_) {
                break;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        handle(task);
    }
    // Every half that still runs stops (in the order of their ids).
    for (auto& [id, half] : halves_) {
        if (const auto stop = half->definition.table.machine_stop) {
            call(*half, [&] { stop(half.get(), half->definition.table.user_data); });
        }
    }
    halves_.clear();
}

void MachineHost::handle(Task& task) {
    const auto found = halves_.find(task.id);
    switch (task.kind) {
    case Task::start: {
        if (found != halves_.end()) {
            return; // It runs: the session asked again (an agent's link replays what it asked).
        }
        const Definition* definition = modules::find(task.id);
        std::string problem;
        if (!definition) {
            problem = "This process has not registered module \"" + task.id + "\" (rcs_register_module)";
        } else if (definition->table.version != task.version) {
            problem = "This process has version " + std::to_string(definition->table.version) + " of module \"" +
                      task.id + "\", the session " + std::to_string(task.version);
        } else if (!definition->machine() || !(definition->table.roles & (1u << role_))) {
            problem = "Module \"" + task.id + "\" has no machine's half here (RCS_MODULE_MACHINE, roles)";
        }
        if (!problem.empty()) {
            log::warning("modules", problem);
            output_.status(task.id, RCS_ERROR_UNSUPPORTED, problem);
            return;
        }
        auto half = std::make_unique<rcs_module_impl>(*definition, log::current());
        half->host = this;
        auto& started = *half;
        halves_.emplace(task.id, std::move(half));
        // Before the half's first call, so that the session hears it runs
        // before any message of it; what the session sends then comes after.
        output_.status(task.id, RCS_OK, {});
        if (const auto start = definition->table.machine_start) {
            call(started, [&] { start(&started, role_, definition->table.user_data); });
        }
        return;
    }
    case Task::stop:
        if (found != halves_.end()) {
            auto& half = *found->second;
            if (const auto stop = half.definition.table.machine_stop) {
                call(half, [&] { stop(&half, half.definition.table.user_data); });
            }
            halves_.erase(found);
        }
        return;
    case Task::data:
        if (found != halves_.end()) {
            auto& half = *found->second;
            if (const auto receive = half.definition.table.from_session) {
                call(half, [&] {
                    receive(&half, task.bytes.data(), uint32_t(task.bytes.size()), half.definition.table.user_data);
                });
            }
        }
        return;
    }
}

} // namespace rcsdk::modules
