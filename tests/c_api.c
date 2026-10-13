/* The public header compiles as C, the C entry points link, and configurations
 * are validated extension by extension. */
#include <rcsdk/agent.h>
#include <rcsdk/android.h>
#include <rcsdk/clipboard.h>
#include <rcsdk/gpu.h>
#include <rcsdk/host_codec.h>
#include <rcsdk/host_transport.h>
#include <rcsdk/module.h>
#include <rcsdk/rate_control.h>
#include <rcsdk/rcsdk.h>
#include <rcsdk/tuning.h>

#include <stdio.h>
#include <string.h>

static void onFrame(const rcs_frame* frame, void* user_data) {
    (void)frame;
    (void)user_data;
}

static void onGpuFrame(rcs_gpu_frame frame, void* user_data) {
    (void)frame;
    (void)user_data;
}

static void onEncodedVideo(const rcs_encoded_packet* packet, void* user_data) {
    (void)packet;
    (void)user_data;
}

static void onEncodedAudio(const rcs_encoded_audio* packet, void* user_data) {
    (void)packet;
    (void)user_data;
}

/* The reason the most recent failing call gave, kept across later calls. */
static char lastReason[256];

static void keepReason(void) {
    if (rcs_get_last_call_error(lastReason, sizeof(lastReason)) != RCS_OK) {
        lastReason[0] = '\0';
    }
}

static rcs_result create(const rcs_session_config* config, const rcs_session_callbacks* callbacks) {
    rcs_session session = NULL;
    const rcs_result result = rcs_session_create(config, callbacks, &session);
    keepReason(); /* Before destroy, which starts without a reason. */
    if ((result == RCS_OK) != (session != NULL) || rcs_session_destroy(session) != RCS_OK) {
        return RCS_ERROR_INTERNAL;
    }
    return result;
}

/* What start() says about a configuration create() takes; the session is gone afterwards. */
static rcs_result started(const rcs_session_config* config, const rcs_session_callbacks* callbacks) {
    rcs_session session = NULL;
    rcs_result result = rcs_session_create(config, callbacks, &session);
    if (result == RCS_OK) {
        result = rcs_session_start(session);
        keepReason();
        if (rcs_session_destroy(session) != RCS_OK) {
            return RCS_ERROR_INTERNAL;
        }
    }
    return result;
}

/* Whether a sharer with the default capture and encoder starts here: this
 * system captures a display and encodes H.264 in hardware (iOS and Android
 * capture no display, the simulators encode in software; their sharers'
 * start checks are left out). */
static int sharerStarts(void) {
    rcs_capabilities capabilities = RCS_CAPABILITIES_INIT;
    return rcs_get_capabilities(&capabilities) == RCS_OK &&
           (capabilities.capture_sources & (1u << RCS_CAPTURE_DISPLAY)) != 0 &&
           (capabilities.hardware_encoders & (1u << RCS_CODEC_H264)) != 0;
}

/* Whether a viewer can start here: an H.264 decoder (none without a platform
 * layer, e.g. WebAssembly outside a browser). Checks made at start come after
 * the codec check. */
static int viewerStarts(void) {
    rcs_capabilities capabilities = RCS_CAPABILITIES_INIT;
    return rcs_get_capabilities(&capabilities) == RCS_OK &&
           ((capabilities.hardware_decoders | capabilities.software_decoders) & (1u << RCS_CODEC_H264)) != 0;
}

static int reason(const char* text) {
    return strstr(lastReason, text) != NULL;
}

/* A host encoder that is never opened here: its calls only need to exist. */
static rcs_result hostOpen(const rcs_host_encoder_settings* settings, void* data) {
    (void)settings;
    (void)data;
    return RCS_OK;
}
static rcs_result hostEncode(const rcs_host_encoder_frame* frame, void* data) {
    (void)frame;
    (void)data;
    return RCS_OK;
}
static void hostClose(uint32_t stream, void* data) {
    (void)stream;
    (void)data;
}
static void hostEncoderClose(uint32_t stream, uint32_t encoding, void* data) {
    (void)stream;
    (void)encoding;
    (void)data;
}
static rcs_result hostDecoderOpen(const rcs_host_decoder_settings* settings, void* data) {
    (void)settings;
    (void)data;
    return RCS_OK;
}
static rcs_result hostDecode(const rcs_encoded_packet* packet, void* data) {
    (void)packet;
    (void)data;
    return RCS_OK;
}

/* The host's channel, never used here. */
static void signalSink(const char* signal, uint64_t route, void* user_data) {
    (void)signal;
    (void)route;
    (void)user_data;
}

static int32_t hostSend(uint64_t route, const uint8_t* data, uint32_t size, void* user_data) {
    (void)route;
    (void)data;
    (void)size;
    (void)user_data;
    return 1;
}

/* What a library's records looked like when they reached the log callback. */
static struct {
    int count;
    rcs_log_record last;
    char module[32], message[64];
    int inCallback;          /* rcs_log_in_callback() there. */
    rcs_result writtenThere; /* rcs_log_write() there: dropped. */
} written;

static void onRecord(const rcs_log_record* record, void* user_data) {
    rcs_log_record nested = *record;
    (void)user_data;
    ++written.count;
    written.last = *record;
    strncpy(written.module, record->module, sizeof(written.module) - 1);
    strncpy(written.message, record->message, sizeof(written.message) - 1);
    written.inCallback = rcs_log_in_callback();
    written.writtenThere = rcs_log_write(&nested);
}

/* Every failed check ends the test here, naming its line. */
static int failedAt(int line) {
    char now[256];
    rcs_get_last_call_error(now, sizeof(now));
    fprintf(stderr, "c_api.c:%d: check failed (last reason: \"%s\")\n", line, now[0] ? now : lastReason);
    return 1;
}

int main(void) {
    rcs_capabilities capabilities = RCS_CAPABILITIES_INIT;
    rcs_session_config config = RCS_SESSION_CONFIG_INIT;
    rcs_session_callbacks callbacks = RCS_SESSION_CALLBACKS_INIT;
    rcs_decoder_config codec = RCS_DECODER_CONFIG_INIT;
    rcs_share_video_config video = RCS_SHARE_VIDEO_CONFIG_INIT;
    rcs_decoder_config receive = RCS_DECODER_CONFIG_INIT;
    rcs_extension unknown = {99u, sizeof(rcs_extension), NULL};
    rcs_session session = NULL;
    rcs_state state = RCS_STATE_FAILED;
    rcs_result last = RCS_ERROR_INTERNAL;
    rcs_video_receive_stats received = RCS_VIDEO_RECEIVE_STATS_INIT;
    rcs_video_send_stats sent = RCS_VIDEO_SEND_STATS_INIT;
    rcs_peer_info peer = RCS_PEER_INFO_INIT;
    char failure[64];
    rcs_codec_implementation implementations[64];
    uint32_t count = 0, listed = 0, i;

    /* The library is the version of these headers, in number and in text. */
    {
        char version[32];
        snprintf(version, sizeof(version), "%u.%u.%u%s%s", RCSDK_VERSION_MAJOR, RCSDK_VERSION_MINOR,
                 RCSDK_VERSION_PATCH, RCSDK_VERSION_LABEL[0] ? "-" : "", RCSDK_VERSION_LABEL);
        if (rcs_version_number() != RCSDK_VERSION || rcs_header_version() != RCSDK_VERSION ||
            strcmp(rcs_version(), version) != 0 ||
            strcmp(rcs_result_message(RCS_OK), "Success") != 0) {
            return failedAt(__LINE__);
        }
    }
    if (rcs_get_capabilities(&capabilities) != RCS_OK || rcs_get_capabilities(NULL) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    if (rcs_list_displays(NULL, sizeof(rcs_display_info), 0, &count) != RCS_OK ||
        rcs_list_displays(NULL, 0, 4, &count) != RCS_ERROR_INVALID_ARGUMENT ||
        rcs_list_displays(NULL, sizeof(rcs_display_info) + 4, 0, &count) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    /* Windows: counted, listed; the list's element is checked like any other. */
    {
        rcs_window_info windows[64];
        uint32_t windowCount = 0;
        if (rcs_list_windows(NULL, sizeof(rcs_window_info), 0, &windowCount) != RCS_OK ||
            rcs_list_windows(windows, sizeof(rcs_window_info), 64, &windowCount) == RCS_ERROR_INVALID_ARGUMENT ||
            rcs_list_windows(NULL, sizeof(rcs_window_info) - 8, 0, &windowCount) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_list_windows(windows, sizeof(rcs_window_info), 4, NULL) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
    }
    /* Networks: counted, listed, the default first, each with a name, an
     * index and an address; a kind only where the system says kinds. A
     * browser has none of its own. */
    if (!(capabilities.transports & (1u << RCS_TRANSPORT_UDP))) {
        if (rcs_list_networks(NULL, sizeof(rcs_network_info), 0, &count) != RCS_ERROR_UNSUPPORTED ||
            capabilities.network_facts || rcs_list_wifi(NULL, sizeof(rcs_wifi_info), 0, &count) != RCS_ERROR_UNSUPPORTED ||
            capabilities.wifi_facts) {
            return failedAt(__LINE__);
        }
    } else {
        /* The Wi-Fi connected: only what the system says (wifi_facts), AWDL
         * on another channel only where it is up. */
        rcs_wifi_info wifi[8];
        uint32_t wifiCount = 0;
        if (rcs_list_wifi(NULL, sizeof(rcs_wifi_info), 0, &wifiCount) != RCS_OK || wifiCount > 8 ||
            rcs_list_wifi(wifi, sizeof(rcs_wifi_info), 8, &wifiCount) != RCS_OK ||
            rcs_list_wifi(NULL, sizeof(rcs_wifi_info) + 4, 0, &wifiCount) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        for (i = 0; i < wifiCount; ++i) {
            const rcs_wifi_info* w = &wifi[i];
            const uint32_t facts = capabilities.wifi_facts;
            if (!w->name[0] || w->band > RCS_WIFI_BAND_6_GHZ ||
                (!(facts & RCS_WIFI_FACT_CHANNEL) && (w->band || w->channel)) ||
                (!(facts & RCS_WIFI_FACT_WIDTH) && w->width_mhz) || (!(facts & RCS_WIFI_FACT_SIGNAL) && w->signal_dbm) ||
                (!(facts & RCS_WIFI_FACT_NOISE) && w->noise_dbm) || (!(facts & RCS_WIFI_FACT_RATE) && w->rate_mbps) ||
                (!(facts & RCS_WIFI_FACT_PEER_TO_PEER) && w->peer_to_peer) || (w->other_channel && !w->peer_to_peer) ||
                w->signal_dbm > 0) {
                return failedAt(__LINE__);
            }
            printf("  wifi %s: band %u, channel %u, %u MHz, signal %d dBm, noise %d dBm, %u Mbit/s, AWDL %u%s\n",
                   w->name, w->band, w->channel, w->width_mhz, w->signal_dbm, w->noise_dbm, w->rate_mbps,
                   w->peer_to_peer, w->other_channel ? " on another channel" : "");
        }
        rcs_network_info networks[32];
        uint32_t networkCount = 0;
        if (rcs_list_networks(NULL, sizeof(rcs_network_info), 0, &networkCount) != RCS_OK || networkCount > 32 ||
            rcs_list_networks(networks, sizeof(rcs_network_info), 32, &networkCount) != RCS_OK ||
            rcs_list_networks(NULL, sizeof(rcs_network_info) - 4, 0, &networkCount) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_notify_network_change() != RCS_OK) {
            return failedAt(__LINE__);
        }
        for (i = 0; i < networkCount; ++i) {
            const rcs_network_info* n = &networks[i];
            if (!n->name[0] || !n->index || (!n->ipv4 && !n->ipv6) || n->kind > RCS_NETWORK_OTHER || (n->flags >> 4) ||
                (i && (n->flags & RCS_NETWORK_FLAG_DEFAULT)) ||
                (!(capabilities.network_facts & RCS_NETWORK_FACT_KIND) && n->kind != RCS_NETWORK_UNKNOWN)) {
                return failedAt(__LINE__);
            }
        }
    }
    /* Pictures: what to take is a capture extension alone, of a source the
     * SDK captures, at most 8192 a side; the result goes to the caller's buffer. */
    {
        rcs_capture_config what = RCS_CAPTURE_CONFIG_INIT;
        rcs_picture out = RCS_PICTURE_INIT;
        uint8_t pixels[64];
        out.data = pixels;
        out.capacity = sizeof(pixels);
        if (rcs_capture_picture(&what, 16, 16, NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_capture_picture(NULL, 16, 16, &out) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_capture_picture(&what, 0, 16, &out) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_capture_picture(&what, 16, 8193, &out) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        what.source = RCS_CAPTURE_HOST;
        if (rcs_capture_picture(&what, 16, 16, &out) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        keepReason();
        if (!reason("HOST and ENCODED")) {
            return failedAt(__LINE__);
        }
        what.source = RCS_CAPTURE_DISPLAY;
        out.struct_size = 4;
        if (rcs_capture_picture(&what, 16, 16, &out) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
    }
    /* The capture sources this system offers: the host's own pictures always. */
    {
        rcs_capabilities sources = RCS_CAPABILITIES_INIT;
        if (rcs_get_capabilities(&sources) != RCS_OK || !(sources.capture_sources & (1u << RCS_CAPTURE_HOST)) ||
            !(sources.capture_sources & (1u << RCS_CAPTURE_ENCODED)) ||
            (sources.capture_sources >> (RCS_CAPTURE_ENCODED + 1))) {
            return failedAt(__LINE__);
        }
    }
    /* Every codec in the capability masks has an implementation, and vice versa. */
    if (rcs_list_codec_implementations(NULL, sizeof(rcs_codec_implementation), 0, &count) != RCS_OK || count > 64 ||
        rcs_list_codec_implementations(implementations, sizeof(rcs_codec_implementation), 64, &count) != RCS_OK) {
        return failedAt(__LINE__);
    }
    {
        uint32_t masks[2][2] = {{0, 0}, {0, 0}}; /* [direction - 1][acceleration] */
        uint32_t f;
        for (i = 0; i < count; ++i) {
            /* The largest picture each takes: within its codec's own limit (H.264: 4096 a side). */
            const uint32_t side = implementations[i].codec == RCS_CODEC_H264 ? 4096u : 8192u;
            /* Every implementation but NVDEC (4:4:4 only) takes 4:2:0 8-bit pictures, and only formats some
             * version defines. */
            const uint32_t formats = implementations[i].picture_formats;
            const int takes420 = (formats & (1u << RCS_PICTURE_420_8)) != 0;
            if (!implementations[i].name[0] || implementations[i].backend == RCS_BACKEND_AUTO ||
                implementations[i].max_width < 16 || implementations[i].max_height < 16 ||
                implementations[i].max_width > side || implementations[i].max_height > side ||
                takes420 == (implementations[i].backend == RCS_BACKEND_NVDEC) || !formats || (formats & ~0x7Eu)) {
                return failedAt(__LINE__);
            }
            /* The most it takes a second: of formats it takes, at least 1080p30 where said (H.264 level 4.0). */
            for (f = 0; f < 8; ++f) {
                const uint64_t rate = implementations[i].max_pixels_per_second[f];
                if (rate && (!(formats & (1u << f)) || rate < 1920ull * 1080 * 30)) {
                    return failedAt(__LINE__);
                }
            }
            masks[implementations[i].direction - 1][implementations[i].acceleration] |= 1u << implementations[i].codec;
        }
        if (masks[0][0] != capabilities.hardware_encoders || masks[0][1] != capabilities.software_encoders ||
            masks[1][0] != capabilities.hardware_decoders || masks[1][1] != capabilities.software_decoders) {
            return failedAt(__LINE__);
        }
    }

    /* A session needs a non-zero key and nothing else. */
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("key")) {
        return failedAt(__LINE__);
    }
    if (rcs_generate_connection_key(config.identity.key, sizeof(config.identity.key)) != RCS_OK ||
        rcs_session_create(&config, NULL, &session) != RCS_OK) {
        return failedAt(__LINE__);
    }
    if (rcs_session_get_state(session, &state) != RCS_OK || state != RCS_STATE_IDLE ||
        rcs_session_get_stats(session, RCS_STATS_VIDEO_RECEIVE, &received) != RCS_OK ||
        rcs_session_get_stats(session, RCS_STATS_VIDEO_SEND, &sent) != RCS_ERROR_UNSUPPORTED ||
        rcs_session_get_failure(session, &last, failure, sizeof(failure)) != RCS_OK || last != RCS_OK ||
        failure[0] || rcs_session_get_failure(session, &last, NULL, 0) != RCS_OK ||
        rcs_session_get_failure(session, &last, NULL, 4) != RCS_ERROR_INVALID_ARGUMENT ||
        rcs_session_get_peer_info(session, &peer) != RCS_OK || peer.platform != RCS_PLATFORM_UNKNOWN) {
        return failedAt(__LINE__);
    }
    /* Actions need a run, and the role and source they belong to. */
    {
        rcs_frame frame;
        rcs_audio_frame sound;
        memset(&frame, 0, sizeof(frame));
        memset(&sound, 0, sizeof(sound));
        frame.struct_size = sizeof(frame);
        sound.struct_size = sizeof(sound);
        if (rcs_session_request_keyframe(session) != RCS_ERROR_INVALID_STATE ||
            rcs_session_push_video_frame(session, &frame) != RCS_ERROR_UNSUPPORTED ||
            rcs_session_push_audio(session, &sound) != RCS_ERROR_INVALID_STATE ||
            rcs_session_push_video_frame(session, NULL) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        frame.struct_size -= 4;
        if (rcs_session_push_video_frame(session, &frame) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
    }
    /* Data messages: checked before the state is; both roles have the statistics. */
    {
        rcs_data_message message = RCS_DATA_MESSAGE_INIT;
        rcs_data_stats data = RCS_DATA_STATS_INIT;
        const uint8_t byte = 1;
        if (rcs_session_send_data(session, NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_send_data(session, &message) != RCS_ERROR_INVALID_ARGUMENT) { /* No data. */
            return failedAt(__LINE__);
        }
        message.channel = 4;
        message.size = 1;
        message.data = &byte;
        if (rcs_session_send_data(session, &message) != RCS_ERROR_INVALID_STATE) { /* Not running. */
            return failedAt(__LINE__);
        }
        message.flags = RCS_DATA_UNRELIABLE;
        message.size = RCS_DATA_MAX_DATAGRAM + 1;
        if (rcs_session_send_data(session, &message) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        if (rcs_session_get_stats(session, RCS_STATS_DATA, &data) != RCS_OK || data.queued_bytes) {
            return failedAt(__LINE__);
        }
    }
    /* Updates while idle: any extension of the role; others are refused. */
    receive.acceleration = RCS_ACCELERATION_SOFTWARE;
    if (rcs_session_update(session, &receive.header) != RCS_OK ||
        rcs_session_update(session, &video.header) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    keepReason();
    if (!reason("RCS_EXT_SHARE_VIDEO") || rcs_session_get_state(session, &state) != RCS_OK) {
        return failedAt(__LINE__);
    }
    keepReason(); /* A successful call leaves no reason. */
    if (lastReason[0]) {
        return failedAt(__LINE__);
    }
    if (rcs_session_destroy(session) != RCS_OK) {
        return failedAt(__LINE__);
    }
    /* Headers of another version are refused. */
    config.version = RCSDK_VERSION + 1;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("version")) {
        return failedAt(__LINE__);
    }
    config.version = RCSDK_VERSION;

    /* Defaults: H.264 at 1920x1080, 60 FPS, 20 Mbit/s; the sharer's own windows
     * and sounds are shared like everything else. */
    {
        rcs_capture_config capture = RCS_CAPTURE_CONFIG_INIT;
        rcs_send_audio_config audio = RCS_SEND_AUDIO_CONFIG_INIT;
        rcs_encoder_config encoder = RCS_ENCODER_CONFIG_INIT;
        rcs_decoder_config decoder = RCS_DECODER_CONFIG_INIT;
        rcs_view_request_config request = RCS_VIEW_REQUEST_CONFIG_INIT;
        rcs_encoded_packet packet = RCS_ENCODED_PACKET_INIT;
        rcs_host_encoder_config hostEncoder = RCS_HOST_ENCODER_CONFIG_INIT;
        rcs_host_decoder_config hostDecoder = RCS_HOST_DECODER_CONFIG_INIT;
        rcs_video_tuning_config tuning = RCS_VIDEO_TUNING_CONFIG_INIT;
        /* ...8-bit 4:2:0 SDR pictures, the BT.709 matrix, limited range. */
        if (encoder.formats[0] != RCS_PICTURE_420_8 || encoder.formats[1] != 0 || encoder.matrix != RCS_MATRIX_BT709 ||
            encoder.range != RCS_RANGE_LIMITED || decoder.formats[0] != RCS_PICTURE_420_8 || decoder.formats[1] != 0 ||
            request.format != 0 || request.cursor != RCS_CURSOR_IN_VIDEO || packet.struct_size != sizeof(packet) ||
            packet.format != RCS_PICTURE_420_8 || packet.color.primaries != RCS_PRIMARIES_BT709 ||
            packet.color.transfer != RCS_TRANSFER_BT709 || packet.color.matrix != RCS_MATRIX_BT709 ||
            packet.color.range != RCS_RANGE_LIMITED || hostEncoder.picture_formats != (1u << RCS_PICTURE_420_8) ||
            hostDecoder.picture_formats != (1u << RCS_PICTURE_420_8) || hostEncoder.low_latency != 1u) {
            return failedAt(__LINE__);
        }
        if (encoder.codecs[0] != RCS_CODEC_H264 || encoder.codecs[1] != 0 ||
            encoder.acceleration != RCS_ACCELERATION_HARDWARE || tuning.speed != RCS_ENCODER_FASTEST ||
            codec.codecs[0] != RCS_CODEC_H264 || codec.codecs[1] != 0 ||
            codec.acceleration != RCS_ACCELERATION_HARDWARE || codec.backend != RCS_BACKEND_AUTO ||
            video.width != 1920 || video.height != 1080 || video.frames_per_second != 60 ||
            video.bitrate_bps != 20000000u || capture.exclude_current_process != 0 || !audio.enabled ||
            audio.exclude_current_process != 0) {
            return failedAt(__LINE__);
        }
    }
    /* Codecs are known, unique, without gaps. */
    config.extensions = &codec.header;
    codec.codecs[1] = RCS_CODEC_H264;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("twice")) {
        return failedAt(__LINE__);
    }
    codec.codecs[1] = 0;
    codec.codecs[2] = RCS_CODEC_HEVC;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("gaps")) {
        return failedAt(__LINE__);
    }
    codec.codecs[0] = 0;
    codec.codecs[2] = 0;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT) { /* None listed. */
        return failedAt(__LINE__);
    }
    codec.codecs[0] = RCS_CODEC_HEVC;
    codec.codecs[1] = RCS_CODEC_H264;
    if (create(&config, &callbacks) != RCS_OK) {
        return failedAt(__LINE__);
    }
    /* Acceleration is hardware, software or, for decoders, hardware first. */
    codec.acceleration = RCS_ACCELERATION_HARDWARE_FIRST;
    if (create(&config, &callbacks) != RCS_OK) {
        return failedAt(__LINE__);
    }
    codec.acceleration = 3;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("unknown acceleration")) {
        return failedAt(__LINE__);
    }
    codec.acceleration = RCS_ACCELERATION_SOFTWARE;
    /* Values and extension types this library does not define are refused, not ignored. */
    codec.backend = 77;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("backend")) {
        return failedAt(__LINE__);
    }
    codec.backend = RCS_BACKEND_AUTO;
    codec.codecs[2] = 9;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("unknown codec")) {
        return failedAt(__LINE__);
    }
    codec.codecs[2] = 0;
    codec.header.next = &unknown;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("extension type")) {
        return failedAt(__LINE__);
    }
    /* Each extension at most once, with exactly its own size. */
    codec.header.next = &codec.header;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    codec.header.next = NULL;
    codec.header.struct_size = sizeof(rcs_extension);
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("struct_size")) {
        return failedAt(__LINE__);
    }
    codec.header.struct_size = sizeof(codec) + 4;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("struct_size")) {
        return failedAt(__LINE__);
    }
    codec.header.struct_size = sizeof(codec);

    /* Images go to one callback, never to both. */
    callbacks.on_frame = onFrame;
    callbacks.on_gpu_frame = onGpuFrame;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    callbacks.on_frame = NULL;
    if (create(&config, &callbacks) != RCS_OK) {
        return failedAt(__LINE__);
    }
    /* Extensions apply to their role only. */
    codec.header.next = &video.header;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("sharing")) {
        return failedAt(__LINE__);
    }
    config.role = RCS_ROLE_SHARE;
    config.extensions = &video.header;
    video.width = 641;
    if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("width")) {
        return failedAt(__LINE__);
    }
    video.width = 1280;
    video.height = 720;
    if (create(&config, &callbacks) != RCS_OK) {
        return failedAt(__LINE__);
    }
    {
        rcs_encoder_config encoder = RCS_ENCODER_CONFIG_INIT;
        encoder.acceleration = RCS_ACCELERATION_HARDWARE_FIRST; /* Encoders choose per stream too. */
        video.header.next = &encoder.header;
        if (create(&config, &callbacks) != RCS_OK) {
            return failedAt(__LINE__);
        }
        encoder.acceleration = RCS_ACCELERATION_HARDWARE_FIRST + 1;
        if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("unknown acceleration")) {
            return failedAt(__LINE__);
        }
        encoder.acceleration = RCS_ACCELERATION_HARDWARE;
        encoder.backend = RCS_BACKEND_HOST; /* Reported, never chosen. */
        if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("reported, not chosen")) {
            return failedAt(__LINE__);
        }
        video.header.next = NULL;
    }
    /* A host D3D11 device is for Windows viewers only. */
    config.role = RCS_ROLE_VIEW;
    config.extensions = &codec.header;
    {
        rcs_gpu_device_config device = RCS_GPU_DEVICE_CONFIG_INIT;
        codec.header.next = &device.header;
        device.d3d11_device = &config; /* Any non-null pointer; never dereferenced by validation. */
#ifdef _WIN32
        if (create(&config, &callbacks) != RCS_OK) {
            return failedAt(__LINE__);
        }
#else
        /* A field this system has no use for: valid, but start() refuses it (UNSUPPORTED). */
        if (viewerStarts() && (started(&config, &callbacks) != RCS_ERROR_UNSUPPORTED || !reason("Windows"))) {
            return failedAt(__LINE__);
        }
#endif
        config.role = RCS_ROLE_SHARE; /* It is a viewer's. */
        codec.header.next = NULL;
        config.extensions = &device.header;
        if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("viewing")) {
            return failedAt(__LINE__);
        }
        config.role = RCS_ROLE_VIEW;
        config.extensions = &codec.header;
    }
    /* The encoded packets, to the host on either end; a viewer may take them
     * without decoding (decode 0), which nothing else may then expect. */
    {
        rcs_encoded_output_config output = RCS_ENCODED_OUTPUT_CONFIG_INIT;
        rcs_encoded_audio sound = RCS_ENCODED_AUDIO_INIT;
        rcs_encoded_packet packet = RCS_ENCODED_PACKET_INIT;
        rcs_host_decoder_config hostDecoder = RCS_HOST_DECODER_CONFIG_INIT;
        if (output.decode != 1u || output.user_data || output.on_video || output.on_audio || packet.after_gap ||
            sound.struct_size != sizeof(sound) || sound.viewer_id || sound.codec || sound.sample_rate != 48000u ||
            sound.channels != 2u || sound.data || sound.config) {
            return failedAt(__LINE__);
        }
        codec.header.next = &output.header;
        if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("on_video or on_audio")) {
            return failedAt(__LINE__); /* Nothing to hand them to. */
        }
        output.on_audio = onEncodedAudio; /* The sound's alone, decoding as usual. */
        if (create(&config, &callbacks) != RCS_OK) {
            return failedAt(__LINE__);
        }
        output.decode = 2;
        if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("decode")) {
            return failedAt(__LINE__);
        }
        output.decode = 0; /* The picture would go nowhere. */
        if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("on_video")) {
            return failedAt(__LINE__);
        }
        output.on_video = onEncodedVideo;
        if (started(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("on_gpu_frame")) {
            return failedAt(__LINE__); /* No picture is decoded for it. */
        }
        /* A host codec needs no host decoder when nothing decodes, and a host decoder has nothing to do. */
        codec.codecs[2] = RCS_CODEC_HOST_FIRST;
        if (create(&config, &callbacks) != RCS_OK) {
            return failedAt(__LINE__);
        }
        hostDecoder.codecs[0] = RCS_CODEC_HOST_FIRST;
        strcpy(hostDecoder.name, "unused");
        hostDecoder.open = hostDecoderOpen;
        hostDecoder.decode = hostDecode;
        hostDecoder.reset = hostClose;
        hostDecoder.close = hostClose;
        output.header.next = &hostDecoder.header;
        if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("decodes nothing")) {
            return failedAt(__LINE__);
        }
        output.header.next = NULL;
        codec.codecs[2] = 0;
        /* Sharers take it too, and leave decode to viewers. */
        config.role = RCS_ROLE_SHARE;
        config.extensions = &output.header;
        if (create(&config, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        config.role = RCS_ROLE_VIEW;
        config.extensions = &codec.header;
        codec.header.next = NULL;
    }
    /* What the viewer asks for must be something it offers. */
    {
        rcs_view_request_config request = RCS_VIEW_REQUEST_CONFIG_INIT;
        codec.header.next = &request.header;
        request.codec = RCS_CODEC_AV1; /* codec lists HEVC and H.264. */
        if (create(&config, &callbacks) != RCS_ERROR_INVALID_ARGUMENT || !reason("RCS_EXT_DECODER")) {
            return failedAt(__LINE__);
        }
        request.codec = RCS_CODEC_H264;
        request.audio_codec = RCS_AUDIO_AAC; /* The default sound offers it. */
        if (create(&config, &callbacks) != RCS_OK) {
            return failedAt(__LINE__);
        }
        codec.header.next = NULL;
    }
    /* A structure of another size comes from other headers: refused, never misread. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_session_callbacks larger = RCS_SESSION_CALLBACKS_INIT;
        rcs_stream_info stream = RCS_STREAM_INFO_INIT;
        rcs_network_stats network = RCS_NETWORK_STATS_INIT;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        capabilities.struct_size -= 8;
        if (rcs_get_capabilities(&capabilities) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        keepReason();
        if (!reason("RCS_CAPABILITIES_INIT")) {
            return failedAt(__LINE__);
        }
        larger.struct_size += 8;
        if (create(&plain, &larger) != RCS_ERROR_INVALID_ARGUMENT || !reason("RCS_SESSION_CALLBACKS_INIT")) {
            return failedAt(__LINE__);
        }
        plain.struct_size -= 8;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("RCS_SESSION_CONFIG_INIT")) {
            return failedAt(__LINE__);
        }
        plain.struct_size = sizeof(plain);
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_get_stream_info(session, 0, &stream) != RCS_OK ||
            rcs_session_get_stats(session, RCS_STATS_NETWORK, &network) != RCS_OK) {
            return failedAt(__LINE__);
        }
        stream.struct_size -= 4;
        network.struct_size += 8;
        if (rcs_session_get_stream_info(session, 0, &stream) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_get_stats(session, RCS_STATS_NETWORK, &network) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        network.struct_size = sizeof(network);
        if (rcs_session_get_stats(session, 99, &network) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        rcs_session_destroy(session);
    }
    /* The sound devices and the microphone permission. Android lists them
     * through Java: without the host's VM (a test program has none) both say so. */
    {
        rcs_audio_device_info devices[64];
        uint32_t count = 0, i;
        const rcs_permission permission = rcs_microphone_permission();
        rcs_capabilities caps = RCS_CAPABILITIES_INIT;
        void* vm = NULL;
        void* context = NULL;
        if (rcs_get_capabilities(&caps) == RCS_OK && caps.platform == RCS_PLATFORM_ANDROID &&
            rcs_android_get_java(&vm, &context) != RCS_OK) {
            if (rcs_list_audio_devices(NULL, sizeof(rcs_audio_device_info), 0, &count) != RCS_ERROR_UNSUPPORTED ||
                (keepReason(), !reason("rcs_android_initialize")) || permission != RCS_PERMISSION_UNAVAILABLE ||
                rcs_request_microphone_permission() != RCS_ERROR_UNSUPPORTED) {
                return failedAt(__LINE__);
            }
        } else if (rcs_list_audio_devices(NULL, sizeof(rcs_audio_device_info), 0, &count) != RCS_OK || count > 64 ||
            rcs_list_audio_devices(devices, sizeof(rcs_audio_device_info) - 4, 64, &count) !=
                RCS_ERROR_INVALID_ARGUMENT ||
            rcs_list_audio_devices(devices, sizeof(rcs_audio_device_info), 64, &count) != RCS_OK ||
            permission > RCS_PERMISSION_GRANTED) {
            return failedAt(__LINE__);
        }
        for (i = 0; i < count; ++i) {
            if (!devices[i].device_id || devices[i].input > 1 || devices[i].is_default > 1 ||
                !memchr(devices[i].name, 0, sizeof(devices[i].name))) {
                return failedAt(__LINE__);
            }
        }
    }
    /* The sound extensions are validated like the others. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_receive_audio_config audio = RCS_RECEIVE_AUDIO_CONFIG_INIT;
        rcs_audio_tuning_config tuning = RCS_AUDIO_TUNING_CONFIG_INIT;
        rcs_send_audio_config shared = RCS_SEND_AUDIO_CONFIG_INIT;
        /* Starts on a system without hardware decoders too (the iOS simulator). */
        rcs_decoder_config decoder = RCS_DECODER_CONFIG_INIT;
        decoder.acceleration = RCS_ACCELERATION_HARDWARE_FIRST;
        tuning.header.next = &decoder.header;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.extensions = &audio.header;
        audio.codecs[1] = RCS_AUDIO_OPUS; /* Listed twice. */
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("twice")) {
            return failedAt(__LINE__);
        }
        audio.codecs[1] = 0;
        audio.codecs[2] = 0;
        audio.codecs[0] = 7;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("unknown codec")) {
            return failedAt(__LINE__);
        }
        audio.codecs[0] = RCS_AUDIO_OPUS;
        /* The delay controls (both roles take the tuning): out-of-range values are refused... */
        audio.header.next = &tuning.header;
        tuning.max_delay_ms = 5;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("max_delay_ms")) {
            return failedAt(__LINE__);
        }
        tuning.max_delay_ms = 200;
        tuning.redundancy = 2;
        if (tuning.redundancy == 0 || create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("redundancy")) {
            return failedAt(__LINE__);
        }
        tuning.redundancy = 1;
        tuning.playback_period_frames = 8;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("playback_period_frames")) {
            return failedAt(__LINE__);
        }
        tuning.playback_period_frames = 128;
        tuning.buffer_margin_us = 200000;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("buffer_margin_us")) {
            return failedAt(__LINE__);
        }
        tuning.buffer_margin_us = 500;
        /* ...Windows-only ones are refused elsewhere... */
        tuning.playback_queue_frames = 720;
#ifdef _WIN32
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
#else
        if (create(&plain, NULL) != RCS_OK ||
            (viewerStarts() && (started(&plain, NULL) != RCS_ERROR_UNSUPPORTED || !reason("Windows only")))) {
            return failedAt(__LINE__);
        }
#endif
        tuning.playback_queue_frames = 0;
        /* ...and valid ones taken. */
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* Sound both ways, on both roles. A viewer sends a microphone or its host's sound, never what its
         * machine plays (it holds the sharer's); a microphone may go without voice processing. */
        audio.header.next = &shared.header;
        shared.header.next = &tuning.header;
        if (shared.device != 0 || shared.voice_processing != 1 || create(&plain, NULL) != RCS_OK ||
            (viewerStarts() &&
             (started(&plain, NULL) != RCS_ERROR_UNSUPPORTED || !reason("a viewer sends a microphone")))) {
            return failedAt(__LINE__);
        }
        shared.source = RCS_AUDIO_MICROPHONE + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("unknown source")) {
            return failedAt(__LINE__);
        }
        shared.source = RCS_AUDIO_MICROPHONE;
        shared.voice_processing = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("voice_processing")) {
            return failedAt(__LINE__);
        }
        shared.voice_processing = 0;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* The sample buffers' two sounds, both at full volume by default; up to four times, 0 leaving one out. */
        if (shared.sample_buffer_audio_percent != 100 || shared.sample_buffer_microphone_percent != 100) {
            return failedAt(__LINE__);
        }
        shared.sample_buffer_microphone_percent = 401;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("sample_buffer_microphone_percent")) {
            return failedAt(__LINE__);
        }
        shared.sample_buffer_microphone_percent = 0;
        shared.sample_buffer_audio_percent = 400;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        shared.sample_buffer_audio_percent = shared.sample_buffer_microphone_percent = 100;
        /* A sharer that plays its viewers' sound while sending what its machine plays would send them
         * their own voices back: refused, unless it leaves its own sound out or sends the host's. */
        plain.role = RCS_ROLE_SHARE;
        tuning.header.next = NULL; /* The decoder is a viewer's. */
        shared.source = RCS_AUDIO_SYSTEM;
        if (sharerStarts() && (started(&plain, NULL) != RCS_ERROR_UNSUPPORTED || !reason("their own voices back"))) {
            return failedAt(__LINE__);
        }
        shared.source = RCS_AUDIO_HOST;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        audio.playback = 0; /* The host plays: its business. */
        shared.source = RCS_AUDIO_SYSTEM;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        audio.playback = 1;
        plain.role = RCS_ROLE_VIEW;
        shared = (rcs_send_audio_config)RCS_SEND_AUDIO_CONFIG_INIT;
        audio.header.next = &tuning.header;
        /* The sharer's: unknown backends and periods out of range are refused. */
        plain.role = RCS_ROLE_SHARE;
        plain.extensions = &tuning.header;
        tuning.capture_backend = 9;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("capture_backend")) {
            return failedAt(__LINE__);
        }
        tuning.capture_backend = RCS_AUDIO_CAPTURE_AUTO;
        tuning.capture_period_frames = 10;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("capture_period_frames")) {
            return failedAt(__LINE__);
        }
        tuning.capture_period_frames = 0;
        plain.extensions = &shared.header;
        shared.bitrate_bps = 8000;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("bitrate_bps")) {
            return failedAt(__LINE__);
        }
        shared.bitrate_bps = 128000;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        {
            rcs_capabilities caps = RCS_CAPABILITIES_INIT;
            const uint32_t builtIn = (1u << RCS_AUDIO_OPUS) | (1u << RCS_AUDIO_PCM);
            /* macOS and Windows capture what the machine plays; iOS and Android let no application capture
             * the sound of others from native code, a browser none at all, and Linux has no layer yet. */
            if (rcs_get_capabilities(&caps) != RCS_OK ||
                !caps.audio_capture_backends ==
                    (caps.platform == RCS_PLATFORM_MACOS || caps.platform == RCS_PLATFORM_WINDOWS) ||
                (caps.audio_encoders & builtIn) != builtIn || (caps.audio_decoders & builtIn) != builtIn) {
                return failedAt(__LINE__);
            }
        }
    }
    /* What a lost connection does: on by default; its pace and the network's
     * timeouts (tuning) bounds checked. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_reconnect_config reconnect = RCS_RECONNECT_CONFIG_INIT;
        rcs_network_tuning_config network = RCS_NETWORK_TUNING_CONFIG_INIT;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.extensions = &reconnect.header;
        reconnect.header.next = &network.header;
        if (!reconnect.keep_listening || !reconnect.reconnect || network.reconnect_window_ms != 30000 ||
            network.reconnect_interval_ms != 1000 || network.connect_timeout_ms != 5000 ||
            network.idle_timeout_ms != 5000 || network.ice_gather_ms != 1500 || network.ice_connect_ms != 15000 ||
            network.turn_allocate_ms != 5000 || network.send_queue_bytes != 64u << 20 ||
            network.in_flight_bytes != 65536 || network.local_interface[0] || network.audio_dscp != 0 ||
            network.ice_addresses != 0) {
            return failedAt(__LINE__);
        }
        network.audio_dscp = 64;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("audio_dscp")) {
            return failedAt(__LINE__);
        }
        network.audio_dscp = 46;
        network.reconnect_window_ms = 500;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("reconnect_window_ms")) {
            return failedAt(__LINE__);
        }
        network.reconnect_window_ms = 0; /* Until stopped. */
        network.reconnect_interval_ms = 50;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("reconnect_interval_ms")) {
            return failedAt(__LINE__);
        }
        network.reconnect_interval_ms = 100;
        network.connect_timeout_ms = 999;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("connect_timeout_ms")) {
            return failedAt(__LINE__);
        }
        network.connect_timeout_ms = 60000;
        network.idle_timeout_ms = 60001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("idle_timeout_ms")) {
            return failedAt(__LINE__);
        }
        network.idle_timeout_ms = 1000;
        network.ice_gather_ms = 100;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("ice_gather_ms")) {
            return failedAt(__LINE__);
        }
        network.ice_gather_ms = 200;
        network.ice_connect_ms = 120001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("ice_connect_ms")) {
            return failedAt(__LINE__);
        }
        network.ice_connect_ms = 5000;
        network.ice_addresses = RCS_ICE_ADDRESSES_LOOPBACK << 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("ice_addresses")) {
            return failedAt(__LINE__);
        }
        network.ice_addresses = RCS_ICE_ADDRESSES_LOOPBACK;
        network.turn_allocate_ms = 500;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("turn_allocate_ms")) {
            return failedAt(__LINE__);
        }
        network.turn_allocate_ms = 30000;
        memset(network.local_interface, 'a', sizeof(network.local_interface));
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("local_interface")) {
            return failedAt(__LINE__);
        }
        strcpy(network.local_interface, "en0");
        /* The paths' timings. */
        if (network.path_stall_ms != 1000 || network.path_hold_ms != 30000 || network.standby_probe_ms != 15000) {
            return failedAt(__LINE__);
        }
        network.path_stall_ms = 199;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("path_stall_ms")) {
            return failedAt(__LINE__);
        }
        network.path_stall_ms = 200;
        network.path_hold_ms = 999;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("path_hold_ms")) {
            return failedAt(__LINE__);
        }
        network.path_hold_ms = 3600000;
        network.standby_probe_ms = 60001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("standby_probe_ms")) {
            return failedAt(__LINE__);
        }
        network.standby_probe_ms = 1000;
        plain.role = RCS_ROLE_SHARE; /* Both roles take them. */
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
    }
    /* What a change of network does (rcs_paths_config): each field 0 or 1, the policies and network kinds
     * some version defines. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_paths_config paths = RCS_PATHS_CONFIG_INIT;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.extensions = &paths.header;
        if (paths.migrate != 1 || paths.excluded_networks || paths.policy != RCS_PATH_POLICY_FOLLOW_SYSTEM ||
            paths.standby || paths.restore_control || create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        paths.migrate = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("migrate")) {
            return failedAt(__LINE__);
        }
        paths.migrate = 0;
        paths.excluded_networks = 1u << (RCS_NETWORK_OTHER + 1);
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("excluded_networks")) {
            return failedAt(__LINE__);
        }
        paths.excluded_networks = 0;
        paths.policy = RCS_PATH_POLICY_STAY + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("policy")) {
            return failedAt(__LINE__);
        }
        paths.policy = RCS_PATH_POLICY_STAY;
        paths.standby = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("standby")) {
            return failedAt(__LINE__);
        }
        paths.standby = 0;
        paths.restore_control = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("restore_control")) {
            return failedAt(__LINE__);
        }
        paths.restore_control = 1;
        plain.role = RCS_ROLE_SHARE; /* Both roles take it. */
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
    }
    /* The way to the TURN server is checked, and over TLS how it proves itself. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_ice_servers_config transport = RCS_ICE_SERVERS_CONFIG_INIT;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.extensions = &transport.header;
        transport.turn_transport = 5;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("turn_transport")) {
            return failedAt(__LINE__);
        }
        transport.turn_transport = RCS_TURN_TCP;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* A pin or a name only with TLS: nothing else would check them. */
        transport.turn_certificate_sha256[31] = 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("turn_certificate_sha256")) {
            return failedAt(__LINE__);
        }
        memset(transport.turn_certificate_sha256, 0, sizeof(transport.turn_certificate_sha256));
        strcpy(transport.turn_tls_server_name, "turn.example.com");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("turn_tls_server_name")) {
            return failedAt(__LINE__);
        }
        /* Over TLS a server known by name is checked by that name... */
        transport.turn_transport = RCS_TURN_TLS;
        transport.turn_tls_server_name[0] = '\0';
        strcpy(transport.turn_server, "turn.example.com:443");
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        strcpy(transport.turn_server, "turn.example.com");
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* ...one known by its address needs a name or a pin... */
        strcpy(transport.turn_server, "192.0.2.1:443");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("turn_server is an address")) {
            return failedAt(__LINE__);
        }
        strcpy(transport.turn_server, "192.0.2.1");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("turn_server is an address")) {
            return failedAt(__LINE__);
        }
        strcpy(transport.turn_server, "[2001:db8::1]");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("turn_server is an address")) {
            return failedAt(__LINE__);
        }
        /* ...as does one whose host is no DNS name (not UTF-8 here)... */
        strcpy(transport.turn_server, "turn.\xff\xfe.example.com:443");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("must be a DNS name")) {
            return failedAt(__LINE__);
        }
        strcpy(transport.turn_server, "turn example.com");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("must be a DNS name")) {
            return failedAt(__LINE__);
        }
        strcpy(transport.turn_tls_server_name, "turn.example.com");
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        transport.turn_tls_server_name[0] = '\0';
        transport.turn_certificate_sha256[0] = 0xAB;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* ...and a name is a DNS name: no address, no port, NUL-terminated. */
        strcpy(transport.turn_tls_server_name, "192.0.2.1");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("DNS name")) {
            return failedAt(__LINE__);
        }
        strcpy(transport.turn_tls_server_name, "turn.example.com:443");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("DNS name")) {
            return failedAt(__LINE__);
        }
        memset(transport.turn_tls_server_name, 'a', sizeof(transport.turn_tls_server_name));
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("NUL-terminated")) {
            return failedAt(__LINE__);
        }
        strcpy(transport.turn_tls_server_name, "turn-2.example.com");
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
    }
    /* The host's own channel: its kind and datagram size are checked, its
     * address and port are not used, the transport and the extension come
     * together, and its calls fit a running session of this transport only. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_host_transport_config channel = RCS_HOST_TRANSPORT_CONFIG_INIT;
        rcs_session_callbacks callbacks = RCS_SESSION_CALLBACKS_INIT;
        rcs_session session = NULL;
        const uint8_t byte = 1;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        if (channel.host_channel != RCS_HOST_CHANNEL_DATAGRAM || channel.host_max_datagram != 0 ||
            channel.on_transport_send || channel.user_data) {
            return failedAt(__LINE__);
        }
        plain.connect.transport = RCS_TRANSPORT_FORWARDER + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("transport")) {
            return failedAt(__LINE__);
        }
        plain.connect.transport = RCS_TRANSPORT_HOST;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("RCS_EXT_HOST_TRANSPORT")) {
            return failedAt(__LINE__); /* The transport needs the channel's extension... */
        }
        plain.extensions = &channel.header;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("on_transport_send")) {
            return failedAt(__LINE__); /* ...with its callback. */
        }
        channel.on_transport_send = hostSend;
        channel.host_channel = RCS_HOST_CHANNEL_STREAM + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("host_channel")) {
            return failedAt(__LINE__);
        }
        channel.host_channel = RCS_HOST_CHANNEL_STREAM;
        channel.host_max_datagram = RCS_HOST_MIN_DATAGRAM - 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("host_max_datagram")) {
            return failedAt(__LINE__);
        }
        channel.host_max_datagram = RCS_HOST_MAX_DATAGRAM + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("host_max_datagram")) {
            return failedAt(__LINE__);
        }
        channel.host_max_datagram = RCS_HOST_MIN_DATAGRAM;
        plain.connect.address[0] = '\0';
        plain.connect.port = 0;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        if (rcs_session_create(&plain, &callbacks, &session) != RCS_OK ||
            rcs_session_transport_receive(session, 0, &byte, 1) != RCS_ERROR_INVALID_STATE ||
            rcs_session_transport_closed(session, 0) != RCS_ERROR_INVALID_STATE ||
            rcs_session_transport_receive(session, 0, NULL, 1) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_transport_receive(NULL, 0, &byte, 1) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_transport_closed(NULL, 0) != RCS_ERROR_INVALID_ARGUMENT || rcs_session_destroy(session) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* The channel's extension without its transport has nothing to carry. */
        plain.connect.transport = RCS_TRANSPORT_UDP;
        strcpy(plain.connect.address, "127.0.0.1");
        plain.connect.port = 47990;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            !reason("FORWARDER with link RCS_FORWARDER_HOST, only")) {
            return failedAt(__LINE__);
        }
        plain.extensions = NULL;
        if (rcs_session_create(&plain, &callbacks, &session) != RCS_OK ||
            rcs_session_transport_receive(session, 0, &byte, 1) != RCS_ERROR_UNSUPPORTED ||
            rcs_session_transport_closed(session, 0) != RCS_ERROR_UNSUPPORTED || rcs_session_destroy(session) != RCS_OK) {
            return failedAt(__LINE__);
        }
    }
    /* Through a forwarder: its extension with the transport, and each field checked. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_forwarder_config forwarder = RCS_FORWARDER_CONFIG_INIT;
        rcs_forwarder_stats stats = RCS_FORWARDER_STATS_INIT;
        rcs_session session = NULL;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        if (forwarder.token[0] || forwarder.forwarder_key_sha256[0] || forwarder.key_rotation_ms ||
            forwarder.from_admission || stats.struct_size != sizeof(stats)) {
            return failedAt(__LINE__);
        }
        plain.connect.transport = RCS_TRANSPORT_FORWARDER;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("RCS_EXT_FORWARDER")) {
            return failedAt(__LINE__);
        }
        plain.extensions = &forwarder.header;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("token")) {
            return failedAt(__LINE__);
        }
        strcpy(forwarder.token, "rcsf1.payload.signature");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("forwarder_key_sha256")) {
            return failedAt(__LINE__);
        }
        forwarder.forwarder_key_sha256[5] = 1;
        forwarder.key_rotation_ms = 999;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("key_rotation_ms")) {
            return failedAt(__LINE__);
        }
        forwarder.key_rotation_ms = 60000;
        forwarder.from_admission = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("from_admission")) {
            return failedAt(__LINE__);
        }
        forwarder.from_admission = 1;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_get_stats(session, RCS_STATS_FORWARDER, &stats) != RCS_OK || stats.media_frames ||
            rcs_session_destroy(session) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* Over the host's own channel to it (a browser page's WebSocket): the channel's extension with it,
         * no address, datagrams of 1200 bytes; the hop's channel has route 0 alone. A system without
         * sockets of its own reaches a forwarder that way only, and says so. */
        {
            rcs_host_transport_config channel = RCS_HOST_TRANSPORT_CONFIG_INIT;
            rcs_capabilities capabilities = RCS_CAPABILITIES_INIT;
            const uint8_t byte = 1;
            forwarder.link = RCS_FORWARDER_HOST;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("RCS_EXT_HOST_TRANSPORT")) {
                return failedAt(__LINE__);
            }
            channel.on_transport_send = hostSend;
            forwarder.header.next = &channel.header;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("connect.address stays empty")) {
                return failedAt(__LINE__);
            }
            plain.connect.address[0] = 0;
            plain.connect.port = 0;
            channel.host_max_datagram = RCS_HOST_MIN_DATAGRAM;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("at least 1200")) {
                return failedAt(__LINE__);
            }
            channel.host_max_datagram = 0;
            if (create(&plain, NULL) != RCS_OK || rcs_session_create(&plain, NULL, &session) != RCS_OK ||
                rcs_session_transport_receive(session, 0, &byte, 1) != RCS_ERROR_INVALID_STATE ||
                rcs_session_destroy(session) != RCS_OK) {
                return failedAt(__LINE__);
            }
            if (rcs_get_capabilities(&capabilities) != RCS_OK ||
                !(capabilities.transports & (1u << RCS_TRANSPORT_FORWARDER)) ||
                !(capabilities.forwarder_links & (1u << RCS_FORWARDER_HOST))) {
                return failedAt(__LINE__);
            }
            forwarder.link = RCS_FORWARDER_UDP;
            forwarder.header.next = NULL;
            strcpy(plain.connect.address, "127.0.0.1");
            plain.connect.port = 47990;
            if (!(capabilities.forwarder_links & (1u << RCS_FORWARDER_UDP)) &&
                (started(&plain, NULL) != RCS_ERROR_UNSUPPORTED || !reason("forwarder_links"))) {
                return failedAt(__LINE__);
            }
        }
        plain.connect.transport = RCS_TRANSPORT_UDP;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("FORWARDER only")) {
            return failedAt(__LINE__);
        }
        plain.extensions = NULL;
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_get_stats(session, RCS_STATS_FORWARDER, &stats) != RCS_ERROR_UNSUPPORTED ||
            rcs_session_destroy(session) != RCS_OK) {
            return failedAt(__LINE__);
        }
    }
    /* Device keys and identities. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT; /* No identity.key: not needed for a password. */
        rcs_identity* identity = &plain.identity;
        rcs_device_identity_config device_identity = RCS_DEVICE_IDENTITY_CONFIG_INIT;
        uint8_t device[RCS_DEVICE_KEY_SIZE], other[RCS_DEVICE_KEY_SIZE], publicKey[RCS_PUBLIC_KEY_SIZE];
        uint8_t trusted[2 * RCS_PUBLIC_KEY_SIZE];
        if (rcs_generate_device_key(device, sizeof(device)) != RCS_OK ||
            rcs_generate_device_key(other, sizeof(other)) != RCS_OK ||
            rcs_generate_device_key(device, 32) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_device_public_key(device, sizeof(device), publicKey, sizeof(publicKey)) != RCS_OK ||
            memcmp(publicKey, device, RCS_PUBLIC_KEY_SIZE) != 0 || memcmp(device, other, sizeof(device)) == 0) {
            return failedAt(__LINE__);
        }
        /* A private scalar with another key's public half is not a key pair. */
        memcpy(other + RCS_PUBLIC_KEY_SIZE, device + RCS_PUBLIC_KEY_SIZE, 32);
        if (rcs_device_public_key(other, sizeof(other), publicKey, sizeof(publicKey)) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("key is empty")) {
            return failedAt(__LINE__); /* The default secret is the connection key. */
        }
        identity->secret = RCS_SECRET_PASSWORD;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("password")) {
            return failedAt(__LINE__);
        }
        strcpy(identity->password, "a long random password");
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* A PIN: 4..32 characters of UTF-8; pin_attempts 1..1000. */
        identity->secret = RCS_SECRET_PIN;
        strcpy(identity->password, "123456789012345678901234567890123");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("secret PIN")) {
            return failedAt(__LINE__);
        }
        {
            char pin[40];
            if (rcs_generate_pin(pin, sizeof(pin), 3) != RCS_ERROR_INVALID_ARGUMENT ||
                rcs_generate_pin(pin, sizeof(pin), 33) != RCS_ERROR_INVALID_ARGUMENT ||
                rcs_generate_pin(pin, 6, 6) != RCS_ERROR_BUFFER_TOO_SMALL ||
                rcs_generate_pin(NULL, 7, 6) != RCS_ERROR_INVALID_ARGUMENT || rcs_generate_pin(pin, 33, 32) != RCS_OK ||
                strlen(pin) != 32 || strspn(pin, "0123456789") != 32 || rcs_generate_pin(pin, 7, 6) != RCS_OK ||
                strlen(pin) != 6 || strspn(pin, "0123456789") != 6) {
                return failedAt(__LINE__);
            }
            strcpy(identity->password, pin);
        }
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* Four CJK characters, 12 bytes. */
        strcpy(identity->password, "\xe5\xaf\x86\xe7\xa0\x81\xe5\xaf\x86\xe7\xa0\x81");
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        strcpy(identity->password, "123");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("4..32")) {
            return failedAt(__LINE__);
        }
        strcpy(identity->password, "12\xff"
                                  "45");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("UTF-8")) {
            return failedAt(__LINE__);
        }
        strcpy(identity->password, "1234");
        identity->pin_attempts = 0;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("pin_attempts")) {
            return failedAt(__LINE__);
        }
        identity->pin_attempts = 1001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("pin_attempts")) {
            return failedAt(__LINE__);
        }
        identity->pin_attempts = 1000;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        identity->secret = 4;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("unknown secret")) {
            return failedAt(__LINE__);
        }
        identity->secret = RCS_SECRET_NONE; /* Needs a device key and trusted keys. */
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("secret NONE")) {
            return failedAt(__LINE__);
        }
        plain.extensions = &device_identity.header;
        device_identity.has_device_key = 1;
        memcpy(device_identity.device_key, other, sizeof(other));
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("device_key")) {
            return failedAt(__LINE__);
        }
        memcpy(device_identity.device_key, device, sizeof(device));
        memcpy(trusted, device, RCS_PUBLIC_KEY_SIZE);
        memset(trusted + RCS_PUBLIC_KEY_SIZE, 1, RCS_PUBLIC_KEY_SIZE); /* Not a point. */
        trusted[RCS_PUBLIC_KEY_SIZE] = 0x04;
        device_identity.trusted_keys = trusted;
        device_identity.trusted_key_count = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("trusted_keys[1]")) {
            return failedAt(__LINE__);
        }
        device_identity.trusted_key_count = 1;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        device_identity.trusted_keys = NULL;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("trusted_keys")) {
            return failedAt(__LINE__);
        }
        device_identity.trusted_keys = trusted;
        /* Another identity for a session that exists: checked as at create, with the device identity. */
        {
            rcs_session session = NULL;
            rcs_identity next = *identity;
            if (rcs_session_create(&plain, NULL, &session) != RCS_OK) {
                return failedAt(__LINE__);
            }
            next.secret = RCS_SECRET_PIN;
            strcpy(next.password, "12");
            if (rcs_session_set_identity(session, &next) != RCS_ERROR_INVALID_ARGUMENT ||
                (keepReason(), !reason("4..32")) || rcs_session_set_identity(session, NULL) != RCS_ERROR_INVALID_ARGUMENT) {
                return failedAt(__LINE__);
            }
            strcpy(next.password, "2468");
            if (rcs_session_set_identity(session, &next) != RCS_OK) {
                return failedAt(__LINE__);
            }
            rcs_session_destroy(session);
        }
    }
    /* Secure desktops. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_capture_config capture = RCS_CAPTURE_CONFIG_INIT;
        rcs_desktop_info desktop = RCS_DESKTOP_INFO_INIT;
        rcs_session session = NULL;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.role = RCS_ROLE_SHARE;
        plain.extensions = &capture.header;
        capture.secure_desktop = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("secure_desktop")) {
            return failedAt(__LINE__);
        }
        capture.secure_desktop = 1; /* Valid to ask; whether this process can is checked when it starts. */
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        plain.extensions = NULL;
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_get_desktop_info(session, &desktop) != RCS_OK || desktop.kind != RCS_DESKTOP_UNKNOWN ||
            desktop.struct_size != sizeof(desktop)) {
            return failedAt(__LINE__);
        }
        desktop.struct_size = 1;
        if (rcs_session_get_desktop_info(session, &desktop) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_get_desktop_info(NULL, &desktop) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        /* Stopping with a reason: only the reasons that exist. */
        if (rcs_session_stop_with_reason(session, RCS_STOP_RESTART) != RCS_OK ||
            rcs_session_stop_with_reason(session, 9) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_stop_with_reason(NULL, RCS_STOP_NORMAL) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        rcs_session_destroy(session);
        if (strcmp(rcs_result_message(RCS_ERROR_RESTARTING), "Unknown result") == 0 ||
            strcmp(rcs_result_message(RCS_ERROR_REPLACED), "Unknown result") == 0 ||
            strcmp(rcs_result_message(RCS_ERROR_REFUSED), "Unknown result") == 0) {
            return failedAt(__LINE__);
        }
    }
    /* A process no supervisor started (rcsdk/supervisor.h): nothing to wait for (without a limit too), no key. */
    {
        if (rcs_helper_wait_for_stop(10) != RCS_ERROR_UNSUPPORTED ||
            rcs_helper_wait_for_stop(UINT32_MAX) != RCS_ERROR_UNSUPPORTED) {
            return failedAt(__LINE__);
        }
        {
            uint8_t key[32];
            if (rcs_helper_get_key(NULL, 32) != RCS_ERROR_INVALID_ARGUMENT ||
                rcs_helper_get_key(key, 16) != RCS_ERROR_INVALID_ARGUMENT ||
                rcs_helper_get_key(key, sizeof(key)) != RCS_ERROR_UNSUPPORTED) {
                return 111;
            }
            keepReason();
            if (!reason("supervisor")) {
                return 111;
            }
        }
    }
    /* The agent link, and the agent itself. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_agent_link_config link = RCS_AGENT_LINK_CONFIG_INIT;
        rcs_capture_config capture = RCS_CAPTURE_CONFIG_INIT;
        rcs_agent_config agentConfig = RCS_AGENT_CONFIG_INIT;
        rcs_log_config agentLog = RCS_LOG_CONFIG_INIT, agentLogAgain = RCS_LOG_CONFIG_INIT;
        rcs_network_tuning_config notForAgents = RCS_NETWORK_TUNING_CONFIG_INIT;
        rcs_agent_info info = RCS_AGENT_INFO_INIT;
        rcs_agent agent = NULL;
        rcs_session session = NULL;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.role = RCS_ROLE_SHARE;
        plain.extensions = &link.header;
#ifdef _WIN32
        strcpy(link.endpoint, "rcsdk-test-endpoint");
#else
        strcpy(link.endpoint, "/tmp/rcsdk-test-endpoint.sock");
#endif
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("key")) {
            return failedAt(__LINE__); /* An endpoint needs its key. */
        }
        memset(link.key, 3, sizeof(link.key));
        link.access = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("access")) {
            return failedAt(__LINE__);
        }
        link.access = RCS_AGENT_ACCESS_ANY_USER;
        strcpy(link.endpoint, "relative");
#ifdef _WIN32
        strcpy(link.endpoint, "a/b");
#endif
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("endpoint")) {
            return failedAt(__LINE__);
        }
#ifdef _WIN32
        strcpy(link.endpoint, "rcsdk-test-endpoint");
#else
        strcpy(link.endpoint, "/tmp/rcsdk-test-endpoint.sock");
#endif
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        plain.role = RCS_ROLE_VIEW; /* The agent is a sharer's. */
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("sharing")) {
            return failedAt(__LINE__);
        }
        plain.role = RCS_ROLE_SHARE;
        capture.source = RCS_CAPTURE_HOST; /* The host's own pictures are encoded in the session's process. */
        link.header.next = &capture.header;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("RCS_EXT_AGENT_LINK")) {
            return failedAt(__LINE__);
        }
        link.header.next = NULL;
        link.endpoint[0] = '\0'; /* No endpoint: the session's own process is the agent. */
        capture.source = RCS_CAPTURE_HOST;
        link.header.next = &capture.header;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* The queries. */
        link.header.next = NULL;
        if (link.on_agent_attach || link.user_data) {
            return 77; /* Without on_agent_attach every agent holding the key is taken. */
        }
        if (info.result != RCS_OK || info.message[0] || info.account[0] || info.process_id) {
            return 77; /* RCS_AGENT_INFO_INIT: nothing known. */
        }
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK || rcs_session_get_agent_info(session, &info) != RCS_OK ||
            !info.attached || info.struct_size != sizeof(info) || !info.process_id || !info.account[0] ||
            info.result != RCS_OK) {
            return failedAt(__LINE__); /* The session's own agent: this process, as the system names its account. */
        }
        info.struct_size = 1;
        if (rcs_session_get_agent_info(session, &info) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_get_agent_info(session, NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_list_displays(session, NULL, 1, 0, NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_list_windows(session, NULL, 1, 0, NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_list_windows(session, NULL, sizeof(rcs_window_info), 0, &listed) != RCS_OK) {
            return failedAt(__LINE__);
        }
        rcs_session_destroy(session);
        /* An agent needs the headers of this library, an endpoint and its key. */
        if (rcs_agent_create(&agentConfig, NULL, &agent) != RCS_ERROR_INVALID_ARGUMENT || agent != NULL) {
            return failedAt(__LINE__);
        }
        memset(agentConfig.key, 3, sizeof(agentConfig.key));
        strcpy(agentConfig.endpoint, link.endpoint[0] ? link.endpoint : "x");
        agentConfig.reconnect_interval_ms = 10;
#ifdef _WIN32
        strcpy(agentConfig.endpoint, "rcsdk-test-endpoint");
#else
        strcpy(agentConfig.endpoint, "/tmp/rcsdk-test-endpoint.sock");
#endif
        if (rcs_agent_create(&agentConfig, NULL, &agent) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        keepReason();
        if (!reason("reconnect_interval_ms")) {
            return failedAt(__LINE__);
        }
        agentConfig.reconnect_interval_ms = 1000;
        if (agentConfig.access != RCS_AGENT_ACCESS_PRIVILEGED) {
            return 80; /* By default an agent serves privileged sessions only. */
        }
        agentConfig.access = 2;
        if (rcs_agent_create(&agentConfig, NULL, &agent) != RCS_ERROR_INVALID_ARGUMENT) {
            return 80;
        }
        keepReason();
        if (!reason("access")) {
            return 80;
        }
        agentConfig.access = RCS_AGENT_ACCESS_ANY_USER;
        agentConfig.version = RCSDK_VERSION + 1;
        if (rcs_agent_create(&agentConfig, NULL, &agent) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        keepReason();
        if (!reason("headers")) {
            return failedAt(__LINE__);
        }
        agentConfig.version = RCSDK_VERSION;
        /* An agent's chain holds RCS_EXT_LOG, once, with a level some version defines. */
        agentConfig.extensions = &notForAgents.header;
        if (rcs_agent_create(&agentConfig, NULL, &agent) != RCS_ERROR_INVALID_ARGUMENT || agent ||
            (keepReason(), !reason("they take RCS_EXT_LOG"))) {
            return failedAt(__LINE__);
        }
        agentConfig.extensions = &agentLog.header;
        agentLog.max_level = RCS_LOG_DEBUG + 1;
        if (rcs_agent_create(&agentConfig, NULL, &agent) != RCS_ERROR_INVALID_ARGUMENT ||
            (keepReason(), !reason("max_level"))) {
            return failedAt(__LINE__);
        }
        agentLog.max_level = RCS_LOG_WARNING;
        agentLog.header.next = &agentLogAgain.header;
        if (rcs_agent_create(&agentConfig, NULL, &agent) != RCS_ERROR_INVALID_ARGUMENT ||
            (keepReason(), !reason("chained twice"))) {
            return failedAt(__LINE__);
        }
        agentLog.header.next = NULL;
        agentLog.header.struct_size = sizeof(rcs_extension);
        if (rcs_agent_create(&agentConfig, NULL, &agent) != RCS_ERROR_INVALID_ARGUMENT ||
            (keepReason(), !reason("RCS_EXT_LOG: struct_size"))) {
            return failedAt(__LINE__);
        }
        agentLog.header.struct_size = sizeof(agentLog);
        if (rcs_agent_create(&agentConfig, NULL, &agent) != RCS_OK || !agent) {
            return failedAt(__LINE__);
        }
        {
            rcs_state state = RCS_STATE_FAILED;
            rcs_result result = RCS_ERROR_INTERNAL;
            if (rcs_agent_get_state(agent, &state) != RCS_OK || state != RCS_STATE_IDLE ||
                rcs_agent_get_failure(agent, &result, NULL, 0) != RCS_OK || result != RCS_OK ||
                rcs_agent_wait(agent, 10) != RCS_ERROR_INVALID_STATE || rcs_agent_stop(agent) != RCS_OK ||
                rcs_agent_destroy(agent) != RCS_OK || rcs_agent_destroy(NULL) != RCS_OK) {
                return failedAt(__LINE__);
            }
        }
    }
    /* Presets: the settings of the extensions the host does not chain, each
     * *_INIT being BALANCED's; a chained one is the host's entirely. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_share_video_config video = RCS_SHARE_VIDEO_CONFIG_INIT, balanced = RCS_SHARE_VIDEO_CONFIG_INIT;
        rcs_video_tuning_config tuning = RCS_VIDEO_TUNING_CONFIG_INIT;
        rcs_view_request_config request = RCS_VIEW_REQUEST_CONFIG_INIT;
        rcs_encoder_config encoder = RCS_ENCODER_CONFIG_INIT;
        rcs_module_config module = RCS_MODULE_CONFIG_INIT;
        rcs_host_transport_config channel = RCS_HOST_TRANSPORT_CONFIG_INIT;
        rcs_session session = NULL;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        /* Field by field: the structure's padding is no setting. */
        if (rcs_preset_defaults(RCS_PRESET_BALANCED, &video.header) != RCS_OK || video.width != balanced.width ||
            video.height != balanced.height || video.frames_per_second != balanced.frames_per_second ||
            video.bitrate_bps != balanced.bitrate_bps || video.rate_mode != balanced.rate_mode ||
            video.min_bitrate_bps != balanced.min_bitrate_bps || video.follow_requests != balanced.follow_requests ||
            video.max_width != balanced.max_width || video.max_bitrate_bps != balanced.max_bitrate_bps ||
            rcs_preset_defaults(RCS_PRESET_LOW_BANDWIDTH, &video.header) != RCS_OK || video.width != 1280 ||
            video.height != 720 || video.frames_per_second != 30 || video.header.type != RCS_EXT_SHARE_VIDEO ||
            rcs_preset_defaults(RCS_PRESET_LOW_LATENCY, &tuning.header) != RCS_OK || !tuning.input_boost_ms ||
            rcs_preset_defaults(RCS_PRESET_QUALITY, &encoder.header) != RCS_OK ||
            encoder.formats[0] != RCS_PICTURE_444_8 || encoder.formats[1] != RCS_PICTURE_420_8) {
            return failedAt(__LINE__);
        }
        /* A `next` passed in stays; other presets, types, sizes and the types no preset has are refused. */
        video.header.next = &tuning.header;
        if (rcs_preset_defaults(RCS_PRESET_BALANCED, &video.header) != RCS_OK || video.header.next != &tuning.header ||
            rcs_preset_defaults(RCS_PRESET_QUALITY + 1, &video.header) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_preset_defaults(RCS_PRESET_BALANCED, &unknown) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_preset_defaults(RCS_PRESET_BALANCED, &module.header) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_preset_defaults(RCS_PRESET_BALANCED, &channel.header) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_preset_defaults(RCS_PRESET_BALANCED, NULL) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        video.header.struct_size -= 4;
        if (rcs_preset_defaults(RCS_PRESET_BALANCED, &video.header) != RCS_ERROR_INVALID_ARGUMENT ||
            (keepReason(), !reason("struct_size"))) {
            return failedAt(__LINE__);
        }
        video.header.struct_size = sizeof(video);
        video.header.next = NULL;
        plain.preset = RCS_PRESET_QUALITY + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("preset")) {
            return failedAt(__LINE__);
        }
        /* A viewer of LOW_BANDWIDTH asks for 1280x720 at 30 FPS; another preset at once changes it. */
        plain.preset = RCS_PRESET_LOW_BANDWIDTH;
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_get_settings(session, &request.header) != RCS_OK || request.width != 1280 ||
            request.frames_per_second != 30 || rcs_session_get_settings(session, &video.header) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_get_settings(session, &module.header) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_set_preset(session, RCS_PRESET_BALANCED) != RCS_OK ||
            rcs_session_get_settings(session, &request.header) != RCS_OK || request.width != 0 ||
            rcs_session_set_preset(session, 9) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        rcs_session_destroy(session);
        /* A sharer of LOW_LATENCY with its own video: the video is the host's, the tuning the preset's. */
        plain.role = RCS_ROLE_SHARE;
        plain.preset = RCS_PRESET_LOW_LATENCY;
        video = balanced;
        plain.extensions = &video.header;
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_get_settings(session, &tuning.header) != RCS_OK || tuning.input_boost_ms == 0 ||
            rcs_session_get_settings(session, &video.header) != RCS_OK ||
            video.bitrate_bps != balanced.bitrate_bps || rcs_session_set_preset(session, RCS_PRESET_LOW_BANDWIDTH) != RCS_OK ||
            rcs_session_get_settings(session, &video.header) != RCS_OK || video.width != 1920 ||
            rcs_session_get_settings(session, &tuning.header) != RCS_OK || tuning.overuse_queue_ms != 20 ||
            rcs_session_get_settings(session, &channel.header) != RCS_OK || channel.on_transport_send) {
            return failedAt(__LINE__);
        }
        rcs_session_destroy(session);
    }
    /* Data channel limits are bounds checked. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_network_tuning_config data = RCS_NETWORK_TUNING_CONFIG_INIT;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.extensions = &data.header;
        data.send_queue_bytes = 1024;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("send_queue_bytes")) {
            return failedAt(__LINE__);
        }
        data.send_queue_bytes = 1u << 20;
        data.in_flight_bytes = 100;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("in_flight_bytes")) {
            return failedAt(__LINE__);
        }
        data.in_flight_bytes = 4096;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
    }
    /* Modules: registered once, their settings checked by them; the clipboard's
     * settings bounds checked, and a copy checked before anything is sent. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_module_config use = RCS_MODULE_CONFIG_INIT;
        rcs_clipboard_settings clipboard = RCS_CLIPBOARD_SETTINGS_INIT;
        rcs_clipboard_content content = RCS_CLIPBOARD_CONTENT_INIT;
        rcs_clipboard_stats stats = RCS_CLIPBOARD_STATS_INIT;
        rcs_module_stats moduleStats = RCS_MODULE_STATS_INIT;
        rcs_module_info listed[8];
        uint32_t count = 0;
        rcs_session session = NULL;
        rcs_module module = NULL;
        rcs_session_callbacks none = RCS_SESSION_CALLBACKS_INIT;
        const uint8_t text = 'a';
        uint8_t large[2000];
        rcs_clipboard_item item;
        item.format = RCS_CLIPBOARD_TEXT;
        item.size = 1;
        item.data = &text;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        use.module = RCS_CLIPBOARD_MODULE;
        use.settings = &clipboard;
        use.size = sizeof(clipboard);
        plain.extensions = &use.header;
        if (clipboard.flow != 0 || clipboard.target != RCS_CLIPBOARD_SYSTEM ||
            clipboard.formats != 0 || clipboard.max_bytes != 16u << 20 ||
            clipboard.check_interval_ms != 250 || clipboard.send_existing != 0 || clipboard.send_concealed != 0 ||
            content.flow != RCS_CLIPBOARD_SEND || content.item_size != sizeof(rcs_clipboard_item)) {
            return failedAt(__LINE__);
        }
        /* Not registered yet. */
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("not registered")) {
            return failedAt(__LINE__);
        }
        if (rcs_register_module(rcs_clipboard_module()) != RCS_OK ||
            rcs_register_module(rcs_clipboard_module()) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_register_module(NULL) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        if (rcs_list_modules(NULL, sizeof(rcs_module_info), 0, &count) != RCS_OK || count != 1 ||
            rcs_list_modules(listed, sizeof(rcs_module_info), 8, &count) != RCS_OK ||
            strcmp(listed[0].id, RCS_CLIPBOARD_MODULE) != 0 ||
            listed[0].flags != (RCS_MODULE_PEER | RCS_MODULE_MACHINE)) {
            return failedAt(__LINE__);
        }
        clipboard.flow = 4;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("flow")) {
            return failedAt(__LINE__);
        }
        clipboard.flow = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
        clipboard.target = 3;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("target")) {
            return failedAt(__LINE__);
        }
        clipboard.target = RCS_CLIPBOARD_HOST;
        clipboard.formats = 1u << 6;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("formats")) {
            return failedAt(__LINE__);
        }
        clipboard.formats = RCS_CLIPBOARD_ALL_FORMATS;
        clipboard.max_bytes = 1023;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("max_bytes")) {
            return failedAt(__LINE__);
        }
        clipboard.max_bytes = 1024;
        clipboard.check_interval_ms = 49;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("check_interval_ms")) {
            return failedAt(__LINE__);
        }
        clipboard.check_interval_ms = 5000;
        clipboard.send_existing = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("send_existing")) {
            return failedAt(__LINE__);
        }
        clipboard.send_existing = 1;
        clipboard.send_concealed = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("send_concealed")) {
            return failedAt(__LINE__);
        }
        clipboard.send_concealed = 1;
        /* RECEIVE into the host's own clipboard needs on_clipboard. */
        if (create(&plain, &none) != RCS_ERROR_INVALID_ARGUMENT || !reason("on_clipboard")) {
            return failedAt(__LINE__);
        }
        clipboard.flow = RCS_CLIPBOARD_SEND;
        use.size = 4; /* Not its settings. */
        if (create(&plain, &none) != RCS_ERROR_INVALID_ARGUMENT || !reason("rcs_clipboard_settings")) {
            return failedAt(__LINE__);
        }
        use.size = sizeof(clipboard);
        if (rcs_session_create(&plain, &none, &session) != RCS_OK) {
            return failedAt(__LINE__);
        }
        if (rcs_module_find(session, RCS_CLIPBOARD_MODULE, &module) != RCS_OK || !module ||
            rcs_module_session(module) != session || rcs_module_find(session, "nope", &module) != RCS_ERROR_INVALID_STATE) {
            return failedAt(__LINE__);
        }
        content.count = 1;
        content.items = &item;
        if (rcs_clipboard_send(session, &content) != RCS_ERROR_INVALID_STATE || /* Not running. */
            rcs_clipboard_send(NULL, &content) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_clipboard_send(session, NULL) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        content.struct_size = 4;
        if (rcs_clipboard_send(session, &content) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        content.struct_size = sizeof(content);
        content.flow = RCS_CLIPBOARD_RECEIVE;
        if (rcs_clipboard_send(session, &content) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        content.flow = RCS_CLIPBOARD_SEND;
        memset(large, 'a', sizeof(large));
        item.size = sizeof(large); /* More than max_bytes, checked once running. */
        item.data = large;
        if (rcs_clipboard_send(session, &content) != RCS_ERROR_INVALID_STATE) {
            return failedAt(__LINE__);
        }
        if (rcs_clipboard_get_stats(session, 0, &stats) != RCS_OK || stats.sent ||
            rcs_session_get_module_stats(session, 0, RCS_CLIPBOARD_MODULE, &moduleStats) != RCS_OK ||
            moduleStats.sent_messages || rcs_session_get_module_stats(session, 0, "nope", &moduleStats) != RCS_ERROR_INVALID_STATE) {
            return failedAt(__LINE__);
        }
        rcs_session_destroy(session);
        if (strcmp(rcs_result_message(RCS_ERROR_DEVICE), "Unknown result") == 0) {
            return failedAt(__LINE__);
        }
    }
    /* The host's own encoder: codecs, a name and its calls; the new entry points refuse NULL. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_host_encoder_config encoder = RCS_HOST_ENCODER_CONFIG_INIT;
        rcs_encoder_config listed = RCS_ENCODER_CONFIG_INIT;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.role = RCS_ROLE_SHARE;
        plain.extensions = &encoder.header;
        if (RCS_MAX_CODECS != 8 || RCS_CODEC_HOST_FIRST != 16 || RCS_CODEC_HOST_LAST != 31) {
            return failedAt(__LINE__);
        }
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("codecs")) {
            return failedAt(__LINE__);
        }
        encoder.codecs[0] = RCS_CODEC_HOST_FIRST;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("name")) {
            return failedAt(__LINE__);
        }
        strcpy(encoder.name, "a host's encoder");
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("open and close")) {
            return failedAt(__LINE__);
        }
        encoder.open = hostOpen;
        encoder.encode = hostEncode;
        encoder.close = hostEncoderClose;
        /* H.264 is listed by default; the host's codec 16 is offered beside it. */
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* The temporal layers it makes: 1..3 (rcs_scalability). */
        if (encoder.temporal_layers != 1) {
            return failedAt(__LINE__);
        }
        encoder.temporal_layers = 0;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("temporal_layers")) {
            return failedAt(__LINE__);
        }
        encoder.temporal_layers = RCS_SCALABILITY_L1T3 + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("temporal_layers")) {
            return failedAt(__LINE__);
        }
        encoder.temporal_layers = RCS_SCALABILITY_L1T3;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        listed.codecs[0] = RCS_CODEC_HOST_FIRST + 1; /* Not the host encoder's. */
        encoder.header.next = &listed.header;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("host codec 17")) {
            return failedAt(__LINE__);
        }
        if (rcs_encoder_output_push(NULL, NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_push_encoded_video(NULL, NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_video_ready(NULL) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
    }
    /* The video's tuning: the defaults of adaptive bitrate's rules and the
     * rest, their ranges; both roles take it (each uses its own fields). */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_video_tuning_config rules = RCS_VIDEO_TUNING_CONFIG_INIT;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.extensions = &rules.header;
        if (rules.overuse_queue_ms != 30 || rules.severe_queue_ms != 60 || rules.clear_queue_ms != 10 ||
            rules.overuse_persist_ms != 150 || rules.overuse_loss_permille != 100 ||
            rules.clear_loss_permille != 50 || rules.decrease_percent != 85 ||
            rules.decrease_interval_ms != 300 || rules.probe_increase_percent != 50 || rules.increase_percent != 8 ||
            rules.recovery_increase_percent != 25 || rules.frame_spread_us != 1000 ||
            rules.max_pacing_bps != 800000000u || rules.path_pacing_percent != 110 || rules.path_burst_us != 8000 ||
            rules.fec_mode != RCS_FEC_AUTO || rules.fec_percent != 50 ||
            rules.ref_invalidation != 1 || rules.degradation != RCS_DEGRADE_FRAME_RATE ||
            rules.min_size_percent != 50 || rules.start_bitrate_bps != 0 || rules.input_boost_ms != 0 ||
            rules.speed != RCS_ENCODER_FASTEST || rules.keyframe_interval_ms != 0 ||
            rules.keyframe_min_interval_ms != 500 || rules.queue_frames != 64 || rules.max_queue_age_ms != 80) {
            return failedAt(__LINE__);
        }
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        rules.queue_frames = 257;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("queue_frames")) {
            return failedAt(__LINE__);
        }
        rules.queue_frames = 64;
        rules.max_queue_age_ms = 2001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("max_queue_age_ms")) {
            return failedAt(__LINE__);
        }
        rules.max_queue_age_ms = 80;
        rules.keyframe_min_interval_ms = 50;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("keyframe_min_interval_ms")) {
            return failedAt(__LINE__);
        }
        rules.keyframe_min_interval_ms = 500;
        rules.speed = RCS_ENCODER_BEST_QUALITY + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("speed")) {
            return failedAt(__LINE__);
        }
        rules.speed = RCS_ENCODER_BALANCED;
        rules.input_boost_ms = 1001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("input_boost_ms")) {
            return failedAt(__LINE__);
        }
        rules.input_boost_ms = 0;
        rules.start_bitrate_bps = 100000;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("start_bitrate_bps")) {
            return failedAt(__LINE__);
        }
        rules.start_bitrate_bps = 0;
        rules.degradation = RCS_DEGRADE_BALANCED + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("degradation")) {
            return failedAt(__LINE__);
        }
        rules.degradation = RCS_DEGRADE_SIZE;
        rules.min_size_percent = 24;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("min_size_percent")) {
            return failedAt(__LINE__);
        }
        rules.min_size_percent = 50;
        plain.role = RCS_ROLE_SHARE;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        rules.severe_queue_ms = 20;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("severe_queue_ms")) {
            return failedAt(__LINE__);
        }
        rules.severe_queue_ms = 60;
        rules.clear_loss_permille = 100;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("clear_loss_permille")) {
            return failedAt(__LINE__);
        }
        rules.clear_loss_permille = 50;
        rules.overuse_persist_ms = 2001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("overuse_persist_ms")) {
            return failedAt(__LINE__);
        }
        rules.overuse_persist_ms = 0;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        rules.overuse_persist_ms = 150;
        rules.increase_percent = 1001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("increase_percent")) {
            return failedAt(__LINE__);
        }
        rules.increase_percent = 8;
        rules.frame_spread_us = 0;
        rules.max_pacing_bps = 4000000000u;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        rules.frame_spread_us = 100001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("frame_spread_us")) {
            return failedAt(__LINE__);
        }
        rules.frame_spread_us = 100000;
        rules.max_pacing_bps = 999999;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("max_pacing_bps")) {
            return failedAt(__LINE__);
        }
        rules.max_pacing_bps = 4000000001u;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("max_pacing_bps")) {
            return failedAt(__LINE__);
        }
        /* Pacing to the path: 0 (none) or 110..1000 percent of its rate, after a burst of 0..100 000 us. */
        rules.max_pacing_bps = 800000000u;
        rules.path_pacing_percent = 0;
        rules.path_burst_us = 0;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        rules.path_pacing_percent = 1000;
        rules.path_burst_us = 100000;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        rules.path_pacing_percent = 109;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("path_pacing_percent")) {
            return failedAt(__LINE__);
        }
        rules.path_pacing_percent = 1001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("path_pacing_percent")) {
            return failedAt(__LINE__);
        }
        rules.path_pacing_percent = 110;
        rules.path_burst_us = 100001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("path_burst_us")) {
            return failedAt(__LINE__);
        }
        rules.path_burst_us = 8000;
        /* Parity for lost video: OFF, AUTO or FIXED, 1..100 per 100 fragments. */
        rules.fec_mode = RCS_FEC_FIXED;
        rules.fec_percent = 100;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        rules.fec_mode = RCS_FEC_FIXED + 1;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("fec_mode")) {
            return failedAt(__LINE__);
        }
        rules.fec_mode = RCS_FEC_OFF;
        rules.fec_percent = 0;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("fec_percent")) {
            return failedAt(__LINE__);
        }
        rules.fec_percent = 101;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("fec_percent")) {
            return failedAt(__LINE__);
        }
        rules.fec_percent = 50;
        rules.ref_invalidation = 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("ref_invalidation")) {
            return failedAt(__LINE__);
        }
        rules.ref_invalidation = 1;
        /* Several viewers: keyframes, moves and lower encodings. */
        if (rules.viewer_keyframe_interval_ms != 2000 || rules.loss_keyframes_move != 3 ||
            rules.move_down_ms != 2000 || rules.move_up_ms != 10000 || rules.lower_idle_ms != 10000) {
            return failedAt(__LINE__);
        }
        rules.viewer_keyframe_interval_ms = 99;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("viewer_keyframe_interval_ms")) {
            return failedAt(__LINE__);
        }
        rules.viewer_keyframe_interval_ms = 2000;
        rules.loss_keyframes_move = 61;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("loss_keyframes_move")) {
            return failedAt(__LINE__);
        }
        rules.loss_keyframes_move = 0;
        rules.move_down_ms = 199;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("move_down_ms")) {
            return failedAt(__LINE__);
        }
        rules.move_down_ms = 200;
        rules.move_up_ms = 120001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("move_up_ms")) {
            return failedAt(__LINE__);
        }
        rules.move_up_ms = 1000;
        rules.lower_idle_ms = 999;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("lower_idle_ms")) {
            return failedAt(__LINE__);
        }
        rules.lower_idle_ms = 600000;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
    }
    /* Several viewers (rcs_viewers_config): the defaults, the ranges, the transports that carry them; the
     * functions on one viewer name it by an id the run gave. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_viewers_config viewers = RCS_VIEWERS_CONFIG_INIT;
        rcs_host_transport_config host = RCS_HOST_TRANSPORT_CONFIG_INIT;
        rcs_session_callbacks callbacks = RCS_SESSION_CALLBACKS_INIT;
        rcs_viewer_info list[2] = {RCS_VIEWER_INFO_INIT, RCS_VIEWER_INFO_INIT};
        rcs_network_stats network = RCS_NETWORK_STATS_INIT;
        rcs_data_message message = RCS_DATA_MESSAGE_INIT;
        rcs_input_event input = RCS_INPUT_EVENT_INIT;
        rcs_viewer_decision decision = RCS_VIEWER_DECISION_INIT;
        rcs_session session = NULL;
        rcs_result result = RCS_OK;
        uint32_t viewerCount = 9;
        const uint8_t byte = 1;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        plain.role = RCS_ROLE_SHARE;
        plain.extensions = &viewers.header;
        if (viewers.max_viewers != 0 || viewers.bitrate_from != RCS_BITRATE_FROM_QUORUM ||
            viewers.quorum_percent != 50 || viewers.admission_timeout_ms != 60000 || callbacks.on_viewer_join ||
            list[0].struct_size != sizeof(rcs_viewer_info) || list[0].route || message.viewer_id ||
            input.viewer_id || decision.struct_size != sizeof(decision) || decision.admission != RCS_ADMIT ||
            decision.control || decision.reason[0] || create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* How long a viewer waits for the host's decision: 0 (no limit) or 1 s to an hour. */
        viewers.admission_timeout_ms = 999;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("admission_timeout_ms")) {
            return failedAt(__LINE__);
        }
        viewers.admission_timeout_ms = 3600001;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        viewers.admission_timeout_ms = 0;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        viewers.admission_timeout_ms = 1000;
        viewers.max_viewers = RCS_MAX_VIEWERS + 1;
        if (RCS_MAX_VIEWERS != 256 || create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("max_viewers")) {
            return failedAt(__LINE__);
        }
        viewers.max_viewers = RCS_MAX_VIEWERS;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        viewers.bitrate_from = 3;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("bitrate_from")) {
            return failedAt(__LINE__);
        }
        viewers.bitrate_from = RCS_BITRATE_FROM_MAIN;
        viewers.quorum_percent = 0;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("quorum_percent")) {
            return failedAt(__LINE__);
        }
        viewers.quorum_percent = 101;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("quorum_percent")) {
            return failedAt(__LINE__);
        }
        viewers.quorum_percent = 100;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        /* More encodings of a stream: AUTO with two at most by default; counts, modes and codecs checked. */
        {
            rcs_encodings_config encodings = RCS_ENCODINGS_CONFIG_INIT;
            rcs_viewer_video video = RCS_VIEWER_VIDEO_INIT;
            rcs_group_stats group = RCS_GROUP_STATS_INIT;
            rcs_video_send_stats send = RCS_VIDEO_SEND_STATS_INIT;
            if (encodings.mode != RCS_ENCODINGS_AUTO || encodings.count != 2 || encodings.lower[0].codecs[0] ||
                video.counts_for_bitrate != 1 || video.encoding != RCS_ENCODING_ANY ||
                decision.video.encoding != RCS_ENCODING_ANY || RCS_MAX_ENCODINGS != 3) {
                return failedAt(__LINE__);
            }
            viewers.header.next = &encodings.header;
            if (create(&plain, NULL) != RCS_OK) {
                return failedAt(__LINE__);
            }
            encodings.count = RCS_MAX_ENCODINGS + 1;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("count")) {
                return failedAt(__LINE__);
            }
            encodings.count = 2;
            encodings.mode = RCS_ENCODINGS_FIXED + 1;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("mode")) {
                return failedAt(__LINE__);
            }
            encodings.mode = RCS_ENCODINGS_FIXED;
            encodings.lower[0].codecs[0] = RCS_CODEC_AV1; /* Not among RCS_EXT_ENCODER's codecs. */
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("lower[].codecs[]")) {
                return failedAt(__LINE__);
            }
            encodings.lower[0].codecs[0] = RCS_CODEC_H264;
            encodings.lower[1].max_pixel_rate = 255;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("max_pixel_rate")) {
                return failedAt(__LINE__);
            }
            encodings.lower[1].max_pixel_rate = 0;
            /* Temporal layers: L1T1..L1T3, of the main encoding and of each lower one. */
            if (encodings.scalability != RCS_SCALABILITY_L1T1 ||
                encodings.lower[0].scalability != RCS_SCALABILITY_L1T1) {
                return failedAt(__LINE__);
            }
            encodings.scalability = 0;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("scalability")) {
                return failedAt(__LINE__);
            }
            encodings.scalability = RCS_SCALABILITY_L1T3 + 1;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("scalability")) {
                return failedAt(__LINE__);
            }
            encodings.scalability = RCS_SCALABILITY_L1T3;
            encodings.lower[1].scalability = RCS_SCALABILITY_L1T3 + 1;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("lower[].scalability")) {
                return failedAt(__LINE__);
            }
            encodings.lower[1].scalability = RCS_SCALABILITY_L1T2;
            encodings.lower[0].frames_per_second = 241;
            if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("frames_per_second")) {
                return failedAt(__LINE__);
            }
            encodings.lower[0].frames_per_second = 0;
            if (create(&plain, NULL) != RCS_OK) {
                return failedAt(__LINE__);
            }
            /* How one viewer gets the picture: counts_for_bitrate 0 or 1, layers 0..3, an encoding of the
             * stream's (checked before the viewer is looked for). */
            if (rcs_session_create(&plain, NULL, &session) != RCS_OK) {
                return failedAt(__LINE__);
            }
            video.layers = RCS_SCALABILITY_L1T3 + 1;
            result = rcs_session_set_viewer_video(session, 1, &video);
            keepReason();
            if (result != RCS_ERROR_INVALID_ARGUMENT || !reason("layers")) {
                return failedAt(__LINE__);
            }
            video.layers = 0;
            video.counts_for_bitrate = 2;
            result = rcs_session_set_viewer_video(session, 1, &video);
            keepReason();
            if (result != RCS_ERROR_INVALID_ARGUMENT || !reason("counts_for_bitrate")) {
                return failedAt(__LINE__);
            }
            video.counts_for_bitrate = 1;
            video.encoding = RCS_MAX_ENCODINGS;
            result = rcs_session_set_viewer_video(session, 1, &video);
            keepReason();
            if (result != RCS_ERROR_INVALID_ARGUMENT || !reason("encoding")) {
                return failedAt(__LINE__);
            }
            video.encoding = RCS_ENCODING_ANY;
            rcs_session_destroy(session);
            session = NULL;
            if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
                rcs_session_set_viewer_video(session, 1, &video) != RCS_ERROR_INVALID_ARGUMENT ||
                rcs_session_get_stats(session, RCS_STATS_GROUP, &group) != RCS_OK || group.viewers ||
                rcs_session_get_stats(session, RCS_STATS_VIEWER_VIDEO, &group) != RCS_ERROR_INVALID_ARGUMENT ||
                rcs_session_get_encoding_stats(session, 0, 1, &send) != RCS_ERROR_INVALID_STATE ||
                rcs_session_get_encoding_stats(session, 0, 0, &send) != RCS_OK) {
                return failedAt(__LINE__);
            }
            rcs_session_destroy(session);
            session = NULL;
            viewers.header.next = NULL;
        }
        /* The host's channel carries several, a route each, as P2P and RELAY do. */
        plain.connect.transport = RCS_TRANSPORT_HOST;
        viewers.header.next = &host.header;
        host.on_transport_send = hostSend;
        if (sharerStarts() && started(&plain, &callbacks) != RCS_OK) {
            return failedAt(__LINE__);
        }
        viewers.max_viewers = 0;
        if (sharerStarts() && started(&plain, &callbacks) != RCS_OK) {
            return failedAt(__LINE__);
        }
        viewers.header.next = NULL;
        plain.connect.transport = RCS_TRANSPORT_P2P;
        viewers.max_viewers = 3;
        callbacks.on_signal = signalSink;
        if (sharerStarts() && started(&plain, &callbacks) != RCS_OK) {
            return failedAt(__LINE__);
        }
        if (rcs_session_create(&plain, &callbacks, &session) != RCS_OK ||
            rcs_session_signal(session, "rcs1.CQEIMGExYjJjM2QgMDAxMTIyMzM0NDU1NjY3Nzg4OTlhYWJiY2NkZGVlZmYA", 7) !=
                RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_signal(session, NULL, 7) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        rcs_session_destroy(session);
        session = NULL;
        callbacks.on_signal = NULL;
        plain.connect.transport = RCS_TRANSPORT_UDP;
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_list_viewers(session, NULL, sizeof(rcs_viewer_info), 0, &viewerCount) != RCS_OK ||
            viewerCount != 0 ||
            rcs_session_list_viewers(session, list, sizeof(rcs_viewer_info) - 4, 2, &viewerCount) !=
                RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_get_viewer_stats(session, 1, RCS_STATS_NETWORK, &network) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_disconnect_viewer(session, 1) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_set_viewer_control(session, 1, 2) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_set_main_viewer(session, 1) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_set_main_viewer(session, 0) != RCS_OK ||
            rcs_session_admit_viewer(session, 1, &decision) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_admit_viewer(session, 1, NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_admit_viewer(NULL, 1, &decision) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        /* A decision is to admit or to refuse (LATER is on_viewer_join's), with a reason that ends. */
        decision.admission = RCS_ADMIT_LATER;
        result = rcs_session_admit_viewer(session, 1, &decision);
        keepReason();
        if (result != RCS_ERROR_INVALID_ARGUMENT || !reason("RCS_ADMIT")) {
            return failedAt(__LINE__);
        }
        decision.admission = RCS_REFUSE;
        memset(decision.reason, 'x', sizeof(decision.reason));
        result = rcs_session_admit_viewer(session, 1, &decision);
        keepReason();
        if (result != RCS_ERROR_INVALID_ARGUMENT || !reason("reason")) {
            return failedAt(__LINE__);
        }
        decision.reason[0] = '\0';
        decision.struct_size = 4;
        if (rcs_session_admit_viewer(session, 1, &decision) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        decision.struct_size = sizeof(decision);
        rcs_session_destroy(session);
        /* A viewing session has no viewers, and sends with viewer_id 0. */
        plain.role = RCS_ROLE_VIEW;
        plain.extensions = NULL;
        message.viewer_id = 1;
        message.data = &byte;
        message.size = 1;
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_list_viewers(session, NULL, sizeof(rcs_viewer_info), 0, &viewerCount) !=
                RCS_ERROR_UNSUPPORTED ||
            rcs_session_disconnect_viewer(session, 1) != RCS_ERROR_UNSUPPORTED ||
            rcs_session_admit_viewer(session, 1, &decision) != RCS_ERROR_UNSUPPORTED ||
            rcs_session_send_data(session, &message) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        rcs_session_destroy(session);
        if (strcmp(rcs_result_message(RCS_ERROR_BUSY), "Unknown result") == 0) {
            return failedAt(__LINE__);
        }
    }
    if (strcmp(rcs_result_message(RCS_ERROR_DISCONNECTED), "Unknown result") == 0) {
        return failedAt(__LINE__);
    }
    if (strcmp(rcs_result_message(RCS_ERROR_DEVICE), "Unknown result") == 0) {
        return failedAt(__LINE__);
    }
    /* What libraries built on the SDK use: their records go where the SDK's go, under its rules,
     * and their functions say why they fail as the SDK's do. */
    {
        rcs_log_record record = {0};
        char text[64];
        int tag = 3;
        record.struct_size = sizeof(record);
        record.level = RCS_LOG_INFO;
        record.module = "library";
        record.message = "written";
        if (rcs_log_write(&record) != RCS_OK || rcs_log_in_callback() ||
            rcs_set_log_callback(RCS_LOG_INFO, onRecord, NULL) != RCS_OK || written.count) {
            return failedAt(__LINE__); /* No callback yet: dropped. */
        }
        if (rcs_log_write(&record) != RCS_OK || written.count != 1 || !written.last.timestamp_ns ||
            strcmp(written.module, "library") != 0 || strcmp(written.message, "written") != 0 ||
            written.last.source != RCS_LOG_SOURCE_PROCESS || !written.inCallback ||
            written.writtenThere != RCS_OK) {
            return failedAt(__LINE__); /* The nested record was dropped: one call, not two. */
        }
        record.level = RCS_LOG_DEBUG; /* Above max_level. */
        if (rcs_log_write(&record) != RCS_OK || written.count != 1) {
            return failedAt(__LINE__);
        }
        record.level = RCS_LOG_WARNING;
        record.source = RCS_LOG_SOURCE_SUPERVISOR; /* A supervisor names its handle. */
        if (rcs_log_write(&record) != RCS_ERROR_INVALID_ARGUMENT || written.count != 1) {
            return failedAt(__LINE__);
        }
        keepReason();
        if (!reason("handle")) {
            return failedAt(__LINE__);
        }
        record.supervisor = (rcs_supervisor)&tag;
        record.user_data = &tag;
        record.timestamp_ns = 5;
        if (rcs_log_write(&record) != RCS_OK || written.count != 2 || written.last.supervisor != record.supervisor ||
            written.last.user_data != &tag || written.last.timestamp_ns != 5) {
            return failedAt(__LINE__);
        }
        record.source = 9;
        if (rcs_log_write(&record) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        record.source = RCS_LOG_SOURCE_PROCESS;
        record.supervisor = NULL;
        record.message = NULL;
        if (rcs_log_write(&record) != RCS_ERROR_INVALID_ARGUMENT || rcs_log_write(NULL) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_set_log_callback(RCS_LOG_INFO, NULL, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        if (rcs_set_last_call_error(RCS_ERROR_BUSY, "the library's reason") != RCS_ERROR_BUSY ||
            rcs_get_last_call_error(text, sizeof(text)) != RCS_OK || strcmp(text, "the library's reason") != 0 ||
            rcs_set_last_call_error(RCS_OK, "ignored") != RCS_OK ||
            rcs_get_last_call_error(text, sizeof(text)) != RCS_OK || text[0]) {
            return failedAt(__LINE__);
        }
    }
    /* The application's audio session: iOS has one; values are checked first everywhere. */
    {
        rcs_audio_session_config audio = RCS_AUDIO_SESSION_CONFIG_INIT;
        rcs_capabilities caps = RCS_CAPABILITIES_INIT;
        const int ios = rcs_get_capabilities(&caps) == RCS_OK && caps.platform == RCS_PLATFORM_IOS;
        audio.manager = 3;
        if (rcs_set_audio_session_config(&audio) != RCS_ERROR_INVALID_ARGUMENT || (keepReason(), !reason("manager"))) {
            return failedAt(__LINE__);
        }
        audio.manager = RCS_AUDIO_SESSION_HOST;
        audio.mix_with_others = 2;
        if (rcs_set_audio_session_config(&audio) != RCS_ERROR_INVALID_ARGUMENT ||
            (keepReason(), !reason("mix_with_others"))) {
            return failedAt(__LINE__);
        }
        audio.mix_with_others = 0;
        audio.struct_size = 4;
        if (rcs_set_audio_session_config(&audio) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_get_audio_session_config(&audio) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
        audio.struct_size = sizeof(audio);
        if (ios) {
            rcs_audio_session_config now = RCS_AUDIO_SESSION_CONFIG_INIT;
            if (rcs_get_audio_session_config(&now) != RCS_OK || now.manager != RCS_AUDIO_SESSION_SDK ||
                rcs_set_audio_session_config(&audio) != RCS_OK || rcs_get_audio_session_config(&now) != RCS_OK ||
                now.manager != RCS_AUDIO_SESSION_HOST) {
                return failedAt(__LINE__);
            }
            audio.manager = RCS_AUDIO_SESSION_SDK;
            if (rcs_set_audio_session_config(&audio) != RCS_OK) {
                return failedAt(__LINE__);
            }
        } else if (rcs_set_audio_session_config(&audio) != RCS_ERROR_UNSUPPORTED ||
                   (keepReason(), !reason("iOS")) || rcs_get_audio_session_config(&audio) != RCS_ERROR_UNSUPPORTED) {
            return failedAt(__LINE__);
        }
    }
    /* Pausing, at any state, with the bits this version defines; both roles. */
    {
        rcs_session_config plain = RCS_SESSION_CONFIG_INIT;
        rcs_background_config background = RCS_BACKGROUND_CONFIG_INIT;
        rcs_session session = NULL;
        memset(plain.identity.key, 7, sizeof(plain.identity.key));
        if (background.pause != RCS_PAUSE_VIDEO || rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_pause(session, RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO) != RCS_OK ||
            rcs_session_resume(session, RCS_PAUSE_AUDIO) != RCS_OK ||
            rcs_session_pause(session, 1u << 2) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_resume(NULL, RCS_PAUSE_VIDEO) != RCS_ERROR_INVALID_ARGUMENT ||
            rcs_session_destroy(session) != RCS_OK) {
            return failedAt(__LINE__);
        }
        plain.extensions = &background.header;
        background.pause = 1u << 2;
        if (create(&plain, NULL) != RCS_ERROR_INVALID_ARGUMENT || !reason("pause")) {
            return failedAt(__LINE__);
        }
        background.pause = 0;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        plain.role = RCS_ROLE_SHARE;
        if (create(&plain, NULL) != RCS_OK) {
            return failedAt(__LINE__);
        }
        plain.extensions = NULL;
        if (rcs_session_create(&plain, NULL, &session) != RCS_OK ||
            rcs_session_pause(session, RCS_PAUSE_VIDEO) != RCS_OK || rcs_session_destroy(session) != RCS_OK) {
            return failedAt(__LINE__);
        }
    }
    printf("C interface compiles as C and links; %u codec implementations\n", count);
    for (i = 0; i < count; ++i) {
        printf("  %s %s codec %u: %s\n", implementations[i].direction == RCS_CODEC_ENCODER ? "encoder" : "decoder",
               implementations[i].acceleration == RCS_ACCELERATION_HARDWARE ? "hardware" : "software",
               implementations[i].codec, implementations[i].name);
    }
    return 0;
}
