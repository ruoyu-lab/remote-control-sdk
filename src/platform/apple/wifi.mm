// Apple's Wi-Fi (rcs_list_wifi). macOS: CoreWLAN says each interface's
// channel, band, width, signal, noise and rate (none of which needs location
// access; the network's name would). iOS has no such API: its Wi-Fi
// interfaces come from Network.framework (networks()), with nothing but
// whether AWDL is up. AWDL, both: its interface (awdl0) up and running.
#include "platform/platform.hpp"

#include <TargetConditionals.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <cstring>

#if TARGET_OS_OSX
#import <CoreWLAN/CoreWLAN.h>
#endif

namespace rcsdk::platform {
namespace {

bool awdlUp() {
    ifaddrs* list = nullptr;
    if (getifaddrs(&list)) {
        return false;
    }
    bool up = false;
    for (const ifaddrs* i = list; i; i = i->ifa_next) {
        if (!std::strcmp(i->ifa_name, "awdl0") && (i->ifa_flags & IFF_UP) && (i->ifa_flags & IFF_RUNNING)) {
            up = true;
        }
    }
    freeifaddrs(list);
    return up;
}

void copyName(rcs_wifi_info& info, const std::string& name) {
    std::strncpy(info.name, name.c_str(), sizeof(info.name) - 1);
}

#if TARGET_OS_OSX
rcs_wifi_band bandOf(CWChannelBand band) {
    switch (band) {
    case kCWChannelBand2GHz:
        return RCS_WIFI_BAND_2_4_GHZ;
    case kCWChannelBand5GHz:
        return RCS_WIFI_BAND_5_GHZ;
    case kCWChannelBand6GHz:
        return RCS_WIFI_BAND_6_GHZ;
    default:
        return RCS_WIFI_BAND_UNKNOWN;
    }
}

uint32_t widthOf(CWChannelWidth width) {
    switch (width) {
    case kCWChannelWidth20MHz:
        return 20;
    case kCWChannelWidth40MHz:
        return 40;
    case kCWChannelWidth80MHz:
        return 80;
    case kCWChannelWidth160MHz:
        return 160;
    default:
        return 0;
    }
}
#endif

} // namespace

std::vector<rcs_wifi_info> wifiNetworks() {
    const bool awdl = awdlUp();
    std::vector<rcs_wifi_info> list;
#if TARGET_OS_OSX
    @autoreleasepool {
        for (CWInterface* i in [[CWWiFiClient sharedWiFiClient] interfaces]) {
            CWChannel* channel = i.wlanChannel;
            if (!i.powerOn || i.activePHYMode == kCWPHYModeNone || !channel || !i.interfaceName) {
                continue; // Off, or with no network (its channel then is the last one's).
            }
            rcs_wifi_info info{};
            copyName(info, i.interfaceName.UTF8String);
            info.index = if_nametoindex(info.name);
            info.band = bandOf(channel.channelBand);
            info.channel = uint32_t(channel.channelNumber);
            info.width_mhz = widthOf(channel.channelWidth);
            info.signal_dbm = int32_t(i.rssiValue);
            info.noise_dbm = int32_t(i.noiseMeasurement);
            info.rate_mbps = uint32_t(i.transmitRate);
            info.peer_to_peer = awdl;
            list.push_back(info);
        }
    }
#else
    for (const auto& network : networks()) {
        if (network.kind == RCS_NETWORK_WIFI) {
            rcs_wifi_info info{};
            copyName(info, network.name);
            info.index = network.index;
            info.peer_to_peer = awdl;
            list.push_back(info);
        }
    }
#endif
    return list;
}

uint32_t wifiFacts() noexcept {
#if TARGET_OS_OSX
    return RCS_WIFI_FACT_CHANNEL | RCS_WIFI_FACT_WIDTH | RCS_WIFI_FACT_SIGNAL | RCS_WIFI_FACT_NOISE |
           RCS_WIFI_FACT_RATE | RCS_WIFI_FACT_PEER_TO_PEER;
#else
    return RCS_WIFI_FACT_PEER_TO_PEER;
#endif
}

} // namespace rcsdk::platform
