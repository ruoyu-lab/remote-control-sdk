// The process's modules (rcs_register_module): definitions copied once and
// kept for the process's life, so sessions and agents hold plain references.
#pragma once

#include <rcsdk/module.h>

#include <string>
#include <string_view>
#include <vector>

namespace rcsdk::modules {

struct Definition {
    std::string id;
    rcs_module_definition table{}; // table.id points at `id`.
    bool peer() const noexcept { return table.flags & RCS_MODULE_PEER; }
    bool machine() const noexcept { return table.flags & RCS_MODULE_MACHINE; }
};

// Throws Failure(INVALID_ARGUMENT) naming what is wrong with the definition.
void registerModule(const rcs_module_definition& definition);
// Null when no module has this id.
const Definition* find(std::string_view id) noexcept;
// In the order they were registered.
std::vector<const Definition*> registered();

// An id as rcsdk/module.h describes it.
bool validId(std::string_view id) noexcept;

} // namespace rcsdk::modules
