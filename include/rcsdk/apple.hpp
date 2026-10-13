// Remote Control SDK - C++ wrapper of what Core Media hands over (rcsdk/apple.h).
#pragma once

#include <rcsdk/apple.h>
#include <rcsdk/rcsdk.hpp>

namespace rcsdk {

// Share: a CMSampleBufferRef's picture or sound (see rcs_session_push_sample_buffer);
// orientation 0 takes ReplayKit's from the buffer.
inline rcs_result pushSampleBuffer(const Session& session, const void* sampleBuffer, rcs_sample_buffer_type type,
                                   uint32_t orientation = 0) noexcept {
    rcs_sample_buffer_info info = RCS_SAMPLE_BUFFER_INFO_INIT;
    info.type = type;
    info.orientation = orientation;
    return rcs_session_push_sample_buffer(session.handle(), sampleBuffer, &info);
}

} // namespace rcsdk
