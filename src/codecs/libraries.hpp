// The codec libraries a build may carry, each in a file of its own
// (openh264.cpp, libvpx.cpp, libaom.cpp, dav1d.cpp) built when its
// RCSDK_WITH_* option is on.
#pragma once

#include "platform/platform.hpp"

#include <memory>
#include <vector>

namespace rcsdk::codecs {

namespace openh264 {
std::vector<platform::CodecImplementation> implementations();
std::unique_ptr<platform::VideoEncoder> encoder();
std::unique_ptr<platform::VideoDecoder> decoder();
} // namespace openh264

namespace libvpx {
std::vector<platform::CodecImplementation> implementations();
std::unique_ptr<platform::VideoEncoder> encoder();
std::unique_ptr<platform::VideoDecoder> decoder();
} // namespace libvpx

namespace libaom { // Encoding only.
std::vector<platform::CodecImplementation> implementations();
std::unique_ptr<platform::VideoEncoder> encoder();
} // namespace libaom

namespace dav1d { // Decoding only.
std::vector<platform::CodecImplementation> implementations();
std::unique_ptr<platform::VideoDecoder> decoder();
} // namespace dav1d

} // namespace rcsdk::codecs
