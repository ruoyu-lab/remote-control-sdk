// iOS system queries. iOS lets no application capture the screen or inject
// input into other applications, so nothing here is captured or controlled:
// a sharer sends the host's own pictures (capture sources HOST, encoded with
// VideoToolbox, and ENCODED), and its host executes the viewers' input.
#include "media/picture_size.hpp"
#include "platform/apple/codecs.hpp"
#include "platform/platform.hpp"

#include <rcsdk/gpu.h>

namespace rcsdk::platform {

rcs_capabilities capabilities() {
    rcs_capabilities value = RCS_CAPABILITIES_INIT;
    value.platform = RCS_PLATFORM_IOS;
    value.capture_sources = (1u << RCS_CAPTURE_HOST) | (1u << RCS_CAPTURE_ENCODED);
    // The host's pictures reach the encoder in its YCbCr layout, converted where they are not.
    value.picture_formats = sdrPictureFormats;
    value.pushed_gpu_frames = 1u << RCS_GPU_CVPIXELBUFFER;
    value.software_decoding_gpu_frames = 1; // Software decoders output IOSurface pixel buffers too.
    return value;
}

std::vector<CodecImplementation> codecImplementations() {
    auto result = apple::encoderImplementations();
    const auto decoders = apple::decoderImplementations();
    result.insert(result.end(), decoders.begin(), decoders.end());
    return result;
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
    fail(RCS_ERROR_UNSUPPORTED, "iOS lets no application capture the screen; share the host's bitstream "
                                "(RCS_CAPTURE_ENCODED)");
}

Thumbnail captureThumbnail(const ThumbnailRequest&) {
    fail(RCS_ERROR_UNSUPPORTED, "iOS lets no application capture the screen");
}

std::unique_ptr<InputInjector> createInputInjector(const CaptureTarget&, const std::vector<uint32_t>&, const InputOptions&) {
    fail(RCS_ERROR_UNSUPPORTED, "iOS lets no application inject input into others");
}

std::optional<SharedArea> sharedArea(const CaptureTarget&) {
    return std::nullopt;
}

} // namespace rcsdk::platform
