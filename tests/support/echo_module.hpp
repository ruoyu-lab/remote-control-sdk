// Modules of the tests' (rcsdk/module.h), on the public interface only. Each
// records every call into it (with the half it went to), checks that calls
// into one half never overlap, and answers:
//   session half  "ping:x" from its kind with "pong:x"; "emit" with an event
//                 of its own (kind 300, payload "payload") and records what an
//                 event of a kind below RCS_MODULE_EVENT_OWN gets ("emit-low");
//                 its settings are text, refused when "refuse".
//   machine half  each message of the session half with "machine:" and it.
// Registered once per id in the process.
#pragma once

#include <rcsdk/module.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rcsdk::test {

class EchoModule {
public:
    struct Call {
        rcs_module module = nullptr;
        std::string what; // "start", "update", "peer_joined", "received", "machine_start"...
        rcs_viewer_id peer = 0;
        std::string data; // The settings, hello, message, or role ("share", "view").
        uint32_t flags = 0;
    };

    EchoModule(const char* id, uint32_t flags, uint32_t maxMessage) : id_(id) {
        auto& d = definition;
        d.struct_size = sizeof(d);
        d.id = id_.c_str();
        d.version = 1;
        d.flags = flags;
        d.roles = (1u << RCS_ROLE_SHARE) | (1u << RCS_ROLE_VIEW);
        d.max_message_bytes = maxMessage;
        d.user_data = this;
        d.check = [](rcs_role, const void* settings, uint32_t size, char* reason, uint32_t capacity,
                     void*) -> rcs_result {
            if (size == 6 && std::memcmp(settings, "refuse", 6) == 0) {
                std::snprintf(reason, capacity, "%s", "the test refuses these settings");
                return RCS_ERROR_INVALID_ARGUMENT;
            }
            return RCS_OK;
        };
        d.start = [](rcs_module m, rcs_role role, const void* settings, uint32_t size, void* self) {
            auto& e = of(self);
            const Inside inside(e, m);
            e.record(m, "start", 0, text(settings, size), role);
            const std::string hello = std::string("hello from ") + (role == RCS_ROLE_SHARE ? "share" : "view");
            rcs_module_set_hello(m, reinterpret_cast<const uint8_t*>(hello.data()), uint32_t(hello.size()));
        };
        d.update = [](rcs_module m, const void* settings, uint32_t size, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "update", 0, text(settings, size));
        };
        d.peer_joined = [](rcs_module m, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "peer_joined", peer, text(hello, size));
        };
        d.peer_hello = [](rcs_module m, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "peer_hello", peer, text(hello, size));
        };
        d.peer_left = [](rcs_module m, rcs_viewer_id peer, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "peer_left", peer, {});
        };
        d.received = [](rcs_module m, rcs_viewer_id peer, const uint8_t* data, uint32_t size, uint32_t flags,
                        void* self) {
            auto& e = of(self);
            const Inside inside(e, m);
            const std::string message = text(data, size);
            e.record(m, "received", peer, message.size() > 64 ? "large:" + std::to_string(message.size()) : message,
                     flags);
            if (message.rfind("ping:", 0) == 0) {
                const std::string answer = "pong:" + message.substr(5);
                rcs_module_send(m, peer, reinterpret_cast<const uint8_t*>(answer.data()), uint32_t(answer.size()), 0,
                                7);
            } else if (message == "emit") {
                rcs_module_emit(m, peer, 300, "payload", 7);
                if (rcs_module_emit(m, peer, 5, nullptr, 0) == RCS_ERROR_INVALID_ARGUMENT) {
                    e.record(m, "emit-low", peer, {});
                }
            }
        };
        d.sent = [](rcs_module m, rcs_viewer_id peer, uint64_t tag, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "sent", peer, std::to_string(tag));
        };
        d.room = [](rcs_module m, rcs_viewer_id peer, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "room", peer, {});
        };
        d.machine_joined = [](rcs_module m, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "machine_joined", 0, {});
        };
        d.machine_left = [](rcs_module m, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "machine_left", 0, {});
        };
        d.from_machine = [](rcs_module m, const uint8_t* data, uint32_t size, void* self) {
            const Inside inside(of(self), m);
            const std::string message = text(data, size);
            of(self).record(m, "from_machine", 0,
                            message.size() > 64 ? "large:" + std::to_string(message.size()) : message);
        };
        d.stop = [](rcs_module m, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "stop", 0, {});
        };
        d.machine_start = [](rcs_module m, rcs_role role, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "machine_start", 0, role == RCS_ROLE_SHARE ? "share" : "view");
        };
        d.from_session = [](rcs_module m, const uint8_t* data, uint32_t size, void* self) {
            const Inside inside(of(self), m);
            std::string answer = "machine:" + text(data, size);
            of(self).record(m, "from_session", 0,
                            answer.size() > 64 ? "large:" + std::to_string(size) : answer.substr(8));
            rcs_module_send_machine(m, reinterpret_cast<const uint8_t*>(answer.data()), uint32_t(answer.size()));
        };
        d.machine_stop = [](rcs_module m, void* self) {
            const Inside inside(of(self), m);
            of(self).record(m, "machine_stop", 0, {});
        };
    }

    rcs_module_definition definition{};
    std::atomic<bool> overlapped{false};

    std::vector<Call> calls(const std::string& what) {
        std::lock_guard lock(mutex_);
        std::vector<Call> found;
        for (const auto& call : calls_) {
            if (call.what == what) {
                found.push_back(call);
            }
        }
        return found;
    }
    // A call of this kind into a half of `session`'s (null: any) with this data (empty: any).
    bool saw(const std::string& what, rcs_session session = nullptr, const std::string& data = {}) {
        for (const auto& call : calls(what)) {
            if ((!session || rcs_module_session(call.module) == session) && (data.empty() || call.data == data)) {
                return true;
            }
        }
        return false;
    }
    size_t count(const std::string& what, rcs_session session = nullptr) {
        size_t n = 0;
        for (const auto& call : calls(what)) {
            n += !session || rcs_module_session(call.module) == session;
        }
        return n;
    }
    void clear() {
        std::lock_guard lock(mutex_);
        calls_.clear();
    }

private:
    // A call under way: one into the same half meanwhile overlaps it.
    struct Inside {
        Inside(EchoModule& module, rcs_module half) : module_(module), half_(half) {
            std::lock_guard lock(module_.mutex_);
            if (++module_.inside_[half_] > 1) {
                module_.overlapped = true;
            }
        }
        ~Inside() {
            std::lock_guard lock(module_.mutex_);
            --module_.inside_[half_];
        }
        EchoModule& module_;
        rcs_module half_;
    };

    static EchoModule& of(void* self) { return *static_cast<EchoModule*>(self); }
    static std::string text(const void* data, uint32_t size) {
        return data ? std::string(static_cast<const char*>(data), size) : std::string();
    }
    void record(rcs_module m, const char* what, rcs_viewer_id peer, std::string data, uint32_t flags = 0) {
        std::lock_guard lock(mutex_);
        calls_.push_back({m, what, peer, std::move(data), flags});
    }

    const std::string id_;
    std::mutex mutex_;
    std::vector<Call> calls_;
    std::map<rcs_module, int> inside_;
};

// The module `id` of the tests', registered at its first use.
inline EchoModule& echoModule(const char* id = "test.echo",
                              uint32_t flags = RCS_MODULE_PEER | RCS_MODULE_MACHINE,
                              uint32_t maxMessage = 32u << 20) {
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<EchoModule>> modules;
    std::lock_guard lock(mutex);
    auto& module = modules[id];
    if (!module) {
        module = std::make_unique<EchoModule>(id, flags, maxMessage);
        if (rcs_register_module(&module->definition) != RCS_OK) {
            std::fprintf(stderr, "Registering %s failed\n", id);
            std::abort();
        }
    }
    return *module;
}

} // namespace rcsdk::test
