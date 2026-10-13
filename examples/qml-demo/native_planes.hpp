// Wraps a decoded NV12 GPU frame as two QRhi textures (luma R8, chroma RG8)
// without copying: CVPixelBuffer planes as Metal textures on macOS, shader
// views of the D3D11 texture on Windows. The frame must live on the device
// Qt Quick renders with (see RemoteSession::setHostDevice on Windows).
#pragma once

#include <rcsdk/rcsdk.hpp>

#include <rhi/qrhi.h>

#include <QString>

#include <memory>

// One frame's textures. Keeps the frame (and its surface) alive while the GPU
// may still read it.
struct NativePlanes {
    virtual ~NativePlanes() = default;
    rcsdk::GpuFrame frame;
    std::unique_ptr<QRhiTexture> luma, chroma;
};

class PlaneImporter {
public:
    virtual ~PlaneImporter() = default;
    // Fails with a reason (format, device) instead of drawing something else.
    virtual std::unique_ptr<NativePlanes> import(const rcsdk::GpuFrame& frame, QString& error) = 0;

    static std::unique_ptr<PlaneImporter> create(QRhi* rhi);
};
