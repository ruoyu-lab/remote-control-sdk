// Media Foundation lifetime and enumeration helpers.
#pragma once

#include "platform/windows/adapters.hpp"
#include "platform/windows/d3d.hpp"
#include "platform/windows/win.hpp"

#include "media/nal.hpp"
#include "media/picture_size.hpp"
#include "media/video.hpp"
#include "util/log.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <type_traits>
#include <vector>

namespace rcsdk::platform::win {

// Media Foundation's functions, from mfplat.dll at run time: Windows N and KN
// editions without the Media Feature Pack and Windows Server without its Media
// Foundation feature have none, and the SDK still loads and runs there (NVENC,
// the host's codecs, data). Nothing is called unless `unavailable` is empty.
struct MfPlat {
    decltype(&::MFStartup) startup = nullptr;
    decltype(&::MFShutdown) shutdown = nullptr;
    // MFTEnum2 (Windows 10 1703), missing from MinGW's headers.
    HRESULT(WINAPI* enumerate)(GUID category, UINT32 flags, const MFT_REGISTER_TYPE_INFO* input,
                               const MFT_REGISTER_TYPE_INFO* output, IMFAttributes* attributes,
                               IMFActivate*** activations, UINT32* count) = nullptr;
    decltype(&::MFCreateAttributes) createAttributes = nullptr;
    decltype(&::MFCreateMediaType) createMediaType = nullptr;
    decltype(&::MFCreateSample) createSample = nullptr;
    decltype(&::MFCreateMemoryBuffer) createMemoryBuffer = nullptr;
    decltype(&::MFCreateDXGISurfaceBuffer) createDxgiSurfaceBuffer = nullptr;
    decltype(&::MFCreateDXGIDeviceManager) createDxgiDeviceManager = nullptr;
    std::string unavailable; // Why Media Foundation cannot be used; empty when it can.
};

// Loaded once; the module stays loaded for the process's lifetime.
inline const MfPlat& mfplat() {
    static const MfPlat result = [] {
        MfPlat f;
        HMODULE module = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
            f.unavailable = "Media Foundation is not installed (Windows N/KN editions: the Media Feature Pack; "
                            "Windows Server: the Media Foundation feature)";
        }
        const auto resolve = [&](auto& function, const char* name) {
            if (module && f.unavailable.empty()) {
                function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(module, name));
                if (!function) {
                    f.unavailable = std::string("Media Foundation's mfplat.dll has no ") + name;
                }
            }
        };
        resolve(f.startup, "MFStartup");
        resolve(f.shutdown, "MFShutdown");
        resolve(f.enumerate, "MFTEnum2");
        resolve(f.createAttributes, "MFCreateAttributes");
        resolve(f.createMediaType, "MFCreateMediaType");
        resolve(f.createSample, "MFCreateSample");
        resolve(f.createMemoryBuffer, "MFCreateMemoryBuffer");
        resolve(f.createDxgiSurfaceBuffer, "MFCreateDXGISurfaceBuffer");
        resolve(f.createDxgiDeviceManager, "MFCreateDXGIDeviceManager");
        if (!f.unavailable.empty()) {
            log::info("codec", f.unavailable + ": its encoders, decoders and AAC are not offered");
        }
        return f;
    }();
    return result;
}

inline bool mediaFoundationInstalled() {
    return mfplat().unavailable.empty();
}

// Throws Failure(UNSUPPORTED) naming what to install where Media Foundation is missing.
inline void requireMediaFoundation() {
    if (!mediaFoundationInstalled()) {
        fail(RCS_ERROR_UNSUPPORTED, mfplat().unavailable);
    }
}

// Initialises COM and Media Foundation for the lifetime of the object. A thread
// the host already initialised as single-threaded apartment is accepted.
// Throws Failure(UNSUPPORTED) where Media Foundation is not installed.
class MediaFoundation {
public:
    MediaFoundation() {
        requireMediaFoundation();
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
            check(com, "COM initialization");
        }
        uninitializeCom_ = SUCCEEDED(com);
        const HRESULT started = mfplat().startup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(started)) {
            if (uninitializeCom_) {
                CoUninitialize();
            }
            check(started, "Media Foundation startup");
        }
    }
    ~MediaFoundation() {
        mfplat().shutdown();
        if (uninitializeCom_) {
            CoUninitialize();
        }
    }
    MediaFoundation(const MediaFoundation&) = delete;
    MediaFoundation& operator=(const MediaFoundation&) = delete;

private:
    bool uninitializeCom_ = false;
};

// The codecs this backend implements, and the Media Foundation subtype of each.
inline constexpr rcs_codec codecs[] = {RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_VP8, RCS_CODEC_VP9, RCS_CODEC_AV1};
inline const GUID& subtype(rcs_codec codec) {
    switch (codec) {
    case RCS_CODEC_H264:
        return MFVideoFormat_H264;
    case RCS_CODEC_HEVC:
        return MFVideoFormat_HEVC;
    case RCS_CODEC_VP8:
        return MFVideoFormat_VP80;
    case RCS_CODEC_VP9:
        return MFVideoFormat_VP90;
    case RCS_CODEC_AV1:
        return MFVideoFormat_AV1;
    default:
        fail(RCS_ERROR_UNSUPPORTED, std::string("Media Foundation does not implement ") + codecName(codec));
    }
}

// One compressed picture as Windows' decoders take it (Media Foundation's,
// NVDEC's parser): H.264 and HEVC in Annex B, keyframes with their
// parameter sets in-band; AV1 a temporal unit in low-overhead format
// (delimiter, the sequence header on keyframes, the frame's OBUs); VP8 and
// VP9 the frame, or the VP9 superframe, as it is.
inline std::vector<uint8_t> accessUnit(rcs_codec codec, const EncodedPacket& packet) {
    std::vector<uint8_t> stream;
    if (codec == RCS_CODEC_AV1) {
        stream = {0x12, 0x00};
        if (packet.keyframe) {
            stream.insert(stream.end(), packet.config.begin(), packet.config.end());
        }
        stream.insert(stream.end(), packet.data.begin(), packet.data.end());
    } else if (vpCodec(codec)) {
        stream = packet.data;
    } else {
        if (packet.keyframe) {
            for (const auto unit : nal::units(packet.config)) {
                nal::appendAnnexB(stream, unit);
            }
        }
        for (const auto unit : nal::units(packet.data)) {
            nal::appendAnnexB(stream, unit);
        }
    }
    return stream;
}

// Transforms producing (encoders) or consuming (decoders) `format`, best first;
// none without Media Foundation.
inline std::vector<Com<IMFActivate>> transforms(const GUID& category, UINT32 flags, const GUID& format,
                                                IMFAttributes* attributes = nullptr) {
    if (!mediaFoundationInstalled()) {
        return {};
    }
    const MFT_REGISTER_TYPE_INFO type{MFMediaType_Video, format};
    const bool decoder = category == MFT_CATEGORY_VIDEO_DECODER;
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    std::vector<Com<IMFActivate>> result;
    if (SUCCEEDED(mfplat().enumerate(category, flags | MFT_ENUM_FLAG_SORTANDFILTER, decoder ? &type : nullptr,
                                     decoder ? nullptr : &type, attributes, &activations, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            result.emplace_back().value = activations[i];
        }
        CoTaskMemFree(activations);
    }
    return result;
}

// Hardware encoders of the GPU `adapter`, or of every GPU.
inline std::vector<Com<IMFActivate>> hardwareEncoders(const GUID& format, const LUID* adapter = nullptr) {
    // MFT_ENUM_ADAPTER_LUID, also missing from MinGW's headers.
    static constexpr GUID adapterAttribute{
        0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}};
    Com<IMFAttributes> attributes;
    if (adapter && mediaFoundationInstalled()) {
        check(mfplat().createAttributes(attributes.out(), 1), "Encoder enumeration", RCS_ERROR_CODEC);
        attributes->SetBlob(adapterAttribute, reinterpret_cast<const UINT8*>(adapter), sizeof(LUID));
    }
    return transforms(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE, format, attributes.value);
}

inline std::vector<Com<IMFActivate>> softwareEncoders(const GUID& format) {
    return transforms(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT, format);
}

// Synchronous decoders: Windows' own and those of installed codec extensions
// (HEVC, AV1). They decode on the GPU when given a D3D11 device manager.
inline std::vector<Com<IMFActivate>> decoders(const GUID& format) {
    return transforms(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT, format);
}

// Whether Windows has a decoder of `codec` installed (H.264's its own, the
// others' from its Video Extensions).
inline bool windowsDecodes(rcs_codec codec) {
    if (!mediaFoundationInstalled()) {
        return false;
    }
    MediaFoundation started;
    return !decoders(subtype(codec)).empty();
}

// The D3D11 video decoder profile of `codec` pictures of `format`, and the
// texture format they decode to. None (all zero) for 4:4:4: Windows'
// decoders decode none on the GPU (decoder.cpp), whatever profiles the GPU
// lists; nor for H.264 beyond 8 bits.
struct DecoderProfile {
    GUID profile;
    DXGI_FORMAT output;
};
inline DecoderProfile decoderProfile(rcs_codec codec, rcs_picture_format format) {
    // D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, missing from MinGW's headers.
    static constexpr GUID av1Profile0{0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}};
    const bool wide = bitDepth(format) > 8;
    const DXGI_FORMAT output = wide ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    if (chroma444(format)) {
        return {GUID{}, output};
    }
    switch (codec) {
    case RCS_CODEC_HEVC:
        return {wide ? D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10 : D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, output};
    case RCS_CODEC_VP8:
        return {wide ? GUID{} : D3D11_DECODER_PROFILE_VP8_VLD, output};
    case RCS_CODEC_VP9: // Profiles 0 and 2.
        return {wide ? D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2 : D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0, output};
    case RCS_CODEC_AV1:
        return {av1Profile0, output}; // AV1's Main profile: 8 and 10 bits.
    default:
        return {wide ? GUID{} : D3D11_DECODER_PROFILE_H264_VLD_NOFGT, output};
    }
}

// The picture formats `device`'s GPU decodes in `codec`, as
// (1u << rcs_picture_format) bits: those whose D3D11 decoder profile it
// lists, to their texture format (H.264 and VP8 to NV12; HEVC Main and Main
// 10, VP9 profiles 0 and 2, AV1 to NV12 and P010). Without one, a decoder
// given the device decodes on the CPU instead.
inline uint32_t gpuDecodedFormats(ID3D11Device* device, rcs_codec codec) {
    Com<ID3D11VideoDevice> video;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(video.out())))) {
        return 0;
    }
    std::vector<GUID> profiles;
    for (UINT i = 0, count = video->GetVideoDecoderProfileCount(); i < count; ++i) {
        if (GUID profile{}; SUCCEEDED(video->GetVideoDecoderProfile(i, &profile))) {
            profiles.push_back(profile);
        }
    }
    uint32_t formats = 0;
    for (const rcs_picture_format format : {RCS_PICTURE_420_8, RCS_PICTURE_420_10}) {
        const DecoderProfile wanted = decoderProfile(codec, format);
        BOOL supported = FALSE;
        if (wanted.profile != GUID{} && std::find(profiles.begin(), profiles.end(), wanted.profile) != profiles.end() &&
            SUCCEEDED(video->CheckVideoDecoderFormat(&wanted.profile, wanted.output, &supported)) && supported) {
            formats |= 1u << format;
        }
    }
    return formats;
}

// The codecs `device`'s GPU decodes, as (1u << rcs_codec) bits.
inline uint32_t gpuDecoders(ID3D11Device* device) {
    uint32_t decoded = 0;
    for (const rcs_codec codec : codecs) {
        if (gpuDecodedFormats(device, codec)) {
            decoded |= 1u << codec;
        }
    }
    return decoded;
}

// Whether `device`'s GPU, which decodes `codec` pictures of `format`
// (gpuDecodedFormats), decodes them at width x height. Up to 1920x1088
// always; beyond, as its driver answers for decoder configurations of that
// size, which is what Chromium relies on too.
inline bool gpuDecodes(ID3D11Device* device, rcs_codec codec, rcs_picture_format format, UINT width, UINT height) {
    if (width <= 1920 && height <= 1088) {
        return true;
    }
    Com<ID3D11VideoDevice> video;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(video.out())))) {
        return false;
    }
    const DecoderProfile profile = decoderProfile(codec, format);
    const D3D11_VIDEO_DECODER_DESC desc{profile.profile, width, height, profile.output};
    UINT configurations = 0;
    return SUCCEEDED(video->GetVideoDecoderConfigCount(&desc, &configurations)) && configurations > 0;
}

// The largest picture `device`'s GPU decodes in `codec` (one it has the
// profile of), one side at a time: the widest 1088 rows high and the tallest
// 1088 columns wide it decodes in 4:2:0 8-bit, at least 1920x1088, within the
// codec's limit.
inline PictureSize largestDecoded(ID3D11Device* device, rcs_codec codec) {
    const auto limit = codecLimit(codec);
    PictureSize largest{1920, 1088};
    for (const UINT side :
         {8192u, 7680u, 5120u, 4352u, 4320u, 4096u, 3840u, 2880u, 2560u, 2304u, 2160u, 1920u, 1440u, 1200u}) {
        if (side <= limit.width && side > largest.width && gpuDecodes(device, codec, RCS_PICTURE_420_8, side, 1088)) {
            largest.width = side;
        }
        if (side <= limit.height && side > largest.height &&
            gpuDecodes(device, codec, RCS_PICTURE_420_8, 1088, side)) {
            largest.height = side;
        }
    }
    return largest;
}

// A string attribute of a transform's activation, as UTF-8; empty when absent.
inline std::string stringAttribute(IMFActivate* activation, const GUID& key) {
    WCHAR* text = nullptr;
    UINT32 length = 0;
    if (!activation || FAILED(activation->GetAllocatedString(key, &text, &length))) {
        return {};
    }
    std::string result = narrow(std::wstring_view(text, length));
    CoTaskMemFree(text);
    return result;
}

// A transform's friendly name, e.g. "NVIDIA HEVC Encoder MFT".
inline std::string friendlyName(IMFActivate* activation) {
    return stringAttribute(activation, MFT_FRIENDLY_NAME_Attribute);
}

// Whether a transform starts (is activated, then shut down again); where it
// does not, `failure` says why.
inline bool starts(IMFActivate* activation, HRESULT& failure) {
    Com<IMFTransform> transform;
    failure = activation->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(transform.out()));
    if (FAILED(failure)) {
        return false;
    }
    transform.reset();
    activation->ShutdownObject();
    return true;
}

// Why a hardware encoder MFTEnum2 lists for a GPU does not start. NVIDIA's
// open a D3D11 device of their own when they start, on the GPU Windows gives
// programs by default (the first DXGI adapter), whichever GPU they were
// listed for, and fail (E_UNEXPECTED) where that GPU is not NVIDIA's: the
// display GPU of a hybrid laptop, a desktop whose main display is on its
// integrated GPU (driver 616.92, an RTX 4090 next to an Intel GPU whose
// adapter came first).
inline std::string startFailure(IMFActivate* encoder, HRESULT failure) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(failure));
    std::string why = friendlyName(encoder) + " does not start (HRESULT " + code + ")";
    const auto adapters = hardwareAdapters();
    if (_stricmp(stringAttribute(encoder, MFT_ENUM_HARDWARE_VENDOR_ID_Attribute).c_str(), "VEN_10DE") == 0 &&
        !adapters.empty() && adapters.front().vendorId != nvidiaVendor) {
        why += ": NVIDIA's Media Foundation encoders start only while an NVIDIA GPU is the one Windows gives "
               "programs by default (here " +
               narrow(adapters.front().description) +
               "; Windows' graphics settings choose it per program), and NVENC (RCS_BACKEND_NVENC) encodes on "
               "NVIDIA's GPUs either way";
    }
    return why;
}

// The picture formats a hardware encoder takes in `codec`, as
// (1u << rcs_picture_format) bits, from the inputs it registers
// (MFT_INPUT_TYPES_Attributes): 4:2:0 8-bit always (NV12); for HEVC 10 bits
// where P010 is among them, 4:4:4 where AYUV is, and 10-bit 4:4:4 where Y410
// is (NVIDIA's registers all three, Intel's P010 only). H.264 and AV1 stay
// 4:2:0 8-bit: NVIDIA's H.264 encoder refuses High 4:4:4 and Intel's takes
// NV12 only; NVIDIA's AV1 encoder registers P010 and takes the type, yet
// refuses P010 textures (MF_E_UNSUPPORTED_D3D_TYPE, RTX 4090; NVENC encodes
// AV1 10-bit there). Whether a stream is what was asked is checked at each
// keyframe (media/sampling.hpp).
inline uint32_t encoderFormats(IMFActivate* encoder, rcs_codec codec) {
    uint32_t formats = 1u << RCS_PICTURE_420_8;
    UINT8* blob = nullptr;
    UINT32 size = 0;
    if (codec != RCS_CODEC_HEVC || FAILED(encoder->GetAllocatedBlob(MFT_INPUT_TYPES_Attributes, &blob, &size))) {
        return formats;
    }
    const auto* types = reinterpret_cast<const MFT_REGISTER_TYPE_INFO*>(blob);
    for (UINT32 i = 0; i < size / sizeof(MFT_REGISTER_TYPE_INFO); ++i) {
        const GUID& subtype = types[i].guidSubtype;
        if (subtype == MFVideoFormat_P010) {
            formats |= 1u << RCS_PICTURE_420_10;
        } else if (subtype == MFVideoFormat_AYUV) {
            formats |= 1u << RCS_PICTURE_444_8;
        } else if (subtype == MFVideoFormat_Y410) {
            formats |= 1u << RCS_PICTURE_444_10;
        }
    }
    CoTaskMemFree(blob);
    return formats;
}

// MFSampleExtension_LongTermReferenceFrameInfo (mfapi.h; MinGW's lacks it): on
// an encoder's output sample, the long-term reference slot the picture was
// marked as (bits 0..15, 0xffff for none) and those it used (bits 16..31).
inline constexpr GUID longTermReferenceInfo = {0x9154733f, 0xe1bd, 0x41bf,
                                               {0x81, 0xd3, 0xfc, 0xd9, 0x18, 0xf7, 0x13, 0x32}};

// An activated transform. Shutting the activation down releases resources a
// hardware transform holds, such as encoder sessions.
class Transform {
public:
    Transform() = default;
    ~Transform() { reset(); }
    Transform(const Transform&) = delete;
    Transform& operator=(const Transform&) = delete;

    // Activates the first transform in `candidates` that can be created;
    // where none can, `failure` (if given) says why the last could not.
    bool activate(std::vector<Com<IMFActivate>>& candidates, HRESULT* failure = nullptr) {
        reset();
        for (auto& candidate : candidates) {
            const HRESULT result =
                candidate->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(transform_.out()));
            if (SUCCEEDED(result)) {
                activation_ = std::move(candidate);
                return true;
            }
            if (failure) {
                *failure = result;
            }
        }
        return false;
    }
    void reset() noexcept {
        transform_.reset();
        if (activation_) {
            activation_->ShutdownObject();
            activation_.reset();
        }
    }
    std::string name() const { return friendlyName(activation_.value); }
    IMFTransform* operator->() const noexcept { return transform_.value; }
    IMFTransform* get() const noexcept { return transform_.value; }
    explicit operator bool() const noexcept { return bool(transform_); }

private:
    Com<IMFActivate> activation_;
    Com<IMFTransform> transform_;
};

inline uint64_t packLuid(const LUID& luid) noexcept {
    return (uint64_t(uint32_t(luid.HighPart)) << 32) | luid.LowPart;
}

inline LUID adapterLuid(ID3D11Device* device) {
    Com<IDXGIDevice> dxgi;
    check(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.out())), "DXGI device");
    Com<IDXGIAdapter> adapter;
    check(dxgi->GetAdapter(adapter.out()), "DXGI adapter");
    DXGI_ADAPTER_DESC desc{};
    check(adapter->GetDesc(&desc), "DXGI adapter description");
    return desc.AdapterLuid;
}

inline Com<IMFMediaType> videoType(const GUID& subtype, uint32_t width, uint32_t height, uint32_t framesPerSecond) {
    Com<IMFMediaType> type;
    check(mfplat().createMediaType(type.out()), "Media type creation", RCS_ERROR_CODEC);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type.value, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.value, MF_MT_FRAME_RATE, framesPerSecond, 1);
    MFSetAttributeRatio(type.value, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return type;
}

// Copies a sample's bytes into contiguous memory.
inline std::vector<uint8_t> sampleBytes(IMFSample* sample) {
    Com<IMFMediaBuffer> buffer;
    check(sample->ConvertToContiguousBuffer(buffer.out()), "Sample buffer", RCS_ERROR_CODEC);
    BYTE* data = nullptr;
    DWORD length = 0;
    check(buffer->Lock(&data, nullptr, &length), "Sample buffer map", RCS_ERROR_CODEC);
    std::vector<uint8_t> bytes(data, data + length);
    buffer->Unlock();
    return bytes;
}

} // namespace rcsdk::platform::win
