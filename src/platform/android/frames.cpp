// Pictures as hardware buffers on Android. Decoded ones are the decoder's
// images (decoder.cpp), described for on_gpu_frame or read as BGRA for
// on_frame: converted on the CPU where the CPU maps them, else drawn as BGRA8
// on the GPU. The host's own (rcs_session_push_gpu_frame,
// rcs_session_push_hardware_buffer) are drawn into buffers of the SDK's when
// pushed (gl.cpp): its producer reuses its buffer once it lets go of it,
// whoever else holds a reference.
#include "media/picture_size.hpp"
#include "media/yuv.hpp"
#include "platform/android/gl.hpp"
#include "platform/platform.hpp"

#include <rcsdk/gpu.h>

#include <vector>

namespace rcsdk::platform {
namespace {

AHardwareBuffer_Desc describe(const void* buffer) {
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(static_cast<const AHardwareBuffer*>(buffer), &desc);
    return desc;
}

void requireSampled(const AHardwareBuffer_Desc& desc) {
    if (!(desc.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The hardware buffer is not for the GPU to sample "
                                         "(AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)");
    }
}

std::string sizeText(uint32_t width, uint32_t height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

// YCbCr 4:2:0 planes the CPU mapped, converted to BGRA8 with the frame's colour.
void yuvPlanesToBgra(const AHardwareBuffer_Planes& planes, const VideoFrame& frame, uint8_t* bgra, uint32_t stride) {
    const auto& y = planes.planes[0];
    const auto& u = planes.planes[1];
    const auto& v = planes.planes[2];
    const auto* cb = static_cast<const uint8_t*>(u.data);
    const auto* cr = static_cast<const uint8_t*>(v.data);
    const uint8_t* interleaved = cb;
    size_t interleavedStride = u.rowStride;
    std::vector<uint8_t> chroma;
    if (u.pixelStride != 2 || cr != cb + 1) {
        const uint32_t chromaWidth = (frame.width + 1) / 2, chromaHeight = (frame.height + 1) / 2;
        chroma.resize(size_t(chromaWidth) * 2 * chromaHeight);
        for (uint32_t row = 0; row < chromaHeight; ++row) {
            for (uint32_t x = 0; x < chromaWidth; ++x) {
                chroma[(size_t(row) * chromaWidth + x) * 2] = cb[size_t(row) * u.rowStride + size_t(x) * u.pixelStride];
                chroma[(size_t(row) * chromaWidth + x) * 2 + 1] =
                    cr[size_t(row) * v.rowStride + size_t(x) * v.pixelStride];
            }
        }
        interleaved = chroma.data();
        interleavedStride = size_t(chromaWidth) * 2;
    }
    yuvToBgra(static_cast<const uint8_t*>(y.data), y.rowStride, interleaved, interleavedStride, frame.width,
              frame.height, YuvLayout{}, yuvCoefficients(frame.color.matrix, frame.color.range, 8), bgra, stride);
}

// Turns pictures of any size upright and fits them into the stream (rcs_session_push_hardware_buffer).
class HardwareBufferPictures final : public SamplePictures {
public:
    Picture fit(const void* image, uint32_t rotation, uint32_t width, uint32_t height) override {
        const auto desc = describe(image);
        requireSampled(desc);
        const bool across = rotation == 90 || rotation == 270;
        const uint32_t uprightWidth = across ? desc.height : desc.width;
        const uint32_t uprightHeight = across ? desc.width : desc.height;
        Picture picture;
        picture.content = uprightWidth == width && uprightHeight == height
                              ? Rect{0, 0, width, height}
                              : letterbox(uprightWidth, uprightHeight, width, height);
        picture.frame.width = width;
        picture.frame.height = height;
        picture.frame.surface = android::gl::draw(static_cast<AHardwareBuffer*>(const_cast<void*>(image)), rotation,
                                                  picture.content, width, height);
        picture.frame.color = rgbColor(picture.frame.color);
        picture.frame.timestampNs = monotonicNs();
        picture.size = {uprightWidth, uprightHeight, 0};
        return picture;
    }
};

} // namespace

void withCpuImage(const VideoFrame& frame, const std::function<void(const rcs_frame&)>& use) {
    auto* buffer = static_cast<AHardwareBuffer*>(frame.surface.get());
    if (!buffer) {
        fail(RCS_ERROR_UNSUPPORTED, "Frame has no hardware buffer");
    }
    const auto desc = describe(buffer);
    rcs_frame view{};
    view.struct_size = sizeof(view);
    view.width = frame.width;
    view.height = frame.height;
    view.format = RCS_PIXEL_BGRA8;
    view.stream = frame.stream;
    view.sequence = frame.sequence;
    view.timestamp_ns = frame.timestampNs;
    // Pictures the CPU maps, converted into rows of our own.
    std::vector<uint8_t> converted;
    const auto convert = [&] {
        converted.resize(size_t(frame.width) * frame.height * 4);
        view.stride_bytes = frame.width * 4;
        view.data = converted.data();
        view.data_size = converted.size();
    };
    const bool readable = desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK;
    if (readable && android::gl::layoutOf(desc.format) == RCS_PIXEL_BGRA8) {
        void* address = nullptr;
        if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &address) != 0) {
            fail(RCS_ERROR_CAPTURE, "The picture cannot be read on the CPU");
        }
        convert();
        for (uint32_t y = 0; y < frame.height; ++y) { // RGBA in memory: red and blue swapped.
            const auto* from = static_cast<const uint8_t*>(address) + size_t(y) * desc.stride * 4;
            auto* to = converted.data() + size_t(y) * frame.width * 4;
            for (uint32_t x = 0; x < frame.width; ++x) {
                to[x * 4] = from[x * 4 + 2];
                to[x * 4 + 1] = from[x * 4 + 1];
                to[x * 4 + 2] = from[x * 4];
                to[x * 4 + 3] = 255;
            }
        }
        AHardwareBuffer_unlock(buffer, nullptr);
        use(view);
        return;
    }
    // YCbCr 4:2:0 the CPU maps: chroma interleaved (NV12, NV21) or in planes of their own.
    AHardwareBuffer_Planes planes{};
    if (readable && AHardwareBuffer_lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &planes) == 0) {
        if (planes.planeCount == 3) {
            convert();
            yuvPlanesToBgra(planes, frame, converted.data(), view.stride_bytes);
            AHardwareBuffer_unlock(buffer, nullptr);
            use(view);
            return;
        }
        AHardwareBuffer_unlock(buffer, nullptr);
    }
    // The decoder's own layout (any depth and chroma, compressed): the GPU
    // draws it as BGRA8 into a buffer the CPU maps, whose rows go to `use`
    // as they are.
    if (!(desc.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)) {
        fail(RCS_ERROR_CAPTURE, "The picture is laid out for neither the CPU nor the GPU to read");
    }
    const rcs_color* yuv = frame.color.matrix ? &frame.color : nullptr; // RGB pictures have no matrix.
    android::gl::read(buffer, frame.width, frame.height, yuv, [&](const uint8_t* bgra, uint32_t strideBytes) {
        view.data = bgra;
        view.stride_bytes = strideBytes;
        view.data_size = size_t(strideBytes) * frame.height;
        use(view);
    });
}

std::shared_ptr<void> retainHostDevice(void*) {
    return nullptr; // Only Windows takes a host device; the configuration is validated.
}

void checkHostDevice(void*, uint32_t) {
    fail(RCS_ERROR_UNSUPPORTED, "Host GPU devices are used on Windows only");
}

VideoFrame hostGpuFrame(const rcs_gpu_frame_info& info) {
    if (info.resource_type != RCS_GPU_AHARDWAREBUFFER) {
        fail(RCS_ERROR_UNSUPPORTED, "Android takes the host's pictures as AHardwareBuffers (RCS_GPU_AHARDWAREBUFFER)");
    }
    if (!info.resource) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "resource is no AHardwareBuffer");
    }
    const auto desc = describe(info.resource);
    requireSampled(desc);
    if (!android::gl::layoutOf(desc.format) || android::gl::layoutOf(desc.format) != info.format) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The hardware buffer's format is not the one `format` names (BGRA8: "
                                         "R8G8B8A8 or R8G8B8X8; NV12: Y8Cb8Cr8_420)");
    }
    if (desc.width != info.width || desc.height != info.height) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The hardware buffer is " + sizeText(desc.width, desc.height) + ", not the " +
                                             sizeText(info.width, info.height) + " width and height say");
    }
    VideoFrame frame;
    frame.width = info.width;
    frame.height = info.height;
    frame.surface = android::gl::draw(static_cast<AHardwareBuffer*>(info.resource), 0,
                                      Rect{0, 0, info.width, info.height}, info.width, info.height);
    frame.color = rgbColor(frame.color);
    frame.timestampNs = info.timestamp_ns ? info.timestamp_ns : monotonicNs();
    return frame;
}

bool describeGpuFrame(const VideoFrame& frame, rcs_gpu_frame_info& info) {
    auto* buffer = static_cast<AHardwareBuffer*>(frame.surface.get());
    if (!buffer) {
        return false;
    }
    const auto desc = describe(buffer);
    info.resource_type = RCS_GPU_AHARDWAREBUFFER;
    // Decoded: the picture's layout, in the decoder's own (Y8Cb8Cr8_420, implementation-defined).
    info.format = frame.layout ? frame.layout : android::gl::layoutOf(desc.format);
    info.width = frame.width;
    info.height = frame.height;
    info.surface_width = desc.width;
    info.surface_height = desc.height;
    info.color = rgbLayout(info.format) ? rgbColor(frame.color) : frame.color;
    info.stream = frame.stream;
    info.sequence = frame.sequence;
    info.timestamp_ns = frame.timestampNs;
    info.resource = buffer;
    info.device = nullptr;
    return true;
}

HostImages hostImages() noexcept {
    return HostImages::hardwareBuffers;
}

std::unique_ptr<SamplePictures> createSamplePictures() {
    return std::make_unique<HardwareBufferPictures>();
}

std::unique_ptr<SampleSound> createSampleSound() {
    fail(RCS_ERROR_UNSUPPORTED, "Android has no Core Media sample buffers: push sound with rcs_session_push_audio");
}

} // namespace rcsdk::platform
