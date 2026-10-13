// Intel's Video Processing Library (oneVPL), driven directly
// (RCS_BACKEND_ONEVPL): the runtime the Intel graphics driver installs
// (libmfx64-gen.dll, in the driver's store, where the display adapter's
// DriverStorePathForVPL registry value says: where oneVPL's own dispatcher
// looks first), loaded when first needed and called without the dispatcher;
// its declarations come from third_party/onevpl. It encodes H.264, HEVC and
// AV1 on Intel GPUs, next to their Media Foundation encoders, with what those
// do not offer: AV1 where Windows lists no Intel AV1 encoder, HEVC 4:4:4
// where the GPU encodes it, three temporal layers, reference invalidation
// with per-picture reference lists, the fixed-function low-power encoder.
// Not VP9: Intel's runtime lists a VP9 encoder but ignores the keyframes
// asked of it (driver 32.0.101.8860). It decodes H.264, HEVC, VP9 and AV1
// there, with what Windows' decoders do not: the 4:4:4 they decode on the
// CPU only (HEVC's range extensions, VP9 profiles 1 and 3, AV1 High, where
// the GPU decodes them), and codecs whose Video Extension is not installed.
#pragma once

#include "media/picture_size.hpp"
#include "platform/platform.hpp"

#include <d3d11.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace rcsdk::platform::win {

// What a GPU's encoders or decoders take: the codecs, as (1u << codec) bits,
// the largest picture of each and its picture formats (the profiles and the
// input or output layouts the runtime lists for Direct3D 11 textures).
struct VplCodecs {
    uint32_t codecs = 0;
    std::array<PictureSize, RCS_CODEC_AV1 + 1> largest{}; // By codec...
    std::array<uint32_t, RCS_CODEC_AV1 + 1> formats{};    // ...as (1u << rcs_picture_format) bits.
};

// Each Intel GPU the runtime encodes or decodes on (as its implementation
// descriptions say; one per adapter, matched by LUID): its encoders and
// decoders, whether its encoders drop references on request and how many
// temporal layers they make, and its name. Empty without the runtime. Probed
// once.
struct VplAdapter {
    LUID luid{};
    uint32_t implementation = 0; // The runtime's number for it (VendorImplID).
    VplCodecs encoders, decoders;
    uint32_t refInvalidation = 0; // Codecs, as (1u << codec) bits.
    std::array<uint32_t, RCS_CODEC_AV1 + 1> temporalLayers{};
    std::string name;
};
const std::vector<VplAdapter>& vplAdapters();

// The name a oneVPL encoder or decoder of `codec` on `gpu` goes by.
std::string vplName(rcs_codec codec, const std::string& gpu);

// RCS_BACKEND_AUTO: whether oneVPL comes first for `codec` on the GPU of
// `device` (null: CPU frames, uploaded to the default GPU), before Media
// Foundation (and codecImplementations() lists it first there).
bool vplFirst(rcs_codec codec, ID3D11Device* device);

// RCS_BACKEND_AUTO in hardware: whether a stream of `codec` pictures of
// `format` goes to oneVPL's decoder, on the GPU of `device` (null: on one of
// its own), where oneVPL decodes it there: a 4:4:4 one, which Media
// Foundation decodes on the CPU only, or one of a codec Windows has no
// decoder of. Its 4:2:0 otherwise stays with Media Foundation, which takes
// 0.5 to 3 ms less a picture on the same GPU (docs/PERFORMANCE.md).
bool vplDecodesFirst(rcs_codec codec, rcs_picture_format format, ID3D11Device* device);

std::unique_ptr<VideoEncoder> createVplEncoder();
std::unique_ptr<VideoDecoder> createVplDecoder();

} // namespace rcsdk::platform::win
