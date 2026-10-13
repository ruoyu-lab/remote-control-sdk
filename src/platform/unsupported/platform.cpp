// Systems without a platform layer (RCS_PLATFORM_UNKNOWN): the library builds
// and has the host's pictures and bitstreams, the codec libraries built in and
// the portable code, nothing of the system's.
#include "platform/platform.hpp"

namespace rcsdk::platform {

rcs_capabilities capabilities() {
    rcs_capabilities value = RCS_CAPABILITIES_INIT;
    value.capture_sources = (1u << RCS_CAPTURE_HOST) | (1u << RCS_CAPTURE_ENCODED);
    return value;
}
std::vector<CodecImplementation> codecImplementations() {
    return {};
}
DesktopState desktopState() {
    return {};
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

std::unique_ptr<ScreenCapture> createScreenCapture(const CaptureTarget&) {
    fail(RCS_ERROR_UNSUPPORTED, "Screen capture is not available on this platform");
}
Thumbnail captureThumbnail(const ThumbnailRequest&) {
    fail(RCS_ERROR_UNSUPPORTED, "Screen capture is not available on this platform");
}
std::unique_ptr<VideoEncoder> createVideoEncoder(const EncoderConfig&) {
    fail(RCS_ERROR_UNSUPPORTED, "Video encoding is not available on this platform");
}
std::unique_ptr<VideoDecoder> createVideoDecoder(const DecoderConfig&) {
    fail(RCS_ERROR_UNSUPPORTED, "Video decoding is not available on this platform");
}
std::unique_ptr<InputInjector> createInputInjector(const CaptureTarget&, const std::vector<uint32_t>&, bool) {
    fail(RCS_ERROR_UNSUPPORTED, "Input injection is not available on this platform");
}
std::optional<SharedArea> sharedArea(const CaptureTarget&) {
    return std::nullopt;
}

void withCpuImage(const VideoFrame&, const std::function<void(const rcs_frame&)>&) {
    fail(RCS_ERROR_UNSUPPORTED, "Native surfaces are not available on this platform");
}
std::unique_ptr<SurfaceReader> createSurfaceReader() {
    fail(RCS_ERROR_CODEC, "Native surfaces are not available here");
}
std::unique_ptr<SurfaceScaler> createSurfaceScaler() {
    fail(RCS_ERROR_UNSUPPORTED, "Native surfaces are not available here");
}

// No surfaces here: the host gets CPU pixels.
std::unique_ptr<SurfaceWriter> createSurfaceWriter(const DecoderConfig&) {
    struct CpuWriter final : SurfaceWriter {
        VideoFrame write(const YuvImage& image, const Picture& picture) override {
            VideoFrame frame;
            toBgra(image, picture, frame);
            return frame;
        }
    };
    return std::make_unique<CpuWriter>();
}
uint64_t deviceAdapter(const std::shared_ptr<void>&) noexcept {
    return 0;
}

std::shared_ptr<void> retainHostDevice(void*) {
    return nullptr; // Only Windows takes a host device; the configuration is validated.
}

void checkHostDevice(void*, uint32_t) {
    fail(RCS_ERROR_UNSUPPORTED, "Host GPU devices are used on Windows only");
}

bool describeGpuFrame(const VideoFrame&, rcs_gpu_frame_info&) {
    return false;
}

VideoFrame hostGpuFrame(const rcs_gpu_frame_info&) {
    fail(RCS_ERROR_UNSUPPORTED, "This system takes no GPU pictures from the host (rcs_capabilities.pushed_gpu_frames)");
}

HostImages hostImages() noexcept {
    return HostImages::none;
}

std::unique_ptr<SamplePictures> createSamplePictures() {
    fail(RCS_ERROR_UNSUPPORTED, "This system has no Core Media sample buffers: push pictures with "
                                "rcs_session_push_video_frame");
}

std::unique_ptr<SampleSound> createSampleSound() {
    fail(RCS_ERROR_UNSUPPORTED, "This system has no Core Media sample buffers: push sound with rcs_session_push_audio");
}

std::unique_ptr<AudioCapture> createAudioCapture(rcs_audio_capture_backend) {
    fail(RCS_ERROR_UNSUPPORTED, "Audio capture is not available on this platform");
}
uint32_t audioCaptureBackends() noexcept {
    return 0;
}
rcs_audio_capture_backend defaultAudioCaptureBackend() noexcept {
    return RCS_AUDIO_CAPTURE_AUTO;
}
bool audioCapturePeriodSelectable(rcs_audio_capture_backend) noexcept {
    return false;
}
bool audioExclusionAvailable() noexcept {
    return false;
}
std::unique_ptr<AudioPlayer> createAudioPlayer() {
    fail(RCS_ERROR_UNSUPPORTED, "Audio playback is not available on this platform");
}
std::vector<AudioDevice> audioDevices() {
    return {};
}
rcs_permission microphonePermission() noexcept {
    return RCS_PERMISSION_UNAVAILABLE;
}
rcs_result requestMicrophonePermission() {
    return RCS_ERROR_UNSUPPORTED;
}
std::unique_ptr<AudioCapture> createMicrophoneCapture(const MicrophoneConfig&) {
    fail(RCS_ERROR_UNSUPPORTED, "Microphones are not available on this platform");
}
std::unique_ptr<audio::Encoder> createAacEncoder(uint32_t) {
    fail(RCS_ERROR_UNSUPPORTED, "AAC is not available on this platform");
}
std::unique_ptr<audio::Decoder> createAacDecoder(std::span<const uint8_t>) {
    fail(RCS_ERROR_UNSUPPORTED, "AAC is not available on this platform");
}
std::vector<uint32_t> aacBitrates() {
    return {};
}

} // namespace rcsdk::platform
