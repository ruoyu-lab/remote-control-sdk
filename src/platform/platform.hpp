// Operating-system services the session builds on. One implementation per
// platform lives in platform/<os>/; all of them throw rcsdk::Failure on error.
#pragma once

#include "media/audio_codec.hpp"
#include "media/cursor.hpp"
#include "media/video.hpp"
#include "media/yuv_image.hpp"
#include "util/api.hpp"
#include "util/util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rcsdk::platform {

// ---- System queries --------------------------------------------------------

// The platform, the capture sources, the input types it executes, the
// picture formats, the GPU pictures it takes and makes, and the rest this
// system can say; the codec masks are left to the session, which derives
// them from codecImplementations().
rcs_capabilities capabilities();

struct CodecImplementation {
    rcs_codec codec = 0;
    rcs_codec_direction direction = RCS_CODEC_ENCODER;
    rcs_acceleration acceleration = RCS_ACCELERATION_HARDWARE;
    rcs_backend backend = RCS_BACKEND_AUTO;
    uint64_t adapterLuid = 0;
    // The largest picture it takes, within the codec's own limit
    // (rcs_codec_implementation.max_width).
    uint32_t maxWidth = 0, maxHeight = 0;
    // The picture formats it encodes or decodes, as (1u << rcs_picture_format) bits.
    uint32_t pictureFormats = defaultPictureFormats;
    std::string name;
    // Encoders: drops pictures from its references on request
    // (VideoEncoder::invalidateAfter, rcs_codec_implementation.ref_invalidation)...
    bool refInvalidation = false;
    // ...and the most temporal layers it makes (EncoderConfig::temporalLayers,
    // rcs_codec_implementation.temporal_layers); 1 where it makes none.
    uint32_t temporalLayers = 1;
    // The most pixels a second it takes in each picture format (by
    // rcs_picture_format; its highest level's: media/levels.hpp), as width x
    // height x frames per second; 0 where nothing says
    // (rcs_codec_implementation.max_pixels_per_second).
    std::array<uint64_t, 8> maxPixelRates{};
};
// Every encoder and decoder createVideoEncoder()/createVideoDecoder() can open,
// best first within a codec, direction and acceleration: RCS_BACKEND_AUTO
// takes the first for the GPU in question.
std::vector<CodecImplementation> codecImplementations();
std::vector<rcs_display_info> displays();
// rcs_list_windows: the windows a user would pick from to share, front to back.
std::vector<rcs_window_info> windows();
rcs_permission screenPermission() noexcept;
rcs_result requestScreenPermission();
// Whether this process may inject input at all (rcs_input_permission)...
rcs_permission inputPermission() noexcept;
// ...and whether the system takes it now: on Windows the desktop that receives
// input must be the user's, or with `secureDesktop` one this process follows
// (rcs_capture_config.secure_desktop). A lock screen pauses it; it is no
// matter of permission.
bool inputAvailable(bool secureDesktop) noexcept;
rcs_result requestInputPermission();
// Takes a reference to a host's GPU device (Windows: ID3D11Device); null stays null.
std::shared_ptr<void> retainHostDevice(void* device);
// Throws Failure naming why decoding cannot run on the host's device
// (rcs_gpu_device_config.d3d11_device) on this system: INVALID_ARGUMENT for a
// device the host made unsuitable, UNSUPPORTED for what its GPU cannot do.
// `codecs`: (1u << codec) bits; the GPU must decode one of them.
void checkHostDevice(void* device, uint32_t codecs);

// ---- Screen capture --------------------------------------------------------

// What a capture shows, and what remote input maps across (rcs_capture_config).
struct CaptureTarget {
    rcs_capture_source source = RCS_CAPTURE_DISPLAY; // DISPLAY, REGION, WINDOW or APPLICATION.
    uint32_t displayId = 0;                          // DISPLAY, REGION, APPLICATION; 0: the main display.
    Rect region;                                     // REGION: in the display's pixels.
    uint64_t windowId = 0;                           // WINDOW.
    uint32_t processId = 0;                          // APPLICATION.
    bool operator==(const CaptureTarget&) const = default;
};

struct CaptureConfig {
    uint32_t width = 0, height = 0, framesPerSecond = 60;
    CaptureTarget target;
    bool excludeCurrentProcess = false;
    bool cursorInVideo = true; // Else the pointer is left out and reported by cursor().
    bool secureDesktop = false; // Windows: follow the desktop that receives input (secure ones too).
    // What the encoder makes of the pictures: macOS captures in its YCbCr
    // layout (4:2:0 or 4:4:4, 8 or 10 bits, the matrix and range), Windows
    // draws SDR or, for HDR10, PQ with BT.2020 primaries.
    Picture picture;
};

// What is shared, as capture sees it: the size in pixels of the display, the
// region or the window (APPLICATION: its display), and the display's refresh
// rate (0: the system does not say).
struct DisplayMode {
    uint32_t width = 0, height = 0, refreshHz = 0;
    bool operator==(const DisplayMode&) const = default;
};

class ScreenCapture {
public:
    virtual ~ScreenCapture() = default;
    virtual void start(const CaptureConfig& config, CancelToken cancelled) = 0;
    // Waits up to `timeout` for a new picture. Returns false on timeout, when
    // interrupt() was called or while paused; a static screen produces no new
    // pictures.
    virtual bool next(VideoFrame& frame, std::chrono::milliseconds timeout, CancelToken cancelled) = 0;
    // Wakes a blocked next(), e.g. because encoded output became ready.
    virtual void interrupt() noexcept {}
    // Until untilNs (monotonicNs), pictures come at twice the configured rate:
    // right after remote input, its effect matters more than the frame budget.
    void boost(uint64_t untilNs) noexcept { boostUntilNs_ = untilNs; }
    // Draws the pointer into the pictures or leaves it out from the next one
    // on. Left out, pointer moves alone produce no picture; cursor() follows it.
    virtual void setCursorInVideo(bool inVideo) = 0;
    // The pointer as of the latest next(); imageSerial moves on with its image.
    virtual CursorState cursor() const = 0;
    virtual void stop() noexcept = 0;
    virtual uint64_t captured() const noexcept = 0;
    virtual uint64_t overwritten() const noexcept = 0;
    // Why nothing can be captured right now (a lock screen, a secure desktop,
    // a display mode change; a window minimized or closed): next() resumes by
    // itself once it can, except after GONE.
    virtual rcs_capture_pause paused() const noexcept { return RCS_CAPTURE_PAUSE_NONE; }
    // How often capture paused.
    virtual uint64_t interruptions() const noexcept { return 0; }

protected:
    bool boosted() const noexcept { return monotonicNs() < boostUntilNs_.load(); }

    // Pacing to the frame rate without a cadence of its own: a picture goes
    // as soon as the rate allows, with credit for half a picture more, so
    // that a source at the stream's own rate (a 60 Hz screen streamed at 60
    // FPS, a host pushing at the stream's rate) never waits for a phase of
    // the SDK's and jitter costs nothing; over time no more than the rate go,
    // twice it while boosted. Capture thread only: pace() at start, due()
    // says when the next picture may go, took() when one went.
    void pace(uint32_t framesPerSecond) noexcept {
        interval_ = std::chrono::nanoseconds(1'000'000'000 / framesPerSecond);
        credit_ = mostCredit;
        credited_ = Clock::now();
    }
    Clock::time_point due() const noexcept {
        const double rate = boosted() ? 2 : 1;
        const auto now = Clock::now();
        const double credit = creditAt(now, rate);
        return credit >= 1 ? now
                           : now + std::chrono::duration_cast<Clock::duration>(interval_ * ((1 - credit) / rate));
    }
    void took() noexcept {
        const auto now = Clock::now();
        credit_ = std::max(0.0, creditAt(now, boosted() ? 2 : 1) - 1);
        credited_ = now;
    }

private:
    double creditAt(Clock::time_point time, double rate) const noexcept {
        return std::min(mostCredit, credit_ + rate * std::chrono::duration<double>(time - credited_) / interval_);
    }

    static constexpr double mostCredit = 1.5;
    std::atomic<uint64_t> boostUntilNs_{0};
    std::chrono::nanoseconds interval_{16'666'667};
    double credit_ = mostCredit;
    Clock::time_point credited_{};

public:
    // After start(): where what is shared sits inside each frame
    // (letterboxing), and its own mode. May change with any frame: a window
    // that resizes, capture that resumes after a display mode change.
    virtual Rect content() const noexcept = 0;
    virtual DisplayMode display() const noexcept = 0;
    // After start(): the ID3D11Device producing the frames (Windows), else null.
    virtual std::shared_ptr<void> device() const noexcept { return {}; }
    // After start(), for HDR10 pictures: the shared display's metadata as the
    // system describes it (rcs_hdr_metadata); 0 where it says nothing.
    virtual rcs_hdr_metadata hdrMetadata() const noexcept { return {}; }
};

// A capture for `target`'s source; Failure(UNSUPPORTED) for one this system
// does not offer (rcs_capabilities.capture_sources).
std::unique_ptr<ScreenCapture> createScreenCapture(const CaptureTarget& target);

// ---- Thumbnails --------------------------------------------------------------

// What rcs_capture_picture takes a picture of, and how large it may be.
struct ThumbnailRequest {
    CaptureTarget target;
    bool excludeCurrentProcess = false;
    bool secureDesktop = false;
    uint32_t maxWidth = 0, maxHeight = 0; // 1..8192
    bool operator==(const ThumbnailRequest&) const = default;
};
inline constexpr uint32_t maxThumbnailSide = 8192;

// A picture as rcs_picture holds it: opaque BGRA8 rows of width * 4 bytes.
struct Thumbnail {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

// The size of a thumbnail of a picture `width` x `height`: fitted into
// maxWidth x maxHeight keeping the aspect ratio, never enlarged.
inline std::pair<uint32_t, uint32_t> thumbnailSize(uint32_t width, uint32_t height, uint32_t maxWidth,
                                                   uint32_t maxHeight) noexcept {
    if (width <= maxWidth && height <= maxHeight) {
        return {width, height};
    }
    const double scale = std::min(double(maxWidth) / width, double(maxHeight) / height);
    return {uint32_t(std::clamp<long>(std::lround(width * scale), 1, long(maxWidth))),
            uint32_t(std::clamp<long>(std::lround(height * scale), 1, long(maxHeight)))};
}

// Takes one picture of what `request` shares, now, without the pointer, at
// thumbnailSize() of what is shared. Throws Failure: INVALID_STATE where a
// running capture would pause (the reason says why), UNSUPPORTED for a
// setting one picture cannot carry out, TIMEOUT when no picture comes within
// three seconds, CAPTURE when capture fails.
Thumbnail captureThumbnail(const ThumbnailRequest& request);

// ---- Video encoding --------------------------------------------------------

// rcs_video_tuning_config.software_threads: 1..64, 0 the SDK's choice.
inline constexpr uint32_t maxSoftwareThreads = 64;

struct EncoderConfig {
    rcs_codec codec = RCS_CODEC_H264;
    uint32_t width = 0, height = 0, framesPerSecond = 60, bitrate = 0;
    rcs_acceleration acceleration = RCS_ACCELERATION_HARDWARE; // HARDWARE or SOFTWARE.
    rcs_backend backend = RCS_BACKEND_AUTO;
    std::shared_ptr<void> device; // ScreenCapture::device(); null for CPU frames.
    Picture picture;              // One of the implementation's pictureFormats.
    rcs_encoder_speed speed = RCS_ENCODER_FASTEST;
    // The codec libraries' (src/codecs/): screen content tools (AUTO decided
    // already: DETAIL or MOTION), and their threads (0: the SDK's choice).
    rcs_content_hint content = RCS_CONTENT_DETAIL;
    uint32_t softwareThreads = 0;
    // Temporal layers to make (rcs_scalability), 1..3: an encoder makes as
    // many of them as it can (VideoEncoder::temporalLayers), each picture's
    // in EncodedPacket::temporalId and discardable.
    uint32_t temporalLayers = 1;
};

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    // Uses exactly the configured codec and acceleration: throws
    // Failure(RCS_ERROR_UNSUPPORTED) when they are not available, never substitutes.
    virtual void open(const EncoderConfig& config) = 0;
    virtual void submit(const VideoFrame& frame) = 0;
    // Returns the next finished packet, waiting up to `timeout`.
    virtual bool receive(EncodedPacket& packet, std::chrono::milliseconds timeout) = 0;
    // Frames submitted but not yet returned by receive().
    virtual size_t inFlight() const noexcept = 0;
    virtual void requestKeyframe() noexcept = 0;
    // Reference invalidation: drops the pictures after `sequence` from its
    // references, so that the next packet refers to none of them and says so
    // (EncodedPacket::recoversAfter). An encoder that cannot, or no longer
    // holds what came before, makes the next packet a keyframe.
    virtual void invalidateAfter(uint64_t sequence) noexcept {
        (void)sequence;
        requestKeyframe();
    }
    virtual bool refInvalidation() const noexcept { return false; }
    // The temporal layers it makes now (1: none), at most those configured;
    // may change along with lowLatency(). A picture of layer t refers to
    // pictures of layers up to t only, so that those of the layers above a
    // viewer's may be left out; a viewer may take the layers above again
    // from a picture of layer 0 on.
    virtual uint32_t temporalLayers() const noexcept { return 1; }
    // Target bitrate from the next submitted frame on (adaptive bitrate).
    virtual void setBitrate(uint32_t bitsPerSecond) noexcept = 0;
    // The target the encoder runs at: the configured one, then each one of
    // setBitrate() it took. One it refused leaves the previous in place
    // (logged once per encoder).
    virtual uint32_t bitrate() const noexcept = 0;
    // Encodes one frame at a time with the least latency (rcs_video_send_stats::low_latency).
    virtual bool lowLatency() const noexcept = 0;
    // After open(): the implementation in use, as named by codecImplementations().
    // May change along with lowLatency().
    virtual std::string name() const = 0;
    // The backend it runs on (RCS_BACKEND_HOST: the host's own).
    virtual rcs_backend backend() const noexcept = 0;
    // Called from an encoder thread whenever a packet becomes ready.
    virtual void onOutputReady(std::function<void()> callback) { (void)callback; }
};

// An encoder for `config` (its backend, or with AUTO the first the system
// lists for the codec on the GPU of config.device), not opened yet.
std::unique_ptr<VideoEncoder> createVideoEncoder(const EncoderConfig& config);

// ---- Video decoding --------------------------------------------------------

struct DecoderConfig {
    rcs_codec codec = RCS_CODEC_H264;
    rcs_acceleration acceleration = RCS_ACCELERATION_HARDWARE; // HARDWARE or SOFTWARE.
    rcs_backend backend = RCS_BACKEND_AUTO;
    uint32_t softwareThreads = 0; // The codec libraries' (0: the SDK's choice).
    // The host takes CPU images: surfaces are converted to BGRA where they
    // are decoded (VideoToolbox; the GPU's video processor on Windows), so
    // reading them needs no conversion. Otherwise surfaces keep the stream's
    // YCbCr layout (NV12, P010, NV24, P410).
    bool cpuImages = false;
    // What the stream's packets are (EncodedPacket::picture): the surfaces'
    // layout and their colour follow it.
    Picture picture;
    // Windows: the host's ID3D11Device to decode on (rcs_gpu_device_config.d3d11_device).
    std::shared_ptr<void> device;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // Uses exactly the configured codec and acceleration: throws
    // Failure(RCS_ERROR_UNSUPPORTED) when they are not available, never substitutes.
    virtual void open(const DecoderConfig& config) = 0;
    // Returns false while the decoder needs more input before producing a picture.
    virtual bool decode(const EncodedPacket& packet, VideoFrame& frame, CancelToken cancelled) = 0;
    // Discards decoder state; the next packet must be a keyframe.
    virtual void reset() = 0;
    // After open(): the implementation in use, as named by codecImplementations().
    virtual std::string name() const = 0;
    // The backend it runs on (RCS_BACKEND_HOST: the host's own).
    virtual rcs_backend backend() const noexcept = 0;
};

// A decoder for `config` (its backend, or with AUTO the first the system
// lists that takes the stream's codec and picture format on the GPU of
// config.device), not opened yet.
std::unique_ptr<VideoDecoder> createVideoDecoder(const DecoderConfig& config);

// ---- Pictures for the codec libraries built into the SDK (src/codecs/) ------

// Encoding: reads captured frames with a native surface (ScreenCapture, the
// host's GPU pictures) as YuvImages of a stream's layout and colour, for the
// duration of `use`: in place where the surface already is that (macOS
// capture), else converted on the GPU and read back. One per encoder (it
// keeps what its conversions need). Frames of CPU pixels never come here
// (src/codecs/ converts them). Throws Failure(CODEC) for a surface it cannot
// read.
class SurfaceReader {
public:
    virtual ~SurfaceReader() = default;
    virtual void read(const VideoFrame& frame, const Picture& picture,
                      const std::function<void(const YuvImage&)>& use) = 0;
};
std::unique_ptr<SurfaceReader> createSurfaceReader();

// Decoding: makes the pictures a codec library decoded frames of this system
// for on_gpu_frame: native surfaces of the picture's layout (on the
// DecoderConfig's device on Windows), their colour the picture's. Where the
// system has no surface for them, frames of CPU BGRA pixels
// (VideoFrame::pixels). One per decoder.
class SurfaceWriter {
public:
    virtual ~SurfaceWriter() = default;
    virtual VideoFrame write(const YuvImage& image, const Picture& picture) = 0;
};
std::unique_ptr<SurfaceWriter> createSurfaceWriter(const DecoderConfig& config);

// A stream's lower encodings (RCS_EXT_ENCODINGS): a captured frame's native
// surface made smaller for an encoder of another size, into a surface of the
// same layout and colour, on the GPU (macOS and iOS: VTPixelTransferSession;
// Windows: the video processor; Android: a draw on the GL thread). One per
// encoding (it keeps what its conversions need). Frames of CPU pixels never
// come here (the agent scales them). Throws Failure: UNSUPPORTED where this
// system has no surfaces to scale, CODEC when the GPU fails.
class SurfaceScaler {
public:
    virtual ~SurfaceScaler() = default;
    virtual VideoFrame scale(const VideoFrame& frame, uint32_t width, uint32_t height) = 0;
};
std::unique_ptr<SurfaceScaler> createSurfaceScaler();

// The GPU adapter a device belongs to (Windows: its LUID; else 0), to find a
// sharer's hardware encoder (HARDWARE_FIRST).
uint64_t deviceAdapter(const std::shared_ptr<void>& device) noexcept;

// ---- Frame access ----------------------------------------------------------

// Maps a native surface as BGRA for the duration of `use` (may read back from the GPU).
void withCpuImage(const VideoFrame& frame, const std::function<void(const rcs_frame&)>& use);
// Describes a native surface; false when the frame has none.
bool describeGpuFrame(const VideoFrame& frame, rcs_gpu_frame_info& info);
// The host's own GPU image (rcs_session_push_gpu_frame) as a frame, retained:
// surface, size, colour and time. Throws Failure: UNSUPPORTED where this system
// takes none of that kind (rcs_capabilities.pushed_gpu_frames), INVALID_ARGUMENT
// for an image that is not what `info` says.
VideoFrame hostGpuFrame(const rcs_gpu_frame_info& info);

// The images this system's hosts hand over as they come, of any size:
// Apple's CMSampleBuffers (rcs_session_push_sample_buffer), Android's
// AHardwareBuffers (rcs_session_push_hardware_buffer).
enum class HostImages { none, sampleBuffers, hardwareBuffers };
HostImages hostImages() noexcept;

// The pictures in hostImages(), one source's in turn: each turned upright and
// fitted into the stream's size on the GPU, the rest black.
class SamplePictures {
public:
    struct Picture {
        VideoFrame frame; // width x height.
        Rect content;     // Where the picture lies in it.
        DisplayMode size; // The picture upright.
    };
    virtual ~SamplePictures() = default;
    // Apple: a CMSampleBufferRef, `orientation` a CGImagePropertyOrientation
    // (0 from ReplayKit's attachment). Android: an AHardwareBuffer*,
    // `orientation` the degrees it is turned clockwise to be upright. Throws
    // Failure: INVALID_ARGUMENT without a picture, UNSUPPORTED for a mirrored
    // one, DEVICE when the GPU cannot.
    virtual Picture fit(const void* image, uint32_t orientation, uint32_t width, uint32_t height) = 0;
};
// Throws Failure(UNSUPPORTED) on systems without hostImages().
std::unique_ptr<SamplePictures> createSamplePictures();

// Apple: the sound in CMSampleBuffers, one source's in turn, as 48 kHz stereo
// float32 (resampled across buffers).
class SampleSound {
public:
    virtual ~SampleSound() = default;
    // Appends the buffer's sound to `samples`. Throws Failure:
    // INVALID_ARGUMENT for one without linear PCM.
    virtual void convert(const void* sampleBuffer, std::vector<float>& samples) = 0;
};
std::unique_ptr<SampleSound> createSampleSound();

// ---- Audio -------------------------------------------------------------------

// The sound this machine plays, as 48 kHz stereo float32. Capture runs on the
// platform's own thread and keeps itself going: when the sound cannot be
// captured (no output device, a device removed), it reports that through
// `status`, retries, and reports again once it captures.
struct AudioCaptureConfig {
    bool excludeCurrentProcess = false;
    uint32_t periodFrames = 0; // 0: the backend's default.
};
struct AudioStatus {
    bool active = false;
    rcs_result result = RCS_OK; // Not active: why.
    std::string message;
    bool voiceProcessing = false; // A microphone: whether the system cancels echo and suppresses noise.
};
class AudioCapture {
public:
    // Both run on the platform's threads and must not throw.
    using Deliver = std::function<void(std::span<const float> samples, uint64_t timestampNs)>;
    using Status = std::function<void(const AudioStatus& status)>;
    virtual ~AudioCapture() = default;
    // Returns at once; sound and status changes follow on capture threads,
    // never after stop() returned.
    virtual void start(const AudioCaptureConfig& config, Deliver deliver, Status status) = 0;
    virtual void stop() noexcept = 0;
    // The frames delivered at a time while capturing; 0 before.
    virtual uint32_t periodFrames() const noexcept = 0;
};
// One of audioCaptureBackends(); AUTO is resolved by the caller.
std::unique_ptr<AudioCapture> createAudioCapture(rcs_audio_capture_backend backend);
// rcs_capabilities.audio_capture_backends and audio_exclusion.
uint32_t audioCaptureBackends() noexcept;
bool audioExclusionAvailable() noexcept;
// What RCS_AUDIO_CAPTURE_AUTO selects here (0 without any).
rcs_audio_capture_backend defaultAudioCaptureBackend() noexcept;
// Whether `backend` takes AudioCaptureConfig::periodFrames.
bool audioCapturePeriodSelectable(rcs_audio_capture_backend backend) noexcept;

// The machine's sound devices, inputs and outputs (rcs_list_audio_devices).
struct AudioDevice {
    uint32_t id = 0; // Stays while the device stays connected; never 0.
    bool input = false;
    bool isDefault = false;
    std::string name;
};
std::vector<AudioDevice> audioDevices();
// Whether this process may record from microphones (macOS: TCC, the user's
// answer to the system's prompt; Windows: the privacy setting for desktop
// apps), and asking for it (macOS shows the prompt; elsewhere the answer as it is).
rcs_permission microphonePermission() noexcept;
rcs_result requestMicrophonePermission();
// A microphone: `device` from audioDevices() (0: the system's default input,
// followed when it changes), 48 kHz stereo float32 as AudioCapture delivers
// it. `voiceProcessing` asks the system to cancel echo of what this machine
// plays and suppress noise (macOS: voice processing I/O; Windows: the
// communications stream, where the device's driver offers it); AudioStatus
// says when it does not.
struct MicrophoneConfig {
    uint32_t device = 0;
    bool voiceProcessing = true;
};
std::unique_ptr<AudioCapture> createMicrophoneCapture(const MicrophoneConfig& config);

// Plays 48 kHz stereo float32 on the system's default output device and
// follows it when it changes. `fill` runs on the device's real-time thread.
// A setting the device cannot do is reported through `status` (inactive,
// UNSUPPORTED or DEVICE with the reason), never replaced by another.
struct PlaybackConfig {
    uint32_t periodFrames = 0; // 0: the platform's default.
    uint32_t queueFrames = 0;  // Windows: 0 for one and a half periods.
};
class AudioPlayer {
public:
    // Both run on the platform's threads and must not throw.
    using Fill = std::function<void(float* samples, uint32_t frames)>;
    using Status = std::function<void(const AudioStatus& status)>;
    virtual ~AudioPlayer() = default;
    virtual void start(const PlaybackConfig& config, Fill fill, Status status) = 0;
    virtual void stop() noexcept = 0;
    struct Device {
        std::string name; // Empty while none plays.
        uint32_t periodFrames = 0, queueFrames = 0;
        uint64_t underruns = 0; // Since start: the device went without sound because `fill` came too late.
    };
    virtual Device device() const = 0;
};
std::unique_ptr<AudioPlayer> createAudioPlayer();

// AAC-LC from the system's codecs. Throws Failure(CODEC) when they cannot
// open, Failure(UNSUPPORTED) where the system has none (Windows without Media
// Foundation) or for a bitrate or a configuration (aacConfig) they do not take.
std::unique_ptr<audio::Encoder> createAacEncoder(uint32_t bitrate);
std::unique_ptr<audio::Decoder> createAacDecoder(std::span<const uint8_t> config);
// Bitrates the AAC encoder takes (empty: any in 64..320 kbit/s).
std::vector<uint32_t> aacBitrates();

// ---- The application (iOS) ------------------------------------------------------

// rcs_set_audio_session_config / rcs_get_audio_session_config: the
// application's audio session, set up by the SDK's coordinator or by the host.
// Failure(UNSUPPORTED) on systems without one.
void setAudioSessionConfig(const rcs_audio_session_config& config);
rcs_audio_session_config audioSessionConfig();

// Starts telling `changed` whether the application is in the background (iOS:
// no scene of it in front, where it may not use the GPU), at each change, on
// a thread of the system's, for the rest of the process (seams::watchAppState
// calls it once). Systems without that state never call it.
void observeAppState(std::function<void(bool background)> changed);

// ---- Java (Android) ------------------------------------------------------------

// rcs_android_initialize: keeps `javaVm` and the application context of
// `context`. Throws Failure: UNSUPPORTED on systems without Java,
// INVALID_ARGUMENT for a context that is no Context.
void setJava(void* javaVm, void* context);
// rcs_android_get_java: the JavaVM* and the application context (a global
// reference). Throws Failure: UNSUPPORTED on systems without Java,
// INVALID_STATE before setJava.
std::pair<void*, void*> java();

// ---- The network ---------------------------------------------------------------

// While a reference is held: this system's Wi-Fi out of its power saving
// (rcs_network_tuning_config.wifi_low_latency), for every session of the
// process. Android: a low-latency Wi-Fi lock; Windows: media streaming mode
// and no background scans on each connected Wi-Fi interface. Null where no
// Wi-Fi is connected (Windows); Failure saying why not where the system cannot
// (UNSUPPORTED: Apple's, or no Java VM; PERMISSION_REQUIRED: no WAKE_LOCK).
std::shared_ptr<void> lowLatencyWifi();

// This machine's networks that are up with a usable address
// (transport::interfaces), the default first, with what the system says of
// each (rcs_network_info): Apple's from Network.framework, Windows' from
// iphlpapi and the network list manager, Android's from ConnectivityManager
// (with the Java VM); elsewhere nothing (UNKNOWN), but which one the system's
// route to the internet takes (DEFAULT). Throws Failure: NETWORK when the
// list cannot be read, PERMISSION_REQUIRED where the system wants one.
struct Network {
    std::string name;
    uint32_t index = 0;
    rcs_network_kind kind = RCS_NETWORK_UNKNOWN;
    uint32_t flags = 0; // RCS_NETWORK_FLAG_* bits.
    bool ipv4 = false, ipv6 = false;
};
std::vector<Network> networks();
// What networks() can say here, as RCS_NETWORK_FACT_* bits (rcs_capabilities.network_facts).
uint32_t networkFacts() noexcept;
// This machine's Wi-Fi networks that it is connected to, with what the
// system says of each (rcs_wifi_info; other_channel is the caller's): macOS
// from CoreWLAN, iOS only AWDL's state, Windows from the WLAN API, Android
// from WifiManager (with the Java VM); elsewhere none. What wifiNetworks()
// can say here, as RCS_WIFI_FACT_* bits (rcs_capabilities.wifi_facts).
std::vector<rcs_wifi_info> wifiNetworks();
uint32_t wifiFacts() noexcept;
// While a reference is held, the system's changes of its networks (an
// interface or an address that came or went, another default network, a VPN
// up or down) move transport::networkChanged() on: one watcher for every
// session of the process. Apple: nw_path_monitor; Windows: iphlpapi's
// notifications; Android and elsewhere: a look at the interfaces every half
// second (and, with the Java VM, at the default network).
std::shared_ptr<void> watchNetworks();

// ---- Input injection -------------------------------------------------------

class InputInjector {
public:
    virtual ~InputInjector() = default;
    virtual rcs_result apply(const rcs_input_event& event) = 0;
    // Releases every key and button this injector pressed.
    virtual void releaseAll() noexcept = 0;
};

// How an injector executes input, where systems differ.
struct InputOptions {
    bool secureDesktop = false; // Windows: inject into the desktop that receives input, secure ones too.
    rcs_caps_lock capsLock = RCS_CAPS_LOCK_INPUT_SOURCE; // macOS (rcs_control_config.caps_lock).
    uint32_t capsLockHoldMs = 500;
    bool operator==(const InputOptions&) const = default;
};

// Pointer positions of stream 0 map across what `target` shows (its display,
// its region, the window where it is now); with WINDOW and APPLICATION, input
// stays inside it (see rcs_capture_config). Those of stream N map across the
// whole display streamDisplays[N - 1] (rcs_streams_config); another stream's
// are INVALID_ARGUMENT. One injector serves them all, so a button pressed on
// one display is still held on the next.
std::unique_ptr<InputInjector> createInputInjector(const CaptureTarget& target,
                                                   const std::vector<uint32_t>& streamDisplays = {},
                                                   const InputOptions& options = {});

// The pixel, of `pixels` across, that an input position names (0 the first,
// 65535 the last), rounded as viewers round it (rcsdk::ViewGeometry): a pixel
// a viewer points at comes back as itself.
inline long pixelAt(uint32_t position, long pixels) noexcept {
    return std::lround(position / 65535.0 * std::max(pixels - 1, 0L));
}

// ---- Where what is shared shows ------------------------------------------------

// Where `target` shows on this machine's screen now, as remote input maps
// across it (rcs_module_get_shared): its edges in the system's own screen
// coordinates (Windows: physical pixels of the virtual screen; macOS: global
// points), the right and bottom ones just outside, and the process whose
// window or application it is (WINDOW, APPLICATION; 0 otherwise). None while
// it does not show: a window minimized or gone, a region off its display, a
// display gone.
struct SharedArea {
    double left = 0, top = 0, right = 0, bottom = 0;
    uint32_t processId = 0;
};
std::optional<SharedArea> sharedArea(const CaptureTarget& target);

// ---- The machine's desktop and the secure attention sequence ------------------

struct DesktopState {
    rcs_desktop_kind kind = RCS_DESKTOP_UNKNOWN;
    bool userLoggedOn = false;
    bool operator==(const DesktopState&) const = default;
};
// What the machine shows now (Windows and macOS; unknown elsewhere). Needs no privilege.
DesktopState desktopState();
// rcs_capabilities.secure_desktop: this process can capture and control secure
// desktops (Windows: LocalSystem in a user session).
bool secureDesktopAvailable() noexcept;
// rcs_capabilities.secure_attention, and sending Ctrl+Alt+Del to the session of
// process `forProcess` (0: this process's). The result is what
// RCS_INPUT_SECURE_ATTENTION documents; a refusal is logged with its reason.
bool secureAttentionAvailable() noexcept;
rcs_result secureAttention(uint32_t forProcess);

// ---- A supervisor's helper -----------------------------------------------------------
// (The supervisor itself is RemoteControl::Supervisor's, beside the core.)

// A wait's limit; none waits without end.
using WaitLimit = std::optional<std::chrono::milliseconds>;
// The public interface's timeout_ms: UINT32_MAX is no limit.
inline WaitLimit waitLimit(uint32_t timeoutMs) noexcept {
    return timeoutMs == UINT32_MAX ? WaitLimit() : WaitLimit(std::chrono::milliseconds(timeoutMs));
}
// As Windows' waits take a limit: INFINITE (0xFFFFFFFF) for none, else its
// milliseconds, at most 0xFFFFFFFE (one more would be INFINITE).
inline uint32_t windowsTimeout(WaitLimit limit) noexcept {
    if (!limit) {
        return 0xFFFFFFFFu;
    }
    return uint32_t(std::clamp<int64_t>(limit->count(), 0, 0xFFFFFFFE));
}

// In a helper the supervisor started: waits for its request to end.
// UNSUPPORTED in any other process.
rcs_result waitForSupervisorStop(WaitLimit limit) noexcept;
// In a helper the supervisor started: the key it was handed. Throws
// Failure(UNSUPPORTED) in any other process, or when it was handed none.
std::array<uint8_t, 32> helperKey();

} // namespace rcsdk::platform
