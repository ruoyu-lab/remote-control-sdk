// The textures a capture draws its pictures into. A picture handed out stays
// its own as long as anyone holds it (the encoder, the host's preview); the
// next one takes a pooled texture nobody holds, or a new one.
#pragma once

#include "platform/windows/d3d.hpp"

#include <cstring>
#include <memory>
#include <vector>

namespace rcsdk::platform::win {

class PicturePool {
public:
    struct Picture {
        std::shared_ptr<void> surface; // ID3D11Texture2D
        Com<ID3D11RenderTargetView> target;
    };

    // A BGRA picture of this size (the same each time) on `device`.
    Picture next(ID3D11Device* device, uint32_t width, uint32_t height) {
        Picture next;
        for (const auto& pooled : pictures_) {
            if (pooled.surface.use_count() == 1) {
                next.surface = pooled.surface;
                next.target.assign(pooled.target.value);
                return next;
            }
        }
        auto desc = textureDesc(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        Com<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, nullptr, texture.out()), "Capture texture", RCS_ERROR_CAPTURE);
        check(device->CreateRenderTargetView(texture.value, nullptr, next.target.out()), "Capture target",
              RCS_ERROR_CAPTURE);
        next.surface = share(texture.value);
        if (pictures_.size() < 4) { // Enough for the encoder, the last picture and a preview.
            Picture& pooled = pictures_.emplace_back();
            pooled.surface = next.surface;
            pooled.target.assign(next.target.value);
        }
        return next;
    }

    void clear() { pictures_.clear(); }

private:
    std::vector<Picture> pictures_;
};

// The textures a decoder hands its pictures out in, of one description: as
// above, a picture stays its own while anyone holds it (the host, the
// session's queue), and the next takes a pooled texture nobody holds, or a
// new one. A new texture costs the GPU its first use too: on an Intel GPU, a
// 4K picture drawn into a new texture took about 10 ms more.
class TexturePool {
public:
    std::shared_ptr<void> next(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc) {
        if (std::memcmp(&desc, &desc_, sizeof(desc)) != 0) {
            textures_.clear();
            desc_ = desc;
        }
        for (const auto& pooled : textures_) {
            if (pooled.use_count() == 1) {
                return pooled;
            }
        }
        Com<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, nullptr, texture.out()), "Decoded picture", RCS_ERROR_CODEC);
        auto next = share(texture.value);
        if (textures_.size() < 4) { // Enough for the host's, those queued and the next.
            textures_.push_back(next);
        }
        return next;
    }

private:
    D3D11_TEXTURE2D_DESC desc_{};
    std::vector<std::shared_ptr<void>> textures_;
};

} // namespace rcsdk::platform::win
