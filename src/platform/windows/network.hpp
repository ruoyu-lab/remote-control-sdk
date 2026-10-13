// Windows' Wi-Fi settings for streaming (network.cpp).
#pragma once

namespace rcsdk::platform::win {

// Whether this system has the WLAN service's client API (rcs_capabilities.wifi_low_latency).
bool wifiSettable() noexcept;

} // namespace rcsdk::platform::win
