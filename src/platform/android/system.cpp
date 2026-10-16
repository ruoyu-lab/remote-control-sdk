// Android system queries. Android lets no application capture the screen or
// inject input from native code: MediaProjection and accessibility services
// are Java components of the host's. A sharer sends the host's own pictures
// (capture source HOST: rcs_session_push_hardware_buffer with what
// MediaProjection drew, encoded with MediaCodec; or ENCODED), and its host
// executes the viewers' input (RCS_INPUT_HOST).
#include "platform/android/codecs.hpp"
#include "platform/platform.hpp"

#include <rcsdk/gpu.h>

namespace rcsdk::platform {

rcs_capabilities capabilities() {
    rcs_capabilities value = RCS_CAPABILITIES_INIT;
    value.platform = RCS_PLATFORM_ANDROID;
    value.capture_sources = (1u << RCS_CAPTURE_HOST) | (1u << RCS_CAPTURE_ENCODED);
    // The host's pictures reach the encoder as RGB; it writes 4:2:0 8-bit.
    value.picture_formats = defaultPictureFormats;
    value.pushed_gpu_frames = 1u << RCS_GPU_AHARDWAREBUFFER;
    value.software_decoding_gpu_frames = 1; // Software decoders draw into hardware buffers too.
    value.wifi_low_latency = 1;
    return value;
}

std::shared_ptr<void> stayResponsive() {
    return nullptr;
}

std::vector<CodecImplementation> codecImplementations() {
    return android::implementations();
}

std::vector<rcs_display_info> displays() {
    return {};
}

std::vector<rcs_window_info> windows() {
    return {};
}

rcs_permission screenPermission() noexcept {
    return RCS_PERMISSION_UNAVAILABLE;
}

rcs_result requestScreenPermission() {
    return RCS_ERROR_UNSUPPORTED;
}

rcs_permission inputPermission() noexcept {
    return RCS_PERMISSION_UNAVAILABLE;
}

bool inputAvailable(bool) noexcept {
    return false;
}

rcs_result requestInputPermission() {
    return RCS_ERROR_UNSUPPORTED;
}

DesktopState desktopState() {
    return {};
}

std::unique_ptr<ScreenCapture> createScreenCapture(const CaptureTarget&) {
    fail(RCS_ERROR_UNSUPPORTED, "Android captures the screen only through MediaProjection in Java: push its "
                                "pictures (RCS_CAPTURE_HOST, rcs_session_push_hardware_buffer)");
}

Thumbnail captureThumbnail(const ThumbnailRequest&) {
    fail(RCS_ERROR_UNSUPPORTED, "Android captures the screen only through MediaProjection in Java");
}

std::unique_ptr<InputInjector> createInputInjector(const CaptureTarget&, const std::vector<uint32_t>&, const InputOptions&) {
    fail(RCS_ERROR_UNSUPPORTED, "Android lets an application act on others only through an accessibility "
                                "service of its own: execute the viewers' input in the host (RCS_INPUT_HOST)");
}

std::optional<SharedArea> sharedArea(const CaptureTarget&) {
    return std::nullopt;
}

} // namespace rcsdk::platform
