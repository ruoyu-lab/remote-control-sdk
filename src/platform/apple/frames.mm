// Decoded pictures as the host gets them on macOS and iOS: pixel buffers
// described for on_gpu_frame, or mapped as BGRA for on_frame; and captured
// ones made smaller for a stream's lower encodings.
#include "media/yuv.hpp"
#include "platform/apple/cf.hpp"
#include "platform/platform.hpp"

#include <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <cstring>
#include <vector>

namespace rcsdk::platform {

void withCpuImage(const VideoFrame& frame, const std::function<void(const rcs_frame&)>& use) {
    auto buffer = static_cast<CVPixelBufferRef>(frame.surface.get());
    if (!buffer) {
        fail(RCS_ERROR_UNSUPPORTED, "Frame has no pixel buffer");
    }
    const OSType format = CVPixelBufferGetPixelFormatType(buffer);
    const rcs_pixel_format layout = apple::layoutOf(format);
    if (!layout) {
        fail(RCS_ERROR_UNSUPPORTED, "Unsupported pixel buffer format for CPU output");
    }
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        fail(RCS_ERROR_CAPTURE, "Could not map the pixel buffer");
    }
    struct Unlock {
        CVPixelBufferRef buffer;
        ~Unlock() { CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly); }
    } unlock{buffer};
    rcs_frame view{};
    view.struct_size = sizeof(view);
    view.width = frame.width;
    view.height = frame.height;
    view.format = RCS_PIXEL_BGRA8;
    view.stream = frame.stream;
    view.sequence = frame.sequence;
    view.timestamp_ns = frame.timestampNs;
    if (format == kCVPixelFormatType_32BGRA) {
        view.stride_bytes = static_cast<uint32_t>(CVPixelBufferGetBytesPerRow(buffer));
        view.data = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        view.data_size = uint64_t(view.stride_bytes) * CVPixelBufferGetHeight(buffer);
        use(view);
        return;
    }
    std::vector<uint8_t> converted(size_t(frame.width) * frame.height * 4);
    const YuvLayout yuv{layout == RCS_PIXEL_NV24 || layout == RCS_PIXEL_P410,
                        layout == RCS_PIXEL_P010 || layout == RCS_PIXEL_P410 ? 10u : 8u};
    yuvToBgra(static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0)),
              CVPixelBufferGetBytesPerRowOfPlane(buffer, 0),
              static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1)),
              CVPixelBufferGetBytesPerRowOfPlane(buffer, 1), frame.width, frame.height, yuv,
              yuvCoefficients(frame.color.matrix, frame.color.range, yuv.bits), converted.data(), frame.width * 4);
    view.stride_bytes = frame.width * 4;
    view.data = converted.data();
    view.data_size = converted.size();
    use(view);
}

namespace {

// Holds a pixel buffer's base address for one scope.
class Locked {
public:
    Locked(CVPixelBufferRef buffer, CVPixelBufferLockFlags flags) : buffer_(buffer), flags_(flags) {
        if (CVPixelBufferLockBaseAddress(buffer, flags) != kCVReturnSuccess) {
            fail(RCS_ERROR_CODEC, "Could not map the pixel buffer");
        }
    }
    ~Locked() { CVPixelBufferUnlockBaseAddress(buffer_, flags_); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;

private:
    CVPixelBufferRef buffer_;
    CVPixelBufferLockFlags flags_;
};

// Capture writes the stream's layout already: read in place. The host's BGRA
// pixel buffers are converted on the CPU.
class Reader final : public SurfaceReader {
public:
    void read(const VideoFrame& frame, const Picture& picture,
              const std::function<void(const YuvImage&)>& use) override {
        auto buffer = static_cast<CVPixelBufferRef>(frame.surface.get());
        if (!buffer) {
            fail(RCS_ERROR_CODEC, "A frame without a pixel buffer");
        }
        const rcs_pixel_format layout = apple::layoutOf(CVPixelBufferGetPixelFormatType(buffer));
        const Locked locked(buffer, kCVPixelBufferLock_ReadOnly);
        if (layout == yuvLayoutOf(picture.format)) {
            use({layout, frame.width, frame.height,
                 static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0)),
                 CVPixelBufferGetBytesPerRowOfPlane(buffer, 0),
                 static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1)),
                 CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)});
        } else if (layout == RCS_PIXEL_BGRA8) {
            fromBgra(static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer)),
                     CVPixelBufferGetBytesPerRow(buffer), frame.width, frame.height, picture, converted_);
            use(converted_.view());
        } else {
            fail(RCS_ERROR_CODEC, "A pixel buffer of another layout than the stream's (" + pictureText(picture) + ")");
        }
    }

private:
    YuvBuffer converted_;
};

void copyPlane(const uint8_t* from, size_t fromStride, uint8_t* to, size_t toStride, size_t rowBytes, uint32_t rows) {
    for (uint32_t row = 0; row < rows; ++row) {
        std::memcpy(to + toStride * row, from + fromStride * row, rowBytes);
    }
}

// Decoded pictures into IOSurface pixel buffers of the stream's layout, from a
// pool of the latest format and size.
class Writer final : public SurfaceWriter {
public:
    VideoFrame write(const YuvImage& image, const Picture& picture) override {
        const OSType format = apple::pixelFormatOf(picture);
        if (!pool_ || format != format_ || image.width != width_ || image.height != height_) {
            const auto attributes = apple::surfaceAttributes(image.width, image.height, format);
            if (CVPixelBufferPoolCreate(nullptr, nullptr, attributes.value, pool_.out()) != kCVReturnSuccess) {
                fail(RCS_ERROR_CODEC, "Could not create pixel buffers for decoded pictures");
            }
            format_ = format;
            width_ = image.width;
            height_ = image.height;
        }
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_.value, &buffer) != kCVReturnSuccess) {
            fail(RCS_ERROR_CODEC, "Could not get a pixel buffer for a decoded picture");
        }
        VideoFrame frame;
        frame.surface = apple::retainSurface(buffer);
        CVPixelBufferRelease(buffer); // The frame holds it now.
        {
            const Locked locked(buffer, 0);
            const bool wide = image.layout == RCS_PIXEL_P010 || image.layout == RCS_PIXEL_P410;
            const bool full = image.layout == RCS_PIXEL_NV24 || image.layout == RCS_PIXEL_P410;
            const size_t sample = wide ? 2 : 1;
            copyPlane(image.luma, image.lumaStride, static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0)),
                      CVPixelBufferGetBytesPerRowOfPlane(buffer, 0), size_t(image.width) * sample, image.height);
            copyPlane(image.chroma, image.chromaStride,
                      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1)),
                      CVPixelBufferGetBytesPerRowOfPlane(buffer, 1),
                      size_t(full ? image.width : (image.width + 1) / 2) * 2 * sample,
                      full ? image.height : (image.height + 1) / 2);
        }
        const auto keys = apple::colorKeysOf(picture);
        CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, keys.primaries, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, keys.transfer, kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, keys.matrix, kCVAttachmentMode_ShouldPropagate);
        frame.width = image.width;
        frame.height = image.height;
        frame.color = colorOf(picture);
        return frame;
    }

private:
    apple::CFRef<CVPixelBufferPoolRef> pool_;
    OSType format_ = 0;
    uint32_t width_ = 0, height_ = 0;
};

// A captured picture made smaller for a lower encoding: VideoToolbox scales
// it on the GPU into a pixel buffer of its own format (and attachments).
class Scaler final : public SurfaceScaler {
public:
    VideoFrame scale(const VideoFrame& frame, uint32_t width, uint32_t height) override {
        auto source = static_cast<CVPixelBufferRef>(frame.surface.get());
        if (!source) {
            fail(RCS_ERROR_UNSUPPORTED, "The picture has no pixel buffer to scale");
        }
        if (!transfer_) {
            apple::check(VTPixelTransferSessionCreate(nullptr, transfer_.out()), "Scaling a picture", RCS_ERROR_CODEC);
        }
        const OSType format = CVPixelBufferGetPixelFormatType(source);
        if (!pool_ || format != format_ || width != width_ || height != height_) {
            const auto attributes = apple::surfaceAttributes(width, height, format);
            if (CVPixelBufferPoolCreate(nullptr, nullptr, attributes.value, pool_.out()) != kCVReturnSuccess) {
                fail(RCS_ERROR_CODEC, "Could not create pixel buffers for scaled pictures");
            }
            format_ = format;
            width_ = width;
            height_ = height;
        }
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_.value, &buffer) != kCVReturnSuccess) {
            fail(RCS_ERROR_CODEC, "Could not get a pixel buffer for a scaled picture");
        }
        VideoFrame scaled = frame;
        scaled.surface = apple::retainSurface(buffer);
        CVPixelBufferRelease(buffer); // The frame holds it now.
        CVBufferPropagateAttachments(source, buffer);
        apple::check(VTPixelTransferSessionTransferImage(transfer_.value, source, buffer), "Scaling a picture",
                     RCS_ERROR_CODEC);
        scaled.width = width;
        scaled.height = height;
        scaled.waitGpu = nullptr; // VideoToolbox's transfer is done when it returns.
        return scaled;
    }

private:
    apple::CFRef<VTPixelTransferSessionRef> transfer_;
    apple::CFRef<CVPixelBufferPoolRef> pool_;
    OSType format_ = 0;
    uint32_t width_ = 0, height_ = 0;
};

} // namespace

std::unique_ptr<SurfaceReader> createSurfaceReader() {
    return std::make_unique<Reader>();
}

std::unique_ptr<SurfaceScaler> createSurfaceScaler() {
    return std::make_unique<Scaler>();
}

std::unique_ptr<SurfaceWriter> createSurfaceWriter(const DecoderConfig&) {
    return std::make_unique<Writer>();
}

uint64_t deviceAdapter(const std::shared_ptr<void>&) noexcept {
    return 0;
}

std::shared_ptr<void> retainHostDevice(void*) {
    return nullptr; // Only Windows takes a host device; the configuration is validated.
}

void checkHostDevice(void*, uint32_t) { fail(RCS_ERROR_UNSUPPORTED, "Host GPU devices are used on Windows only"); }

VideoFrame hostGpuFrame(const rcs_gpu_frame_info& info) {
    if (info.resource_type != RCS_GPU_CVPIXELBUFFER) {
        fail(RCS_ERROR_UNSUPPORTED, "This system takes the host's pictures as CVPixelBuffers (RCS_GPU_CVPIXELBUFFER)");
    }
    auto buffer = static_cast<CVPixelBufferRef>(info.resource);
    if (!buffer || CFGetTypeID(buffer) != CVPixelBufferGetTypeID()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "resource is no CVPixelBufferRef");
    }
    const rcs_pixel_format layout = apple::layoutOf(CVPixelBufferGetPixelFormatType(buffer));
    if (!layout || layout != info.format) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The pixel buffer's format is not the one `format` names (BGRA8, NV12, "
                                         "P010, NV24 or P410)");
    }
    const auto width = uint32_t(CVPixelBufferGetWidth(buffer)), height = uint32_t(CVPixelBufferGetHeight(buffer));
    if (width != info.width || height != info.height) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The pixel buffer is " + std::to_string(width) + "x" +
                                             std::to_string(height) + ", not the " + std::to_string(info.width) +
                                             "x" + std::to_string(info.height) + " width and height say");
    }
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.surface = apple::retainSurface(buffer);
    if (rgbLayout(layout)) {
        frame.color = rgbColor(frame.color);
    } else if (!info.color.matrix || !info.color.range) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "A YCbCr picture needs its color.matrix and color.range");
    } else {
        frame.color = info.color;
        frame.color.primaries = info.color.primaries ? info.color.primaries : RCS_PRIMARIES_BT709;
        frame.color.transfer = info.color.transfer ? info.color.transfer : RCS_TRANSFER_BT709;
    }
    frame.timestampNs = info.timestamp_ns ? info.timestamp_ns : monotonicNs();
    return frame;
}

bool describeGpuFrame(const VideoFrame& frame, rcs_gpu_frame_info& info) {
    auto buffer = static_cast<CVPixelBufferRef>(frame.surface.get());
    if (!buffer) {
        return false;
    }
    info.resource_type = RCS_GPU_CVPIXELBUFFER;
    info.format = apple::layoutOf(CVPixelBufferGetPixelFormatType(buffer));
    info.width = frame.width;
    info.height = frame.height;
    info.surface_width = static_cast<uint32_t>(CVPixelBufferGetWidth(buffer));
    info.surface_height = static_cast<uint32_t>(CVPixelBufferGetHeight(buffer));
    info.color = rgbLayout(info.format) ? rgbColor(frame.color) : frame.color;
    info.stream = frame.stream;
    info.sequence = frame.sequence;
    info.timestamp_ns = frame.timestampNs;
    info.resource = buffer;
    info.device = nullptr;
    return true;
}

} // namespace rcsdk::platform
