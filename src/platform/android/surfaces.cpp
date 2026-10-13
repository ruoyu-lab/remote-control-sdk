// The codec libraries' pictures on Android (platform.hpp: SurfaceReader,
// SurfaceWriter). Encoding: a captured hardware buffer is read as BGRA
// (mapped where the CPU can, else drawn by the GPU, as for on_frame) and
// converted to the stream's layout on the CPU. Decoding: 8-bit 4:2:0
// pictures go into YCbCr hardware buffers (AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420),
// the other formats, which Android's buffers have no YCbCr layout of, into
// RGBA ones, converted on the CPU. A stream's lower encodings: captured
// buffers drawn smaller on the GPU (gl::draw).
#include "platform/android/gl.hpp"
#include "platform/platform.hpp"

#include <android/hardware_buffer.h>

#include <cstring>

namespace rcsdk::platform {
namespace {

class Reader final : public SurfaceReader {
public:
    void read(const VideoFrame& frame, const Picture& picture,
              const std::function<void(const YuvImage&)>& use) override {
        withCpuImage(frame, [&](const rcs_frame& image) {
            fromBgra(image.data, image.stride_bytes, image.width, image.height, picture, converted_);
        });
        use(converted_.view());
    }

private:
    YuvBuffer converted_;
};

std::shared_ptr<void> allocate(uint32_t width, uint32_t height, uint32_t format) {
    AHardwareBuffer_Desc desc{};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = format;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    AHardwareBuffer* buffer = nullptr;
    if (AHardwareBuffer_allocate(&desc, &buffer) != 0) {
        fail(RCS_ERROR_CODEC, "Could not allocate a hardware buffer for a decoded picture");
    }
    return {buffer, [](void* b) { AHardwareBuffer_release(static_cast<AHardwareBuffer*>(b)); }};
}

class Writer final : public SurfaceWriter {
public:
    VideoFrame write(const YuvImage& image, const Picture& picture) override {
        VideoFrame frame;
        frame.width = image.width;
        frame.height = image.height;
        if (image.layout == RCS_PIXEL_NV12) {
            frame.surface = allocate(image.width, image.height, AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420);
            auto* buffer = static_cast<AHardwareBuffer*>(frame.surface.get());
            AHardwareBuffer_Planes planes{};
            if (AHardwareBuffer_lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &planes) != 0 ||
                planes.planeCount != 3) {
                fail(RCS_ERROR_CODEC, "The decoded picture's hardware buffer cannot be written on the CPU");
            }
            const auto& y = planes.planes[0];
            for (uint32_t row = 0; row < image.height; ++row) {
                std::memcpy(static_cast<uint8_t*>(y.data) + size_t(y.rowStride) * row,
                            image.luma + image.lumaStride * row, image.width);
            }
            const auto& u = planes.planes[1];
            const auto& v = planes.planes[2];
            for (uint32_t row = 0; row < (image.height + 1) / 2; ++row) {
                const uint8_t* from = image.chroma + image.chromaStride * row;
                auto* cb = static_cast<uint8_t*>(u.data) + size_t(u.rowStride) * row;
                auto* cr = static_cast<uint8_t*>(v.data) + size_t(v.rowStride) * row;
                for (uint32_t x = 0; x < (image.width + 1) / 2; ++x) {
                    cb[size_t(x) * u.pixelStride] = from[x * 2];
                    cr[size_t(x) * v.pixelStride] = from[x * 2 + 1];
                }
            }
            AHardwareBuffer_unlock(buffer, nullptr);
            frame.color = colorOf(picture);
            return frame;
        }
        VideoFrame pixels;
        toBgra(image, picture, pixels);
        frame.surface = allocate(image.width, image.height, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        auto* buffer = static_cast<AHardwareBuffer*>(frame.surface.get());
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(buffer, &desc);
        void* address = nullptr;
        if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &address) != 0) {
            fail(RCS_ERROR_CODEC, "The decoded picture's hardware buffer cannot be written on the CPU");
        }
        for (uint32_t row = 0; row < image.height; ++row) { // BGRA to RGBA in memory.
            const uint8_t* from = pixels.pixels->data() + size_t(pixels.stride) * row;
            auto* to = static_cast<uint8_t*>(address) + size_t(desc.stride) * 4 * row;
            for (uint32_t x = 0; x < image.width; ++x) {
                to[x * 4] = from[x * 4 + 2];
                to[x * 4 + 1] = from[x * 4 + 1];
                to[x * 4 + 2] = from[x * 4];
                to[x * 4 + 3] = 255;
            }
        }
        AHardwareBuffer_unlock(buffer, nullptr);
        frame.color = pixels.color;
        return frame;
    }
};

class Scaler final : public SurfaceScaler {
public:
    VideoFrame scale(const VideoFrame& frame, uint32_t width, uint32_t height) override {
        auto* source = static_cast<AHardwareBuffer*>(frame.surface.get());
        if (!source) {
            fail(RCS_ERROR_UNSUPPORTED, "The picture has no hardware buffer to scale");
        }
        VideoFrame scaled = frame;
        scaled.surface = android::gl::draw(source, 0, Rect{0, 0, width, height}, width, height);
        scaled.width = width;
        scaled.height = height;
        scaled.layout = 0;                       // RGBA8 now...
        scaled.color = rgbColor(frame.color);    // ...of the picture's primaries and transfer.
        scaled.waitGpu = nullptr;                // Drawn once draw() returned.
        return scaled;
    }
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

} // namespace rcsdk::platform
