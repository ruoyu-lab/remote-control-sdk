// The machine's hardware GPUs as DXGI lists them, each once.
#pragma once

#include "platform/windows/d3d.hpp"

#include <dxgi1_2.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace rcsdk::platform::win {

constexpr UINT nvidiaVendor = 0x10DE;

struct Adapter {
    LUID luid{};
    UINT vendorId = 0;
    std::wstring description;
};

// The hardware GPUs, in DXGI's order. A GPU can appear as several adapters
// with their own LUIDs (here an RTX 4090 three times, next to a virtual display
// driver); the first stands for it.
inline std::vector<Adapter> hardwareAdapters() {
    std::vector<DXGI_ADAPTER_DESC1> seen;
    std::vector<Adapter> result;
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.out())))) {
        return result;
    }
    for (UINT a = 0;; ++a) {
        Com<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.out()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            continue;
        }
        const bool repeated = std::any_of(seen.begin(), seen.end(), [&](const DXGI_ADAPTER_DESC1& other) {
            return other.VendorId == desc.VendorId && other.DeviceId == desc.DeviceId &&
                   other.SubSysId == desc.SubSysId && other.Revision == desc.Revision &&
                   other.DedicatedVideoMemory == desc.DedicatedVideoMemory &&
                   std::memcmp(other.Description, desc.Description, sizeof(desc.Description)) == 0;
        });
        if (!repeated) {
            seen.push_back(desc);
            result.push_back({desc.AdapterLuid, desc.VendorId, desc.Description});
        }
    }
    return result;
}

// The adapter a device was made on: its vendor and its name.
inline DXGI_ADAPTER_DESC adapterOf(ID3D11Device* device) {
    DXGI_ADAPTER_DESC desc{};
    Com<IDXGIDevice> dxgi;
    Com<IDXGIAdapter> adapter;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.out()))) &&
        SUCCEEDED(dxgi->GetAdapter(adapter.out()))) {
        adapter->GetDesc(&desc);
    }
    return desc;
}

// The DXGI adapter with this LUID; null when there is none.
inline Com<IDXGIAdapter1> adapterWith(const LUID& luid) {
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.out())))) {
        return {};
    }
    for (UINT a = 0;; ++a) {
        Com<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.out()) == DXGI_ERROR_NOT_FOUND) {
            return {};
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
            desc.AdapterLuid.HighPart == luid.HighPart) {
            return adapter;
        }
    }
}

} // namespace rcsdk::platform::win
