// H.264, HEVC, VP9 and AV1 decoding with VideoToolbox, on the hardware
// decoder or on the CPU as configured (VP9 and AV1: hardware only; AV1 from
// the M3 on), into IOSurface
// pixel buffers either way: in the stream's own YCbCr layout (420v / 420f,
// x420 / xf20, 444v / 444f, x444 / xf44: VideoToolbox's default for 10-bit
// streams is a private packed format), or BGRA when the host reads frames on
// the CPU. Converting to BGRA costs 3-5 ms per 4K frame, a third of a 120 FPS
// frame interval. The frames' colour is the stream's, as its packets say.
#include "media/av1.hpp"
#include "media/nal.hpp"
#include "platform/apple/cf.hpp"
#include "platform/apple/codecs.hpp"
#include "platform/platform.hpp"

#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>

namespace rcsdk::platform {
namespace {

using apple::CFRef;

class VideoToolboxDecoder final : public VideoDecoder {
  public:
    ~VideoToolboxDecoder() override { close(); }

    void open(const DecoderConfig& config) override {
        apple::codecType(config.codec); // Rejects codecs this backend does not implement.
        if (config.acceleration == RCS_ACCELERATION_HARDWARE && !apple::hardwareDecodes(config.codec)) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("This device has no hardware ") + codecName(config.codec) +
                                            " decoder");
        }
        if (config.acceleration == RCS_ACCELERATION_SOFTWARE && config.codec != RCS_CODEC_H264 &&
            config.codec != RCS_CODEC_HEVC) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("VideoToolbox decodes ") + codecName(config.codec) +
                                            " in hardware only");
        }
        config_ = config;
    }

    bool decode(const EncodedPacket& packet, VideoFrame& frame, CancelToken) override {
        if (!session_ || packet.config != activeConfig_ || packet.picture != activePicture_) {
            create(packet);
        }
        CFRef<CMBlockBufferRef> block;
        apple::check(CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, packet.data.size(), nullptr, nullptr, 0,
                                                      packet.data.size(), 0, block.out()),
                   "Compressed buffer allocation", RCS_ERROR_CODEC);
        apple::check(CMBlockBufferReplaceDataBytes(packet.data.data(), block.value, 0, packet.data.size()),
                   "Compressed buffer copy", RCS_ERROR_CODEC);
        const size_t size = packet.data.size();
        CFRef<CMSampleBufferRef> sample;
        apple::check(
            CMSampleBufferCreateReady(nullptr, block.value, format_.value, 1, 0, nullptr, 1, &size, sample.out()),
            "Compressed sample creation", RCS_ERROR_CODEC);
        auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample.value, true);
        auto metadata = static_cast<CFMutableDictionaryRef>(const_cast<void*>(CFArrayGetValueAtIndex(attachments, 0)));
        CFDictionarySetValue(metadata, kCMSampleAttachmentKey_NotSync,
                             packet.keyframe ? kCFBooleanFalse : kCFBooleanTrue);

        __block OSStatus outputStatus = noErr;
        __block CVImageBufferRef output = nullptr;
        const OSStatus status = VTDecompressionSessionDecodeFrameWithOutputHandler(
            session_.value, sample.value, 0, nullptr,
            ^(OSStatus result, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime) {
                outputStatus = result;
                if (image) {
                    output = CVPixelBufferRetain(image);
                }
            });
        apple::check(status, "Decode", RCS_ERROR_CODEC);
        if (!output && outputStatus == noErr) {
            VTDecompressionSessionWaitForAsynchronousFrames(session_.value);
        }
        CFRef<CVImageBufferRef> image(output);
        apple::check(outputStatus, "Decoder output", RCS_ERROR_CODEC);
        if (!image) {
            return false;
        }
        frame.surface = apple::retainSurface(image.value);
        frame.width = static_cast<uint32_t>(CVPixelBufferGetWidth(image.value));
        frame.height = static_cast<uint32_t>(CVPixelBufferGetHeight(image.value));
        frame.color = colorOf(packet.picture);
        return true;
    }

    std::string name() const override {
        return apple::decoderName(config_.codec, config_.acceleration == RCS_ACCELERATION_HARDWARE);
    }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_VIDEOTOOLBOX; }

    // Without frame reordering nothing is buffered, and the next keyframe
    // restarts prediction. Keeping the session matters: recreating it takes
    // about 45 ms, several 120 FPS frame intervals.
    void reset() override {
        if (session_) {
            VTDecompressionSessionWaitForAsynchronousFrames(session_.value);
        }
    }

  private:
    void create(const EncodedPacket& packet) {
        close();
        describe(packet);
        const bool hardware = config_.acceleration == RCS_ACCELERATION_HARDWARE;
        auto specification = apple::dictionary();
        CFDictionarySetValue(specification.value, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
                             hardware ? kCFBooleanTrue : kCFBooleanFalse);
        if (hardware) {
            CFDictionarySetValue(specification.value,
                                 kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
        }
        const OSType output = config_.cpuImages ? kCVPixelFormatType_32BGRA : apple::pixelFormatOf(packet.picture);
        auto attributes = apple::surfaceAttributes(0, 0, output);
        const std::string creating = std::string("Creating a ") + codecName(config_.codec) + " decoder for " +
                                     std::to_string(packet.width) + "x" + std::to_string(packet.height) + " pictures";
        apple::check(VTDecompressionSessionCreate(nullptr, format_.value, specification.value, attributes.value, nullptr,
                                                session_.out()),
                   creating.c_str(), RCS_ERROR_CODEC);
        VTSessionSetProperty(session_.value, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
        activeConfig_ = packet.config;
        activePicture_ = packet.picture;
    }

    void describe(const EncodedPacket& packet) {
        if (config_.codec == RCS_CODEC_AV1 || config_.codec == RCS_CODEC_VP9) {
            // The configuration travels as in an 'av01' or 'vp09' sample
            // entry: an AV1CodecConfigurationRecord made from the sequence
            // header, or the 'vpcC' record as it is; samples hold the frame
            // OBUs, or the VP9 frame or superframe. The stream's colour goes
            // with it: VideoToolbox does not take it from the bitstream.
            const bool av1 = config_.codec == RCS_CODEC_AV1;
            const auto record = av1 ? av1::codecConfiguration(packet.config) : packet.config;
            CFRef<CFDataRef> box(CFDataCreate(nullptr, record.data(), CFIndex(record.size())));
            auto atoms = apple::dictionary();
            CFDictionarySetValue(atoms.value, av1 ? CFSTR("av1C") : CFSTR("vpcC"), box.value);
            auto extensions = apple::dictionary();
            CFDictionarySetValue(extensions.value, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms,
                                 atoms.value);
            const auto colour = apple::colorKeysOf(packet.picture);
            CFDictionarySetValue(extensions.value, kCMFormatDescriptionExtension_ColorPrimaries, colour.primaries);
            CFDictionarySetValue(extensions.value, kCMFormatDescriptionExtension_TransferFunction, colour.transfer);
            CFDictionarySetValue(extensions.value, kCMFormatDescriptionExtension_YCbCrMatrix, colour.matrix);
            CFDictionarySetValue(extensions.value, kCMFormatDescriptionExtension_FullRangeVideo,
                                 packet.picture.range == RCS_RANGE_FULL ? kCFBooleanTrue : kCFBooleanFalse);
            apple::check(CMVideoFormatDescriptionCreate(nullptr, apple::codecType(config_.codec),
                                                      int32_t(packet.width), int32_t(packet.height), extensions.value,
                                                      format_.out()),
                       av1 ? "AV1 format description" : "VP9 format description", RCS_ERROR_CODEC);
            return;
        }
        std::vector<const uint8_t*> sets;
        std::vector<size_t> sizes;
        for (const auto unit : nal::units(packet.config)) {
            sets.push_back(unit.data());
            sizes.push_back(unit.size());
        }
        const OSStatus described = config_.codec == RCS_CODEC_HEVC
                                       ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                                             nullptr, sets.size(), sets.data(), sizes.data(), 4, nullptr, format_.out())
                                       : CMVideoFormatDescriptionCreateFromH264ParameterSets(
                                             nullptr, sets.size(), sets.data(), sizes.data(), 4, format_.out());
        apple::check(described, "Video format description", RCS_ERROR_CODEC);
    }

    void close() noexcept {
        if (session_) {
            VTDecompressionSessionInvalidate(session_.value);
            session_.reset();
        }
        format_.reset();
        activeConfig_.clear();
    }

    DecoderConfig config_;
    CFRef<VTDecompressionSessionRef> session_;
    CFRef<CMVideoFormatDescriptionRef> format_;
    std::vector<uint8_t> activeConfig_;
    Picture activePicture_;
};

} // namespace

std::unique_ptr<VideoDecoder> createVideoDecoder(const DecoderConfig&) {
    return std::make_unique<VideoToolboxDecoder>();
}

} // namespace rcsdk::platform
