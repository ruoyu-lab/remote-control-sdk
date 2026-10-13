// The GPU's video processor, for one conversion at a time between RGB and
// YCbCr: BGRA pictures to NV12 for the encoders, decoded NV12 to BGRA for
// hosts that read frames on the CPU. The YCbCr side has the picture's matrix
// (BT.709 or BT.601) and range; RGB is full range.
#pragma once
#include "platform/windows/d3d.hpp"

#include "media/video.hpp"

namespace rcsdk::platform::win {
class VideoProcessor {
    Com<ID3D11VideoDevice> video_;
    Com<ID3D11VideoContext> context_;
    Com<ID3D11VideoProcessorEnumerator> enumeration_;
    Com<ID3D11VideoProcessor> processor_;
    ID3D11Device* owner_ = nullptr;
    UINT inputW_ = 0, inputH_ = 0, outputW_ = 0, outputH_ = 0;

    static bool yuv(DXGI_FORMAT format) noexcept {
        return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010 || format == DXGI_FORMAT_AYUV;
    }

public:
    // Whether `device`'s video processor writes pictures of `format` (NV12,
    // P010, AYUV: drivers differ).
    static bool writes(ID3D11Device* device, DXGI_FORMAT format) {
        Com<ID3D11VideoDevice> video;
        if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(video.out())))) {
            return false;
        }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = content.OutputFrameRate = {60, 1};
        content.InputWidth = content.OutputWidth = 1920;
        content.InputHeight = content.OutputHeight = 1080;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        Com<ID3D11VideoProcessorEnumerator> enumeration;
        UINT flags = 0;
        return SUCCEEDED(video->CreateVideoProcessorEnumerator(&content, enumeration.out())) &&
               SUCCEEDED(enumeration->CheckVideoProcessorFormat(format, &flags)) &&
               (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT);
    }

    // Draws the top left `width` x `height` of `input` (its subresource, e.g.
    // a slice of a decoder's texture array) over all of `output`, one of them
    // YCbCr of `picture`'s matrix and range.
    void blit(ID3D11Device* device, ID3D11DeviceContext* immediate, ID3D11Texture2D* input, UINT subresource,
              UINT width, UINT height, ID3D11Texture2D* output, const Picture& picture) {
        constexpr rcs_result codecError = RCS_ERROR_CODEC;
        D3D11_TEXTURE2D_DESC src{}, dst{};
        input->GetDesc(&src);
        output->GetDesc(&dst);
        if (owner_ != device || inputW_ != src.Width || inputH_ != src.Height || outputW_ != dst.Width ||
            outputH_ != dst.Height) {
            processor_.reset();
            enumeration_.reset();
            video_.reset();
            context_.reset();
            check(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(video_.out())),
                  "D3D11 video device", codecError);
            check(immediate->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(context_.out())),
                  "D3D11 video context", codecError);
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputFrameRate = {60, 1};
            content.OutputFrameRate = {60, 1};
            content.InputWidth = src.Width;
            content.InputHeight = src.Height;
            content.OutputWidth = dst.Width;
            content.OutputHeight = dst.Height;
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            check(video_->CreateVideoProcessorEnumerator(&content, enumeration_.out()), "Video processor enumeration",
                  codecError);
            check(video_->CreateVideoProcessor(enumeration_.value, 0, processor_.out()), "GPU video processor",
                  codecError);
            owner_ = device;
            inputW_ = src.Width;
            inputH_ = src.Height;
            outputW_ = dst.Width;
            outputH_ = dst.Height;
        }
        Com<ID3D11VideoProcessorInputView> in;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        iv.Texture2D.MipSlice = subresource % src.MipLevels;
        iv.Texture2D.ArraySlice = subresource / src.MipLevels;
        check(video_->CreateVideoProcessorInputView(input, enumeration_.value, &iv, in.out()), "GPU video input view",
              codecError);
        Com<ID3D11VideoProcessorOutputView> out;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        check(video_->CreateVideoProcessorOutputView(output, enumeration_.value, &ov, out.out()),
              "GPU video output view", codecError);
        const RECT source{0, 0, LONG(width), LONG(height)}, whole{0, 0, LONG(dst.Width), LONG(dst.Height)};
        context_->VideoProcessorSetStreamFrameFormat(processor_.value, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        context_->VideoProcessorSetStreamAutoProcessingMode(processor_.value, 0, FALSE);
        context_->VideoProcessorSetStreamSourceRect(processor_.value, 0, TRUE, &source);
        context_->VideoProcessorSetStreamDestRect(processor_.value, 0, TRUE, &whole);
        context_->VideoProcessorSetOutputTargetRect(processor_.value, TRUE, &whole);
        // YCbCr_Matrix 1 is BT.709, 0 BT.601; RGB_Range 0 is full range.
        const auto colour = [&](DXGI_FORMAT format) {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE space{};
            space.YCbCr_Matrix = picture.matrix == RCS_MATRIX_BT601 ? 0 : 1;
            space.Nominal_Range = !yuv(format) || picture.range == RCS_RANGE_FULL
                                      ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                                      : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            return space;
        };
        const auto inputColor = colour(src.Format), outputColor = colour(dst.Format);
        context_->VideoProcessorSetStreamColorSpace(processor_.value, 0, &inputColor);
        context_->VideoProcessorSetOutputColorSpace(processor_.value, &outputColor);
        D3D11_VIDEO_COLOR black{};
        black.RGBA.A = 1;
        context_->VideoProcessorSetOutputBackgroundColor(processor_.value, FALSE, &black);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = in.value;
        check(context_->VideoProcessorBlt(processor_.value, out.value, 0, 1, &stream), "GPU scale/color conversion",
              codecError);
    }
};
} // namespace rcsdk::platform::win
