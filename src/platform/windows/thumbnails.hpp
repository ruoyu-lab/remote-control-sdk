// Thumbnails on Windows (rcs_capture_picture): what is shared drawn small on
// the GPU and read back. The pictures come from desktop duplication
// (capture.cpp) and Windows.Graphics.Capture (window_capture.cpp).
#pragma once

#include "platform/platform.hpp"
#include "platform/windows/d3d.hpp"
#include "platform/windows/surface_shader.hpp"

#include <functional>
#include <vector>

namespace rcsdk::platform::win {

// `from` of `source` (a desktop's or a window's texture; its turns, sdrWhite
// and the visible parts as SurfaceDraw takes them), fitted into maxWidth x maxHeight
// (thumbnailSize) and read back. It is drawn at its own size into a texture
// with mip levels first and minified from those (trilinear): a single
// bilinear tap per pixel would alias at a reduction such as 12:1.
Thumbnail drawThumbnail(SurfaceShader& shader, const Device& d3d, ID3D11Texture2D* source, UINT turns, const RECT& from,
                        float sdrWhite, const std::vector<RECT>* visible, uint32_t maxWidth, uint32_t maxHeight);

// Runs `take` on a thread of its own, returning what it returned or throwing
// what it threw: a capture may attach its thread to another desktop, and
// should not depend on the caller's COM apartment.
Thumbnail onThread(const std::function<Thumbnail()>& take);

// WINDOW: one picture with Windows.Graphics.Capture (window_capture.cpp).
Thumbnail windowThumbnail(const ThumbnailRequest& request);

} // namespace rcsdk::platform::win
