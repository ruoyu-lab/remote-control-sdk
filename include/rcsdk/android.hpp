// Remote Control SDK - C++ wrapper of what Android hands over (rcsdk/android.h).
#pragma once

#include <rcsdk/android.h>
#include <rcsdk/rcsdk.hpp>

namespace rcsdk {

// The process's JavaVM* and any Context of the application (see rcs_android_initialize).
inline rcs_result initializeAndroid(void* javaVm, void* context) noexcept {
    return rcs_android_initialize(javaVm, context);
}

// Share: an AHardwareBuffer's picture, turned `rotation` degrees clockwise to stand
// (see rcs_session_push_hardware_buffer).
inline rcs_result pushHardwareBuffer(const Session& session, const void* hardwareBuffer,
                                     uint32_t rotation = 0) noexcept {
    rcs_hardware_buffer_info info = RCS_HARDWARE_BUFFER_INFO_INIT;
    info.rotation = rotation;
    return rcs_session_push_hardware_buffer(session.handle(), hardwareBuffer, &info);
}

} // namespace rcsdk
