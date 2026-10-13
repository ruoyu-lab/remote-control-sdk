// BGRA pictures to the YCbCr layouts video processors need not write, on the
// GPU, with the picture's matrix (BT.709 or BT.601) and range, scaled to the
// target as the video processor scales:
//   AYUV      8-bit 4:4:4 packed (DXGI_FORMAT_AYUV, the bytes Cr, Cb, Y, A):
//             NVENC's and Media Foundation's 4:4:4 input. An RTX 4090's video
//             processor does not write it.
//   Y410      10-bit 4:4:4 packed (DXGI_FORMAT_Y410, a word of Cb, Y, Cr and
//             A): Media Foundation's 10-bit 4:4:4 input. No GPU tried takes
//             it as a render target.
// A compute shader writes each pixel's word through an unordered-access view.
#pragma once
#include "platform/windows/d3d.hpp"

#include "media/video.hpp"

#include <d3dcompiler.h>

#include <cstring>

namespace rcsdk::platform::win {

class YuvShader {
public:
    enum class Layout : UINT { ayuv = 0, y410 = 1 };

    static DXGI_FORMAT textureFormat(Layout layout) noexcept {
        return layout == Layout::ayuv ? DXGI_FORMAT_AYUV : DXGI_FORMAT_Y410;
    }

    // Whether `device` stores to textures of `layout` from a shader.
    static bool supported(ID3D11Device* device, Layout layout) {
        UINT support = 0;
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 stores{textureFormat(layout), 0};
        return SUCCEEDED(device->CheckFormatSupport(textureFormat(layout), &support)) &&
               (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) &&
               SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &stores, sizeof(stores))) &&
               (stores.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE);
    }

    // A texture for `width` x `height` pictures of `layout`.
    static D3D11_TEXTURE2D_DESC targetDesc(Layout layout, UINT width, UINT height) {
        return textureDesc(width, height, textureFormat(layout), D3D11_BIND_UNORDERED_ACCESS);
    }

    // Draws the top left `width` x `height` of `input` (BGRA) over all of
    // `output` (a targetDesc texture of `layout`).
    void draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* input, UINT width, UINT height,
              ID3D11Texture2D* output, Layout layout, const Picture& picture) {
        constexpr rcs_result codecError = RCS_ERROR_CODEC;
        ContextLock transaction(context);
        if (owner_ != device) {
            compute_.reset();
            sampler_.reset();
            constants_.reset();
            copy_.reset();
            constexpr auto shader = R"(
Texture2D<float4> image : register(t0);SamplerState sampling : register(s0);RWTexture2D<uint> target : register(u0);
cbuffer Rows : register(b0){float4 yRow;float4 uRow;float4 vRow;float4 part;uint layout;uint width;uint height;uint top;};
[numthreads(8,8,1)]void cs(uint3 id:SV_DispatchThreadID){if(id.x>=width||id.y>=height)return;
float3 c=saturate(image.SampleLevel(sampling,(float2(id.xy)+.5)/float2(width,height)*part.xy,0).rgb);
uint y=(uint)clamp(round(dot(yRow.rgb,c)+yRow.a),0,top),u=(uint)clamp(round(dot(uRow.rgb,c)+uRow.a),0,top),v=(uint)clamp(round(dot(vRow.rgb,c)+vRow.a),0,top);
target[id.xy]=layout==0?v|u<<8|y<<16|255u<<24:u|y<<10|v<<20|3u<<30;}
)";
            Com<ID3DBlob> code, error;
            if (FAILED(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "cs", "cs_5_0", 0, 0, code.out(),
                                  error.out()))) {
                fail(codecError, std::string("Compiling the YCbCr conversion shader failed: ") +
                                     (error ? static_cast<const char*>(error->GetBufferPointer()) : "no compiler output"));
            }
            check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, compute_.out()),
                  "YCbCr conversion pipeline", codecError);
            D3D11_SAMPLER_DESC sample{};
            sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sample.MaxLOD = D3D11_FLOAT32_MAX;
            check(device->CreateSamplerState(&sample, sampler_.out()), "YCbCr scaling sampler", codecError);
            D3D11_BUFFER_DESC b{};
            b.ByteWidth = sizeof(Rows);
            b.Usage = D3D11_USAGE_DEFAULT;
            b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            check(device->CreateBuffer(&b, nullptr, constants_.out()), "YCbCr shader constants", codecError);
            owner_ = device;
        }
        D3D11_TEXTURE2D_DESC src{}, dst{};
        input->GetDesc(&src);
        output->GetDesc(&dst);
        ID3D11Texture2D* readable = input;
        if (!(src.BindFlags & D3D11_BIND_SHADER_RESOURCE)) { // E.g. a host's texture.
            D3D11_TEXTURE2D_DESC have{};
            if (copy_) {
                copy_->GetDesc(&have);
            }
            if (!copy_ || have.Width != src.Width || have.Height != src.Height || have.Format != src.Format) {
                copy_.reset();
                auto desc = textureDesc(src.Width, src.Height, src.Format, D3D11_BIND_SHADER_RESOURCE);
                check(device->CreateTexture2D(&desc, nullptr, copy_.out()), "YCbCr source copy", codecError);
            }
            context->CopySubresourceRegion(copy_.value, 0, 0, 0, 0, input, 0, nullptr);
            readable = copy_.value;
        }
        Com<ID3D11ShaderResourceView> source;
        check(device->CreateShaderResourceView(readable, nullptr, source.out()), "YCbCr source view", codecError);
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_UINT; // The pixel's word.
        uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        Com<ID3D11UnorderedAccessView> target;
        check(device->CreateUnorderedAccessView(output, &uv, target.out()), "YCbCr target view", codecError);

        const Rows constants = rows(picture, layout == Layout::ayuv ? 8 : 10, float(width) / float(src.Width),
                                    float(height) / float(src.Height), layout, dst.Width, dst.Height);
        context->UpdateSubresource(constants_.value, 0, nullptr, &constants, 0, 0);
        context->CSSetShader(compute_.value, nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &constants_.value);
        context->CSSetSamplers(0, 1, &sampler_.value);
        context->CSSetShaderResources(0, 1, &source.value);
        context->CSSetUnorderedAccessViews(0, 1, &target.value, nullptr);
        context->Dispatch((dst.Width + 7) / 8, (dst.Height + 7) / 8, 1);
        ID3D11ShaderResourceView* noSource = nullptr;
        ID3D11UnorderedAccessView* noTarget = nullptr;
        context->CSSetShaderResources(0, 1, &noSource);
        context->CSSetUnorderedAccessViews(0, 1, &noTarget, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }

private:
    // The shader's constants (cbuffer Rows): Y, Cb and Cr each as weights of
    // R, G, B and an offset, in code values of `bits`; the part of the
    // source drawn; the layout, the picture's size and the largest code.
    struct Rows {
        float y[4], u[4], v[4];
        float part[4];
        UINT layout, width, height, top;
    };
    static_assert(sizeof(Rows) % 16 == 0);

    // Y' and C' of `picture`'s matrix and range from full-range RGB in 0..1,
    // as media/yuv.hpp reads them back (H.273's quantisation).
    static Rows rows(const Picture& picture, unsigned bits, float partW, float partH, Layout layout, UINT width,
                     UINT height) {
        const double kr = picture.matrix == RCS_MATRIX_BT601 ? 0.299 : 0.2126;
        const double kb = picture.matrix == RCS_MATRIX_BT601 ? 0.114 : 0.0722;
        const double kg = 1 - kr - kb;
        const double unit = double(1u << (bits - 8)), top = double((1u << bits) - 1);
        const bool limited = picture.range != RCS_RANGE_FULL;
        const double luma = limited ? 219 * unit : top, chroma = limited ? 224 * unit : top;
        const double yOffset = limited ? 16 * unit : 0, half = 128 * unit;
        const double cb = chroma / (2 * (1 - kb)), cr = chroma / (2 * (1 - kr));
        return {{float(kr * luma), float(kg * luma), float(kb * luma), float(yOffset)},
                {float(-kr * cb), float(-kg * cb), float((1 - kb) * cb), float(half)},
                {float((1 - kr) * cr), float(-kg * cr), float(-kb * cr), float(half)},
                {partW, partH, 0, 0},
                UINT(layout),
                width,
                height,
                UINT(top)};
    }

    Com<ID3D11ComputeShader> compute_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11Buffer> constants_;
    Com<ID3D11Texture2D> copy_; // Sources the shader cannot read, copied.
    ID3D11Device* owner_ = nullptr;
};
} // namespace rcsdk::platform::win
