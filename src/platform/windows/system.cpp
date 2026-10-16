// Windows system queries, permissions and frame access.
#include "platform/windows/adapters.hpp"
#include "platform/windows/d3d.hpp"
#include "platform/windows/desktop.hpp"
#include "platform/windows/display.hpp"
#include "platform/windows/media_foundation.hpp"
#include "platform/windows/network.hpp"
#include "platform/windows/nvdec.hpp"
#include "platform/windows/nvenc.hpp"
#include "platform/windows/onevpl.hpp"
#include "platform/windows/pointer_input.hpp"
#include "platform/windows/window_capture.hpp"

#include "media/yuv.hpp"
#include "media/picture_size.hpp"
#include "platform/decode_probes.hpp"
#include "platform/platform.hpp"

#include <d3d11.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

namespace rcsdk::platform {

namespace {

// Codecs the default GPU decodes, and the largest picture and the picture formats of each.
struct GpuDecoding {
    uint32_t codecs = 0;
    uint64_t adapterLuid = 0;
    std::array<PictureSize, RCS_CODEC_AV1 + 1> largest{}; // By codec...
    std::array<uint32_t, RCS_CODEC_AV1 + 1> formats{};    // ...as (1u << rcs_picture_format) bits.
};

GpuDecoding hardwareDecoders() {
    GpuDecoding result;
    try {
        win::Device d3d;
        d3d.create();
        result.adapterLuid = win::packLuid(win::adapterLuid(d3d.device.value));
        result.codecs = win::gpuDecoders(d3d.device.value);
        for (const rcs_codec codec : win::codecs) {
            if (result.codecs & (1u << codec)) {
                result.largest[codec] = win::largestDecoded(d3d.device.value, codec);
                result.formats[codec] = win::gpuDecodedFormats(d3d.device.value, codec);
            }
        }
    } catch (const Failure&) { // No hardware device: software decoding only.
    }
    return result;
}

// What Windows' own software decoders take, as documented: H.264 and HEVC at
// most 4096x2304 (learn.microsoft.com, "H.264 Video Decoder" and "H.265 / HEVC
// Video Decoder"); the VP8, VP9 and AV1 extensions' say nothing, so their
// codecs' limits.
PictureSize softwareDecoded(rcs_codec codec) {
    if (codec == RCS_CODEC_H264 || codec == RCS_CODEC_HEVC) {
        return {4096, 2304};
    }
    const auto limit = codecLimit(codec);
    return {limit.width, limit.height};
}

// Whether the software decoder of `codec` decodes the keyframe of `format`
// (decode_probes.hpp).
bool softwareDecodes(rcs_codec codec, const probes::Probe& probe) {
    try {
        DecoderConfig settings;
        settings.codec = codec;
        settings.acceleration = RCS_ACCELERATION_SOFTWARE;
        settings.cpuImages = true;
        settings.picture.format = probe.format;
        auto decoder = createVideoDecoder(settings);
        decoder->open(settings);
        EncodedPacket packet;
        packet.codec = codec;
        packet.picture.format = probe.format;
        packet.width = probes::width;
        packet.height = probes::height;
        packet.keyframe = true;
        packet.config.assign(probe.config.begin(), probe.config.end());
        packet.data.assign(probe.data.begin(), probe.data.end());
        VideoFrame frame;
        static const std::atomic<bool> never{false};
        return decoder->decode(packet, frame, never) && frame.width == probes::width;
    } catch (const Failure&) {
        return false;
    }
}

// The picture formats they decode: H.264 4:2:0 8-bit; HEVC Main, Main 10 and
// the 4:4:4 range extensions at 8 and 10 bits (Microsoft documents Main and
// Main 10 only, but the HEVC Video Extension decodes 4:4:4 too, found by
// feeding it 4:4:4 keyframes); AV1's Main profile (4:2:0 at 8 and 10 bits);
// VP8 4:2:0 8-bit; VP9 4:2:0 8-bit and the profiles whose probes it decodes.
uint32_t softwareFormats(rcs_codec codec) {
    switch (codec) {
    case RCS_CODEC_HEVC:
        return sdrPictureFormats;
    case RCS_CODEC_AV1:
        return defaultPictureFormats | (1u << RCS_PICTURE_420_10);
    case RCS_CODEC_VP9: {
        uint32_t formats = defaultPictureFormats;
        for (const auto& probe : probes::all) {
            if (probe.codec == codec && softwareDecodes(codec, probe)) {
                formats |= 1u << probe.format;
            }
        }
        return formats;
    }
    default:
        return defaultPictureFormats;
    }
}

// Whether input injected from this process reaches the desktop that receives
// input now: the user's own (win::userDesktop), or with `secureDesktop` any
// this process can open (LocalSystem opens the secure ones).
bool inputDesktopReachable(bool secureDesktop) {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP);
    if (!desktop) {
        return false;
    }
    if (secureDesktop) {
        CloseDesktop(desktop);
        return true;
    }
    const bool normal = win::nameOf(desktop) == win::userDesktop;
    CloseDesktop(desktop);
    return normal;
}

} // namespace

rcs_capabilities capabilities() {
    rcs_capabilities value = RCS_CAPABILITIES_INIT;
    value.platform = RCS_PLATFORM_WINDOWS;
    value.capture_sources = (1u << RCS_CAPTURE_DISPLAY) | (1u << RCS_CAPTURE_REGION) | (1u << RCS_CAPTURE_APPLICATION) |
                            (1u << RCS_CAPTURE_HOST) | (1u << RCS_CAPTURE_ENCODED);
    if (win::windowCaptureAvailable()) {
        value.capture_sources |= 1u << RCS_CAPTURE_WINDOW;
    }
    // Capture draws BGRA; each encoder converts it to the YCbCr it encodes.
    value.picture_formats = sdrPictureFormats;
    value.secure_desktop = secureDesktopAvailable();
    value.secure_attention = secureAttentionAvailable();
    for (const rcs_input_type type : {RCS_INPUT_MOVE, RCS_INPUT_MOVE_RELATIVE, RCS_INPUT_BUTTON, RCS_INPUT_WHEEL,
                                      RCS_INPUT_SCROLL, RCS_INPUT_KEY, RCS_INPUT_TEXT, RCS_INPUT_RELEASE_ALL}) {
        value.input_types |= 1u << type;
    }
    if (win::syntheticPointersAvailable()) {
        value.input_types |= (1u << RCS_INPUT_TOUCH) | (1u << RCS_INPUT_PEN);
    }
    if (value.secure_attention) {
        value.input_types |= 1u << RCS_INPUT_SECURE_ATTENTION;
    }
    value.software_decoding_gpu_frames = 1; // Software decoding's pictures are uploaded as BGRA8 textures.
    value.wifi_low_latency = win::wifiSettable();
    return value;
}

std::shared_ptr<void> stayResponsive() {
    return nullptr;
}

std::vector<CodecImplementation> codecImplementations() {
    std::vector<CodecImplementation> result;
    std::optional<win::MediaFoundation> mf; // Without it only NVENC: no transforms.
    if (win::mediaFoundationInstalled()) {
        mf.emplace();
    }
    const auto adapters = win::hardwareAdapters();
    const GpuDecoding gpu = hardwareDecoders();
    const auto& nvencAdapters = win::nvencAdapters();
    const auto& vplAdapters = win::vplAdapters();
    constexpr rcs_backend backend = RCS_BACKEND_MEDIA_FOUNDATION;
    for (const rcs_codec codec : win::codecs) {
        const GUID& format = win::subtype(codec);
        // Media Foundation's encoders say nothing of the sizes they take.
        const auto limit = codecLimit(codec);
        // A sharer encodes on the GPU driving its display. Adapters without
        // encoders of their own (virtual display adapters) get every GPU's
        // from the enumeration; a transform's vendor tells them apart. An
        // encoder that does not start is not offered (NVIDIA's where the GPU
        // Windows gives programs by default is another: win::startFailure).
        for (const win::Adapter& adapter : adapters) {
            // NVENC on an NVIDIA GPU, before its Media Foundation encoder (win::nvencFirst).
            const auto nvenc =
                std::find_if(nvencAdapters.begin(), nvencAdapters.end(), [&](const win::NvencAdapter& n) {
                    return win::packLuid(n.luid) == win::packLuid(adapter.luid);
                });
            if (nvenc != nvencAdapters.end() && (nvenc->codecs & (1u << codec))) {
                const PictureSize largest = nvenc->largest[codec];
                result.push_back({codec, RCS_CODEC_ENCODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_NVENC,
                                  win::packLuid(adapter.luid), largest.width, largest.height, nvenc->formats[codec],
                                  win::nvencName(codec, nvenc->name), (nvenc->refInvalidation & (1u << codec)) != 0,
                                  std::max(1u, nvenc->temporalLayers[codec]), nvenc->pixelRates[codec]});
            }
            // oneVPL on an Intel GPU, before its Media Foundation encoder (win::vplFirst).
            const auto vpl = std::find_if(vplAdapters.begin(), vplAdapters.end(), [&](const win::VplAdapter& v) {
                return win::packLuid(v.luid) == win::packLuid(adapter.luid);
            });
            if (vpl != vplAdapters.end() && (vpl->encoders.codecs & (1u << codec))) {
                const PictureSize largest = vpl->encoders.largest[codec];
                result.push_back({codec, RCS_CODEC_ENCODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_ONEVPL,
                                  win::packLuid(adapter.luid), largest.width, largest.height,
                                  vpl->encoders.formats[codec], win::vplName(codec, vpl->name),
                                  (vpl->refInvalidation & (1u << codec)) != 0,
                                  std::max(1u, vpl->temporalLayers[codec])});
            }
            char vendor[16];
            std::snprintf(vendor, sizeof(vendor), "VEN_%04X", adapter.vendorId);
            std::vector<std::string> names;
            for (auto& encoder : win::hardwareEncoders(format, &adapter.luid)) {
                std::string name = win::friendlyName(encoder.value);
                const std::string encoderVendor =
                    win::stringAttribute(encoder.value, MFT_ENUM_HARDWARE_VENDOR_ID_Attribute);
                if ((!encoderVendor.empty() && _stricmp(encoderVendor.c_str(), vendor) != 0) ||
                    std::find(names.begin(), names.end(), name) != names.end()) {
                    continue; // Another GPU's, or registered twice (Intel).
                }
                names.push_back(name);
                // Starting one takes 0.1 to 0.4 s: NVIDIA's are tried where
                // they may not start, all others are offered as listed.
                HRESULT failure = S_OK;
                if (adapter.vendorId == win::nvidiaVendor && adapters.front().vendorId != win::nvidiaVendor &&
                    !win::starts(encoder.value, failure)) {
                    log::info("codec", win::startFailure(encoder.value, failure) + ": not offered on " +
                                           win::narrow(adapter.description));
                    continue;
                }
                result.push_back({codec, RCS_CODEC_ENCODER, RCS_ACCELERATION_HARDWARE, backend,
                                  win::packLuid(adapter.luid), limit.width, limit.height,
                                  win::encoderFormats(encoder.value, codec), std::move(name)});
            }
        }
        // Only H.264 has a real-time software encoder (Microsoft's).
        if (codec == RCS_CODEC_H264) {
            for (auto& encoder : win::softwareEncoders(format)) {
                result.push_back({codec, RCS_CODEC_ENCODER, RCS_ACCELERATION_SOFTWARE, backend, 0, limit.width,
                                  limit.height, defaultPictureFormats, win::friendlyName(encoder.value)});
            }
        }
        // The system decoders (HEVC, VP8, VP9 and AV1: Store video extensions) decode on the CPU,
        // and on the GPU where it has a D3D11 decoder profile for the codec.
        auto decoders = win::decoders(format);
        if (!decoders.empty()) {
            const std::string name = win::friendlyName(decoders.front().value);
            if (gpu.codecs & (1u << codec)) {
                const PictureSize largest = gpu.largest[codec];
                result.push_back({codec, RCS_CODEC_DECODER, RCS_ACCELERATION_HARDWARE, backend, gpu.adapterLuid,
                                  largest.width, largest.height, gpu.formats[codec], name});
            }
            const PictureSize software = softwareDecoded(codec);
            result.push_back({codec, RCS_CODEC_DECODER, RCS_ACCELERATION_SOFTWARE, backend, 0, software.width,
                              software.height, softwareFormats(codec), name});
        }
        // NVDEC on each NVIDIA GPU, after Media Foundation's: the 4:4:4
        // pictures those decode on the CPU only.
        for (const win::NvdecAdapter& nvdec : win::nvdecAdapters()) {
            if (nvdec.codecs & (1u << codec)) {
                const PictureSize largest = nvdec.largest[codec];
                result.push_back({codec, RCS_CODEC_DECODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_NVDEC,
                                  win::packLuid(nvdec.luid), largest.width, largest.height, nvdec.formats[codec],
                                  win::nvdecName(codec, nvdec.name)});
            }
        }
        // oneVPL on each Intel GPU, after them: the 4:4:4 those decode on the
        // CPU only (where no NVIDIA GPU decodes it), and 4:2:0 when asked for
        // or where Windows has no decoder of the codec (win::vplDecodesFirst).
        for (const win::VplAdapter& vpl : vplAdapters) {
            if (vpl.decoders.codecs & (1u << codec)) {
                const PictureSize largest = vpl.decoders.largest[codec];
                result.push_back({codec, RCS_CODEC_DECODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_ONEVPL,
                                  win::packLuid(vpl.luid), largest.width, largest.height, vpl.decoders.formats[codec],
                                  win::vplName(codec, vpl.name)});
            }
        }
    }
    return result;
}

// Ids as win/display.hpp defines them. The current mode's size is the
// desktop's, a display turned to portrait's too; its orientation (DMDO_90:
// turned 90 degrees clockwise) says how far it is turned.
std::vector<rcs_display_info> displays() {
    std::vector<rcs_display_info> result;
    for (const auto& monitor : win::monitors()) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(monitor.device.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
            const uint32_t refresh = mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0; // 0, 1: default.
            const uint32_t turns = (mode.dmFields & DM_DISPLAYORIENTATION) ? mode.dmDisplayOrientation % 4 : 0;
            const RECT& r = monitor.rect; // The primary display's top left is the virtual screen's origin.
            result.push_back({monitor.id, mode.dmPelsWidth, mode.dmPelsHeight, refresh, turns * 90,
                              monitor.primary ? 1u : 0u, r.left, r.top, uint32_t(r.right - r.left),
                              uint32_t(r.bottom - r.top)});
        }
    }
    return result;
}

rcs_permission screenPermission() noexcept {
    DWORD session = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &session) && session ? RCS_PERMISSION_GRANTED
                                                                            : RCS_PERMISSION_REQUIRED;
}

rcs_result requestScreenPermission() {
    return screenPermission() == RCS_PERMISSION_GRANTED ? RCS_OK : RCS_ERROR_PERMISSION_REQUIRED;
}

// A process in a user's session may inject input there; one in session 0 (a
// service) has no desktop to inject into, and nothing grants it one.
rcs_permission inputPermission() noexcept {
    return win::sessionId() != 0 ? RCS_PERMISSION_GRANTED : RCS_PERMISSION_UNAVAILABLE;
}

bool inputAvailable(bool secureDesktop) noexcept {
    return inputPermission() == RCS_PERMISSION_GRANTED && inputDesktopReachable(secureDesktop);
}

rcs_result requestInputPermission() {
    return inputPermission() == RCS_PERMISSION_GRANTED ? RCS_OK : RCS_ERROR_UNSUPPORTED;
}

void withCpuImage(const VideoFrame& frame, const std::function<void(const rcs_frame&)>& use) {
    auto* texture = static_cast<ID3D11Texture2D*>(frame.surface.get());
    if (!texture) {
        fail(RCS_ERROR_UNSUPPORTED, "Frame has no D3D11 texture");
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (frame.width > desc.Width || frame.height > desc.Height) {
        fail(RCS_ERROR_INTERNAL, "The frame is larger than its texture");
    }
    win::Com<ID3D11Device> device;
    texture->GetDevice(device.out());
    win::Com<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.out());

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.ArraySize = 1;
    desc.MipLevels = 1;
    win::Com<ID3D11Texture2D> staging;
    win::check(device->CreateTexture2D(&desc, nullptr, staging.out()), "CPU readback texture");
    D3D11_MAPPED_SUBRESOURCE map{};
    {
        win::ContextLock lock(context.value);
        context->CopySubresourceRegion(staging.value, 0, 0, 0, 0, texture, 0, nullptr);
        win::check(context->Map(staging.value, 0, D3D11_MAP_READ, 0, &map), "CPU readback");
    }
    struct Unmap {
        ID3D11DeviceContext* context;
        ID3D11Texture2D* texture;
        ~Unmap() { context->Unmap(texture, 0); }
    } unmap{context.value, staging.value};

    rcs_frame view{};
    view.struct_size = sizeof(view);
    view.width = frame.width;
    view.height = frame.height;
    view.format = RCS_PIXEL_BGRA8;
    view.stream = frame.stream;
    view.sequence = frame.sequence;
    view.timestamp_ns = frame.timestampNs;
    const auto* bytes = static_cast<const uint8_t*>(map.pData);
    if (desc.Format == DXGI_FORMAT_NV12 || desc.Format == DXGI_FORMAT_P010) {
        const YuvLayout layout{false, desc.Format == DXGI_FORMAT_P010 ? 10u : 8u};
        std::vector<uint8_t> converted(size_t(frame.width) * frame.height * 4);
        yuvToBgra(bytes, map.RowPitch, bytes + size_t(map.RowPitch) * desc.Height, map.RowPitch, frame.width,
                  frame.height, layout, yuvCoefficients(frame.color.matrix, frame.color.range, layout.bits),
                  converted.data(), frame.width * 4);
        view.data = converted.data();
        view.stride_bytes = frame.width * 4;
        view.data_size = converted.size();
        use(view);
    } else if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        view.data = bytes;
        view.stride_bytes = map.RowPitch;
        view.data_size = uint64_t(map.RowPitch) * frame.height;
        use(view);
    } else {
        fail(RCS_ERROR_UNSUPPORTED, "Unsupported texture format for CPU output");
    }
}

std::shared_ptr<void> retainHostDevice(void* device) {
    return device ? win::share(static_cast<ID3D11Device*>(device)) : nullptr;
}

void checkHostDevice(void* device, uint32_t codecs) {
    win::Device d3d;
    d3d.adopt(static_cast<ID3D11Device*>(device));
    if (codecs && !(win::gpuDecoders(d3d.device.value) & codecs)) {
        std::string listed;
        for (const rcs_codec codec : win::codecs) {
            if (codecs & (1u << codec)) {
                listed += (listed.empty() ? "" : ", ") + std::string(codecName(codec));
            }
        }
        fail(RCS_ERROR_UNSUPPORTED, "The GPU of d3d11_device decodes none of " + listed + " in hardware");
    }
}

bool describeGpuFrame(const VideoFrame& frame, rcs_gpu_frame_info& info) {
    auto* texture = static_cast<ID3D11Texture2D*>(frame.surface.get());
    if (!texture) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    info.resource_type = RCS_GPU_D3D11_TEXTURE2D;
    switch (desc.Format) {
    case DXGI_FORMAT_NV12:
        info.format = RCS_PIXEL_NV12;
        break;
    case DXGI_FORMAT_P010:
        info.format = RCS_PIXEL_P010;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        info.format = RCS_PIXEL_RGB10A2;
        break;
    default:
        info.format = RCS_PIXEL_BGRA8;
        break;
    }
    info.width = frame.width;
    info.height = frame.height;
    info.surface_width = desc.Width;
    info.surface_height = desc.Height;
    info.color = rgbLayout(info.format) ? rgbColor(frame.color) : frame.color;
    info.stream = frame.stream;
    info.sequence = frame.sequence;
    info.timestamp_ns = frame.timestampNs;
    info.resource = texture;
    info.device = frame.device.get();
    return true;
}

// The host's D3D11 textures as pictures: not yet (rcs_capabilities.pushed_gpu_frames is 0).
VideoFrame hostGpuFrame(const rcs_gpu_frame_info&) {
    fail(RCS_ERROR_UNSUPPORTED, "Windows takes no GPU pictures from the host yet (rcs_capabilities.pushed_gpu_frames); "
                                "push BGRA8 with rcs_session_push_video_frame");
}

HostImages hostImages() noexcept {
    return HostImages::none;
}

std::unique_ptr<SamplePictures> createSamplePictures() {
    fail(RCS_ERROR_UNSUPPORTED, "Windows has no Core Media sample buffers: push pictures with "
                                "rcs_session_push_video_frame");
}

std::unique_ptr<SampleSound> createSampleSound() {
    fail(RCS_ERROR_UNSUPPORTED, "Windows has no Core Media sample buffers: push sound with rcs_session_push_audio");
}

} // namespace rcsdk::platform
