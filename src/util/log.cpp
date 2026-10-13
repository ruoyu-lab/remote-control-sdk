#include "util/log.hpp"

#include "util/util.hpp"

#include <mutex>

namespace rcsdk::log {
namespace {

std::atomic<rcs_log_level> processLevel{0}; // 0: no callback.
std::mutex mutex;                           // Serialises calls, callback changes and ends.
rcs_log_callback hostCallback = nullptr;
void* hostUserData = nullptr;
thread_local SourceRef threadSource;
thread_local bool inCallback = false;

// The most detailed level a source writes: its own (0: none), or the process's.
rcs_log_level limit(rcs_log_level own, rcs_log_level process) noexcept {
    return !process ? 0 : own ? own : process;
}
rcs_log_level limit(const Source* source, rcs_log_level process) noexcept {
    return limit(source ? source->level.load(std::memory_order_relaxed) : 0, process);
}

} // namespace

SourceRef makeSource(rcs_log_source kind, void* handle, void* userData, rcs_log_level level) {
    return std::make_shared<Source>(kind, handle, userData, level);
}

void end(const SourceRef& source) noexcept {
    std::lock_guard lock(mutex);
    source->ended = true;
}

SourceRef current() noexcept {
    return threadSource;
}

Scope::Scope(SourceRef source) noexcept : previous_(std::exchange(threadSource, std::move(source))) {}

Scope::~Scope() {
    threadSource = std::move(previous_);
}

bool writing() noexcept {
    return inCallback;
}

rcs_result setCallback(rcs_log_level maxLevel, rcs_log_callback newCallback, void* newUserData) noexcept {
    if (inCallback) {
        return RCS_ERROR_CALLBACK_THREAD;
    }
    std::lock_guard lock(mutex);
    hostCallback = newCallback;
    hostUserData = newUserData;
    processLevel = newCallback ? maxLevel : 0;
    return RCS_OK;
}

bool enabled(rcs_log_level messageLevel) noexcept {
    return messageLevel <= limit(threadSource.get(), processLevel.load(std::memory_order_relaxed));
}

void write(rcs_log_level messageLevel, const char* module, const std::string& message) noexcept {
    if (!enabled(messageLevel) || inCallback) {
        return;
    }
    const Source* source = threadSource.get();
    std::lock_guard lock(mutex);
    if (!hostCallback || messageLevel > limit(source, processLevel.load())) {
        return;
    }
    rcs_log_record record{}; // PROCESS unless its source says otherwise.
    record.struct_size = sizeof(record);
    record.level = messageLevel;
    record.module = module;
    record.message = message.c_str();
    record.timestamp_ns = monotonicNs();
    if (source && !source->ended) {
        record.source = source->kind;
        record.user_data = source->userData;
        switch (source->kind) {
        case RCS_LOG_SOURCE_SESSION:
            record.session = static_cast<rcs_session>(source->handle);
            break;
        case RCS_LOG_SOURCE_AGENT:
            record.agent = static_cast<rcs_agent>(source->handle);
            break;
        case RCS_LOG_SOURCE_SUPERVISOR:
            record.supervisor = static_cast<rcs_supervisor>(source->handle);
            break;
        default:
            break;
        }
    }
    inCallback = true;
    hostCallback(&record, hostUserData);
    inCallback = false;
}

void write(const rcs_log_record& given, rcs_log_level own) noexcept {
    if (inCallback) {
        return;
    }
    std::lock_guard lock(mutex);
    if (!hostCallback || given.level > limit(own, processLevel.load())) {
        return;
    }
    rcs_log_record record = given;
    if (!record.timestamp_ns) {
        record.timestamp_ns = monotonicNs();
    }
    inCallback = true;
    hostCallback(&record, hostUserData);
    inCallback = false;
}

} // namespace rcsdk::log
