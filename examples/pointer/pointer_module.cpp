// "com.example.pointer": the viewer's half sends where it points, the
// sharer's tells its host. Nine bytes a message: visible, then x and y,
// big-endian.
#include "pointer_module.h"

#include <cstdint>

namespace {

constexpr uint32_t messageBytes = 9;

const rcs_module_definition definition = [] {
    rcs_module_definition d{};
    d.struct_size = sizeof(d);
    d.id = EXAMPLE_POINTER_MODULE;
    d.version = 1;
    d.flags = RCS_MODULE_PEER;
    d.roles = (1u << RCS_ROLE_SHARE) | (1u << RCS_ROLE_VIEW);
    d.max_message_bytes = messageBytes;
    d.check = [](rcs_role, const void*, uint32_t size, char* reason, uint32_t capacity, void*) -> rcs_result {
        if (size == 0) {
            return RCS_OK;
        }
        if (capacity) {
            const char text[] = "com.example.pointer takes no settings";
            uint32_t i = 0;
            for (; i + 1 < capacity && text[i]; ++i) {
                reason[i] = text[i];
            }
            reason[i] = '\0';
        }
        return RCS_ERROR_INVALID_ARGUMENT;
    };
    // A sharer's: what a viewer sent, checked (it may be anyone's), to the host.
    d.received = [](rcs_module module, rcs_viewer_id peer, const uint8_t* data, uint32_t size, uint32_t, void*) {
        if (size != messageBytes || data[0] > 1) {
            rcs_module_log(module, RCS_LOG_WARNING, "A viewer sent a pointer this end does not read");
            return;
        }
        const auto number = [&](int at) {
            return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 |
                   uint32_t(data[at + 3]);
        };
        const example_pointer where{data[0], number(1), number(5)};
        if (where.x > 65535 || where.y > 65535) {
            return;
        }
        rcs_module_emit(module, peer, EXAMPLE_POINTER_MOVED, &where, sizeof(where));
    };
    return d;
}();

} // namespace

extern "C" {

const rcs_module_definition* example_pointer_module(void) {
    return &definition;
}

rcs_result example_pointer_point(rcs_session session, const example_pointer* where) {
    if (!where || where->visible > 1 || where->x > 65535 || where->y > 65535) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "visible 0 or 1, x and y 0..65535");
    }
    rcs_module module = nullptr;
    if (const rcs_result found = rcs_module_find(session, EXAMPLE_POINTER_MODULE, &module); found != RCS_OK) {
        return found;
    }
    const uint8_t message[messageBytes] = {uint8_t(where->visible),
                                           uint8_t(where->x >> 24), uint8_t(where->x >> 16),
                                           uint8_t(where->x >> 8),  uint8_t(where->x),
                                           uint8_t(where->y >> 24), uint8_t(where->y >> 16),
                                           uint8_t(where->y >> 8),  uint8_t(where->y)};
    return rcs_module_send(module, 0, message, messageBytes, RCS_MODULE_UNRELIABLE, 0);
}

} // extern "C"
