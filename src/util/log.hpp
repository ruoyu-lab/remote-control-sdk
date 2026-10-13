// The host's log callback (rcs_set_log_callback). Messages are for rare events;
// check enabled() before building an expensive one.
//
// Every message belongs to the source of the thread that writes it: the
// session, agent or supervisor that thread works for, or the process. The C
// API sets the source for the duration of each call on an object (Scope);
// threads the SDK starts take the source of the thread that starts them
// (log::thread), and callbacks the system makes on threads of its own write
// with the source of the code that handed them over (log::bound).
#pragma once

#include "util/api.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace rcsdk::log {

// A session, agent or supervisor as its messages name it.
struct Source {
    Source(rcs_log_source kind, void* handle, void* userData, rcs_log_level level) noexcept
        : kind(kind), handle(handle), userData(userData), level(level) {}

    const rcs_log_source kind;
    void* const handle;   // rcs_session, rcs_agent or rcs_supervisor.
    void* const userData; // Of its callbacks.
    std::atomic<rcs_log_level> level; // Its own maximum; 0 follows the process.
    bool ended = false;               // Its handle is gone (the log's mutex).
};
using SourceRef = std::shared_ptr<Source>;

SourceRef makeSource(rcs_log_source kind, void* handle, void* userData, rcs_log_level level);
// Once its object is destroyed: later messages of its threads belong to the
// process, and none being written still names it when this returns.
void end(const SourceRef& source) noexcept;

// The source of this thread's messages; null for the process.
SourceRef current() noexcept;

// Makes `source` this thread's while it lives.
class Scope {
public:
    explicit Scope(SourceRef source) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    SourceRef previous_;
};

// A thread whose messages belong to the source of the thread starting it.
template <class Function>
std::thread thread(Function&& function) {
    return std::thread([source = current(), function = std::forward<Function>(function)]() mutable {
        const Scope scope(std::move(source));
        function();
    });
}

// `function`, writing with the source of the thread that binds it: for a
// callback handed to code that calls it on threads of its own (the system's
// audio capture, for one).
template <class Function>
auto bound(Function function) {
    return [source = current(), function = std::move(function)](auto&&... args) mutable -> decltype(auto) {
        const Scope scope(source);
        return function(std::forward<decltype(args)>(args)...);
    };
}

rcs_result setCallback(rcs_log_level maxLevel, rcs_log_callback callback, void* userData) noexcept;
// Whether this thread is in the host's log callback.
bool writing() noexcept;

bool enabled(rcs_log_level level) noexcept;
void write(rcs_log_level level, const char* module, const std::string& message) noexcept;
// A record of a library built on the SDK (rcs_log_write), under the same
// rules: `own` is the level of the session or agent it names (0: none).
void write(const rcs_log_record& record, rcs_log_level own) noexcept;

inline void error(const char* module, const std::string& message) noexcept {
    write(RCS_LOG_ERROR, module, message);
}
inline void warning(const char* module, const std::string& message) noexcept {
    write(RCS_LOG_WARNING, module, message);
}
inline void info(const char* module, const std::string& message) noexcept {
    write(RCS_LOG_INFO, module, message);
}
inline void debug(const char* module, const std::string& message) noexcept {
    write(RCS_LOG_DEBUG, module, message);
}

} // namespace rcsdk::log
