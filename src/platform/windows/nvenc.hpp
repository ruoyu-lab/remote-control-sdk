// NVIDIA's encoder interface (NVENC), driven directly (RCS_BACKEND_NVENC):
// the driver's nvEncodeAPI64.dll, loaded when first needed, through the
// newest version of the interface it speaks of the two the SDK has (13.0
// from driver 570 on, 12.0 from 522.25 on; nvenc_api.hpp); their
// declarations come from third_party/nvenc.
#pragma once

#include "media/picture_size.hpp"
#include "platform/platform.hpp"

#include <d3d11.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace rcsdk::platform::win {

// Each NVIDIA GPU with NVENC (hardwareAdapters(), once each): the codecs it
// encodes, as (1u << codec) bits, the largest picture of each (as the driver
// reports, within the codec's own limit), the picture formats of each (as its
// capabilities say), and its name. Empty without an
// NVIDIA driver of 522.25 or later. Probed once every NVIDIA GPU answered,
// and again once the adapters change (IDXGIFactory1::IsCurrent); a GPU that
// opens no session (its sessions all in use, no device now) is left out and
// asked again at the next call.
struct NvencAdapter {
    LUID luid{};
    uint32_t codecs = 0;
    std::array<PictureSize, RCS_CODEC_AV1 + 1> largest{}; // By codec...
    std::array<uint32_t, RCS_CODEC_AV1 + 1> formats{};    // ...as (1u << rcs_picture_format) bits.
    uint32_t refInvalidation = 0; // Codecs whose sessions drop references on request, as (1u << codec) bits.
    // The most temporal layers each makes (1: none): temporal SVC where the
    // driver says it can, H.264's in either interface, HEVC's and AV1's in 13.0.
    std::array<uint32_t, RCS_CODEC_AV1 + 1> temporalLayers{};
    // The pixels a second each takes at most, by picture format: its highest
    // level's (NV_ENC_CAPS_LEVEL_MAX; for 4:4:4 the highest that starts); 0:
    // the driver does not say.
    std::array<std::array<uint64_t, 8>, RCS_CODEC_AV1 + 1> pixelRates{};
    std::string name;
};
std::vector<NvencAdapter> nvencAdapters();

// RCS_BACKEND_AUTO: whether NVENC comes first for `codec` on the GPU of
// `device` (null: CPU frames, uploaded to the default GPU), before Media
// Foundation (and codecImplementations() lists it first there). Measured on
// an RTX 4090 (docs/PERFORMANCE.md): NVENC encodes 1 to 2.5 ms faster per
// frame and answers input about 2 ms sooner end to end.
bool nvencFirst(rcs_codec codec, ID3D11Device* device);

// The name an NVENC encoder of `codec` on `gpu` goes by.
std::string nvencName(rcs_codec codec, const std::string& gpu);

std::unique_ptr<VideoEncoder> createNvencEncoder();

} // namespace rcsdk::platform::win
