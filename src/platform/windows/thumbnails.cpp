#include "platform/windows/thumbnails.hpp"

#include "util/log.hpp"

#include <cstring>
#include <exception>
#include <optional>

namespace rcsdk::platform::win {

Thumbnail drawThumbnail(SurfaceShader& shader, const Device& d3d, ID3D11Texture2D* source, UINT turns, const RECT& from,
                        float sdrWhite, const std::vector<RECT>* visible, uint32_t maxWidth, uint32_t maxHeight) {
    constexpr rcs_result captureError = RCS_ERROR_CAPTURE;
    ID3D11Device* device = d3d.device.value;
    ID3D11DeviceContext* context = d3d.context.value;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    const auto width = uint32_t(from.right - from.left), height = uint32_t(from.bottom - from.top);
    const auto [pictureWidth, pictureHeight] = thumbnailSize(width, height, maxWidth, maxHeight);

    // What is shared at its own size (HDR made SDR, an application's covered
    // parts black), and its mip levels.
    Com<ID3D11ShaderResourceView> sourceView;
    check(device->CreateShaderResourceView(source, nullptr, sourceView.out()), "Thumbnail source view", captureError);
    auto fullDesc =
        textureDesc(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    fullDesc.MipLevels = 0; // All of them.
    fullDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    Com<ID3D11Texture2D> full;
    check(device->CreateTexture2D(&fullDesc, nullptr, full.out()), "Thumbnail texture", captureError);
    Com<ID3D11RenderTargetView> fullTarget;
    check(device->CreateRenderTargetView(full.value, nullptr, fullTarget.out()), "Thumbnail target", captureError);
    Com<ID3D11ShaderResourceView> fullView;
    check(device->CreateShaderResourceView(full.value, nullptr, fullView.out()), "Thumbnail view", captureError);
    SurfaceDraw draw;
    draw.source = sourceView.value;
    draw.sourceWidth = sourceDesc.Width;
    draw.sourceHeight = sourceDesc.Height;
    draw.turns = turns;
    draw.from = from;
    draw.sdrWhite = sdrWhite;
    draw.target = fullTarget.value;
    draw.to = {0, 0, LONG(width), LONG(height)};
    draw.visible = visible;
    shader.draw(device, context, draw);
    {
        ContextLock lock(context);
        context->GenerateMips(fullView.value);
    }

    // Minified into the picture.
    auto pictureDesc = textureDesc(pictureWidth, pictureHeight, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    Com<ID3D11Texture2D> picture;
    check(device->CreateTexture2D(&pictureDesc, nullptr, picture.out()), "Thumbnail picture", captureError);
    Com<ID3D11RenderTargetView> pictureTarget;
    check(device->CreateRenderTargetView(picture.value, nullptr, pictureTarget.out()), "Thumbnail picture target",
          captureError);
    SurfaceDraw minify;
    minify.source = fullView.value;
    minify.sourceWidth = width;
    minify.sourceHeight = height;
    minify.from = {0, 0, LONG(width), LONG(height)};
    minify.target = pictureTarget.value;
    minify.to = {0, 0, LONG(pictureWidth), LONG(pictureHeight)};
    shader.draw(device, context, minify);

    pictureDesc.Usage = D3D11_USAGE_STAGING;
    pictureDesc.BindFlags = 0;
    pictureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&pictureDesc, nullptr, staging.out()), "Thumbnail readback texture", captureError);
    Thumbnail result{pictureWidth, pictureHeight, std::vector<uint8_t>(size_t(pictureWidth) * pictureHeight * 4)};
    ContextLock lock(context);
    context->CopyResource(staging.value, picture.value);
    D3D11_MAPPED_SUBRESOURCE map{};
    check(context->Map(staging.value, 0, D3D11_MAP_READ, 0, &map), "Thumbnail readback", captureError);
    const auto* rows = static_cast<const uint8_t*>(map.pData);
    for (uint32_t y = 0; y < pictureHeight; ++y) {
        std::memcpy(result.pixels.data() + size_t(y) * pictureWidth * 4, rows + size_t(y) * map.RowPitch,
                    size_t(pictureWidth) * 4);
    }
    context->Unmap(staging.value, 0);
    return result;
}

Thumbnail onThread(const std::function<Thumbnail()>& take) {
    std::optional<Thumbnail> picture;
    std::exception_ptr failure;
    log::thread([&] {
        try {
            picture = take();
        } catch (...) {
            failure = std::current_exception();
        }
    }).join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return std::move(*picture);
}

} // namespace rcsdk::platform::win
