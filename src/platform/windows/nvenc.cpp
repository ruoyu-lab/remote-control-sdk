// NVENC, through the newest version of its interface the driver speaks
// (nvenc_api.hpp): 13.0 from driver 570 on, 12.0 from 522.25 on (no temporal
// layers in HEVC and AV1 there, which the log says); none before.
#include "platform/windows/nvenc.hpp"

#include "platform/windows/adapters.hpp"
#include "platform/windows/nvenc_api.hpp"
#include "platform/windows/win.hpp"

#include "util/log.hpp"

#include <map>
#include <mutex>

namespace rcsdk::platform::win {
namespace {

// Why the driver, which speaks interfaces up to `supported`, gave no
// function table of `wanted`: too old, or it refused one it says it speaks.
std::string without(const NvencApi& wanted, uint32_t supported) {
    const std::string speaks =
        "the NVIDIA driver speaks interface " + std::to_string(supported >> 4) + "." + std::to_string(supported & 15);
    return supported >= wanted.version
               ? speaks + " but gave no function table of " + wanted.name
               : speaks + "; " + wanted.name + " needs driver " + wanted.driver + " or later";
}

// The interface this process uses, chosen once; null without an NVIDIA
// driver of 522.25 or later. Only the chosen one asks the driver for its
// function table: the driver keeps the version last asked for.
const NvencApi* nvenc() {
    static const NvencApi* const chosen = []() -> const NvencApi* {
        const NvencApi& newest = nvenc13::api();
        if (newest.loaded()) {
            return &newest;
        }
        const uint32_t supported = newest.supported();
        if (const NvencApi& older = nvenc12::api(); older.loaded()) {
            log::info("encoder", std::string("NVENC uses its interface ") + older.name +
                                     ", without temporal layers in HEVC and AV1: " + without(newest, supported));
            return &older;
        }
        if (supported) {
            log::info("encoder", "NVENC is not offered: " + without(nvenc12::api(), supported));
        }
        return nullptr;
    }();
    return chosen;
}

bool sameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

} // namespace

std::optional<NvencAdapter> nvencDefaultAdapter(const std::vector<NvencAdapter>& gpus) {
    const auto hardware = hardwareAdapters();
    for (const NvencAdapter& gpu : gpus) {
        if (!hardware.empty() && sameLuid(gpu.luid, hardware.front().luid)) {
            return gpu;
        }
    }
    return std::nullopt;
}

std::string nvencName(rcs_codec codec, const std::string& gpu) {
    return std::string("NVENC ") + codecName(codec) + " (" + gpu + ")";
}

std::vector<NvencAdapter> nvencAdapters() {
    const NvencApi* api = nvenc();
    if (!api) {
        return {};
    }
    struct Probe {
        std::mutex mutex;
        Com<IDXGIFactory1> factory;        // Null until every NVIDIA GPU answered...
        std::vector<NvencAdapter> answers; // ...with these.
    };
    static auto* probe = new Probe; // Not released while the process exits.
    std::lock_guard lock(probe->mutex);
    if (probe->factory && probe->factory->IsCurrent()) {
        return probe->answers;
    }
    Com<IDXGIFactory1> factory; // Before the adapters are listed: no longer current once they change.
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.out())))) {
        return {};
    }
    std::vector<NvencAdapter> found;
    bool answered = true;
    for (const Adapter& adapter : hardwareAdapters()) {
        if (adapter.vendorId != nvidiaVendor) {
            continue;
        }
        NvencAdapter gpu;
        gpu.luid = adapter.luid;
        gpu.name = narrow(adapter.description);
        if (const auto dxgi = adapterWith(adapter.luid)) {
            try {
                Device d3d;
                d3d.create(dxgi.value);
                api->probe(d3d.device.value, &gpu);
            } catch (const Failure&) { // No device on it now.
            }
        }
        if (gpu.codecs) {
            found.push_back(std::move(gpu));
        } else {
            answered = false; // Asked again next time: its sessions may all be in use now.
        }
    }
    probe->answers = found;
    probe->factory = answered ? std::move(factory) : Com<IDXGIFactory1>{};
    return found;
}

bool nvencFirst(rcs_codec codec, ID3D11Device* device) {
    const NvencApi* api = nvenc();
    if (!api) {
        return false;
    }
    if (!device) { // CPU frames: the default GPU's.
        const auto gpu = nvencDefaultAdapter(nvencAdapters());
        return gpu && (gpu->codecs & (1u << codec));
    }
    // The GPU of the device, probed once it answers (one GPU may be several
    // adapters); one that opens no session now is asked again next time.
    static std::mutex mutex;
    static std::map<uint64_t, uint32_t> codecs;
    const DXGI_ADAPTER_DESC adapter = adapterOf(device);
    if (adapter.VendorId != nvidiaVendor) {
        return false;
    }
    const uint64_t luid = (uint64_t(uint32_t(adapter.AdapterLuid.HighPart)) << 32) | adapter.AdapterLuid.LowPart;
    std::lock_guard lock(mutex);
    auto found = codecs.find(luid);
    if (found == codecs.end()) {
        const uint32_t mask = api->probe(device, nullptr);
        if (!mask) {
            return false;
        }
        found = codecs.emplace(luid, mask).first;
    }
    return (found->second & (1u << codec)) != 0;
}

// Without a driver that speaks either version, 12.0's encoder, which fails
// to open saying what NVENC needs.
std::unique_ptr<VideoEncoder> createNvencEncoder() {
    const NvencApi* api = nvenc();
    return (api ? *api : nvenc12::api()).create();
}

} // namespace rcsdk::platform::win
