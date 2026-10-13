// Core Media sample buffers as the host's pictures and sound
// (rcs_session_push_sample_buffer). A picture is turned upright
// (VTPixelRotationSession) and fitted into the stream, its shape kept and the
// rest black (VTPixelTransferSession), on the GPU into IOSurface-backed
// buffers of its own format; sound is converted to 48 kHz stereo float32 by
// an AudioConverter that keeps its state across buffers, so resampling runs on
// without seams.
#include "platform/platform.hpp"

#include "media/audio.hpp"
#include "media/picture_size.hpp"
#include "platform/apple/cf.hpp"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace rcsdk::platform {
namespace {

// What turns a picture lying as CGImagePropertyOrientation `orientation`
// upright; null for a mirrored one.
CFStringRef uprightRotation(uint32_t orientation) {
    switch (orientation) {
    case 1:
        return kVTRotation_0;
    case 3:
        return kVTRotation_180;
    case 6:
        return kVTRotation_CW90;
    case 8:
        return kVTRotation_CCW90;
    default:
        return nullptr;
    }
}

// How ReplayKit says a picture lies; upright without it.
uint32_t attachedOrientation(CMSampleBufferRef buffer) {
    CFTypeRef value = CMGetAttachment(buffer, CFSTR("RPVideoSampleOrientationKey"), nullptr);
    int32_t orientation = 1;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt32Type, &orientation);
    }
    return uint32_t(orientation);
}

// A YCbCr picture's matrix as its buffer says (BT.709 without one).
rcs_color_matrix matrixOf(CVPixelBufferRef buffer) {
    apple::CFRef<CFTypeRef> matrix(CVBufferCopyAttachment(buffer, kCVImageBufferYCbCrMatrixKey, nullptr));
    if (matrix && CFEqual(matrix.value, kCVImageBufferYCbCrMatrix_ITU_R_601_4)) {
        return RCS_MATRIX_BT601;
    }
    if (matrix && CFEqual(matrix.value, kCVImageBufferYCbCrMatrix_ITU_R_2020)) {
        return RCS_MATRIX_BT2020;
    }
    return RCS_MATRIX_BT709;
}

bool fullRange(OSType format) {
    return format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
           format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange ||
           format == kCVPixelFormatType_444YpCbCr8BiPlanarFullRange ||
           format == kCVPixelFormatType_444YpCbCr10BiPlanarFullRange;
}

// IOSurface-backed buffers of one size and format, made again when those change.
class Buffers {
public:
    // Retained.
    CVPixelBufferRef take(uint32_t width, uint32_t height, OSType format) {
        if (!pool_ || width != width_ || height != height_ || format != format_) {
            pool_.reset();
            const auto attributes = apple::surfaceAttributes(width, height, format);
            if (CVPixelBufferPoolCreate(nullptr, nullptr, attributes.value, pool_.out()) != kCVReturnSuccess) {
                fail(RCS_ERROR_DEVICE, "No pixel buffers of " + std::to_string(width) + "x" + std::to_string(height));
            }
            width_ = width;
            height_ = height;
            format_ = format;
        }
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_.value, &buffer) != kCVReturnSuccess) {
            fail(RCS_ERROR_DEVICE, "No pixel buffer for the picture");
        }
        return buffer;
    }

private:
    apple::CFRef<CVPixelBufferPoolRef> pool_;
    uint32_t width_ = 0, height_ = 0;
    OSType format_ = 0;
};

class ApplePictures final : public SamplePictures {
public:
    ~ApplePictures() override {
        if (rotation_) {
            VTPixelRotationSessionInvalidate(rotation_.value);
        }
        if (transfer_) {
            VTPixelTransferSessionInvalidate(transfer_.value);
        }
    }

    Picture fit(const void* sampleBuffer, uint32_t orientation, uint32_t width, uint32_t height) override {
        auto* buffer = static_cast<CMSampleBufferRef>(const_cast<void*>(sampleBuffer));
        if (CFGetTypeID(buffer) != CMSampleBufferGetTypeID()) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "sample_buffer is no CMSampleBufferRef");
        }
        CVImageBufferRef image = CMSampleBufferGetImageBuffer(buffer);
        if (!image || CFGetTypeID(image) != CVPixelBufferGetTypeID()) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "The sample buffer holds no picture (a CVPixelBuffer)");
        }
        const OSType format = CVPixelBufferGetPixelFormatType(image);
        const rcs_pixel_format layout = apple::layoutOf(format);
        if (!layout) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "The picture is in a pixel format the encoders do not take (BGRA8, NV12, "
                                             "P010, NV24 or P410)");
        }
        const bool attached = !orientation;
        orientation = attached ? attachedOrientation(buffer) : orientation;
        CFStringRef rotation = uprightRotation(orientation);
        if (!rotation) {
            fail(orientation >= 1 && orientation <= 8 ? RCS_ERROR_UNSUPPORTED : RCS_ERROR_INVALID_ARGUMENT,
                 std::string(attached ? "The buffer's RPVideoSampleOrientationKey" : "orientation") + " is " +
                     std::to_string(orientation) + ": pictures are turned upright from 1, 3, 6 or 8, never mirrored");
        }
        const auto sourceWidth = uint32_t(CVPixelBufferGetWidth(image));
        const auto sourceHeight = uint32_t(CVPixelBufferGetHeight(image));
        const bool sideways = CFEqual(rotation, kVTRotation_CW90) || CFEqual(rotation, kVTRotation_CCW90);
        const uint32_t uprightWidth = sideways ? sourceHeight : sourceWidth;
        const uint32_t uprightHeight = sideways ? sourceWidth : sourceHeight;

        apple::CFRef<CVPixelBufferRef> upright;
        CVPixelBufferRef source = image;
        if (!CFEqual(rotation, kVTRotation_0)) {
            if (!rotation_ && VTPixelRotationSessionCreate(nullptr, rotation_.out()) != noErr) {
                fail(RCS_ERROR_DEVICE, "VideoToolbox cannot turn pictures (VTPixelRotationSession)");
            }
            VTSessionSetProperty(rotation_.value, kVTPixelRotationPropertyKey_Rotation, rotation);
            upright.reset(rotated_.take(uprightWidth, uprightHeight, format));
            if (VTPixelRotationSessionRotateImage(rotation_.value, image, upright.value) != noErr) {
                fail(RCS_ERROR_DEVICE, "Turning the picture upright failed (VTPixelRotationSession)");
            }
            source = upright.value;
        }

        apple::CFRef<CVPixelBufferRef> out;
        if (uprightWidth == width && uprightHeight == height) {
            out.reset(CVPixelBufferRetain(source));
        } else {
            if (!transfer_) {
                if (VTPixelTransferSessionCreate(nullptr, transfer_.out()) != noErr) {
                    fail(RCS_ERROR_DEVICE, "VideoToolbox cannot scale pictures (VTPixelTransferSession)");
                }
                // The shape kept, the rest black.
                VTSessionSetProperty(transfer_.value, kVTPixelTransferPropertyKey_ScalingMode,
                                     kVTScalingMode_Letterbox);
            }
            out.reset(fitted_.take(width, height, format));
            if (VTPixelTransferSessionTransferImage(transfer_.value, source, out.value) != noErr) {
                fail(RCS_ERROR_DEVICE, "Fitting the picture into the stream failed (VTPixelTransferSession)");
            }
        }

        Picture picture;
        picture.frame.width = width;
        picture.frame.height = height;
        picture.frame.surface = apple::retainSurface(out.value);
        if (rgbLayout(layout)) {
            picture.frame.color = rgbColor(picture.frame.color);
        } else {
            picture.frame.color = {RCS_PRIMARIES_BT709, RCS_TRANSFER_BT709, matrixOf(image),
                                   fullRange(format) ? RCS_RANGE_FULL : RCS_RANGE_LIMITED};
        }
        picture.frame.timestampNs = monotonicNs();
        picture.content = letterbox(uprightWidth, uprightHeight, width, height);
        picture.size = {uprightWidth, uprightHeight, 0};
        return picture;
    }

private:
    apple::CFRef<VTPixelRotationSessionRef> rotation_;
    apple::CFRef<VTPixelTransferSessionRef> transfer_;
    Buffers rotated_, fitted_;
};

constexpr OSStatus inputSpent = 'rcsd'; // The input procedure has nothing more for now.

class AppleSound final : public SampleSound {
public:
    ~AppleSound() override {
        if (converter_) {
            AudioConverterDispose(converter_);
        }
    }

    void convert(const void* sampleBuffer, std::vector<float>& samples) override {
        auto* buffer = static_cast<CMSampleBufferRef>(const_cast<void*>(sampleBuffer));
        if (CFGetTypeID(buffer) != CMSampleBufferGetTypeID()) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "sample_buffer is no CMSampleBufferRef");
        }
        CMFormatDescriptionRef description = CMSampleBufferGetFormatDescription(buffer);
        const AudioStreamBasicDescription* format =
            description ? CMAudioFormatDescriptionGetStreamBasicDescription(description) : nullptr;
        if (!format || format->mFormatID != kAudioFormatLinearPCM) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "The sample buffer holds no linear PCM sound");
        }
        if (format->mChannelsPerFrame < 1 || format->mChannelsPerFrame > 2 || format->mSampleRate <= 0) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "The sound has " + std::to_string(format->mChannelsPerFrame) +
                                                 " channels: one or two are taken");
        }
        const auto frames = CMItemCount(CMSampleBufferGetNumSamples(buffer));
        if (frames <= 0) {
            return;
        }
        open(*format);

        // The buffer's bytes, one AudioBuffer per channel where they are not interleaved.
        size_t listSize = 0;
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(buffer, &listSize, nullptr, 0, nullptr, nullptr, 0,
                                                                nullptr);
        list_.resize(std::max(listSize, sizeof(AudioBufferList)));
        auto* list = reinterpret_cast<AudioBufferList*>(list_.data());
        apple::CFRef<CMBlockBufferRef> block;
        if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
                buffer, nullptr, list, list_.size(), nullptr, nullptr,
                kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, block.out()) != noErr) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "The sample buffer's sound cannot be read");
        }

        Input input{list, UInt32(frames)};
        const double ratio = audioRate / format->mSampleRate;
        const auto room = UInt32(std::ceil(frames * ratio)) + 64;
        const size_t start = samples.size();
        samples.resize(start + size_t(room) * audioChannels);
        AudioBufferList out;
        out.mNumberBuffers = 1;
        out.mBuffers[0].mNumberChannels = audioChannels;
        out.mBuffers[0].mDataByteSize = room * audioChannels * sizeof(float);
        out.mBuffers[0].mData = samples.data() + start;
        UInt32 produced = room;
        const OSStatus status = AudioConverterFillComplexBuffer(converter_, provide, &input, &produced, &out, nullptr);
        if (status != noErr && status != inputSpent) {
            samples.resize(start);
            fail(RCS_ERROR_DEVICE, "Converting the sound failed (AudioConverter " + std::to_string(status) + ")");
        }
        samples.resize(start + size_t(produced) * audioChannels);
    }

private:
    struct Input {
        AudioBufferList* list;
        UInt32 frames; // Not handed over yet.
    };

    // The converter's input: the buffer's sound once, then nothing for now.
    static OSStatus provide(AudioConverterRef, UInt32* packets, AudioBufferList* data,
                            AudioStreamPacketDescription**, void* user) {
        auto* input = static_cast<Input*>(user);
        if (!input->frames) {
            *packets = 0;
            return inputSpent;
        }
        *packets = input->frames;
        for (UInt32 i = 0; i < data->mNumberBuffers && i < input->list->mNumberBuffers; ++i) {
            data->mBuffers[i] = input->list->mBuffers[i];
        }
        input->frames = 0;
        return noErr;
    }

    // A converter for this format, kept while it stays.
    void open(const AudioStreamBasicDescription& format) {
        if (converter_ && std::memcmp(&format, &format_, sizeof(format)) == 0) {
            return;
        }
        if (converter_) {
            AudioConverterDispose(converter_);
            converter_ = nullptr;
        }
        AudioStreamBasicDescription out{};
        out.mSampleRate = audioRate;
        out.mFormatID = kAudioFormatLinearPCM;
        out.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        out.mChannelsPerFrame = audioChannels;
        out.mBitsPerChannel = 32;
        out.mBytesPerFrame = audioChannels * sizeof(float);
        out.mFramesPerPacket = 1;
        out.mBytesPerPacket = out.mBytesPerFrame;
        if (const OSStatus status = AudioConverterNew(&format, &out, &converter_); status != noErr) {
            converter_ = nullptr;
            fail(RCS_ERROR_INVALID_ARGUMENT, "This sound cannot be converted to 48 kHz stereo (AudioConverter " +
                                                 std::to_string(status) + ")");
        }
        if (format.mChannelsPerFrame == 1) { // Mono on both channels.
            const SInt32 map[2] = {0, 0};
            AudioConverterSetProperty(converter_, kAudioConverterChannelMap, sizeof(map), map);
        }
        format_ = format;
    }

    AudioConverterRef converter_ = nullptr;
    AudioStreamBasicDescription format_{};
    std::vector<uint8_t> list_; // Room for the buffer's AudioBufferList.
};

} // namespace

HostImages hostImages() noexcept {
    return HostImages::sampleBuffers;
}

std::unique_ptr<SamplePictures> createSamplePictures() {
    return std::make_unique<ApplePictures>();
}

std::unique_ptr<SampleSound> createSampleSound() {
    return std::make_unique<AppleSound>();
}

} // namespace rcsdk::platform
