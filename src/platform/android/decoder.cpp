// H.264, HEVC, VP8, VP9 and AV1 decoding with MediaCodec, on the device's hardware
// decoder or Android's software one as configured, into an AImageReader: the
// decoder draws each picture into one of the reader's hardware buffers, which
// the frame holds (the reader's image) until the host lets go of it. GPU
// frames keep the decoder's own layout (AIMAGE_FORMAT_PRIVATE, sampled as an
// external texture), whatever the bit depth and chroma; hosts that read
// frames on the CPU get 4:2:0 8-bit pictures in YUV_420_888 buffers (which
// Qualcomm's decoders still lay out compressed, for the GPU only), and the
// others in the decoder's own layout: frames.cpp reads them as BGRA8.
// The stream's parameter sets go to the decoder at configure time (csd-0,
// csd-1), so a stream that changes them gets a new decoder, as on the other
// systems; VP8 and VP9 have none (their frames say all).
#include "media/av1.hpp"
#include "media/nal.hpp"
#include "platform/android/codecs.hpp"
#include "platform/decode_probes.hpp"
#include "util/log.hpp"

#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

namespace rcsdk::platform {
namespace {

using namespace std::chrono_literals;
using android::check;

// The reader the decoder draws into; its images keep it while the host holds them.
struct Reader {
    AImageReader* reader = nullptr;
    std::mutex access; // Acquiring and deleting images.
    std::mutex signal;
    std::condition_variable arrived;
    uint64_t images = 0; // Images that arrived.
    ~Reader() {
        if (reader) {
            AImageReader_delete(reader);
        }
    }
};

// A decoded picture: the reader's image, given back when the frame goes.
struct Held {
    std::shared_ptr<Reader> reader;
    AImage* image = nullptr;
    ~Held() {
        std::lock_guard lock(reader->access);
        AImage_delete(image);
    }
};

// Images the host may hold at once besides the one on its way.
constexpr int32_t readerImages = 8;

// The layout a picture of `format` has in a GPU frame (rcs_gpu_frame_info.format).
rcs_pixel_format layoutOf(rcs_picture_format format) noexcept {
    const bool wide = bitDepth(format) > 8;
    if (chroma444(format)) {
        return wide ? RCS_PIXEL_P410 : RCS_PIXEL_NV24;
    }
    return wide ? RCS_PIXEL_P010 : RCS_PIXEL_NV12;
}

// Whether a decoder's buffer of `bufferFormat` keeps what pictures of
// `format` hold: of the formats Android names, the YCbCr ones are 4:2:0 and
// all but P010 are 8-bit (a hardware decoder's own formats it does not name).
bool keeps(uint32_t bufferFormat, rcs_picture_format format) noexcept {
    constexpr uint32_t yv12 = 0x32315659, nv21 = 0x11; // HAL_PIXEL_FORMAT_YV12, _YCrCb_420_SP.
    const bool yuv420 = bufferFormat == AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 || bufferFormat == yv12 ||
                        bufferFormat == nv21;
    const bool narrow = yuv420 || bufferFormat == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    const bool subsampled = yuv420 || bufferFormat == AHARDWAREBUFFER_FORMAT_YCbCr_P010;
    return !(bitDepth(format) > 8 && narrow) && !(chroma444(format) && subsampled);
}

class MediaCodecDecoder final : public VideoDecoder {
public:
    MediaCodecDecoder() = default;
    // The decoder MediaCodec names `name`, for GPU frames.
    MediaCodecDecoder(std::string name, rcs_codec codec) : name_(std::move(name)) { config_.codec = codec; }
    ~MediaCodecDecoder() override { close(); }

    void open(const DecoderConfig& config) override {
        const auto& entry =
            android::implementation(config.codec, RCS_CODEC_DECODER, config.acceleration, RCS_BACKEND_AUTO);
        name_ = entry.name;
        config_ = config;
    }

    // Whether the keyframe `packet` alone (the stream ends after it) becomes
    // a picture whose buffer keeps the packet's picture format, and which
    // shows the probe's grey (the emulator's GPU draws 10-bit buffers black).
    bool probe(const EncodedPacket& packet) {
        static const std::atomic<bool> never{false};
        create(packet);
        queue(packet, never);
        const ssize_t index = AMediaCodec_dequeueInputBuffer(codec_.get(), 100'000);
        if (index < 0) {
            return false;
        }
        check(AMediaCodec_queueInputBuffer(codec_.get(), size_t(index), 0, 0, ++time_,
                                           AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM),
              "Ending the stream");
        VideoFrame frame;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!output(packet, frame, never, 150ms)) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
        }
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(static_cast<AHardwareBuffer*>(frame.surface.get()), &desc);
        if (!keeps(desc.format, packet.picture.format)) {
            return false;
        }
        bool grey = false;
        withCpuImage(frame, [&](const rcs_frame& image) {
            const uint8_t* centre = image.data + size_t(image.height / 2) * image.stride_bytes + image.width / 2 * 4;
            grey = std::abs(centre[0] - 128) <= 16 && std::abs(centre[1] - 128) <= 16 && std::abs(centre[2] - 128) <= 16;
        });
        return grey;
    }

    bool decode(const EncodedPacket& packet, VideoFrame& frame, CancelToken cancelled) override {
        if (!codec_ || packet.config != activeConfig_ || packet.width != width_ || packet.height != height_) {
            create(packet);
        }
        if (!queue(packet, cancelled)) {
            return false;
        }
        // A decoder takes a few milliseconds a picture, its first longer
        // while it allocates its buffers (Qualcomm's: some 60 ms for an 8K
        // keyframe): that wait once for each decoder created, not for every
        // packet until a picture came, nor after a flush. A decoder that
        // holds pictures until more packets come (Android's software H.264
        // decoder holds two) took it for each of those, and a short receive
        // queue overflowed meanwhile, again at each keyframe that flushed it:
        // no picture ever. One it keeps longer comes with a later packet.
        return output(packet, frame, cancelled, std::exchange(wait_, 30ms));
    }

    // Without frame reordering nothing is buffered but what flush() drops; the
    // parameter sets given at configure time go to the decoder again.
    void reset() override {
        if (codec_) {
            check(AMediaCodec_flush(codec_.get()), "Flushing the decoder");
            rendered_ = 0;
            std::lock_guard lock(reader_->signal);
            reader_->images = 0;
        }
    }

    std::string name() const override { return name_; }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_MEDIACODEC; }

private:
    void create(const EncodedPacket& packet) {
        close();
        const std::string creating = "Creating a " + name_ + " decoder for " + std::to_string(packet.width) + "x" +
                                     std::to_string(packet.height) + " pictures";
        auto reader = std::make_shared<Reader>();
        const bool cpu = config_.cpuImages && packet.picture.format == RCS_PICTURE_420_8; // YUV_420_888 is that.
        check(AImageReader_newWithUsage(int32_t(packet.width), int32_t(packet.height),
                                        cpu ? AIMAGE_FORMAT_YUV_420_888 : AIMAGE_FORMAT_PRIVATE,
                                        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                            (cpu ? AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN : 0),
                                        readerImages + 2, &reader->reader),
              (creating + ": its image reader").c_str());
        AImageReader_ImageListener listener{reader.get(), [](void* context, AImageReader*) {
                                                auto* self = static_cast<Reader*>(context);
                                                std::lock_guard lock(self->signal);
                                                ++self->images;
                                                self->arrived.notify_all();
                                            }};
        check(AImageReader_setImageListener(reader->reader, &listener), creating.c_str());
        ANativeWindow* window = nullptr;
        check(AImageReader_getWindow(reader->reader, &window), creating.c_str());

        android::Format format(AMediaFormat_new());
        AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, android::mimeOf(config_.codec));
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, int32_t(packet.width));
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, int32_t(packet.height));
        AMediaFormat_setInt32(format.get(), "priority", 0);    // Real time.
        AMediaFormat_setInt32(format.get(), "low-latency", 1); // Android 11: a picture per packet, at once.
        if (uint64_t(packet.width) * packet.height <= 3840 * 2160) {
            // As fast as the hardware goes, not paced to the stream: 1080p
            // in under 2 ms on the S25+, not 4-6. Larger pictures already
            // take all of it (Qualcomm's decoder then drops 8K pictures).
            AMediaFormat_setInt32(format.get(), "operating-rate", INT16_MAX);
        }
        android::setColor(format.get(), packet.picture);
        setParameterSets(format.get(), packet);
        codec_ = android::open(name_);
        check(AMediaCodec_configure(codec_.get(), format.get(), window, nullptr, 0), creating.c_str());
        check(AMediaCodec_start(codec_.get()), creating.c_str());
        reader_ = std::move(reader);
        activeConfig_ = packet.config;
        width_ = packet.width;
        height_ = packet.height;
        rendered_ = 0;
        wait_ = 150ms;
    }

    // H.264: SPS and PPS as csd-0 and csd-1; HEVC: VPS, SPS and PPS as
    // csd-0; Annex B either way. AV1: the AV1CodecConfigurationRecord.
    void setParameterSets(AMediaFormat* format, const EncodedPacket& packet) {
        if (vpCodec(config_.codec)) {
            return;
        }
        if (config_.codec == RCS_CODEC_AV1) {
            const auto record = av1::codecConfiguration(packet.config);
            AMediaFormat_setBuffer(format, "csd-0", record.data(), record.size());
            return;
        }
        std::vector<uint8_t> first, second;
        for (const auto unit : nal::units(packet.config)) {
            const bool pps = config_.codec == RCS_CODEC_H264 && nal::h264::type(unit) == 8;
            nal::appendAnnexB(pps ? second : first, unit);
        }
        AMediaFormat_setBuffer(format, "csd-0", first.data(), first.size());
        if (!second.empty()) {
            AMediaFormat_setBuffer(format, "csd-1", second.data(), second.size());
        }
    }

    // Hands the packet to the decoder; false when cancelled while it had no room.
    bool queue(const EncodedPacket& packet, CancelToken cancelled) {
        ssize_t index = -1;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while ((index = AMediaCodec_dequeueInputBuffer(codec_.get(), 5'000)) < 0) {
            failed(index);
            if (cancelled) {
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                fail(RCS_ERROR_CODEC, name_ + " takes no packets (it waits for its pictures to be let go: release "
                                              "GPU frames once shown)");
            }
            drain(); // Room for its pictures frees its packets.
        }
        size_t capacity = 0;
        uint8_t* buffer = AMediaCodec_getInputBuffer(codec_.get(), size_t(index), &capacity);
        bytes_.clear();
        if (config_.codec == RCS_CODEC_AV1) {
            // The sequence header ahead of a keyframe's OBUs too: Android's
            // software decoder (libgav1) takes it only from there.
            if (packet.keyframe) {
                bytes_ = packet.config;
            }
            bytes_.insert(bytes_.end(), packet.data.begin(), packet.data.end());
        } else if (vpCodec(config_.codec)) {
            bytes_ = packet.data; // A VP8 frame, a VP9 frame or superframe.
        } else {
            for (const auto unit : nal::units(packet.data)) {
                nal::appendAnnexB(bytes_, unit);
            }
        }
        if (!buffer || bytes_.size() > capacity) {
            fail(RCS_ERROR_CODEC, name_ + " has no room for a packet of " + std::to_string(bytes_.size()) + " bytes");
        }
        std::memcpy(buffer, bytes_.data(), bytes_.size());
        check(AMediaCodec_queueInputBuffer(codec_.get(), size_t(index), 0, bytes_.size(), ++time_, 0),
              "Decoding a packet");
        return true;
    }

    // Draws every picture the decoder has into the reader; waits up to `wait` for the first.
    void drain(std::chrono::microseconds wait = 0us) {
        for (;;) {
            AMediaCodecBufferInfo info{};
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_.get(), &info, wait.count());
            if (index >= 0) {
                check(AMediaCodec_releaseOutputBuffer(codec_.get(), size_t(index), true), "Showing a picture");
                ++rendered_;
                wait = 0us;
            } else if (index != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED &&
                       index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                failed(index);
                return;
            }
        }
    }

    // Throws when a dequeue's `index` is the codec's failure, not "none yet":
    // a codec that failed (a stream it cannot decode) takes and gives nothing more.
    void failed(ssize_t index) const {
        if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            fail(RCS_ERROR_CODEC, name_ + " failed: " + android::statusText(media_status_t(index)));
        }
    }

    // The latest picture, waiting up to `wait` for the decoder's next one.
    bool output(const EncodedPacket& packet, VideoFrame& frame, CancelToken cancelled, std::chrono::microseconds wait) {
        drain(wait);
        if (!rendered_) {
            return false;
        }
        {
            std::unique_lock lock(reader_->signal);
            reader_->arrived.wait_for(lock, 100ms, [&] { return reader_->images >= rendered_ || cancelled; });
            if (!reader_->images) {
                return false;
            }
        }
        AImage* image = nullptr;
        media_status_t status;
        {
            std::lock_guard lock(reader_->access);
            status = AImageReader_acquireLatestImage(reader_->reader, &image);
        }
        if (status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED) {
            if (!warnedHeld_) {
                warnedHeld_ = true;
                log::warning("decoder", "The host holds " + std::to_string(readerImages) +
                                            " decoded pictures: pictures wait until it releases GPU frames");
            }
            return false;
        }
        if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            return false;
        }
        check(status, "Taking the decoded picture");
        auto held = std::make_shared<Held>();
        held->reader = reader_;
        held->image = image;
        AHardwareBuffer* buffer = nullptr;
        check(AImage_getHardwareBuffer(image, &buffer), "Taking the decoded picture's buffer");
        // The stream's size is what its packets say; the picture is never read beyond what was decoded.
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(buffer, &desc);
        if (desc.width < packet.width || desc.height < packet.height) {
            fail(RCS_ERROR_CODEC, "The stream's pictures are " + std::to_string(packet.width) + "x" +
                                      std::to_string(packet.height) + ", but they decode to " +
                                      std::to_string(desc.width) + "x" + std::to_string(desc.height));
        }
        frame.surface = std::shared_ptr<void>(held, buffer);
        frame.layout = layoutOf(packet.picture.format);
        frame.width = packet.width;
        frame.height = packet.height;
        frame.color = colorOf(packet.picture);
        return true;
    }

    void close() noexcept {
        if (codec_) {
            AMediaCodec_stop(codec_.get());
            codec_.reset();
        }
        reader_.reset(); // Deleted once the host let go of its pictures.
        activeConfig_.clear();
    }

    DecoderConfig config_;
    std::string name_;
    android::Codec codec_;
    std::shared_ptr<Reader> reader_;
    std::vector<uint8_t> activeConfig_;
    uint32_t width_ = 0, height_ = 0;
    std::vector<uint8_t> bytes_;
    int64_t time_ = 0;                 // The packets' presentation times, in microseconds.
    uint64_t rendered_ = 0;            // Pictures drawn into the reader.
    std::chrono::microseconds wait_{}; // For the next packet's picture (decode()).
    bool warnedHeld_ = false;
};

} // namespace

std::unique_ptr<VideoDecoder> createVideoDecoder(const DecoderConfig&) {
    return std::make_unique<MediaCodecDecoder>();
}

bool android::decodes(const std::string& name, rcs_codec codec, rcs_picture_format format,
                      std::span<const uint8_t> config, std::span<const uint8_t> data) {
    try {
        EncodedPacket packet;
        packet.codec = codec;
        packet.picture.format = format;
        packet.width = probes::width;
        packet.height = probes::height;
        packet.keyframe = true;
        packet.config.assign(config.begin(), config.end());
        packet.data.assign(data.begin(), data.end());
        return MediaCodecDecoder(name, codec).probe(packet);
    } catch (const Failure&) {
        return false;
    }
}

} // namespace rcsdk::platform
