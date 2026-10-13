#include "session/path_info.hpp"

#include "seams/seams.hpp"
#include "session/checks.hpp"
#include "transport/interfaces.hpp"
#include "transport/network_changes.hpp"
#include "util/api.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cstring>

namespace rcsdk::session {

rcs_path_info describePath(const protocol::PathEnd& end) {
    rcs_path_info info{};
    info.route = end.route;
    info.relay_transport = end.relayTransport;
    if (end.remote) {
        copyText(info.remote_address, end.remote->toString());
    }
    auto local = end.local;
    if (!local) {
        return info;
    }
    const auto address = local->addressBytes();
    if (end.hop && std::all_of(address.begin(), address.end(), [](uint8_t b) { return b == 0; })) {
        if (auto source = transport::sourceFor(*end.hop)) {
            source->setPort(local->port());
            local = source;
        }
    }
    copyText(info.local_address, local->toString());
    try { // No list: the path is still what it is.
        const auto interfaces = transport::interfaces();
        for (const auto& network : seams::networks()) {
            const auto holds = std::find_if(interfaces.begin(), interfaces.end(), [&](const transport::Interface& i) {
                return i.index == network.index &&
                       std::any_of(i.addresses.begin(), i.addresses.end(), [&](const transport::Endpoint& e) {
                           return e.addressBytes() == local->addressBytes();
                       });
            });
            if (holds != interfaces.end()) {
                copyText(info.interface_name, network.name);
                info.network = network.kind;
                info.network_flags = network.flags;
                break;
            }
        }
    } catch (const Failure& failure) {
        log::debug("network", std::string("The path's network is not known: ") + failure.what());
    }
    return info;
}

std::vector<rcs_network_info> networks() {
    if (!(capabilities().transports & (1u << RCS_TRANSPORT_UDP))) {
        fail(RCS_ERROR_UNSUPPORTED, "This system has no networks of its own to list (a browser's are the page's)");
    }
    std::vector<rcs_network_info> list;
    for (const auto& network : seams::networks()) {
        rcs_network_info& info = list.emplace_back();
        copyText(info.name, network.name);
        info.index = network.index;
        info.kind = network.kind;
        info.flags = network.flags;
        info.ipv4 = network.ipv4;
        info.ipv6 = network.ipv6;
    }
    return list;
}

std::vector<rcs_wifi_info> wifi() {
    if (!(capabilities().transports & (1u << RCS_TRANSPORT_UDP))) {
        fail(RCS_ERROR_UNSUPPORTED, "This system has no Wi-Fi of its own to list (a browser's is the page's)");
    }
    auto list = seams::wifi();
    for (auto& w : list) {
        const bool awdls = w.band == RCS_WIFI_BAND_2_4_GHZ ? w.channel == 6
                           : w.band == RCS_WIFI_BAND_5_GHZ ? w.channel == 44 || w.channel == 149
                                                           : true; // 6 GHz, or unknown: not known to clash.
        w.other_channel = w.peer_to_peer && w.channel && !awdls;
    }
    return list;
}

rcs_wifi_info wifiUnder(const rcs_path_info& path) {
    if (path.network == RCS_NETWORK_WIFI && path.interface_name[0]) {
        try {
            for (const auto& w : wifi()) {
                if (std::strcmp(w.name, path.interface_name) == 0) {
                    return w;
                }
            }
        } catch (const Failure&) { // As far as the system says.
        }
    }
    return {};
}

namespace {

// The interface `live` has this end's sockets use among `list`, given the one
// they use now (`current`; 0: none); 0: none at all (unbound). Throws
// Failure(NETWORK) saying why no network is left.
uint32_t choose(const SessionConfig& live, const std::vector<platform::Network>& list, uint32_t current) {
    const auto& p = live.paths;
    const std::string only = live.networkTuning.local_interface;
    if (!only.empty()) {
        for (const auto& n : list) {
            if (n.name == only || std::to_string(n.index) == only) {
                return n.index;
            }
        }
        fail(RCS_ERROR_NETWORK,
             "The network interface \"" + only + "\" (local_interface) is not up with a usable address");
    }
    if (!p.excluded_networks && p.policy != RCS_PATH_POLICY_STAY) {
        return 0;
    }
    std::vector<const platform::Network*> allowed;
    for (const auto& n : list) {
        if (!(p.excluded_networks & (1u << n.kind))) {
            allowed.push_back(&n);
        }
    }
    if (allowed.empty()) {
        fail(RCS_ERROR_NETWORK,
             list.empty() ? "No network is up" : "Every network that is up is excluded (rcs_paths_config)");
    }
    const auto inUse =
        std::find_if(allowed.begin(), allowed.end(), [&](const platform::Network* n) { return n->index == current; });
    const auto preferred = std::find_if(allowed.begin(), allowed.end(), [](const platform::Network* n) {
        return (n->flags & RCS_NETWORK_FLAG_DEFAULT) != 0;
    });
    if (p.policy == RCS_PATH_POLICY_STAY && inUse != allowed.end()) {
        return current;
    }
    return preferred != allowed.end() ? (*preferred)->index : inUse != allowed.end() ? current : allowed.front()->index;
}

// The network a standby path goes over beside `inUse` (0: the system's
// default); 0 and `why` when there is none.
uint32_t chooseStandby(const SessionConfig& live, const std::vector<platform::Network>& list, uint32_t inUse,
                       std::string& why) {
    const auto used = std::find_if(list.begin(), list.end(), [&](const platform::Network& n) {
        return inUse ? n.index == inUse : (n.flags & RCS_NETWORK_FLAG_DEFAULT) != 0;
    });
    if (used != list.end() && (used->flags & RCS_NETWORK_FLAG_VPN)) {
        why = "The network in use is a VPN: a standby path beside it would leave the tunnel";
        return 0;
    }
    // Another kind of network first (another medium is what a standby path is for), then the same kind.
    bool other = false;
    for (const bool sameKind : {false, true}) {
        for (const auto& n : list) {
            const bool physical =
                n.kind == RCS_NETWORK_ETHERNET || n.kind == RCS_NETWORK_WIFI || n.kind == RCS_NETWORK_CELLULAR;
            if ((used != list.end() && (n.index == used->index || (n.kind == used->kind) != sameKind)) || !physical ||
                (n.flags & RCS_NETWORK_FLAG_VPN)) {
                continue;
            }
            other = true;
            if (!(live.paths.excluded_networks & (1u << n.kind))) {
                return n.index;
            }
        }
    }
    why = other ? "Every other network is excluded (rcs_paths_config)"
                : "No other Ethernet, Wi-Fi or cellular network is up for a standby path";
    return 0;
}

} // namespace

NetworkChoice::Choice NetworkChoice::look(const SessionConfig& live, uint64_t settings) {
    std::lock_guard lock(mutex_);
    const uint64_t networks = transport::networkGeneration();
    if (networks == networks_ && settings == settings_) {
        return choice_;
    }
    const bool resettled = settings != settings_ && settings_ != ~uint64_t(0);
    networks_ = networks;
    settings_ = settings;
    const auto transport = live.connect.transport;
    if (usesHostChannel(live) ||
        (live.role == RCS_ROLE_SHARE && (transport == RCS_TRANSPORT_UDP || transport == RCS_TRANSPORT_TCP))) {
        return choice_;
    }
    Choice next = choice_;
    next.index = 0;
    next.lost = false;
    next.why.clear();
    next.standbyIndex = 0;
    next.standbyWhy.clear();
    bool gone = false;
    const auto& p = live.paths;
    const bool facts = p.excluded_networks || p.policy == RCS_PATH_POLICY_STAY || p.standby;
    try {
        // What the system says of its networks only when the settings need it
        // (local_interface alone needs the interfaces, and nothing at all the default).
        std::vector<platform::Network> list;
        if (facts || seams::networksOverridden()) {
            list = seams::networks();
        } else if (live.networkTuning.local_interface[0]) {
            for (const auto& i : transport::interfaces()) {
                list.push_back({i.name, i.index});
            }
        }
        gone = choice_.index && std::none_of(list.begin(), list.end(),
                                             [&](const platform::Network& n) { return n.index == choice_.index; });
        try {
            next.index = choose(live, list, choice_.index);
        } catch (const Failure& failure) {
            next.lost = true;
            next.why = failure.what();
        }
        if (live.paths.standby) {
            next.standbyIndex = chooseStandby(live, list, next.index, next.standbyWhy);
        }
    } catch (const Failure& failure) { // No list.
        next.lost = true;
        next.why = failure.what();
        if (live.paths.standby) {
            next.standbyWhy = next.why;
        }
    }
    if (next.index != choice_.index || next.lost != choice_.lost) {
        ++next.version;
        next.cause = gone && !resettled ? RCS_PATH_CAUSE_NETWORK : RCS_PATH_CAUSE_SETTINGS;
        log::info("network", next.lost    ? "No network is left for this end's sockets: " + next.why
                             : next.index ? "This end's sockets use the network of interface " +
                                                std::to_string(next.index) + " from now"
                                          : std::string("This end's sockets use the network the system routes "
                                                        "through from now"));
    }
    if (next.standbyIndex != choice_.standbyIndex || next.standbyWhy != choice_.standbyWhy ||
        (resettled && live.paths.standby != standby_)) {
        ++next.standbyVersion;
    }
    standby_ = live.paths.standby;
    choice_ = std::move(next);
    return choice_;
}

} // namespace rcsdk::session
