// H.264, HEVC, VP8, VP9 and AV1 encoding with a Media Foundation transform: the hardware
// encoder of the GPU that produces the frames or, with software acceleration,
// Microsoft's H.264 software encoder. Each submit converts the frame on the
// GPU to the stream's layout (NV12 and P010 with the video processor, AYUV
// and Y410 with yuv_shader.hpp) and returns once its packet is ready: one
// frame in, one packet out, so an encoder that held frames back (reordering)
// would fail, never delay. 10-bit and 4:4:4 pictures go to the hardware
// encoders whose registered inputs take them (win::encoderFormats). Where the
// encoder takes long-term references the application controls, two of them,
// refreshed in turn every eight pictures, make reference invalidation
// possible, as libvpx's anchors do (src/codecs/libvpx.cpp). The bitrate is
// constant, or peak-constrained with the peak at the target where only that
// lets the encoder take a new one while streaming (Intel's: open()).
// Temporal layers (EncoderConfig::temporalLayers): two where the encoder
// takes CODECAPI_AVEncVideoTemporalLayerCount with a range from 1 to at
// least 2, as Chromium checks it (NVIDIA's HEVC encoder says 3 and fails to
// start with more than 1: crbug.com/1373780; Intel's VP9 encoder, the only
// one, takes 2 and then fails while streaming with E_FAIL: both left out).
// Media Foundation says no picture's layer: HEVC and AV1 say it in the
// bitstream; otherwise a picture nothing refers to (H.264's nal_ref_idc,
// VP9's refresh flags) is taken for the upper layer, and a stream whose upper
// layer the bitstream does not tell apart has all its pictures in layer 0
// (none left out). NVIDIA's H.264 and Intel's H.264 and HEVC encoders layer
// (RTX 4090, Intel UHD 770: codec_roundtrip).
#include "platform/windows/adapters.hpp"
#include "platform/windows/media_foundation.hpp"
#include "platform/windows/video_processor.hpp"
#include "platform/windows/yuv_shader.hpp"

#include "media/encoder_output.hpp"
#include "media/layers.hpp"
#include "platform/platform.hpp"
#include "platform/windows/nvenc.hpp"
#include "platform/windows/onevpl.hpp"
#include "util/log.hpp"

#include <codecapi.h>

#include <array>
#include <cstring>
#include <deque>
#include <optional>
#include <tuple>
#include <utility>

namespace rcsdk::platform {
namespace {

using win::Com;
using win::trace;

constexpr rcs_result codecError = RCS_ERROR_CODEC;

class MediaFoundationEncoder final : public VideoEncoder {
public:
    ~MediaFoundationEncoder() override { stop(); }

    void open(const EncoderConfig& config) override {
        const GUID& format = win::subtype(config.codec);
        config_ = config;
        output_.emplace(config.codec, config.picture, config.width, config.height, config.framesPerSecond);
        bitrate_ = config.bitrate;
        mediaFoundation_.emplace();
        if (config.device) {
            device_.assign(static_cast<ID3D11Device*>(config.device.get()));
        } else { // CPU frames are uploaded to a device of our own.
            ownDevice_.create();
            device_.assign(ownDevice_.device.value);
        }
        device_->GetImmediateContext(context_.out());
        // A constant bitrate where the encoder takes a new one while
        // streaming (NVIDIA's do; at a peak-constrained bitrate they keep
        // theirs). Intel's keep theirs at a constant one, saying S_FALSE, and
        // take it at a peak-constrained one, the peak at the target as the
        // session's budget plans: they get that. One that takes none either
        // way keeps the constant one. (RTX 4090 driver 616.92, Core Ultra 7
        // 270K Plus driver 32.0.101.8860: codec_roundtrip.)
        start(format, eAVEncCommonRateControlMode_CBR);
        if (!takesNewBitrate() &&
            !(start(format, eAVEncCommonRateControlMode_PeakConstrainedVBR) && takesNewBitrate())) {
            start(format, eAVEncCommonRateControlMode_CBR);
        }
    }

    void submit(const VideoFrame& frame) override {
        const uint64_t started = monotonicNs();
        Com<ID3D11Texture2D> source = sourceTexture(frame);
        trace("source prepared");
        waitFor(METransformNeedInput);
        trace("input ready");

        const auto layout = inputOf(config_.picture.format).second;
        const auto inputDesc = layout ? win::YuvShader::targetDesc(*layout, frame.width, frame.height)
                                      : win::textureDesc(frame.width, frame.height,
                                                         bitDepth(config_.picture.format) > 8 ? DXGI_FORMAT_P010
                                                                                              : DXGI_FORMAT_NV12,
                                                         D3D11_BIND_RENDER_TARGET);
        Com<ID3D11Texture2D> input;
        win::check(device_->CreateTexture2D(&inputDesc, nullptr, input.out()), "Encoder input texture", codecError);
        if (layout) {
            yuv_.draw(device_.value, context_.value, source.value, frame.width, frame.height, input.value, *layout,
                      config_.picture);
        } else {
            converter_.blit(device_.value, context_.value, source.value, 0, frame.width, frame.height, input.value,
                            config_.picture);
        }
        trace("color conversion done");

        Com<IMFSample> sample;
        win::check(win::mfplat().createSample(sample.out()), "Encoder sample", codecError);
        Com<IMFMediaBuffer> buffer = hardware_ ? surfaceBuffer(input.value) : memoryBuffer(input.value, frame);
        win::check(sample->AddBuffer(buffer.value), "Encoder sample buffer", codecError);
        stamp(sample.value, frame.timestampNs);
        const std::optional<uint64_t> refreshAfter = references();
        if (forceKeyframe_.exchange(false)) {
            win::check(setOption(CODECAPI_AVEncVideoForceKeyFrame, 1), "Keyframe request", codecError);
        }
        if (const uint32_t bitrate = pendingBitrate_.exchange(0)) {
            applyBitrate(bitrate);
        }
        trace("sample prepared");
        win::check(encoder_->ProcessInput(0, sample.value, 0), "Encoder input", codecError);
        consume(METransformNeedInput);
        trace("ProcessInput done");

        EncodedPacket packet;
        packet.codec = config_.codec;
        packet.width = frame.width;
        packet.height = frame.height;
        packet.sequence = frame.sequence;
        packet.timestampNs = frame.timestampNs;
        while (!readOutput(packet)) {
            // An asynchronous transform may emit an event before the picture is ready.
        }
        if (layers_ > 1) {
            inferTemporalLayer(packet);
        }
        if (ltr_) {
            referred(packet, refreshAfter);
        }
        packet.encodeNs = monotonicNs() - started;
        trace("packet complete");
        ready_.push_back(std::move(packet));
    }

    bool receive(EncodedPacket& packet, std::chrono::milliseconds) override {
        if (ready_.empty()) {
            return false;
        }
        packet = std::move(ready_.front());
        ready_.pop_front();
        return true;
    }

    size_t inFlight() const noexcept override { return ready_.size(); }
    void requestKeyframe() noexcept override { forceKeyframe_ = true; }
    void invalidateAfter(uint64_t sequence) noexcept override {
        if (!ltr_) {
            requestKeyframe();
            return;
        }
        uint64_t wanted = invalidate_.load();
        while (sequence < wanted && !invalidate_.compare_exchange_weak(wanted, sequence)) {
        }
    }
    bool refInvalidation() const noexcept override { return ltr_; }
    uint32_t temporalLayers() const noexcept override { return layers_; }
    void setBitrate(uint32_t bitsPerSecond) noexcept override { pendingBitrate_ = bitsPerSecond; }
    uint32_t bitrate() const noexcept override { return bitrate_; }
    bool lowLatency() const noexcept override { return true; } // Synchronous, low-latency mode.
    std::string name() const override { return encoder_.name(); }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_MEDIA_FOUNDATION; }

private:
    static constexpr uint64_t none = ~0ull;
    static constexpr UINT32 ltrSlots = 2, anchorInterval = 8;

    // Before a picture goes in: with an invalidation pending, it refers only
    // to the newest slot holding a picture from up to then (the others are
    // trusted no more), or is a keyframe where none does; otherwise every
    // eight pictures one slot takes it, in turn. Returns after which picture
    // it refers to nothing (the invalidation).
    std::optional<uint64_t> references() {
        const uint64_t after = invalidate_.exchange(none);
        if (!ltr_ || forceKeyframe_) {
            return std::nullopt;
        }
        if (after != none) {
            std::optional<UINT32> newest;
            for (UINT32 slot = 0; slot < ltrSlots; ++slot) {
                if (slots_[slot] && *slots_[slot] <= after && (!newest || *slots_[slot] > *slots_[*newest])) {
                    newest = slot;
                }
            }
            for (UINT32 slot = 0; slot < ltrSlots; ++slot) {
                if (slot != newest) {
                    slots_[slot].reset();
                }
            }
            if (!newest) {
                forceKeyframe_ = true;
                return std::nullopt;
            }
            win::check(setOption(CODECAPI_AVEncVideoUseLTRFrame, (1u << *newest) | 1u << 16),
                       "Reference invalidation", codecError);
            sinceAnchor_ = 0;
            return after;
        }
        if (++sinceAnchor_ >= anchorInterval) {
            sinceAnchor_ = 0;
            win::check(setOption(CODECAPI_AVEncVideoMarkLTRFrame, nextSlot_), "Long-term reference", codecError);
            nextSlot_ = (nextSlot_ + 1) % ltrSlots;
        }
        return std::nullopt;
    }

    // After a picture came out: which slot the encoder marked it as (a
    // keyframe empties them all first: it marks itself and those after it),
    // and whether it went on from an invalidation.
    void referred(EncodedPacket& packet, std::optional<uint64_t> refreshAfter) {
        if (packet.keyframe) {
            slots_.fill(std::nullopt);
            sinceAnchor_ = 0;
        } else if (refreshAfter) {
            packet.recoversAfter = refreshAfter;
        }
        if (lastLtrInfo_) {
            const UINT32 marked = *lastLtrInfo_ & 0xffff;
            if (marked < ltrSlots) {
                slots_[marked] = packet.sequence;
            }
        }
    }

    // The capture time in 100 ns units from the first frame on, so rate
    // control sees the real frame rate (twice the configured one while input
    // boosts capture). A frame stamped no later than the one before (a
    // repeated picture) follows it by the nominal interval.
    void stamp(IMFSample* sample, uint64_t timestampNs) {
        const LONGLONG nominal = 10'000'000 / config_.framesPerSecond;
        if (!stamped_) {
            stamped_ = true;
            firstNs_ = timestampNs;
            lastTime_ = -nominal;
        }
        LONGLONG time = timestampNs > firstNs_ ? LONGLONG((timestampNs - firstNs_) / 100) : 0;
        if (time <= lastTime_) {
            time = lastTime_ + nominal;
        }
        sample->SetSampleTime(time);
        sample->SetSampleDuration(time - lastTime_);
        lastTime_ = time;
    }

    // A new target from the next frame on; one the encoder refuses (also
    // with S_FALSE: it keeps its bitrate) leaves the previous in place, which
    // bitrate() reports. At a peak-constrained bitrate the peak moves with
    // the target, the one moving away from the other first.
    void applyBitrate(uint32_t bitrate) {
        const auto set = [&](const GUID& key, uint32_t value) {
            return setOption(key, value) == S_OK;
        };
        bool taken = false;
        if (!peak_) {
            taken = set(CODECAPI_AVEncCommonMeanBitRate, bitrate);
        } else {
            const bool up = bitrate > bitrate_;
            const GUID& first = up ? CODECAPI_AVEncCommonMaxBitRate : CODECAPI_AVEncCommonMeanBitRate;
            const GUID& second = up ? CODECAPI_AVEncCommonMeanBitRate : CODECAPI_AVEncCommonMaxBitRate;
            taken = set(first, bitrate);
            if (taken && !set(second, bitrate)) {
                taken = false;
                set(first, bitrate_);
            }
        }
        if (taken) {
            bitrate_ = bitrate;
        } else if (!bitrateRefused_) {
            bitrateRefused_ = true;
            log::warning("encoder", encoder_.name() + " refused a bitrate of " + std::to_string(bitrate) +
                                        " bit/s; it stays at " + std::to_string(bitrate_.load()));
        }
    }

    Com<ID3D11Texture2D> sourceTexture(const VideoFrame& frame) {
        Com<ID3D11Texture2D> texture;
        if (frame.surface) {
            texture.assign(static_cast<ID3D11Texture2D*>(frame.surface.get()));
            return texture;
        }
        // CPU BGRA frame (test sources): upload it.
        auto desc = win::textureDesc(frame.width, frame.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        const D3D11_SUBRESOURCE_DATA data{frame.pixels->data(), frame.stride, 0};
        win::check(device_->CreateTexture2D(&desc, &data, texture.out()), "Frame upload", codecError);
        return texture;
    }

    // The input subtype of pictures of `format`, and the shader layout that
    // writes it (none: the video processor does).
    static std::pair<const GUID&, std::optional<win::YuvShader::Layout>> inputOf(rcs_picture_format format) {
        switch (format) {
        case RCS_PICTURE_420_10:
            return {MFVideoFormat_P010, std::nullopt};
        case RCS_PICTURE_444_8:
            return {MFVideoFormat_AYUV, win::YuvShader::Layout::ayuv};
        case RCS_PICTURE_444_10:
            return {MFVideoFormat_Y410, win::YuvShader::Layout::y410};
        default:
            return {MFVideoFormat_NV12, std::nullopt};
        }
    }

    static UINT32 hevcProfile(rcs_picture_format format) {
        switch (format) {
        case RCS_PICTURE_420_10:
            return eAVEncH265VProfile_Main_420_10;
        case RCS_PICTURE_444_8:
            return eAVEncH265VProfile_Main_444_8;
        case RCS_PICTURE_444_10:
            return eAVEncH265VProfile_Main_444_10;
        default:
            return eAVEncH265VProfile_Main_420_8;
        }
    }

    void createTransform(const GUID& format) {
        hardware_ = config_.acceleration == RCS_ACCELERATION_HARDWARE;
        const auto layout = inputOf(config_.picture.format).second;
        if (layout && !win::YuvShader::supported(device_.value, *layout)) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("The capturing GPU cannot write ") +
                                            pictureFormatName(config_.picture.format) + " pictures for its encoder");
        }
        if (hardware_) {
            const LUID adapter = win::adapterLuid(device_.value);
            auto candidates = win::hardwareEncoders(format, &adapter);
            const bool any = !candidates.empty();
            std::erase_if(candidates, [&](const Com<IMFActivate>& c) {
                return !(win::encoderFormats(c.value, config_.codec) & (1u << config_.picture.format));
            });
            HRESULT failure = S_OK;
            if (!encoder_.activate(candidates, &failure)) {
                fail(RCS_ERROR_UNSUPPORTED,
                     !any ? std::string("The capturing GPU has no hardware ") + codecName(config_.codec) + " encoder"
                     : candidates.empty()
                         ? std::string("The capturing GPU's ") + codecName(config_.codec) + " encoder takes no " +
                               pictureFormatName(config_.picture.format) + " pictures"
                         : std::string("The capturing GPU's ") + codecName(config_.codec) +
                               " encoder: " + win::startFailure(candidates.back().value, failure));
            }
            return;
        }
        if (config_.picture.format != RCS_PICTURE_420_8) { // As codecImplementations() lists it.
            fail(RCS_ERROR_UNSUPPORTED, std::string("Microsoft's software H.264 encoder takes 4:2:0 8-bit pictures, "
                                                    "not ") +
                                            pictureFormatName(config_.picture.format));
        }
        // Software HEVC encoders (codec extensions) are too slow for real time.
        auto candidates =
            config_.codec == RCS_CODEC_H264 ? win::softwareEncoders(format) : std::vector<Com<IMFActivate>>{};
        if (!encoder_.activate(candidates)) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("No software ") + codecName(config_.codec) + " encoder");
        }
    }

    // A new transform, set up for `mode` (open()); false where it refuses a
    // peak-constrained bitrate.
    bool start(const GUID& format, eAVEncCommonRateControlMode mode) {
        stop();
        createTransform(format);
        return configure(format, mode);
    }

    void stop() {
        if (encoder_) {
            encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        options_.reset();
        events_.reset();
        encoder_.reset();
        manager_.reset();
        needInput_ = haveOutput_ = 0;
    }

    // Whether the encoder takes a new target while streaming (S_OK; S_FALSE
    // is no): asked for a quarter less, then the configured one again.
    bool takesNewBitrate() {
        const bool taken = setOption(CODECAPI_AVEncCommonMeanBitRate, config_.bitrate - config_.bitrate / 4) == S_OK;
        if (taken) {
            win::check(setOption(CODECAPI_AVEncCommonMeanBitRate, config_.bitrate), "Encoder bitrate", codecError);
        }
        return taken;
    }

    bool configure(const GUID& format, eAVEncCommonRateControlMode mode) {
        trace("configure begin");
        Com<IMFAttributes> attributes;
        win::check(encoder_->GetAttributes(attributes.out()), "Encoder attributes", codecError);
        UINT32 value = 0;
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &value);
        if (value) { // Hardware MFTs are asynchronous and report readiness as events.
            attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            win::check(
                encoder_->QueryInterface(__uuidof(IMFMediaEventGenerator), reinterpret_cast<void**>(events_.out())),
                "Encoder events", codecError);
        }
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (hardware_) {
            value = 0;
            attributes->GetUINT32(MF_SA_D3D11_AWARE, &value);
            if (!value) {
                fail(codecError, "The hardware encoder does not accept D3D11 input");
            }
            UINT token = 0;
            win::check(win::mfplat().createDxgiDeviceManager(&token, manager_.out()), "Encoder device manager",
                       codecError);
            win::check(manager_->ResetDevice(device_.value, token), "Encoder device binding", codecError);
            win::check(
                encoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.value)),
                "Encoder D3D11 binding", codecError);
        }

        // MinGW has no __uuidof(ICodecAPI); use the IID directly.
        win::check(encoder_->QueryInterface(IID_ICodecAPI, reinterpret_cast<void**>(options_.out())), "Encoder options",
                   codecError);
        // Required: what the stream promises. Low latency, a constant bitrate
        // (what the rate control and the video budget plan with) at the
        // configured rate, or one peaking there (open()).
        VARIANT lowLatency{};
        lowLatency.vt = VT_BOOL;
        lowLatency.boolVal = VARIANT_TRUE;
        win::check(options_->SetValue(&CODECAPI_AVLowLatencyMode, &lowLatency), "Encoder low-latency mode", codecError);
        if (const HRESULT result = setOption(CODECAPI_AVEncCommonRateControlMode, mode); FAILED(result)) {
            if (mode != eAVEncCommonRateControlMode_CBR) {
                return false;
            }
            win::check(result, "Encoder constant bitrate", codecError);
        }
        win::check(setOption(CODECAPI_AVEncCommonMeanBitRate, config_.bitrate), "Encoder bitrate", codecError);
        peak_ = mode == eAVEncCommonRateControlMode_PeakConstrainedVBR &&
                setOption(CODECAPI_AVEncCommonMaxBitRate, config_.bitrate) == S_OK;
        if (mode == eAVEncCommonRateControlMode_PeakConstrainedVBR && !peak_) {
            log::debug("encoder", encoder_.name() + " keeps its own peak bitrate");
        }
        // Optional, refused by some hardware encoders (NVIDIA's H.264 MFT): B-frames
        // are off in low-latency mode anyway (and an encoder holding frames back
        // would fail, see above).
        if (FAILED(setOption(CODECAPI_AVEncMPVDefaultBPictureCount, 0u))) {
            log::debug("encoder", encoder_.name() + " keeps its own B-frame count");
        }
        // Optional, refused by some hardware encoders: keyframes come only
        // when the session asks for them (rcs_encoder_config.keyframe_interval_ms).
        // A GOP size of 0 is unlimited for most encoders, but Microsoft's
        // software H.264 encoder takes 0 as its own default (a keyframe every
        // 2 s): the largest GOP first, 0 where that is refused.
        if (FAILED(setOption(CODECAPI_AVEncMPVGOPSize, UINT32_MAX)) &&
            FAILED(setOption(CODECAPI_AVEncMPVGOPSize, 0u))) {
            log::warning("encoder", encoder_.name() + " keeps its own keyframe interval: keyframes may come "
                                                      "without being asked for");
        }
        // rcs_encoder_config.speed: 0 is the fastest, 100 the best quality.
        const UINT32 qualityVsSpeed = config_.speed == RCS_ENCODER_FASTEST    ? 0
                                      : config_.speed == RCS_ENCODER_BALANCED ? 50
                                                                              : 100;
        if (FAILED(setOption(CODECAPI_AVEncCommonQualityVsSpeed, qualityVsSpeed))) {
            log::warning("encoder", encoder_.name() + " keeps its own trade of speed and quality "
                                                      "(rcs_video_tuning_config.speed)");
        }
        // Long-term references the application controls (trusted until it
        // says otherwise: mode 1), where the encoder takes them; set before
        // the types, as the property is static.
        ltr_ = options_->IsSupported(&CODECAPI_AVEncVideoLTRBufferControl) == S_OK &&
               SUCCEEDED(setOption(CODECAPI_AVEncVideoLTRBufferControl, ltrSlots | 1u << 16));
        // Two temporal layers where asked for and taken (see the top).
        layers_ = 1;
        if (config_.temporalLayers > 1 && options_->IsSupported(&CODECAPI_AVEncVideoTemporalLayerCount) == S_OK &&
            !(config_.codec == RCS_CODEC_HEVC && win::adapterOf(device_.value).VendorId == win::nvidiaVendor) &&
            config_.codec != RCS_CODEC_VP9) {
            VARIANT low{}, high{}, step{};
            const auto number = [](const VARIANT& v) { return v.vt == VT_UI4 ? v.ulVal : 0u; };
            if (SUCCEEDED(options_->GetParameterRange(&CODECAPI_AVEncVideoTemporalLayerCount, &low, &high, &step)) &&
                number(low) <= 1 && number(high) >= 2 &&
                SUCCEEDED(setOption(CODECAPI_AVEncVideoTemporalLayerCount, 2u))) {
                layers_ = 2;
            }
        }

        // The picture's matrix and range in (the conversion writes them) and
        // signalled out (the bitstream's colour description), as the macOS
        // encoder does.
        const bool bt601 = config_.picture.matrix == RCS_MATRIX_BT601;
        const bool full = config_.picture.range == RCS_RANGE_FULL;
        const auto colour = [&](IMFMediaType* type) {
            type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
            type->SetUINT32(MF_MT_YUV_MATRIX, bt601 ? MFVideoTransferMatrix_BT601 : MFVideoTransferMatrix_BT709);
            type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, full ? MFNominalRange_0_255 : MFNominalRange_16_235);
        };
        auto output = win::videoType(format, config_.width, config_.height, config_.framesPerSecond);
        output->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate);
        // AV1: Main; VP9: profile 0; VP8 has one: the encoders' 4:2:0 8-bit profiles.
        if (config_.codec == RCS_CODEC_H264 || config_.codec == RCS_CODEC_HEVC) {
            output->SetUINT32(MF_MT_MPEG2_PROFILE, config_.codec == RCS_CODEC_H264 ? UINT32(eAVEncH264VProfile_High)
                                                                                   : hevcProfile(config_.picture.format));
        }
        colour(output.value);
        // A size the encoder does not take is refused here.
        const std::string size = std::to_string(config_.width) + "x" + std::to_string(config_.height);
        win::check(encoder_->SetOutputType(0, output.value, 0), ("Encoder output type for " + size).c_str(),
                   codecError);
        auto input = win::videoType(inputOf(config_.picture.format).first, config_.width, config_.height,
                                    config_.framesPerSecond);
        colour(input.value);
        win::check(encoder_->SetInputType(0, input.value, 0), ("Encoder input type for " + size).c_str(), codecError);
        win::check(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Encoder streaming", codecError);
        win::check(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Encoder start", codecError);
        trace("configure done");
        return true;
    }

    HRESULT setOption(const GUID& key, ULONG value) {
        VARIANT variant{};
        variant.vt = VT_UI4;
        variant.ulVal = value;
        return options_->SetValue(&key, &variant);
    }

    // ---- Asynchronous transform events (hardware encoders) ----------------

    void pollEvents() {
        for (;;) {
            Com<IMFMediaEvent> event;
            const HRESULT result = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.out());
            if (result == MF_E_NO_EVENTS_AVAILABLE) {
                return;
            }
            win::check(result, "Encoder event", codecError);
            MediaEventType type = MEUnknown;
            HRESULT status = S_OK;
            event->GetType(&type);
            event->GetStatus(&status);
            win::check(status, "Encoder event status", codecError);
            if (type == METransformNeedInput) {
                ++needInput_;
            } else if (type == METransformHaveOutput) {
                ++haveOutput_;
            }
        }
    }

    void waitFor(MediaEventType type) {
        if (!events_) {
            return;
        }
        const auto deadline = Clock::now() + 3s;
        for (pollEvents(); (type == METransformNeedInput ? needInput_ : haveOutput_) == 0; pollEvents()) {
            if (Clock::now() >= deadline) {
                fail(codecError, "The hardware encoder stopped responding");
            }
            sleepPrecise(250us);
        }
    }

    void consume(MediaEventType type) {
        if (events_) {
            --(type == METransformNeedInput ? needInput_ : haveOutput_);
        }
    }

    // ---- Samples -----------------------------------------------------------

    Com<IMFMediaBuffer> surfaceBuffer(ID3D11Texture2D* texture) {
        Com<IMFMediaBuffer> buffer;
        win::check(win::mfplat().createDxgiSurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, buffer.out()),
                   "Encoder surface buffer", codecError);
        return buffer;
    }

    // The software encoder needs the NV12 image in system memory.
    Com<IMFMediaBuffer> memoryBuffer(ID3D11Texture2D* texture, const VideoFrame& frame) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging;
        win::check(device_->CreateTexture2D(&desc, nullptr, staging.out()), "Encoder staging texture", codecError);
        win::ContextLock lock(context_.value);
        context_->CopyResource(staging.value, texture);
        D3D11_MAPPED_SUBRESOURCE map{};
        win::check(context_->Map(staging.value, 0, D3D11_MAP_READ, 0, &map), "Encoder readback", codecError);
        const DWORD size = frame.width * frame.height * 3 / 2;
        Com<IMFMediaBuffer> buffer;
        BYTE* bytes = nullptr;
        if (SUCCEEDED(win::mfplat().createMemoryBuffer(size, buffer.out())) &&
            SUCCEEDED(buffer->Lock(&bytes, nullptr, nullptr))) {
            for (uint32_t row = 0; row < frame.height * 3 / 2; ++row) {
                std::memcpy(bytes + size_t(row) * frame.width,
                            static_cast<uint8_t*>(map.pData) + size_t(row) * map.RowPitch, frame.width);
            }
            buffer->Unlock();
            buffer->SetCurrentLength(size);
        }
        context_->Unmap(staging.value, 0);
        if (!bytes) {
            fail(codecError, "Encoder input buffer allocation failed");
        }
        return buffer;
    }

    // Returns true once `packet` holds a complete access unit.
    bool readOutput(EncodedPacket& packet) {
        waitFor(METransformHaveOutput);
        trace("output event ready");
        MFT_OUTPUT_STREAM_INFO info{};
        win::check(encoder_->GetOutputStreamInfo(0, &info), "Encoder output info", codecError);
        Com<IMFSample> sample;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            Com<IMFMediaBuffer> storage;
            win::check(win::mfplat().createSample(sample.out()), "Encoder output sample", codecError);
            // Room for any picture: no more than the uncompressed NV12 size, plus headers.
            const DWORD bound = DWORD(size_t(config_.width) * config_.height * 3 / 2 + 65536);
            win::check(win::mfplat().createMemoryBuffer(std::max<DWORD>(info.cbSize, bound), storage.out()),
                       "Encoder output buffer", codecError);
            sample->AddBuffer(storage.value);
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = sample.value;
        DWORD status = 0;
        const HRESULT result = encoder_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) {
            output.pEvents->Release();
        }
        consume(METransformHaveOutput);
        if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Intel's encoders announce their final output type before the first
            // packet. Accept it; the encoder then signals the output again.
            Com<IMFMediaType> type;
            win::check(encoder_->GetOutputAvailableType(0, 0, type.out()), "Encoder output renegotiation", codecError);
            win::check(encoder_->SetOutputType(0, type.value, 0), "Encoder output renegotiation", codecError);
            trace("output type renegotiated");
            return false;
        }
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (!events_) {
                fail(codecError, "The encoder buffered a frame despite low-latency mode");
            }
            return false;
        }
        win::check(result, "Encoder output", codecError);
        if (!sample) {
            sample.value = output.pSample; // Transform-provided sample; we now own it.
        }
        UINT32 ltrInfo = 0;
        lastLtrInfo_ = ltr_ && SUCCEEDED(sample->GetUINT32(win::longTermReferenceInfo, &ltrInfo))
                           ? std::optional<UINT32>(ltrInfo)
                           : std::nullopt;
        const auto stream = win::sampleBytes(sample.value);
        if (config_.codec == RCS_CODEC_AV1) {
            UINT32 cleanPoint = 0;
            sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);
            packet.keyframe = cleanPoint != 0;
        }
        output_->split(stream, packet);
        return !packet.data.empty();
    }

    EncoderConfig config_;
    std::optional<win::MediaFoundation> mediaFoundation_;
    win::Device ownDevice_;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    win::Transform encoder_;
    Com<IMFMediaEventGenerator> events_;
    Com<IMFDXGIDeviceManager> manager_;
    Com<ICodecAPI> options_;
    std::atomic<uint32_t> pendingBitrate_{0};
    std::atomic<uint32_t> bitrate_{0};
    bool bitrateRefused_ = false;
    bool peak_ = false; // At a peak-constrained bitrate, the peak at the target.
    win::VideoProcessor converter_; // To NV12 and P010...
    win::YuvShader yuv_;            // ...and to AYUV and Y410.
    bool hardware_ = false;
    std::atomic<bool> forceKeyframe_{false};
    // Long-term references: whether the encoder takes them, the picture each
    // slot holds (as its output said), what the last output said, and an
    // invalidation the next picture makes.
    bool ltr_ = false;
    uint32_t layers_ = 1; // The temporal layers it makes.
    std::array<std::optional<uint64_t>, ltrSlots> slots_;
    UINT32 sinceAnchor_ = 0, nextSlot_ = 0;
    std::optional<UINT32> lastLtrInfo_;
    std::atomic<uint64_t> invalidate_{none};
    unsigned needInput_ = 0, haveOutput_ = 0;
    bool stamped_ = false;
    uint64_t firstNs_ = 0;
    LONGLONG lastTime_ = 0;
    std::optional<EncoderOutput> output_;
    std::deque<EncodedPacket> ready_;
};

} // namespace

// NVENC or oneVPL when asked for, or with AUTO where it comes first for the
// GPU that produces the frames (win::nvencFirst, win::vplFirst); Media
// Foundation otherwise.
std::unique_ptr<VideoEncoder> createVideoEncoder(const EncoderConfig& config) {
    auto* device = static_cast<ID3D11Device*>(config.device.get());
    const bool automatic = config.backend == RCS_BACKEND_AUTO && config.acceleration == RCS_ACCELERATION_HARDWARE;
    if (config.backend == RCS_BACKEND_NVENC || (automatic && win::nvencFirst(config.codec, device))) {
        return win::createNvencEncoder();
    }
    if (config.backend == RCS_BACKEND_ONEVPL || (automatic && win::vplFirst(config.codec, device))) {
        return win::createVplEncoder();
    }
    if (config.backend != RCS_BACKEND_AUTO && config.backend != RCS_BACKEND_MEDIA_FOUNDATION) {
        fail(RCS_ERROR_UNSUPPORTED, std::string(backendName(config.backend)) + " does not encode on Windows");
    }
    return std::make_unique<MediaFoundationEncoder>();
}

} // namespace rcsdk::platform
