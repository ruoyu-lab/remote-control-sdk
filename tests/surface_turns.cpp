// Windows: the capture compositor draws a turned display's panel image
// upright. For each of the four turns, the panel image desktop duplication
// would hand out (support/panel_image.hpp, from Microsoft's documentation and
// sample, not from the shader) of a desktop with four coloured quadrants goes
// through the shader; the picture read back must show the desktop as it is,
// the pointer upright where it is on the desktop, and a region of it.
#include "platform/windows/surface_shader.hpp"
#include "support/check.hpp"
#include "support/panel_image.hpp"

#include <cstdio>
#include <cstring>

using namespace rcsdk;
using platform::win::Com;

namespace {

constexpr uint32_t width = 64, height = 48; // The desktop: wider than high, so that a wrong turn shows.
// BGRA as one value, B in the low byte.
constexpr uint32_t red = 0xFFFF0000u, green = 0xFF00FF00u, blue = 0xFF0000FFu, white = 0xFFFFFFFFu;
constexpr uint32_t magenta = 0xFFFF00FFu, cyan = 0xFF00FFFFu;

uint32_t desktopPixel(uint32_t x, uint32_t y) {
    const bool right = x >= width / 2, lower = y >= height / 2;
    return lower ? (right ? white : blue) : (right ? green : red);
}

Com<ID3D11ShaderResourceView> textureView(ID3D11Device* device, const std::vector<uint32_t>& pixels, uint32_t w,
                                          uint32_t h) {
    auto desc = platform::win::textureDesc(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), w * 4, 0};
    Com<ID3D11Texture2D> texture;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc, &data, texture.out())));
    Com<ID3D11ShaderResourceView> view;
    CHECK(SUCCEEDED(device->CreateShaderResourceView(texture.value, nullptr, view.out())));
    return view;
}

// Draws `draw` into a desktop-sized picture and reads it back.
std::vector<uint32_t> render(platform::win::Device& d3d, platform::win::SurfaceShader& shader,
                             platform::win::SurfaceDraw draw) {
    auto desc = platform::win::textureDesc(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    Com<ID3D11Texture2D> target;
    CHECK(SUCCEEDED(d3d.device->CreateTexture2D(&desc, nullptr, target.out())));
    Com<ID3D11RenderTargetView> targetView;
    CHECK(SUCCEEDED(d3d.device->CreateRenderTargetView(target.value, nullptr, targetView.out())));
    draw.target = targetView.value;
    draw.to = {0, 0, LONG(width), LONG(height)};
    shader.draw(d3d.device.value, d3d.context.value, draw);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    CHECK(SUCCEEDED(d3d.device->CreateTexture2D(&desc, nullptr, staging.out())));
    d3d.context->CopyResource(staging.value, target.value);
    D3D11_MAPPED_SUBRESOURCE map{};
    CHECK(SUCCEEDED(d3d.context->Map(staging.value, 0, D3D11_MAP_READ, 0, &map)));
    std::vector<uint32_t> picture(size_t(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(picture.data() + size_t(y) * width,
                    static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, width * 4);
    }
    d3d.context->Unmap(staging.value, 0);
    return picture;
}

void expect(const std::vector<uint32_t>& picture, uint32_t x, uint32_t y, uint32_t colour, uint32_t turns,
            const char* what) {
    const uint32_t got = picture[size_t(y) * width + x];
    if (got != colour) {
        std::fprintf(stderr, "%u quarter turns, %s: pixel (%u, %u) is %08X, not %08X\n", turns, what, x, y, got,
                     colour);
        CHECK(false);
    }
}

} // namespace

int main() {
    return runTest("surface_turns", [] {
        platform::win::Device d3d;
        try {
            d3d.create();
        } catch (const Failure&) {
            std::printf("surface_turns: no hardware D3D11 device; skipped\n");
            std::exit(77);
        }
        std::vector<uint32_t> desktop(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                desktop[size_t(y) * width + x] = desktopPixel(x, y);
            }
        }
        // The pointer: 4x4, magenta on its left half and cyan on its right, so
        // that a pointer drawn turned shows.
        const std::vector<uint32_t> pointer{magenta, magenta, cyan, cyan, magenta, magenta, cyan, cyan,
                                            magenta, magenta, cyan, cyan, magenta, magenta, cyan, cyan};
        platform::win::SurfaceShader shader;
        const auto pointerView = textureView(d3d.device.value, pointer, 4, 4);
        for (uint32_t turns = 0; turns < 4; ++turns) {
            const auto [panelWidth, panelHeight] = test::panelSize(turns, width, height);
            const auto panel =
                textureView(d3d.device.value, test::panelImage(turns, desktop, width, height), panelWidth, panelHeight);
            platform::win::SurfaceDraw draw;
            draw.source = panel.value;
            draw.sourceWidth = panelWidth;
            draw.sourceHeight = panelHeight;
            draw.turns = turns;
            draw.from = {0, 0, LONG(width), LONG(height)};
            // The whole desktop, upright: every pixel as the desktop has it.
            const auto upright = render(d3d, shader, draw);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    expect(upright, x, y, desktopPixel(x, y), turns, "the desktop");
                }
            }
            // The pointer where the desktop has it, upright.
            draw.pointer = pointerView.value;
            draw.pointerType = 2; // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR
            draw.pointerRect = {8, 8, 12, 12};
            const auto pointed = render(d3d, shader, draw);
            for (uint32_t y = 8; y < 12; ++y) {
                expect(pointed, 8, y, magenta, turns, "the pointer's left");
                expect(pointed, 11, y, cyan, turns, "the pointer's right");
            }
            expect(pointed, 12, 8, red, turns, "beside the pointer");
            // A region: the desktop's top right quarter fills the picture.
            draw.pointer = nullptr;
            draw.pointerType = 0;
            draw.from = {LONG(width / 2), 0, LONG(width), LONG(height / 2)};
            const auto region = render(d3d, shader, draw);
            for (const auto& [x, y] : {std::pair{1u, 1u}, std::pair{width - 2, 1u}, std::pair{1u, height - 2},
                                       std::pair{width - 2, height - 2}, std::pair{width / 2, height / 2}}) {
                expect(region, x, y, green, turns, "the top right region");
            }
        }
    });
}
