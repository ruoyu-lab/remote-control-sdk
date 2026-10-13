// The codec libraries built into the SDK (rcs_backend OPENH264, LIBVPX,
// LIBAOM, DAV1D): software encoders and decoders whose code is the same on
// every system. A build may leave each out (RCSDK_WITH_OPENH264 and the
// like). The platform layer gives them the captured pictures and takes the
// decoded ones (platform.hpp: YuvImage).
#pragma once

#include "platform/platform.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rcsdk::codecs {

// Every codec implementation of this machine: the system's
// (platform::codecImplementations) and the libraries', within each codec and
// direction hardware first, then software: the libraries before the system's
// (every picture format, reference invalidation, the same on every system),
// except H.264, where the system's own comes first.
std::vector<platform::CodecImplementation> implementations();

// Why this build has no implementation of a library backend ("this build has
// no libvpx (RCSDK_WITH_LIBVPX=OFF)"); empty for those it has and for the
// system's backends.
std::string missing(rcs_backend backend);

// The threads a library codec runs: `wanted` (rcs_video_tuning_config.
// software_threads), or with 0 half the logical processors, at most 8.
unsigned threads(uint32_t wanted) noexcept;

// What `acceleration` gives a stream of `codec`, `format` and this size (0 x 0:
// any): HARDWARE_FIRST is the hardware implementation of `backend` (on the
// GPU `adapter` names, for a sharer on Windows; 0: any) where one takes them,
// else software where one does (where neither does, hardware: its open says
// why); the others are themselves.
rcs_acceleration streamAcceleration(rcs_acceleration acceleration, rcs_backend backend, rcs_codec codec,
                                    rcs_codec_direction direction, rcs_picture_format format, uint32_t width,
                                    uint32_t height, uint64_t adapter,
                                    std::span<const platform::CodecImplementation> implementations);

// An encoder or decoder for `config` (its acceleration HARDWARE or SOFTWARE),
// not opened: a library's where the first implementation of its codec,
// acceleration and backend in `implementations` is one, else the system's.
std::unique_ptr<platform::VideoEncoder> createEncoder(const platform::EncoderConfig& config,
                                                      std::span<const platform::CodecImplementation> implementations);
std::unique_ptr<platform::VideoDecoder> createDecoder(const platform::DecoderConfig& config,
                                                      std::span<const platform::CodecImplementation> implementations);

} // namespace rcsdk::codecs
