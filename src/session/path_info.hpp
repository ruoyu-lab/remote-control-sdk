// A connection's path as the host sees it (rcs_path_info): its addresses as
// text, and the network of this end's that carries it, as the system says;
// and which of this end's networks its sockets use.
#pragma once

#include "protocol/connection.hpp"
#include "session/config.hpp"

#include <mutex>

namespace rcsdk::session {

// Looks up the interface (and, where the system says, its kind) that holds
// the address the path's socket sends from: for an unbound socket, the
// address of the system's route to the path's first hop.
rcs_path_info describePath(const protocol::PathEnd& end);

// rcs_list_networks: this machine's networks that are up with a usable
// address. Throws Failure: UNSUPPORTED without sockets (a browser), NETWORK
// when the system's list cannot be read.
std::vector<rcs_network_info> networks();

// rcs_list_wifi: the Wi-Fi networks this machine is connected to, each with
// other_channel worked out (AWDL up, on a 2.4 or 5 GHz channel it does not
// keep to). Throws Failure: UNSUPPORTED without sockets (a browser). The
// one of them `path` goes over (by its interface), if any (nothing where the
// system says nothing; never throws).
std::vector<rcs_wifi_info> wifi();
rcs_wifi_info wifiUnder(const rcs_path_info& path);

// The network this end's sockets use, the same for every connection of the
// session: the interface of the one the settings choose (rcs_paths_config:
// excluded_networks, policy STAY; rcs_network_tuning_config.local_interface),
// bound to it, or none: the system's routes choose, and an unbound socket
// follows the default network by itself. With rcs_paths_config.standby,
// another network for a standby path: allowed, no tunnel, not the one in use
// (the chosen one, or the system's default). A sharer that listens (UDP,
// TCP) is reached where it listens, and the host's channel goes where the
// host's does: neither chooses.
class NetworkChoice {
public:
    struct Choice {
        uint64_t version = 0;                           // One more with each change.
        uint32_t index = 0;                             // The chosen network's interface; 0: none (unbound, or `lost`).
        bool lost = false;                              // No network is left to use: `why`.
        rcs_path_cause cause = RCS_PATH_CAUSE_SETTINGS; // What changed it: SETTINGS, or NETWORK (the one in use went).
        std::string why;
        // The standby path's network (0: none: `standbyWhy`, empty while standby is off).
        uint64_t standbyVersion = 0;
        uint32_t standbyIndex = 0;
        std::string standbyWhy;
    };
    // The choice now, looked at again when the networks
    // (transport::networkGeneration) or the settings (`settings`: their
    // version) changed since the last look.
    Choice look(const SessionConfig& live, uint64_t settings);

private:
    std::mutex mutex_;
    Choice choice_;
    uint64_t networks_ = ~uint64_t(0), settings_ = ~uint64_t(0);
    uint32_t standby_ = 0; // rcs_paths_config.standby as last looked at.
};

} // namespace rcsdk::session
