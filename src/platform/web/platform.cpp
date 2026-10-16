// The browser (the SDK compiled to WebAssembly): decoding with WebCodecs
// (decoder.cpp), sound with Web Audio (audio.cpp); what the platform layer
// (docs/guide/23-browser.md) does not do reports nothing, as on
// the systems without a layer.
#include "platform/web/web.hpp"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <atomic>
#include <memory>

namespace rcsdk::platform {
namespace {

// What the page found the browser decodes: formats by codec, hardware and software.
std::atomic<uint32_t> decoderFormats[2][RCS_CODEC_HOST_FIRST]{};

void on(pthread_t thread, std::function<void()> work) {
    auto* job = new std::function<void()>(std::move(work));
    if (!emscripten_proxy_async(
            emscripten_proxy_get_system_queue(), thread,
            [](void* argument) {
                std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(argument));
                (*owned)();
            },
            job)) {
        delete job;
    }
}

} // namespace

pthread_t mediaThread() {
    static const pthread_t thread = [] {
        pthread_t t{};
        pthread_create(&t, nullptr, [](void*) -> void* {
            emscripten_exit_with_live_runtime(); // Back to its event loop: the page's work runs there.
            return nullptr;
        }, nullptr);
        return t;
    }();
    return thread;
}

void onPage(std::function<void()> work) {
    on(emscripten_main_runtime_thread_id(), std::move(work));
}

void onMedia(std::function<void()> work) {
    on(mediaThread(), std::move(work));
}

uint32_t webDecoderFormats(rcs_codec codec, bool hardware) noexcept {
    return codec < RCS_CODEC_HOST_FIRST ? decoderFormats[hardware][codec].load() : 0;
}


std::string webDecoderName(rcs_codec codec, bool hardware) {
    return std::string("WebCodecs ") + codecName(codec) + (hardware ? " (hardware)" : "");
}

rcs_capabilities capabilities() {
    rcs_capabilities value = RCS_CAPABILITIES_INIT;
    value.platform = RCS_PLATFORM_WEB;
    value.capture_sources = (1u << RCS_CAPTURE_HOST) | (1u << RCS_CAPTURE_ENCODED);
    value.software_decoding_gpu_frames = 1; // WebCodecs gives VideoFrames either way.
    return value;
}
std::vector<CodecImplementation> codecImplementations() {
    std::vector<CodecImplementation> list;
    for (const rcs_codec codec : standardCodecs) {
        for (const bool hardware : {true, false}) {
            if (const uint32_t formats = webDecoderFormats(codec, hardware)) {
                CodecImplementation decoder;
                decoder.codec = codec;
                decoder.direction = RCS_CODEC_DECODER;
                decoder.acceleration = hardware ? RCS_ACCELERATION_HARDWARE : RCS_ACCELERATION_SOFTWARE;
                decoder.backend = RCS_BACKEND_WEBCODECS;
                decoder.maxWidth = decoder.maxHeight = codec == RCS_CODEC_H264 ? 4096 : 8192;
                decoder.pictureFormats = formats;
                decoder.name = webDecoderName(codec, hardware);
                list.push_back(decoder);
            }
        }
    }
    return list;
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
std::shared_ptr<void> stayResponsive() {
    return nullptr;
}

std::unique_ptr<InputInjector> createInputInjector(const CaptureTarget&, const std::vector<uint32_t>&, const InputOptions&) {
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

// A picture is a VideoFrame of the page's, named by its number.
bool describeGpuFrame(const VideoFrame& frame, rcs_gpu_frame_info& info) {
    if (!frame.surface) {
        return false;
    }
    info.resource_type = RCS_GPU_WEB_VIDEO_FRAME;
    info.resource = frame.surface.get();
    info.width = frame.width;
    info.height = frame.height;
    info.surface_width = frame.width;
    info.surface_height = frame.height;
    return true;
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

// What the page's WebCodecs decode (VideoDecoder.isConfigSupported): `codec`
// in these picture formats, in hardware or software. Handed in before the
// first session.
extern "C" EMSCRIPTEN_KEEPALIVE void rcsdk_web_add_decoder(uint32_t codec, int hardware, uint32_t formats) {
    if (codec < RCS_CODEC_HOST_FIRST) {
        rcsdk::platform::decoderFormats[hardware != 0][codec] = formats;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE pthread_t rcsdk_web_media_thread(void) {
    return rcsdk::platform::mediaThread();
}
