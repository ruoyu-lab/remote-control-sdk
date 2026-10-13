// NVIDIA's video decoder interface (NVDEC), driven directly
// (RCS_BACKEND_NVDEC), for what Windows' decoders decode on the CPU only:
// 4:4:4 pictures (HEVC's range extensions, on GPUs that decode them). The
// driver's nvcuvid.dll and the CUDA driver (nvcuda.dll) are loaded when first
// needed; their declarations come from third_party/nvdec. NVDEC's own parser
// reads the stream.
#pragma once

#include "media/picture_size.hpp"
#include "platform/platform.hpp"

#include <d3d11.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace rcsdk::platform::win {

// Each NVIDIA GPU NVDEC decodes 4:4:4 pictures on (hardwareAdapters(), once
// each): the codecs, as (1u << codec) bits, the largest picture of each and
// its 4:4:4 picture formats, as the driver reports them (cuvidGetDecoderCaps),
// and its name. Empty without an NVIDIA driver. Probed once: CUDA sees the
// GPUs there were when it started.
struct NvdecAdapter {
    LUID luid{};
    uint32_t codecs = 0;
    std::array<PictureSize, RCS_CODEC_AV1 + 1> largest{}; // By codec...
    std::array<uint32_t, RCS_CODEC_AV1 + 1> formats{};    // ...as (1u << rcs_picture_format) bits.
    std::string name;
};
const std::vector<NvdecAdapter>& nvdecAdapters();

// The name an NVDEC decoder of `codec` on `gpu` goes by.
std::string nvdecName(rcs_codec codec, const std::string& gpu);

// RCS_BACKEND_AUTO in hardware: whether a stream of `codec` pictures of
// `format` goes to NVDEC, on the GPU of `device` (null: on one of its own):
// a 4:4:4 one, which Media Foundation decodes on the CPU only, where NVDEC
// decodes it there.
bool nvdecFirst(rcs_codec codec, rcs_picture_format format, ID3D11Device* device);

std::unique_ptr<VideoDecoder> createNvdecDecoder();

} // namespace rcsdk::platform::win
