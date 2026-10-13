// Displays as capture, input and rcs_display_info name them. A display's id is
// the number N of its GDI device name \\.\DISPLAYN (DXGI_OUTPUT_DESC::DeviceName,
// MONITORINFOEX::szDevice): it stays while the display stays connected,
// whatever happens to the others or to which one is main. Id 0 asks for the
// main display; capture and input resolve it to that display's id when they
// start and keep it.
#pragma once

#include "platform/windows/d3d.hpp"

#include <cwchar>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace rcsdk::platform::win {

struct Monitor {
    HMONITOR handle = nullptr;
    uint32_t id = 0;
    std::wstring device; // \\.\DISPLAYN
    RECT rect{};         // Physical pixels on the virtual desktop.
    bool primary = false;
};

// N of "\\.\DISPLAYN"; 0 for any other name.
inline uint32_t displayNumber(const wchar_t* device) noexcept {
    static constexpr wchar_t prefix[] = L"\\\\.\\DISPLAY";
    constexpr size_t length = std::size(prefix) - 1;
    if (std::wcsncmp(device, prefix, length) != 0 || !device[length]) {
        return 0;
    }
    uint32_t number = 0;
    for (const wchar_t* digit = device + length; *digit; ++digit) {
        if (*digit < L'0' || *digit > L'9' || number > 100'000) {
            return 0;
        }
        number = number * 10 + uint32_t(*digit - L'0');
    }
    return number;
}

// The displays of the desktop, in physical pixels whatever the calling
// thread's DPI awareness.
inline std::vector<Monitor> monitors() {
    DpiScope dpi;
    std::vector<Monitor> result;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR handle, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(handle, &info)) {
                if (const uint32_t id = displayNumber(info.szDevice)) {
                    reinterpret_cast<std::vector<Monitor>*>(data)->push_back(
                        {handle, id, info.szDevice, info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

// The display `id` names; none while it is not attached.
inline std::optional<Monitor> findMonitor(uint32_t id) {
    for (auto& monitor : monitors()) {
        if (id ? monitor.id == id : monitor.primary) {
            return std::move(monitor);
        }
    }
    return std::nullopt;
}

} // namespace rcsdk::platform::win
