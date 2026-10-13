// NVENC through its interface 12.0 (third_party/nvenc/12.0/nvEncodeAPI.h),
// for drivers from 522.25 that speak no 13.0 (before 570): no temporal layers
// in HEVC and AV1.
#include "platform/windows/nvenc_api.hpp"

// The header's own includes, first: inside the namespace they add nothing.
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

namespace rcsdk::platform::win::nvenc12 {
#include <nvenc/12.0/nvEncodeAPI.h>

constexpr const char* versionName = "12.0";
constexpr const char* firstDriver = "522.25";
} // namespace rcsdk::platform::win::nvenc12

#define RCSDK_NVENC_API nvenc12
#include "platform/windows/nvenc_api.inl"
