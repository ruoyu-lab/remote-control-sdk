// The capture compositor: scales what is shared (the desktop, a part of it, a
// window) into the picture and draws the pointer, on the GPU. An HDR desktop
// (scRGB, FP16) becomes SDR: SDR white, the display's "SDR content
// brightness", maps to white, and what lies above it (HDR highlights) is
// clipped (HDR streaming is outside this format contract). BGRA sources pass
// through without transfer.
//
// A turned display's duplicated image is its panel's, unrotated: the W x H
// desktop of a display turned a quarter clockwise (DXGI_MODE_ROTATION_ROTATE90)
// comes as an H x W texture, the desktop's top edge up its left side. The draw
// works in the desktop's orientation and samples the texel each place shows,
// as Microsoft's desktop duplication sample maps them: desktop (x, y) is
// texel (y, W - x) a quarter turned, (W - x, H - y) half, (H - y, x) three
// quarters.
#pragma once
#include "platform/windows/d3d.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
namespace rcsdk::platform::win {

// One picture: `from` of the source scaled into `to` of the target, black
// around it; with `visible`, only those parts of the source show (black
// elsewhere); the pointer over it. Places in the source are its pixels, in
// its picture's orientation: with `turns`, the desktop's, the source being
// the display's unrotated image of sourceWidth x sourceHeight.
struct SurfaceDraw {
    ID3D11ShaderResourceView* source = nullptr;
    UINT sourceWidth = 0, sourceHeight = 0;
    UINT turns = 0; // Quarter turns clockwise the display is turned (DXGI_MODE_ROTATION_ROTATE90: 1), 0..3.
    RECT from{};
    float sdrWhite = 0; // HDR: the scRGB value of SDR white (1 is 80 nits); 0 for BGRA.
    ID3D11RenderTargetView* target = nullptr;
    RECT to{};
    const std::vector<RECT>* visible = nullptr; // Null: all of `from`.
    ID3D11ShaderResourceView* pointer = nullptr;
    UINT pointerType = 0; // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_*; 0: none.
    RECT pointerRect{};
};

class SurfaceShader {
    // The pixel shader's constants (cbuffer Flags), in whole 16-byte registers.
    struct Flags {
        float white;
        UINT type, w, h;
        float x, y, screenW, screenH;
        float fromX, fromY, fromW, fromH;
        UINT turns, padding[3];
    };
    static_assert(sizeof(Flags) % 16 == 0);

    Com<ID3D11VertexShader> vertex_;
    Com<ID3D11PixelShader> pixel_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11Buffer> constants_;
    Com<ID3D11RasterizerState> scissored_;
    ID3D11Device* owner_ = nullptr;

public:
    void draw(ID3D11Device* device, ID3D11DeviceContext* context, const SurfaceDraw& d) {
        constexpr rcs_result captureError = RCS_ERROR_CAPTURE;
        ContextLock transaction(context);
        if (owner_ != device) {
            vertex_.reset();
            pixel_.reset();
            sampler_.reset();
            constants_.reset();
            scissored_.reset();
            constexpr auto shader = R"(
Texture2D<float4> image : register(t0);SamplerState sampling : register(s0);Texture2D<float4> pointerImage : register(t1);cbuffer Flags : register(b0){float white;uint pointerType;uint pointerW;uint pointerH;float pointerX;float pointerY;float screenW;float screenH;float fromX;float fromY;float fromW;float fromH;uint turns;};
struct V{float4 position:SV_Position;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID){V v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float3 srgb(float3 x){x=saturate(x);return float3(x.r<=.0031308?12.92*x.r:1.055*pow(x.r,1/2.4)-.055,x.g<=.0031308?12.92*x.g:1.055*pow(x.g,1/2.4)-.055,x.b<=.0031308?12.92*x.b:1.055*pow(x.b,1/2.4)-.055);}
float4 ps(V v):SV_Target{float2 pixel=float2(fromX,fromY)+v.uv*float2(fromW,fromH);
float2 texel=turns==1?float2(pixel.y,screenH-pixel.x):turns==2?float2(screenW-pixel.x,screenH-pixel.y):turns==3?float2(screenW-pixel.y,pixel.x):pixel;
float3 c=image.Sample(sampling,texel/float2(screenW,screenH)).rgb;c=white>0?srgb(c/white):c;
float2 cursorPixel=pixel-float2(pointerX,pointerY);
if(pointerType&&all(cursorPixel>=0)&&cursorPixel.x<pointerW&&cursorPixel.y<pointerH){float4 p=pointerImage.Load(int3(int2(cursorPixel),0));
if(pointerType==1)c=(p.r>.5?c:float3(0,0,0));if(pointerType==1&&p.g>.5)c=1-c;
if(pointerType==2)c=p.rgb*p.a+c*(1-p.a);
if(pointerType==4)c=p.a>.5?float3(uint3(round(saturate(c)*255))^uint3(round(p.rgb*255)))/255:p.rgb;
}return float4(c,1);}
)";
            Com<ID3DBlob> v, p, error;
            const auto compile = [&](const char* entry, const char* target, Com<ID3DBlob>& code) {
                if (FAILED(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, entry, target, 0, 0,
                                      code.out(), error.out()))) {
                    fail(captureError, std::string("Compiling the capture shader failed: ") +
                                           (error ? static_cast<const char*>(error->GetBufferPointer())
                                                  : "no compiler output"));
                }
            };
            compile("vs", "vs_5_0", v);
            compile("ps", "ps_5_0", p);
            check(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, vertex_.out()),
                  "Capture vertex pipeline", captureError);
            check(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, pixel_.out()),
                  "Capture pixel pipeline", captureError);
            D3D11_SAMPLER_DESC sample{};
            sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sample.MaxLOD = D3D11_FLOAT32_MAX;
            check(device->CreateSamplerState(&sample, sampler_.out()), "Capture scaling sampler", captureError);
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            raster.ScissorEnable = TRUE;
            check(device->CreateRasterizerState(&raster, scissored_.out()), "Capture mask", captureError);
            D3D11_BUFFER_DESC b{};
            b.ByteWidth = sizeof(Flags);
            b.Usage = D3D11_USAGE_DEFAULT;
            b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            check(device->CreateBuffer(&b, nullptr, constants_.out()), "Capture shader constants", captureError);
            owner_ = device;
        }
        const Flags flags{d.sdrWhite,
                          d.pointer ? d.pointerType : 0u,
                          UINT(d.pointerRect.right - d.pointerRect.left),
                          UINT(d.pointerRect.bottom - d.pointerRect.top),
                          float(d.pointerRect.left),
                          float(d.pointerRect.top),
                          float(d.sourceWidth),
                          float(d.sourceHeight),
                          float(d.from.left),
                          float(d.from.top),
                          float(d.from.right - d.from.left),
                          float(d.from.bottom - d.from.top),
                          d.turns % 4,
                          {}};
        context->UpdateSubresource(constants_.value, 0, nullptr, &flags, 0, 0);
        const float black[4]{0, 0, 0, 1};
        ID3D11RenderTargetView* target = d.target;
        context->ClearRenderTargetView(target, black);
        context->OMSetRenderTargets(1, &target, nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        const RECT& to = d.to;
        D3D11_VIEWPORT viewport{
            float(to.left), float(to.top), float(to.right - to.left), float(to.bottom - to.top), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(d.visible ? scissored_.value : nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->GSSetShader(nullptr, nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->VSSetShader(vertex_.value, nullptr, 0);
        context->PSSetShader(pixel_.value, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &constants_.value);
        context->PSSetSamplers(0, 1, &sampler_.value);
        ID3D11ShaderResourceView* views[]{d.source, d.pointer};
        context->PSSetShaderResources(0, 2, views);
        if (!d.visible) {
            context->Draw(3, 0);
        } else {
            // Each visible part, in the target's pixels (rounded outward: no
            // seams between parts), clips one draw.
            const double scaleX = double(to.right - to.left) / std::max<LONG>(1, d.from.right - d.from.left);
            const double scaleY = double(to.bottom - to.top) / std::max<LONG>(1, d.from.bottom - d.from.top);
            for (const RECT& part : *d.visible) {
                const D3D11_RECT clip{LONG(std::floor(to.left + (part.left - d.from.left) * scaleX)),
                                      LONG(std::floor(to.top + (part.top - d.from.top) * scaleY)),
                                      LONG(std::ceil(to.left + (part.right - d.from.left) * scaleX)),
                                      LONG(std::ceil(to.top + (part.bottom - d.from.top) * scaleY))};
                context->RSSetScissorRects(1, &clip);
                context->Draw(3, 0);
            }
            context->RSSetState(nullptr);
        }
        ID3D11ShaderResourceView* nullViews[2]{};
        context->PSSetShaderResources(0, 2, nullViews);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
};
} // namespace rcsdk::platform::win
