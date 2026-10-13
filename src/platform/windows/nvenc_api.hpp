// One version of NVIDIA's encoder interface (NVENC): nvenc_api.inl compiled
// against that version's header from third_party/nvenc, by nvenc_api13.cpp
// (13.0) and nvenc_api12.cpp (12.0). The versions lay out their structures
// differently (13.0 moved the HEVC and AV1 bit depths to fields of their own
// and gave their old bits to other flags), so one header cannot speak to a
// driver of the other. A driver speaks every version up to the highest it
// reports (NvEncodeAPIGetMaxSupportedVersion); nvenc.cpp takes the newest of
// these it speaks.
#pragma once

#include "platform/windows/nvenc.hpp"

#include <optional>

namespace rcsdk::platform::win {

struct NvencApi {
    uint32_t version;   // (major << 4) | minor, as drivers report versions.
    const char* name;   // "13.0".
    const char* driver; // The first Windows driver that speaks it.
    // Whether this process's driver speaks it (its function table loaded,
    // once per process), and the highest version the driver speaks (0: no
    // NVIDIA driver).
    bool (*loaded)();
    uint32_t (*supported)();
    // The codecs a session on `device` (an NVIDIA GPU's) encodes, as
    // (1u << codec) bits, with what `adapter` (when given) says of each
    // filled in; 0 when no session opens there.
    uint32_t (*probe)(ID3D11Device* device, NvencAdapter* adapter);
    std::unique_ptr<VideoEncoder> (*create)();
};

// The default GPU (where CPU frames go, as on the Media Foundation path) when
// it is one of `gpus`.
std::optional<NvencAdapter> nvencDefaultAdapter(const std::vector<NvencAdapter>& gpus);

namespace nvenc13 {
const NvencApi& api();
}
namespace nvenc12 {
const NvencApi& api();
}

} // namespace rcsdk::platform::win
