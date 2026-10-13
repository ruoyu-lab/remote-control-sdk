// H.264 in software: OpenH264 (third_party/openh264), for systems without a
// software H.264 codec of their own (Linux) and wherever a build turns it on.
//
// Encoding: OpenH264's real-time camera mode for both content hints (its
// screen content mode turns scene change detection on and makes each large
// change an IDR, where the SDK sends keyframes only when asked for), CABAC,
// rate control on the bitrate without skipped pictures, no periodic IDR,
// slices for its threads. It refers to no long-term pictures the
// viewer has not confirmed, which the SDK does not report, so a loss is
// recovered with a keyframe (refInvalidation 0). 4:2:0 8-bit.
// Decoding: Annex B in, the sequence and picture parameter sets ahead of
// every keyframe and of the first packet after a reset or a change of them;
// no error concealment, so a damaged picture fails with OpenH264's reason.
#include "codecs/software_codec.hpp"
#include "media/encoder_output.hpp"
#include "media/nal.hpp"

#include "rcsdk_openh264_names.h"
#include <wels/codec_api.h>

#include <mutex>

namespace rcsdk::codecs::openh264 {
namespace {

using platform::CodecImplementation;

constexpr uint32_t formats = 1u << RCS_PICTURE_420_8;

std::string implementationName() {
    const OpenH264Version v = WelsGetCodecVersion();
    return "OpenH264 H.264 " + std::to_string(v.uMajor) + "." + std::to_string(v.uMinor) + "." +
           std::to_string(v.uRevision);
}

// What OpenH264 last reported as an error, the reason of the failure that follows.
class Errors {
public:
    static void trace(void* context, int level, const char* message) {
        if (level <= WELS_LOG_ERROR) {
            auto* self = static_cast<Errors*>(context);
            const std::lock_guard lock(self->lock_);
            self->last_ = message;
            while (!self->last_.empty() && (self->last_.back() == '\n' || self->last_.back() == '.')) {
                self->last_.pop_back();
            }
        }
    }
    [[noreturn]] void fail(const std::string& what, long result) {
        std::string why;
        {
            const std::lock_guard lock(lock_);
            why.swap(last_);
        }
        rcsdk::fail(RCS_ERROR_CODEC, what + ": " + (why.empty() ? "error " + std::to_string(result) : why));
    }

private:
    std::mutex lock_;
    std::string last_;
};

class Encoder final : public SoftwareEncoder {
public:
    ~Encoder() override {
        if (encoder_) {
            encoder_->Uninitialize();
            WelsDestroySVCEncoder(encoder_);
        }
    }

    std::string name() const override { return implementationName(); }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_OPENH264; }
    bool refInvalidation() const noexcept override { return false; }

private:
    void start(const platform::EncoderConfig& c) override {
        if (c.codec != RCS_CODEC_H264) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("OpenH264 does not encode ") + codecName(c.codec));
        }
        if (!(formats & (1u << c.picture.format))) {
            fail(RCS_ERROR_UNSUPPORTED,
                 std::string("OpenH264 does not encode H.264 in ") + pictureFormatName(c.picture.format));
        }
        if (WelsCreateSVCEncoder(&encoder_) != 0 || !encoder_) {
            fail(RCS_ERROR_CODEC, "OpenH264 could not create an encoder");
        }
        void* context = &errors_;
        option(ENCODER_OPTION_TRACE_CALLBACK_CONTEXT, &context);
        WelsTraceCallback trace = &Errors::trace;
        option(ENCODER_OPTION_TRACE_CALLBACK, &trace);
        int level = WELS_LOG_ERROR;
        option(ENCODER_OPTION_TRACE_LEVEL, &level);
        SEncParamExt p;
        encoder_->GetDefaultParams(&p);
        const unsigned threads = codecs::threads(c.softwareThreads);
        p.iUsageType = CAMERA_VIDEO_REAL_TIME;
        p.iPicWidth = int(c.width);
        p.iPicHeight = int(c.height);
        p.iTargetBitrate = int(c.bitrate);
        p.iMaxBitrate = UNSPECIFIED_BIT_RATE;
        p.iRCMode = RC_BITRATE_MODE;
        p.fMaxFrameRate = float(c.framesPerSecond);
        p.iComplexityMode = c.speed == RCS_ENCODER_BEST_QUALITY ? HIGH_COMPLEXITY
                            : c.speed == RCS_ENCODER_BALANCED   ? MEDIUM_COMPLEXITY
                                                                : LOW_COMPLEXITY;
        p.uiIntraPeriod = 0;
        p.eSpsPpsIdStrategy = CONSTANT_ID;
        p.iEntropyCodingModeFlag = 1; // CABAC
        p.bEnableFrameSkip = false;
        p.bEnableSceneChangeDetect = false;
        p.bEnableDenoise = false;
        p.bEnableLongTermReference = false;
        p.iMultipleThreadIdc = static_cast<unsigned short>(threads);
        SSpatialLayerConfig& layer = p.sSpatialLayers[0];
        layer.iVideoWidth = p.iPicWidth;
        layer.iVideoHeight = p.iPicHeight;
        layer.fFrameRate = p.fMaxFrameRate;
        layer.iSpatialBitrate = p.iTargetBitrate;
        layer.iMaxSpatialBitrate = UNSPECIFIED_BIT_RATE;
        layer.uiProfileIdc = PRO_HIGH;
        layer.uiLevelIdc = LEVEL_UNKNOWN; // OpenH264's, for the size, rate and bitrate.
        layer.sSliceArgument.uiSliceMode = threads > 1 ? SM_FIXEDSLCNUM_SLICE : SM_SINGLE_SLICE;
        layer.sSliceArgument.uiSliceNum = threads;
        const Cicp color = cicpOf(c.picture);
        layer.bVideoSignalTypePresent = true;
        layer.uiVideoFormat = 5; // Unspecified
        layer.bFullRange = c.picture.range == RCS_RANGE_FULL;
        layer.bColorDescriptionPresent = true;
        layer.uiColorPrimaries = color.primaries;
        layer.uiTransferCharacteristics = color.transfer;
        layer.uiColorMatrix = color.matrix;
        if (const int result = encoder_->InitializeExt(&p); result != 0) {
            errors_.fail("Opening OpenH264's encoder", result);
        }
        int format = videoFormatI420;
        option(ENCODER_OPTION_DATAFORMAT, &format);
        output_.emplace(c.codec, c.picture, c.width, c.height, c.framesPerSecond);
        frameMs_ = 1000.0 / std::max(1u, c.framesPerSecond);
    }

    void option(ENCODER_OPTION id, void* value) {
        if (const int result = encoder_->SetOption(id, value); result != 0) {
            errors_.fail("OpenH264's encoder refused option " + std::to_string(int(id)), result);
        }
    }

    std::optional<Output> encode(const YuvImage& image, const Request& request) override {
        if (request.bitrate) {
            SBitrateInfo rate{SPATIAL_LAYER_ALL, int(request.bitrate)};
            if (encoder_->SetOption(ENCODER_OPTION_BITRATE, &rate) != 0) {
                log::warning("codec", "OpenH264 refused the bitrate " + std::to_string(request.bitrate));
            }
        }
        if (request.keyframe) {
            encoder_->ForceIntraFrame(true);
        }
        const Planes planes = planar_.from(image, 8);
        SSourcePicture picture{};
        picture.iColorFormat = videoFormatI420;
        for (int i = 0; i < 3; ++i) {
            picture.pData[i] = const_cast<uint8_t*>(planes.plane[i]);
            picture.iStride[i] = int(planes.stride[i]);
        }
        picture.iPicWidth = int(image.width);
        picture.iPicHeight = int(image.height);
        // Pictures at the stream's rate as far as the rate control goes.
        picture.uiTimeStamp = static_cast<long long>(pictures_++ * frameMs_);
        SFrameBSInfo info{};
        if (const int result = encoder_->EncodeFrame(&picture, &info); result != 0) {
            errors_.fail("OpenH264 could not encode an H.264 picture", result);
        }
        if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid) {
            return std::nullopt;
        }
        stream_.clear();
        for (int l = 0; l < info.iLayerNum; ++l) {
            const SLayerBSInfo& layer = info.sLayerInfo[l];
            size_t size = 0;
            for (int n = 0; n < layer.iNalCount; ++n) {
                size += size_t(layer.pNalLengthInByte[n]);
            }
            stream_.insert(stream_.end(), layer.pBsBuf, layer.pBsBuf + size);
        }
        EncodedPacket packet;
        output_->split(stream_, packet);
        Output out;
        out.data = std::move(packet.data);
        out.config = std::move(packet.config);
        out.keyframe = packet.keyframe;
        return out;
    }

    ISVCEncoder* encoder_ = nullptr;
    Errors errors_;
    PlanarBuffer planar_;
    double frameMs_ = 1000.0 / 60;
    uint64_t pictures_ = 0;
    std::vector<uint8_t> stream_;
    std::optional<EncoderOutput> output_;
};

class Decoder final : public SoftwareDecoder {
public:
    ~Decoder() override { close(); }

    void reset() override {
        close();
        start(config_);
    }
    std::string name() const override { return implementationName(); }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_OPENH264; }

private:
    void start(const platform::DecoderConfig& c) override {
        if (c.codec != RCS_CODEC_H264) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("OpenH264 does not decode ") + codecName(c.codec));
        }
        if (!(formats & (1u << c.picture.format))) {
            fail(RCS_ERROR_UNSUPPORTED,
                 std::string("OpenH264 does not decode H.264 in ") + pictureFormatName(c.picture.format));
        }
        if (WelsCreateDecoder(&decoder_) != 0 || !decoder_) {
            fail(RCS_ERROR_CODEC, "OpenH264 could not create a decoder");
        }
        void* context = &errors_;
        decoder_->SetOption(DECODER_OPTION_TRACE_CALLBACK_CONTEXT, &context);
        WelsTraceCallback trace = &Errors::trace;
        decoder_->SetOption(DECODER_OPTION_TRACE_CALLBACK, &trace);
        int level = WELS_LOG_ERROR;
        decoder_->SetOption(DECODER_OPTION_TRACE_LEVEL, &level);
        SDecodingParam p{};
        p.sVideoProperty.size = sizeof(p.sVideoProperty);
        p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        p.eEcActiveIdc = ERROR_CON_DISABLE;
        p.uiTargetDqLayer = 0xff;
        if (const long result = decoder_->Initialize(&p); result != 0) {
            errors_.fail("Opening OpenH264's decoder", result);
        }
    }

    void close() noexcept {
        if (decoder_) {
            decoder_->Uninitialize();
            WelsDestroyDecoder(decoder_);
            decoder_ = nullptr;
        }
        sentConfig_.clear();
    }

    const YuvImage* decodePicture(const EncodedPacket& packet) override {
        stream_.clear();
        if (packet.keyframe || packet.config != sentConfig_) {
            for (const auto unit : nal::units(packet.config)) {
                nal::appendAnnexB(stream_, unit);
            }
            sentConfig_ = packet.config;
        }
        for (const auto unit : nal::units(packet.data)) {
            nal::appendAnnexB(stream_, unit);
        }
        uint8_t* planes[3]{};
        SBufferInfo info{};
        const DECODING_STATE state = decoder_->DecodeFrameNoDelay(stream_.data(), int(stream_.size()), planes, &info);
        if (state != dsErrorFree) {
            errors_.fail("OpenH264 could not decode an H.264 picture", long(state));
        }
        if (info.iBufferStatus != 1) {
            return nullptr;
        }
        const SSysMEMBuffer& picture = info.UsrData.sSystemBuffer;
        if (picture.iFormat != videoFormatI420) {
            fail(RCS_ERROR_CODEC, "OpenH264 decoded another picture format than the stream's (4:2:0 8-bit)");
        }
        Planes in;
        for (int i = 0; i < 3; ++i) {
            in.plane[i] = planes[i];
            in.stride[i] = size_t(picture.iStride[i ? 1 : 0]);
        }
        in.bits = 8;
        fromPlanar(in, RCS_PIXEL_NV12, uint32_t(picture.iWidth), uint32_t(picture.iHeight), picture_);
        image_ = picture_.view();
        return &image_;
    }

    ISVCDecoder* decoder_ = nullptr;
    Errors errors_;
    std::vector<uint8_t> sentConfig_; // The parameter sets OpenH264 has had since the last reset.
    std::vector<uint8_t> stream_;
    YuvBuffer picture_;
    YuvImage image_;
};

} // namespace

std::vector<CodecImplementation> implementations() {
    return {libraryImplementation(RCS_CODEC_H264, RCS_CODEC_ENCODER, RCS_BACKEND_OPENH264, formats,
                                  implementationName(), false),
            libraryImplementation(RCS_CODEC_H264, RCS_CODEC_DECODER, RCS_BACKEND_OPENH264, formats,
                                  implementationName(), false)};
}

std::unique_ptr<platform::VideoEncoder> encoder() {
    return std::make_unique<Encoder>();
}

std::unique_ptr<platform::VideoDecoder> decoder() {
    return std::make_unique<Decoder>();
}

} // namespace rcsdk::codecs::openh264
