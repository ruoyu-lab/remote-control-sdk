// This machine's active network interfaces and their addresses: ICE host
// candidates, and what a host may restrict a session to.
#pragma once

#include "transport/socket.hpp"

#include <string>
#include <vector>

namespace rcsdk::transport {

struct Interface {
    std::string name;                // macOS: BSD name ("en0"). Windows: adapter name ("Wi-Fi"), UTF-8.
    uint32_t index = 0;              // For binding sockets to it.
    std::vector<Endpoint> addresses; // Usable ones, port 0.
};

// Up, not loopback, with at least one usable address: IPv4, and IPv6 global
// or unique-local (not link-local, which needs an interface scope peers
// cannot use) that is not tentative, duplicated or deprecated; temporary IPv6
// addresses only where the interface has no stable one. Throws
// Failure(NETWORK) with the system's error when the list cannot be read.
std::vector<Interface> interfaces();
// Every usable address of every interface, once.
std::vector<Endpoint> localAddresses();
// The index of the interface among `list` that the system's route to the
// internet leaves by (its route to an address of the documentation's, IPv4
// first: nothing is sent); 0 when none does.
uint32_t defaultInterface(const std::vector<Interface>& list);
// The interface `name` names (its name, or its index as digits); throws
// Failure(NETWORK) listing the available ones when none does.
Interface findInterface(const std::string& name);

} // namespace rcsdk::transport
