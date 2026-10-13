// Changes of this machine's networks, as a count: the system's watcher
// (platform::watchNetworks, while a session runs) and the host
// (rcs_notify_network_change) move it on; connections look at it as they go
// and check their path when it moved.
#pragma once

#include <cstdint>

namespace rcsdk::transport {

uint64_t networkGeneration() noexcept;
void networkChanged() noexcept;

} // namespace rcsdk::transport
