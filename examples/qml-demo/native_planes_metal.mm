// macOS: CVPixelBuffer planes as Metal textures through a CVMetalTextureCache.
#include "native_planes.hpp"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

namespace {

struct MetalPlanes final : NativePlanes {
    CVMetalTextureRef lumaTexture = nullptr, chromaTexture = nullptr;
    ~MetalPlanes() override {
        luma.reset();
        chroma.reset();
        if (lumaTexture) {
            CFRelease(lumaTexture);
        }
        if (chromaTexture) {
            CFRelease(chromaTexture);
        }
    }
};

class MetalImporter final : public PlaneImporter {
  public:
    explicit MetalImporter(QRhi* rhi) : rhi_(rhi) {
        const auto* handles = static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
        // Qt declares the device as MTLDevice*; Metal uses id<MTLDevice>.
        id<MTLDevice> device = (__bridge id<MTLDevice>)(__bridge void*)handles->dev;
        CVMetalTextureCacheCreate(nullptr, nullptr, device, nullptr, &cache_);
    }
    ~MetalImporter() override {
        if (cache_) {
            CFRelease(cache_);
        }
    }

    std::unique_ptr<NativePlanes> import(const rcsdk::GpuFrame& frame, QString& error) override {
        const auto info = frame.info();
        auto buffer = static_cast<CVPixelBufferRef>(info.resource);
        if (info.resource_type != RCS_GPU_CVPIXELBUFFER || info.format != RCS_PIXEL_NV12 ||
            CVPixelBufferGetPlaneCount(buffer) != 2) {
            error = QStringLiteral("Expected an NV12 CVPixelBuffer");
            return nullptr;
        }
        auto planes = std::make_unique<MetalPlanes>();
        planes->frame = frame;
        planes->luma.reset(plane(buffer, 0, MTLPixelFormatR8Unorm, QRhiTexture::R8, planes->lumaTexture));
        planes->chroma.reset(plane(buffer, 1, MTLPixelFormatRG8Unorm, QRhiTexture::RG8, planes->chromaTexture));
        if (!planes->luma || !planes->chroma) {
            error = QStringLiteral("Could not map the pixel buffer as Metal textures");
            return nullptr;
        }
        return planes;
    }

  private:
    QRhiTexture* plane(CVPixelBufferRef buffer, size_t index, MTLPixelFormat metal, QRhiTexture::Format format,
                       CVMetalTextureRef& owner) {
        const size_t width = CVPixelBufferGetWidthOfPlane(buffer, index);
        const size_t height = CVPixelBufferGetHeightOfPlane(buffer, index);
        if (CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, buffer, nullptr, metal, width, height, index,
                                                      &owner) != kCVReturnSuccess) {
            return nullptr;
        }
        id<MTLTexture> texture = CVMetalTextureGetTexture(owner);
        auto* wrapped = rhi_->newTexture(format, QSize(int(width), int(height)));
        if (!wrapped->createFrom({quint64(quintptr((__bridge void*)texture)), 0})) {
            delete wrapped;
            return nullptr;
        }
        return wrapped;
    }

    QRhi* rhi_;
    CVMetalTextureCacheRef cache_ = nullptr;
};

} // namespace

std::unique_ptr<PlaneImporter> PlaneImporter::create(QRhi* rhi) {
    return rhi->backend() == QRhi::Metal ? std::make_unique<MetalImporter>(rhi) : nullptr;
}
