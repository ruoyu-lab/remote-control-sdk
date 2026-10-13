#include "protocol/messages.hpp"

#include "media/av1.hpp"
#include "media/nal.hpp"
#include "media/vpx.hpp"
#include "modules/registry.hpp"
#include "protocol/wire.hpp"
#include "util/util.hpp"

#include <algorithm>

namespace rcsdk::protocol {
namespace {

using crypto::Bytes;

enum Type : uint8_t {
    videoType = 1,
    controlGrantType = 2,
    inputType = 3,
    streamType = 4,
    keyframeRequestType = 5,
    codecOfferType = 6,
    audioFormatType = 7,
    audioType = 8,
    viewerSettingsType = 9,
    cursorShapeType = 10,
    cursorPositionType = 11,
    audioOffType = 12,
    dataPartType = 13,
    dataDatagramType = 14,
    refusedType = 15,
    desktopStateType = 17,
    admissionType = 18,
    refLostType = 19,
    streamEndedType = 20,
    sharerPausedType = 21,
    forwarderHelloType = 22,
    forwarderWelcomeType = 23,
    pipeType = 24,
    peerJoinedType = 25,
    peerLeftType = 26,
    subscribeType = 27,
    pathReportType = 28,
    needKeyframeType = 29,
    peerMovedType = 30,
    sealedVideoType = 31,
    mediaKeyType = 32,
    mediaKeyAckType = 33,
    sealedAudioType = 34,
    pathStallsType = 35
};

// A reliable data message part and an unreliable datagram.
constexpr size_t maxDataPart = 64 * 1024, maxDataDatagram = RCS_DATA_MAX_DATAGRAM;
constexpr size_t maxRefusal = 512; // Bytes of a refusal's reason.

// A pipe's datagram: what the hop carries (a host's channel may carry larger ones).
constexpr size_t maxPipeBytes = RCS_HOST_MAX_DATAGRAM;
// A sealed picture: its message, the seal's tag, room for the header; a
// sealed sound packet likewise.
constexpr size_t maxSealedBytes = maxVideoBytes + 1024 + crypto::Aead::tagSize;
constexpr size_t maxSealedAudioBytes = maxAudioPacketBytes + 64 + crypto::Aead::tagSize;

// The flags byte: keyframe, whether the picture's recovery follows, and
// whether no later picture of its layer or a lower one refers to it.
constexpr uint8_t keyframeFlag = 1, recoversFlag = 2, discardableFlag = 4;
// Temporal layers a picture may be of (rcs_encoded_packet.temporal_id).
constexpr uint32_t maxTemporalId = 7;

Bytes encodeVideo(const EncodedPacket& p) {
    Writer w(videoType);
    w.u8(uint8_t(p.stream))
        .u8(uint8_t(p.encoding))
        .u8(uint8_t(p.temporalId))
        .u32(p.codec)
        .u8(uint8_t(p.picture.format))
        .u8(uint8_t(p.picture.matrix))
        .u8(uint8_t(p.picture.range))
        .u32(p.width)
        .u32(p.height)
        .u8(uint8_t((p.keyframe ? keyframeFlag : 0) | (p.recoversAfter ? recoversFlag : 0) |
                    (p.discardable ? discardableFlag : 0)))
        .u64(p.sequence)
        .u64(p.timestampNs);
    if (p.recoversAfter) {
        w.u64(*p.recoversAfter);
    }
    return w.u32(uint32_t(p.config.size())).u32(uint32_t(p.data.size())).raw(p.config).raw(p.data).take();
}

EncodedPacket decodeVideo(Reader& r) {
    EncodedPacket p;
    p.stream = r.u8();
    p.encoding = r.u8();
    p.temporalId = r.u8();
    p.codec = r.u32();
    p.picture.format = r.u8();
    p.picture.matrix = r.u8();
    p.picture.range = r.u8();
    p.width = r.u32();
    p.height = r.u32();
    const uint8_t flags = r.u8();
    p.keyframe = flags & keyframeFlag;
    p.discardable = flags & discardableFlag;
    p.sequence = r.u64();
    p.timestampNs = r.u64();
    if (flags & recoversFlag) {
        p.recoversAfter = r.u64();
    }
    if (flags > (keyframeFlag | recoversFlag | discardableFlag) || (p.recoversAfter && *p.recoversAfter >= p.sequence) ||
        p.stream >= RCS_MAX_STREAMS || p.encoding >= RCS_MAX_ENCODINGS || p.temporalId > maxTemporalId) {
        Reader::invalid("video packet flags");
    }
    const uint32_t configSize = r.u32(), dataSize = r.u32();
    const auto config = r.raw(configSize);
    const auto data = r.raw(dataSize);
    r.end();
    p.config.assign(config.begin(), config.end());
    p.data.assign(data.begin(), data.end());
    if (const auto problem = videoProblem(p); !problem.empty()) { // Before any decoder sees the bytes.
        Reader::invalid(("video packet: " + problem).c_str());
    }
    return p;
}

Bytes encodeStream(const rcs_stream_info& i) {
    const auto& h = i.hdr;
    return Writer(streamType)
        .u8(uint8_t(i.stream))
        .u8(uint8_t(i.encoding))
        .u8(uint8_t(i.encodings))
        .u8(uint8_t(i.layers))
        .u8(uint8_t(i.received_layers))
        .u32(i.codec)
        .u8(uint8_t(i.format))
        .u8(uint8_t(i.color.matrix))
        .u8(uint8_t(i.color.range))
        .u16(h.red_x)
        .u16(h.red_y)
        .u16(h.green_x)
        .u16(h.green_y)
        .u16(h.blue_x)
        .u16(h.blue_y)
        .u16(h.white_x)
        .u16(h.white_y)
        .u32(h.max_luminance)
        .u32(h.min_luminance)
        .u16(h.max_content_light)
        .u16(h.max_frame_average_light)
        .u32(i.width)
        .u32(i.height)
        .u32(i.frames_per_second)
        .u32(i.bitrate_bps)
        .u32(i.encoder_acceleration)
        .u32(i.content_x)
        .u32(i.content_y)
        .u32(i.content_width)
        .u32(i.content_height)
        .u32(i.display_width)
        .u32(i.display_height)
        .u32(i.display_refresh_hz)
        .u32(uint32_t(i.desktop_x))
        .u32(uint32_t(i.desktop_y))
        .u32(i.desktop_width)
        .u32(i.desktop_height)
        .u32(i.cursor)
        .take();
}

StreamAnnouncement decodeStream(Reader& r) {
    StreamAnnouncement s;
    auto& i = s.info;
    i.struct_size = sizeof(i);
    i.stream = r.u8();
    i.encoding = r.u8();
    i.encodings = r.u8();
    i.layers = r.u8();
    i.received_layers = r.u8();
    i.codec = r.u32();
    Picture picture;
    picture.format = r.u8();
    picture.matrix = r.u8();
    picture.range = r.u8();
    auto& h = i.hdr;
    h.red_x = r.u16();
    h.red_y = r.u16();
    h.green_x = r.u16();
    h.green_y = r.u16();
    h.blue_x = r.u16();
    h.blue_y = r.u16();
    h.white_x = r.u16();
    h.white_y = r.u16();
    h.max_luminance = r.u32();
    h.min_luminance = r.u32();
    h.max_content_light = r.u16();
    h.max_frame_average_light = r.u16();
    i.width = r.u32();
    i.height = r.u32();
    i.frames_per_second = r.u32();
    i.bitrate_bps = r.u32();
    i.encoder_acceleration = r.u32();
    i.content_x = r.u32();
    i.content_y = r.u32();
    i.content_width = r.u32();
    i.content_height = r.u32();
    i.display_width = r.u32();
    i.display_height = r.u32();
    i.display_refresh_hz = r.u32();
    i.desktop_x = int32_t(r.u32());
    i.desktop_y = int32_t(r.u32());
    i.desktop_width = r.u32();
    i.desktop_height = r.u32();
    i.cursor = r.u32();
    r.end();
    const bool contentInside = i.content_width && i.content_height && i.content_x <= i.width &&
                               i.content_y <= i.height && i.content_width <= i.width - i.content_x &&
                               i.content_height <= i.height - i.content_y;
    i.format = picture.format;
    i.color = colorOf(picture);
    // CIE x and y are at most 1 (50 000); SDR streams carry no HDR metadata.
    const bool metadata = std::max({h.red_x, h.red_y, h.green_x, h.green_y, h.blue_x, h.blue_y, h.white_x,
                                    h.white_y}) <= 50'000 &&
                          (hdr(picture.format) || sameHdr(h, rcs_hdr_metadata{}));
    if (!knownCodec(i.codec) || !validPicture(picture) || !metadata || !validStreamSize(i.codec, i.width, i.height) ||
        !i.frames_per_second ||
        i.frames_per_second > 240 || i.encoder_acceleration > RCS_ACCELERATION_SOFTWARE || !contentInside ||
        i.display_width > 65535 || i.display_height > 65535 || i.display_refresh_hz > 1000 ||
        i.desktop_width > 65535 || i.desktop_height > 65535 || i.cursor > RCS_CURSOR_SEPARATE ||
        i.stream >= RCS_MAX_STREAMS || !i.encodings || i.encodings > RCS_MAX_ENCODINGS || i.encoding >= i.encodings ||
        !i.layers || i.layers > RCS_SCALABILITY_L1T3 || !i.received_layers || i.received_layers > i.layers) {
        Reader::invalid("stream announcement");
    }
    return s;
}

bool validAudioCodec(uint32_t codec) {
    return codec >= RCS_AUDIO_OPUS && codec <= RCS_AUDIO_PCM;
}

Bytes encodeOffer(const CodecOffer& m) {
    if (m.largest.size() != m.codecs.size() || m.formats.size() != m.codecs.size()) {
        fail(RCS_ERROR_INTERNAL, "A codec offer without the largest picture and the picture formats of each codec");
    }
    Writer writer(codecOfferType);
    writer.u8(uint8_t(m.codecs.size()));
    for (size_t i = 0; i < m.codecs.size(); ++i) {
        writer.u32(m.codecs[i]).u32(m.largest[i].width).u32(m.largest[i].height).u32(m.formats[i]);
    }
    for (const auto* list : {&m.sendAudio, &m.receiveAudio}) {
        writer.u8(uint8_t(list->size()));
        for (const rcs_audio_codec codec : *list) {
            writer.u32(codec);
        }
    }
    writer.u8(uint8_t((m.sendsAudio ? 1 : 0) | (m.receivesAudio ? 2 : 0)));
    writer.u8(uint8_t(m.platform));
    writer.u32(m.inputTypes);
    return writer.take();
}

// `count` distinct codecs that `valid` accepts.
template <class Valid>
std::vector<uint32_t> readCodecs(Reader& r, Valid valid) {
    std::vector<uint32_t> codecs;
    const uint8_t count = r.u8();
    uint32_t seen = 0;
    for (uint8_t i = 0; i < count; ++i) {
        const uint32_t codec = r.u32();
        if (!valid(codec) || (seen & (1u << codec))) {
            Reader::invalid("codec offer");
        }
        seen |= 1u << codec;
        codecs.push_back(codec);
    }
    return codecs;
}

CodecOffer decodeOffer(Reader& r) {
    CodecOffer m;
    const uint8_t count = r.u8();
    uint32_t seen = 0;
    for (uint8_t i = 0; i < count; ++i) {
        const uint32_t codec = r.u32();
        const PictureSize largest{r.u32(), r.u32()};
        const uint32_t formats = r.u32();
        if (!knownCodec(codec) || (seen & (1u << codec)) || count > RCS_MAX_CODECS || largest.width < 16 ||
            largest.height < 16 || largest.width > codecLimit(codec).width ||
            largest.height > codecLimit(codec).height || !formats || (formats & ~allPictureFormats)) {
            Reader::invalid("codec offer");
        }
        seen |= 1u << codec;
        m.codecs.push_back(codec);
        m.largest.push_back(largest);
        m.formats.push_back(formats);
    }
    m.sendAudio = readCodecs(r, validAudioCodec);
    m.receiveAudio = readCodecs(r, validAudioCodec);
    const uint8_t sound = r.u8();
    m.platform = r.u8();
    m.inputTypes = r.u32();
    r.end();
    m.sendsAudio = sound & 1;
    m.receivesAudio = sound & 2;
    if (m.codecs.empty() || sound > 3 || m.platform > RCS_PLATFORM_WEB || (m.inputTypes & ~allInputTypes)) {
        Reader::invalid("empty codec offer");
    }
    return m;
}

Bytes encodeAudioFormat(const AudioFormat& m) {
    return Writer(audioFormatType)
        .u32(m.codec)
        .u32(m.bitrate)
        .u32(m.framesPerPacket)
        .u8(uint8_t(m.config.size()))
        .raw(m.config)
        .take();
}

AudioFormat decodeAudioFormat(Reader& r) {
    AudioFormat m;
    m.codec = r.u32();
    m.bitrate = r.u32();
    m.framesPerPacket = r.u32();
    const auto config = r.raw(r.u8());
    r.end();
    const uint32_t frames = m.codec == RCS_AUDIO_OPUS ? opusFrames : m.codec == RCS_AUDIO_AAC ? aacFrames : pcmFrames;
    if (!validAudioCodec(m.codec) || m.framesPerPacket != frames || m.bitrate > 2'000'000) {
        Reader::invalid("audio format");
    }
    m.config.assign(config.begin(), config.end());
    return m;
}

Bytes encodeAudio(const AudioPacket& m) {
    return Writer(audioType)
        .u32(m.sequence)
        .u32(m.frames)
        .u64(m.timestampNs)
        .u16(uint16_t(m.data.size()))
        .raw(m.data)
        .u16(uint16_t(m.previousFrames))
        .raw(m.previous)
        .take();
}

AudioPacket decodeAudio(Reader& r, size_t size) {
    constexpr size_t fixed = 1 + 4 + 4 + 8 + 2 + 2;
    AudioPacket m;
    m.sequence = r.u32();
    m.frames = r.u32();
    m.timestampNs = r.u64();
    const size_t length = r.u16();
    if (size < fixed + length) {
        Reader::invalid("audio packet");
    }
    const auto data = r.raw(length);
    m.previousFrames = r.u16();
    const auto previous = r.raw(size - fixed - length);
    r.end();
    if (data.empty() || data.size() + previous.size() > maxAudioPacketBytes || !m.frames || m.frames > aacFrames ||
        previous.empty() != (m.previousFrames == 0) || m.previousFrames > aacFrames) {
        Reader::invalid("audio packet");
    }
    m.data.assign(data.begin(), data.end());
    m.previous.assign(previous.begin(), previous.end());
    return m;
}

Bytes encodeViewerSettings(const ViewerSettings& m) {
    const auto& r = m.request;
    return Writer(viewerSettingsType)
        .u32(r.width)
        .u32(r.height)
        .u32(r.framesPerSecond)
        .u32(r.bitrate)
        .u32(r.codec)
        .u32(r.format)
        .u32(r.audioCodec)
        .u32(r.audioBitrate)
        .u32(m.cursor)
        .u32(m.streams)
        .u32(m.paused)
        .take();
}

ViewerSettings decodeViewerSettings(Reader& r) {
    ViewerSettings m;
    auto& q = m.request;
    q.width = r.u32();
    q.height = r.u32();
    q.framesPerSecond = r.u32();
    q.bitrate = r.u32();
    q.codec = r.u32();
    q.format = r.u32();
    q.audioCodec = r.u32();
    q.audioBitrate = r.u32();
    m.cursor = r.u32();
    m.streams = r.u32();
    m.paused = r.u32();
    r.end();
    const bool size = (!q.width && !q.height) || validPictureSize(q.width, q.height); // The sharer fits it.
    if (!size || q.framesPerSecond > 240 || q.bitrate > 500'000'000 || (q.codec && !knownCodec(q.codec)) ||
        (q.format && !knownPictureFormat(q.format)) ||
        (q.audioCodec && !validAudioCodec(q.audioCodec)) || q.audioBitrate > 510'000 ||
        m.cursor > RCS_CURSOR_SEPARATE || m.streams >= (1u << RCS_MAX_STREAMS) ||
        (m.paused & ~(RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO))) {
        Reader::invalid("viewer settings");
    }
    return m;
}

Bytes encodeCursorShape(const CursorShapePart& m) {
    return Writer(cursorShapeType)
        .u32(m.id)
        .u32(m.type)
        .u32(m.width)
        .u32(m.height)
        .u32(m.hotspotX)
        .u32(m.hotspotY)
        .u32(m.packedSize)
        .u32(m.offset)
        .raw(m.bytes)
        .take();
}

CursorShapePart decodeCursorShape(Reader& r, size_t size) {
    CursorShapePart m;
    m.id = r.u32();
    m.type = r.u32();
    m.width = r.u32();
    m.height = r.u32();
    m.hotspotX = r.u32();
    m.hotspotY = r.u32();
    m.packedSize = r.u32();
    m.offset = r.u32();
    const auto bytes = r.raw(size - 33);
    r.end();
    // A packed image takes at most 4 bytes and 1/128 of a header per pixel.
    const uint64_t pixels = uint64_t(m.width) * m.height;
    if (!m.id || m.type > RCS_CURSOR_HELP || !m.width || !m.height || m.width > maxCursorSize ||
        m.height > maxCursorSize || m.hotspotX >= m.width || m.hotspotY >= m.height || bytes.empty() ||
        bytes.size() > maxCursorPart || m.packedSize > pixels * 4 + pixels / 128 + 1 || m.offset >= m.packedSize ||
        bytes.size() > m.packedSize - m.offset) {
        Reader::invalid("cursor shape");
    }
    m.bytes.assign(bytes.begin(), bytes.end());
    return m;
}

Bytes encodeInput(const Input& m) {
    Writer writer(inputType);
    writer.u64(m.epoch);
    putInputEvent(writer, m.event);
    return writer.take();
}

Input decodeInput(Reader& r) {
    Input m;
    m.epoch = r.u64();
    m.event = readInputEvent(r);
    r.end();
    if (!validInput(m.event)) {
        Reader::invalid("input event");
    }
    return m;
}

Bytes encodePathReport(const PathReport& m) {
    Writer w(pathReportType);
    w.u8(uint8_t(m.peers.size()));
    for (const auto& p : m.peers) {
        w.u64(p.peer).u32(p.targetBps).u32(p.lossPermille).u32(p.queueUs).u32(p.roundTripUs).u32(p.backlogUs);
        w.u32(p.drops).u64(p.bytes);
    }
    return w.take();
}

PathReport decodePathReport(Reader& r) {
    PathReport m;
    const uint8_t count = r.u8();
    if (!count || count > maxPathReportPeers) {
        Reader::invalid("path report");
    }
    for (uint8_t i = 0; i < count; ++i) {
        PathReport::Peer p;
        p.peer = r.u64();
        p.targetBps = r.u32();
        p.lossPermille = r.u32();
        p.queueUs = r.u32();
        p.roundTripUs = r.u32();
        p.backlogUs = r.u32();
        p.drops = r.u32();
        p.bytes = r.u64();
        if (p.lossPermille > 1000) {
            Reader::invalid("path report");
        }
        m.peers.push_back(p);
    }
    r.end();
    return m;
}

SealedVideo decodeSealedVideo(Reader& r) {
    SealedVideo m;
    m.generation = r.u32();
    m.step = r.u32();
    m.counter = r.u64();
    m.stream = r.u8();
    m.encoding = r.u8();
    m.temporal = r.u8();
    m.flags = r.u8();
    const auto sealed = r.raw(r.remaining());
    if (m.stream >= RCS_MAX_STREAMS || m.encoding >= RCS_MAX_ENCODINGS || m.temporal > maxTemporalId ||
        m.flags > (SealedVideo::keyframeFlag | SealedVideo::discardableFlag) ||
        sealed.size() <= crypto::Aead::tagSize || sealed.size() > maxSealedBytes) {
        Reader::invalid("sealed video");
    }
    m.sealed.assign(sealed.begin(), sealed.end());
    return m;
}

SealedAudio decodeSealedAudio(Reader& r) {
    SealedAudio m;
    m.generation = r.u32();
    m.step = r.u32();
    m.counter = r.u64();
    m.sound = r.u8();
    const auto sealed = r.raw(r.remaining());
    if (!m.sound || sealed.size() <= crypto::Aead::tagSize || sealed.size() > maxSealedAudioBytes) {
        Reader::invalid("sealed sound");
    }
    m.sealed.assign(sealed.begin(), sealed.end());
    return m;
}

} // namespace

Bytes sealedHeader(const SealedAudio& m) {
    return Writer(sealedAudioType).u32(m.generation).u32(m.step).u64(m.counter).u8(m.sound).take();
}

Bytes sealedHeader(const SealedVideo& m) {
    return Writer(sealedVideoType)
        .u32(m.generation)
        .u32(m.step)
        .u64(m.counter)
        .u8(m.stream)
        .u8(m.encoding)
        .u8(m.temporal)
        .u8(m.flags)
        .take();
}

bool validStreamSize(rcs_codec codec, uint32_t width, uint32_t height) noexcept {
    return validPictureSize(width, height) && takes(codecLimit(codec), width, height);
}

std::string videoProblem(const EncodedPacket& p) {
    if (!knownCodec(p.codec)) {
        return "a codec no version defines";
    }
    if (!validPicture(p.picture)) {
        return "a picture format no version defines, or a matrix or range its format does not take";
    }
    if (!validStreamSize(p.codec, p.width, p.height)) {
        return "an odd size, one below 16x16, or one beyond what " + std::string(codecName(p.codec)) + " takes (" +
               limitText(codecLimit(p.codec)) + ")";
    }
    if (p.data.empty()) {
        return "no data";
    }
    if (p.data.size() + p.config.size() > maxVideoBytes) {
        return "more data and config than one packet carries (" + std::to_string(maxVideoBytes) + " bytes)";
    }
    if (hostCodec(p.codec)) {
        return {};
    }
    if (vpCodec(p.codec)) {
        return vpx::packetProblem(p.codec, p.picture, p.width, p.height, p.keyframe, p.data, p.config);
    }
    const bool av1 = p.codec == RCS_CODEC_AV1;
    if (p.config.empty()) {
        return av1 ? "no sequence header (config)" : "no parameter sets (config)";
    }
    try {
        if (av1) {
            const auto header = av1::obus(p.config);
            av1::obus(p.data);
            if (header.size() != 1 || header[0].type != av1::obuSequenceHeader) {
                return "config is not exactly one sequence header OBU";
            }
        } else {
            nal::units(p.config);
            nal::units(p.data);
        }
    } catch (const std::runtime_error&) {
        return av1 ? "broken OBU boundaries" : "broken NAL unit boundaries (each needs a 4-byte big-endian length)";
    }
    return {};
}

void putInputEvent(Writer& w, const rcs_input_event& e) {
    w.u32(e.type).u32(e.x).u32(e.y).u32(e.stream).u32(uint32_t(e.dx)).u32(uint32_t(e.dy)).u32(e.code).u32(e.down).u32(e.repeat);
    w.u32(uint32_t(e.wheel_x)).u32(uint32_t(e.wheel_y));
    w.u32(e.pressure).u32(uint32_t(e.tilt_x)).u32(uint32_t(e.tilt_y)).u32(e.rotation).u32(e.pen);
}

rcs_input_event readInputEvent(Reader& r) {
    rcs_input_event e = RCS_INPUT_EVENT_INIT;
    e.type = r.u32();
    e.x = r.u32();
    e.y = r.u32();
    e.stream = r.u32();
    e.dx = int32_t(r.u32());
    e.dy = int32_t(r.u32());
    e.code = r.u32();
    e.down = r.u32();
    e.repeat = r.u32();
    e.wheel_x = int32_t(r.u32());
    e.wheel_y = int32_t(r.u32());
    e.pressure = r.u32();
    e.tilt_x = int32_t(r.u32());
    e.tilt_y = int32_t(r.u32());
    e.rotation = r.u32();
    e.pen = r.u32();
    return e;
}

namespace {
bool within(int32_t value, int32_t limit) noexcept {
    return value >= -limit && value <= limit;
}
} // namespace

bool validInput(const rcs_input_event& e) noexcept {
    if (e.struct_size != sizeof(e) || e.type < RCS_INPUT_MOVE || e.type > RCS_INPUT_SECURE_ATTENTION || e.x > 65535 ||
        e.y > 65535 || e.stream >= RCS_MAX_STREAMS || e.down > 1 || e.repeat > 1 || !within(e.dx, 10'000) || !within(e.dy, 10'000) ||
        e.pressure > 1024 || !within(e.tilt_x, 90) || !within(e.tilt_y, 90) || e.rotation > 359 ||
        e.pen > (RCS_PEN_BARREL | RCS_PEN_ERASER | RCS_PEN_LEAVE)) {
        return false;
    }
    switch (e.type) {
    case RCS_INPUT_BUTTON:
        return e.code >= RCS_BUTTON_LEFT && e.code <= RCS_BUTTON_FORWARD;
    case RCS_INPUT_KEY:
        return (e.code >= 4 && e.code <= 231) ||
               ((e.code & 0xFFFF0000u) == RCS_KEY_CONSUMER && (e.code & 0xFFFFu) && (e.code & 0xFFFFu) <= 0x0FFF);
    case RCS_INPUT_TEXT:
        return e.code == '\t' || e.code == '\n' ||
               (e.code >= 0x20 && e.code != 0x7F && e.code <= 0x10FFFF && (e.code < 0xD800 || e.code > 0xDFFF));
    case RCS_INPUT_WHEEL:
        return within(e.wheel_x, 1000) && within(e.wheel_y, 1000);
    case RCS_INPUT_SCROLL:
        return within(e.wheel_x, 120'000) && within(e.wheel_y, 120'000);
    case RCS_INPUT_TOUCH:
        return e.code <= 9;
    default:
        return true;
    }
}

Bytes encodeModuleList(const ModuleList& list) {
    Writer w(uint8_t(list.modules.size())); // The first byte: how many.
    for (const auto& entry : list.modules) {
        w.u8(uint8_t(entry.id.size()))
            .raw({reinterpret_cast<const uint8_t*>(entry.id.data()), entry.id.size()})
            .u32(entry.version)
            .u16(uint16_t(entry.hello.size()))
            .raw(entry.hello);
    }
    return w.take();
}

ModuleList decodeModuleList(std::span<const uint8_t> bytes) {
    Reader r(bytes);
    ModuleList list;
    const uint8_t count = r.u8();
    if (count > RCS_MODULES_MAX) {
        Reader::invalid("module list");
    }
    for (uint8_t i = 0; i < count; ++i) {
        ModuleList::Entry entry;
        const auto id = r.raw(r.u8());
        entry.id.assign(id.begin(), id.end());
        entry.version = r.u32();
        const auto hello = r.raw(r.u16());
        entry.hello.assign(hello.begin(), hello.end());
        const bool twice = std::any_of(list.modules.begin(), list.modules.end(),
                                       [&](const ModuleList::Entry& e) { return e.id == entry.id; });
        if (!modules::validId(entry.id) || twice || entry.hello.size() > RCS_MODULE_HELLO_MAX) {
            Reader::invalid("module list");
        }
        list.modules.push_back(std::move(entry));
    }
    r.end();
    return list;
}

Bytes encode(const Message& message) {
    struct Encoder {
        Bytes operator()(const CodecOffer& m) const { return encodeOffer(m); }
        Bytes operator()(const ViewerSettings& m) const { return encodeViewerSettings(m); }
        Bytes operator()(const CursorShapePart& m) const { return encodeCursorShape(m); }
        Bytes operator()(const CursorPosition& m) const {
            return Writer(cursorPositionType).u8(m.visible).u8(m.stream).u32(m.x).u32(m.y).u32(m.shapeId).take();
        }
        Bytes operator()(const StreamAnnouncement& m) const { return encodeStream(m.info); }
        Bytes operator()(const EncodedPacket& m) const { return encodeVideo(m); }
        Bytes operator()(const ControlGrant& m) const {
            return Writer(controlGrantType).u8(m.allowed).u64(m.epoch).take();
        }
        Bytes operator()(const Input& m) const { return encodeInput(m); }
        Bytes operator()(const KeyframeRequest& m) const {
            return Writer(keyframeRequestType).u8(m.stream).u8(m.encoding).take();
        }
        Bytes operator()(const StreamGap&) const { fail(RCS_ERROR_INTERNAL, "A stream gap is never sent"); }
        Bytes operator()(const VideoMissing&) const { fail(RCS_ERROR_INTERNAL, "A missing picture is never told"); }
        Bytes operator()(const AudioFormat& m) const { return encodeAudioFormat(m); }
        Bytes operator()(const AudioPacket& m) const { return encodeAudio(m); }
        Bytes operator()(const AudioOff& m) const { return Writer(audioOffType).u8(m.reason).take(); }
        Bytes operator()(const DataPart& m) const {
            return Writer(dataPartType)
                .u8(m.kind)
                .u8(m.module)
                .u32(m.channel)
                .u32(m.index)
                .u32(m.count)
                .raw(m.bytes)
                .take();
        }
        Bytes operator()(const DataDatagram& m) const {
            return Writer(dataDatagramType).u8(m.module).u32(m.channel).raw(m.bytes).take();
        }
        Bytes operator()(const Admission& m) const { return Writer(admissionType).u32(m.remainingMs).take(); }
        Bytes operator()(const StreamEnded& m) const { return Writer(streamEndedType).u8(m.stream).take(); }
        Bytes operator()(const SharerPaused& m) const { return Writer(sharerPausedType).u32(m.paused).take(); }
        Bytes operator()(const RefLost& m) const {
            return Writer(refLostType).u8(m.stream).u8(m.encoding).u64(m.after).u8(m.gaveUp).take();
        }
        Bytes operator()(const DesktopState& m) const {
            return Writer(desktopStateType).u8(uint8_t(m.kind)).u8(m.userLoggedOn).take();
        }
        Bytes operator()(const ForwarderHello& m) const {
            const auto token = std::span(reinterpret_cast<const uint8_t*>(m.token.data()), m.token.size());
            return Writer(forwarderHelloType).u8(m.role).raw(token.first(std::min(token.size(), maxForwarderToken))).take();
        }
        Bytes operator()(const ForwarderWelcome& m) const {
            return Writer(forwarderWelcomeType).u64(m.peer).u8(m.sharer).u32(m.cacheMs).take();
        }
        Bytes operator()(const Pipe& m) const { return Writer(pipeType).u64(m.peer).raw(m.bytes).take(); }
        Bytes operator()(const PeerJoined& m) const { return Writer(peerJoinedType).u64(m.peer).u64(m.route).take(); }
        Bytes operator()(const PeerLeft& m) const { return Writer(peerLeftType).u64(m.peer).take(); }
        Bytes operator()(const Subscribe& m) const {
            return Writer(subscribeType)
                .u64(m.peer)
                .u32(m.streams)
                .u8(m.encoding)
                .u8(m.sound)
                .u32(m.keyGeneration)
                .u32(m.keyStep)
                .take();
        }
        Bytes operator()(const PathReport& m) const { return encodePathReport(m); }
        Bytes operator()(const NeedKeyframe& m) const {
            return Writer(needKeyframeType).u8(m.stream).u8(m.encoding).take();
        }
        Bytes operator()(const PeerMoved& m) const { return Writer(peerMovedType).u64(m.peer).u8(m.encoding).take(); }
        Bytes operator()(const SealedVideo& m) const {
            auto bytes = sealedHeader(m);
            bytes.insert(bytes.end(), m.sealed.begin(), m.sealed.end());
            return bytes;
        }
        Bytes operator()(const SealedAudio& m) const {
            auto bytes = sealedHeader(m);
            bytes.insert(bytes.end(), m.sealed.begin(), m.sealed.end());
            return bytes;
        }
        Bytes operator()(const MediaKey& m) const {
            return Writer(mediaKeyType).u32(m.generation).u32(m.step).raw(m.key).take();
        }
        Bytes operator()(const MediaKeyAck& m) const {
            return Writer(mediaKeyAckType).u32(m.generation).u32(m.step).take();
        }
        Bytes operator()(const PathStalls& m) const {
            return Writer(pathStallsType).u32(m.periodMs).u32(m.stallMs).u32(m.stalls).take();
        }
        Bytes operator()(const Refused& m) const {
            const auto reason = std::span(reinterpret_cast<const uint8_t*>(m.reason.data()), m.reason.size());
            return Writer(refusedType)
                .u32(uint32_t(m.result))
                .raw(reason.first(std::min(reason.size(), maxRefusal)))
                .take();
        }
    };
    return std::visit(Encoder{}, message);
}

Message decode(std::span<const uint8_t> bytes) {
    Reader r(bytes);
    switch (r.u8()) {
    case videoType:
        return decodeVideo(r);
    case controlGrantType: {
        ControlGrant grant;
        const uint8_t allowed = r.u8();
        grant.epoch = r.u64();
        r.end();
        if (allowed > 1) {
            Reader::invalid("control grant");
        }
        grant.allowed = allowed;
        return grant;
    }
    case inputType:
        return decodeInput(r);
    case streamType:
        return decodeStream(r);
    case keyframeRequestType: {
        KeyframeRequest m;
        m.stream = r.u8();
        m.encoding = r.u8();
        r.end();
        if (m.stream >= RCS_MAX_STREAMS || m.encoding >= RCS_MAX_ENCODINGS) {
            Reader::invalid("keyframe request");
        }
        return m;
    }
    case codecOfferType:
        return decodeOffer(r);
    case audioFormatType:
        return decodeAudioFormat(r);
    case audioType:
        if (bytes.size() < 18) {
            Reader::invalid("audio packet");
        }
        return decodeAudio(r, bytes.size());
    case viewerSettingsType:
        return decodeViewerSettings(r);
    case dataPartType: {
        DataPart m;
        const uint8_t kind = r.u8();
        m.module = r.u8();
        m.channel = r.u32();
        m.index = r.u32();
        m.count = r.u32();
        const auto payload = r.raw(r.remaining());
        // Only data has a channel, only a module's message or hello a module;
        // only a hello may be empty, and it comes in one part.
        const bool numbered = kind == DataPart::moduleMessage || kind == DataPart::moduleHello;
        const bool emptyHello = kind == DataPart::moduleHello && m.count == 1;
        if (kind > DataPart::moduleHello || (m.channel && kind != DataPart::data) ||
            (numbered ? !m.module || m.module > RCS_MODULES_MAX : m.module != 0) || !m.count || m.index >= m.count ||
            (payload.empty() && !emptyHello) || payload.size() > maxDataPart) {
            Reader::invalid("data part");
        }
        m.kind = DataPart::Kind(kind);
        m.bytes.assign(payload.begin(), payload.end());
        return m;
    }
    case dataDatagramType: {
        DataDatagram m;
        m.module = r.u8();
        m.channel = r.u32();
        const auto payload = r.raw(r.remaining());
        if (m.module > RCS_MODULES_MAX || (m.module && m.channel) || payload.empty() ||
            payload.size() > maxDataDatagram) {
            Reader::invalid("data datagram");
        }
        m.bytes.assign(payload.begin(), payload.end());
        return m;
    }
    case refusedType: {
        const auto result = rcs_result(int32_t(r.u32()));
        const auto reason = r.raw(r.remaining());
        if ((result != RCS_ERROR_AUTHENTICATION && result != RCS_ERROR_REFUSED && result != RCS_ERROR_TIMEOUT &&
             result != RCS_ERROR_UNSUPPORTED && result != RCS_ERROR_BUSY) ||
            reason.size() > maxRefusal) {
            Reader::invalid("refusal");
        }
        return Refused{result, std::string(reason.begin(), reason.end())};
    }
    case desktopStateType: {
        DesktopState m;
        const uint8_t kind = r.u8(), loggedOn = r.u8();
        r.end();
        if (kind > RCS_DESKTOP_SECURE || loggedOn > 1) {
            Reader::invalid("desktop state");
        }
        m.kind = kind;
        m.userLoggedOn = loggedOn != 0;
        return m;
    }
    case streamEndedType: {
        StreamEnded m{r.u8()};
        r.end();
        if (m.stream >= RCS_MAX_STREAMS) {
            Reader::invalid("stream end");
        }
        return m;
    }
    case sharerPausedType: {
        SharerPaused m{r.u32()};
        r.end();
        if (m.paused & ~(RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO)) {
            Reader::invalid("sharer's pause");
        }
        return m;
    }
    case refLostType: {
        RefLost m;
        m.stream = r.u8();
        m.encoding = r.u8();
        m.after = r.u64();
        const uint8_t gaveUp = r.u8();
        r.end();
        if (gaveUp > 1 || m.stream >= RCS_MAX_STREAMS || m.encoding >= RCS_MAX_ENCODINGS) {
            Reader::invalid("lost picture");
        }
        m.gaveUp = gaveUp;
        return m;
    }
    case admissionType: {
        Admission m;
        m.remainingMs = r.u32();
        r.end();
        return m;
    }
    case audioOffType: {
        const uint8_t reason = r.u8();
        r.end();
        if (reason > AudioOff::noCommonCodec) {
            Reader::invalid("audio off");
        }
        return AudioOff{AudioOff::Reason(reason)};
    }
    case cursorShapeType:
        if (bytes.size() < 34) {
            Reader::invalid("cursor shape");
        }
        return decodeCursorShape(r, bytes.size());
    case cursorPositionType: {
        CursorPosition m;
        const uint8_t visible = r.u8();
        m.stream = r.u8();
        m.x = r.u32();
        m.y = r.u32();
        m.shapeId = r.u32();
        r.end();
        if (visible > 1 || m.stream >= RCS_MAX_STREAMS || m.x > 65535 || m.y > 65535) {
            Reader::invalid("cursor position");
        }
        m.visible = visible;
        return m;
    }
    case forwarderHelloType: {
        ForwarderHello m;
        const uint8_t role = r.u8();
        const auto token = r.raw(r.remaining());
        if ((role != ForwarderHello::publish && role != ForwarderHello::subscribe) || token.empty() ||
            token.size() > maxForwarderToken) {
            Reader::invalid("forwarder hello");
        }
        m.role = ForwarderHello::Role(role);
        m.token.assign(token.begin(), token.end());
        return m;
    }
    case forwarderWelcomeType: {
        ForwarderWelcome m;
        m.peer = r.u64();
        const uint8_t sharer = r.u8();
        m.cacheMs = r.u32();
        r.end();
        if (sharer > 1) {
            Reader::invalid("forwarder welcome");
        }
        m.sharer = sharer;
        return m;
    }
    case pipeType: {
        Pipe m;
        m.peer = r.u64();
        const auto payload = r.raw(r.remaining());
        if (payload.empty() || payload.size() > maxPipeBytes) {
            Reader::invalid("pipe");
        }
        m.bytes.assign(payload.begin(), payload.end());
        return m;
    }
    case peerJoinedType: {
        PeerJoined m;
        m.peer = r.u64();
        m.route = r.u64();
        r.end();
        return m;
    }
    case peerLeftType: {
        PeerLeft m{r.u64()};
        r.end();
        return m;
    }
    case subscribeType: {
        Subscribe m;
        m.peer = r.u64();
        m.streams = r.u32();
        m.encoding = r.u8();
        m.sound = r.u8();
        m.keyGeneration = r.u32();
        m.keyStep = r.u32();
        r.end();
        if (m.streams >= (1u << RCS_MAX_STREAMS) || m.encoding >= RCS_MAX_ENCODINGS) {
            Reader::invalid("subscription");
        }
        return m;
    }
    case pathReportType:
        return decodePathReport(r);
    case needKeyframeType: {
        NeedKeyframe m;
        m.stream = r.u8();
        m.encoding = r.u8();
        r.end();
        if (m.stream >= RCS_MAX_STREAMS || m.encoding >= RCS_MAX_ENCODINGS) {
            Reader::invalid("keyframe need");
        }
        return m;
    }
    case peerMovedType: {
        PeerMoved m;
        m.peer = r.u64();
        m.encoding = r.u8();
        r.end();
        if (m.encoding >= RCS_MAX_ENCODINGS) {
            Reader::invalid("peer moved");
        }
        return m;
    }
    case sealedVideoType:
        return decodeSealedVideo(r);
    case sealedAudioType:
        return decodeSealedAudio(r);
    case mediaKeyType: {
        MediaKey m;
        m.generation = r.u32();
        m.step = r.u32();
        const auto key = r.raw(m.key.size());
        r.end();
        if (!m.generation) {
            Reader::invalid("media key");
        }
        std::copy(key.begin(), key.end(), m.key.begin());
        return m;
    }
    case mediaKeyAckType: {
        MediaKeyAck m;
        m.generation = r.u32();
        m.step = r.u32();
        r.end();
        return m;
    }
    case pathStallsType: {
        PathStalls m;
        m.periodMs = r.u32();
        m.stallMs = r.u32();
        m.stalls = r.u32();
        r.end();
        // What a stall watch says: a pace of 0.2..3 s, stalls of the last 10 s.
        if ((m.periodMs && (m.periodMs < 200 || m.periodMs > 3000)) || m.stallMs > 10'000 || m.stalls > 64) {
            Reader::invalid("path stalls");
        }
        return m;
    }
    default:
        Reader::invalid("unknown type");
    }
}

} // namespace rcsdk::protocol
