// Systems that say nothing of their networks but what their interfaces are
// and where their routes go: no kinds, nothing metered, no Wi-Fi. (Apple,
// Windows, Android: their own.)
#include "platform/platform.hpp"
#include "transport/interfaces.hpp"

#include <algorithm>

namespace rcsdk::platform {

std::vector<Network> networks() {
    const auto interfaces = transport::interfaces();
    const uint32_t preferred = transport::defaultInterface(interfaces);
    std::vector<Network> list;
    for (const auto& i : interfaces) {
        Network network{i.name, i.index};
        for (const auto& address : i.addresses) {
            (address.ipv6() ? network.ipv6 : network.ipv4) = true;
        }
        network.flags = i.index == preferred ? RCS_NETWORK_FLAG_DEFAULT : 0;
        list.push_back(std::move(network));
    }
    std::stable_partition(list.begin(), list.end(),
                          [](const Network& n) { return (n.flags & RCS_NETWORK_FLAG_DEFAULT) != 0; });
    return list;
}

uint32_t networkFacts() noexcept {
    return 0;
}

std::vector<rcs_wifi_info> wifiNetworks() {
    return {};
}

uint32_t wifiFacts() noexcept {
    return 0;
}

} // namespace rcsdk::platform
