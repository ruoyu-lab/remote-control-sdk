// NVDEC (nvdec.hpp). Its parser calls back from within cuvidParseVideoData
// when the stream's format is known (the decoder is made, again for a new
// one), when a picture is ready to decode and when one is ready to show; with
// no display delay and each packet a whole picture, a packet's picture shows
// within the same call. The picture, planar YCbCr 4:4:4 in CUDA memory (8
// bits, or 10 in the high bits of 16), is copied into a Direct3D 11 texture
// array of its three planes registered with CUDA, and a shader of ours draws
// it into a BGRA8 texture of its own with the stream's matrix and range
// (bgra_shader.hpp; the texture is reused once nobody holds it,
// picture_pool.hpp).
#include "platform/windows/nvdec.hpp"

#include "platform/windows/adapters.hpp"
#include "platform/windows/bgra_shader.hpp"
#include "platform/windows/media_foundation.hpp"
#include "platform/windows/picture_pool.hpp"

#include "util/log.hpp"

// The headers' own includes, first: inside the namespace they add nothing.
// The namespace keeps their names (TIMECODE) from Windows' own.
#include <stddef.h>
#include <stdint.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnested-anon-types" // Structures in their unions.
#endif
namespace rcsdk::platform::win::nvdec {
#include <nvdec/dynlink_cuda.h>
#include <nvdec/dynlink_nvcuvid.h>
} // namespace rcsdk::platform::win::nvdec
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <type_traits>

namespace rcsdk::platform::win {
namespace {

using namespace nvdec;

constexpr rcs_result codecError = RCS_ERROR_CODEC;

// The CUDA driver's Direct3D 11 interop (the toolkit's cudaD3D11.h, which
// nv-codec-headers leave out).
typedef CUresult CUDAAPI tcuGraphicsD3D11RegisterResource(CUgraphicsResource* resource, ID3D11Resource* texture,
                                                          unsigned int flags);

// The driver's functions, loaded once; none where `unavailable` says why.
struct Driver {
    tcuInit* init = nullptr;
    tcuDeviceGetCount* deviceCount = nullptr;
    tcuDeviceGet* device = nullptr;
    tcuDeviceGetLuid* deviceLuid = nullptr;
    tcuDevicePrimaryCtxRetain* retain = nullptr;
    tcuDevicePrimaryCtxRelease* release = nullptr;
    tcuCtxPushCurrent_v2* push = nullptr;
    tcuCtxPopCurrent_v2* pop = nullptr;
    tcuMemcpy2D_v2* copy = nullptr;
    tcuGraphicsD3D11RegisterResource* registerTexture = nullptr;
    tcuGraphicsUnregisterResource* unregisterTexture = nullptr;
    tcuGraphicsMapResources* map = nullptr;
    tcuGraphicsUnmapResources* unmap = nullptr;
    tcuGraphicsSubResourceGetMappedArray* mappedArray = nullptr;
    tcuGetErrorName* errorName = nullptr;
    tcuvidGetDecoderCaps* decoderCaps = nullptr;
    tcuvidCreateDecoder* createDecoder = nullptr;
    tcuvidDestroyDecoder* destroyDecoder = nullptr;
    tcuvidDecodePicture* decodePicture = nullptr;
    tcuvidMapVideoFrame64* mapFrame = nullptr;
    tcuvidUnmapVideoFrame64* unmapFrame = nullptr;
    tcuvidCreateVideoParser* createParser = nullptr;
    tcuvidParseVideoData* parse = nullptr;
    tcuvidDestroyVideoParser* destroyParser = nullptr;
    std::string unavailable;
};

const Driver& driver() {
    static const Driver result = [] {
        Driver d;
        HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE cuvid = cuda ? LoadLibraryExW(L"nvcuvid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
        if (!cuvid) {
            d.unavailable = "it needs an NVIDIA GPU and its driver (nvcuda.dll, nvcuvid.dll)";
            return d;
        }
        const auto resolve = [&](HMODULE module, auto& function, const char* name) {
            if (d.unavailable.empty()) {
                function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(module, name));
                if (!function) {
                    d.unavailable = std::string("the NVIDIA driver has no ") + name;
                }
            }
        };
        resolve(cuda, d.init, "cuInit");
        resolve(cuda, d.deviceCount, "cuDeviceGetCount");
        resolve(cuda, d.device, "cuDeviceGet");
        resolve(cuda, d.deviceLuid, "cuDeviceGetLuid");
        resolve(cuda, d.retain, "cuDevicePrimaryCtxRetain");
        resolve(cuda, d.release, "cuDevicePrimaryCtxRelease");
        resolve(cuda, d.push, "cuCtxPushCurrent_v2");
        resolve(cuda, d.pop, "cuCtxPopCurrent_v2");
        resolve(cuda, d.copy, "cuMemcpy2D_v2");
        resolve(cuda, d.registerTexture, "cuGraphicsD3D11RegisterResource");
        resolve(cuda, d.unregisterTexture, "cuGraphicsUnregisterResource");
        resolve(cuda, d.map, "cuGraphicsMapResources");
        resolve(cuda, d.unmap, "cuGraphicsUnmapResources");
        resolve(cuda, d.mappedArray, "cuGraphicsSubResourceGetMappedArray");
        resolve(cuda, d.errorName, "cuGetErrorName");
        resolve(cuvid, d.decoderCaps, "cuvidGetDecoderCaps");
        resolve(cuvid, d.createDecoder, "cuvidCreateDecoder");
        resolve(cuvid, d.destroyDecoder, "cuvidDestroyDecoder");
        resolve(cuvid, d.decodePicture, "cuvidDecodePicture");
        resolve(cuvid, d.mapFrame, "cuvidMapVideoFrame64");
        resolve(cuvid, d.unmapFrame, "cuvidUnmapVideoFrame64");
        resolve(cuvid, d.createParser, "cuvidCreateVideoParser");
        resolve(cuvid, d.parse, "cuvidParseVideoData");
        resolve(cuvid, d.destroyParser, "cuvidDestroyVideoParser");
        if (d.unavailable.empty() && d.init(0) != CUDA_SUCCESS) {
            d.unavailable = "CUDA does not start here (cuInit failed)";
        }
        return d; // The modules stay loaded for the process's lifetime.
    }();
    return result;
}

void check(CUresult result, const char* operation, rcs_result category = codecError) {
    if (result == CUDA_SUCCESS) {
        return;
    }
    const char* name = nullptr;
    driver().errorName(result, &name);
    fail(category, std::string("NVDEC: ") + operation + " failed (" + (name ? name : "CUDA error") + ", " +
                       std::to_string(int(result)) + ")");
}

cudaVideoCodec cudaCodec(rcs_codec codec) {
    switch (codec) {
    case RCS_CODEC_H264:
        return cudaVideoCodec_H264;
    case RCS_CODEC_HEVC:
        return cudaVideoCodec_HEVC;
    case RCS_CODEC_VP8:
        return cudaVideoCodec_VP8;
    case RCS_CODEC_VP9:
        return cudaVideoCodec_VP9;
    default:
        return cudaVideoCodec_AV1;
    }
}

// The CUDA device of the GPU `luid` names.
std::optional<CUdevice> cudaDevice(const LUID& luid) {
    int count = 0;
    if (driver().deviceCount(&count) != CUDA_SUCCESS) {
        return std::nullopt;
    }
    for (int i = 0; i < count; ++i) {
        CUdevice device = 0;
        char id[sizeof(LUID)]{};
        unsigned int nodes = 0;
        if (driver().device(&device, i) == CUDA_SUCCESS && driver().deviceLuid(id, &nodes, device) == CUDA_SUCCESS &&
            std::memcmp(id, &luid, sizeof(id)) == 0) {
            return device;
        }
    }
    return std::nullopt;
}

// A GPU's primary CUDA context, held while this lives.
class PrimaryContext {
public:
    explicit PrimaryContext(CUdevice device) : device_(device) {
        check(driver().retain(&context_, device), "Opening the GPU's CUDA context");
    }
    ~PrimaryContext() { driver().release(device_); }
    PrimaryContext(const PrimaryContext&) = delete;
    PrimaryContext& operator=(const PrimaryContext&) = delete;
    CUcontext get() const noexcept { return context_; }

private:
    CUdevice device_;
    CUcontext context_ = nullptr;
};

// `context` current on this thread while this lives.
class Current {
public:
    explicit Current(CUcontext context) { check(driver().push(context), "Making the CUDA context current"); }
    ~Current() {
        CUcontext popped = nullptr;
        driver().pop(&popped);
    }
    Current(const Current&) = delete;
    Current& operator=(const Current&) = delete;
};

bool decodes(const NvdecAdapter& gpu, rcs_codec codec, rcs_picture_format format) {
    return codec <= RCS_CODEC_AV1 && (gpu.formats[codec] & (1u << format));
}

// What 4:4:4 the GPU decodes of each codec; its CUDA context is current.
void probe(NvdecAdapter& gpu) {
    for (const rcs_codec codec : {RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_VP9, RCS_CODEC_AV1}) {
        for (const rcs_picture_format format : {RCS_PICTURE_444_8, RCS_PICTURE_444_10}) {
            const bool wide = bitDepth(format) > 8;
            CUVIDDECODECAPS caps{};
            caps.eCodecType = cudaCodec(codec);
            caps.eChromaFormat = cudaVideoChromaFormat_444;
            caps.nBitDepthMinus8 = bitDepth(format) - 8;
            const unsigned output = wide ? cudaVideoSurfaceFormat_YUV444_16Bit : cudaVideoSurfaceFormat_YUV444;
            if (driver().decoderCaps(&caps) != CUDA_SUCCESS || !caps.bIsSupported ||
                !(caps.nOutputFormatMask & (1u << output))) {
                continue;
            }
            const auto limit = codecLimit(codec);
            const PictureSize largest{std::min(limit.width, caps.nMaxWidth), std::min(limit.height, caps.nMaxHeight)};
            PictureSize& known = gpu.largest[codec];
            known = gpu.formats[codec]
                        ? PictureSize{std::min(known.width, largest.width), std::min(known.height, largest.height)}
                        : largest;
            gpu.codecs |= 1u << codec;
            gpu.formats[codec] |= 1u << format;
        }
    }
}

class NvdecDecoder final : public VideoDecoder {
public:
    ~NvdecDecoder() override { close(); }

    // Takes the device, the CUDA context and the parser now: what this GPU
    // does not decode fails here; the decoder itself is made at the stream's
    // first keyframe.
    void open(const DecoderConfig& config) override {
        config_ = config;
        const rcs_picture_format format = config.picture.format;
        if (config.acceleration != RCS_ACCELERATION_HARDWARE) {
            fail(RCS_ERROR_UNSUPPORTED, "NVDEC decodes in hardware only");
        }
        if (!driver().unavailable.empty()) {
            fail(RCS_ERROR_UNSUPPORTED, "NVDEC is not available: " + driver().unavailable);
        }
        if (!chroma444(format)) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("NVDEC decodes 4:4:4 pictures here; Media Foundation decodes ") +
                                            pictureFormatName(format) +
                                            " ones on the GPU (RCS_BACKEND_MEDIA_FOUNDATION, or AUTO)");
        }
        const auto& gpus = nvdecAdapters();
        const NvdecAdapter* gpu = nullptr;
        if (config.device) {
            d3d_.adopt(static_cast<ID3D11Device*>(config.device.get()));
            const DXGI_ADAPTER_DESC adapter = adapterOf(d3d_.device.value);
            if (adapter.VendorId != nvidiaVendor) {
                fail(RCS_ERROR_UNSUPPORTED, "The decoding GPU (" + narrow(adapter.Description) +
                                                ") is not NVIDIA's: NVDEC cannot decode for it (choose "
                                                "RCS_BACKEND_MEDIA_FOUNDATION, or AUTO)");
            }
            for (const NvdecAdapter& candidate : gpus) {
                if (packLuid(candidate.luid) == packLuid(adapter.AdapterLuid)) {
                    gpu = &candidate;
                }
            }
            if (!gpu || !decodes(*gpu, config.codec, format)) {
                fail(RCS_ERROR_UNSUPPORTED, narrow(adapter.Description) + " does not decode " +
                                                codecName(config.codec) + " in " + pictureFormatName(format));
            }
        } else {
            for (const NvdecAdapter& candidate : gpus) {
                if (!gpu && decodes(candidate, config.codec, format)) {
                    gpu = &candidate;
                }
            }
            if (!gpu) {
                fail(RCS_ERROR_UNSUPPORTED, std::string("No NVIDIA GPU here decodes ") + codecName(config.codec) +
                                                " in " + pictureFormatName(format));
            }
            const auto adapter = adapterWith(gpu->luid);
            if (!adapter) { // Gone since it was probed (removed, or its driver restarted).
                fail(codecError, "The NVIDIA GPU " + gpu->name + " is no longer among the system's adapters");
            }
            d3d_.create(adapter.value);
        }
        name_ = nvdecName(config.codec, gpu->name);
        largest_ = gpu->largest[config.codec];
        const auto device = cudaDevice(gpu->luid);
        if (!device) {
            fail(codecError, "CUDA no longer sees " + gpu->name);
        }
        context_.emplace(*device);
        createParser();
    }

    bool decode(const EncodedPacket& packet, VideoFrame& frame, CancelToken) override {
        const std::vector<uint8_t> unit = accessUnit(config_.codec, packet);
        CUVIDSOURCEDATAPACKET data{};
        data.flags = CUVID_PKT_ENDOFPICTURE | CUVID_PKT_TIMESTAMP;
        data.payload_size = tcu_ulong(unit.size());
        data.payload = unit.data();
        data.timestamp = CUvideotimestamp(packet.sequence); // Carries the sequence through the decoder.
        shown_.reset();
        Current current(context_->get());
        const CUresult parsed = driver().parse(parser_, &data);
        if (error_) {
            std::rethrow_exception(std::exchange(error_, nullptr));
        }
        check(parsed, "Parsing the stream");
        if (!shown_) {
            return false;
        }
        const UINT width = UINT(format_.display_area.right - format_.display_area.left);
        const UINT height = UINT(format_.display_area.bottom - format_.display_area.top);
        if (packet.width != width || packet.height != height) {
            fail(codecError, "The stream announced " + std::to_string(packet.width) + "x" +
                                 std::to_string(packet.height) + " pictures; they decode to " + std::to_string(width) +
                                 "x" + std::to_string(height));
        }
        copyPlanes(*shown_, width, height);
        frame.surface =
            textures_.next(d3d_.device.value, textureDesc(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
        draw(static_cast<ID3D11Texture2D*>(frame.surface.get()), width, height, packet.picture);
        frame.sequence = uint64_t(shown_->timestamp);
        frame.width = width;
        frame.height = height;
        frame.color = rgbColor(colorOf(packet.picture));
        frame.device = share(d3d_.device.value);
        frame.waitGpu = gpuFence(d3d_.device.value, d3d_.context.value);
        return true;
    }

    // A parser that has seen nothing; the decoder stays for the next
    // keyframe of the same format.
    void reset() override {
        Current current(context_->get());
        driver().destroyParser(parser_);
        parser_ = nullptr;
        createParser();
    }

    std::string name() const override { return name_; }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_NVDEC; }

private:
    // The planes of the picture shown last, registered with CUDA.
    struct Planes {
        Com<ID3D11Texture2D> texture;
        Com<ID3D11ShaderResourceView> view;
        CUgraphicsResource registered = nullptr;
    };

    void createParser() {
        CUVIDPARSERPARAMS params{};
        params.CodecType = cudaCodec(config_.codec);
        params.ulMaxNumDecodeSurfaces = 1; // The sequence callback says how many.
        params.ulErrorThreshold = 100;     // Shows what it decoded, as Windows' decoders do.
        params.ulMaxDisplayDelay = 0;      // Each picture as soon as it is decoded.
        params.pUserData = this;
        params.pfnSequenceCallback = sequenceReady;
        params.pfnDecodePicture = pictureReady;
        params.pfnDisplayPicture = pictureShown;
        Current current(context_->get());
        check(driver().createParser(&parser_, &params), "Creating the parser");
    }

    // The parser's calls. A failure is kept for decode() to throw (it cannot
    // pass through the parser) and stops the parse (0).
    static int CUDAAPI sequenceReady(void* self, CUVIDEOFORMAT* format) {
        auto* decoder = static_cast<NvdecDecoder*>(self);
        try {
            return decoder->configure(*format);
        } catch (...) {
            decoder->error_ = std::current_exception();
            return 0;
        }
    }
    static int CUDAAPI pictureReady(void* self, CUVIDPICPARAMS* picture) {
        auto* decoder = static_cast<NvdecDecoder*>(self);
        try {
            check(driver().decodePicture(decoder->decoder_, picture), "Decoding a picture");
            return 1;
        } catch (...) {
            decoder->error_ = std::current_exception();
            return 0;
        }
    }
    static int CUDAAPI pictureShown(void* self, CUVIDPARSERDISPINFO* shown) {
        if (shown) {
            static_cast<NvdecDecoder*>(self)->shown_ = *shown;
        }
        return 1;
    }

    // The stream's format (on its first keyframe, and whenever it changes):
    // a decoder of it, and planes of its pictures' size. Returns the decode
    // surfaces the parser cycles through.
    int configure(const CUVIDEOFORMAT& format) {
        const bool wide = format.bit_depth_luma_minus8 > 0;
        if (format.chroma_format != cudaVideoChromaFormat_444 || format.bit_depth_luma_minus8 > 2 ||
            wide != (bitDepth(config_.picture.format) > 8)) {
            fail(codecError, std::string("The stream's pictures are not ") + pictureFormatName(config_.picture.format) +
                                 ", as it said");
        }
        const auto& a = format.display_area;
        const auto& b = format_.display_area;
        if (decoder_ && format.coded_width == format_.coded_width && format.coded_height == format_.coded_height &&
            format.bit_depth_luma_minus8 == format_.bit_depth_luma_minus8 && a.left == b.left && a.top == b.top &&
            a.right == b.right && a.bottom == b.bottom) {
            return format.min_num_decode_surfaces;
        }
        destroyDecoder();
        const UINT width = UINT(a.right - a.left), height = UINT(a.bottom - a.top);
        const std::string size = std::to_string(width) + "x" + std::to_string(height);
        if (!takes(narrowed(codecLimit(config_.codec), largest_.width, largest_.height), format.coded_width,
                   format.coded_height)) {
            fail(codecError, name_ + " cannot decode " + size + " pictures: it takes " +
                                 limitText(narrowed(codecLimit(config_.codec), largest_.width, largest_.height)));
        }
        CUVIDDECODECREATEINFO info{};
        info.ulWidth = info.ulMaxWidth = format.coded_width;
        info.ulHeight = info.ulMaxHeight = format.coded_height;
        info.ulNumDecodeSurfaces = format.min_num_decode_surfaces;
        info.CodecType = format.codec;
        info.ChromaFormat = format.chroma_format;
        info.ulCreationFlags = cudaVideoCreate_PreferCUVID;
        info.bitDepthMinus8 = format.bit_depth_luma_minus8;
        info.display_area.left = short(a.left);
        info.display_area.top = short(a.top);
        info.display_area.right = short(a.right);
        info.display_area.bottom = short(a.bottom);
        info.OutputFormat = wide ? cudaVideoSurfaceFormat_YUV444_16Bit : cudaVideoSurfaceFormat_YUV444;
        info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
        info.ulTargetWidth = width;
        info.ulTargetHeight = height;
        info.ulNumOutputSurfaces = 1;
        check(driver().createDecoder(&decoder_, &info), ("Creating a decoder for " + size + " pictures").c_str());
        format_ = format;
        createPlanes(width, height, wide);
        return format.min_num_decode_surfaces;
    }

    // A texture array of the Y, Cb and Cr planes, which CUDA writes and the
    // shader reads.
    void createPlanes(UINT width, UINT height, bool wide) {
        auto desc =
            textureDesc(width, height, wide ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        desc.ArraySize = 3;
        win::check(d3d_.device->CreateTexture2D(&desc, nullptr, planes_.texture.out()), "Decoded planes", codecError);
        win::check(d3d_.device->CreateShaderResourceView(planes_.texture.value, nullptr, planes_.view.out()),
                   "Decoded planes view", codecError);
        check(driver().registerTexture(&planes_.registered, planes_.texture.value, CU_GRAPHICS_REGISTER_FLAGS_NONE),
              "Sharing the planes with CUDA");
    }

    // The picture `shown` into the planes. Mapping waits for its decoding.
    void copyPlanes(const CUVIDPARSERDISPINFO& shown, UINT width, UINT height) {
        CUVIDPROCPARAMS processing{};
        processing.progressive_frame = shown.progressive_frame;
        processing.top_field_first = shown.top_field_first;
        processing.unpaired_field = shown.repeat_first_field < 0;
        unsigned long long pointer = 0;
        unsigned int pitch = 0;
        check(driver().mapFrame(decoder_, shown.picture_index, &pointer, &pitch, &processing),
              "Reading a decoded picture");
        struct Unmap {
            CUvideodecoder decoder;
            unsigned long long pointer;
            ~Unmap() { driver().unmapFrame(decoder, pointer); }
        } unmapFrame{decoder_, pointer};
        CUgraphicsResource resource = planes_.registered;
        check(driver().map(1, &resource, nullptr), "Mapping the planes");
        struct Release {
            CUgraphicsResource resource;
            ~Release() { driver().unmap(1, &resource, nullptr); }
        } unmapPlanes{resource};
        // Each plane follows the one before at its height rounded up to even.
        const size_t planeRows = (size_t(height) + 1) & ~size_t(1);
        const size_t sample = format_.bit_depth_luma_minus8 > 0 ? 2 : 1;
        for (unsigned int plane = 0; plane < 3; ++plane) {
            CUarray array = nullptr;
            check(driver().mappedArray(&array, resource, plane, 0), "Reaching a plane");
            CUDA_MEMCPY2D copy{};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice = CUdeviceptr(pointer + size_t(pitch) * planeRows * plane);
            copy.srcPitch = pitch;
            copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
            copy.dstArray = array;
            copy.WidthInBytes = size_t(width) * sample;
            copy.Height = height;
            check(driver().copy(&copy), "Copying a decoded plane");
        }
    }

    // The planes to BGRA in `target`, `width` x `height`.
    void draw(ID3D11Texture2D* target, UINT width, UINT height, const Picture& picture) {
        shader_.draw(d3d_.device.value, d3d_.context.value, planes_.view.value,
                     format_.bit_depth_luma_minus8 > 0 ? BgraShader::Source::planes16 : BgraShader::Source::planes8,
                     target, width, height, picture);
    }

    void destroyDecoder() noexcept {
        if (planes_.registered) {
            driver().unregisterTexture(planes_.registered);
        }
        planes_ = {};
        if (decoder_) {
            driver().destroyDecoder(decoder_);
            decoder_ = nullptr;
        }
    }

    void close() noexcept {
        if (!context_) {
            return;
        }
        if (driver().push(context_->get()) == CUDA_SUCCESS) {
            if (parser_) {
                driver().destroyParser(parser_);
            }
            destroyDecoder();
            CUcontext popped = nullptr;
            driver().pop(&popped);
        }
        context_.reset();
    }

    DecoderConfig config_;
    std::string name_;
    PictureSize largest_;
    Device d3d_;
    std::optional<PrimaryContext> context_;
    CUvideoparser parser_ = nullptr;
    CUvideodecoder decoder_ = nullptr;
    CUVIDEOFORMAT format_{}; // The decoder's.
    Planes planes_;
    BgraShader shader_;
    TexturePool textures_; // The pictures handed out.
    // What the parser said in the last call: the picture to show, a failure.
    std::optional<CUVIDPARSERDISPINFO> shown_;
    std::exception_ptr error_;
};

} // namespace

const std::vector<NvdecAdapter>& nvdecAdapters() {
    static const std::vector<NvdecAdapter> result = [] {
        std::vector<NvdecAdapter> found;
        if (!driver().unavailable.empty()) {
            return found;
        }
        for (const Adapter& adapter : hardwareAdapters()) {
            if (adapter.vendorId != nvidiaVendor) {
                continue;
            }
            NvdecAdapter gpu;
            gpu.luid = adapter.luid;
            gpu.name = narrow(adapter.description);
            if (const auto device = cudaDevice(adapter.luid)) {
                try {
                    PrimaryContext context(*device);
                    Current current(context.get());
                    probe(gpu);
                } catch (const Failure& failure) {
                    log::info("decoder", "NVDEC on " + gpu.name + ": " + failure.what());
                }
            }
            if (gpu.codecs) {
                found.push_back(std::move(gpu));
            }
        }
        return found;
    }();
    return result;
}

std::string nvdecName(rcs_codec codec, const std::string& gpu) {
    return std::string("NVDEC ") + codecName(codec) + " (" + gpu + ")";
}

bool nvdecFirst(rcs_codec codec, rcs_picture_format format, ID3D11Device* device) {
    if (!chroma444(format)) {
        return false;
    }
    const auto& gpus = nvdecAdapters();
    const uint64_t luid = device ? packLuid(adapterOf(device).AdapterLuid) : 0;
    return std::any_of(gpus.begin(), gpus.end(), [&](const NvdecAdapter& gpu) {
        return (!device || packLuid(gpu.luid) == luid) && decodes(gpu, codec, format);
    });
}

std::unique_ptr<VideoDecoder> createNvdecDecoder() {
    return std::make_unique<NvdecDecoder>();
}

} // namespace rcsdk::platform::win
