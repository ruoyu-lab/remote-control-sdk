// Remote Control SDK - C++ wrapper of the host's GPU device (rcsdk/gpu.h).
// GpuFrame, which on_gpu_frame hands over, is in rcsdk/rcsdk.hpp.
#pragma once

#include <rcsdk/gpu.h>
#include <rcsdk/rcsdk.hpp>

namespace rcsdk {

template <>
inline rcs_gpu_device_config defaults<rcs_gpu_device_config>() noexcept {
    return RCS_GPU_DEVICE_CONFIG_INIT;
}

// View, Windows: decode on the host's ID3D11Device (see rcs_gpu_device_config):
//   config.use(rcsdk::gpuDevice(device));
inline rcs_gpu_device_config gpuDevice(void* d3d11Device) noexcept {
    rcs_gpu_device_config c = RCS_GPU_DEVICE_CONFIG_INIT;
    c.d3d11_device = d3d11Device;
    return c;
}

// Share with capture source HOST: the next picture as a GPU image of the
// host's own (see rcs_session_push_gpu_frame), e.g. a CVPixelBuffer.
inline rcs_result pushGpuFrame(const Session& session, const rcs_gpu_frame_info& frame) noexcept {
    return rcs_session_push_gpu_frame(session.handle(), &frame);
}

} // namespace rcsdk
