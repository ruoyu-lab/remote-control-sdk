// Decoded YCbCr 4:4:4 pictures to BGRA8 on the GPU, with the stream's matrix
// and range (H.273's quantisation, as media/yuv.hpp reads it): the GPU
// decoders' 4:4:4, which hosts get as Windows' software decoders' 4:4:4
// pictures come (Direct3D 11 has no 4:4:4 YCbCr format a host could sample).
// The sources, each read through a shader-resource view:
//   planes8   NVDEC's 8-bit: Y, Cb and Cr in the three layers of an R8 texture array.
//   planes16  NVDEC's 10-bit: the same in R16, the value in the top 10 bits.
//   ayuv      oneVPL's 8-bit: DXGI_FORMAT_AYUV read as R8G8B8A8 (Cr, Cb, Y, A).
//   y410      oneVPL's 10-bit: DXGI_FORMAT_Y410 read as R10G10B10A2 (Cb, Y, Cr, A).
#pragma once
#include "platform/windows/d3d.hpp"

#include "media/video.hpp"

#include <d3dcompiler.h>

#include <cstring>

namespace rcsdk::platform::win {

class BgraShader {
public:
    enum class Source { planes8, planes16, ayuv, y410 };

    // The view format a packed source's texture is read through.
    static DXGI_FORMAT viewFormat(Source source) noexcept {
        return source == Source::ayuv ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM;
    }

    // Draws `source` (a view of a picture of `layout`) into the top left
    // `width` x `height` of `target` (a BGRA8 render target).
    void draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, Source layout,
              ID3D11Texture2D* target, UINT width, UINT height, const Picture& picture) {
        constexpr rcs_result codecError = RCS_ERROR_CODEC;
        ContextLock transaction(context);
        if (owner_ != device) {
            constexpr auto shader = R"(
Texture2DArray<float> planes : register(t0);Texture2D<float4> packed : register(t1);
cbuffer Colour : register(b0){float4 r;float4 g;float4 b;float4 offset;};
float4 vs(uint id:SV_VertexID):SV_Position{float2 at=float2((id<<1)&2,id&2);return float4(at*float2(2,-2)+float2(-1,1),0,1);}
float4 rgb(float3 s){float3 c=s*offset.w-offset.xyz;return float4(saturate(float3(dot(r.xyz,c),dot(g.xyz,c),dot(b.xyz,c))),1);}
float4 fromPlanes(float4 at:SV_Position):SV_Target{int2 p=int2(at.xy);
return rgb(float3(planes.Load(int4(p,0,0)),planes.Load(int4(p,1,0)),planes.Load(int4(p,2,0))));}
float4 fromPacked(float4 at:SV_Position):SV_Target{return rgb(packed.Load(int3(at.xy,0)).rgb);}
)";
            const auto compile = [&](const char* entry, const char* model) {
                Com<ID3DBlob> code, error;
                if (FAILED(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, entry, model, 0, 0, code.out(),
                                      error.out()))) {
                    fail(codecError,
                         std::string("Compiling the YCbCr 4:4:4 shader failed: ") +
                             (error ? static_cast<const char*>(error->GetBufferPointer()) : "no compiler output"));
                }
                return code;
            };
            const auto vertex = compile("vs", "vs_5_0"), planes = compile("fromPlanes", "ps_5_0"),
                       packed = compile("fromPacked", "ps_5_0");
            check(
                device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, vertex_.out()),
                "YCbCr 4:4:4 vertex shader", codecError);
            check(
                device->CreatePixelShader(planes->GetBufferPointer(), planes->GetBufferSize(), nullptr, planes_.out()),
                "YCbCr 4:4:4 pixel shader", codecError);
            check(
                device->CreatePixelShader(packed->GetBufferPointer(), packed->GetBufferSize(), nullptr, packed_.out()),
                "YCbCr 4:4:4 pixel shader", codecError);
            D3D11_BUFFER_DESC b{};
            b.ByteWidth = sizeof(Colour);
            b.Usage = D3D11_USAGE_DEFAULT;
            b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            check(device->CreateBuffer(&b, nullptr, constants_.out()), "YCbCr 4:4:4 shader constants", codecError);
            owner_ = device;
        }
        Com<ID3D11RenderTargetView> output;
        check(device->CreateRenderTargetView(target, nullptr, output.out()), "Decoded picture view", codecError);
        const Colour constants = colour(picture, layout);
        context->UpdateSubresource(constants_.value, 0, nullptr, &constants, 0, 0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_.value, nullptr, 0);
        const bool planar = layout == Source::planes8 || layout == Source::planes16;
        context->PSSetShader(planar ? planes_.value : packed_.value, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &constants_.value);
        context->PSSetShaderResources(planar ? 0 : 1, 1, &source);
        context->RSSetState(nullptr);
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        context->OMSetRenderTargets(1, &output.value, nullptr);
        context->Draw(3, 0);
        ID3D11ShaderResourceView* noSource = nullptr;
        context->PSSetShaderResources(planar ? 0 : 1, 1, &noSource);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

private:
    // The shader's constants (cbuffer Colour): R, G and B each as weights of
    // the three samples read less their offsets, in code values; the offsets,
    // and what turns a sample read into a code value.
    struct Colour {
        float r[4], g[4], b[4], offset[4];
    };
    static_assert(sizeof(Colour) % 16 == 0);

    // RGB in 0..1 from YCbCr of `picture`'s matrix and range, its samples
    // where `layout` keeps them.
    static Colour colour(const Picture& picture, Source layout) {
        const double kr = picture.matrix == RCS_MATRIX_BT601    ? 0.299
                          : picture.matrix == RCS_MATRIX_BT2020 ? 0.2627
                                                                : 0.2126;
        const double kb = picture.matrix == RCS_MATRIX_BT601    ? 0.114
                          : picture.matrix == RCS_MATRIX_BT2020 ? 0.0593
                                                                : 0.0722;
        const double kg = 1 - kr - kb;
        const bool wide = layout == Source::planes16 || layout == Source::y410;
        const unsigned bits = wide ? 10 : 8;
        const double unit = double(1u << (bits - 8)), top = double((1u << bits) - 1);
        const bool limited = picture.range != RCS_RANGE_FULL;
        const double luma = limited ? 219 * unit : top, chroma = limited ? 224 * unit : top;
        // A UNORM sample to a code value.
        const double scale = layout == Source::planes16 ? 65535.0 / 64 : top;
        // Which of the samples read holds Y, Cb and Cr.
        static constexpr int ayuv[3]{2, 1, 0}, y410[3]{1, 0, 2}, planes[3]{0, 1, 2};
        const int* at = layout == Source::ayuv ? ayuv : layout == Source::y410 ? y410 : planes;
        const double rows[3][3]{{1 / luma, 0, 2 * (1 - kr) / chroma},
                                {1 / luma, -2 * kb * (1 - kb) / kg / chroma, -2 * kr * (1 - kr) / kg / chroma},
                                {1 / luma, 2 * (1 - kb) / chroma, 0}};
        const double offsets[3]{limited ? 16 * unit : 0, 128 * unit, 128 * unit};
        Colour c{};
        float* out[3]{c.r, c.g, c.b};
        for (int component = 0; component < 3; ++component) { // Y, Cb, Cr.
            for (int row = 0; row < 3; ++row) {
                out[row][at[component]] = float(rows[row][component]);
            }
            c.offset[at[component]] = float(offsets[component]);
        }
        c.offset[3] = float(scale);
        return c;
    }

    Com<ID3D11VertexShader> vertex_;
    Com<ID3D11PixelShader> planes_, packed_;
    Com<ID3D11Buffer> constants_;
    ID3D11Device* owner_ = nullptr;
};

} // namespace rcsdk::platform::win
