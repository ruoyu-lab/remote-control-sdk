// NVENC through its interface 13.0 (third_party/nvenc/nvEncodeAPI.h), which
// drivers 570 and later speak: HEVC's and AV1's temporal layers too.
#include "platform/windows/nvenc_api.hpp"

// The header's own includes, first: inside the namespace they add nothing.
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

namespace rcsdk::platform::win::nvenc13 {
#include <nvenc/nvEncodeAPI.h>

constexpr const char* versionName = "13.0";
constexpr const char* firstDriver = "570";
} // namespace rcsdk::platform::win::nvenc13

#define RCSDK_NVENC_API nvenc13
#include "platform/windows/nvenc_api.inl"
