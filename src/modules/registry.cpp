#include "modules/registry.hpp"

#include "util/util.hpp"

#include <deque>
#include <mutex>

namespace rcsdk::modules {
namespace {

std::mutex mutex;
std::deque<Definition> definitions; // A deque: references stay valid as it grows.

} // namespace

bool validId(std::string_view id) noexcept {
    if (id.empty() || id.size() >= RCS_MODULE_ID_MAX) {
        return false;
    }
    for (const char c : id) {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!letter && !(c >= '0' && c <= '9') && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

void registerModule(const rcs_module_definition& d) {
    if (d.struct_size != sizeof(d)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_definition: struct_size must be sizeof(rcs_module_definition) "
                                         "of this library's headers (" RCSDK_VERSION_STRING ")");
    }
    if (!d.id || !validId(d.id)) {
        fail(RCS_ERROR_INVALID_ARGUMENT,
             "rcs_module_definition: id must be 1..63 letters, digits, dots and dashes");
    }
    if (d.flags & ~uint32_t(RCS_MODULE_PEER | RCS_MODULE_MACHINE)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_definition: flags takes RCS_MODULE_PEER and RCS_MODULE_MACHINE");
    }
    constexpr uint32_t roles = (1u << RCS_ROLE_SHARE) | (1u << RCS_ROLE_VIEW);
    if (!d.roles || (d.roles & ~roles)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_definition: roles must be (1u << RCS_ROLE_SHARE) and "
                                         "(1u << RCS_ROLE_VIEW) bits, not 0");
    }
    if (!d.max_message_bytes || d.max_message_bytes > RCS_MODULE_MAX_MESSAGE) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_definition: max_message_bytes must be 1..RCS_MODULE_MAX_MESSAGE");
    }
    std::lock_guard lock(mutex);
    for (const auto& known : definitions) {
        if (known.id == d.id) {
            fail(RCS_ERROR_INVALID_ARGUMENT, std::string("rcs_register_module: \"") + d.id + "\" is registered");
        }
    }
    auto& added = definitions.emplace_back();
    added.id = d.id;
    added.table = d;
    added.table.id = added.id.c_str();
}

const Definition* find(std::string_view id) noexcept {
    std::lock_guard lock(mutex);
    for (const auto& known : definitions) {
        if (known.id == id) {
            return &known;
        }
    }
    return nullptr;
}

std::vector<const Definition*> registered() {
    std::lock_guard lock(mutex);
    std::vector<const Definition*> list;
    for (const auto& known : definitions) {
        list.push_back(&known);
    }
    return list;
}

} // namespace rcsdk::modules
