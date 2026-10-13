// The codec libraries' pictures on Windows (platform.hpp: SurfaceReader,
// SurfaceWriter), and captured ones made smaller for a stream's lower
// encodings (SurfaceScaler: the video processor draws them into a texture of
// their format). Encoding: a captured BGRA texture is converted on its GPU
// to the stream's layout, NV12 or P010 by the video processor, AYUV or Y410
// (4:4:4) by the YCbCr shader, and read back; 4:4:4 is rearranged into NV24
// or P410 on the CPU. Decoding: 4:2:0 pictures go up as NV12 or P010
// textures, 4:4:4 ones as BGRA8 (as the system's software decoders give
// them: Direct3D 11 has no bi-planar 4:4:4 format).
#include "platform/platform.hpp"
#include "platform/windows/media_foundation.hpp"
#include "platform/windows/video_processor.hpp"
#include "platform/windows/yuv_shader.hpp"

#include <cstring>

namespace rcsdk::platform {
namespace {

using win::Com;
constexpr rcs_result codecError = RCS_ERROR_CODEC;

class Reader final : public SurfaceReader {
public:
    void read(const VideoFrame& frame, const Picture& picture,
              const std::function<void(const YuvImage&)>& use) override {
        auto* source = static_cast<ID3D11Texture2D*>(frame.surface.get());
        if (!source) {
            fail(codecError, "A frame without a texture");
        }
        Com<ID3D11Device> device;
        source->GetDevice(device.out());
        if (device.value != device_.value) {
            device_.assign(device.value);
            device_->GetImmediateContext(context_.out());
            target_.reset();
            staging_.reset();
        }
        const bool full = chroma444(picture.format), deep = bitDepth(picture.format) > 8;
        const auto layout = deep ? win::YuvShader::Layout::y410 : win::YuvShader::Layout::ayuv;
        const auto targetDesc = full ? win::YuvShader::targetDesc(layout, frame.width, frame.height)
                                     : win::textureDesc(frame.width, frame.height,
                                                        deep ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12,
                                                        D3D11_BIND_RENDER_TARGET);
        D3D11_TEXTURE2D_DESC sourceDesc{};
        source->GetDesc(&sourceDesc);
        const bool direct = sourceDesc.Format == targetDesc.Format; // The host's YCbCr pictures, as they are.
        prepare(targetDesc, direct);
        {
            win::ContextLock lock(context_.value);
            if (direct) {
                context_->CopySubresourceRegion(staging_.value, 0, 0, 0, 0, source, 0, nullptr);
            } else if (full) {
                yuv_.draw(device_.value, context_.value, source, frame.width, frame.height, target_.value, layout,
                          picture);
                context_->CopyResource(staging_.value, target_.value);
            } else {
                converter_.blit(device_.value, context_.value, source, 0, frame.width, frame.height, target_.value,
                                picture);
                context_->CopyResource(staging_.value, target_.value);
            }
        }
        D3D11_MAPPED_SUBRESOURCE map{};
        {
            win::ContextLock lock(context_.value);
            win::check(context_->Map(staging_.value, 0, D3D11_MAP_READ, 0, &map), "Picture readback", codecError);
        }
        struct Unmap {
            ID3D11DeviceContext* context;
            ID3D11Texture2D* texture;
            ~Unmap() {
                win::ContextLock lock(context);
                context->Unmap(texture, 0);
            }
        } unmap{context_.value, staging_.value};
        const auto* bytes = static_cast<const uint8_t*>(map.pData);
        if (!full) { // NV12 and P010: chroma follows the luma rows of the texture's height.
            const rcs_pixel_format format = deep ? RCS_PIXEL_P010 : RCS_PIXEL_NV12;
            use({format, frame.width, frame.height, bytes, map.RowPitch, bytes + size_t(map.RowPitch) * targetDesc.Height,
                 map.RowPitch});
            return;
        }
        // AYUV (the bytes V, U, Y, A) and Y410 (U, Y, V in 10 bits each, U lowest) into NV24 and P410.
        packed_.resize(deep ? RCS_PIXEL_P410 : RCS_PIXEL_NV24, frame.width, frame.height);
        for (uint32_t row = 0; row < frame.height; ++row) {
            const uint8_t* from = bytes + size_t(map.RowPitch) * row;
            uint8_t* luma = packed_.luma() + packed_.lumaStride() * row;
            uint8_t* chroma = packed_.chroma() + packed_.chromaStride() * row;
            for (uint32_t x = 0; x < frame.width; ++x) {
                if (deep) {
                    uint32_t word;
                    std::memcpy(&word, from + x * 4, 4);
                    const auto put = [](uint8_t* at, uint32_t value) {
                        const auto sample = uint16_t((value & 0x3ff) << 6);
                        std::memcpy(at, &sample, 2);
                    };
                    put(luma + x * 2, word >> 10);
                    put(chroma + x * 4, word);
                    put(chroma + x * 4 + 2, word >> 20);
                } else {
                    luma[x] = from[x * 4 + 2];
                    chroma[x * 2] = from[x * 4 + 1];
                    chroma[x * 2 + 1] = from[x * 4];
                }
            }
        }
        use(packed_.view());
    }

private:
    // The conversion's target and the readback texture, of `desc`'s size and format.
    void prepare(const D3D11_TEXTURE2D_DESC& desc, bool direct) {
        D3D11_TEXTURE2D_DESC have{};
        if (staging_) {
            staging_->GetDesc(&have);
        }
        if (!staging_ || have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
            target_.reset();
            auto readback = desc;
            readback.BindFlags = 0;
            readback.Usage = D3D11_USAGE_STAGING;
            readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            readback.MiscFlags = 0;
            win::check(device_->CreateTexture2D(&readback, nullptr, staging_.out()), "Readback texture", codecError);
        }
        if (!direct && !target_) {
            win::check(device_->CreateTexture2D(&desc, nullptr, target_.out()), "Conversion texture", codecError);
        }
    }

    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11Texture2D> target_, staging_;
    win::VideoProcessor converter_;
    win::YuvShader yuv_;
    YuvBuffer packed_;
};

class Writer final : public SurfaceWriter {
public:
    explicit Writer(const DecoderConfig& config) {
        if (config.device) {
            d3d_.adopt(static_cast<ID3D11Device*>(config.device.get()));
        } else {
            d3d_.create();
        }
    }

    VideoFrame write(const YuvImage& image, const Picture& picture) override {
        VideoFrame frame;
        frame.width = image.width;
        frame.height = image.height;
        Com<ID3D11Texture2D> texture;
        if (image.layout == RCS_PIXEL_NV12 || image.layout == RCS_PIXEL_P010) {
            // Direct3D takes the chroma right after the luma rows, at the same pitch.
            const size_t sample = image.layout == RCS_PIXEL_P010 ? 2 : 1, pitch = size_t(image.width) * sample;
            const uint32_t chromaRows = (image.height + 1) / 2;
            contiguous_.resize(pitch * (image.height + chromaRows));
            for (uint32_t row = 0; row < image.height; ++row) {
                std::memcpy(contiguous_.data() + pitch * row, image.luma + image.lumaStride * row, pitch);
            }
            for (uint32_t row = 0; row < chromaRows; ++row) {
                std::memcpy(contiguous_.data() + pitch * (image.height + row), image.chroma + image.chromaStride * row,
                            pitch);
            }
            const auto desc = win::textureDesc(image.width, image.height,
                                               sample == 2 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12,
                                               D3D11_BIND_SHADER_RESOURCE);
            const D3D11_SUBRESOURCE_DATA data{contiguous_.data(), UINT(pitch), 0};
            win::check(d3d_.device->CreateTexture2D(&desc, &data, texture.out()), "Decoded picture upload",
                       codecError);
            frame.color = colorOf(picture);
        } else {
            VideoFrame pixels;
            toBgra(image, picture, pixels);
            const auto desc = win::textureDesc(image.width, image.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
            const D3D11_SUBRESOURCE_DATA data{pixels.pixels->data(), pixels.stride, 0};
            win::check(d3d_.device->CreateTexture2D(&desc, &data, texture.out()), "Decoded picture upload",
                       codecError);
            frame.color = pixels.color;
        }
        frame.surface = win::share(texture.value);
        frame.device = win::share(d3d_.device.value);
        return frame;
    }

private:
    win::Device d3d_;
    std::vector<uint8_t> contiguous_;
};

class Scaler final : public SurfaceScaler {
public:
    VideoFrame scale(const VideoFrame& frame, uint32_t width, uint32_t height) override {
        auto* source = static_cast<ID3D11Texture2D*>(frame.surface.get());
        if (!source) {
            fail(RCS_ERROR_UNSUPPORTED, "The picture has no texture to scale");
        }
        Com<ID3D11Device> device;
        source->GetDevice(device.out());
        if (device.value != device_.value) {
            device_.assign(device.value);
            device_->GetImmediateContext(context_.out());
        }
        D3D11_TEXTURE2D_DESC sourceDesc{};
        source->GetDesc(&sourceDesc);
        // A texture of its own each time: an encoder may still hold the previous one.
        const auto desc = win::textureDesc(width, height, sourceDesc.Format,
                                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Com<ID3D11Texture2D> texture;
        win::check(device_->CreateTexture2D(&desc, nullptr, texture.out()), "Scaled picture", codecError);
        {
            win::ContextLock lock(context_.value);
            scaler_.blit(device_.value, context_.value, source, 0, frame.width, frame.height, texture.value,
                         Picture{});
        }
        VideoFrame scaled = frame;
        scaled.width = width;
        scaled.height = height;
        scaled.surface = win::share(texture.value);
        return scaled;
    }

private:
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    win::VideoProcessor scaler_;
};

} // namespace

std::unique_ptr<SurfaceReader> createSurfaceReader() {
    return std::make_unique<Reader>();
}

std::unique_ptr<SurfaceScaler> createSurfaceScaler() {
    return std::make_unique<Scaler>();
}

std::unique_ptr<SurfaceWriter> createSurfaceWriter(const DecoderConfig& config) {
    return std::make_unique<Writer>(config);
}

uint64_t deviceAdapter(const std::shared_ptr<void>& device) noexcept {
    try {
        return device ? win::packLuid(win::adapterLuid(static_cast<ID3D11Device*>(device.get()))) : 0;
    } catch (const Failure&) {
        return 0;
    }
}

} // namespace rcsdk::platform
