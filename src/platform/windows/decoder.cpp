// H.264, HEVC, VP8, VP9 and AV1 decoding with the system's Media Foundation
// decoders (the others than H.264 come with Windows' Video Extensions). Hardware
// acceleration gives them a D3D11 device manager: they decode on the GPU and
// return NV12 textures (P010 for 10-bit pictures), converted to BGRA on the
// GPU when the host reads frames on the CPU. Software acceleration decodes on
// the CPU and returns BGRA pixels, converted there (uploaded as BGRA8
// textures for hosts that take GPU frames); HEVC 4:4:4 too, which
// the HEVC decoder gives as AYUV (Y410 at 10 bits). On the GPU it takes 4:4:4
// pictures and returns none (RTX 4090 and an Intel GPU, even when drained,
// though both GPUs list the range extensions' D3D11 profiles), so hardware
// decoding is 4:2:0 only here; NVDEC decodes 4:4:4 on NVIDIA GPUs (nvdec.hpp).
//
// The stream announces its pictures' size; the decoder's own description
// checks it: its pictures' size where it states one (the minimum display
// aperture), else the size of its buffers, which the announced size must fit.
// A frame is never read beyond what the decoder produced; a stream that
// claims otherwise is a decoder error.
#include "platform/windows/media_foundation.hpp"
#include "platform/windows/nvdec.hpp"
#include "platform/windows/onevpl.hpp"
#include "platform/windows/picture_pool.hpp"
#include "platform/windows/video_processor.hpp"

#include "media/yuv.hpp"
#include "platform/platform.hpp"

#include <codecapi.h>

#include <deque>
#include <optional>

namespace rcsdk::platform {
namespace {

using win::Com;

constexpr rcs_result codecError = RCS_ERROR_CODEC;

class MediaFoundationDecoder final : public VideoDecoder {
public:
    ~MediaFoundationDecoder() override {
        for (auto& copy : retiring_) {
            copy.wait(2000);
        }
    }

    // Takes the device and the transform now: a codec or acceleration this
    // system lacks fails here, not at the first packet.
    void open(const DecoderConfig& config) override {
        win::subtype(config.codec); // Rejects codecs this backend does not implement.
        config_ = config;
        mediaFoundation_.emplace();
        if (config.acceleration == RCS_ACCELERATION_HARDWARE) {
            if (config.device) {
                d3d_.adopt(static_cast<ID3D11Device*>(config.device.get()));
            } else {
                d3d_.create();
            }
            if (chroma444(config.picture.format)) {
                fail(RCS_ERROR_UNSUPPORTED, std::string("Windows' ") + codecName(config.codec) +
                                                " decoder decodes no 4:4:4 pictures on the GPU (its HEVC decoder "
                                                "takes them and returns none); NVDEC does on NVIDIA GPUs that "
                                                "decode them (RCS_BACKEND_NVDEC), software acceleration decodes "
                                                "HEVC 4:4:4");
            }
            if (!(win::gpuDecodedFormats(d3d_.device.value, config.codec) & (1u << config.picture.format))) {
                fail(RCS_ERROR_UNSUPPORTED, std::string("This GPU does not decode ") + codecName(config.codec) +
                                                (config.picture.format == RCS_PICTURE_420_8
                                                     ? std::string()
                                                     : std::string(" in ") + pictureFormatName(config.picture.format)));
            }
            UINT token = 0;
            win::check(win::mfplat().createDxgiDeviceManager(&token, manager_.out()), "Decoder device manager",
                       codecError);
            win::check(manager_->ResetDevice(d3d_.device.value, token), "Decoder device binding", codecError);
        } else if (!config.cpuImages) { // Software decoding's pictures go up as textures.
            if (config.device) {
                d3d_.adopt(static_cast<ID3D11Device*>(config.device.get()));
            } else {
                d3d_.create();
            }
        }
        activate();
    }

    bool decode(const EncodedPacket& packet, VideoFrame& frame, CancelToken) override {
        retireCopies();
        if (!configured_ || packet.config != activeConfig_) {
            if (configured_) { // New parameters: a transform that never saw the old ones.
                activate();
            }
            configure(packet);
        }
        queued_.push_back(sample(packet));
        submitQueued();
        for (int attempt = 0; attempt < 5; ++attempt) {
            Com<IMFSample> output;
            const HRESULT result = readOutput(output);
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
                selectOutputType();
                continue;
            }
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (queued_.empty()) {
                    return false;
                }
                submitQueued();
                continue;
            }
            win::check(result, "Decoder output", codecError);
            const bool fits = aperture_ ? packet.width == aperture_->width && packet.height == aperture_->height
                                        : packet.width <= coded_.width && packet.height <= coded_.height;
            if (!fits) {
                const Size decoded = aperture_.value_or(coded_);
                fail(codecError, "The stream announced " + std::to_string(packet.width) + "x" +
                                     std::to_string(packet.height) + " pictures; they decode to " +
                                     std::to_string(decoded.width) + "x" + std::to_string(decoded.height));
            }
            LONGLONG time = 0;
            output->GetSampleTime(&time);
            frame.sequence = uint64_t(time);
            frame.width = packet.width;
            frame.height = packet.height;
            frame.color = colorOf(packet.picture);
            if (manager_) {
                gpuFrame(output, frame, packet.picture);
            } else {
                cpuFrame(output, frame, packet.picture);
                if (!config_.cpuImages) {
                    upload(frame);
                }
            }
            return true;
        }
        fail(codecError, "The decoder kept changing its output format");
    }

    std::string name() const override { return decoder_.name(); }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_MEDIA_FOUNDATION; }

    void reset() override {
        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        queued_.clear();
    }

private:
    struct Retiring {
        Com<IMFSample> sample;
        std::function<rcs_result(uint32_t)> wait;
    };
    struct Size {
        UINT32 width = 0, height = 0;
    };

    // A new transform, bound to the device for hardware decoding.
    void activate() {
        auto candidates = win::decoders(win::subtype(config_.codec));
        if (!decoder_.activate(candidates)) {
            fail(RCS_ERROR_UNSUPPORTED,
                 std::string("No ") + codecName(config_.codec) + " decoder is installed" +
                     (config_.codec == RCS_CODEC_H264 ? "" : " (it comes with Windows' Video Extensions)"));
        }
        configured_ = false;
        queued_.clear();
        Com<IMFAttributes> attributes;
        win::check(decoder_->GetAttributes(attributes.out()), "Decoder attributes", codecError);
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (manager_) {
            UINT32 aware = 0;
            attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware);
            const HRESULT bound = aware ? decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                                   reinterpret_cast<ULONG_PTR>(manager_.value))
                                        : E_NOTIMPL;
            win::check(bound, "Hardware decoding", RCS_ERROR_UNSUPPORTED);
        }
    }

    // A size the GPU does not decode fails here: Media Foundation's decoders
    // would decode it on the CPU instead.
    void configure(const EncodedPacket& packet) {
        const std::string size = std::to_string(packet.width) + "x" + std::to_string(packet.height);
        if (manager_ &&
            !win::gpuDecodes(d3d_.device.value, config_.codec, config_.picture.format, packet.width, packet.height)) {
            fail(codecError, std::string("This GPU does not decode ") + codecName(config_.codec) + " pictures of " +
                                 size + " (its driver offers no decoder of that size)");
        }
        auto input = win::videoType(win::subtype(config_.codec), packet.width, packet.height, 60);
        if (config_.codec == RCS_CODEC_HEVC && config_.picture.format != RCS_PICTURE_420_8) {
            // The profile (MF_MT_VIDEO_PROFILE, whose GUID MinGW names only
            // MF_MT_MPEG2_PROFILE): without it the HEVC decoder offers no
            // P010, AYUV or Y410 output until its first picture. AV1's offers
            // P010 anyway.
            const bool wide = bitDepth(config_.picture.format) > 8;
            input->SetUINT32(MF_MT_MPEG2_PROFILE,
                             chroma444(config_.picture.format)
                                 ? UINT32(wide ? eAVEncH265VProfile_Main_444_10 : eAVEncH265VProfile_Main_444_8)
                                 : UINT32(eAVEncH265VProfile_Main_420_10));
        }
        win::check(decoder_->SetInputType(0, input.value, 0), ("Decoder input type for " + size).c_str(), codecError);
        selectOutputType();
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        activeConfig_ = packet.config;
        configured_ = true;
    }

    // The output of the stream's format (NV12, P010; AYUV, Y410 for 4:4:4),
    // and the layout it comes in: buffers of the coded size (e.g. 1088 rows
    // for 1080p H.264), the picture at their top left (the minimum display
    // aperture, where the decoder states it).
    void selectOutputType() {
        const auto& [wanted, label, pixelBytes] = outputOf(config_.picture.format);
        for (DWORD index = 0;; ++index) {
            Com<IMFMediaType> candidate;
            win::check(decoder_->GetOutputAvailableType(0, index, candidate.out()), label, codecError);
            GUID subtype{};
            candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != wanted) {
                continue;
            }
            win::check(decoder_->SetOutputType(0, candidate.value, 0), "Decoder output type", codecError);
            UINT32 stride = 0;
            MFGetAttributeSize(candidate.value, MF_MT_FRAME_SIZE, &coded_.width, &coded_.height);
            candidate->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
            const UINT32 rowBytes = coded_.width * pixelBytes;
            // The HEVC decoder's AYUV and Y410 types state the stride of a
            // byte a pixel, yet hold rows of four (found on a 640-wide
            // stream: 640 stated, 2560 in the buffer).
            outputStride_ = stride >= rowBytes ? stride : rowBytes;
            aperture_.reset();
            MFVideoArea area{};
            if (SUCCEEDED(candidate->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                                             sizeof(area), nullptr))) {
                if (area.OffsetX.value || area.OffsetY.value) {
                    fail(codecError, "The decoder's pictures start inside its buffers");
                }
                aperture_ = Size{UINT32(area.Area.cx), UINT32(area.Area.cy)};
            }
            const Size picture = aperture_.value_or(coded_);
            if (picture.width > coded_.width || picture.height > coded_.height) {
                fail(codecError, "The decoder describes pictures larger than its buffers (" +
                                     std::to_string(picture.width) + "x" + std::to_string(picture.height) + " in " +
                                     std::to_string(coded_.width) + "x" + std::to_string(coded_.height) + ")");
            }
            return;
        }
    }

    // The decoder output of pictures of `format`: its subtype, what failing
    // to get it is called, and the bytes of a pixel (of its luma plane).
    struct Output {
        const GUID& subtype;
        const char* label;
        UINT32 pixelBytes;
    };
    static Output outputOf(rcs_picture_format format) {
        switch (format) {
        case RCS_PICTURE_420_10:
            return {MFVideoFormat_P010, "Decoder P010 output", 2};
        case RCS_PICTURE_444_8:
            return {MFVideoFormat_AYUV, "Decoder AYUV output", 4};
        case RCS_PICTURE_444_10:
            return {MFVideoFormat_Y410, "Decoder Y410 output", 4};
        default:
            return {MFVideoFormat_NV12, "Decoder NV12 output", 1};
        }
    }

    // One compressed picture as the system decoder expects it.
    Com<IMFSample> sample(const EncodedPacket& packet) {
        const std::vector<uint8_t> stream = win::accessUnit(config_.codec, packet);
        Com<IMFMediaBuffer> buffer;
        win::check(win::mfplat().createMemoryBuffer(DWORD(stream.size()), buffer.out()), "Decoder input buffer",
                   codecError);
        BYTE* bytes = nullptr;
        win::check(buffer->Lock(&bytes, nullptr, nullptr), "Decoder input map", codecError);
        std::copy(stream.begin(), stream.end(), bytes);
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(stream.size()));
        Com<IMFSample> sample;
        win::check(win::mfplat().createSample(sample.out()), "Decoder input sample", codecError);
        sample->AddBuffer(buffer.value);
        sample->SetSampleTime(LONGLONG(packet.sequence)); // Carries the sequence through the decoder.
        sample->SetSampleDuration(1);
        return sample;
    }

    void submitQueued() {
        while (!queued_.empty()) {
            const HRESULT result = decoder_->ProcessInput(0, queued_.front().value, 0);
            if (result == MF_E_NOTACCEPTING) {
                if (queued_.size() > 8) {
                    fail(codecError, "The decoder stopped accepting input");
                }
                return; // Drain output first.
            }
            win::check(result, "Decoder input", codecError);
            queued_.pop_front();
        }
    }

    HRESULT readOutput(Com<IMFSample>& output) {
        MFT_OUTPUT_STREAM_INFO info{};
        win::check(decoder_->GetOutputStreamInfo(0, &info), "Decoder output info", codecError);
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            Com<IMFMediaBuffer> storage;
            win::check(win::mfplat().createSample(output.out()), "Decoder output sample", codecError);
            win::check(
                win::mfplat().createMemoryBuffer(
                    std::max<DWORD>(info.cbSize, outputStride_ * ((coded_.height + 15) & ~15u) * 3 / 2), storage.out()),
                "Decoder output buffer", codecError);
            output->AddBuffer(storage.value);
        }
        MFT_OUTPUT_DATA_BUFFER data{};
        data.pSample = output.value;
        DWORD status = 0;
        const HRESULT result = decoder_->ProcessOutput(0, 1, &data, &status);
        if (data.pEvents) {
            data.pEvents->Release();
        }
        if (SUCCEEDED(result) && !output) {
            output.value = data.pSample; // Decoder-provided sample; we now own it.
        }
        return result;
    }

    // Copies the decoded texture out of the decoder's pool so the public frame
    // stays valid after the decoder is gone: a GPU copy, or the conversion to
    // BGRA when the host reads frames on the CPU (instead of converting there),
    // into a texture of ours reused once nobody holds it (picture_pool.hpp).
    void gpuFrame(Com<IMFSample>& output, VideoFrame& frame, const Picture& picture) {
        Com<IMFMediaBuffer> buffer;
        win::check(output->GetBufferByIndex(0, buffer.out()), "Decoded buffer", codecError);
        auto surface = buffer.as<IMFDXGIBuffer>("Decoded DXGI surface", codecError);
        Com<ID3D11Texture2D> texture;
        win::check(surface->GetResource(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.out())),
                   "Decoded texture", codecError);
        UINT subresource = 0;
        surface->GetSubresourceIndex(&subresource);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width < frame.width || desc.Height < frame.height) {
            fail(codecError, "The decoder's texture is smaller than its pictures");
        }
        frame.surface =
            textures_.next(d3d_.device.value,
                           config_.cpuImages ? win::textureDesc(frame.width, frame.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)
                                             : win::textureDesc(desc.Width, desc.Height, desc.Format,
                                                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
        auto* owned = static_cast<ID3D11Texture2D*>(frame.surface.get());
        {
            win::ContextLock lock(d3d_.context.value);
            if (config_.cpuImages) {
                converter_.blit(d3d_.device.value, d3d_.context.value, texture.value, subresource, frame.width,
                                frame.height, owned, picture);
            } else {
                d3d_.context->CopySubresourceRegion(owned, 0, 0, 0, 0, texture.value, subresource, nullptr);
            }
        }
        auto fence = win::gpuFence(d3d_.device.value, d3d_.context.value);
        retiring_.push_back({std::move(output), fence}); // Keep the pool sample until the copy finished.
        frame.device = win::share(d3d_.device.value);
        frame.waitGpu = std::move(fence);
    }

    // NV12, P010, AYUV or Y410 (selectOutputType) to BGRA.
    void cpuFrame(Com<IMFSample>& output, VideoFrame& frame, const Picture& picture) {
        const auto yuv = win::sampleBytes(output.value);
        const bool packed = chroma444(picture.format);
        if (uint64_t(outputStride_) * coded_.height * (packed ? 2 : 3) / 2 > yuv.size()) {
            fail(codecError, "The decoder's buffer is smaller than its pictures (" + std::to_string(yuv.size()) +
                                 " bytes for " + std::to_string(coded_.width) + "x" + std::to_string(coded_.height) +
                                 ", rows of " + std::to_string(outputStride_) + " bytes)");
        }
        const unsigned bits = bitDepth(picture.format);
        const auto coefficients = yuvCoefficients(picture.matrix, picture.range, bits);
        auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(frame.width) * frame.height * 4);
        if (packed) {
            packedYuvToBgra(yuv.data(), outputStride_, frame.width, frame.height,
                            bits > 8 ? PackedYuv::y410 : PackedYuv::ayuv, coefficients, pixels->data(),
                            frame.width * 4);
        } else {
            yuvToBgra(yuv.data(), outputStride_, yuv.data() + size_t(outputStride_) * coded_.height, outputStride_,
                      frame.width, frame.height, YuvLayout{false, bits}, coefficients, pixels->data(),
                      frame.width * 4);
        }
        frame.color = rgbColor(frame.color);
        frame.pixels = std::move(pixels);
        frame.stride = frame.width * 4;
    }

    // A software-decoded picture (BGRA pixels) as a BGRA8 texture, for hosts
    // that take GPU frames.
    void upload(VideoFrame& frame) {
        auto desc = win::textureDesc(frame.width, frame.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        const D3D11_SUBRESOURCE_DATA data{frame.pixels->data(), frame.stride, 0};
        Com<ID3D11Texture2D> texture;
        win::check(d3d_.device->CreateTexture2D(&desc, &data, texture.out()), "Decoded frame upload", codecError);
        frame.surface = win::share(texture.value);
        frame.device = win::share(d3d_.device.value);
        frame.pixels.reset();
        frame.stride = 0;
    }

    // Bounds outstanding GPU copies so the decoder's sample pool is never exhausted.
    void retireCopies() {
        while (!retiring_.empty() && retiring_.front().wait(0) == RCS_OK) {
            retiring_.pop_front();
        }
        if (retiring_.size() >= 3) {
            if (retiring_.front().wait(1000) != RCS_OK) {
                fail(codecError, "Decoded frame copies are not completing");
            }
            retiring_.pop_front();
        }
    }

    DecoderConfig config_;
    std::optional<win::MediaFoundation> mediaFoundation_;
    win::Device d3d_;
    Com<IMFDXGIDeviceManager> manager_;
    win::Transform decoder_;
    win::VideoProcessor converter_;
    win::TexturePool textures_; // The GPU frames handed out.
    bool configured_ = false;
    std::vector<uint8_t> activeConfig_;
    std::deque<Com<IMFSample>> queued_;
    std::deque<Retiring> retiring_;
    Size coded_;                   // The decoder's buffers...
    std::optional<Size> aperture_; // ...and its pictures, where it says.
    UINT32 outputStride_ = 0;
};

} // namespace

// NVDEC or oneVPL when asked for, or with AUTO in hardware where they decode
// the stream on the GPU of config.device: the 4:4:4 pictures Media Foundation
// decodes on the CPU only, NVDEC first (NVIDIA GPUs, win::nvdecFirst), then
// oneVPL (Intel GPUs, win::vplDecodesFirst), which takes codecs Windows has
// no decoder of too; Media Foundation otherwise.
std::unique_ptr<VideoDecoder> createVideoDecoder(const DecoderConfig& config) {
    const bool automatic = config.backend == RCS_BACKEND_AUTO && config.acceleration == RCS_ACCELERATION_HARDWARE;
    auto* device = static_cast<ID3D11Device*>(config.device.get());
    if (config.backend == RCS_BACKEND_NVDEC ||
        (automatic && win::nvdecFirst(config.codec, config.picture.format, device))) {
        return win::createNvdecDecoder();
    }
    if (config.backend == RCS_BACKEND_ONEVPL ||
        (automatic && win::vplDecodesFirst(config.codec, config.picture.format, device))) {
        return win::createVplDecoder();
    }
    if (config.backend != RCS_BACKEND_AUTO && config.backend != RCS_BACKEND_MEDIA_FOUNDATION) {
        fail(RCS_ERROR_UNSUPPORTED, std::string(backendName(config.backend)) + " does not decode on Windows");
    }
    return std::make_unique<MediaFoundationDecoder>();
}

} // namespace rcsdk::platform
