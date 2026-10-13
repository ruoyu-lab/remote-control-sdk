#include "transport/interfaces.hpp"

#include "transport/socket_error.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
// macOS tells each IPv6 address's state; iOS keeps that interface private (no
// netinet6/in6_var.h), so there every global and unique-local address is a
// candidate, temporary ones too, and the connectivity checks sort them out.
#if defined(__APPLE__) && TARGET_OS_OSX
#define RCSDK_IPV6_FLAGS 1
#include <netinet6/in6_var.h>
#include <sys/ioctl.h>
#endif
#endif

namespace rcsdk::transport {
namespace {

bool usable(const Endpoint& endpoint) {
    const auto bytes = endpoint.addressBytes();
    if (bytes.size() == 4) {
        return bytes[0] != 127 && !(bytes[0] == 169 && bytes[1] == 254) && bytes[0] != 0;
    }
    if (bytes.size() == 16) {
        const bool linkLocal = bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80;
        const bool loopback =
            std::all_of(bytes.begin(), bytes.end() - 1, [](uint8_t b) { return b == 0; }) && bytes.back() == 1;
        const bool global = (bytes[0] & 0xe0) == 0x20, uniqueLocal = (bytes[0] & 0xfe) == 0xfc;
        return !linkLocal && !loopback && (global || uniqueLocal);
    }
    return false;
}

#ifdef RCSDK_IPV6_FLAGS
// getifaddrs lists every IPv6 address the kernel holds, including ones that
// are being given up or never worked; Windows reports the same through
// DadState. Also tells which ones are temporary (RFC 8981).
struct Ipv6Flags {
    bool usable = false, temporary = false;
};

Ipv6Flags ipv6Flags(int probe, const char* name, const sockaddr* address) {
    in6_ifreq request{};
    std::strncpy(request.ifr_name, name, sizeof(request.ifr_name) - 1);
    std::memcpy(&request.ifr_ifru.ifru_addr, address, sizeof(sockaddr_in6));
    if (probe < 0 || ioctl(probe, SIOCGIFAFLAG_IN6, &request) != 0) {
        return {};
    }
    const int flags = request.ifr_ifru.ifru_flags6;
    constexpr int unusable = IN6_IFF_ANYCAST | IN6_IFF_TENTATIVE | IN6_IFF_DUPLICATED | IN6_IFF_DETACHED |
                             IN6_IFF_DEPRECATED | IN6_IFF_CLAT46;
    return {(flags & unusable) == 0, (flags & IN6_IFF_TEMPORARY) != 0};
}
#endif

} // namespace

std::vector<Interface> interfaces() {
    std::vector<Interface> result;
    const auto add = [&](const std::string& name, uint32_t index, const Endpoint& endpoint) {
        if (!endpoint.valid() || !usable(endpoint)) {
            return;
        }
        auto found = std::find_if(result.begin(), result.end(), [&](const Interface& i) { return i.name == name; });
        if (found == result.end()) {
            found = result.insert(result.end(), Interface{name, index, {}});
        }
        if (std::find(found->addresses.begin(), found->addresses.end(), endpoint) == found->addresses.end()) {
            found->addresses.push_back(endpoint);
        }
    };
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        status =
            GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (status != NO_ERROR) {
        fail(RCS_ERROR_NETWORK, "Could not list the network interfaces (Windows error " + std::to_string(status) + ")");
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        std::string name(
            size_t(WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, nullptr, 0, nullptr, nullptr)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, name.data(), int(name.size()), nullptr, nullptr);
        name.resize(std::strlen(name.c_str()));
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (unicast->DadState != IpDadStatePreferred) {
                continue; // Tentative, duplicate or deprecated.
            }
            add(name, uint32_t(adapter->IfIndex),
                Endpoint::fromSockaddr(unicast->Address.lpSockaddr, size_t(unicast->Address.iSockaddrLength)));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        fail(RCS_ERROR_NETWORK, "Could not list the network interfaces: " + lastSocketErrorText());
    }
#ifdef RCSDK_IPV6_FLAGS
    const int probe = ::socket(AF_INET6, SOCK_DGRAM, 0);
    std::vector<std::pair<std::string, Endpoint>> temporary; // Kept only where no stable address is.
#endif
    for (auto* entry = list; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || !(entry->ifa_flags & IFF_UP) || !(entry->ifa_flags & IFF_RUNNING) ||
            (entry->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }
        const auto family = entry->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) {
            continue;
        }
        const auto endpoint =
            Endpoint::fromSockaddr(entry->ifa_addr, family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
#ifdef RCSDK_IPV6_FLAGS
        if (family == AF_INET6) {
            const auto flags = ipv6Flags(probe, entry->ifa_name, entry->ifa_addr);
            if (!flags.usable) {
                continue;
            }
            if (flags.temporary) {
                temporary.emplace_back(entry->ifa_name, endpoint);
                continue;
            }
        }
#endif
        add(entry->ifa_name, if_nametoindex(entry->ifa_name), endpoint);
    }
    freeifaddrs(list);
#ifdef RCSDK_IPV6_FLAGS
    if (probe >= 0) {
        ::close(probe);
    }
    // A temporary address changes within a day and reaches nothing a stable
    // one of the same scope (global or unique-local) on the interface does not.
    const auto global = [](const Endpoint& e) {
        return (e.addressBytes()[0] & 0xe0) == 0x20;
    };
    for (const auto& [name, endpoint] : temporary) {
        const auto found =
            std::find_if(result.begin(), result.end(), [&](const Interface& i) { return i.name == name; });
        const bool stable = found != result.end() &&
                            std::any_of(found->addresses.begin(), found->addresses.end(),
                                        [&](const Endpoint& e) { return e.ipv6() && global(e) == global(endpoint); });
        if (!stable) {
            add(name, if_nametoindex(name.c_str()), endpoint);
        }
    }
#endif
#endif
    return result;
}

std::vector<Endpoint> localAddresses() {
    std::vector<Endpoint> unique; // An address can sit on several interfaces.
    for (const auto& i : interfaces()) {
        for (const auto& endpoint : i.addresses) {
            if (std::find(unique.begin(), unique.end(), endpoint) == unique.end()) {
                unique.push_back(endpoint);
            }
        }
    }
    return unique;
}

uint32_t defaultInterface(const std::vector<Interface>& list) {
    for (const char* documentation : {"192.0.2.1", "2001:db8::1"}) {
        const auto source = sourceFor(Endpoint::parse(documentation, 9));
        if (!source) {
            continue;
        }
        for (const auto& i : list) {
            for (const auto& address : i.addresses) {
                if (address.addressBytes() == source->addressBytes()) {
                    return i.index;
                }
            }
        }
    }
    return 0;
}

Interface findInterface(const std::string& name) {
    const auto all = interfaces();
    for (const auto& i : all) {
        if (i.name == name || std::to_string(i.index) == name) {
            return i;
        }
    }
    std::string available;
    for (const auto& i : all) {
        available += (available.empty() ? "" : ", ") + i.name;
    }
    fail(RCS_ERROR_NETWORK, "No active network interface \"" + name + "\" with a usable address (available: " +
                                (available.empty() ? std::string("none") : available) + ")");
}

} // namespace rcsdk::transport
