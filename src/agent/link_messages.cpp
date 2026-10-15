#include "agent/link_messages.hpp"

#include "protocol/messages.hpp"
#include "modules/registry.hpp"
#include "protocol/wire.hpp"

#include <rcsdk/gpu.h>

#include <algorithm>
#include <cstring>

namespace rcsdk::agent::link {
namespace {

using crypto::Bytes;
using protocol::Reader;
using protocol::Writer;

// The codecs some version defines, as (1u << codec) bits.
// The first seven bytes never change; the eighth is the link's format.
constexpr uint8_t linkMagic[8] = {'R', 'C', 'S', 'L', 'I', 'N', 'K', 2};

enum Type : uint8_t {
    agentHelloType = 0,
    acceptedType,
    refusedType,
    replacedType,
    heartbeatType,
    startVideoType,
    stopVideoType,
    setRateType,
    setCursorInVideoType,
    setInputBoostType,
    requestKeyframeType,
    invalidateAfterType,
    backlogType,
    startAudioType,
    stopAudioType,
    configureInputType,
    applyInputType,
    releaseInputType,
    moduleStartType,
    moduleStopType,
    listWindowsType,
    startedType,
    packetType,
    contentType,
    modeType,
    captureType,
    pointerType,
    displayListType,
    availabilityType,
    desktopType,
    inputResultsType,
    pcmType,
    audioStateType,
    windowListType,
    moduleDataType,
    moduleStatusType,
    failedType,
    encodingFailedType
};

constexpr size_t maxImplementations = 128, maxDisplays = 32, maxText = 1024, maxName = 63, maxWindows = 4096;
constexpr size_t maxPcmSamples = 96'000; // One second of stereo.

bool flag(uint8_t value) {
    if (value > 1) {
        Reader::invalid("a flag is 0 or 1");
    }
    return value != 0;
}

// A result this version defines: RCS_OK, or an error down to the last one.
rcs_result readResult(Reader& r) {
    const auto result = rcs_result(int32_t(r.u32()));
    if (result > RCS_OK || result < RCS_ERROR_REFUSED) {
        Reader::invalid("a result no version defines");
    }
    return result;
}

rcs_result readError(Reader& r) {
    const rcs_result result = readResult(r);
    if (result == RCS_OK) {
        Reader::invalid("a failure without an error");
    }
    return result;
}

void putText(Writer& w, const std::string& text, size_t limit) {
    const size_t size = std::min(text.size(), limit);
    w.u32(uint32_t(size)).raw(std::span(reinterpret_cast<const uint8_t*>(text.data()), size));
}

std::string readText(Reader& r, size_t limit) {
    const uint32_t size = r.u32();
    if (size > limit) {
        Reader::invalid("text too long");
    }
    const auto bytes = r.raw(size);
    return std::string(bytes.begin(), bytes.end());
}

std::string readModuleId(Reader& r) {
    std::string id = readText(r, RCS_MODULE_ID_MAX - 1);
    if (!modules::validId(id)) {
        Reader::invalid("module id");
    }
    return id;
}

// ---- Parts -----------------------------------------------------------------------------------

void putCapabilities(Writer& w, const rcs_capabilities& c) {
    w.u32(c.platform)
        .u32(c.capture_sources)
        .u32(c.picture_formats)
        .u32(c.hardware_encoders)
        .u32(c.software_encoders)
        .u32(c.hardware_decoders)
        .u32(c.software_decoders)
        .u32(c.software_decoding_gpu_frames)
        .u32(c.pushed_gpu_frames)
        .u32(c.audio_capture_backends)
        .u32(c.audio_exclusion)
        .u32(c.audio_encoders)
        .u32(c.audio_decoders)
        .u32(c.input_types)
        .u32(c.secure_desktop)
        .u32(c.secure_attention);
}

rcs_capabilities readCapabilities(Reader& r) {
    rcs_capabilities c = RCS_CAPABILITIES_INIT;
    c.platform = r.u32();
    c.capture_sources = r.u32();
    c.picture_formats = r.u32();
    c.hardware_encoders = r.u32();
    c.software_encoders = r.u32();
    c.hardware_decoders = r.u32();
    c.software_decoders = r.u32();
    c.software_decoding_gpu_frames = r.u32();
    c.pushed_gpu_frames = r.u32();
    c.audio_capture_backends = r.u32();
    c.audio_exclusion = r.u32();
    c.audio_encoders = r.u32();
    c.audio_decoders = r.u32();
    c.input_types = r.u32();
    c.secure_desktop = r.u32();
    c.secure_attention = r.u32();
    const uint32_t audioMask = (1u << (RCS_AUDIO_PCM + 1)) - 2, backendMask = 0xF;
    const uint32_t gpuResourceMask =
        (1u << RCS_GPU_D3D11_TEXTURE2D) | (1u << RCS_GPU_CVPIXELBUFFER) | (1u << RCS_GPU_AHARDWAREBUFFER);
    const uint32_t sourceMask = (1u << (RCS_CAPTURE_ENCODED + 1)) - 1;
    if (c.platform > RCS_PLATFORM_WEB || (c.capture_sources & ~sourceMask) ||
        (c.picture_formats & ~allPictureFormats) || c.audio_exclusion > 1 ||
        (c.input_types & ~protocol::allInputTypes) || c.secure_desktop > 1 || c.secure_attention > 1 ||
        c.software_decoding_gpu_frames > 1 || (c.pushed_gpu_frames & ~gpuResourceMask) ||
        ((c.hardware_encoders | c.software_encoders | c.hardware_decoders | c.software_decoders) & ~standardCodecBits) ||
        ((c.audio_encoders | c.audio_decoders) & ~audioMask) ||
        c.audio_capture_backends > backendMask) {
        Reader::invalid("capabilities");
    }
    return c;
}

void putDisplays(Writer& w, const std::vector<rcs_display_info>& list) {
    w.u8(uint8_t(std::min(list.size(), maxDisplays)));
    for (size_t i = 0; i < std::min(list.size(), maxDisplays); ++i) {
        const auto& d = list[i];
        w.u32(d.display_id).u32(d.pixel_width).u32(d.pixel_height).u32(d.refresh_hz).u32(d.rotation_degrees);
        w.u32(d.is_main).u32(uint32_t(d.desktop_x)).u32(uint32_t(d.desktop_y)).u32(d.desktop_width);
        w.u32(d.desktop_height);
    }
}

std::vector<rcs_display_info> readDisplays(Reader& r) {
    const uint8_t count = r.u8();
    if (count > maxDisplays) {
        Reader::invalid("too many displays");
    }
    std::vector<rcs_display_info> list(count);
    for (auto& d : list) {
        d.display_id = r.u32();
        d.pixel_width = r.u32();
        d.pixel_height = r.u32();
        d.refresh_hz = r.u32();
        d.rotation_degrees = r.u32();
        d.is_main = r.u32();
        d.desktop_x = int32_t(r.u32());
        d.desktop_y = int32_t(r.u32());
        d.desktop_width = r.u32();
        d.desktop_height = r.u32();
        if (!d.display_id || d.pixel_width > 65535 || d.pixel_height > 65535 || d.refresh_hz > 1000 ||
            d.rotation_degrees % 90 || d.rotation_degrees > 270 || d.is_main > 1 || d.desktop_width > 65535 ||
            d.desktop_height > 65535) {
            Reader::invalid("display");
        }
    }
    return list;
}

void putMachine(Writer& w, const Machine& m) {
    putCapabilities(w, m.capabilities);
    w.u8(uint8_t(std::min(m.implementations.size(), maxImplementations)));
    for (size_t i = 0; i < std::min(m.implementations.size(), maxImplementations); ++i) {
        const auto& impl = m.implementations[i];
        w.u32(impl.codec).u32(impl.direction).u32(impl.acceleration).u32(impl.backend).u64(impl.adapterLuid);
        w.u32(impl.maxWidth).u32(impl.maxHeight).u32(impl.pictureFormats).u8(impl.refInvalidation);
        w.u8(uint8_t(impl.temporalLayers));
        for (const uint64_t rate : impl.maxPixelRates) {
            w.u64(rate);
        }
        putText(w, impl.name, maxName);
    }
    w.u32(m.hostEncoders).u32(m.hostEncoderFormats);
    putDisplays(w, m.displays);
    w.u32(m.screenPermission).u32(m.inputPermission).u32(m.defaultAudioBackend).u32(m.audioPeriodBackends);
}

Machine readMachine(Reader& r) {
    Machine m;
    m.capabilities = readCapabilities(r);
    const uint8_t count = r.u8();
    if (count > maxImplementations) {
        Reader::invalid("too many codec implementations");
    }
    m.implementations.resize(count);
    for (auto& impl : m.implementations) {
        impl.codec = r.u32();
        impl.direction = r.u32();
        impl.acceleration = r.u32();
        impl.backend = r.u32();
        impl.adapterLuid = r.u64();
        impl.maxWidth = r.u32();
        impl.maxHeight = r.u32();
        impl.pictureFormats = r.u32();
        const uint8_t invalidation = r.u8();
        impl.refInvalidation = invalidation != 0;
        impl.temporalLayers = r.u8();
        for (auto& rate : impl.maxPixelRates) {
            rate = r.u64();
        }
        impl.name = readText(r, maxName);
        if (!standardCodec(impl.codec) || impl.direction < RCS_CODEC_ENCODER ||
            impl.direction > RCS_CODEC_DECODER || impl.acceleration > RCS_ACCELERATION_SOFTWARE ||
            impl.backend == RCS_BACKEND_AUTO || !choosableBackend(impl.backend) || impl.maxWidth < 16 ||
            impl.maxHeight < 16 ||
            impl.maxWidth > codecLimit(impl.codec).width || impl.maxHeight > codecLimit(impl.codec).height ||
            !impl.pictureFormats || (impl.pictureFormats & ~allPictureFormats) || invalidation > 1 ||
            (invalidation && impl.direction != RCS_CODEC_ENCODER) || !impl.temporalLayers ||
            impl.temporalLayers > RCS_SCALABILITY_L1T3 ||
            (impl.temporalLayers > 1 && impl.direction != RCS_CODEC_ENCODER)) {
            Reader::invalid("codec implementation");
        }
    }
    m.hostEncoders = r.u32();
    m.hostEncoderFormats = r.u32();
    if ((m.hostEncoders & ~knownCodecBits) || (m.hostEncoderFormats & ~allPictureFormats)) {
        Reader::invalid("host encoder codecs");
    }
    m.displays = readDisplays(r);
    m.screenPermission = r.u32();
    m.inputPermission = r.u32();
    m.defaultAudioBackend = r.u32();
    m.audioPeriodBackends = r.u32();
    if (m.screenPermission > RCS_PERMISSION_GRANTED || m.inputPermission > RCS_PERMISSION_GRANTED ||
        m.defaultAudioBackend > RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK || m.audioPeriodBackends > 0xF) {
        Reader::invalid("machine");
    }
    return m;
}

void putTarget(Writer& w, const platform::CaptureTarget& t) {
    w.u32(t.source).u32(t.displayId).u32(t.region.x).u32(t.region.y).u32(t.region.width).u32(t.region.height);
    w.u64(t.windowId).u32(t.processId);
}

// What the machine captures: never the host's own pictures, which stay in the
// session's process.
platform::CaptureTarget readTarget(Reader& r) {
    platform::CaptureTarget t;
    t.source = r.u32();
    t.displayId = r.u32();
    t.region.x = r.u32();
    t.region.y = r.u32();
    t.region.width = r.u32();
    t.region.height = r.u32();
    t.windowId = r.u64();
    t.processId = r.u32();
    const auto& g = t.region;
    const bool region =
        g.width >= 16 && g.height >= 16 && g.x <= 65535 && g.y <= 65535 && g.width <= 65535 && g.height <= 65535;
    if (t.source > RCS_CAPTURE_APPLICATION || (t.source == RCS_CAPTURE_REGION && !region) ||
        (t.source == RCS_CAPTURE_WINDOW && !t.windowId) || (t.source == RCS_CAPTURE_APPLICATION && !t.processId)) {
        Reader::invalid("capture target");
    }
    return t;
}

void putVideoStream(Writer& w, const VideoStream& s) {
    w.u32(s.codec).u8(uint8_t(s.picture.format)).u8(uint8_t(s.picture.matrix)).u8(uint8_t(s.picture.range));
    w.u32(s.width).u32(s.height).u32(s.framesPerSecond);
    putTarget(w, s.target);
    w.u8(s.excludeCurrentProcess).u32(s.acceleration).u32(s.backend).u32(s.speed).u32(s.content);
    w.u32(s.softwareThreads).u8(s.secureDesktop).u8(uint8_t(s.temporalLayers));
}

VideoStream readVideoStream(Reader& r) {
    VideoStream s;
    s.codec = r.u32();
    s.picture.format = r.u8();
    s.picture.matrix = r.u8();
    s.picture.range = r.u8();
    s.width = r.u32();
    s.height = r.u32();
    s.framesPerSecond = r.u32();
    s.target = readTarget(r);
    s.excludeCurrentProcess = flag(r.u8());
    s.acceleration = r.u32();
    s.backend = r.u32();
    s.speed = r.u32();
    s.content = r.u32();
    s.softwareThreads = r.u32();
    s.secureDesktop = flag(r.u8());
    s.temporalLayers = r.u8();
    if (!knownCodec(s.codec) || !validPicture(s.picture) || !protocol::validStreamSize(s.codec, s.width, s.height) ||
        !s.framesPerSecond || s.framesPerSecond > 240 || s.acceleration > RCS_ACCELERATION_HARDWARE_FIRST ||
        !choosableBackend(s.backend) || s.speed < RCS_ENCODER_FASTEST || s.speed > RCS_ENCODER_BEST_QUALITY ||
        s.content > RCS_CONTENT_MOTION || s.softwareThreads > platform::maxSoftwareThreads || !s.temporalLayers ||
        s.temporalLayers > RCS_SCALABILITY_L1T3) {
        Reader::invalid("video stream");
    }
    return s;
}

void putRect(Writer& w, const Rect& rect) {
    w.u32(rect.x).u32(rect.y).u32(rect.width).u32(rect.height);
}

Rect readRect(Reader& r) {
    Rect rect;
    rect.x = r.u32();
    rect.y = r.u32();
    rect.width = r.u32();
    rect.height = r.u32();
    if (!rect.width || !rect.height || rect.x > 65535 || rect.y > 65535 || rect.width > 65535 || rect.height > 65535) {
        Reader::invalid("content rectangle");
    }
    return rect;
}

void putDisplayMode(Writer& w, const platform::DisplayMode& mode) {
    w.u32(mode.width).u32(mode.height).u32(mode.refreshHz);
}

platform::DisplayMode readDisplayMode(Reader& r) {
    platform::DisplayMode mode;
    mode.width = r.u32();
    mode.height = r.u32();
    mode.refreshHz = r.u32();
    if (mode.width > 65535 || mode.height > 65535 || mode.refreshHz > 1000) {
        Reader::invalid("display mode");
    }
    return mode;
}

void putCursor(Writer& w, const CursorState& s) {
    w.u8(s.visible).u32(s.x).u32(s.y).u64(s.imageSerial).u8(s.image != nullptr);
    if (s.image) {
        const auto packed = packPixels(s.image->pixels);
        w.u32(s.image->type).u32(s.image->width).u32(s.image->height).u32(s.image->hotspotX).u32(s.image->hotspotY);
        w.u32(uint32_t(packed.size())).raw(packed);
    }
}

CursorState readCursor(Reader& r) {
    CursorState s;
    s.visible = flag(r.u8());
    s.x = r.u32();
    s.y = r.u32();
    s.imageSerial = r.u64();
    if (s.x > 65535 || s.y > 65535) {
        Reader::invalid("pointer position");
    }
    if (flag(r.u8())) {
        auto image = std::make_shared<CursorImage>();
        image->type = r.u32();
        image->width = r.u32();
        image->height = r.u32();
        image->hotspotX = r.u32();
        image->hotspotY = r.u32();
        const uint32_t packedSize = r.u32();
        if (image->type > RCS_CURSOR_HELP || !image->width || !image->height || image->width > maxCursorSize ||
            image->height > maxCursorSize || image->hotspotX >= image->width || image->hotspotY >= image->height ||
            packedSize > uint64_t(image->width) * image->height * 4 * 2) {
            Reader::invalid("pointer image");
        }
        image->pixels = unpackPixels(r.raw(packedSize), size_t(image->width) * image->height);
        s.image = std::move(image);
    }
    return s;
}

void putText(Writer& w, const char* text, size_t limit) {
    putText(w, std::string(text, strnlen(text, limit)), limit);
}

// Text into a fixed field that holds it and its terminator.
void readText(Reader& r, char* to, size_t size) {
    const std::string text = readText(r, size - 1);
    std::memcpy(to, text.data(), text.size());
    to[text.size()] = 0;
}

Bytes encodeWindowList(const WindowList& m) {
    Writer w(windowListType);
    const size_t count = std::min(m.windows.size(), maxWindows);
    w.u32(m.serial).u32(uint32_t(count));
    for (size_t i = 0; i < count; ++i) {
        const auto& v = m.windows[i];
        w.u64(v.window_id).u32(v.process_id).u32(v.display_id).u32(uint32_t(v.x)).u32(uint32_t(v.y));
        w.u32(v.width).u32(v.height).u8(v.minimized != 0).u8(v.focused != 0);
        putText(w, v.title, sizeof(v.title) - 1);
        putText(w, v.application, sizeof(v.application) - 1);
    }
    return w.take();
}

WindowList decodeWindowList(Reader& r) {
    WindowList m;
    m.serial = r.u32();
    const uint32_t count = r.u32();
    if (count > maxWindows) {
        Reader::invalid("too many windows");
    }
    m.windows.resize(count);
    for (auto& v : m.windows) {
        v.window_id = r.u64();
        v.process_id = r.u32();
        v.display_id = r.u32();
        v.x = int32_t(r.u32());
        v.y = int32_t(r.u32());
        v.width = r.u32();
        v.height = r.u32();
        v.minimized = flag(r.u8());
        v.focused = flag(r.u8());
        readText(r, v.title, sizeof(v.title));
        readText(r, v.application, sizeof(v.application));
        if (!v.window_id || v.width > 65535 || v.height > 65535) {
            Reader::invalid("window");
        }
    }
    r.end();
    return m;
}

// ---- Messages --------------------------------------------------------------------------------

// A stream's index as the link carries it...
uint8_t readIndex(Reader& r) {
    const uint8_t index = r.u8();
    if (index >= RCS_MAX_STREAMS) {
        Reader::invalid("stream");
    }
    return index;
}

// ...and one of its encodings'.
uint8_t readEncoding(Reader& r) {
    const uint8_t encoding = r.u8();
    if (encoding >= RCS_MAX_ENCODINGS) {
        Reader::invalid("encoding");
    }
    return encoding;
}

Bytes encodeVideoStart(const StartVideo& m) {
    Writer w(startVideoType);
    w.u8(uint8_t(m.start.index)).u8(uint8_t(m.start.encoding));
    putVideoStream(w, m.start.stream);
    return w.u32(m.start.bitrate).u8(m.start.budgeted).u8(m.start.cursorInVideo).u32(m.start.inputBoostMs).take();
}

StartVideo decodeVideoStart(Reader& r) {
    StartVideo m;
    m.start.index = readIndex(r);
    m.start.encoding = readEncoding(r);
    m.start.stream = readVideoStream(r);
    m.start.bitrate = r.u32();
    m.start.budgeted = flag(r.u8());
    m.start.cursorInVideo = flag(r.u8());
    m.start.inputBoostMs = r.u32();
    r.end();
    if (m.start.bitrate < 100'000 || m.start.bitrate > 500'000'000 || m.start.inputBoostMs > 1000) {
        Reader::invalid("video start");
    }
    return m;
}

Bytes encodeStarted(const Started& m) {
    Writer w(startedType);
    w.u8(uint8_t(m.started.index)).u8(uint8_t(m.started.encoding));
    putVideoStream(w, m.started.stream);
    putRect(w, m.started.content);
    putDisplayMode(w, m.started.display);
    w.u32(m.started.acceleration).u32(m.started.backend).u8(m.started.lowLatency).u8(m.started.refInvalidation);
    w.u8(uint8_t(m.started.temporalLayers));
    putText(w, m.started.encoder, maxName);
    const auto& h = m.started.hdr;
    for (const uint16_t value : {h.red_x, h.red_y, h.green_x, h.green_y, h.blue_x, h.blue_y, h.white_x, h.white_y}) {
        w.u16(value);
    }
    w.u32(h.max_luminance).u32(h.min_luminance).u16(h.max_content_light).u16(h.max_frame_average_light);
    return w.take();
}

Started decodeStarted(Reader& r) {
    Started m;
    m.started.index = readIndex(r);
    m.started.encoding = readEncoding(r);
    m.started.stream = readVideoStream(r);
    m.started.content = readRect(r);
    m.started.display = readDisplayMode(r);
    m.started.acceleration = r.u32();
    m.started.backend = r.u32();
    m.started.lowLatency = flag(r.u8());
    m.started.refInvalidation = flag(r.u8());
    m.started.temporalLayers = r.u8();
    m.started.encoder = readText(r, maxName);
    auto& h = m.started.hdr;
    for (uint16_t* value : {&h.red_x, &h.red_y, &h.green_x, &h.green_y, &h.blue_x, &h.blue_y, &h.white_x, &h.white_y}) {
        *value = r.u16();
        if (*value > 50'000) { // CIE x and y are at most 1.
            Reader::invalid("HDR metadata");
        }
    }
    h.max_luminance = r.u32();
    h.min_luminance = r.u32();
    h.max_content_light = r.u16();
    h.max_frame_average_light = r.u16();
    r.end();
    if (!fits(m.started.content, m.started.stream.width, m.started.stream.height)) {
        Reader::invalid("content outside the picture");
    }
    if (m.started.acceleration > RCS_ACCELERATION_SOFTWARE || m.started.backend > RCS_BACKEND_HOST ||
        !m.started.temporalLayers || m.started.temporalLayers > m.started.stream.temporalLayers) {
        Reader::invalid("encoder");
    }
    return m;
}

Bytes encodePacket(const Packet& m) {
    Writer w(packetType);
    w.u64(m.packet.encodeNs).raw(protocol::encode(m.packet));
    return w.take();
}

Packet decodePacket(Reader& r) {
    Packet m;
    m.packet.encodeNs = r.u64();
    const auto network = protocol::decode(r.raw(r.remaining())); // The picture's own checks.
    auto* packet = std::get_if<EncodedPacket>(&network);
    if (!packet) {
        Reader::invalid("video packet");
    }
    const uint64_t encodeNs = m.packet.encodeNs;
    m.packet = std::move(*packet);
    m.packet.encodeNs = encodeNs;
    return m;
}

Bytes encodePcm(const Pcm& m) {
    Writer w(pcmType);
    w.u64(m.timestampNs).u32(m.periodFrames).u32(uint32_t(m.samples.size()));
    return w.raw(std::span(reinterpret_cast<const uint8_t*>(m.samples.data()), m.samples.size() * sizeof(float))).take();
}

Pcm decodePcm(Reader& r) {
    Pcm m;
    m.timestampNs = r.u64();
    m.periodFrames = r.u32();
    const uint32_t count = r.u32();
    if (!count || count > maxPcmSamples || count % 2 != 0 || m.periodFrames > 48'000) {
        Reader::invalid("sound");
    }
    const auto bytes = r.raw(size_t(count) * sizeof(float));
    r.end();
    m.samples.resize(count);
    std::memcpy(m.samples.data(), bytes.data(), bytes.size());
    return m;
}

} // namespace

Bytes prefix(uint32_t version) {
    Bytes bytes(std::begin(linkMagic), std::end(linkMagic));
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(uint8_t(version >> shift));
    }
    return bytes;
}

std::string versionText(uint32_t version) {
    return std::to_string(version >> 16) + "." + std::to_string((version >> 8) & 0xffu) + "." +
           std::to_string(version & 0xffu);
}

void checkPrefix(std::span<const uint8_t> bytes, const char* peer) {
    if (bytes.size() != prefixSize || !std::equal(linkMagic, linkMagic + 7, bytes.begin())) {
        fail(RCS_ERROR_NETWORK, std::string("The ") + peer + " does not speak the agent link");
    }
    uint32_t version = 0;
    for (size_t i = 8; i < prefixSize; ++i) {
        version = (version << 8) | bytes[i];
    }
    if (bytes[7] != linkMagic[7] || version != RCSDK_VERSION) {
        const std::string mine = peer == std::string("agent") ? "the session" : "the agent";
        fail(RCS_ERROR_UNSUPPORTED, std::string("The ") + peer + " is version " + versionText(version) +
                                        " (link format " + std::to_string(bytes[7]) + "), " + mine + " " +
                                        RCSDK_VERSION_STRING + " (link format " + std::to_string(linkMagic[7]) +
                                        "); run the same version on both");
    }
}

transport::Credentials linkCredentials(const crypto::Key& key) {
    static constexpr char label[] = "rcsdk agent link";
    const auto bound = crypto::hkdf(key, {}, std::span(reinterpret_cast<const uint8_t*>(label), sizeof(label) - 1),
                                    crypto::Key{}.size());
    crypto::Key linkKey{};
    std::copy(bound.begin(), bound.end(), linkKey.begin());
    return transport::Credentials::fromKey(linkKey);
}

std::optional<crypto::Bytes> ModuleAssembly::add(ModuleData&& part, size_t limit) {
    auto& partial = partial_[part.id];
    if (part.index == 0) {
        if (partial.count) { // A module's messages come whole, one after another.
            partial_.erase(part.id);
            fail(RCS_ERROR_PROTOCOL, "A module's message began before the one before it was complete");
        }
        partial = {part.count, 0, {}};
    }
    if (part.index != partial.next || part.count != partial.count ||
        partial.bytes.size() + part.bytes.size() > limit) {
        partial_.erase(part.id);
        fail(RCS_ERROR_PROTOCOL, "A module's message in parts out of order, or larger than the module takes");
    }
    partial.bytes.insert(partial.bytes.end(), part.bytes.begin(), part.bytes.end());
    if (++partial.next < partial.count) {
        return std::nullopt;
    }
    crypto::Bytes whole = std::move(partial.bytes);
    partial_.erase(part.id);
    return whole;
}

bool fits(const Rect& content, uint32_t width, uint32_t height) noexcept {
    return uint64_t(content.x) + content.width <= width && uint64_t(content.y) + content.height <= height;
}

Bytes encode(const Message& message) {
    struct Encoder {
        Bytes operator()(const AgentHello& m) const {
            Writer w(agentHelloType);
            putMachine(w, m.machine);
            return w.take();
        }
        Bytes operator()(const Accepted&) const { return Writer(acceptedType).take(); }
        Bytes operator()(const Refused& m) const {
            Writer w(refusedType);
            w.u32(uint32_t(m.result));
            putText(w, m.reason, maxText);
            return w.take();
        }
        Bytes operator()(const Replaced&) const { return Writer(replacedType).take(); }
        Bytes operator()(const Heartbeat&) const { return Writer(heartbeatType).take(); }
        Bytes operator()(const StartVideo& m) const { return encodeVideoStart(m); }
        Bytes operator()(const StopVideo& m) const { return Writer(stopVideoType).u8(m.index).u8(m.encoding).take(); }
        Bytes operator()(const SetRate& m) const {
            return Writer(setRateType).u8(m.index).u8(m.encoding).u32(m.bitrate).u8(m.budgeted).take();
        }
        Bytes operator()(const SetCursorInVideo& m) const { return Writer(setCursorInVideoType).u8(m.inVideo).take(); }
        Bytes operator()(const SetInputBoost& m) const { return Writer(setInputBoostType).u32(m.ms).take(); }
        Bytes operator()(const RequestKeyframe& m) const {
            return Writer(requestKeyframeType).u8(m.index).u8(m.encoding).take();
        }
        Bytes operator()(const InvalidateAfter& m) const {
            return Writer(invalidateAfterType).u8(m.index).u8(m.encoding).u64(m.sequence).take();
        }
        Bytes operator()(const Backlog& m) const {
            Writer w(backlogType);
            for (const uint64_t ns : m.ns) {
                w.u64(ns);
            }
            return w.take();
        }
        Bytes operator()(const StartAudio& m) const {
            return Writer(startAudioType)
                .u32(m.start.backend)
                .u8(m.start.excludeCurrentProcess)
                .u32(m.start.periodFrames)
                .u8(m.start.microphone)
                .u32(m.start.device)
                .u8(m.start.voiceProcessing)
                .take();
        }
        Bytes operator()(const StopAudio&) const { return Writer(stopAudioType).take(); }
        Bytes operator()(const ConfigureInput& m) const {
            Writer w(configureInputType);
            putTarget(w, m.target);
            w.u8(uint8_t(m.streamDisplays.size()));
            for (const uint32_t display : m.streamDisplays) {
                w.u32(display);
            }
            return w.u8(m.options.secureDesktop).u32(m.options.capsLock).u32(m.options.capsLockHoldMs).take();
        }
        Bytes operator()(const ApplyInput& m) const {
            Writer w(applyInputType);
            protocol::putInputEvent(w, m.event);
            return w.take();
        }
        Bytes operator()(const ReleaseInput&) const { return Writer(releaseInputType).take(); }
        Bytes operator()(const ModuleStart& m) const {
            Writer w(moduleStartType);
            putText(w, m.id, RCS_MODULE_ID_MAX - 1);
            return w.u32(m.version).take();
        }
        Bytes operator()(const ModuleStop& m) const {
            Writer w(moduleStopType);
            putText(w, m.id, RCS_MODULE_ID_MAX - 1);
            return w.take();
        }
        Bytes operator()(const ListWindows& m) const { return Writer(listWindowsType).u32(m.serial).take(); }
        Bytes operator()(const Started& m) const { return encodeStarted(m); }
        Bytes operator()(const Packet& m) const { return encodePacket(m); }
        Bytes operator()(const Content& m) const {
            Writer w(contentType);
            w.u8(m.index);
            putRect(w, m.content);
            putDisplayMode(w, m.display);
            return w.take();
        }
        Bytes operator()(const Mode& m) const {
            Writer w(modeType);
            w.u8(m.index).u8(m.encoding).u32(m.codec).u8(m.lowLatency).u8(m.refInvalidation).u8(m.temporalLayers);
            putText(w, m.name, maxName);
            return w.take();
        }
        Bytes operator()(const Capture& m) const {
            Writer w(captureType);
            w.u8(m.index)
                .u32(m.status.paused)
                .u64(m.status.captured)
                .u64(m.status.overwritten)
                .u64(m.status.interruptions);
            for (const uint64_t limited : m.status.limitedMs) {
                w.u64(limited);
            }
            return w.take();
        }
        Bytes operator()(const EncodingFailed& m) const {
            Writer w(encodingFailedType);
            w.u8(m.index).u8(m.encoding).u32(uint32_t(m.result));
            putText(w, m.message, maxText);
            return w.take();
        }
        Bytes operator()(const Pointer& m) const {
            Writer w(pointerType);
            w.u8(m.index);
            putCursor(w, m.state);
            return w.take();
        }
        Bytes operator()(const DisplayList& m) const {
            Writer w(displayListType);
            putDisplays(w, m.list);
            return w.take();
        }
        Bytes operator()(const Availability& m) const { return Writer(availabilityType).u8(m.available).take(); }
        Bytes operator()(const Desktop& m) const {
            return Writer(desktopType).u32(m.state.kind).u8(m.state.userLoggedOn).take();
        }
        Bytes operator()(const InputResults& m) const {
            return Writer(inputResultsType).u32(uint32_t(m.last)).u64(m.applied).take();
        }
        Bytes operator()(const Pcm& m) const { return encodePcm(m); }
        Bytes operator()(const AudioState& m) const {
            Writer w(audioStateType);
            w.u8(m.status.active).u32(uint32_t(m.status.result));
            putText(w, m.status.message, maxText);
            return w.u8(m.status.voiceProcessing).take();
        }
        Bytes operator()(const WindowList& m) const { return encodeWindowList(m); }
        Bytes operator()(const ModuleData& m) const {
            Writer w(moduleDataType);
            putText(w, m.id, RCS_MODULE_ID_MAX - 1);
            return w.u32(m.index).u32(m.count).raw(m.bytes).take();
        }
        Bytes operator()(const ModuleStatus& m) const {
            Writer w(moduleStatusType);
            putText(w, m.id, RCS_MODULE_ID_MAX - 1);
            w.u32(uint32_t(m.result));
            putText(w, m.message, maxText);
            return w.take();
        }
        Bytes operator()(const Failed& m) const {
            Writer w(failedType);
            w.u32(uint32_t(m.result));
            putText(w, m.message, maxText);
            return w.take();
        }
    };
    return std::visit(Encoder{}, message);
}

Message decode(std::span<const uint8_t> bytes) {
    Reader r(bytes);
    switch (r.u8()) {
    case agentHelloType: {
        AgentHello m{readMachine(r)};
        r.end();
        return m;
    }
    case acceptedType:
        r.end();
        return Accepted{};
    case refusedType: {
        Refused m;
        m.result = readError(r);
        m.reason = readText(r, maxText);
        r.end();
        return m;
    }
    case replacedType:
        r.end();
        return Replaced{};
    case heartbeatType:
        r.end();
        return Heartbeat{};
    case startVideoType:
        return decodeVideoStart(r);
    case stopVideoType: {
        StopVideo m;
        m.index = readIndex(r);
        m.encoding = readEncoding(r);
        r.end();
        return m;
    }
    case setRateType: {
        SetRate m;
        m.index = readIndex(r);
        m.encoding = readEncoding(r);
        m.bitrate = r.u32();
        m.budgeted = flag(r.u8());
        r.end();
        if (m.bitrate < 100'000 || m.bitrate > 500'000'000) {
            Reader::invalid("rate");
        }
        return m;
    }
    case setCursorInVideoType: {
        SetCursorInVideo m{flag(r.u8())};
        r.end();
        return m;
    }
    case setInputBoostType: {
        SetInputBoost m{r.u32()};
        r.end();
        if (m.ms > 1000) {
            Reader::invalid("input boost");
        }
        return m;
    }
    case invalidateAfterType: {
        InvalidateAfter m;
        m.index = readIndex(r);
        m.encoding = readEncoding(r);
        m.sequence = r.u64();
        r.end();
        return m;
    }
    case requestKeyframeType: {
        RequestKeyframe m;
        m.index = readIndex(r);
        m.encoding = readEncoding(r);
        r.end();
        return m;
    }
    case backlogType: {
        Backlog m;
        for (uint64_t& ns : m.ns) {
            ns = r.u64();
        }
        r.end();
        return m;
    }
    case startAudioType: {
        StartAudio m;
        m.start.backend = r.u32();
        m.start.excludeCurrentProcess = flag(r.u8());
        m.start.periodFrames = r.u32();
        m.start.microphone = flag(r.u8());
        m.start.device = r.u32();
        m.start.voiceProcessing = flag(r.u8());
        r.end();
        if (m.start.backend > RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK || m.start.periodFrames > 4800) {
            Reader::invalid("audio start");
        }
        return m;
    }
    case stopAudioType:
        r.end();
        return StopAudio{};
    case configureInputType: {
        ConfigureInput m;
        m.target = readTarget(r);
        const uint8_t displays = r.u8();
        if (displays >= RCS_MAX_STREAMS) {
            Reader::invalid("stream displays");
        }
        for (uint8_t i = 0; i < displays; ++i) {
            m.streamDisplays.push_back(r.u32());
        }
        m.options.secureDesktop = flag(r.u8());
        m.options.capsLock = r.u32();
        m.options.capsLockHoldMs = r.u32();
        if (m.options.capsLock > RCS_CAPS_LOCK_CAPS_LOCK || m.options.capsLockHoldMs < 100 ||
            m.options.capsLockHoldMs > 5000) {
            Reader::invalid("caps lock");
        }
        r.end();
        return m;
    }
    case applyInputType: {
        ApplyInput m;
        m.event = protocol::readInputEvent(r);
        r.end();
        if (!protocol::validInput(m.event)) {
            Reader::invalid("input event");
        }
        return m;
    }
    case releaseInputType:
        r.end();
        return ReleaseInput{};
    case moduleStartType: {
        ModuleStart m;
        m.id = readModuleId(r);
        m.version = r.u32();
        r.end();
        return m;
    }
    case moduleStopType: {
        ModuleStop m;
        m.id = readModuleId(r);
        r.end();
        return m;
    }
    case listWindowsType: {
        ListWindows m{r.u32()};
        r.end();
        return m;
    }
    case startedType:
        return decodeStarted(r);
    case packetType:
        return decodePacket(r);
    case contentType: {
        Content m;
        m.index = readIndex(r);
        m.content = readRect(r);
        m.display = readDisplayMode(r);
        r.end();
        return m;
    }
    case modeType: {
        Mode m;
        m.index = readIndex(r);
        m.encoding = readEncoding(r);
        m.codec = r.u32();
        m.lowLatency = flag(r.u8());
        m.refInvalidation = flag(r.u8());
        m.temporalLayers = r.u8();
        m.name = readText(r, maxName);
        r.end();
        if (!knownCodec(m.codec) || !m.temporalLayers || m.temporalLayers > RCS_SCALABILITY_L1T3) {
            Reader::invalid("encoder mode");
        }
        return m;
    }
    case captureType: {
        Capture m;
        m.index = readIndex(r);
        m.status.paused = r.u32();
        m.status.captured = r.u64();
        m.status.overwritten = r.u64();
        m.status.interruptions = r.u64();
        for (uint64_t& limited : m.status.limitedMs) {
            limited = r.u64();
        }
        r.end();
        if (m.status.paused > RCS_CAPTURE_PAUSE_GONE) { // Only the session says that no agent is there.
            Reader::invalid("capture pause");
        }
        return m;
    }
    case pointerType: {
        Pointer m;
        m.index = readIndex(r);
        m.state = readCursor(r);
        r.end();
        return m;
    }
    case displayListType: {
        DisplayList m{readDisplays(r)};
        r.end();
        return m;
    }
    case availabilityType: {
        Availability m{flag(r.u8())};
        r.end();
        return m;
    }
    case desktopType: {
        Desktop m;
        m.state.kind = r.u32();
        m.state.userLoggedOn = flag(r.u8());
        r.end();
        if (m.state.kind > RCS_DESKTOP_SECURE) {
            Reader::invalid("desktop");
        }
        return m;
    }
    case inputResultsType: {
        InputResults m;
        m.last = readResult(r);
        m.applied = r.u64();
        r.end();
        return m;
    }
    case pcmType:
        return decodePcm(r);
    case audioStateType: {
        AudioState m;
        m.status.active = flag(r.u8());
        m.status.result = readResult(r);
        m.status.message = readText(r, maxText);
        m.status.voiceProcessing = flag(r.u8());
        r.end();
        if (m.status.active && m.status.result != RCS_OK) {
            Reader::invalid("active sound with an error");
        }
        return m;
    }
    case windowListType:
        return decodeWindowList(r);
    case moduleDataType: {
        ModuleData m;
        m.id = readModuleId(r);
        m.index = r.u32();
        m.count = r.u32();
        const size_t size = r.remaining();
        if (!m.count || m.index >= m.count || m.count > RCS_MODULE_MAX_MESSAGE / modulePart || !size ||
            size > modulePart) {
            Reader::invalid("module data");
        }
        const auto payload = r.raw(size);
        m.bytes.assign(payload.begin(), payload.end());
        return m;
    }
    case moduleStatusType: {
        ModuleStatus m;
        m.id = readModuleId(r);
        m.result = readResult(r);
        m.message = readText(r, maxText);
        r.end();
        return m;
    }
    case failedType: {
        Failed m;
        m.result = readError(r);
        m.message = readText(r, maxText);
        r.end();
        return m;
    }
    case encodingFailedType: {
        EncodingFailed m;
        m.index = readIndex(r);
        m.encoding = readEncoding(r);
        m.result = readError(r);
        m.message = readText(r, maxText);
        r.end();
        if (!m.encoding) { // The main encoding's failure is the agent's (Failed).
            Reader::invalid("encoding failure");
        }
        return m;
    }
    default:
        Reader::invalid("unknown type");
    }
}

} // namespace rcsdk::agent::link
