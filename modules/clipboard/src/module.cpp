// "rcsdk.clipboard": the module's definition (rcsdk/module.h), its calls
// into the session and machine halves, and its public functions.
#include "machine_half.hpp"
#include "session_half.hpp"

#include <cstdio>

namespace rcsdk::clipboard {
namespace {

// The settings the session's chain carries; check() took them.
const rcs_clipboard_settings& settingsOf(const void* settings) {
    return *static_cast<const rcs_clipboard_settings*>(settings);
}

// The same with formats 0 as every format the clipboard carries.
rcs_clipboard_settings resolved(const void* settings) {
    rcs_clipboard_settings c = settingsOf(settings);
    if (!c.formats) {
        c.formats = c.target == RCS_CLIPBOARD_SYSTEM ? systemFormats() : allFormats;
    }
    return c;
}

std::string checked(const void* settings, uint32_t size, rcs_result& result) {
    result = RCS_ERROR_INVALID_ARGUMENT;
    if (!settings || size != sizeof(rcs_clipboard_settings) ||
        settingsOf(settings).struct_size != sizeof(rcs_clipboard_settings)) {
        return "rcs_clipboard_settings: initialise it with RCS_CLIPBOARD_SETTINGS_INIT from this library's headers, "
               "and pass its size";
    }
    const auto& c = settingsOf(settings);
    if (c.flow & ~uint32_t(RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE)) {
        return "rcs_clipboard_settings: flow takes RCS_CLIPBOARD_SEND and RCS_CLIPBOARD_RECEIVE";
    }
    if (c.target != RCS_CLIPBOARD_SYSTEM && c.target != RCS_CLIPBOARD_HOST) {
        return "rcs_clipboard_settings: unknown target";
    }
    if (c.formats & ~knownFormats) {
        return "rcs_clipboard_settings: formats must be (1u << rcs_clipboard_format) bits";
    }
    if (c.files_stand_in > 1) {
        return "rcs_clipboard_settings: files_stand_in must be 0 or 1";
    }
    const char* end = static_cast<const char*>(std::memchr(c.files_directory, 0, sizeof(c.files_directory)));
    const std::string_view directory(c.files_directory, end ? size_t(end - c.files_directory) : 0);
    if (!end || !utf8Characters(directory) || (!directory.empty() && !fullPath(directory))) {
        return "rcs_clipboard_settings: files_directory must be empty or a full path, UTF-8, with its NUL";
    }
    if (c.max_bytes < minBytes || c.max_bytes > maxBytes) {
        return "rcs_clipboard_settings: max_bytes must be 1 KiB..256 MiB";
    }
    if (c.check_interval_ms < 50 || c.check_interval_ms > 5000) {
        return "rcs_clipboard_settings: check_interval_ms must be 50..5 000";
    }
    if (c.send_existing > 1 || c.send_concealed > 1) {
        return "rcs_clipboard_settings: send_existing and send_concealed must be 0 or 1";
    }
    if (!c.flow) {
        result = RCS_OK;
        return {};
    }
    if (c.target == RCS_CLIPBOARD_HOST) {
        if ((c.flow & RCS_CLIPBOARD_RECEIVE) && !c.on_clipboard) {
            return "rcs_clipboard_settings: target RCS_CLIPBOARD_HOST with RCS_CLIPBOARD_RECEIVE needs on_clipboard, "
                   "where copies come";
        }
        result = RCS_OK;
        return {};
    }
    // Target SYSTEM: a sharer's agent of another process runs on this machine
    // too (what that process's clipboard does with files, it says itself).
    const uint32_t carried = systemFormats();
    if (!carried || (c.formats & ~(carried | filesFormat))) {
        result = RCS_ERROR_UNSUPPORTED;
        return carried ? "rcs_clipboard_settings: this system's clipboard does not carry every format asked for"
                       : "rcs_clipboard_settings: this system has no clipboard the module can use (target "
                         "RCS_CLIPBOARD_HOST works everywhere)";
    }
    if ((c.formats & filesFormat) && !systemCarriesFiles()) {
        result = RCS_ERROR_UNSUPPORTED;
        return "rcs_clipboard_settings: this system's clipboard does not carry files (FILES)";
    }
    result = RCS_OK;
    return {};
}

// The session half's state, made at its first use and kept for the session's
// life (the run's counters stay readable after it).
Hub& hub(rcs_module module) {
    if (auto* hub = static_cast<Hub*>(rcs_module_state(module))) {
        return *hub;
    }
    auto* made = new Hub(module);
    rcs_module_set_state(module, made, [](void* state) { delete static_cast<Hub*>(state); });
    return *made;
}

Machine& machine(rcs_module module) {
    return *static_cast<Machine*>(rcs_module_state(module));
}

// What a call does, its failures logged: they never reach the SDK.
template <class Call>
void guarded(rcs_module module, Call&& call) noexcept {
    try {
        call();
    } catch (const std::exception& error) {
        rcs_module_log(module, RCS_LOG_ERROR, error.what());
    }
}

std::span<const uint8_t> bytes(const uint8_t* data, uint32_t size) {
    return {data, size};
}

rcs_module_definition makeDefinition() {
    rcs_module_definition d{};
    d.struct_size = sizeof(d);
    d.id = RCS_CLIPBOARD_MODULE;
    d.version = moduleVersion;
    d.flags = RCS_MODULE_PEER | RCS_MODULE_MACHINE;
    d.roles = (1u << RCS_ROLE_SHARE) | (1u << RCS_ROLE_VIEW);
    d.max_message_bytes = uint32_t(maxMessageBytes);
    d.check = [](rcs_role, const void* settings, uint32_t size, char* reason, uint32_t capacity,
                 void*) -> rcs_result {
        rcs_result result = RCS_OK;
        const std::string why = checked(settings, size, result);
        if (result != RCS_OK && capacity) {
            std::snprintf(reason, capacity, "%s", why.c_str());
        }
        return result;
    };
    d.start = [](rcs_module module, rcs_role role, const void* settings, uint32_t, void*) {
        guarded(module, [&] { hub(module).start(role, resolved(settings)); });
    };
    d.update = [](rcs_module module, const void* settings, uint32_t, void*) {
        guarded(module, [&] { hub(module).update(resolved(settings)); });
    };
    d.peer_joined = [](rcs_module module, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void*) {
        guarded(module, [&] { hub(module).peerJoined(peer, bytes(hello, size)); });
    };
    d.peer_hello = [](rcs_module module, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void*) {
        guarded(module, [&] { hub(module).peerHello(peer, bytes(hello, size)); });
    };
    d.peer_left = [](rcs_module module, rcs_viewer_id peer, void*) {
        guarded(module, [&] { hub(module).peerLeft(peer); });
    };
    d.received = [](rcs_module module, rcs_viewer_id peer, const uint8_t* data, uint32_t size,
                    rcs_module_message_flags, void*) {
        guarded(module, [&] { hub(module).received(peer, bytes(data, size)); });
    };
    d.sent = [](rcs_module module, rcs_viewer_id peer, uint64_t tag, void*) {
        guarded(module, [&] { hub(module).sent(peer, tag); });
    };
    d.machine_joined = [](rcs_module module, void*) { guarded(module, [&] { hub(module).machineJoined(); }); };
    d.machine_left = [](rcs_module module, void*) { guarded(module, [&] { hub(module).machineLeft(); }); };
    d.from_machine = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] { hub(module).fromMachine(bytes(data, size)); });
    };
    d.stop = [](rcs_module module, void*) { guarded(module, [&] { hub(module).stop(); }); };
    d.machine_start = [](rcs_module module, rcs_role role, void*) {
        rcs_module_set_state(module, new Machine(module, role), [](void* state) {
            delete static_cast<Machine*>(state);
        });
    };
    d.from_session = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] { machine(module).fromSession(bytes(data, size)); });
    };
    d.machine_stop = [](rcs_module module, void*) { guarded(module, [&] { machine(module).stop(); }); };
    return d;
}

// The session's half of the module, or why not (the reason is set).
rcs_result find(rcs_session session, rcs_module& module) {
    if (!session) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    return rcs_module_find(session, RCS_CLIPBOARD_MODULE, &module);
}

} // namespace
} // namespace rcsdk::clipboard

using namespace rcsdk::clipboard;

extern "C" {

const rcs_module_definition* rcs_clipboard_module(void) {
    static const rcs_module_definition definition = makeDefinition();
    return &definition;
}

uint32_t rcs_clipboard_system_formats(void) {
    return systemFormats();
}

uint32_t rcs_clipboard_system_files(char* reason, uint32_t capacity) {
    std::string why;
    const uint32_t files = systemFiles(why);
    if (reason && capacity) {
        std::snprintf(reason, capacity, "%s", why.c_str());
    }
    return files;
}

rcs_result rcs_clipboard_send(rcs_session session, const rcs_clipboard_content* content) {
    rcs_module module = nullptr;
    if (!content || content->struct_size != sizeof(*content)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "Initialise rcs_clipboard_content with RCS_CLIPBOARD_CONTENT_INIT from this "
                                       "library's headers");
    }
    if (const rcs_result found = find(session, module); found != RCS_OK) {
        return found;
    }
    return rcs_module_run(
        module,
        [](rcs_module m, void* argument) -> rcs_result {
            try {
                return hub(m).send(*static_cast<const rcs_clipboard_content*>(argument));
            } catch (const std::exception& error) {
                return rcs_set_last_call_error(RCS_ERROR_INTERNAL, error.what());
            }
        },
        const_cast<rcs_clipboard_content*>(content));
}

rcs_result rcs_clipboard_get_peer(rcs_session session, rcs_viewer_id viewer, rcs_clipboard_peer* peer) {
    rcs_module module = nullptr;
    if (!peer || peer->struct_size != sizeof(*peer)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "Initialise rcs_clipboard_peer with RCS_CLIPBOARD_PEER_INIT from this "
                                       "library's headers");
    }
    if (const rcs_result found = find(session, module); found != RCS_OK) {
        return found;
    }
    struct Ask {
        rcs_viewer_id viewer;
        rcs_clipboard_peer* peer;
    } ask{viewer, peer};
    return rcs_module_run(
        module,
        [](rcs_module m, void* argument) -> rcs_result {
            const auto& a = *static_cast<Ask*>(argument);
            return hub(m).peer(a.viewer, *a.peer);
        },
        &ask);
}

rcs_result rcs_clipboard_get_stats(rcs_session session, rcs_viewer_id viewer, rcs_clipboard_stats* stats) {
    rcs_module module = nullptr;
    if (!stats || stats->struct_size != sizeof(*stats)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "Initialise rcs_clipboard_stats with RCS_CLIPBOARD_STATS_INIT from this "
                                       "library's headers");
    }
    if (const rcs_result found = find(session, module); found != RCS_OK) {
        return found;
    }
    struct Ask {
        rcs_viewer_id viewer;
        rcs_clipboard_stats* stats;
    } ask{viewer, stats};
    return rcs_module_run(
        module,
        [](rcs_module m, void* argument) -> rcs_result {
            const auto& a = *static_cast<Ask*>(argument);
            return hub(m).stats(a.viewer, *a.stats);
        },
        &ask);
}

} // extern "C"
