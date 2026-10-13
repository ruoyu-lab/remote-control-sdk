// Apple's networks (macOS, iOS) through Network.framework: a path monitor
// says which interfaces the default path may use, in the order it prefers
// them (the default first), what kind each is, whether the path is expensive
// (cellular, a phone's hotspot) or constrained (Low Data Mode), and when any
// of that changes. Tunnels have no kind of their own there: a VPN is told by
// its interface's name (utun, ipsec, ppp: not an API, so a guess).
#include "platform/platform.hpp"
#include "transport/interfaces.hpp"
#include "transport/network_changes.hpp"

#include <Network/Network.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace rcsdk::platform {
namespace {

rcs_network_kind kindOf(nw_interface_type_t type) {
    switch (type) {
    case nw_interface_type_wifi:
        return RCS_NETWORK_WIFI;
    case nw_interface_type_cellular:
        return RCS_NETWORK_CELLULAR;
    case nw_interface_type_wired:
        return RCS_NETWORK_ETHERNET;
    case nw_interface_type_other:
        return RCS_NETWORK_OTHER;
    default:
        return RCS_NETWORK_UNKNOWN;
    }
}

bool tunnel(const std::string& name) {
    for (const char* prefix : {"utun", "ipsec", "ppp", "tun", "tap", "wg"}) {
        if (name.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

// What the latest path said.
struct PathFacts {
    bool known = false;
    std::vector<std::pair<uint32_t, rcs_network_kind>> interfaces; // Its interfaces by index, the default first.
    bool expensive = false, constrained = false;
};

class Monitor {
public:
    Monitor()
        : queue_(dispatch_queue_create("rcsdk.networks", DISPATCH_QUEUE_SERIAL)), monitor_(nw_path_monitor_create()),
          cancelled_(dispatch_semaphore_create(0)) {
        nw_path_monitor_set_queue(monitor_, queue_);
        nw_path_monitor_set_update_handler(monitor_, ^(nw_path_t path) {
          update(path);
        });
        dispatch_semaphore_t cancelled = cancelled_;
        nw_path_monitor_set_cancel_handler(monitor_, ^{
          dispatch_semaphore_signal(cancelled);
        });
        nw_path_monitor_start(monitor_);
    }
    // The cancel handler comes after the last update: none runs on a monitor that is gone.
    ~Monitor() {
        nw_path_monitor_cancel(monitor_);
        dispatch_semaphore_wait(cancelled_, DISPATCH_TIME_FOREVER);
    }
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    // The latest path's facts; the first ones come within moments of the start.
    PathFacts facts() {
        std::unique_lock lock(mutex_);
        updated_.wait_for(lock, std::chrono::seconds(1), [&] { return facts_.known; });
        return facts_;
    }

private:
    void update(nw_path_t path) {
        __block PathFacts facts;
        facts.known = true;
        if (nw_path_get_status(path) == nw_path_status_satisfied) {
            nw_path_enumerate_interfaces(path, ^bool(nw_interface_t interface) {
              facts.interfaces.emplace_back(nw_interface_get_index(interface),
                                            kindOf(nw_interface_get_type(interface)));
              return true;
            });
        }
        facts.expensive = nw_path_is_expensive(path);
        facts.constrained = nw_path_is_constrained(path);
        bool first = false;
        {
            std::lock_guard lock(mutex_);
            first = !facts_.known;
            facts_ = std::move(facts);
        }
        updated_.notify_all();
        if (!first) { // The first says how things are, not that they changed.
            transport::networkChanged();
        }
    }

    dispatch_queue_t queue_;
    nw_path_monitor_t monitor_;
    dispatch_semaphore_t cancelled_;
    std::mutex mutex_;
    std::condition_variable updated_;
    PathFacts facts_;
};

// The process's monitor while anything holds it (watchNetworks, a listing).
std::shared_ptr<Monitor> monitor() {
    static std::mutex mutex;
    static std::weak_ptr<Monitor> running;
    std::lock_guard lock(mutex);
    if (auto shared = running.lock()) {
        return shared;
    }
    auto made = std::make_shared<Monitor>();
    running = made;
    return made;
}

} // namespace

std::vector<Network> networks() {
    const PathFacts facts = monitor()->facts();
    const auto find = [&](uint32_t index) {
        return std::find_if(facts.interfaces.begin(), facts.interfaces.end(),
                            [&](const auto& entry) { return entry.first == index; });
    };
    // A tunnel's network is the first other one the path may use.
    rcs_network_kind underlying = RCS_NETWORK_UNKNOWN;
    for (const auto& [index, kind] : facts.interfaces) {
        if (kind != RCS_NETWORK_OTHER && kind != RCS_NETWORK_UNKNOWN) {
            underlying = kind;
            break;
        }
    }
    std::vector<Network> list;
    for (const auto& i : transport::interfaces()) {
        Network network{i.name, i.index};
        for (const auto& address : i.addresses) {
            (address.ipv6() ? network.ipv6 : network.ipv4) = true;
        }
        const auto found = find(i.index);
        if (tunnel(i.name)) {
            network.flags |= RCS_NETWORK_FLAG_VPN;
            network.kind = underlying;
        } else if (found != facts.interfaces.end()) {
            network.kind = found->second;
        }
        if (found == facts.interfaces.begin() && found != facts.interfaces.end()) {
            network.flags |= RCS_NETWORK_FLAG_DEFAULT | (facts.expensive ? RCS_NETWORK_FLAG_METERED : 0) |
                             (facts.constrained ? RCS_NETWORK_FLAG_CONSTRAINED : 0);
        }
        if (network.kind == RCS_NETWORK_CELLULAR) {
            network.flags |= RCS_NETWORK_FLAG_METERED;
        }
        list.push_back(std::move(network));
    }
    std::stable_partition(list.begin(), list.end(),
                          [](const Network& n) { return (n.flags & RCS_NETWORK_FLAG_DEFAULT) != 0; });
    return list;
}

uint32_t networkFacts() noexcept {
    return RCS_NETWORK_FACT_KIND | RCS_NETWORK_FACT_METERED | RCS_NETWORK_FACT_CONSTRAINED;
}

std::shared_ptr<void> watchNetworks() {
    return monitor();
}

} // namespace rcsdk::platform
