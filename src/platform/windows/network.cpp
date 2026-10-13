// Windows' networks: what each interface is (iphlpapi: its type), what each
// costs (the network list manager: the user's metered setting, a data limit,
// roaming), and when any interface, address or
// route changes (iphlpapi's notifications). A VPN is told by its interface's
// type (a tunnel, PPP, a virtual adapter): a TAP adapter passes for Ethernet.
//
// And its Wi-Fi out of its power saving while connected, as game streaming
// clients keep it: on each connected Wi-Fi interface, media streaming mode
// (the adapter stays awake for a stream) and no background scans (which take
// the radio off its channel now and then). Through the WLAN service's client
// API, loaded when first needed (wlanapi.dll: Windows Server has it with the
// Wireless LAN feature only); the settings last while the handle that made
// them stays open. What each connected one is (rcs_list_wifi) comes from the
// same API: its channel, signal and rate; its band from the frequency of the
// access point it is with (its BSS entry, which Windows 11 may keep from an
// application without location access: the band is then unknown).
#include "platform/windows/network.hpp"

#include "platform/platform.hpp"
#include "transport/interfaces.hpp" // Winsock before windows.h.
#include "transport/network_changes.hpp"
#include "platform/windows/d3d.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <windows.h>
#include <wlanapi.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace rcsdk::platform {
namespace {

rcs_network_kind kindOf(IFTYPE type) {
    switch (type) {
    case IF_TYPE_ETHERNET_CSMACD:
        return RCS_NETWORK_ETHERNET;
    case IF_TYPE_IEEE80211:
        return RCS_NETWORK_WIFI;
    case IF_TYPE_WWANPP:
    case IF_TYPE_WWANPP2:
        return RCS_NETWORK_CELLULAR;
    default:
        return RCS_NETWORK_OTHER;
    }
}

bool tunnel(IFTYPE type) {
    return type == IF_TYPE_TUNNEL || type == IF_TYPE_PPP || type == IF_TYPE_PROP_VIRTUAL;
}

// The network list manager's cost (NLM_CONNECTION_COST_*) of each connected
// adapter, by its interface's GUID.
std::vector<std::pair<GUID, DWORD>> costs() {
    win::ComThread com;
    win::Com<INetworkListManager> manager;
    win::check(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, __uuidof(INetworkListManager),
                                reinterpret_cast<void**>(manager.out())),
               "Creating the network list manager", RCS_ERROR_NETWORK);
    win::Com<IEnumNetworkConnections> connections;
    win::check(manager->GetNetworkConnections(connections.out()), "INetworkListManager::GetNetworkConnections",
               RCS_ERROR_NETWORK);
    std::vector<std::pair<GUID, DWORD>> result;
    win::Com<INetworkConnection> connection;
    while (connections->Next(1, connection.out(), nullptr) == S_OK) {
        GUID adapter{};
        DWORD cost = 0;
        if (SUCCEEDED(connection->GetAdapterId(&adapter)) &&
            SUCCEEDED(
                connection.as<INetworkConnectionCost>("INetworkConnectionCost", RCS_ERROR_NETWORK)->GetCost(&cost))) {
            result.emplace_back(adapter, cost);
        }
    }
    return result;
}

void WINAPI interfaceChanged(void*, MIB_IPINTERFACE_ROW*, MIB_NOTIFICATION_TYPE) {
    transport::networkChanged();
}
void WINAPI addressChanged(void*, MIB_UNICASTIPADDRESS_ROW*, MIB_NOTIFICATION_TYPE) {
    transport::networkChanged();
}
void WINAPI routeChanged(void*, MIB_IPFORWARD_ROW2*, MIB_NOTIFICATION_TYPE) {
    transport::networkChanged();
}

// One of iphlpapi's notifications, cancelled when it goes (which waits for a
// callback that runs).
struct Notification {
    HANDLE handle = nullptr;
    Notification() = default;
    Notification(const Notification&) = delete;
    Notification& operator=(const Notification&) = delete;
    ~Notification() {
        if (handle) {
            CancelMibChangeNotify2(handle);
        }
    }
};

void notified(DWORD error, const char* what) {
    if (error != NO_ERROR) {
        fail(RCS_ERROR_NETWORK, std::string(what) + " failed (error " + std::to_string(error) + ")");
    }
}

// An interface, an address or a route that came, went or changed.
struct Watcher {
    Notification interfaces, addresses, routes;
    Watcher() {
        notified(NotifyIpInterfaceChange(AF_UNSPEC, interfaceChanged, nullptr, FALSE, &interfaces.handle),
                 "NotifyIpInterfaceChange");
        notified(NotifyUnicastIpAddressChange(AF_UNSPEC, addressChanged, nullptr, FALSE, &addresses.handle),
                 "NotifyUnicastIpAddressChange");
        notified(NotifyRouteChange2(AF_UNSPEC, routeChanged, nullptr, FALSE, &routes.handle), "NotifyRouteChange2");
    }
};

struct Wlan {
    decltype(&WlanOpenHandle) open = nullptr;
    decltype(&WlanEnumInterfaces) enumerate = nullptr;
    decltype(&WlanSetInterface) set = nullptr;
    decltype(&WlanQueryInterface) query = nullptr;
    decltype(&WlanGetNetworkBssList) bssList = nullptr;
    decltype(&WlanFreeMemory) free = nullptr;
    decltype(&WlanCloseHandle) close = nullptr;
};

const Wlan& wlan() {
    static const Wlan api = [] {
        Wlan w;
        if (HMODULE module = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            w.open = reinterpret_cast<decltype(w.open)>(GetProcAddress(module, "WlanOpenHandle"));
            w.enumerate = reinterpret_cast<decltype(w.enumerate)>(GetProcAddress(module, "WlanEnumInterfaces"));
            w.set = reinterpret_cast<decltype(w.set)>(GetProcAddress(module, "WlanSetInterface"));
            w.query = reinterpret_cast<decltype(w.query)>(GetProcAddress(module, "WlanQueryInterface"));
            w.bssList = reinterpret_cast<decltype(w.bssList)>(GetProcAddress(module, "WlanGetNetworkBssList"));
            w.free = reinterpret_cast<decltype(w.free)>(GetProcAddress(module, "WlanFreeMemory"));
            w.close = reinterpret_cast<decltype(w.close)>(GetProcAddress(module, "WlanCloseHandle"));
        }
        return w.open && w.enumerate && w.set && w.query && w.bssList && w.free && w.close ? w : Wlan{};
    }();
    return api;
}

} // namespace

std::vector<Network> networks() {
    const auto costed = costs();
    const auto interfaces = transport::interfaces();
    const uint32_t preferred = transport::defaultInterface(interfaces);
    std::vector<Network> list;
    for (const auto& i : interfaces) {
        Network network{i.name, i.index};
        for (const auto& address : i.addresses) {
            (address.ipv6() ? network.ipv6 : network.ipv4) = true;
        }
        MIB_IF_ROW2 row{};
        row.InterfaceIndex = i.index;
        if (GetIfEntry2(&row) == NO_ERROR) {
            if (tunnel(row.Type)) {
                network.flags |= RCS_NETWORK_FLAG_VPN;
            } else {
                network.kind = kindOf(row.Type);
            }
            for (const auto& [adapter, cost] : costed) {
                if (std::memcmp(&adapter, &row.InterfaceGuid, sizeof(GUID)) != 0) {
                    continue;
                }
                if (cost & (NLM_CONNECTION_COST_FIXED | NLM_CONNECTION_COST_VARIABLE | NLM_CONNECTION_COST_ROAMING)) {
                    network.flags |= RCS_NETWORK_FLAG_METERED;
                }
                if (cost & (NLM_CONNECTION_COST_OVERDATALIMIT | NLM_CONNECTION_COST_APPROACHINGDATALIMIT)) {
                    network.flags |= RCS_NETWORK_FLAG_CONSTRAINED;
                }
            }
        }
        if (network.kind == RCS_NETWORK_CELLULAR) {
            network.flags |= RCS_NETWORK_FLAG_METERED;
        }
        if (i.index == preferred) {
            network.flags |= RCS_NETWORK_FLAG_DEFAULT;
        }
        list.push_back(std::move(network));
    }
    // A tunnel's network is the one under it: the first other one with a kind,
    // the default first.
    std::stable_partition(list.begin(), list.end(),
                          [](const Network& n) { return (n.flags & RCS_NETWORK_FLAG_DEFAULT) != 0; });
    const auto under = std::find_if(list.begin(), list.end(), [](const Network& n) {
        return !(n.flags & RCS_NETWORK_FLAG_VPN) && n.kind != RCS_NETWORK_OTHER && n.kind != RCS_NETWORK_UNKNOWN;
    });
    for (auto& network : list) {
        if ((network.flags & RCS_NETWORK_FLAG_VPN) && under != list.end()) {
            network.kind = under->kind;
        }
    }
    return list;
}

uint32_t networkFacts() noexcept {
    return RCS_NETWORK_FACT_KIND | RCS_NETWORK_FACT_METERED | RCS_NETWORK_FACT_CONSTRAINED;
}

std::shared_ptr<void> watchNetworks() {
    static std::mutex mutex;
    static std::weak_ptr<Watcher> running; // One for every session of the process.
    std::lock_guard lock(mutex);
    if (auto shared = running.lock()) {
        return shared;
    }
    auto made = std::make_shared<Watcher>();
    running = made;
    return made;
}

std::vector<rcs_wifi_info> wifiNetworks() {
    const Wlan& api = wlan();
    if (!api.open) {
        return {};
    }
    DWORD version = 0;
    HANDLE handle = nullptr;
    if (api.open(2, nullptr, &version, &handle) != ERROR_SUCCESS) {
        return {}; // WLAN AutoConfig is not running: no Wi-Fi in use.
    }
    std::shared_ptr<void> closing(handle, [](void* h) { wlan().close(h, nullptr); });
    WLAN_INTERFACE_INFO_LIST* interfaces = nullptr;
    if (const DWORD error = api.enumerate(handle, nullptr, &interfaces); error != ERROR_SUCCESS) {
        fail(RCS_ERROR_NETWORK, "WlanEnumInterfaces failed (error " + std::to_string(error) + ")");
    }
    std::shared_ptr<void> freeing(interfaces, [](void* p) { wlan().free(p); });
    // What the interface says of `opcode`, or nothing.
    const auto query = [&](const GUID& guid, WLAN_INTF_OPCODE opcode, auto& value) {
        DWORD size = 0;
        void* data = nullptr;
        if (api.query(handle, &guid, opcode, nullptr, &size, &data, nullptr) != ERROR_SUCCESS) {
            return false;
        }
        const bool fits = size >= sizeof(value);
        if (fits) {
            std::memcpy(&value, data, sizeof(value));
        }
        api.free(data);
        return fits;
    };
    std::vector<rcs_wifi_info> list;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& item = interfaces->InterfaceInfo[i];
        if (item.isState != wlan_interface_state_connected) {
            continue;
        }
        rcs_wifi_info info{};
        NET_LUID luid{};
        NET_IFINDEX index = 0;
        wchar_t alias[IF_MAX_STRING_SIZE + 1] = {};
        if (ConvertInterfaceGuidToLuid(&item.InterfaceGuid, &luid) == NO_ERROR) {
            if (ConvertInterfaceLuidToIndex(&luid, &index) == NO_ERROR) {
                info.index = index;
            }
            if (ConvertInterfaceLuidToAlias(&luid, alias, IF_MAX_STRING_SIZE + 1) == NO_ERROR) {
                WideCharToMultiByte(CP_UTF8, 0, alias, -1, info.name, int(sizeof(info.name) - 1), nullptr, nullptr);
            }
        }
        ULONG channel = 0;
        LONG rssi = 0;
        if (query(item.InterfaceGuid, wlan_intf_opcode_channel_number, channel)) {
            info.channel = channel;
        }
        if (query(item.InterfaceGuid, wlan_intf_opcode_rssi, rssi)) {
            info.signal_dbm = rssi;
        }
        WLAN_CONNECTION_ATTRIBUTES connection{};
        if (query(item.InterfaceGuid, wlan_intf_opcode_current_connection, connection)) {
            auto& association = connection.wlanAssociationAttributes;
            info.rate_mbps = association.ulTxRate / 1000;
            WLAN_BSS_LIST* bss = nullptr;
            if (api.bssList(handle, &item.InterfaceGuid, &association.dot11Ssid, dot11_BSS_type_infrastructure,
                            connection.wlanSecurityAttributes.bSecurityEnabled, nullptr, &bss) == ERROR_SUCCESS) {
                for (DWORD b = 0; b < bss->dwNumberOfItems; ++b) {
                    const WLAN_BSS_ENTRY& entry = bss->wlanBssEntries[b];
                    if (std::memcmp(entry.dot11Bssid, association.dot11Bssid, sizeof(entry.dot11Bssid))) {
                        continue;
                    }
                    const ULONG mhz = entry.ulChCenterFrequency / 1000;
                    info.band = mhz >= 5925 ? RCS_WIFI_BAND_6_GHZ
                                : mhz >= 4900 ? RCS_WIFI_BAND_5_GHZ
                                : mhz >= 2400 ? RCS_WIFI_BAND_2_4_GHZ
                                              : RCS_WIFI_BAND_UNKNOWN;
                }
                api.free(bss);
            }
        }
        list.push_back(info);
    }
    return list;
}

uint32_t wifiFacts() noexcept {
    return wlan().open ? RCS_WIFI_FACT_CHANNEL | RCS_WIFI_FACT_SIGNAL | RCS_WIFI_FACT_RATE : 0;
}

bool win::wifiSettable() noexcept {
    return wlan().open != nullptr;
}

std::shared_ptr<void> lowLatencyWifi() {
    static std::mutex mutex;
    static std::weak_ptr<void> held; // One handle for every session of the process.
    std::lock_guard lock(mutex);
    if (auto shared = held.lock()) {
        return shared;
    }
    const Wlan& api = wlan();
    if (!api.open) {
        fail(RCS_ERROR_UNSUPPORTED, "This Windows has no WLAN service (wlanapi.dll)");
    }
    DWORD version = 0;
    HANDLE handle = nullptr;
    if (const DWORD error = api.open(2, nullptr, &version, &handle); error != ERROR_SUCCESS) {
        if (error == ERROR_SERVICE_NOT_ACTIVE) {
            return nullptr; // WLAN AutoConfig is not running: no Wi-Fi in use.
        }
        fail(RCS_ERROR_INTERNAL, "WlanOpenHandle failed (error " + std::to_string(error) + ")");
    }
    std::shared_ptr<void> shared(handle, [](void* h) { wlan().close(h, nullptr); });
    WLAN_INTERFACE_INFO_LIST* list = nullptr;
    if (const DWORD error = api.enumerate(handle, nullptr, &list); error != ERROR_SUCCESS) {
        fail(RCS_ERROR_INTERNAL, "WlanEnumInterfaces failed (error " + std::to_string(error) + ")");
    }
    DWORD refused = ERROR_SUCCESS;
    unsigned streaming = 0;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& item = list->InterfaceInfo[i];
        if (item.isState != wlan_interface_state_connected) {
            continue;
        }
        BOOL off = FALSE, on = TRUE;
        api.set(handle, &item.InterfaceGuid, wlan_intf_opcode_background_scan_enabled, sizeof(off), &off, nullptr);
        if (const DWORD error = api.set(handle, &item.InterfaceGuid, wlan_intf_opcode_media_streaming_mode,
                                        sizeof(on), &on, nullptr);
            error == ERROR_SUCCESS) {
            ++streaming;
        } else {
            refused = error;
        }
    }
    api.free(list);
    if (!streaming) {
        if (refused != ERROR_SUCCESS) {
            fail(RCS_ERROR_INTERNAL, "The Wi-Fi interface refused media streaming mode (error " +
                                         std::to_string(refused) + ")");
        }
        return nullptr; // No Wi-Fi connected.
    }
    held = shared;
    return shared;
}

} // namespace rcsdk::platform
