// oneVPL (onevpl.hpp). Each submit converts the frame on the GPU to the
// stream's layout in a texture of ours (NV12 and P010 with the video
// processor, AYUV and Y410 with yuv_shader.hpp, as the Media Foundation path
// does), copies it into an input surface the runtime gives
// (MFXMemory_GetSurfaceForEncode, on our device), encodes it and waits for its
// packet: one frame in, one packet out (AsyncDepth 1, no B-frames), so an
// encoder that held a picture back would fail, never delay.
//
// The session is tuned for low latency: the encoder's low-power mode (VDEnc,
// the fixed-function encoder), a constant bitrate with the smallest buffer it
// takes (one picture asked for; Intel's give about two), no HRD conformance
// (bitrate changes reset the session without a keyframe), the target usage of
// rcs_encoder_config.speed, the display-remoting scenario and the content
// hint. Keyframes come when asked for, and otherwise every 65 535 pictures,
// the longest group of pictures the interface takes (some 18 minutes at 60
// FPS).
//
// Reference invalidation (mfxExtRefListCtrl, per picture): the encoder keeps
// the last few pictures and refers to one at a time; after a loss the next
// picture refers to the newest kept one from before it and rejects those
// after it. Temporal layers where a session answers that it takes them
// (mfxExtAvcTemporalLayers for H.264 and HEVC, mfxExtTemporalLayers for
// AV1); their layer is in the bitstream (H.264's SVC prefix units, HEVC's NAL
// unit headers, AV1's OBU extensions). Not both at once: a layered stream
// recovers with a keyframe.
//
// Decoding (VplDecoder) runs a session of its own on the decoding device:
// the decoder is made at the stream's first keyframe, from its sequence
// header, and again at a keyframe of new parameters; each packet goes in as
// one complete picture (MFX_BITSTREAM_COMPLETE_FRAME), so a picture comes out
// of the call its packet went into. The runtime allocates the pictures
// (Direct3D 11 textures on the device); each is copied out of its pool into a
// texture of ours (reused once nobody holds it, picture_pool.hpp): 4:2:0
// as it is (NV12, P010; to BGRA by the video processor for hosts that read
// CPU images), 4:4:4 (AYUV, Y410) drawn as BGRA8 by a shader of ours with the
// stream's matrix and range (bgra_shader.hpp), as Windows' software decoders'
// and NVDEC's 4:4:4 pictures come (the video processor, tried first, took 2
// to 7 ms more a picture on the Intel GPU). A picture the runtime marks as
// decoded wrong (MFX_CORRUPTION_MAJOR: from references it does not have, as
// NVIDIA's Media Foundation HEVC encoder's recovery after a loss comes to
// Intel's decoder) fails the decoder instead of showing: the session goes on
// from a keyframe.
#include "platform/windows/onevpl.hpp"

#include "platform/windows/adapters.hpp"
#include "platform/windows/bgra_shader.hpp"
#include "platform/windows/media_foundation.hpp"
#include "platform/windows/picture_pool.hpp"
#include "platform/windows/video_processor.hpp"
#include "platform/windows/yuv_shader.hpp"

#include "media/encoder_output.hpp"
#include "media/layers.hpp"
#include "util/log.hpp"

#include <cfgmgr32.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct" // Structures and unions in theirs.
#pragma clang diagnostic ignored "-Wnested-anon-types"
#endif
#include <onevpl/mfximplcaps.h>
#include <onevpl/mfxvideo.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <type_traits>

namespace rcsdk::platform::win {
namespace {

constexpr rcs_result codecError = RCS_ERROR_CODEC;

// The runtime's functions, loaded once; none where `unavailable` says why.
struct Runtime {
    decltype(&MFXQueryImplsDescription) describe = nullptr;
    decltype(&MFXReleaseImplDescription) release = nullptr;
    decltype(&MFXInitialize) initialize = nullptr;
    decltype(&MFXClose) close = nullptr;
    decltype(&MFXVideoCORE_SetHandle) setHandle = nullptr;
    decltype(&MFXVideoCORE_SyncOperation) sync = nullptr;
    decltype(&MFXVideoENCODE_Query) query = nullptr;
    decltype(&MFXVideoENCODE_Init) init = nullptr;
    decltype(&MFXVideoENCODE_Reset) reset = nullptr;
    decltype(&MFXVideoENCODE_Close) closeEncoder = nullptr;
    decltype(&MFXVideoENCODE_GetVideoParam) parameters = nullptr;
    decltype(&MFXVideoENCODE_EncodeFrameAsync) encode = nullptr;
    decltype(&MFXMemory_GetSurfaceForEncode) surface = nullptr;
    decltype(&MFXVideoDECODE_DecodeHeader) readHeader = nullptr;
    decltype(&MFXVideoDECODE_Init) initDecoder = nullptr;
    decltype(&MFXVideoDECODE_Close) closeDecoder = nullptr;
    decltype(&MFXVideoDECODE_DecodeFrameAsync) decode = nullptr;
    std::string unavailable;
};

// The directories the Intel display adapters' drivers keep the runtime in
// (their DriverStorePathForVPL), as oneVPL's dispatcher finds them.
std::vector<std::wstring> runtimeDirectories() {
    constexpr wchar_t displays[] = L"{4d36e968-e325-11ce-bfc1-08002be10318}";
    constexpr ULONG present = CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT;
    std::vector<wchar_t> ids;
    CONFIGRET listed = CR_SUCCESS;
    do {
        ULONG size = 0;
        if (CM_Get_Device_ID_List_SizeW(&size, displays, present) != CR_SUCCESS) {
            return {};
        }
        ids.assign(size + 1, L'\0');
        listed = CM_Get_Device_ID_ListW(displays, ids.data(), size, present);
    } while (listed == CR_BUFFER_SMALL);
    std::vector<std::wstring> found;
    for (const wchar_t* id = ids.data(); listed == CR_SUCCESS && *id; id += wcslen(id) + 1) {
        if (!wcsstr(id, L"VEN_8086") && !wcsstr(id, L"ven_8086")) {
            continue;
        }
        DEVINST node = 0;
        HKEY key = nullptr;
        if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(id), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS ||
            CM_Open_DevNode_Key(node, KEY_READ, 0, RegDisposition_OpenExisting, &key, CM_REGISTRY_SOFTWARE) !=
                CR_SUCCESS) {
            continue;
        }
        wchar_t path[MAX_PATH]{};
        DWORD bytes = sizeof(path) - sizeof(wchar_t);
        if (RegQueryValueExW(key, L"DriverStorePathForVPL", nullptr, nullptr, reinterpret_cast<BYTE*>(path), &bytes) ==
                ERROR_SUCCESS &&
            path[0]) {
            found.emplace_back(path);
        }
        RegCloseKey(key);
    }
    return found;
}

const Runtime& runtime() {
    static const Runtime result = [] {
        Runtime r;
        HMODULE module = nullptr;
        for (std::wstring file : runtimeDirectories()) {
            if (file.back() != L'\\') {
                file += L'\\';
            }
            file += L"libmfx64-gen.dll";
            if ((module = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))) {
                break;
            }
        }
        if (!module) {
            r.unavailable = "it needs an Intel GPU and its graphics driver, which installs oneVPL's runtime "
                            "(libmfx64-gen.dll)";
            return r;
        }
        const auto resolve = [&](auto& function, const char* name) {
            if (r.unavailable.empty()) {
                function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(module, name));
                if (!function) {
                    r.unavailable = std::string("the Intel graphics driver's oneVPL runtime has no ") + name;
                }
            }
        };
        resolve(r.describe, "MFXQueryImplsDescription");
        resolve(r.release, "MFXReleaseImplDescription");
        resolve(r.initialize, "MFXInitialize");
        resolve(r.close, "MFXClose");
        resolve(r.setHandle, "MFXVideoCORE_SetHandle");
        resolve(r.sync, "MFXVideoCORE_SyncOperation");
        resolve(r.query, "MFXVideoENCODE_Query");
        resolve(r.init, "MFXVideoENCODE_Init");
        resolve(r.reset, "MFXVideoENCODE_Reset");
        resolve(r.closeEncoder, "MFXVideoENCODE_Close");
        resolve(r.parameters, "MFXVideoENCODE_GetVideoParam");
        resolve(r.encode, "MFXVideoENCODE_EncodeFrameAsync");
        resolve(r.surface, "MFXMemory_GetSurfaceForEncode");
        resolve(r.readHeader, "MFXVideoDECODE_DecodeHeader");
        resolve(r.initDecoder, "MFXVideoDECODE_Init");
        resolve(r.closeDecoder, "MFXVideoDECODE_Close");
        resolve(r.decode, "MFXVideoDECODE_DecodeFrameAsync");
        return r; // The module stays loaded for the process's lifetime.
    }();
    return result;
}

const char* statusName(mfxStatus status) {
    switch (status) {
    case MFX_ERR_UNSUPPORTED:
        return "unsupported";
    case MFX_ERR_INVALID_VIDEO_PARAM:
        return "invalid video parameters";
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
        return "incompatible video parameters";
    case MFX_ERR_MEMORY_ALLOC:
        return "out of memory";
    case MFX_ERR_NOT_ENOUGH_BUFFER:
        return "output buffer too small";
    case MFX_ERR_DEVICE_FAILED:
        return "device failed";
    case MFX_ERR_DEVICE_LOST:
        return "device lost";
    case MFX_ERR_GPU_HANG:
        return "GPU hang";
    case MFX_ERR_INVALID_HANDLE:
        return "invalid handle";
    case MFX_ERR_NOT_INITIALIZED:
        return "not initialized";
    case MFX_ERR_ABORTED:
        return "aborted";
    case MFX_WRN_IN_EXECUTION:
        return "still running";
    default:
        return "error";
    }
}

// Errors; warnings (positive) pass.
void check(mfxStatus status, const char* operation, rcs_result category = codecError) {
    if (status >= MFX_ERR_NONE) {
        return;
    }
    fail(status == MFX_ERR_UNSUPPORTED || status == MFX_ERR_INVALID_VIDEO_PARAM ||
                 status == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM
             ? RCS_ERROR_UNSUPPORTED
             : category,
         std::string("oneVPL: ") + operation + " failed (" + statusName(status) + ", " + std::to_string(int(status)) +
             ")");
}

mfxU32 codecId(rcs_codec codec) {
    switch (codec) {
    case RCS_CODEC_H264:
        return MFX_CODEC_AVC;
    case RCS_CODEC_HEVC:
        return MFX_CODEC_HEVC;
    case RCS_CODEC_VP9:
        return MFX_CODEC_VP9;
    case RCS_CODEC_AV1:
        return MFX_CODEC_AV1;
    default:
        return 0;
    }
}

// What the SDK encodes and decodes with oneVPL (not VP9's encoder: the
// header's comment).
constexpr rcs_codec encodedCodecs[]{RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_AV1};
constexpr rcs_codec decodedCodecs[]{RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_VP9, RCS_CODEC_AV1};

// The profile of `codec` pictures of `format` (H.264: 4:2:0 8-bit only).
mfxU16 profileOf(rcs_codec codec, rcs_picture_format format) {
    const bool full = chroma444(format), wide = bitDepth(format) > 8;
    switch (codec) {
    case RCS_CODEC_H264:
        return full || wide ? 0 : MFX_PROFILE_AVC_HIGH;
    case RCS_CODEC_HEVC:
        return full ? MFX_PROFILE_HEVC_REXT : wide ? MFX_PROFILE_HEVC_MAIN10 : MFX_PROFILE_HEVC_MAIN;
    case RCS_CODEC_VP9:
        return full ? (wide ? MFX_PROFILE_VP9_3 : MFX_PROFILE_VP9_1) : (wide ? MFX_PROFILE_VP9_2 : MFX_PROFILE_VP9_0);
    default:
        return full ? MFX_PROFILE_AV1_HIGH : MFX_PROFILE_AV1_MAIN;
    }
}

// The layout of pictures of `format`, the encoders' input and the decoders'
// output: as the runtime names it, and as Direct3D does.
mfxU32 fourccOf(rcs_picture_format format) {
    const bool wide = bitDepth(format) > 8;
    return chroma444(format) ? (wide ? MFX_FOURCC_Y410 : MFX_FOURCC_AYUV) : (wide ? MFX_FOURCC_P010 : MFX_FOURCC_NV12);
}
DXGI_FORMAT dxgiOf(rcs_picture_format format) {
    const bool wide = bitDepth(format) > 8;
    return chroma444(format) ? (wide ? DXGI_FORMAT_Y410 : DXGI_FORMAT_AYUV)
                             : (wide ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12);
}

// What the runtime says a GPU encodes or decodes (`description`, its Enc or
// Dec): for each of `codecs`, the picture formats whose profile and layout it
// lists for Direct3D 11 textures, and the largest picture.
template <class Description>
void describe(const Description& description, std::span<const rcs_codec> codecs, VplCodecs& gpu) {
    for (mfxU16 c = 0; c < description.NumCodecs; ++c) {
        const auto& listed = description.Codecs[c];
        const auto codec = std::find_if(codecs.begin(), codecs.end(),
                                        [&](rcs_codec candidate) { return codecId(candidate) == listed.CodecID; });
        if (codec == codecs.end()) {
            continue;
        }
        for (mfxU16 p = 0; p < listed.NumProfiles; ++p) {
            const auto& profile = listed.Profiles[p];
            for (mfxU16 m = 0; m < profile.NumMemTypes; ++m) {
                const auto& memory = profile.MemDesc[m];
                if (memory.MemHandleType != MFX_RESOURCE_DX11_TEXTURE) {
                    continue;
                }
                for (mfxU16 f = 0; f < memory.NumColorFormats; ++f) {
                    for (const rcs_picture_format format :
                         {RCS_PICTURE_420_8, RCS_PICTURE_420_10, RCS_PICTURE_444_8, RCS_PICTURE_444_10}) {
                        if (memory.ColorFormats[f] != fourccOf(format) ||
                            profileOf(*codec, format) != profile.Profile) {
                            continue;
                        }
                        const auto limit = codecLimit(*codec);
                        const PictureSize largest{std::min(limit.width, memory.Width.Max),
                                                  std::min(limit.height, memory.Height.Max)};
                        PictureSize& known = gpu.largest[*codec];
                        known = gpu.formats[*codec] ? PictureSize{std::min(known.width, largest.width),
                                                                  std::min(known.height, largest.height)}
                                                    : largest;
                        gpu.formats[*codec] |= 1u << format;
                        gpu.codecs |= 1u << *codec;
                    }
                }
            }
        }
    }
}

// Pictures kept to refer to where pictures can be dropped from the
// references: about 80 ms at 60 FPS, as NVENC keeps them.
constexpr mfxU16 referenceFrames = 5;

// An extension buffer of type T with its header filled in.
template <class T>
T extension(mfxU32 id) {
    T buffer{};
    buffer.Header.BufferId = id;
    buffer.Header.BufferSz = sizeof(T);
    return buffer;
}

// An encoding session's settings and the extension buffers they point to.
struct Settings {
    mfxVideoParam video{};
    mfxExtCodingOption coding = extension<mfxExtCodingOption>(MFX_EXTBUFF_CODING_OPTION);
    mfxExtCodingOption3 coding3 = extension<mfxExtCodingOption3>(MFX_EXTBUFF_CODING_OPTION3);
    mfxExtVideoSignalInfo signal = extension<mfxExtVideoSignalInfo>(MFX_EXTBUFF_VIDEO_SIGNAL_INFO);
    // Temporal layers: H.264's and HEVC's buffer, AV1's (the universal one).
    mfxExtAvcTemporalLayers avcLayers = extension<mfxExtAvcTemporalLayers>(MFX_EXTBUFF_AVC_TEMPORAL_LAYERS);
    mfxExtTemporalLayers av1Layers = extension<mfxExtTemporalLayers>(MFX_EXTBUFF_UNIVERSAL_TEMPORAL_LAYERS);
    std::array<mfxTemporalLayer, 3> av1LayerList{};
    std::vector<mfxExtBuffer*> buffers;
    // The rate control's buffer and initial delay in KB: one picture at the
    // first bitrate, asked for, then what the encoder made of them.
    uint32_t buffer = 0, initialDelay = 0;

    Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    void attach() {
        video.ExtParam = buffers.data();
        video.NumExtParam = mfxU16(buffers.size());
    }

    // For `config`'s stream with `layers` temporal layers, keeping
    // `references` pictures to refer to (1 each: a single active reference).
    void prepare(const EncoderConfig& config, uint32_t layers, mfxU16 references) {
        const rcs_picture_format format = config.picture.format;
        const bool full = chroma444(format), wide = bitDepth(format) > 8;
        video.AsyncDepth = 1;
        video.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
        auto& m = video.mfx;
        m.CodecId = codecId(config.codec);
        m.CodecProfile = profileOf(config.codec, format);
        m.TargetUsage = config.speed == RCS_ENCODER_FASTEST    ? MFX_TARGETUSAGE_BEST_SPEED
                        : config.speed == RCS_ENCODER_BALANCED ? MFX_TARGETUSAGE_BALANCED
                                                               : MFX_TARGETUSAGE_BEST_QUALITY;
        m.LowPower = MFX_CODINGOPTION_ON;
        m.GopPicSize = 0xFFFF;
        m.GopRefDist = 1;
        m.IdrInterval = config.codec == RCS_CODEC_HEVC ? 1 : 0; // Each keyframe an IDR picture.
        m.NumRefFrame = references;
        m.RateControlMethod = MFX_RATECONTROL_CBR;
        rate(config.bitrate, config.framesPerSecond);
        auto& f = m.FrameInfo;
        f.FourCC = fourccOf(format);
        f.ChromaFormat = full ? MFX_CHROMAFORMAT_YUV444 : MFX_CHROMAFORMAT_YUV420;
        f.BitDepthLuma = f.BitDepthChroma = wide ? 10 : 8;
        f.Shift = f.FourCC == MFX_FOURCC_P010 ? 1 : 0; // P010 keeps its 10 bits high.
        f.Width = mfxU16((config.width + 15) & ~15u);
        f.Height = mfxU16((config.height + 15) & ~15u);
        f.CropW = mfxU16(config.width);
        f.CropH = mfxU16(config.height);
        f.FrameRateExtN = config.framesPerSecond;
        f.FrameRateExtD = 1;
        f.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;

        buffers.clear();
        if (config.codec == RCS_CODEC_H264 || config.codec == RCS_CODEC_HEVC) {
            // No HRD: the bitrate changes without a new sequence.
            coding.NalHrdConformance = MFX_CODINGOPTION_OFF;
            coding.VuiNalHrdParameters = MFX_CODINGOPTION_OFF;
            coding.AUDelimiter = MFX_CODINGOPTION_OFF;
            buffers.push_back(&coding.Header);
        }
        coding3.ScenarioInfo = MFX_SCENARIO_DISPLAY_REMOTING;
        coding3.ContentInfo =
            config.content == RCS_CONTENT_MOTION ? MFX_CONTENT_FULL_SCREEN_VIDEO : MFX_CONTENT_NON_VIDEO_SCREEN;
        std::fill(std::begin(coding3.NumRefActiveP), std::end(coding3.NumRefActiveP), mfxU16(1));
        coding3.TargetChromaFormatPlus1 = mfxU16(f.ChromaFormat + 1);
        coding3.TargetBitDepthLuma = coding3.TargetBitDepthChroma = f.BitDepthLuma;
        buffers.push_back(&coding3.Header);
        // The picture's colour: what the conversion writes, and what the stream says.
        const auto& p = config.picture;
        const bool high = hdr(format);
        signal.VideoFormat = 5; // Unspecified.
        signal.VideoFullRange = p.range == RCS_RANGE_FULL ? 1 : 0;
        signal.ColourDescriptionPresent = 1;
        signal.ColourPrimaries = high ? 9 : 1;
        signal.TransferCharacteristics = high ? 16 : 1;
        signal.MatrixCoefficients = p.matrix == RCS_MATRIX_BT2020 ? 9 : p.matrix == RCS_MATRIX_BT601 ? 6 : 1;
        buffers.push_back(&signal.Header);
        if (layers > 1 && config.codec == RCS_CODEC_AV1) {
            for (uint32_t layer = 0; layer < layers; ++layer) {
                av1LayerList[layer] = {};
                av1LayerList[layer].FrameRateScale = mfxU16(1u << layer);
            }
            av1Layers.NumLayers = mfxU16(layers);
            av1Layers.Layers = av1LayerList.data();
            buffers.push_back(&av1Layers.Header);
        } else if (layers > 1) {
            for (uint32_t layer = 0; layer < 8; ++layer) {
                avcLayers.Layer[layer].Scale = layer < layers ? mfxU16(1u << layer) : 0;
            }
            buffers.push_back(&avcLayers.Header); // HEVC's has the same identifier.
        }
        attach();
    }

    // The rate control at `bitrate`, constant, in the units the interface
    // counts in (kbit/s and KB of 1000, times BRCParamMultiplier where they
    // exceed 16 bits). The encoder changes the buffer only with a new
    // sequence (a keyframe): it stays.
    void rate(uint32_t bitrate, uint32_t framesPerSecond) {
        auto& m = video.mfx;
        const uint32_t kbps = std::max(1u, bitrate / 1000);
        if (!buffer) {
            buffer = std::max(1u, bitrate / 8 / framesPerSecond / 1000);
            initialDelay = std::max(1u, buffer / 2);
        }
        const uint32_t multiplier = std::max(kbps, buffer) / 65535 + 1;
        m.BRCParamMultiplier = mfxU16(multiplier);
        m.TargetKbps = m.MaxKbps = mfxU16(kbps / multiplier);
        m.BufferSizeInKB = mfxU16(std::max(1u, buffer / multiplier));
        m.InitialDelayInKB = mfxU16(std::max(1u, initialDelay / multiplier));
    }
};

// The most temporal layers (1..3) `session`'s encoder of `codec` takes, as it
// answers a query for a 1080p stream.
uint32_t layersOf(mfxSession session, rcs_codec codec, decltype(&MFXVideoENCODE_Query) query) {
    for (uint32_t layers = 3; layers > 1; --layers) {
        EncoderConfig config;
        config.codec = codec;
        config.width = 1920;
        config.height = 1080;
        config.bitrate = 10'000'000;
        Settings in, out;
        in.prepare(config, layers, mfxU16(layers));
        out.prepare(config, layers, mfxU16(layers));
        // A warning is about other settings (the rate control's buffer): the layers must stay as asked.
        const mfxStatus status = query(session, &in.video, &out.video);
        const bool kept = codec == RCS_CODEC_AV1 ? out.av1Layers.NumLayers == layers
                                                 : out.avcLayers.Layer[layers - 1].Scale == (1u << (layers - 1));
        if (status >= MFX_ERR_NONE && kept) {
            return layers;
        }
    }
    return 1;
}

class VplEncoder final : public VideoEncoder {
public:
    ~VplEncoder() override { close(); }

    void open(const EncoderConfig& config) override {
        config_ = config;
        bitrate_ = config.bitrate;
        output_.emplace(config.codec, config.picture, config.width, config.height, config.framesPerSecond);
        if (config.acceleration != RCS_ACCELERATION_HARDWARE) {
            fail(RCS_ERROR_UNSUPPORTED, "oneVPL encodes in hardware only");
        }
        const Runtime& vpl = runtime();
        if (!vpl.unavailable.empty()) {
            fail(RCS_ERROR_UNSUPPORTED, "oneVPL is not available: " + vpl.unavailable);
        }
        const auto& gpus = vplAdapters();
        if (config.device) {
            device_.assign(static_cast<ID3D11Device*>(config.device.get()));
        } else { // CPU frames are uploaded to a device of our own: on the default GPU, or the first Intel one.
            const auto hardware = hardwareAdapters();
            const VplAdapter* gpu = nullptr;
            for (const VplAdapter& candidate : gpus) {
                const bool preferred = !hardware.empty() && packLuid(candidate.luid) == packLuid(hardware.front().luid);
                if (candidate.encoders.codecs && (!gpu || preferred)) {
                    gpu = &candidate;
                }
            }
            if (!gpu) {
                fail(RCS_ERROR_UNSUPPORTED, "No Intel GPU encodes with oneVPL here");
            }
            const auto adapter = adapterWith(gpu->luid);
            if (!adapter) { // Gone since it was probed.
                fail(codecError, "The Intel GPU " + gpu->name + " is no longer among the system's adapters");
            }
            ownDevice_.create(adapter.value);
            device_.assign(ownDevice_.device.value);
        }
        device_->GetImmediateContext(context_.out());
        const DXGI_ADAPTER_DESC adapter = adapterOf(device_.value);
        const auto gpu = std::find_if(gpus.begin(), gpus.end(), [&](const VplAdapter& g) {
            return packLuid(g.luid) == packLuid(adapter.AdapterLuid);
        });
        if (gpu == gpus.end()) {
            fail(RCS_ERROR_UNSUPPORTED, "The GPU producing the frames (" + narrow(adapter.Description) +
                                            ") is not one oneVPL encodes on (choose RCS_BACKEND_MEDIA_FOUNDATION, "
                                            "or AUTO)");
        }
        name_ = vplName(config.codec, gpu->name);
        const rcs_picture_format format = config.picture.format;
        const VplCodecs& encoders = gpu->encoders;
        if (!(encoders.codecs & (1u << config.codec)) || !(encoders.formats[config.codec] & (1u << format))) {
            fail(RCS_ERROR_UNSUPPORTED,
                 name_ + ": this GPU does not encode " + codecName(config.codec) +
                     ((encoders.codecs & (1u << config.codec)) ? std::string(" in ") + pictureFormatName(format)
                                                               : std::string()));
        }
        const auto limit = narrowed(codecLimit(config.codec), encoders.largest[config.codec].width,
                                    encoders.largest[config.codec].height);
        if (!takes(limit, config.width, config.height)) {
            fail(codecError, name_ + " cannot encode " + std::to_string(config.width) + "x" +
                                 std::to_string(config.height) + " pictures: it takes " + limitText(limit));
        }
        const auto layout =
            chroma444(format) ? std::optional(bitDepth(format) > 8 ? YuvShader::Layout::y410 : YuvShader::Layout::ayuv)
                              : std::nullopt;
        if (layout ? !YuvShader::supported(device_.value, *layout)
                   : !VideoProcessor::writes(device_.value, dxgiOf(format))) {
            fail(RCS_ERROR_UNSUPPORTED, name_ + ": this GPU does not convert pictures to " + pictureFormatName(format));
        }
        layout_ = layout;
        const auto inputDesc = layout
                                   ? YuvShader::targetDesc(*layout, config.width, config.height)
                                   : textureDesc(config.width, config.height, dxgiOf(format), D3D11_BIND_RENDER_TARGET);
        win::check(device_->CreateTexture2D(&inputDesc, nullptr, input_.out()), "Encoder input texture", codecError);

        mfxInitializationParam session{};
        session.AccelerationMode = MFX_ACCEL_MODE_VIA_D3D11;
        session.VendorImplID = gpu->implementation;
        check(vpl.initialize(session, &session_), "Opening a session");
        check(vpl.setHandle(session_, MFX_HANDLE_D3D11_DEVICE, device_.value), "Giving the session its GPU");
        layers_ = std::min(config.temporalLayers, std::max(1u, gpu->temporalLayers[config.codec]));
        invalidates_ = (gpu->refInvalidation & (1u << config.codec)) && layers_ == 1;
        // Kept pictures: some to go back to after a loss, else those layer 0's pictures refer to.
        settings_.prepare(config, layers_, invalidates_ ? referenceFrames : mfxU16(layers_));
        check(vpl.init(session_, &settings_.video),
              ("Initializing the encoder at " + std::to_string(config.width) + "x" + std::to_string(config.height))
                  .c_str());
        mfxVideoParam actual{};
        check(vpl.parameters(session_, &actual), "Reading the encoder's settings");
        if (actual.mfx.FrameInfo.FourCC != fourccOf(format) ||
            actual.mfx.CodecProfile != settings_.video.mfx.CodecProfile) {
            fail(RCS_ERROR_UNSUPPORTED, name_ + " does not encode " + pictureFormatName(format) + " pictures");
        }
        const uint32_t multiplier = std::max<uint32_t>(1, actual.mfx.BRCParamMultiplier);
        settings_.buffer = actual.mfx.BufferSizeInKB * multiplier;
        settings_.initialDelay = actual.mfx.InitialDelayInKB * multiplier;
        settings_.rate(config.bitrate, config.framesPerSecond);
        // Room for any picture: no more than four bytes a pixel.
        bitstreamBytes_.resize(size_t(config.width) * config.height * 4 + 65536);
        bitstream_.Data = bitstreamBytes_.data();
        bitstream_.MaxLength = mfxU32(bitstreamBytes_.size());
    }

    void submit(const VideoFrame& frame) override {
        const uint64_t started = monotonicNs();
        const Runtime& vpl = runtime();
        Com<ID3D11Texture2D> source = sourceTexture(frame);
        if (layout_) {
            yuv_.draw(device_.value, context_.value, source.value, frame.width, frame.height, input_.value, *layout_,
                      config_.picture);
        } else {
            converter_.blit(device_.value, context_.value, source.value, 0, frame.width, frame.height, input_.value,
                            config_.picture);
        }
        if (const uint32_t bitrate = pendingBitrate_.exchange(0)) {
            reconfigure(bitrate);
        }
        mfxFrameSurface1* surface = nullptr;
        check(vpl.surface(session_, &surface), "Taking an input surface");
        struct Release {
            mfxFrameSurface1* surface;
            ~Release() { surface->FrameInterface->Release(surface); }
        } release{surface};
        mfxHDL resource = nullptr;
        mfxResourceType type{};
        check(surface->FrameInterface->GetNativeHandle(surface, &resource, &type), "Reaching an input surface");
        auto* texture = static_cast<ID3D11Texture2D*>(resource);
        D3D11_TEXTURE2D_DESC desc{};
        if (type == MFX_RESOURCE_DX11_TEXTURE) {
            texture->GetDesc(&desc);
        }
        if (type != MFX_RESOURCE_DX11_TEXTURE || desc.ArraySize != 1) {
            fail(codecError, name_ + ": the runtime's input surfaces are not textures of their own");
        }
        {
            ContextLock lock(context_.value);
            const D3D11_BOX box{0, 0, 0, config_.width, config_.height, 1};
            context_->CopySubresourceRegion(texture, 0, 0, 0, 0, input_.value, 0, &box);
        }
        surface->Data.TimeStamp = frame.timestampNs / 100'000 * 9; // 90 kHz.
        surface->Data.FrameOrder = order_;

        mfxEncodeCtrl control{};
        auto references = extension<mfxExtRefListCtrl>(MFX_EXTBUFF_UNIVERSAL_REFLIST_CTRL);
        mfxExtBuffer* controls[]{&references.Header};
        // A keyframe: H.264's and HEVC's an IDR picture, AV1's a key frame.
        const mfxU16 keyframe = config_.codec == RCS_CODEC_H264 || config_.codec == RCS_CODEC_HEVC
                                    ? MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF
                                    : MFX_FRAMETYPE_I | MFX_FRAMETYPE_REF;
        if (keyframe_.exchange(false)) {
            control.FrameType = keyframe;
            encoded_.clear(); // Nothing before it is referred to any more.
            invalidate_ = none;
        } else if (const uint64_t after = invalidate_.exchange(none); after != none) {
            recoversAfter_ = refer(references, after);
            if (recoversAfter_) {
                control.ExtParam = controls;
                control.NumExtParam = 1;
            } else {
                control.FrameType = keyframe;
                encoded_.clear();
            }
        }
        bitstream_.DataOffset = 0;
        bitstream_.DataLength = 0;
        mfxSyncPoint sync = nullptr;
        mfxStatus status = MFX_ERR_NONE;
        for (const auto deadline = Clock::now() + 3s;;) {
            status = vpl.encode(session_, &control, surface, &bitstream_, &sync);
            if (status != MFX_WRN_DEVICE_BUSY) {
                break;
            }
            if (Clock::now() >= deadline) {
                fail(codecError, name_ + ": the GPU stayed busy");
            }
            sleepPrecise(250us);
        }
        if (status == MFX_ERR_MORE_DATA) {
            fail(codecError, name_ + " held a picture back despite its low-latency settings");
        }
        check(status, "Encoding");
        check(vpl.sync(session_, sync, 3000), "Waiting for the encoded picture");

        EncodedPacket packet;
        packet.codec = config_.codec;
        packet.width = frame.width;
        packet.height = frame.height;
        packet.sequence = frame.sequence;
        packet.timestampNs = frame.timestampNs;
        packet.keyframe = (bitstream_.FrameType & (MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR)) != 0;
        packet.recoversAfter = packet.keyframe ? std::nullopt : std::exchange(recoversAfter_, std::nullopt);
        if (packet.keyframe) {
            encoded_.clear();
        }
        encoded_.emplace_back(frame.sequence, order_++);
        if (encoded_.size() > referenceFrames) {
            encoded_.pop_front();
        }
        output_->split({bitstream_.Data + bitstream_.DataOffset, bitstream_.DataLength}, packet);
        if (layers_ > 1) {
            readTemporalLayer(packet);
        }
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
    // On the encoding thread, between pictures: the next picture refers to
    // nothing after `sequence` (refer()).
    void invalidateAfter(uint64_t sequence) noexcept override {
        if (!invalidates_) {
            keyframe_ = true;
            return;
        }
        uint64_t wanted = invalidate_.load();
        while (sequence < wanted && !invalidate_.compare_exchange_weak(wanted, sequence)) {
        }
    }
    bool refInvalidation() const noexcept override { return invalidates_; }
    uint32_t temporalLayers() const noexcept override { return layers_; }
    void setBitrate(uint32_t bitsPerSecond) noexcept override { pendingBitrate_ = bitsPerSecond; }
    uint32_t bitrate() const noexcept override { return bitrate_; }
    bool lowLatency() const noexcept override { return true; }
    std::string name() const override { return name_; }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_ONEVPL; }

private:
    static constexpr uint64_t none = ~0ull;

    // A new bitrate from this picture on, without a keyframe (no HRD); one the
    // encoder refuses leaves the previous in place.
    void reconfigure(uint32_t bitrate) {
        const mfxVideoParam previous = settings_.video;
        settings_.rate(bitrate, config_.framesPerSecond);
        auto reset = extension<mfxExtEncoderResetOption>(MFX_EXTBUFF_ENCODER_RESET_OPTION);
        reset.StartNewSequence = MFX_CODINGOPTION_OFF;
        std::vector<mfxExtBuffer*> buffers = settings_.buffers;
        buffers.push_back(&reset.Header);
        settings_.video.ExtParam = buffers.data();
        settings_.video.NumExtParam = mfxU16(buffers.size());
        const mfxStatus status = runtime().reset(session_, &settings_.video);
        settings_.attach();
        if (status >= MFX_ERR_NONE) {
            bitrate_ = bitrate;
            return;
        }
        settings_.video = previous;
        if (!bitrateRefused_) {
            bitrateRefused_ = true;
            log::warning("encoder", name_ + " refused a bitrate of " + std::to_string(bitrate) + " bit/s (" +
                                        statusName(status) + "); it stays at " + std::to_string(bitrate_.load()));
        }
    }

    // The reference lists of the picture after a loss of those after
    // `after`: the newest kept picture from up to then first, those after it
    // rejected. Returns `after`, or none where nothing kept is that old (a
    // keyframe then).
    std::optional<uint64_t> refer(mfxExtRefListCtrl& lists, uint64_t after) {
        for (auto& entry : lists.PreferredRefList) {
            entry.FrameOrder = mfxU32(MFX_FRAMEORDER_UNKNOWN);
        }
        for (auto& entry : lists.RejectedRefList) {
            entry.FrameOrder = mfxU32(MFX_FRAMEORDER_UNKNOWN);
        }
        for (auto& entry : lists.LongTermRefList) {
            entry.FrameOrder = mfxU32(MFX_FRAMEORDER_UNKNOWN);
        }
        std::optional<mfxU32> anchor;
        size_t rejected = 0;
        for (const auto& [sequence, order] : encoded_) {
            if (sequence <= after) {
                anchor = order;
            } else if (rejected < std::size(lists.RejectedRefList)) {
                lists.RejectedRefList[rejected].FrameOrder = order;
                lists.RejectedRefList[rejected++].PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
            }
        }
        if (!anchor) {
            return std::nullopt;
        }
        lists.NumRefIdxL0Active = 1;
        lists.PreferredRefList[0].FrameOrder = *anchor;
        lists.PreferredRefList[0].PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        // What was rejected is gone from the kept pictures.
        std::erase_if(encoded_, [&](const auto& e) { return e.first > after; });
        return after;
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
        if (session_) {
            runtime().closeEncoder(session_);
            runtime().close(session_);
            session_ = nullptr;
        }
    }

    EncoderConfig config_;
    std::string name_;
    Device ownDevice_;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11Texture2D> input_; // The picture in the stream's layout...
    std::optional<YuvShader::Layout> layout_;
    VideoProcessor converter_; // ...written by the video processor (NV12, P010)...
    YuvShader yuv_;            // ...or the shader (AYUV, Y410).
    mfxSession session_ = nullptr;
    Settings settings_;
    std::vector<mfxU8> bitstreamBytes_;
    mfxBitstream bitstream_{};
    std::optional<EncoderOutput> output_;
    std::atomic<bool> keyframe_{true};
    // Reference invalidation: whether this stream uses it, the pictures the
    // encoder may still refer to (sequence, frame order; newest last), a
    // loss the next picture goes on from, and that picture's
    // EncodedPacket::recoversAfter.
    bool invalidates_ = false;
    uint32_t layers_ = 1;
    mfxU32 order_ = 0;
    std::deque<std::pair<uint64_t, mfxU32>> encoded_;
    std::atomic<uint64_t> invalidate_{none};
    std::optional<uint64_t> recoversAfter_;
    std::atomic<uint32_t> pendingBitrate_{0};
    std::atomic<uint32_t> bitrate_{0};
    bool bitrateRefused_ = false;
    std::deque<EncodedPacket> ready_;
};

bool decodes(const VplAdapter& gpu, rcs_codec codec, rcs_picture_format format) {
    return codec <= RCS_CODEC_AV1 && (gpu.decoders.formats[codec] & (1u << format));
}

class VplDecoder final : public VideoDecoder {
public:
    ~VplDecoder() override { close(); }

    // Takes the device and a session now: what this GPU does not decode
    // fails here; the decoder itself is made at the stream's first keyframe.
    void open(const DecoderConfig& config) override {
        config_ = config;
        const rcs_picture_format format = config.picture.format;
        if (config.acceleration != RCS_ACCELERATION_HARDWARE) {
            fail(RCS_ERROR_UNSUPPORTED, "oneVPL decodes in hardware only");
        }
        const Runtime& vpl = runtime();
        if (!vpl.unavailable.empty()) {
            fail(RCS_ERROR_UNSUPPORTED, "oneVPL is not available: " + vpl.unavailable);
        }
        const auto& gpus = vplAdapters();
        const VplAdapter* gpu = nullptr;
        const std::string wanted = std::string(codecName(config.codec)) + " in " + pictureFormatName(format);
        if (config.device) {
            d3d_.adopt(static_cast<ID3D11Device*>(config.device.get()));
            const DXGI_ADAPTER_DESC adapter = adapterOf(d3d_.device.value);
            for (const VplAdapter& candidate : gpus) {
                if (packLuid(candidate.luid) == packLuid(adapter.AdapterLuid)) {
                    gpu = &candidate;
                }
            }
            if (!gpu || !gpu->decoders.codecs) {
                fail(RCS_ERROR_UNSUPPORTED, "The decoding GPU (" + narrow(adapter.Description) +
                                                ") is not one oneVPL decodes on (choose "
                                                "RCS_BACKEND_MEDIA_FOUNDATION, or AUTO)");
            }
            if (!decodes(*gpu, config.codec, format)) {
                fail(RCS_ERROR_UNSUPPORTED, vplName(config.codec, gpu->name) + ": this GPU does not decode " + wanted);
            }
        } else { // A device of our own: on the default GPU where it decodes the stream, else the first that does.
            const auto hardware = hardwareAdapters();
            for (const VplAdapter& candidate : gpus) {
                const bool preferred = !hardware.empty() && packLuid(candidate.luid) == packLuid(hardware.front().luid);
                if (decodes(candidate, config.codec, format) && (!gpu || preferred)) {
                    gpu = &candidate;
                }
            }
            if (!gpu) {
                fail(RCS_ERROR_UNSUPPORTED, "No Intel GPU here decodes " + wanted + " with oneVPL");
            }
            const auto adapter = adapterWith(gpu->luid);
            if (!adapter) { // Gone since it was probed.
                fail(codecError, "The Intel GPU " + gpu->name + " is no longer among the system's adapters");
            }
            d3d_.create(adapter.value);
        }
        name_ = vplName(config.codec, gpu->name);
        largest_ = gpu->decoders.largest[config.codec];
        mfxInitializationParam session{};
        session.AccelerationMode = MFX_ACCEL_MODE_VIA_D3D11;
        session.VendorImplID = gpu->implementation;
        check(vpl.initialize(session, &session_), "Opening a session");
        check(vpl.setHandle(session_, MFX_HANDLE_D3D11_DEVICE, d3d_.device.value), "Giving the session its GPU");
    }

    bool decode(const EncodedPacket& packet, VideoFrame& frame, CancelToken) override {
        const Runtime& vpl = runtime();
        std::vector<uint8_t> unit = accessUnit(config_.codec, packet);
        mfxBitstream stream{};
        stream.Data = unit.data();
        stream.DataLength = stream.MaxLength = mfxU32(unit.size());
        stream.DataFlag = MFX_BITSTREAM_COMPLETE_FRAME; // A whole picture: the decoder waits for no more.
        stream.TimeStamp = packet.sequence;             // Carries the sequence through the decoder.
        if (packet.keyframe && (!decoding_ || packet.config != activeConfig_)) {
            start(stream, packet);
        }
        if (!decoding_) {
            return false; // Nothing before the first keyframe.
        }
        struct Held { // A picture of the runtime's, given back when done.
            mfxFrameSurface1* surface = nullptr;
            ~Held() { reset(nullptr); }
            void reset(mfxFrameSurface1* next) {
                if (surface) {
                    surface->FrameInterface->Release(surface);
                }
                surface = next;
            }
        } picture;
        for (const auto deadline = Clock::now() + 3s; stream.DataLength;) {
            mfxFrameSurface1* out = nullptr;
            mfxSyncPoint sync = nullptr;
            const mfxStatus status = vpl.decode(session_, &stream, nullptr, &out, &sync);
            if (status == MFX_ERR_MORE_DATA) {
                break; // Taken, nothing to show yet.
            }
            check(status, "Decoding");
            if (status == MFX_ERR_NONE && out) {
                picture.reset(out);
                check(vpl.sync(session_, sync, 3000), "Waiting for the decoded picture");
            } else if (Clock::now() >= deadline) { // Busy, or warnings without a picture.
                fail(codecError, name_ + ": the decoder stayed busy (" + std::to_string(int(status)) + ")");
            } else if (status == MFX_WRN_DEVICE_BUSY) {
                sleepPrecise(250us);
            }
        }
        if (!picture.surface) {
            return false;
        }
        // Not MFX_CORRUPTION_REFERENCE_FRAME: Intel's H.264 decoder marks each
        // picture after a reference invalidation so, decoded right.
        if (picture.surface->Data.Corrupted & MFX_CORRUPTION_MAJOR) {
            fail(codecError, name_ + ": picture " + std::to_string(picture.surface->Data.TimeStamp) +
                                 " decoded wrong (the runtime marks it corrupted: it refers to pictures the decoder "
                                 "does not have); the stream must go on from a keyframe");
        }
        output(*picture.surface, packet, frame);
        return true;
    }

    // A new decoder at the next keyframe, as at the stream's start. Not
    // MFXVideoDECODE_Reset: Intel's AV1 decoder decodes the keyframe after
    // it, then fails the next picture (MFX_ERR_DEVICE_FAILED, driver
    // 32.0.101.8860).
    void reset() override { stop(); }

    std::string name() const override { return name_; }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_ONEVPL; }

private:
    // A decoder of the stream the keyframe `packet` (in `stream`) begins,
    // again for one of new parameters: its pictures as their sequence header
    // says, which must be what the stream announced.
    void start(mfxBitstream& stream, const EncodedPacket& packet) {
        const Runtime& vpl = runtime();
        stop();
        mfxVideoParam params{};
        params.mfx.CodecId = codecId(config_.codec);
        check(vpl.readHeader(session_, &stream, &params), "Reading the stream's header");
        const auto& f = params.mfx.FrameInfo;
        const rcs_picture_format format = config_.picture.format;
        if (f.FourCC != fourccOf(format) ||
            f.ChromaFormat != (chroma444(format) ? MFX_CHROMAFORMAT_YUV444 : MFX_CHROMAFORMAT_YUV420)) {
            fail(codecError,
                 std::string("The stream's pictures are not ") + pictureFormatName(format) + ", as it said");
        }
        const std::string size = std::to_string(f.CropW) + "x" + std::to_string(f.CropH);
        if (packet.width != f.CropW || packet.height != f.CropH) {
            fail(codecError, "The stream announced " + std::to_string(packet.width) + "x" +
                                 std::to_string(packet.height) + " pictures; they decode to " + size);
        }
        const auto limit = narrowed(codecLimit(config_.codec), largest_.width, largest_.height);
        if (!takes(limit, f.CropW, f.CropH)) {
            fail(codecError, name_ + " cannot decode " + size + " pictures: it takes " + limitText(limit));
        }
        params.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        params.AsyncDepth = 1;
        check(vpl.initDecoder(session_, &params), ("Initializing the decoder for " + size + " pictures").c_str());
        activeConfig_ = packet.config;
        decoding_ = true;
    }

    void stop() noexcept {
        if (decoding_) {
            runtime().closeDecoder(session_);
            decoding_ = false;
        }
    }

    // `picture`, decoded, out of the runtime's pool into a texture of ours.
    void output(mfxFrameSurface1& picture, const EncodedPacket& packet, VideoFrame& frame) {
        mfxHDL resource = nullptr;
        mfxResourceType type{};
        check(picture.FrameInterface->GetNativeHandle(&picture, &resource, &type), "Reaching a decoded picture");
        auto* texture = static_cast<ID3D11Texture2D*>(resource);
        D3D11_TEXTURE2D_DESC desc{};
        if (type == MFX_RESOURCE_DX11_TEXTURE) {
            texture->GetDesc(&desc);
        }
        if (type != MFX_RESOURCE_DX11_TEXTURE || desc.ArraySize != 1) {
            fail(codecError, name_ + ": the runtime's pictures are not textures of their own");
        }
        const UINT width = picture.Info.CropW, height = picture.Info.CropH;
        if (desc.Width < width || desc.Height < height) {
            fail(codecError, name_ + ": the decoder's texture is smaller than its pictures");
        }
        ID3D11Device* device = d3d_.device.value;
        ID3D11DeviceContext* context = d3d_.context.value;
        const bool bgra = chroma444(config_.picture.format) || config_.cpuImages;
        const auto ownDesc = bgra ? textureDesc(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)
                                  : textureDesc(desc.Width, desc.Height, desc.Format,
                                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        frame.surface = textures_.next(device, ownDesc);
        auto* owned = static_cast<ID3D11Texture2D*>(frame.surface.get());
        if (chroma444(config_.picture.format)) {
            if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
                fail(codecError, name_ + ": the runtime's pictures are no shader resources");
            }
            const auto layout =
                bitDepth(config_.picture.format) > 8 ? BgraShader::Source::y410 : BgraShader::Source::ayuv;
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = BgraShader::viewFormat(layout);
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            Com<ID3D11ShaderResourceView> source;
            win::check(device->CreateShaderResourceView(texture, &view, source.out()), "Decoded picture view",
                       codecError);
            shader_.draw(device, context, source.value, layout, owned, width, height, packet.picture);
        } else if (config_.cpuImages) {
            ContextLock lock(context);
            converter_.blit(device, context, texture, 0, width, height, owned, packet.picture);
        } else {
            ContextLock lock(context);
            context->CopySubresourceRegion(owned, 0, 0, 0, 0, texture, 0, nullptr);
        }
        frame.sequence = picture.Data.TimeStamp;
        frame.width = width;
        frame.height = height;
        frame.color = colorOf(packet.picture);
        frame.device = share(device);
        frame.waitGpu = gpuFence(device, context);
    }

    void close() noexcept {
        if (session_) {
            stop();
            runtime().close(session_);
            session_ = nullptr;
        }
    }

    DecoderConfig config_;
    std::string name_;
    PictureSize largest_;
    Device d3d_;
    mfxSession session_ = nullptr;
    bool decoding_ = false;             // Since a keyframe started the decoder...
    std::vector<uint8_t> activeConfig_; // ...of these parameters.
    BgraShader shader_;                 // 4:4:4 to BGRA.
    TexturePool textures_;              // The pictures handed out.
    VideoProcessor converter_;          // CPU images of 4:2:0 to BGRA.
};

} // namespace

const std::vector<VplAdapter>& vplAdapters() {
    static const std::vector<VplAdapter> result = [] {
        std::vector<VplAdapter> found;
        const Runtime& vpl = runtime();
        if (!vpl.unavailable.empty()) {
            return found;
        }
        mfxU32 count = 0, idCount = 0;
        mfxHDL* descriptions = vpl.describe(MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &count);
        mfxHDL* ids = vpl.describe(MFX_IMPLCAPS_DEVICE_ID_EXTENDED, &idCount);
        const auto adapters = hardwareAdapters();
        for (mfxU32 i = 0; descriptions && ids && i < count && i < idCount; ++i) {
            const auto* description = static_cast<const mfxImplDescription*>(descriptions[i]);
            const auto* id = static_cast<const mfxExtendedDeviceId*>(ids[i]);
            if (!description || !id || description->Impl != MFX_IMPL_TYPE_HARDWARE || !id->LUIDValid) {
                continue;
            }
            LUID luid{};
            std::memcpy(&luid, id->DeviceLUID, sizeof(luid));
            const auto adapter = std::find_if(adapters.begin(), adapters.end(),
                                              [&](const Adapter& a) { return packLuid(a.luid) == packLuid(luid); });
            if (adapter == adapters.end()) {
                continue;
            }
            VplAdapter gpu;
            gpu.luid = luid;
            gpu.implementation = description->VendorImplID;
            gpu.name = narrow(adapter->description);
            describe(description->Enc, encodedCodecs, gpu.encoders);
            describe(description->Dec, decodedCodecs, gpu.decoders);
            // Reference lists per picture: H.264, HEVC and AV1 (codec_roundtrip checks each).
            gpu.refInvalidation = gpu.encoders.codecs;
            gpu.temporalLayers.fill(1);
            // Temporal layers, as a session there answers.
            if (const auto dxgi = adapterWith(luid); dxgi && gpu.encoders.codecs) {
                try {
                    Device d3d;
                    d3d.create(dxgi.value);
                    mfxInitializationParam init{};
                    init.AccelerationMode = MFX_ACCEL_MODE_VIA_D3D11;
                    init.VendorImplID = gpu.implementation;
                    mfxSession session = nullptr;
                    if (vpl.initialize(init, &session) == MFX_ERR_NONE) {
                        if (vpl.setHandle(session, MFX_HANDLE_D3D11_DEVICE, d3d.device.value) == MFX_ERR_NONE) {
                            for (const rcs_codec codec : encodedCodecs) {
                                if (gpu.encoders.codecs & (1u << codec)) {
                                    gpu.temporalLayers[codec] = layersOf(session, codec, vpl.query);
                                }
                            }
                        }
                        vpl.close(session);
                    }
                } catch (const Failure&) { // No device on it now: no layers.
                }
            }
            if (gpu.encoders.codecs || gpu.decoders.codecs) {
                found.push_back(std::move(gpu));
            }
        }
        for (mfxU32 i = 0; descriptions && i < count; ++i) {
            vpl.release(descriptions[i]);
        }
        for (mfxU32 i = 0; ids && i < idCount; ++i) {
            vpl.release(ids[i]);
        }
        return found;
    }();
    return result;
}

std::string vplName(rcs_codec codec, const std::string& gpu) {
    return std::string("oneVPL ") + codecName(codec) + " (" + gpu + ")";
}

bool vplFirst(rcs_codec codec, ID3D11Device* device) {
    const auto& gpus = vplAdapters();
    const auto hardware = device ? std::vector<Adapter>{} : hardwareAdapters();
    const uint64_t luid = device             ? packLuid(adapterOf(device).AdapterLuid)
                          : hardware.empty() ? 0
                                             : packLuid(hardware.front().luid);
    return std::any_of(gpus.begin(), gpus.end(), [&](const VplAdapter& gpu) {
        return packLuid(gpu.luid) == luid && (gpu.encoders.codecs & (1u << codec));
    });
}

bool vplDecodesFirst(rcs_codec codec, rcs_picture_format format, ID3D11Device* device) {
    const auto& gpus = vplAdapters();
    const uint64_t luid = device ? packLuid(adapterOf(device).AdapterLuid) : 0;
    const bool decoded = std::any_of(gpus.begin(), gpus.end(), [&](const VplAdapter& gpu) {
        return (!device || packLuid(gpu.luid) == luid) && decodes(gpu, codec, format);
    });
    return decoded && (chroma444(format) || !windowsDecodes(codec));
}

std::unique_ptr<VideoEncoder> createVplEncoder() {
    return std::make_unique<VplEncoder>();
}

std::unique_ptr<VideoDecoder> createVplDecoder() {
    return std::make_unique<VplDecoder>();
}

} // namespace rcsdk::platform::win
