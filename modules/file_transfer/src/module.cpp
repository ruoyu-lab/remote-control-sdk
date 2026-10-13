// "rcsdk.file-transfer": the module's definition (rcsdk/module.h), its calls
// into the session and machine halves, and its public functions.
#include "machine_half.hpp"
#include "session_half.hpp"

#include <cstdio>

namespace rcsdk::file_transfer {
namespace {

// The settings the session's chain carries; check() took them.
const rcs_file_transfer_settings& settingsOf(const void* settings) {
    return *static_cast<const rcs_file_transfer_settings*>(settings);
}

std::string checked(const void* settings, uint32_t size) {
    if (!settings || size != sizeof(rcs_file_transfer_settings) ||
        settingsOf(settings).struct_size != sizeof(rcs_file_transfer_settings)) {
        return "rcs_file_transfer_settings: initialise it with RCS_FILE_TRANSFER_SETTINGS_INIT from this library's "
               "headers, and pass its size";
    }
    const auto& c = settingsOf(settings);
    if (c.flow & ~uint32_t(RCS_FILE_SEND | RCS_FILE_RECEIVE)) {
        return "rcs_file_transfer_settings: flow takes RCS_FILE_SEND and RCS_FILE_RECEIVE";
    }
    if (c.target != RCS_FILE_SYSTEM && c.target != RCS_FILE_HOST) {
        return "rcs_file_transfer_settings: unknown target";
    }
    if (!c.max_items || c.max_items > maxItemsLimit) {
        return "rcs_file_transfer_settings: max_items must be 1..100 000";
    }
    if (!c.max_pending_offers || c.max_pending_offers > maxPendingOffersLimit) {
        return "rcs_file_transfer_settings: max_pending_offers must be 1..10 000";
    }
    // An offer of max_items fits while nothing else waits.
    if (c.max_pending_items < c.max_items || c.max_pending_items > maxPendingItemsLimit) {
        return "rcs_file_transfer_settings: max_pending_items must be max_items..1 000 000";
    }
    if (c.max_pending_items_total < c.max_items || c.max_pending_items_total > maxPendingItemsLimit) {
        return "rcs_file_transfer_settings: max_pending_items_total must be max_items..1 000 000";
    }
    if (!c.max_publications || c.max_publications > maxPublicationsLimit) {
        return "rcs_file_transfer_settings: max_publications must be 1..10 000";
    }
    if (!c.max_fetches || c.max_fetches > maxFetchesLimit) {
        return "rcs_file_transfer_settings: max_fetches must be 1..64";
    }
    if (c.offer_timeout_ms < 1000 || c.offer_timeout_ms > 3'600'000) {
        return "rcs_file_transfer_settings: offer_timeout_ms must be 1 000..3 600 000";
    }
    if (c.reconnect_wait_ms > 3'600'000) {
        return "rcs_file_transfer_settings: reconnect_wait_ms must be 0..3 600 000";
    }
    if (c.keep_partial > 1) {
        return "rcs_file_transfer_settings: keep_partial must be 0 or 1";
    }
    if (c.target == RCS_FILE_SYSTEM && (c.read || c.write || c.resume)) {
        return "rcs_file_transfer_settings: read, write and resume are for target RCS_FILE_HOST";
    }
    if (c.target == RCS_FILE_HOST && (c.flow & RCS_FILE_SEND) && !c.read) {
        return "rcs_file_transfer_settings: target RCS_FILE_HOST with RCS_FILE_SEND needs read";
    }
    if (c.target == RCS_FILE_HOST && (c.flow & RCS_FILE_RECEIVE) && !c.write) {
        return "rcs_file_transfer_settings: target RCS_FILE_HOST with RCS_FILE_RECEIVE needs write";
    }
    return {};
}

// The session half's state, made at its first use and kept for the session's
// life (its transfers stay readable after a run).
Hub& made(rcs_module module) {
    if (auto* hub = static_cast<Hub*>(rcs_module_state(module))) {
        return *hub;
    }
    auto* hub = new Hub(module);
    rcs_module_set_state(module, hub, [](void* state) { delete static_cast<Hub*>(state); });
    return *hub;
}

// For one of the SDK's calls.
Hub& hub(rcs_module module) {
    return made(module);
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
    d.id = RCS_FILE_TRANSFER_MODULE;
    d.version = moduleVersion;
    d.flags = RCS_MODULE_PEER | RCS_MODULE_MACHINE;
    d.roles = (1u << RCS_ROLE_SHARE) | (1u << RCS_ROLE_VIEW);
    d.max_message_bytes = maxMessageBytes;
    d.check = [](rcs_role, const void* settings, uint32_t size, char* reason, uint32_t capacity,
                 void*) -> rcs_result {
        const std::string why = checked(settings, size);
        if (why.empty()) {
            return RCS_OK;
        }
        if (capacity) {
            std::snprintf(reason, capacity, "%s", why.c_str());
        }
        return RCS_ERROR_INVALID_ARGUMENT;
    };
    d.start = [](rcs_module module, rcs_role role, const void* settings, uint32_t, void*) {
        guarded(module, [&] { hub(module).start(role, settingsOf(settings)); });
    };
    d.update = [](rcs_module module, const void* settings, uint32_t, void*) {
        guarded(module, [&] { hub(module).update(settingsOf(settings)); });
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
    d.room = [](rcs_module module, rcs_viewer_id peer, void*) { guarded(module, [&] { hub(module).room(peer); }); };
    d.machine_joined = [](rcs_module module, void*) { guarded(module, [&] { hub(module).machineJoined(); }); };
    d.machine_left = [](rcs_module module, void*) { guarded(module, [&] { hub(module).machineLeft(); }); };
    d.from_machine = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] { hub(module).fromMachine(bytes(data, size)); });
    };
    d.stop = [](rcs_module module, void*) { guarded(module, [&] { hub(module).stop(); }); };
    d.machine_start = [](rcs_module module, rcs_role, void*) {
        guarded(module, [&] {
            rcs_module_set_state(module, new Machine(module), [](void* state) { delete static_cast<Machine*>(state); });
        });
    };
    d.from_session = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] {
            if (auto* machine = static_cast<Machine*>(rcs_module_state(module))) {
                machine->fromSession(bytes(data, size));
            }
        });
    };
    d.machine_stop = [](rcs_module module, void*) { rcs_module_set_state(module, nullptr, nullptr); };
    return d;
}

// `call(hub)` as one of the module's calls in `session`, or why not.
template <class Call>
rcs_result onHub(rcs_session session, Call call) {
    if (!session) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    rcs_module module = nullptr;
    if (const rcs_result found = rcs_module_find(session, RCS_FILE_TRANSFER_MODULE, &module); found != RCS_OK) {
        return found;
    }
    return rcs_module_run(
        module,
        [](rcs_module m, void* argument) -> rcs_result {
            try {
                return (*static_cast<Call*>(argument))(made(m));
            } catch (const std::exception& error) {
                return rcs_set_last_call_error(RCS_ERROR_INTERNAL, error.what());
            }
        },
        &call);
}

rcs_result notInitialised(const char* type, const char* init) {
    return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, (std::string("Initialise ") + type + " with " + init +
                                                                " from this library's headers")
                                                                   .c_str());
}

} // namespace
} // namespace rcsdk::file_transfer

using namespace rcsdk::file_transfer;

extern "C" {

const rcs_module_definition* rcs_file_transfer_module(void) {
    static const rcs_module_definition definition = makeDefinition();
    return &definition;
}

rcs_result rcs_file_send(rcs_session session, const rcs_file_offer* offer, uint64_t* out_transfer) {
    if (!offer || offer->struct_size != sizeof(*offer)) {
        return notInitialised("rcs_file_offer", "RCS_FILE_OFFER_INIT");
    }
    if (!out_transfer) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "out_transfer is NULL");
    }
    return onHub(session, [&](Hub& h) { return h.send(*offer, *out_transfer); });
}

rcs_result rcs_file_accept(rcs_session session, uint64_t transfer, const char* directory,
                           rcs_file_conflict on_conflict) {
    return onHub(session, [&](Hub& h) { return h.accept(transfer, directory, on_conflict); });
}

rcs_result rcs_file_reject(rcs_session session, uint64_t transfer, const char* reason) {
    return onHub(session, [&](Hub& h) { return h.reject(transfer, reason); });
}

rcs_result rcs_file_cancel(rcs_session session, uint64_t transfer) {
    return onHub(session, [&](Hub& h) { return h.cancel(transfer); });
}

rcs_result rcs_file_pause(rcs_session session, uint64_t transfer, uint32_t paused) {
    return onHub(session, [&](Hub& h) { return h.pause(transfer, paused); });
}

rcs_result rcs_file_get_transfer(rcs_session session, uint64_t transfer, rcs_file_transfer_info* out) {
    if (!out || out->struct_size != sizeof(*out)) {
        return notInitialised("rcs_file_transfer_info", "RCS_FILE_TRANSFER_INFO_INIT");
    }
    return onHub(session, [&](Hub& h) { return h.info(transfer, *out); });
}

rcs_result rcs_file_get_items(rcs_session session, uint64_t transfer, uint32_t first, rcs_file_item_info* items,
                              uint32_t item_size, uint32_t capacity, uint32_t* out_count) {
    if (item_size != sizeof(rcs_file_item_info) || (!items && capacity) || !out_count) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "item_size sizeof(rcs_file_item_info), items for capacity, and out_count");
    }
    return onHub(session, [&](Hub& h) { return h.items(transfer, first, items, capacity, *out_count); });
}

rcs_result rcs_file_get_peer(rcs_session session, rcs_viewer_id viewer, rcs_file_peer* out) {
    if (!out || out->struct_size != sizeof(*out)) {
        return notInitialised("rcs_file_peer", "RCS_FILE_PEER_INIT");
    }
    return onHub(session, [&](Hub& h) { return h.peer(viewer, *out); });
}

rcs_result rcs_file_discard(rcs_session session, const char* directory, const uint8_t* token) {
    return onHub(session, [&](Hub& h) { return h.discard(directory, token); });
}

rcs_result rcs_file_publish(rcs_session session, const rcs_file_publication* publication, uint8_t* out_id) {
    if (!publication || publication->struct_size != sizeof(*publication)) {
        return notInitialised("rcs_file_publication", "RCS_FILE_PUBLICATION_INIT");
    }
    if (!out_id) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "out_id is NULL");
    }
    return onHub(session, [&](Hub& h) { return h.publish(*publication, out_id); });
}

rcs_result rcs_file_unpublish(rcs_session session, const uint8_t* publication, uint32_t flags) {
    return onHub(session, [&](Hub& h) { return h.unpublish(publication, flags); });
}

rcs_result rcs_file_fetch(rcs_session session, const rcs_file_fetch_request* request, uint64_t* out_transfer) {
    if (!request || request->struct_size != sizeof(*request)) {
        return notInitialised("rcs_file_fetch_request", "RCS_FILE_FETCH_REQUEST_INIT");
    }
    if (!out_transfer) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "out_transfer is NULL");
    }
    return onHub(session, [&](Hub& h) { return h.fetch(*request, *out_transfer); });
}

rcs_result rcs_file_read(rcs_session session, uint64_t transfer, uint32_t item, uint64_t offset, uint32_t size,
                         uint64_t tag) {
    return onHub(session, [&](Hub& h) { return h.read(transfer, item, offset, size, tag); });
}

rcs_result rcs_file_close(rcs_session session, uint64_t transfer, rcs_result outcome, const char* reason) {
    return onHub(session, [&](Hub& h) { return h.close(transfer, outcome, reason); });
}

} // extern "C"
