// H.264, HEVC and AV1 encoding with NVENC, driven directly, through one
// version of its interface (nvenc_api.hpp): included by nvenc_api13.cpp and
// nvenc_api12.cpp right after that version's header, which each puts in the
// namespace RCSDK_NVENC_API names, with the version's name and first driver
// (versionName, firstDriver).
//
// Each submit converts the frame on the GPU (to NV12 and P010 with the same video
// processor, matrix and range as the Media Foundation path; to AYUV with a
// shader of ours, yuv_shader.hpp, as video processors need not write it)
// into one of a few input surfaces registered with the
// encoder once, encodes it synchronously and returns once its packet is
// ready: one frame in, one packet out. The surfaces are NV12 for 8-bit 4:2:0
// pictures; P010 for 10-bit 4:2:0 ones, encoded as HEVC Main 10 or AV1 Main
// where the GPU says it can (NV_ENC_CAPS_SUPPORT_10BIT_ENCODE; H.264 takes 8
// bits only); AYUV (NV_ENC_BUFFER_FORMAT_AYUV) for 8-bit 4:4:4 ones, encoded
// as H.264 High 4:4:4 Predictive or HEVC Format Range Extensions where it says
// it can (NV_ENC_CAPS_SUPPORT_YUV444_ENCODE; AV1 takes 4:2:0 only); and the
// BGRA picture itself (NV_ENC_BUFFER_FORMAT_ARGB) for 10-bit 4:4:4 HEVC,
// where it can do both: NVENC converts it with the VUI's matrix and range.
// It registers no D3D11 texture of 10-bit 4:4:4 YCbCr (YUV444_10BIT planes
// in an R16 texture three pictures high are refused, RTX 4090).
//
// The session is tuned the way game streaming tunes it: preset P1 (P4 or P7
// for the other rcs_encoder_config.speed) with the ultra-low-latency tuning,
// a constant bitrate with a one-frame buffer, no
// B-frames, no lookahead or multiple passes, an endless GOP (keyframes only
// when asked for) and parameter sets repeated with each keyframe. Bitrate
// changes reconfigure the running session, without a keyframe.
// Temporal layers (EncoderConfig::temporalLayers): temporal SVC where the
// driver says it can (NV_ENC_CAPS_SUPPORT_TEMPORAL_SVC, up to
// NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS): H.264's on either version, with an
// SVC prefix unit before each slice that says its layer; HEVC's and AV1's on
// 13.0 only (12.0 has none), the layer in each NAL unit's header and OBU's
// extension (not tried on a GPU yet).

#include "platform/windows/adapters.hpp"
#include "platform/windows/d3d.hpp"
#include "platform/windows/video_processor.hpp"
#include "platform/windows/win.hpp"
#include "platform/windows/yuv_shader.hpp"

#include "media/encoder_output.hpp"
#include "media/levels.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace rcsdk::platform::win::RCSDK_NVENC_API {
namespace {

constexpr rcs_result codecError = RCS_ERROR_CODEC;
// This version, as the driver reports versions.
constexpr uint32_t thisVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;

// The driver's interface in this version, loaded once; not loaded without an
// NVIDIA driver that speaks it.
struct Driver {
    NV_ENCODE_API_FUNCTION_LIST functions{};
    decltype(&NvEncodeAPICreateInstance) create = nullptr;
    uint32_t supported = 0; // The highest version the driver speaks.
    bool loaded = false;
};

const Driver& driver() {
    static const Driver result = [] {
        Driver loaded;
        HMODULE module = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) { // No NVIDIA driver.
            return loaded;
        }
        using Version = decltype(&NvEncodeAPIGetMaxSupportedVersion);
        using Create = decltype(&NvEncodeAPICreateInstance);
        const auto version = reinterpret_cast<Version>(GetProcAddress(module, "NvEncodeAPIGetMaxSupportedVersion"));
        const auto create = reinterpret_cast<Create>(GetProcAddress(module, "NvEncodeAPICreateInstance"));
        if (!version || !create || version(&loaded.supported) != NV_ENC_SUCCESS || loaded.supported < thisVersion) {
            return loaded;
        }
        loaded.functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        loaded.create = create;
        loaded.loaded = create(&loaded.functions) == NV_ENC_SUCCESS;
        return loaded; // The module stays loaded for the process's lifetime.
    }();
    return result;
}

const GUID& codecGuid(rcs_codec codec) {
    switch (codec) {
    case RCS_CODEC_HEVC:
        return NV_ENC_CODEC_HEVC_GUID;
    case RCS_CODEC_AV1:
        return NV_ENC_CODEC_AV1_GUID;
    default:
        return NV_ENC_CODEC_H264_GUID;
    }
}

const char* statusName(NVENCSTATUS status) {
    switch (status) {
    case NV_ENC_ERR_NO_ENCODE_DEVICE:
        return "no encode device";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:
        return "unsupported device";
    case NV_ENC_ERR_INVALID_PARAM:
        return "invalid parameter";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:
        return "unsupported parameter";
    case NV_ENC_ERR_OUT_OF_MEMORY:
        return "out of memory";
    case NV_ENC_ERR_INVALID_VERSION:
        return "invalid version";
    case NV_ENC_ERR_ENCODER_BUSY:
        return "encoder busy";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY:
        return "too many sessions for this GPU and driver";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED:
        return "registering the surface failed";
    default:
        return "error";
    }
}

// The codecs an open session encodes, as (1u << codec) bits.
uint32_t codecsOf(void* encoder) {
    const auto& nv = driver().functions;
    uint32_t count = 0, codecs = 0;
    if (nv.nvEncGetEncodeGUIDCount(encoder, &count) != NV_ENC_SUCCESS || !count) {
        return 0;
    }
    std::vector<GUID> guids(count);
    if (nv.nvEncGetEncodeGUIDs(encoder, guids.data(), count, &count) != NV_ENC_SUCCESS) {
        return 0;
    }
    guids.resize(count);
    for (const rcs_codec codec : {RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_AV1}) {
        if (std::find(guids.begin(), guids.end(), codecGuid(codec)) != guids.end()) {
            codecs |= 1u << codec;
        }
    }
    return codecs;
}

// The driver's module keeps one interface version for the whole process: the
// one the last NvEncodeAPICreateInstance asked for. NVIDIA's own Media
// Foundation encoder asks for its own, older one when it opens, and a session
// opened afterwards with ours fails (NV_ENC_ERR_INVALID_VERSION). So ours is
// asked for again right before each session opens; sessions already open
// keep theirs.
NVENCSTATUS openSession(ID3D11Device* device, void** encoder) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    NV_ENCODE_API_FUNCTION_LIST functions{};
    functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (const NVENCSTATUS status = driver().create(&functions); status != NV_ENC_SUCCESS) {
        return status;
    }
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = device;
    open.apiVersion = NVENCAPI_VERSION;
    return functions.nvEncOpenEncodeSessionEx(&open, encoder);
}

// The largest picture an open session encodes in `codec`, as the driver
// reports it (NV_ENC_CAPS_WIDTH_MAX, _HEIGHT_MAX), within the codec's own
// limit; the codec's limit where the driver does not say.
PictureSize largestOf(void* encoder, rcs_codec codec) {
    const auto cap = [&](NV_ENC_CAPS which) {
        NV_ENC_CAPS_PARAM param{};
        param.version = NV_ENC_CAPS_PARAM_VER;
        param.capsToQuery = which;
        int value = 0;
        return driver().functions.nvEncGetEncodeCaps(encoder, codecGuid(codec), &param, &value) == NV_ENC_SUCCESS &&
                       value >= 16
                   ? uint32_t(value)
                   : UINT32_MAX;
    };
    const auto limit = codecLimit(codec);
    return {std::min(limit.width, cap(NV_ENC_CAPS_WIDTH_MAX)), std::min(limit.height, cap(NV_ENC_CAPS_HEIGHT_MAX))};
}

// A capability of an open session in `codec` (NV_ENC_CAPS_*), 0 where the driver does not say.
int capability(void* encoder, rcs_codec codec, NV_ENC_CAPS which) {
    NV_ENC_CAPS_PARAM param{};
    param.version = NV_ENC_CAPS_PARAM_VER;
    param.capsToQuery = which;
    int value = 0;
    return driver().functions.nvEncGetEncodeCaps(encoder, codecGuid(codec), &param, &value) == NV_ENC_SUCCESS ? value
                                                                                                             : 0;
}

// The picture formats an open session encodes in `codec`, from its capabilities.
uint32_t formatsOf(void* encoder, rcs_codec codec) {
    uint32_t formats = defaultPictureFormats;
    if (codec != RCS_CODEC_AV1 && capability(encoder, codec, NV_ENC_CAPS_SUPPORT_YUV444_ENCODE)) {
        formats |= 1u << RCS_PICTURE_444_8;
    }
    if (codec != RCS_CODEC_H264 && capability(encoder, codec, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE)) {
        formats |= 1u << RCS_PICTURE_420_10;
        if (formats & (1u << RCS_PICTURE_444_8)) {
            formats |= 1u << RCS_PICTURE_444_10;
        }
    }
    return formats;
}

// The temporal layers an open session makes in `codec`, at most three (1:
// none): 12.0 has temporal SVC for H.264 only.
uint32_t temporalLayersOf(void* encoder, rcs_codec codec) {
    if ((NVENCAPI_MAJOR_VERSION < 13 && codec != RCS_CODEC_H264) ||
        !capability(encoder, codec, NV_ENC_CAPS_SUPPORT_TEMPORAL_SVC)) {
        return 1;
    }
    return uint32_t(std::clamp(capability(encoder, codec, NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS), 1, 3));
}

// The highest level an open session encodes `codec` at, as the bitstream
// says it (NV_ENC_CAPS_LEVEL_MAX); none where the driver does not say (AV1's
// level 2.0 is 0: asked, not assumed).
std::optional<uint32_t> highestLevel(void* encoder, rcs_codec codec) {
    NV_ENC_CAPS_PARAM param{};
    param.version = NV_ENC_CAPS_PARAM_VER;
    param.capsToQuery = NV_ENC_CAPS_LEVEL_MAX;
    int level = 0;
    if (driver().functions.nvEncGetEncodeCaps(encoder, codecGuid(codec), &param, &level) != NV_ENC_SUCCESS ||
        level < 0) {
        return std::nullopt;
    }
    return uint32_t(level);
}

// Whether a session on `device` starts encoding `codec` in `format` at
// 3840x2160 and `fps` frames a second (the level that takes it).
bool startsAt(ID3D11Device* device, rcs_codec codec, rcs_picture_format format, uint32_t fps) {
    const auto& nv = driver().functions;
    void* encoder = nullptr;
    if (openSession(device, &encoder) != NV_ENC_SUCCESS) {
        return false;
    }
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    bool started = false;
    if (nv.nvEncGetEncodePresetConfigEx(encoder, codecGuid(codec), NV_ENC_PRESET_P1_GUID,
                                        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset) == NV_ENC_SUCCESS) {
        NV_ENC_CONFIG settings = preset.presetCfg;
        settings.gopLength = NVENC_INFINITE_GOPLENGTH;
        settings.frameIntervalP = 1;
        settings.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        settings.rcParams.averageBitRate = settings.rcParams.maxBitRate = 20'000'000;
        const bool full = chroma444(format), deep = bitDepth(format) > 8;
        if (codec == RCS_CODEC_HEVC) {
            settings.profileGUID = full ? NV_ENC_HEVC_PROFILE_FREXT_GUID
                                   : deep ? NV_ENC_HEVC_PROFILE_MAIN10_GUID
                                          : NV_ENC_HEVC_PROFILE_MAIN_GUID;
            auto& h = settings.encodeCodecConfig.hevcConfig;
            h.chromaFormatIDC = full ? 3 : 1;
#if NVENCAPI_MAJOR_VERSION >= 13
            h.inputBitDepth = h.outputBitDepth = deep ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
#else
            h.pixelBitDepthMinus8 = deep ? 2 : 0;
#endif
        } else {
            settings.profileGUID = full ? NV_ENC_H264_PROFILE_HIGH_444_GUID : NV_ENC_H264_PROFILE_HIGH_GUID;
            settings.encodeCodecConfig.h264Config.chromaFormatIDC = full ? 3 : 1;
        }
        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = codecGuid(codec);
        init.presetGUID = NV_ENC_PRESET_P1_GUID;
        init.encodeWidth = init.darWidth = init.maxEncodeWidth = 3840;
        init.encodeHeight = init.darHeight = init.maxEncodeHeight = 2160;
        init.frameRateNum = fps;
        init.frameRateDen = 1;
        init.enablePTD = 1;
        init.encodeConfig = &settings;
        init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        started = nv.nvEncInitializeEncoder(encoder, &init) == NV_ENC_SUCCESS;
    }
    nv.nvEncDestroyEncoder(encoder);
    return started;
}

// The most pixels a second a session on `device` encodes `codec` in `format`
// at: its highest level's (media/levels.hpp). 4:4:4 may stop lower than the
// driver says of the codec (RTX 4090, driver 616.92: H.264 4:4:4 at level
// 6.0, 3840x2160 at 128 FPS, its 4:2:0 at 6.2): from the highest level down,
// the first that starts 4K at a rate the level below does not take.
uint64_t pixelRateOf(ID3D11Device* device, void* encoder, rcs_codec codec, rcs_picture_format format) {
    const auto highest = highestLevel(encoder, codec);
    if (!highest) {
        return 0;
    }
    if (!chroma444(format) || (codec != RCS_CODEC_H264 && codec != RCS_CODEC_HEVC)) {
        return levelPixelRate(codec, *highest);
    }
    static constexpr uint32_t h264[] = {62, 61, 60, 52, 51}, hevc[] = {186, 183, 180, 156, 153};
    const std::span<const uint32_t> levels = codec == RCS_CODEC_H264 ? std::span(h264) : std::span(hevc);
    constexpr uint64_t uhd = 3840 * 2160;
    for (size_t i = 0; i + 1 < levels.size(); ++i) {
        if (levels[i] > *highest) {
            continue;
        }
        const uint32_t beyondBelow = uint32_t(levelPixelRate(codec, levels[i + 1]) / uhd + 1);
        if (startsAt(device, codec, format, beyondBelow)) {
            return levelPixelRate(codec, levels[i]);
        }
    }
    return levelPixelRate(codec, levels.back());
}

uint32_t probe(ID3D11Device* device, NvencAdapter* adapter) {
    if (!driver().loaded) {
        return 0;
    }
    void* encoder = nullptr;
    const NVENCSTATUS status = openSession(device, &encoder);
    const uint32_t codecs = status == NV_ENC_SUCCESS ? codecsOf(encoder) : 0;
    if (adapter) {
        adapter->codecs = codecs;
        for (const rcs_codec codec : {RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_AV1}) {
            if (!(codecs & (1u << codec))) {
                continue;
            }
            adapter->largest[codec] = largestOf(encoder, codec);
            adapter->formats[codec] = formatsOf(encoder, codec);
            if (capability(encoder, codec, NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION)) {
                adapter->refInvalidation |= 1u << codec;
            }
            adapter->temporalLayers[codec] = temporalLayersOf(encoder, codec);
            for (rcs_picture_format format = RCS_PICTURE_420_8; format <= RCS_PICTURE_444_10; ++format) {
                if (adapter->formats[codec] & (1u << format)) {
                    adapter->pixelRates[codec][format] = pixelRateOf(device, encoder, codec, format);
                }
            }
        }
    }
    if (encoder) {
        driver().functions.nvEncDestroyEncoder(encoder);
    }
    return codecs;
}

// Pictures kept to refer to where pictures can be dropped from the
// references: about 80 ms at 60 FPS of round trip within which a viewer's
// report of a lost picture still finds something older to refer to, within
// what H.264 levels allow a 4K picture.
constexpr size_t referenceFrames = 5;

class NvencEncoder final : public VideoEncoder {
public:
    ~NvencEncoder() override { close(); }

    void open(const EncoderConfig& config) override {
        config_ = config;
        bitrate_ = config.bitrate;
        output_.emplace(config.codec, config.picture, config.width, config.height, config.framesPerSecond);
        if (config.acceleration != RCS_ACCELERATION_HARDWARE) {
            fail(RCS_ERROR_UNSUPPORTED, "NVENC encodes in hardware only");
        }
        if (!driver().loaded) {
            fail(RCS_ERROR_UNSUPPORTED,
                 std::string("NVENC is not available: it needs an NVIDIA GPU and driver ") + firstDriver + " or later");
        }
        if (config.device) {
            device_.assign(static_cast<ID3D11Device*>(config.device.get()));
        } else { // CPU frames are uploaded to a device of our own: on the default GPU, or the first NVIDIA one.
            const auto gpus = nvencAdapters();
            auto gpu = nvencDefaultAdapter(gpus);
            if (!gpu && !gpus.empty()) {
                gpu = gpus.front();
            }
            if (!gpu) {
                fail(RCS_ERROR_UNSUPPORTED, "No NVIDIA GPU encodes here");
            }
            const auto adapter = adapterWith(gpu->luid);
            if (!adapter) { // Gone since nvencAdapters() looked (removed, or its driver restarted).
                fail(codecError, "The NVIDIA GPU " + gpu->name + " is no longer among the system's adapters");
            }
            ownDevice_.create(adapter.value);
            device_.assign(ownDevice_.device.value);
        }
        device_->GetImmediateContext(context_.out());
        const DXGI_ADAPTER_DESC adapter = adapterOf(device_.value);
        if (adapter.VendorId != nvidiaVendor) {
            fail(RCS_ERROR_UNSUPPORTED, "The GPU producing the frames (" + narrow(adapter.Description) +
                                            ") is not NVIDIA's: NVENC cannot encode them (choose "
                                            "RCS_BACKEND_MEDIA_FOUNDATION, or AUTO)");
        }
        name_ = nvencName(config.codec, narrow(adapter.Description));

        const auto& nv = driver().functions;
        check(openSession(device_.value, &encoder_), "Opening an encode session");
        if (!(codecsOf(encoder_) & (1u << config.codec))) {
            fail(RCS_ERROR_UNSUPPORTED, name_ + ": this GPU does not encode " + codecName(config.codec));
        }
        if (!(formatsOf(encoder_, config.codec) & (1u << config.picture.format))) {
            fail(RCS_ERROR_UNSUPPORTED, name_ + ": this GPU does not encode " + codecName(config.codec) + " in " +
                                            pictureFormatName(config.picture.format));
        }
        const DXGI_FORMAT surfaceFormat = inputFormat();
        if (surfaceFormat == DXGI_FORMAT_AYUV ? !YuvShader::supported(device_.value, YuvShader::Layout::ayuv)
                                              : !VideoProcessor::writes(device_.value, surfaceFormat)) {
            fail(RCS_ERROR_UNSUPPORTED, name_ + ": this GPU does not convert pictures to " +
                                            pictureFormatName(config.picture.format) + " (DXGI format " +
                                            std::to_string(surfaceFormat) + ")");
        }
        if (const auto largest = largestOf(encoder_, config.codec);
            !takes(narrowed(codecLimit(config.codec), largest.width, largest.height), config.width, config.height)) {
            fail(codecError, name_ + " cannot encode " + std::to_string(config.width) + "x" +
                                 std::to_string(config.height) + " pictures: it takes " +
                                 limitText(narrowed(codecLimit(config.codec), largest.width, largest.height)));
        }

        // rcs_encoder_config.speed, always with the ultra-low-latency tuning.
        const GUID presetGuid = config.speed == RCS_ENCODER_FASTEST    ? NV_ENC_PRESET_P1_GUID
                                : config.speed == RCS_ENCODER_BALANCED ? NV_ENC_PRESET_P4_GUID
                                                                       : NV_ENC_PRESET_P7_GUID;
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        check(nv.nvEncGetEncodePresetConfigEx(encoder_, codecGuid(config.codec), presetGuid,
                                              NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset),
              "Reading the low-latency preset");
        settings_ = preset.presetCfg;
        invalidates_ = capability(encoder_, config.codec, NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION) != 0;
        layers_ = std::min(config.temporalLayers, temporalLayersOf(encoder_, config.codec));
        tune(settings_);

        init_.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init_.encodeGUID = codecGuid(config.codec);
        init_.presetGUID = presetGuid;
        init_.encodeWidth = init_.darWidth = init_.maxEncodeWidth = config.width;
        init_.encodeHeight = init_.darHeight = init_.maxEncodeHeight = config.height;
        init_.frameRateNum = config.framesPerSecond;
        init_.frameRateDen = 1;
        init_.enablePTD = 1;
        init_.encodeConfig = &settings_;
        init_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        const std::string initializing =
            "Initializing the encoder at " + std::to_string(config.width) + "x" + std::to_string(config.height);
        check(nv.nvEncInitializeEncoder(encoder_, &init_), initializing.c_str());

        for (auto& surface : surfaces_) {
            const auto desc =
                surfaceFormat == DXGI_FORMAT_AYUV
                    ? YuvShader::targetDesc(YuvShader::Layout::ayuv, config.width, config.height)
                    : textureDesc(config.width, config.height, surfaceFormat, D3D11_BIND_RENDER_TARGET);
            win::check(device_->CreateTexture2D(&desc, nullptr, surface.texture.out()), "Encoder input surface",
                       codecError);
            NV_ENC_REGISTER_RESOURCE resource{};
            resource.version = NV_ENC_REGISTER_RESOURCE_VER;
            resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
            resource.width = config.width;
            resource.height = config.height;
            resource.resourceToRegister = surface.texture.value;
            resource.bufferFormat = bufferFormat();
            resource.bufferUsage = NV_ENC_INPUT_IMAGE;
            check(nv.nvEncRegisterResource(encoder_, &resource), "Registering an input surface");
            surface.registered = resource.registeredResource;
        }
        NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
        bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        check(nv.nvEncCreateBitstreamBuffer(encoder_, &bitstream), "Creating the output buffer");
        bitstream_ = bitstream.bitstreamBuffer;
    }

    void submit(const VideoFrame& frame) override {
        const uint64_t started = monotonicNs();
        const auto& nv = driver().functions;
        Com<ID3D11Texture2D> source = sourceTexture(frame);
        Surface& surface = surfaces_[next_++ % surfaces_.size()];
        if (inputFormat() == DXGI_FORMAT_AYUV) {
            yuv_.draw(device_.value, context_.value, source.value, frame.width, frame.height, surface.texture.value,
                      YuvShader::Layout::ayuv, config_.picture);
        } else {
            converter_.blit(device_.value, context_.value, source.value, 0, frame.width, frame.height,
                            surface.texture.value, config_.picture);
        }
        if (const uint32_t bitrate = pendingBitrate_.exchange(0)) {
            reconfigure(bitrate);
        }

        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = surface.registered;
        check(nv.nvEncMapInputResource(encoder_, &map), "Mapping the input surface");
        struct Unmap {
            void* encoder;
            NV_ENC_INPUT_PTR mapped;
            ~Unmap() { driver().functions.nvEncUnmapInputResource(encoder, mapped); }
        } unmap{encoder_, map.mappedResource};

        NV_ENC_PIC_PARAMS picture{};
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.inputWidth = config_.width;
        picture.inputHeight = config_.height;
        picture.inputBuffer = map.mappedResource;
        picture.bufferFmt = map.mappedBufferFmt;
        picture.outputBitstream = bitstream_;
        picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        picture.inputTimeStamp = frame.timestampNs;
        picture.frameIdx = uint32_t(frame.sequence);
        if (keyframe_.exchange(false)) {
            picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }
        check(nv.nvEncEncodePicture(encoder_, &picture), "Encoding");

        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = bitstream_;
        check(nv.nvEncLockBitstream(encoder_, &lock), "Reading the encoded picture");
        EncodedPacket packet;
        packet.codec = config_.codec;
        packet.width = frame.width;
        packet.height = frame.height;
        packet.sequence = frame.sequence;
        packet.timestampNs = frame.timestampNs;
        packet.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
        // The bitstream says it too: H.264's SVC prefix units, HEVC's NAL unit headers, AV1's OBU extensions.
        packet.temporalId = layers_ > 1 ? lock.temporalId : 0;
        packet.recoversAfter = std::exchange(recoversAfter_, std::nullopt);
        if (packet.keyframe) {
            encoded_.clear(); // Nothing before it is referred to any more.
        }
        encoded_.emplace_back(frame.sequence, frame.timestampNs);
        if (encoded_.size() > referenceFrames) {
            encoded_.pop_front();
        }
        try {
            output_->split({static_cast<const uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes}, packet);
        } catch (...) {
            nv.nvEncUnlockBitstream(encoder_, bitstream_);
            throw;
        }
        nv.nvEncUnlockBitstream(encoder_, bitstream_);
        packet.encodeNs = monotonicNs() - started;
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
    void requestKeyframe() noexcept override { keyframe_ = true; }
    // On the encoding thread, between pictures. NVENC drops a picture from
    // its references by its time stamp; when nothing it still holds is as
    // old as `sequence`, only a keyframe helps.
    void invalidateAfter(uint64_t sequence) noexcept override {
        if (!invalidates_ || encoded_.empty() || sequence < encoded_.front().first) {
            keyframe_ = true;
            return;
        }
        for (const auto& [encoded, timestamp] : encoded_) {
            if (encoded > sequence &&
                driver().functions.nvEncInvalidateRefFrames(encoder_, timestamp) != NV_ENC_SUCCESS) {
                keyframe_ = true;
                return;
            }
        }
        recoversAfter_ = sequence;
    }
    bool refInvalidation() const noexcept override { return invalidates_; }
    uint32_t temporalLayers() const noexcept override { return layers_; }
    void setBitrate(uint32_t bitsPerSecond) noexcept override { pendingBitrate_ = bitsPerSecond; }
    uint32_t bitrate() const noexcept override { return bitrate_; }
    bool lowLatency() const noexcept override { return true; }
    std::string name() const override { return name_; }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_NVENC; }

private:
    struct Surface {
        Com<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR registered = nullptr;
    };

    void check(NVENCSTATUS status, const char* operation) {
        if (status == NV_ENC_SUCCESS) {
            return;
        }
        const char* detail = encoder_ ? driver().functions.nvEncGetLastErrorString(encoder_) : nullptr;
        fail(status == NV_ENC_ERR_UNSUPPORTED_PARAM || status == NV_ENC_ERR_UNSUPPORTED_DEVICE ? RCS_ERROR_UNSUPPORTED
                                                                                               : codecError,
             std::string("NVENC: ") + operation + " failed (" + statusName(status) + ", " + std::to_string(status) +
                 (detail && *detail ? std::string(": ") + detail : std::string()) + ")");
    }

    // The input surfaces' format, as Direct3D and as NVENC name it.
    DXGI_FORMAT inputFormat() const noexcept {
        if (chroma444(config_.picture.format)) {
            return wide() ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_AYUV;
        }
        return wide() ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    }
    NV_ENC_BUFFER_FORMAT bufferFormat() const noexcept {
        if (chroma444(config_.picture.format)) {
            return wide() ? NV_ENC_BUFFER_FORMAT_ARGB : NV_ENC_BUFFER_FORMAT_AYUV;
        }
        return wide() ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    }
    bool wide() const noexcept { return bitDepth(config_.picture.format) > 8; }

    void tune(NV_ENC_CONFIG& c) const {
        c.gopLength = NVENC_INFINITE_GOPLENGTH; // Keyframes when the session asks for them.
        c.frameIntervalP = 1;                   // No B-frames.
        // Where pictures can be dropped from the references, older ones are
        // kept to refer to instead (each picture still refers to the one before).
        const uint32_t dpb = invalidates_ ? uint32_t(referenceFrames) : 0;
        auto& rc = c.rcParams;
        rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        rc.averageBitRate = rc.maxBitRate = config_.bitrate;
        rc.vbvBufferSize = rc.vbvInitialDelay = config_.bitrate / config_.framesPerSecond; // One frame.
        rc.multiPass = NV_ENC_MULTI_PASS_DISABLED;
        rc.enableLookahead = 0;
        rc.enableAQ = 0;
        rc.zeroReorderDelay = 1;
        // The picture's colour: what the conversion writes, and what the
        // stream says (as the other backends do).
        const auto& p = config_.picture;
        const bool high = hdr(p.format), full = p.range == RCS_RANGE_FULL;
        const auto primaries = high ? NV_ENC_VUI_COLOR_PRIMARIES_BT2020 : NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        const auto transfer =
            high ? NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084 : NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        const auto matrix = p.matrix == RCS_MATRIX_BT2020  ? NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL
                            : p.matrix == RCS_MATRIX_BT601 ? NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M
                                                           : NV_ENC_VUI_MATRIX_COEFFS_BT709;
        const auto colour = [&](NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui) {
            vui.videoSignalTypePresentFlag = 1;
            vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
            vui.videoFullRangeFlag = full ? 1 : 0;
            vui.colourDescriptionPresentFlag = 1;
            vui.colourPrimaries = primaries;
            vui.transferCharacteristics = transfer;
            vui.colourMatrix = matrix;
        };
        const bool sampled444 = chroma444(p.format);
#if NVENCAPI_MAJOR_VERSION >= 13
        // The bit depths of the surfaces and of the stream (NVENC widens 8-bit
        // BGRA to 10-bit 4:4:4 HEVC itself).
        const auto inputDepth = inputFormat() == DXGI_FORMAT_P010 ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
        const auto outputDepth = wide() ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
#else
        const uint32_t depthMinus8 = wide() ? 2 : 0; // 12.0: the stream's.
#endif
        switch (config_.codec) {
        case RCS_CODEC_HEVC: {
            c.profileGUID = sampled444 ? NV_ENC_HEVC_PROFILE_FREXT_GUID
                            : wide()   ? NV_ENC_HEVC_PROFILE_MAIN10_GUID
                                       : NV_ENC_HEVC_PROFILE_MAIN_GUID;
            auto& h = c.encodeCodecConfig.hevcConfig;
            h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            h.repeatSPSPPS = 1;
            h.maxNumRefFramesInDPB = dpb;
            h.chromaFormatIDC = sampled444 ? 3 : 1;
#if NVENCAPI_MAJOR_VERSION >= 13
            h.inputBitDepth = inputDepth;
            h.outputBitDepth = outputDepth;
            if (layers_ > 1) {
                h.enableTemporalSVC = 1;
                h.numTemporalLayers = layers_;
                h.maxTemporalLayersMinus1 = layers_ - 1;
            }
#else
            h.pixelBitDepthMinus8 = depthMinus8;
#endif
            colour(h.hevcVUIParameters);
            break;
        }
        case RCS_CODEC_AV1: {
            c.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
            auto& a = c.encodeCodecConfig.av1Config;
            a.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            a.repeatSeqHdr = 1;
            a.maxNumRefFramesInDPB = dpb;
            a.outputAnnexBFormat = 0;
            a.chromaFormatIDC = 1;
#if NVENCAPI_MAJOR_VERSION >= 13
            a.inputBitDepth = inputDepth;
            a.outputBitDepth = outputDepth;
            if (layers_ > 1) {
                a.enableTemporalSVC = 1;
                a.numTemporalLayers = layers_;
                a.maxTemporalLayersMinus1 = layers_ - 1;
            }
#else
            a.inputPixelBitDepthMinus8 = depthMinus8;
            a.pixelBitDepthMinus8 = depthMinus8;
#endif
            a.colorPrimaries = primaries;
            a.transferCharacteristics = transfer;
            a.matrixCoefficients = matrix;
            a.colorRange = full ? 1 : 0;
            break;
        }
        default: {
            c.profileGUID = sampled444 ? NV_ENC_H264_PROFILE_HIGH_444_GUID : NV_ENC_H264_PROFILE_HIGH_GUID;
            auto& h = c.encodeCodecConfig.h264Config;
            h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            h.repeatSPSPPS = 1;
            h.maxNumRefFrames = dpb;
            h.chromaFormatIDC = sampled444 ? 3 : 1;
#if NVENCAPI_MAJOR_VERSION >= 13
            h.inputBitDepth = inputDepth;
            h.outputBitDepth = outputDepth;
#endif
            if (sampled444 && !capability(encoder_, config_.codec, NV_ENC_CAPS_SUPPORT_CABAC)) {
                h.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
            }
            if (layers_ > 1) {
                h.enableTemporalSVC = 1;
                h.numTemporalLayers = h.maxTemporalLayers = layers_;
            }
            colour(h.h264VUIParameters);
            break;
        }
        }
    }

    // A new bitrate from the next frame on, without a keyframe; one the
    // encoder refuses leaves the previous in place.
    void reconfigure(uint32_t bitrate) {
        NV_ENC_CONFIG settings = settings_;
        auto& rc = settings.rcParams;
        rc.averageBitRate = rc.maxBitRate = bitrate;
        rc.vbvBufferSize = rc.vbvInitialDelay = bitrate / config_.framesPerSecond;
        NV_ENC_RECONFIGURE_PARAMS params{};
        params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        params.reInitEncodeParams = init_;
        params.reInitEncodeParams.encodeConfig = &settings;
        if (driver().functions.nvEncReconfigureEncoder(encoder_, &params) == NV_ENC_SUCCESS) {
            settings_ = settings;
            bitrate_ = bitrate;
        } else if (!bitrateRefused_) {
            bitrateRefused_ = true;
            log::warning("encoder", name_ + " refused a bitrate of " + std::to_string(bitrate) +
                                        " bit/s; it stays at " + std::to_string(bitrate_.load()));
        }
    }

    Com<ID3D11Texture2D> sourceTexture(const VideoFrame& frame) {
        Com<ID3D11Texture2D> texture;
        if (frame.surface) {
            texture.assign(static_cast<ID3D11Texture2D*>(frame.surface.get()));
            return texture;
        }
        // CPU BGRA frame (the host's own pictures, test sources): upload it.
        auto desc = textureDesc(frame.width, frame.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        const D3D11_SUBRESOURCE_DATA data{frame.pixels->data(), frame.stride, 0};
        win::check(device_->CreateTexture2D(&desc, &data, texture.out()), "Frame upload", codecError);
        return texture;
    }

    void close() noexcept {
        if (!encoder_) {
            return;
        }
        const auto& nv = driver().functions;
        for (auto& surface : surfaces_) {
            if (surface.registered) {
                nv.nvEncUnregisterResource(encoder_, surface.registered);
            }
        }
        if (bitstream_) {
            nv.nvEncDestroyBitstreamBuffer(encoder_, bitstream_);
        }
        nv.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }

    EncoderConfig config_;
    std::string name_;
    Device ownDevice_;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    void* encoder_ = nullptr;
    NV_ENC_INITIALIZE_PARAMS init_{};
    NV_ENC_CONFIG settings_{};
    std::array<Surface, 3> surfaces_;
    size_t next_ = 0;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    VideoProcessor converter_; // To NV12, P010 and BGRA...
    YuvShader yuv_;            // ...and to AYUV.
    std::optional<EncoderOutput> output_;
    std::atomic<bool> keyframe_{true};
    // Reference invalidation: whether the driver offers it, the pictures the
    // encoder may still refer to (sequence, time stamp; newest last), and the
    // next packet's EncodedPacket::recoversAfter.
    bool invalidates_ = false;
    uint32_t layers_ = 1; // The temporal layers it makes.
    std::deque<std::pair<uint64_t, uint64_t>> encoded_;
    std::optional<uint64_t> recoversAfter_;
    std::atomic<uint32_t> pendingBitrate_{0};
    std::atomic<uint32_t> bitrate_{0};
    bool bitrateRefused_ = false;
    std::deque<EncodedPacket> ready_;
};

} // namespace

const NvencApi& api() {
    static const NvencApi result{
        thisVersion,
        versionName,
        firstDriver,
        [] { return driver().loaded; },
        [] { return driver().supported; },
        probe,
        []() -> std::unique_ptr<VideoEncoder> { return std::make_unique<NvencEncoder>(); },
    };
    return result;
}

} // namespace rcsdk::platform::win::RCSDK_NVENC_API
