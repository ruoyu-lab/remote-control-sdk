#include "session/config.hpp"

#include "media/audio.hpp"
#include "media/picture_size.hpp"
#include "platform/platform.hpp"
#include "protocol/messages.hpp"
#include "protocol/udp_connection.hpp"
#include "transport/ice.hpp"
#include "transport/local_stream.hpp"
#include "transport/socket.hpp"
#include "transport/websocket.hpp"
#include "util/util.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace rcsdk::session {
namespace {

[[noreturn]] void invalid(const std::string& message) {
    fail(RCS_ERROR_INVALID_ARGUMENT, message);
}

void require(bool condition, const char* extension, const std::string& message) {
    if (!condition) {
        invalid(std::string(extension) + ": " + message);
    }
}

// Copies an extension of exactly this library's size; any other comes from
// other headers or was not initialised with its *_INIT macro.
template <class T>
T read(const rcs_extension& extension, const char* name) {
    require(extension.struct_size == sizeof(T), name,
            "struct_size is not this library's; initialise the extension with its *_INIT macro from the "
            "headers of this library (" RCSDK_VERSION_STRING ")");
    T value;
    std::memcpy(&value, &extension, sizeof(T));
    value.header.next = nullptr;
    return value;
}

// Any size a stream may have; whether a codec takes it is for the checks
// against the machine and the peer, and for the stream plan.
constexpr char sizeRange[] = "even, at least 16x16, at most 8192 a side and 35 651 584 pixels (8192x4352)";

bool validMaxSide(uint32_t side) noexcept {
    return !side || (side >= 16 && side <= maxPicture.width && side % 2 == 0);
}

bool validBitrate(uint32_t bitrate) noexcept {
    return bitrate >= 128'000 && bitrate <= 500'000'000;
}

// codecs[] (formats[]: `field`, `item`): at least one (unless `empty`), no gaps or duplicates, each `known`.
template <class Codec, size_t N, class Known>
void checkCodecList(const Codec (&codecs)[N], Known known, const char* name, const char* field = "codecs[]",
                    const char* item = "codec", bool empty = false) {
    uint32_t seen = 0;
    bool ended = false;
    const auto say = [&](const char* what) { return std::string(field) + " " + what + " " + item; };
    for (const Codec value : codecs) {
        if (!value) {
            ended = true;
            continue;
        }
        require(!ended, name, (std::string(field) + " must not have gaps").c_str());
        require(known(value), name, say("holds an unknown").c_str());
        require(!(seen & (1u << value)), name, (say("lists a") + " twice").c_str());
        seen |= 1u << value;
    }
    require(seen != 0 || empty, name, say("lists no").c_str());
}

void checkFormatList(const rcs_picture_format (&formats)[RCS_MAX_PICTURE_FORMATS], const char* name) {
    checkCodecList(formats, knownPictureFormat, name, "formats[]", "picture format");
}

// A host codec's picture_formats: a mask of formats some version defines.
void checkFormatMask(uint32_t formats, const char* name) {
    require(formats && !(formats & ~allPictureFormats), name,
            "picture_formats must hold (1u << rcs_picture_format) bits of formats this version defines");
}

bool knownAudioCodec(uint32_t codec) noexcept {
    return codec >= RCS_AUDIO_OPUS && codec <= RCS_AUDIO_PCM;
}

void checkImplementation(rcs_acceleration acceleration, rcs_backend backend, const char* name) {
    require(acceleration <= RCS_ACCELERATION_HARDWARE_FIRST, name, "unknown acceleration");
    require(choosableBackend(backend), name,
            backend == RCS_BACKEND_HOST ? "RCS_BACKEND_HOST is reported, not chosen: give RCS_EXT_HOST_ENCODER or "
                                          "RCS_EXT_HOST_DECODER"
                                        : "unknown backend");
}

// The host's own codec: the codecs it handles, its name and its calls.
template <class Config>
void checkHostCodec(const Config& c, const char* name) {
    checkCodecList(c.codecs, knownCodec, name);
    checkFormatMask(c.picture_formats, name);
    require(c.acceleration <= RCS_ACCELERATION_SOFTWARE, name, "unknown acceleration");
    require(std::memchr(c.name, 0, sizeof(c.name)) && c.name[0], name, "name must be non-empty and NUL-terminated");
    require(c.open && c.close, name, "open and close are required");
}

// ---- One parser per extension ---------------------------------------------------------

void parseShareVideo(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto v = read<rcs_share_video_config>(extension, name);
    require(validPictureSize(v.width, v.height), name, (std::string("width and height must be ") + sizeRange).c_str());
    require(v.frames_per_second >= 1 && v.frames_per_second <= 240, name, "frames_per_second must be 1..240");
    require(validBitrate(v.bitrate_bps), name, "bitrate_bps must be 128 kbit/s..500 Mbit/s");
    require(v.rate_mode <= RCS_RATE_ADAPTIVE, name, "unknown rate_mode");
    require(!v.min_bitrate_bps || (v.min_bitrate_bps >= 100'000 && v.min_bitrate_bps <= v.bitrate_bps), name,
            "min_bitrate_bps must be 0 or 100 kbit/s..bitrate_bps");
    require(v.follow_requests <= 1, name, "follow_requests must be 0 or 1");
    require(validMaxSide(v.max_width), name, "max_width must be 0 or even, 16..8192");
    require(validMaxSide(v.max_height), name, "max_height must be 0 or even, 16..8192");
    require(v.max_frames_per_second <= 240, name, "max_frames_per_second must be 0..240");
    require(!v.max_bitrate_bps || validBitrate(v.max_bitrate_bps), name,
            "max_bitrate_bps must be 0 or 128 kbit/s..500 Mbit/s");
    next.shareVideo = v;
}

void parseVideoTuning(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto r = read<rcs_video_tuning_config>(extension, name);
    require(r.overuse_queue_ms >= 2 && r.overuse_queue_ms <= 1000, name, "overuse_queue_ms must be 2..1000");
    require(r.severe_queue_ms >= r.overuse_queue_ms && r.severe_queue_ms <= 5000, name,
            "severe_queue_ms must be overuse_queue_ms..5000");
    require(r.clear_queue_ms >= 1 && r.clear_queue_ms < r.overuse_queue_ms, name,
            "clear_queue_ms must be 1..overuse_queue_ms - 1");
    require(r.overuse_persist_ms <= 2000, name, "overuse_persist_ms must be 0..2000");
    require(r.overuse_loss_permille >= 2 && r.overuse_loss_permille <= 1000, name,
            "overuse_loss_permille must be 2..1000");
    require(r.clear_loss_permille >= 1 && r.clear_loss_permille < r.overuse_loss_permille, name,
            "clear_loss_permille must be 1..overuse_loss_permille - 1");
    require(r.decrease_percent >= 10 && r.decrease_percent <= 95, name, "decrease_percent must be 10..95");
    require(r.decrease_interval_ms >= 50 && r.decrease_interval_ms <= 10'000, name,
            "decrease_interval_ms must be 50..10 000");
    require(r.probe_increase_percent <= 1000 && r.increase_percent <= 1000 && r.recovery_increase_percent <= 1000,
            name, "probe_increase_percent, increase_percent and recovery_increase_percent must be 0..1000");
    require(r.frame_spread_us <= 100'000, name, "frame_spread_us must be 0..100 000");
    require(r.max_pacing_bps >= 1'000'000 && r.max_pacing_bps <= 4'000'000'000u, name,
            "max_pacing_bps must be 1 Mbit/s..4000 Mbit/s");
    require(!r.path_pacing_percent || (r.path_pacing_percent >= 110 && r.path_pacing_percent <= 1000), name,
            "path_pacing_percent must be 0 or 110..1000");
    require(r.path_burst_us <= 100'000, name, "path_burst_us must be 0..100 000");
    require(r.fec_mode <= RCS_FEC_FIXED, name, "unknown fec_mode");
    require(r.fec_percent >= 1 && r.fec_percent <= 100, name, "fec_percent must be 1..100");
    require(r.ref_invalidation <= 1, name, "ref_invalidation must be 0 or 1");
    require(!r.start_bitrate_bps || validBitrate(r.start_bitrate_bps), name,
            "start_bitrate_bps must be 0 or 128 kbit/s..500 Mbit/s");
    require(r.degradation >= RCS_DEGRADE_FRAME_RATE && r.degradation <= RCS_DEGRADE_BALANCED, name,
            "degradation must be RCS_DEGRADE_FRAME_RATE, SIZE or BALANCED");
    require(r.min_size_percent >= 25 && r.min_size_percent <= 100, name, "min_size_percent must be 25..100");
    require(r.input_boost_ms <= 1000, name, "input_boost_ms must be 0..1000");
    require(r.speed >= RCS_ENCODER_FASTEST && r.speed <= RCS_ENCODER_BEST_QUALITY, name,
            "speed must be RCS_ENCODER_FASTEST, BALANCED or BEST_QUALITY");
    require(r.content <= RCS_CONTENT_MOTION, name, "content must be RCS_CONTENT_AUTO, DETAIL or MOTION");
    require(r.software_threads <= platform::maxSoftwareThreads, name, "software_threads must be 0..64");
    require(!r.keyframe_interval_ms || (r.keyframe_interval_ms >= 100 && r.keyframe_interval_ms <= 60'000), name,
            "keyframe_interval_ms must be 0 or 100..60 000");
    require(r.keyframe_min_interval_ms >= 100 && r.keyframe_min_interval_ms <= 60'000, name,
            "keyframe_min_interval_ms must be 100..60 000");
    require(r.viewer_keyframe_interval_ms >= 100 && r.viewer_keyframe_interval_ms <= 60'000, name,
            "viewer_keyframe_interval_ms must be 100..60 000");
    require(r.loss_keyframes_move <= 60, name, "loss_keyframes_move must be 0..60");
    require(r.move_down_ms >= 200 && r.move_down_ms <= 30'000, name, "move_down_ms must be 200..30 000");
    require(r.move_up_ms >= 1000 && r.move_up_ms <= 120'000, name, "move_up_ms must be 1000..120 000");
    require(r.lower_idle_ms >= 1000 && r.lower_idle_ms <= 600'000, name, "lower_idle_ms must be 1000..600 000");
    require(r.queue_frames >= 1 && r.queue_frames <= 256, name, "queue_frames must be 1..256");
    require(r.max_queue_age_ms <= 2000, name, "max_queue_age_ms must be 0..2000");
    require(r.path_change_percent <= 100, name, "path_change_percent must be 0..100");
    next.videoTuning = r;
}

void parseEncoder(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto e = read<rcs_encoder_config>(extension, name);
    checkCodecList(e.codecs, knownCodec, name);
    checkFormatList(e.formats, name);
    require(e.matrix != RCS_MATRIX_BT2020, name,
            "matrix BT2020 goes with BT.2020's primaries: list an HDR10 format, which uses it; SDR pictures are "
            "written with BT709 or BT601");
    require(e.matrix == RCS_MATRIX_BT709 || e.matrix == RCS_MATRIX_BT601, name, "unknown matrix");
    require(e.range == RCS_RANGE_LIMITED || e.range == RCS_RANGE_FULL, name, "unknown range");
    checkImplementation(e.acceleration, e.backend, name);
    next.encoder = e;
}

void parseHostEncoder(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto e = read<rcs_host_encoder_config>(extension, name);
    checkHostCodec(e, name);
    require(e.encode, name, "encode is required");
    require(e.cpu_images <= 1 && e.low_latency <= 1, name, "cpu_images and low_latency must be 0 or 1");
    require(e.temporal_layers >= 1 && e.temporal_layers <= RCS_SCALABILITY_L1T3, name, "temporal_layers must be 1..3");
    next.hostEncoder = e;
}

void parseCapture(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto c = read<rcs_capture_config>(extension, name);
    require(c.source <= RCS_CAPTURE_ENCODED, name, "unknown source");
    require(c.exclude_current_process <= 1, name, "exclude_current_process must be 0 or 1");
    require(c.secure_desktop <= 1, name, "secure_desktop must be 0 or 1");
    if (c.source == RCS_CAPTURE_REGION) {
        require(c.region_width >= 16 && c.region_height >= 16 && c.region_width <= 65535 && c.region_height <= 65535 &&
                    c.region_x <= 65535 && c.region_y <= 65535,
                name, "region_width and region_height must be 16..65535, region_x and region_y at most 65535");
    }
    require(c.source != RCS_CAPTURE_WINDOW || c.window_id, name,
            "source WINDOW needs window_id (see rcs_list_windows)");
    require(c.source != RCS_CAPTURE_APPLICATION || c.process_id, name,
            "source APPLICATION needs process_id (see rcs_list_windows)");
    require(!c.secure_desktop || c.source == RCS_CAPTURE_DISPLAY || c.source == RCS_CAPTURE_REGION, name,
            "secure_desktop applies to sources DISPLAY and REGION: windows show on the user's desktop only");
    next.capture = c;
}

void parseStreams(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto s = read<rcs_streams_config>(extension, name);
    for (size_t i = 0; i < std::size(s.displays); ++i) {
        require(s.displays[i] || std::all_of(s.displays + i, std::end(s.displays), [](uint32_t d) { return !d; }),
                name, "displays[]: the used entries come first, the unused ones (0) after them");
        require(!s.displays[i] || std::count(std::begin(s.displays), std::end(s.displays), s.displays[i]) == 1, name,
                "displays[] names a display twice");
    }
    require(std::all_of(std::begin(s.weights), std::end(s.weights), [](uint32_t w) { return w <= 1000; }), name,
            "weights[] must be 0..1000 each");
    next.streams = s;
}

void parseViewers(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto v = read<rcs_viewers_config>(extension, name);
    require(v.max_viewers <= RCS_MAX_VIEWERS, name, "max_viewers must be 0 (the default) or 1..256");
    require(v.bitrate_from <= RCS_BITRATE_FROM_MAIN, name, "unknown bitrate_from");
    require(v.quorum_percent >= 1 && v.quorum_percent <= 100, name, "quorum_percent must be 1..100");
    require(!v.admission_timeout_ms || (v.admission_timeout_ms >= 1000 && v.admission_timeout_ms <= 3600000), name,
            "admission_timeout_ms must be 0 (no limit) or 1000..3600000");
    next.viewers = v;
}

void parseEncodings(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto e = read<rcs_encodings_config>(extension, name);
    require(e.mode <= RCS_ENCODINGS_FIXED, name, "unknown mode");
    require(e.count >= 1 && e.count <= RCS_MAX_ENCODINGS, name, "count must be 1..RCS_MAX_ENCODINGS");
    const auto scalability = [](rcs_scalability s) { return s >= RCS_SCALABILITY_L1T1 && s <= RCS_SCALABILITY_L1T3; };
    require(scalability(e.scalability), name, "scalability must be RCS_SCALABILITY_L1T1, L1T2 or L1T3");
    for (const auto& lower : e.lower) {
        require(scalability(lower.scalability), name, "lower[].scalability must be RCS_SCALABILITY_L1T1, L1T2 or L1T3");
        require(lower.frames_per_second <= 240, name, "lower[].frames_per_second must be 0..240");
        require(!lower.max_pixel_rate || lower.max_pixel_rate >= 16 * 16, name,
                "lower[].max_pixel_rate must be 0 or at least 16 x 16 x 1");
        checkCodecList(lower.codecs, knownCodec, name, "lower[].codecs[]", "codec", true); // None: this end's.
    }
    next.encodings = e;
}

void parseSendAudio(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto a = read<rcs_send_audio_config>(extension, name);
    require(a.enabled <= 1, name, "enabled must be 0 or 1");
    checkCodecList(a.codecs, knownAudioCodec, name);
    if (lists(a.codecs, RCS_AUDIO_OPUS)) {
        require(a.bitrate_bps >= 16'000 && a.bitrate_bps <= 510'000, name,
                "bitrate_bps must be 16 000..510 000 for Opus");
    }
    if (lists(a.codecs, RCS_AUDIO_AAC)) {
        require(a.bitrate_bps >= 64'000 && a.bitrate_bps <= 320'000, name,
                "bitrate_bps must be 64 000..320 000 for AAC");
    }
    require(a.source <= RCS_AUDIO_MICROPHONE, name, "unknown source");
    require(a.exclude_current_process <= 1, name, "exclude_current_process must be 0 or 1");
    require(a.voice_processing <= 1, name, "voice_processing must be 0 or 1");
    require(a.sample_buffer_audio_percent <= 400 && a.sample_buffer_microphone_percent <= 400, name,
            "sample_buffer_audio_percent and sample_buffer_microphone_percent must be 0..400");
    next.sendAudio = a;
}

void parseControl(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto c = read<rcs_control_config>(extension, name);
    require(c.allowed <= 1, name, "allowed must be 0 or 1");
    require(c.target <= RCS_INPUT_HOST, name, "unknown target");
    require(!(c.host_input_types & ~protocol::allInputTypes), name,
            "host_input_types holds a bit that is no input type (1u << rcs_input_type)");
    require(!c.host_input_types || c.target == RCS_INPUT_SYSTEM, name,
            "host_input_types applies to target RCS_INPUT_SYSTEM; with RCS_INPUT_HOST the host executes every type");
    next.control = c;
}

void parseViewRequest(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto r = read<rcs_view_request_config>(extension, name);
    require((!r.width && !r.height) || validPictureSize(r.width, r.height), name,
            (std::string("width and height must both be 0, or ") + sizeRange).c_str());
    require(r.frames_per_second <= 240, name, "frames_per_second must be 0..240");
    require(!r.bitrate_bps || validBitrate(r.bitrate_bps), name, "bitrate_bps must be 0 or 128 kbit/s..500 Mbit/s");
    require(!r.codec || knownCodec(r.codec), name, "unknown codec");
    require(!r.format || knownPictureFormat(r.format), name, "unknown picture format");
    require(r.audio_codec <= RCS_AUDIO_PCM, name, "unknown audio_codec");
    require(!r.audio_bitrate_bps || (r.audio_bitrate_bps >= 16'000 && r.audio_bitrate_bps <= 510'000), name,
            "audio_bitrate_bps must be 0 or 16 000..510 000");
    require(r.cursor <= RCS_CURSOR_SEPARATE, name, "unknown cursor mode");
    require(r.streams < (1u << RCS_MAX_STREAMS), name, "streams names a stream from RCS_MAX_STREAMS on");
    next.viewRequest = r;
}

void parseDecoder(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto d = read<rcs_decoder_config>(extension, name);
    checkCodecList(d.codecs, knownCodec, name);
    checkFormatList(d.formats, name);
    checkImplementation(d.acceleration, d.backend, name);
    next.decoder = d;
}

void parseBackground(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto b = read<rcs_background_config>(extension, name);
    require(!(b.pause & ~(RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO)), name, "pause holds a bit that is no RCS_PAUSE_*");
    next.background = b;
}

void parseHostDecoder(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto d = read<rcs_host_decoder_config>(extension, name);
    checkHostCodec(d, name);
    require(d.decode && d.reset, name, "decode and reset are required");
    next.hostDecoder = d;
}

void parseReceiveAudio(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto a = read<rcs_receive_audio_config>(extension, name);
    require(a.enabled <= 1, name, "enabled must be 0 or 1");
    checkCodecList(a.codecs, knownAudioCodec, name);
    require(a.playback <= 1, name, "playback must be 0 or 1");
    next.receiveAudio = a;
}

void parseAudioTuning(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto a = read<rcs_audio_tuning_config>(extension, name);
    require(a.capture_backend <= RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK, name, "capture_backend is unknown");
    require(!a.capture_period_frames || (a.capture_period_frames >= 64 && a.capture_period_frames <= 4800), name,
            "capture_period_frames must be 0 or 64..4800");
    require(a.redundancy <= 1, name, "redundancy must be 0 or 1");
    require(a.max_delay_ms >= 20 && a.max_delay_ms <= 1000, name, "max_delay_ms must be 20..1000");
    require(a.buffer_margin_us <= 100'000, name, "buffer_margin_us must be 0..100 000");
    require(!a.playback_period_frames || (a.playback_period_frames >= 32 && a.playback_period_frames <= 4096), name,
            "playback_period_frames must be 0 or 32..4096");
    require(a.playback_queue_frames <= audioRate, name, "playback_queue_frames must be 0..48 000");
    // The buffer holds at least what one device period (and queue) takes.
    const uint32_t delayFrames = a.max_delay_ms * (audioRate / 1000);
    require(a.playback_period_frames <= delayFrames && a.playback_queue_frames <= delayFrames, name,
            "playback_period_frames and playback_queue_frames must not exceed max_delay_ms");
    next.audioTuning = a;
}

void parseReconnect(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto r = read<rcs_reconnect_config>(extension, name);
    require(r.keep_listening <= 1, name, "keep_listening must be 0 or 1");
    require(r.reconnect <= 1, name, "reconnect must be 0 or 1");
    next.reconnect = r;
}

void parsePaths(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto p = read<rcs_paths_config>(extension, name);
    require(p.migrate <= 1, name, "migrate must be 0 or 1");
    require(!(p.excluded_networks & ~((1u << (RCS_NETWORK_OTHER + 1)) - 1)), name,
            "excluded_networks has bits no rcs_network_kind defines");
    require(p.policy <= RCS_PATH_POLICY_STAY, name, "policy must be RCS_PATH_POLICY_FOLLOW_SYSTEM or STAY");
    require(p.standby <= 1, name, "standby must be 0 or 1");
    require(p.restore_control <= 1, name, "restore_control must be 0 or 1");
    next.paths = p;
}

void parseNetworkTuning(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto n = read<rcs_network_tuning_config>(extension, name);
    require(std::memchr(n.local_interface, 0, sizeof(n.local_interface)) != nullptr, name,
            "local_interface is not NUL-terminated");
    require(n.wifi_low_latency <= 1, name, "wifi_low_latency must be 0 or 1");
    require(n.audio_dscp <= 63, name, "audio_dscp must be 0..63");
    require(n.send_queue_bytes >= (1u << 20) && n.send_queue_bytes <= (1u << 30), name,
            "send_queue_bytes must be 1 MiB..1 GiB");
    require(n.in_flight_bytes >= 4096 && n.in_flight_bytes <= (16u << 20), name,
            "in_flight_bytes must be 4 KiB..16 MiB");
    require(!n.reconnect_window_ms || (n.reconnect_window_ms >= 1000 && n.reconnect_window_ms <= 3'600'000), name,
            "reconnect_window_ms must be 0 or 1 000..3 600 000");
    require(n.reconnect_interval_ms >= 100 && n.reconnect_interval_ms <= 60'000, name,
            "reconnect_interval_ms must be 100..60 000");
    require(n.connect_timeout_ms >= 1000 && n.connect_timeout_ms <= 60'000, name,
            "connect_timeout_ms must be 1 000..60 000");
    require(n.idle_timeout_ms >= 1000 && n.idle_timeout_ms <= 60'000, name, "idle_timeout_ms must be 1 000..60 000");
    require(n.ice_gather_ms >= 200 && n.ice_gather_ms <= 10'000, name, "ice_gather_ms must be 200..10 000");
    require(n.ice_connect_ms >= 1000 && n.ice_connect_ms <= 120'000, name, "ice_connect_ms must be 1 000..120 000");
    require(!(n.ice_addresses & ~uint32_t(RCS_ICE_ADDRESSES_LOOPBACK)), name,
            "ice_addresses holds a bit that is no RCS_ICE_ADDRESSES_*");
    require(n.turn_allocate_ms >= 1000 && n.turn_allocate_ms <= 30'000, name,
            "turn_allocate_ms must be 1 000..30 000");
    require(n.path_stall_ms >= 200 && n.path_stall_ms <= 10'000, name, "path_stall_ms must be 200..10 000");
    require(n.path_hold_ms >= 1000 && n.path_hold_ms <= 3'600'000, name, "path_hold_ms must be 1 000..3 600 000");
    require(n.standby_probe_ms >= 1000 && n.standby_probe_ms <= 60'000, name, "standby_probe_ms must be 1 000..60 000");
    next.networkTuning = n;
}

// A module's settings, which it checks; replaced for a module there is.
void parseModule(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto c = read<rcs_module_config>(extension, name);
    const std::string id = c.module; // applyExtensions found it to be a string.
    const modules::Definition* definition = modules::find(id);
    if (!definition) {
        invalid(std::string(name) + ": module \"" + id + "\" is not registered (rcs_register_module)");
    }
    if (!(definition->table.roles & (1u << next.role))) {
        invalid(std::string(name) + ": module \"" + id + "\" takes no part in " +
                (next.role == RCS_ROLE_SHARE ? "sharing" : "viewing") + " sessions");
    }
    require(c.size <= RCS_MODULE_SETTINGS_MAX && (c.settings || !c.size), name,
            "settings: NULL with size 0, or at most RCS_MODULE_SETTINGS_MAX bytes");
    if (const auto check = definition->table.check) {
        char reason[256] = {};
        const rcs_result result = check(next.role, c.settings, c.size, reason, sizeof(reason),
                                        definition->table.user_data);
        if (result != RCS_OK) {
            reason[sizeof(reason) - 1] = '\0';
            fail(result == RCS_ERROR_UNSUPPORTED ? RCS_ERROR_UNSUPPORTED : RCS_ERROR_INVALID_ARGUMENT,
                 std::string(name) + " \"" + id + "\": " + (reason[0] ? reason : "the module refused its settings"));
        }
    }
    const auto* bytes = static_cast<const uint8_t*>(c.settings);
    crypto::Bytes settings(bytes, bytes + c.size);
    for (auto& use : next.modules) {
        if (use.module == definition) {
            use.settings = std::move(settings);
            return;
        }
    }
    next.modules.push_back({definition, std::move(settings)});
}

void parseLog(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto l = read<rcs_log_config>(extension, name);
    require(l.max_level <= RCS_LOG_DEBUG, name, "max_level must be 0 or RCS_LOG_ERROR..RCS_LOG_DEBUG");
    next.log = l;
}

void parseDeviceIdentity(const rcs_extension& extension, const char* name, SessionConfig& next) {
    auto identity = read<rcs_device_identity_config>(extension, name);
    require(identity.has_device_key <= 1, name, "has_device_key must be 0 or 1");
    if (identity.has_device_key) {
        try {
            crypto::DeviceKey key(identity.device_key);
        } catch (const Failure&) {
            invalid(std::string(name) + ": device_key is not a P-256 key pair (see rcs_generate_device_key)");
        }
    }
    require(identity.trusted_key_count <= RCS_MAX_TRUSTED_KEYS, name, "trusted_key_count must be 0..1024");
    require(!identity.trusted_key_count || identity.trusted_keys, name, "trusted_keys is NULL");
    std::vector<crypto::PublicKey> trusted(identity.trusted_key_count);
    for (size_t i = 0; i < trusted.size(); ++i) {
        std::copy_n(identity.trusted_keys + i * RCS_PUBLIC_KEY_SIZE, RCS_PUBLIC_KEY_SIZE, trusted[i].begin());
        if (!crypto::validPublicKey(trusted[i])) {
            invalid(std::string(name) + ": trusted_keys[" + std::to_string(i) + "] is not a P-256 public key");
        }
    }
    identity.trusted_keys = nullptr;
    next.deviceIdentity = identity;
    next.trustedKeys = std::move(trusted);
}

void parseAgentLink(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto a = read<rcs_agent_link_config>(extension, name);
    require(std::memchr(a.endpoint, 0, sizeof(a.endpoint)) != nullptr, name, "endpoint is not NUL-terminated");
    require(a.access <= RCS_AGENT_ACCESS_ANY_USER, name, "unknown access");
    if (a.endpoint[0]) {
        require(std::any_of(std::begin(a.key), std::end(a.key), [](uint8_t b) { return b != 0; }), name,
                "key is empty; see rcs_generate_connection_key");
        const auto problem = transport::endpointProblem(a.endpoint);
        require(problem.empty(), name, ("endpoint: " + problem).c_str());
    }
    next.agentLink = a;
}

// A name TLS sends to a server (SNI) and checks its certificate against: a DNS
// name, never an address (SNI carries none) or a port.
bool dnsName(const std::string& name) {
    return !transport::Endpoint::tryParse(name, 0) && name.front() != '.' && name.back() != '.' &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '_' || c == '.';
           });
}

void parseIceServers(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto t = read<rcs_ice_servers_config>(extension, name);
    require(t.turn_transport <= RCS_TURN_TLS, name, "unknown turn_transport");
    for (const auto& server : t.stun_servers) {
        require(std::memchr(server, 0, sizeof(server)) != nullptr, name, "a STUN server is not NUL-terminated");
    }
    for (const char* field : {t.turn_server, t.turn_username, t.turn_password, t.turn_tls_server_name}) {
        require(std::memchr(field, 0, 128) != nullptr, name, "a TURN field is not NUL-terminated");
    }
    const bool pinned = std::any_of(std::begin(t.turn_certificate_sha256), std::end(t.turn_certificate_sha256),
                                    [](uint8_t b) { return b != 0; });
    if (t.turn_transport != RCS_TURN_TLS) {
        require(!pinned && !t.turn_tls_server_name[0], name,
                "turn_certificate_sha256 and turn_tls_server_name apply to turn_transport RCS_TURN_TLS only (a "
                "certificate nothing checks would leave the server unverified)");
    } else if (t.turn_tls_server_name[0]) {
        require(dnsName(t.turn_tls_server_name), name,
                "turn_tls_server_name must be a DNS name (letters, digits, '-', '_' and '.'), without a port and "
                "never an address: a server known by its address is checked with turn_certificate_sha256");
    } else if (!pinned) {
        const auto server = transport::ice::splitHostPort(t.turn_server, transport::turnTlsPort);
        require(!server || !transport::Endpoint::tryParse(server->host, 0), name,
                "turn_server is an address, so a TLS certificate has no name to be checked against: give "
                "turn_tls_server_name, or pin the certificate with turn_certificate_sha256");
        require(!server || dnsName(server->host), name,
                "turn_server's host is the name a TLS certificate is checked against, so it must be a DNS name "
                "(letters, digits, '-', '_' and '.'): give turn_tls_server_name, or pin the certificate with "
                "turn_certificate_sha256");
    }
    next.iceServers = t;
}

void parseEncodedOutput(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto e = read<rcs_encoded_output_config>(extension, name);
    require(e.decode <= 1, name, "decode must be 0 or 1");
    require(e.encodings < (1u << RCS_MAX_ENCODINGS), name, "encodings must be bits below 1u << RCS_MAX_ENCODINGS");
    require(e.on_video || e.on_audio, name, "on_video or on_audio is required");
    next.encodedOutput = e;
}

void parseRateDecider(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto r = read<rcs_rate_decider_config>(extension, name);
    require(r.on_rate_report, name, "on_rate_report is required");
    next.rateDecider = r;
}

void parseHostTransport(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto t = read<rcs_host_transport_config>(extension, name);
    require(t.host_channel <= RCS_HOST_CHANNEL_STREAM, name, "unknown host_channel");
    require(!t.host_max_datagram ||
                (t.host_max_datagram >= RCS_HOST_MIN_DATAGRAM && t.host_max_datagram <= RCS_HOST_MAX_DATAGRAM),
            name, "host_max_datagram must be 0 or RCS_HOST_MIN_DATAGRAM..RCS_HOST_MAX_DATAGRAM (1136..65535)");
    require(t.on_transport_send, name, "on_transport_send is required");
    next.hostTransport = t;
}

void parseForwarder(const rcs_extension& extension, const char* name, SessionConfig& next) {
    const auto f = read<rcs_forwarder_config>(extension, name);
    require(f.token[0] && std::memchr(f.token, 0, sizeof(f.token)), name,
            "token must be non-empty and NUL-terminated (at most RCS_FORWARDER_TOKEN_SIZE - 1 bytes)");
    const auto set = [](const uint8_t (&bytes)[32]) {
        return std::any_of(std::begin(bytes), std::end(bytes), [](uint8_t b) { return b != 0; });
    };
    require(set(f.forwarder_key_sha256), name,
            "forwarder_key_sha256 is required: the SHA-256 of the forwarder's public key (rcsfwd_public_key_sha256)");
    require(f.link <= RCS_FORWARDER_HOST, name, "unknown link");
    require(std::memchr(f.websocket_url, 0, sizeof(f.websocket_url)), name, "websocket_url is not NUL-terminated");
    if (f.link == RCS_FORWARDER_WEBSOCKET) {
        transport::websocket::Url url;
        try {
            url = transport::websocket::parseUrl(f.websocket_url);
        } catch (const Failure& failure) {
            invalid(std::string(name) + ": websocket_url " + failure.what());
        }
        require(url.tls || !set(f.tls_certificate_sha256), name,
                "tls_certificate_sha256 is for a wss:// websocket_url only (nothing would check it)");
        require(!url.tls || set(f.tls_certificate_sha256) || !transport::Endpoint::tryParse(url.host, 0), name,
                "a wss:// websocket_url whose host is an address needs tls_certificate_sha256 (no name to check "
                "the certificate for)");
    } else {
        require(!f.websocket_url[0] && !set(f.tls_certificate_sha256), name,
                "websocket_url and tls_certificate_sha256 are for link RCS_FORWARDER_WEBSOCKET only");
    }
    require(!f.key_rotation_ms || (f.key_rotation_ms >= 1000 && f.key_rotation_ms <= 86'400'000), name,
            "key_rotation_ms must be 0 or 1 000..86 400 000");
    require(f.from_admission <= 1, name, "from_admission must be 0 or 1");
    next.forwarder = f;
}

void parseGpuDevice(const rcs_extension& extension, const char* name, SessionConfig& next) {
    next.gpuDevice = read<rcs_gpu_device_config>(extension, name);
}

// Copies one extension's settings (the parsed ones, in SessionConfig).
template <auto... Members>
void copyMembers(SessionConfig& to, const SessionConfig& from) noexcept {
    ((to.*Members = from.*Members), ...);
}

// One row per extension type: everything that handles an extension follows
// this table, so a new type cannot be left out of the runtime copy or checks.
struct Kind {
    rcs_extension_type type;
    const char* name;
    rcs_role role; // 0: both.
    void (*parse)(const rcs_extension&, const char*, SessionConfig&);
    void (*copy)(SessionConfig&, const SessionConfig&) noexcept;
    // The settings in a SessionConfig, as the host's structure; null for RCS_EXT_MODULE.
    const rcs_extension* (*settings)(const SessionConfig&) noexcept;
    bool preset; // A preset gives its settings.
};

template <auto Member>
const rcs_extension* settingsOf(const SessionConfig& config) noexcept {
    return &(config.*Member).header;
}

template <auto Member, auto... More>
constexpr Kind kind(rcs_extension_type type, const char* name, rcs_role role,
                    void (*parse)(const rcs_extension&, const char*, SessionConfig&), bool preset = false) {
    return {type, name, role, parse, copyMembers<Member, More...>, settingsOf<Member>, preset};
}

constexpr Kind kinds[] = {
    kind<&SessionConfig::shareVideo>(RCS_EXT_SHARE_VIDEO, "RCS_EXT_SHARE_VIDEO", RCS_ROLE_SHARE, parseShareVideo, true),
    kind<&SessionConfig::encoder>(RCS_EXT_ENCODER, "RCS_EXT_ENCODER", RCS_ROLE_SHARE, parseEncoder, true),
    kind<&SessionConfig::capture>(RCS_EXT_CAPTURE, "RCS_EXT_CAPTURE", RCS_ROLE_SHARE, parseCapture),
    kind<&SessionConfig::streams>(RCS_EXT_STREAMS, "RCS_EXT_STREAMS", RCS_ROLE_SHARE, parseStreams),
    kind<&SessionConfig::control>(RCS_EXT_CONTROL, "RCS_EXT_CONTROL", RCS_ROLE_SHARE, parseControl),
    kind<&SessionConfig::viewers>(RCS_EXT_VIEWERS, "RCS_EXT_VIEWERS", RCS_ROLE_SHARE, parseViewers),
    kind<&SessionConfig::encodings>(RCS_EXT_ENCODINGS, "RCS_EXT_ENCODINGS", RCS_ROLE_SHARE, parseEncodings),
    kind<&SessionConfig::viewRequest>(RCS_EXT_VIEW_REQUEST, "RCS_EXT_VIEW_REQUEST", RCS_ROLE_VIEW, parseViewRequest,
                                      true),
    kind<&SessionConfig::decoder>(RCS_EXT_DECODER, "RCS_EXT_DECODER", RCS_ROLE_VIEW, parseDecoder),
    kind<&SessionConfig::sendAudio>(RCS_EXT_SEND_AUDIO, "RCS_EXT_SEND_AUDIO", 0, parseSendAudio, true),
    kind<&SessionConfig::receiveAudio>(RCS_EXT_RECEIVE_AUDIO, "RCS_EXT_RECEIVE_AUDIO", 0, parseReceiveAudio),
    kind<&SessionConfig::iceServers>(RCS_EXT_ICE_SERVERS, "RCS_EXT_ICE_SERVERS", 0, parseIceServers),
    kind<&SessionConfig::reconnect>(RCS_EXT_RECONNECT, "RCS_EXT_RECONNECT", 0, parseReconnect),
    kind<&SessionConfig::background>(RCS_EXT_BACKGROUND, "RCS_EXT_BACKGROUND", 0, parseBackground),
    kind<&SessionConfig::deviceIdentity, &SessionConfig::trustedKeys>(
        RCS_EXT_DEVICE_IDENTITY, "RCS_EXT_DEVICE_IDENTITY", 0, parseDeviceIdentity),
    kind<&SessionConfig::log>(RCS_EXT_LOG, "RCS_EXT_LOG", 0, parseLog),
    {RCS_EXT_MODULE, "RCS_EXT_MODULE", 0, parseModule, copyMembers<&SessionConfig::modules>, nullptr, false},
    kind<&SessionConfig::paths>(RCS_EXT_PATHS, "RCS_EXT_PATHS", 0, parsePaths),
    kind<&SessionConfig::forwarder>(RCS_EXT_FORWARDER, "RCS_EXT_FORWARDER", 0, parseForwarder),
    kind<&SessionConfig::videoTuning>(RCS_EXT_VIDEO_TUNING, "RCS_EXT_VIDEO_TUNING", 0, parseVideoTuning, true),
    kind<&SessionConfig::audioTuning>(RCS_EXT_AUDIO_TUNING, "RCS_EXT_AUDIO_TUNING", 0, parseAudioTuning, true),
    kind<&SessionConfig::networkTuning>(RCS_EXT_NETWORK_TUNING, "RCS_EXT_NETWORK_TUNING", 0, parseNetworkTuning, true),
    kind<&SessionConfig::hostEncoder>(RCS_EXT_HOST_ENCODER, "RCS_EXT_HOST_ENCODER", RCS_ROLE_SHARE, parseHostEncoder),
    kind<&SessionConfig::hostDecoder>(RCS_EXT_HOST_DECODER, "RCS_EXT_HOST_DECODER", RCS_ROLE_VIEW, parseHostDecoder),
    kind<&SessionConfig::encodedOutput>(RCS_EXT_ENCODED_OUTPUT, "RCS_EXT_ENCODED_OUTPUT", 0, parseEncodedOutput),
    kind<&SessionConfig::rateDecider>(RCS_EXT_RATE_DECIDER, "RCS_EXT_RATE_DECIDER", RCS_ROLE_SHARE, parseRateDecider),
    kind<&SessionConfig::hostTransport>(RCS_EXT_HOST_TRANSPORT, "RCS_EXT_HOST_TRANSPORT", 0, parseHostTransport),
    kind<&SessionConfig::gpuDevice>(RCS_EXT_GPU_DEVICE, "RCS_EXT_GPU_DEVICE", RCS_ROLE_VIEW, parseGpuDevice),
    kind<&SessionConfig::agentLink>(RCS_EXT_AGENT_LINK, "RCS_EXT_AGENT_LINK", RCS_ROLE_SHARE, parseAgentLink),
};

// What each preset sets; every other field keeps its *_INIT value (BALANCED's).
// Proposals until measured (PERFORMANCE.md).
void presetSettings(rcs_preset preset, SessionConfig& c) noexcept {
    c.shareVideo = RCS_SHARE_VIDEO_CONFIG_INIT;
    c.encoder = RCS_ENCODER_CONFIG_INIT;
    c.sendAudio = RCS_SEND_AUDIO_CONFIG_INIT;
    c.viewRequest = RCS_VIEW_REQUEST_CONFIG_INIT;
    c.videoTuning = RCS_VIDEO_TUNING_CONFIG_INIT;
    c.audioTuning = RCS_AUDIO_TUNING_CONFIG_INIT;
    c.networkTuning = RCS_NETWORK_TUNING_CONFIG_INIT;
    switch (preset) {
    case RCS_PRESET_LOW_LATENCY:
        c.shareVideo.bitrate_bps = 40'000'000;
        c.videoTuning.start_bitrate_bps = 10'000'000;
        c.videoTuning.frame_spread_us = 500;
        c.videoTuning.input_boost_ms = 100;
        c.videoTuning.max_queue_age_ms = 40;
        c.audioTuning.max_delay_ms = 60;
        c.audioTuning.buffer_margin_us = 1000;
        break;
    case RCS_PRESET_LOW_BANDWIDTH:
        c.shareVideo.width = c.viewRequest.width = 1280;
        c.shareVideo.height = c.viewRequest.height = 720;
        c.shareVideo.frames_per_second = c.viewRequest.frames_per_second = 30;
        c.shareVideo.bitrate_bps = c.viewRequest.bitrate_bps = 4'000'000;
        c.shareVideo.min_bitrate_bps = 300'000;
        c.sendAudio.bitrate_bps = c.viewRequest.audio_bitrate_bps = 96'000;
        c.videoTuning.overuse_queue_ms = 20;
        c.videoTuning.severe_queue_ms = 40;
        c.videoTuning.clear_queue_ms = 8;
        c.videoTuning.overuse_loss_permille = 50;
        c.videoTuning.clear_loss_permille = 10;
        c.videoTuning.decrease_percent = 80;
        c.videoTuning.start_bitrate_bps = 1'000'000;
        c.videoTuning.speed = RCS_ENCODER_BALANCED;
        c.videoTuning.keyframe_min_interval_ms = 1000;
        c.videoTuning.max_queue_age_ms = 150;
        c.audioTuning.max_delay_ms = 250;
        c.audioTuning.redundancy = 0; // The bitrate goes to the picture.
        c.audioTuning.buffer_margin_us = 10'000;
        c.networkTuning.idle_timeout_ms = 10'000;
        break;
    case RCS_PRESET_QUALITY:
        c.shareVideo.bitrate_bps = 50'000'000;
        c.encoder.formats[0] = RCS_PICTURE_444_8;
        c.encoder.formats[1] = RCS_PICTURE_420_8;
        c.videoTuning.speed = RCS_ENCODER_BALANCED;
        c.videoTuning.keyframe_min_interval_ms = 1000;
        break;
    default:
        break;
    }
}

const Kind* kindOf(uint32_t type) noexcept {
    const auto found = std::find_if(std::begin(kinds), std::end(kinds), [&](const Kind& k) { return k.type == type; });
    return found == std::end(kinds) ? nullptr : found;
}

// Settings that depend on another extension's.
void checkAcross(const SessionConfig& c) {
    if (c.viewRequest.codec) {
        require(lists(c.decoder.codecs, c.viewRequest.codec), "RCS_EXT_VIEW_REQUEST",
                "codec must be one of RCS_EXT_DECODER's codecs[]");
    }
    if (c.viewRequest.format) {
        require(lists(c.decoder.formats, c.viewRequest.format), "RCS_EXT_VIEW_REQUEST",
                "format must be one of RCS_EXT_DECODER's formats[]");
    }
    if (c.viewRequest.audio_codec) {
        require(lists(c.receiveAudio.codecs, c.viewRequest.audio_codec), "RCS_EXT_VIEW_REQUEST",
                "audio_codec must be one of RCS_EXT_RECEIVE_AUDIO's codecs[]");
    }
    if (c.identity.secret == RCS_SECRET_NONE && (!c.deviceIdentity.has_device_key || c.trustedKeys.empty())) {
        invalid("rcs_session_config: identity.secret NONE needs RCS_EXT_DEVICE_IDENTITY with device_key and "
                "trusted_keys: nothing else would authenticate the ends");
    }
    for (const auto& lower : c.encodings.lower) {
        for (const rcs_codec codec : lower.codecs) {
            require(!codec || lists(c.encoder.codecs, codec), "RCS_EXT_ENCODINGS",
                    "lower[].codecs[] must be among RCS_EXT_ENCODER's codecs[]");
        }
    }
    if (c.streams.displays[0]) {
        require(c.capture.source == RCS_CAPTURE_DISPLAY, "RCS_EXT_STREAMS",
                "more streams need stream 0 to share a display (RCS_EXT_CAPTURE source DISPLAY)");
        require(!c.capture.display_id || !lists(c.streams.displays, c.capture.display_id), "RCS_EXT_STREAMS",
                "displays[] names the display stream 0 shares (RCS_EXT_CAPTURE display_id)");
    }
    if (hasRemoteAgent(c) && c.capture.source == RCS_CAPTURE_HOST) {
        invalid("RCS_EXT_AGENT_LINK: capture source HOST needs the encoder in this process; the host's own pictures "
                "cannot go to an agent");
    }
    if (hasRemoteAgent(c) && c.capture.source == RCS_CAPTURE_ENCODED) {
        invalid("RCS_EXT_AGENT_LINK: capture source ENCODED sends the host's bitstream from this process; it "
                "cannot go to an agent");
    }
    const bool hostEncoder = c.hostEncoder.codecs[0] != 0;
    if (hostEncoder && c.capture.source == RCS_CAPTURE_ENCODED) {
        invalid("RCS_EXT_HOST_ENCODER: with capture source ENCODED the host pushes its bitstream; there is no "
                "encoder to replace");
    }
    if (hostEncoder && hasRemoteAgent(c)) {
        invalid("RCS_EXT_HOST_ENCODER: the agent of another process encodes; give the host's encoder to it "
                "(rcs_agent_config's extensions)");
    }
    // A host codec needs the host's own codec at this end (an agent of another
    // process says what its encoder handles when it attaches).
    if (c.role == RCS_ROLE_SHARE && c.capture.source != RCS_CAPTURE_ENCODED && !hasRemoteAgent(c)) {
        for (const rcs_codec codec : c.encoder.codecs) {
            if (hostCodec(codec) && !lists(c.hostEncoder.codecs, codec)) {
                invalid(std::string("RCS_EXT_ENCODER: codecs[] lists ") + codecName(codec) +
                        ", which RCS_EXT_HOST_ENCODER does not handle (or use capture source ENCODED)");
            }
        }
    }
    // decode 0: nothing decodes, so every codec is the host's to store.
    if (c.role == RCS_ROLE_VIEW && !c.encodedOutput.decode) {
        if (!c.encodedOutput.on_video) {
            invalid("RCS_EXT_ENCODED_OUTPUT: decode 0 needs on_video: nothing else would get the picture");
        }
        if (c.hostDecoder.codecs[0]) {
            invalid("RCS_EXT_HOST_DECODER: RCS_EXT_ENCODED_OUTPUT's decode 0 decodes nothing; leave it out");
        }
    } else if (c.role == RCS_ROLE_VIEW) {
        for (const rcs_codec codec : c.decoder.codecs) {
            if (hostCodec(codec) && !lists(c.hostDecoder.codecs, codec)) {
                invalid(std::string("RCS_EXT_DECODER: codecs[] lists ") + codecName(codec) +
                        ", which RCS_EXT_HOST_DECODER does not handle");
            }
        }
    }
    const bool forwarder = c.connect.transport == RCS_TRANSPORT_FORWARDER;
    const bool hop = forwarder && c.forwarder.link == RCS_FORWARDER_HOST; // The hop over the host's channel.
    const bool hostTransport = c.connect.transport == RCS_TRANSPORT_HOST || hop;
    if (hostTransport != (c.hostTransport.on_transport_send != nullptr)) {
        invalid(hop             ? "rcs_session_config: RCS_EXT_FORWARDER's link HOST needs RCS_EXT_HOST_TRANSPORT "
                                  "(rcsdk/host_transport.h) to hand the hop's datagrams to the host's channel"
                : hostTransport ? "rcs_session_config: connect.transport HOST needs RCS_EXT_HOST_TRANSPORT "
                                  "(rcsdk/host_transport.h) to hand the session's datagrams to the host's channel"
                                : "RCS_EXT_HOST_TRANSPORT: applies to connect.transport RCS_TRANSPORT_HOST, and to "
                                  "FORWARDER with link RCS_FORWARDER_HOST, only");
    }
    if (hop && c.hostTransport.host_max_datagram && c.hostTransport.host_max_datagram < protocol::udpDatagram) {
        invalid("RCS_EXT_HOST_TRANSPORT: host_max_datagram must be 0 or at least 1200 for a forwarder's hop (its "
                "datagrams, and the forwarder's, are up to 1200 bytes)");
    }
    if (forwarder != (c.forwarder.token[0] != 0)) {
        invalid(forwarder ? "rcs_session_config: connect.transport FORWARDER needs RCS_EXT_FORWARDER: which forwarder "
                            "(forwarder_key_sha256), and the token for its room"
                          : "RCS_EXT_FORWARDER: applies to connect.transport RCS_TRANSPORT_FORWARDER only");
    }
    const bool addressed = c.forwarder.link == RCS_FORWARDER_UDP || c.forwarder.link == RCS_FORWARDER_TCP;
    if (forwarder && !addressed && (c.connect.address[0] || c.connect.port)) {
        invalid(hop ? "rcs_session_config: with RCS_EXT_FORWARDER's link HOST, the host's channel leads to the "
                      "forwarder: connect.address stays empty and connect.port 0"
                    : "rcs_session_config: with RCS_EXT_FORWARDER's link WEBSOCKET, websocket_url says where the "
                      "forwarder is: connect.address stays empty and connect.port 0");
    }
    if (forwarder && addressed && (!c.connect.address[0] || !c.connect.port)) {
        invalid("rcs_session_config: connect.address and connect.port (1..65535) are the forwarder's");
    }
}

// The core configuration's connect (fixed for the session's life).
void checkConnect(const rcs_connect& n) {
    if (n.transport > RCS_TRANSPORT_FORWARDER) {
        invalid("rcs_session_config: unknown connect.transport");
    }
    if (!std::memchr(n.address, 0, sizeof(n.address))) {
        invalid("rcs_session_config: connect.address is not NUL-terminated");
    }
    // The host's channel has no address or port; a forwarder's are checked
    // with its extension (a name, or a WebSocket's URL instead).
    if (n.transport == RCS_TRANSPORT_HOST || n.transport == RCS_TRANSPORT_FORWARDER) {
        if (n.port > 65535) {
            invalid("rcs_session_config: connect.port must be 0..65535");
        }
        return;
    }
    const bool signalled = usesIce(n.transport);
    if (signalled ? n.port > 65535 : n.port < 1 || n.port > 65535) {
        invalid("rcs_session_config: connect.port must be 1..65535 (P2P and RELAY: 0..65535)");
    }
    // UDP and TCP: the address names the peer (view) or what to listen on
    // (share); ICE finds its own.
    if (!signalled && !transport::Endpoint::tryParse(n.address, uint16_t(n.port))) {
        invalid(std::string("rcs_session_config: connect.address \"") + n.address +
                "\" is not a numeric IPv4 or IPv6 address");
    }
}

// The row of `extension`'s type, of this library's size.
const Kind& kindOfSized(const rcs_extension& extension) {
    const Kind* kind = kindOf(extension.type);
    if (!kind) {
        invalid("Unknown extension type " + std::to_string(extension.type));
    }
    if (kind->settings) {
        require(extension.struct_size == kind->settings(SessionConfig{})->struct_size, kind->name,
                "struct_size is not this library's; initialise the extension with its *_INIT macro from the "
                "headers of this library (" RCSDK_VERSION_STRING ")");
    }
    return *kind;
}

// Copies a kind's settings out of `config` into the host's structure, keeping its `next`.
void writeSettings(const Kind& kind, const SessionConfig& config, rcs_extension& extension) {
    const rcs_extension* next = extension.next;
    std::memcpy(&extension, kind.settings(config), extension.struct_size);
    extension.next = next;
}

// RCSDK_VERSION as "major.minor.patch".
std::string versionText(uint32_t version) {
    return std::to_string(version >> 16) + "." + std::to_string((version >> 8) & 0xffu) + "." +
           std::to_string(version & 0xffu);
}

} // namespace

std::string extensionList(uint32_t types) {
    std::string list;
    uint32_t remaining = types;
    for (const Kind& kind : kinds) {
        if (!(types & (1u << kind.type))) {
            continue;
        }
        remaining &= ~(1u << kind.type);
        list += (list.empty() ? "" : remaining ? ", " : " and ") + std::string(kind.name);
    }
    return list;
}

uint32_t extensionsOf(rcs_role role) noexcept {
    uint32_t types = 0;
    for (const Kind& kind : kinds) {
        types |= !kind.role || kind.role == role ? 1u << kind.type : 0;
    }
    return types;
}

void copyRuntimeSettings(SessionConfig& to, const SessionConfig& from) noexcept {
    to.preset = from.preset;
    to.chained = from.chained;
    to.identity = from.identity;
    for (const Kind& kind : kinds) {
        if (!(fixedExtensions & (1u << kind.type))) {
            kind.copy(to, from);
        }
    }
}

void copyExtensionSettings(SessionConfig& to, const SessionConfig& from) noexcept {
    to.preset = from.preset;
    to.chained = from.chained;
    to.identity = from.identity;
    for (const Kind& kind : kinds) {
        kind.copy(to, from);
    }
}

uint32_t systemInputTypes(const rcs_control_config& control) noexcept {
    if (control.target != RCS_INPUT_SYSTEM) {
        return 0;
    }
    return protocol::allInputTypes & ~(1u << RCS_INPUT_RELEASE_ALL) & ~control.host_input_types;
}

RateLimits rateLimits(const SessionConfig& config, uint32_t bitrate) noexcept {
    const auto& v = config.shareVideo;
    RateLimits limits;
    limits.adaptive = v.rate_mode != RCS_RATE_FIXED;
    limits.maximum = bitrate;
    const auto& r = config.videoTuning;
    limits.rules.overuseQueueUs = int64_t(r.overuse_queue_ms) * 1000;
    limits.rules.severeQueueUs = int64_t(r.severe_queue_ms) * 1000;
    limits.rules.clearQueueUs = int64_t(r.clear_queue_ms) * 1000;
    limits.rules.overusePersist = std::chrono::milliseconds(r.overuse_persist_ms);
    limits.rules.overuseLoss = r.overuse_loss_permille;
    limits.rules.clearLoss = r.clear_loss_permille;
    limits.rules.decrease = r.decrease_percent / 100.0;
    limits.rules.decreaseInterval = std::chrono::milliseconds(r.decrease_interval_ms);
    limits.rules.probeIncrease = r.probe_increase_percent / 100.0;
    limits.rules.increase = r.increase_percent / 100.0;
    limits.rules.recoveryIncrease = r.recovery_increase_percent / 100.0;
    limits.rules.pathChange = r.path_change_percent / 100.0;
    limits.rules.frameSpread = std::chrono::microseconds(r.frame_spread_us);
    limits.rules.maxPacingBps = r.max_pacing_bps;
    limits.rules.pathPacing = r.path_pacing_percent;
    limits.rules.pathBurst = std::chrono::microseconds(r.path_burst_us);
    limits.fec = {r.fec_mode, r.fec_percent};
    if (!limits.adaptive) {
        limits.minimum = limits.start = bitrate;
        return limits;
    }
    limits.minimum = std::min(minimumBitrate(v), bitrate);
    // Unless told otherwise, start where most paths can carry it and probe up.
    limits.start = std::clamp(r.start_bitrate_bps ? r.start_bitrate_bps : 4'000'000u, limits.minimum, bitrate);
    limits.rules.pathStartBps = limits.start;
    return limits;
}

uint32_t applyExtensions(SessionConfig& config, const rcs_extension* chain) {
    SessionConfig next = config;
    // Each type at most once, and each module, so a circular chain fails as
    // "chained twice".
    uint32_t seen = 0;
    std::vector<std::string> modules;
    for (const rcs_extension* extension = chain; extension; extension = extension->next) {
        const Kind* kind = kindOf(extension->type);
        if (!kind) {
            invalid("Unknown extension type " + std::to_string(extension->type));
        }
        if (kind->type == RCS_EXT_MODULE) {
            const auto c = read<rcs_module_config>(*extension, kind->name);
            require(c.module && std::memchr(c.module, 0, RCS_MODULE_ID_MAX), kind->name,
                    "module must be a module's id (rcsdk/module.h)");
            require(std::find(modules.begin(), modules.end(), c.module) == modules.end(), kind->name,
                    std::string("module \"") + c.module + "\" chained twice");
            modules.emplace_back(c.module);
            require(modules.size() <= RCS_MODULES_MAX, kind->name, "at most RCS_MODULES_MAX modules");
        } else {
            require(!(seen & (1u << kind->type)), kind->name, "chained twice");
        }
        seen |= 1u << kind->type;
        if (kind->role && kind->role != config.role) {
            invalid(std::string(kind->name) + ": applies to " +
                    (kind->role == RCS_ROLE_SHARE ? "sharing" : "viewing") + " sessions only");
        }
        kind->parse(*extension, kind->name, next);
    }
    next.chained |= seen;
    checkAcross(next);
    config = next;
    return seen;
}

rcs_identity parseIdentity(const rcs_identity& identity) {
    constexpr const char* name = "identity";
    require(identity.secret <= RCS_SECRET_NONE, name, "unknown secret");
    require(identity.secret != RCS_SECRET_KEY ||
                std::any_of(std::begin(identity.key), std::end(identity.key), [](uint8_t b) { return b != 0; }),
            name, "key is empty; see rcs_generate_connection_key (or use another secret)");
    require(std::memchr(identity.password, 0, sizeof(identity.password)) != nullptr, name,
            "password is not NUL-terminated");
    require(identity.secret != RCS_SECRET_PASSWORD || identity.password[0], name, "secret PASSWORD needs a password");
    if (identity.secret == RCS_SECRET_PIN) {
        const auto characters = utf8Characters(identity.password);
        require(characters && *characters >= 4 && *characters <= 32, name,
                "secret PIN needs a PIN of 4..32 UTF-8 characters in password (see rcs_generate_pin)");
    }
    require(identity.pin_attempts >= 1 && identity.pin_attempts <= 1000, name, "pin_attempts must be 1..1000");
    return identity;
}

void applyIdentity(SessionConfig& config, const rcs_identity& identity) {
    SessionConfig next = config;
    next.identity = parseIdentity(identity);
    checkAcross(next);
    config = next;
}

uint32_t presetExtensions() noexcept {
    uint32_t types = 0;
    for (const Kind& kind : kinds) {
        types |= kind.preset ? 1u << kind.type : 0;
    }
    return types;
}

// Sound without the extensions: a sharer sends and receives none, a viewer
// receives and sends none.
void soundDefaults(SessionConfig& config) noexcept {
    if (!(config.chained & (1u << RCS_EXT_SEND_AUDIO)) && config.role == RCS_ROLE_VIEW) {
        config.sendAudio.enabled = 0;
    }
    if (!(config.chained & (1u << RCS_EXT_RECEIVE_AUDIO)) && config.role == RCS_ROLE_SHARE) {
        config.receiveAudio.enabled = 0;
    }
}

uint32_t applyPreset(SessionConfig& config, rcs_preset preset) {
    if (preset > RCS_PRESET_QUALITY) {
        invalid("Unknown preset " + std::to_string(preset));
    }
    SessionConfig values;
    presetSettings(preset, values);
    SessionConfig next = config;
    next.preset = preset;
    const uint32_t types = presetExtensions() & ~config.chained & extensionsOf(config.role);
    for (const Kind& kind : kinds) {
        if (types & (1u << kind.type)) {
            kind.copy(next, values);
        }
    }
    soundDefaults(next);
    checkAcross(next);
    config = next;
    return types;
}

void presetDefaults(rcs_preset preset, rcs_extension& extension) {
    const Kind& kind = kindOfSized(extension);
    if (!kind.preset) {
        invalid(std::string(kind.name) + ": no preset has settings for it; its *_INIT macro holds its defaults");
    }
    if (preset > RCS_PRESET_QUALITY) {
        invalid("Unknown preset " + std::to_string(preset));
    }
    SessionConfig values;
    presetSettings(preset, values);
    writeSettings(kind, values, extension);
}

void getSettings(const SessionConfig& config, rcs_extension& extension) {
    const Kind& kind = kindOfSized(extension);
    if (!kind.settings) {
        invalid("RCS_EXT_MODULE: a module's settings are the module's (rcs_session_get_module_stats and its own "
                "functions)");
    }
    if (kind.role && kind.role != config.role) {
        invalid(std::string(kind.name) + ": applies to " + (kind.role == RCS_ROLE_SHARE ? "sharing" : "viewing") +
                " sessions only");
    }
    writeSettings(kind, config, extension); // The trusted keys stay the session's: trusted_keys is NULL.
}

AgentExtensions parseAgentExtensions(const rcs_extension* chain) {
    SessionConfig parsed; // The parsers write into a session's configuration.
    uint32_t seen = 0;    // Each type at most once, so a circular chain fails as "chained twice".
    for (const rcs_extension* extension = chain; extension; extension = extension->next) {
        if (extension->type != RCS_EXT_LOG && extension->type != RCS_EXT_HOST_ENCODER) {
            invalid("rcs_agent_config: extension type " + std::to_string(extension->type) +
                    " does not apply to agents; they take RCS_EXT_LOG and RCS_EXT_HOST_ENCODER");
        }
        const Kind* kind = kindOf(extension->type);
        require(!(seen & (1u << kind->type)), kind->name, "chained twice");
        seen |= 1u << kind->type;
        kind->parse(*extension, kind->name, parsed);
    }
    AgentExtensions extensions;
    extensions.logLevel = parsed.log.max_level;
    if (seen & (1u << RCS_EXT_HOST_ENCODER)) {
        extensions.hostEncoder = parsed.hostEncoder;
    }
    return extensions;
}

rcs_capture_config parseThumbnailCapture(const rcs_capture_config* what) {
    constexpr const char* name = "RCS_EXT_CAPTURE";
    if (!what || what->header.type != RCS_EXT_CAPTURE) {
        invalid("what: an RCS_EXT_CAPTURE extension is required (RCS_CAPTURE_CONFIG_INIT)");
    }
    require(!what->header.next, name, "next must be NULL: a thumbnail takes the capture's settings alone");
    SessionConfig parsed;
    parseCapture(what->header, name, parsed);
    const rcs_capture_source source = parsed.capture.source;
    require(source != RCS_CAPTURE_HOST && source != RCS_CAPTURE_ENCODED, name,
            "a thumbnail is of a display, a region, a window or an application: the SDK captures nothing for "
            "sources HOST and ENCODED");
    return parsed.capture;
}

std::vector<uint32_t> streamDisplays(const SessionConfig& config) {
    std::vector<uint32_t> displays;
    for (const uint32_t display : config.streams.displays) {
        if (display) {
            displays.push_back(display);
        }
    }
    return displays;
}

SessionConfig parseConfig(const rcs_session_config& c) {
    if (c.struct_size != sizeof(c)) {
        invalid("rcs_session_config: initialise it with RCS_SESSION_CONFIG_INIT from the headers of this library "
                "(" RCSDK_VERSION_STRING ")");
    }
    if (c.version != RCSDK_VERSION) {
        invalid("rcs_session_config: built with the headers of version " + versionText(c.version) +
                ", this library is " RCSDK_VERSION_STRING "; use the headers of the library you ship");
    }
    if (c.role != RCS_ROLE_SHARE && c.role != RCS_ROLE_VIEW) {
        invalid("rcs_session_config: unknown role");
    }
    if (c.preset > RCS_PRESET_QUALITY) {
        invalid("rcs_session_config: unknown preset");
    }
    checkConnect(c.connect);
    SessionConfig config;
    config.role = c.role;
    config.preset = c.preset;
    config.connect = c.connect;
    config.identity = parseIdentity(c.identity);
    presetSettings(c.preset, config);
    soundDefaults(config);
    applyExtensions(config, c.extensions);
    return config;
}

} // namespace rcsdk::session
