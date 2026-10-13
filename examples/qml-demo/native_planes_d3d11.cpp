// Windows: an NV12 ID3D11Texture2D read through two shader views, R8 for luma
// and RG8 for chroma, which D3D11 maps to the texture's two planes.
#include "native_planes.hpp"

#include <d3d11.h>

namespace {

class D3D11Importer final : public PlaneImporter {
public:
    explicit D3D11Importer(QRhi* rhi) : rhi_(rhi) {
        device_ = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles())->dev;
    }

    std::unique_ptr<NativePlanes> import(const rcsdk::GpuFrame& frame, QString& error) override {
        const auto info = frame.info();
        if (info.resource_type != RCS_GPU_D3D11_TEXTURE2D || info.format != RCS_PIXEL_NV12) {
            error = QStringLiteral("Expected an NV12 D3D11 texture");
            return nullptr;
        }
        if (info.device != device_) {
            error = QStringLiteral("The frame is not on Qt Quick's D3D11 device; pass it as "
                                   "rcs_gpu_device_config.d3d11_device (rcsdk::gpuDevice)");
            return nullptr;
        }
        auto planes = std::make_unique<NativePlanes>();
        planes->frame = frame;
        const QSize size(int(info.surface_width), int(info.surface_height));
        planes->luma.reset(rhi_->newTexture(QRhiTexture::R8, size));
        planes->chroma.reset(rhi_->newTexture(QRhiTexture::RG8, size / 2));
        const QRhiTexture::NativeTexture native{quint64(quintptr(info.resource)), 0};
        if (!planes->luma->createFrom(native) || !planes->chroma->createFrom(native)) {
            error = QStringLiteral("Could not create shader views of the NV12 texture");
            return nullptr;
        }
        return planes;
    }

private:
    QRhi* rhi_;
    void* device_ = nullptr;
};

} // namespace

std::unique_ptr<PlaneImporter> PlaneImporter::create(QRhi* rhi) {
    return rhi->backend() == QRhi::D3D11 ? std::make_unique<D3D11Importer>(rhi) : nullptr;
}
