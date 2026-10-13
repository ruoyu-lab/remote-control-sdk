/* Remote Control SDK: the host's own codecs and encoded packets.
 *
 * For hosts that encode or decode themselves (RCS_EXT_HOST_ENCODER,
 * RCS_EXT_HOST_DECODER), push their own bitstream (capture source ENCODED),
 * or take the encoded packets on either end (RCS_EXT_ENCODED_OUTPUT). */
#ifndef RCSDK_HOST_CODEC_H
#define RCSDK_HOST_CODEC_H

#include <rcsdk/tuning.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An encoder or decoder of the host's takes over one end's encoding or
 * decoding of the codecs it lists, while the SDK keeps everything else:
 * negotiation, transport, encryption, adaptive bitrate, keyframe requests and
 * loss recovery, statistics and events. Standard codecs keep their bitstream
 * format (rcs_encoded_packet), so either end may be the host's and the other
 * the system's; host codec ids (RCS_CODEC_HOST_FIRST..LAST) need the host's on
 * both ends. Both are fixed for a run. Events and statistics name the host's
 * codec by the name it gives, with backend RCS_BACKEND_HOST. */

/* What an open host encoder hands its packets to: from open until close
 * returns. */
typedef struct rcs_encoder_output_impl* rcs_encoder_output;

typedef struct rcs_host_encoder_settings {
    uint32_t struct_size;
    uint32_t stream;           /* Which stream it encodes (rcs_streams_config); 0 without it... */
    uint32_t encoding;         /* ...and which of its encodings (RCS_EXT_ENCODINGS; 0 the main one). */
    rcs_codec codec;           /* The negotiated codec... */
    rcs_picture_format format; /* ...and picture format, one of picture_formats, */
    rcs_color color;           /* and how the packets' pictures are written (rcs_encoder_config.matrix, range). */
    uint32_t width, height, frames_per_second;
    uint32_t bitrate_bps;      /* The first target; set_bitrate changes it. */
    rcs_encoder_speed speed;   /* rcs_video_tuning_config.speed: the host's encoder follows it as it can. */
    /* The temporal layers to make (the encoding's rcs_scalability, at most
     * rcs_host_encoder_config.temporal_layers): 1 none; 2 or 3, each packet's
     * temporal_id below it, a picture of layer t referring to pictures of
     * layers up to t only, and nothing before the latest picture of layer 0
     * but pictures of layer 0 (a viewer may take more layers from any picture
     * of layer 0 on). */
    uint32_t temporal_layers;
    void* d3d11_device;        /* Windows, GPU pictures: the ID3D11Device they live on; otherwise NULL. */
    rcs_encoder_output output; /* Where the packets go (rcs_encoder_output_push). */
} rcs_host_encoder_settings;

/* A picture to encode, borrowed for the duration of encode: on the GPU
 * (gpu_frame, as on_gpu_frame gets them; rcs_gpu_frame_get_info says its
 * layout as captured: BGRA8 on Windows, on macOS the YCbCr layout of the
 * stream's format; retain it to keep it beyond the call,
 * e.g. in an asynchronous encoder: the SDK does not reuse a retained picture),
 * or on the CPU (cpu_frame, BGRA8): the other is NULL. */
typedef struct rcs_host_encoder_frame {
    uint32_t struct_size;
    uint32_t stream;   /* As opened... */
    uint32_t encoding; /* ...both. */
    uint64_t sequence; /* The packet of this picture carries the same. */
    uint64_t timestamp_ns;
    uint32_t keyframe; /* Encode it as a keyframe: a stream starts, the viewer or host asked, the interval passed. */
    rcs_gpu_frame gpu_frame;
    const rcs_frame* cpu_frame;
} rcs_host_encoder_frame;

/* The host's encoder (share; an agent in another process takes it in
 * rcs_agent_config, where the encoding happens). Each stream
 * (rcs_streams_config) opens one of its own, and each lower encoding of a
 * stream another (RCS_EXT_ENCODINGS: the same pictures at another frame rate
 * or size): the calls name the stream and the encoding, and those of one
 * encoding come from its encoding thread, one at a time, while different
 * ones' may overlap:
 *   open         when encoding starts, and again (after close) when the
 *                codec or the size changes; settings->output takes the packets.
 *   encode       each picture, at most three of them unanswered at a time:
 *                packets must not wait for later pictures (no lookahead, no
 *                B-frames).
 *   set_bitrate  adaptive bitrate's new target; anything but RCS_OK keeps the
 *                previous one (logged once). NULL: the bitrate opened with
 *                stays; the SDK still skips pictures while the output is over
 *                the target.
 *   close        encoding stops; no packet may be pushed afterwards.
 * Packets come back through rcs_encoder_output_push, during encode or later
 * from any thread (an asynchronous encoder): the packet of picture N tells
 * the SDK that N and the pictures before it are done, so a picture the
 * encoder drops needs no packet. An error from open or encode ends the run
 * with that result; the reason names the encoder. An encoder that fails
 * asynchronously returns the error from its next encode. */
typedef struct rcs_host_encoder_config {
    rcs_extension header;
    rcs_codec codecs[RCS_MAX_CODECS]; /* The codecs it handles; at least one, no duplicates. */
    /* The picture formats it encodes, as (1u << rcs_picture_format) bits: SDR
     * ones (an HDR picture's layout for the host is not defined yet: HDR is
     * UNSUPPORTED). What the session offers in its codecs. */
    uint32_t picture_formats;
    uint32_t cpu_images;              /* 1: always BGRA8 on the CPU (GPU pictures are read back); 0: as captured. */
    rcs_acceleration acceleration;    /* Reported: whether it encodes in hardware. */
    uint32_t low_latency;             /* Reported: whether it encodes one picture at a time. */
    uint32_t temporal_layers;         /* The most temporal layers it makes, 1..3 (1: none). */
    char name[64];                    /* UTF-8, not empty: in events, statistics and reasons. */
    void* user_data;
    rcs_result (*open)(const rcs_host_encoder_settings* settings, void* user_data);
    rcs_result (*encode)(const rcs_host_encoder_frame* frame, void* user_data);
    rcs_result (*set_bitrate)(uint32_t stream, uint32_t encoding, uint32_t bitrate_bps, void* user_data);
    void (*close)(uint32_t stream, uint32_t encoding, void* user_data);
} rcs_host_encoder_config;
#define RCS_HOST_ENCODER_CONFIG_INIT                                                                                   \
    {{RCS_EXT_HOST_ENCODER, sizeof(rcs_host_encoder_config), NULL},                                                    \
     {0},                                                                                                              \
     1u << RCS_PICTURE_420_8,                                                                                          \
     0u,                                                                                                               \
     RCS_ACCELERATION_HARDWARE,                                                                                        \
     1u,                                                                                                               \
     1u,                                                                                                               \
     {0},                                                                                                              \
     NULL,                                                                                                             \
     NULL,                                                                                                             \
     NULL,                                                                                                             \
     NULL,                                                                                                             \
     NULL}
RCS_INLINE rcs_host_encoder_config rcs_host_encoder_config_init(void) {
    rcs_host_encoder_config value = RCS_HOST_ENCODER_CONFIG_INIT;
    return value;
}

/* A packet of the host's encoder, copied. Standard codecs: the format of
 * rcs_encoded_packet; a keyframe carries config, other packets may leave it
 * out (the SDK adds the current one). Host codecs: any bytes, config optional.
 * codec, format, color, width and height are the ones opened (stream and
 * encoding are taken as opened); temporal_id (below the layers opened,
 * rcs_host_encoder_settings.temporal_layers) and discardable as
 * rcs_encoded_packet says. INVALID_ARGUMENT says what is
 * wrong with the packet (rcs_get_last_call_error). Only between open and
 * close's return: the output does not exist outside them. */
RCSDK_API rcs_result rcs_encoder_output_push(rcs_encoder_output output, const rcs_encoded_packet* packet);

typedef struct rcs_host_decoder_settings {
    uint32_t struct_size;
    uint32_t stream; /* Which stream it decodes (rcs_streams_config); 0 without it. */
    rcs_codec codec;
    rcs_picture_format format; /* One of picture_formats... */
    rcs_color color;           /* ...and what its values mean. */
    uint32_t width, height;    /* The stream's size, from its first keyframe. */
} rcs_host_decoder_settings;

/* The host's decoder (view). It decodes and shows the pictures itself:
 * on_frame and on_gpu_frame are not called for the codecs it handles. Each
 * stream (rcs_streams_config) opens one of its own: the calls name the
 * stream, and those of one stream come from its decoding thread, one at a
 * time, while different streams' may overlap:
 *   open    at the stream's first keyframe, and again (after close) when the
 *           codec, the size or the decoder settings change.
 *   decode  each packet in order, after the SDK's queue (queue_frames,
 *           max_queue_age_ms) and loss recovery: a stream resumes at a
 *           keyframe. RCS_OK: taken. An error: the SDK closes and opens it
 *           again and goes on at the next keyframe (RCS_EVENT_RECOVERY,
 *           RCS_RECOVERY_DECODER); failing again before it took a packet ends
 *           the run with that result.
 *   reset   packets were dropped: forget the state; the next is a keyframe.
 *   close   decoding stops.
 * decoded_frames counts the packets it took. */
typedef struct rcs_host_decoder_config {
    rcs_extension header;
    rcs_codec codecs[RCS_MAX_CODECS]; /* The codecs it handles; at least one, no duplicates. */
    uint32_t picture_formats;         /* The picture formats it decodes, as (1u << rcs_picture_format) bits. */
    rcs_acceleration acceleration;    /* Reported: whether it decodes in hardware. */
    char name[64];                    /* UTF-8, not empty. */
    void* user_data;
    rcs_result (*open)(const rcs_host_decoder_settings* settings, void* user_data);
    rcs_result (*decode)(const rcs_encoded_packet* packet, void* user_data);
    void (*reset)(uint32_t stream, void* user_data);
    void (*close)(uint32_t stream, void* user_data);
} rcs_host_decoder_config;
#define RCS_HOST_DECODER_CONFIG_INIT                                                                                   \
    {{RCS_EXT_HOST_DECODER, sizeof(rcs_host_decoder_config), NULL},                                                    \
     {0},                                                                                                              \
     1u << RCS_PICTURE_420_8,                                                                                          \
     RCS_ACCELERATION_HARDWARE,                                                                                        \
     {0},                                                                                                              \
     NULL,                                                                                                             \
     NULL,                                                                                                             \
     NULL,                                                                                                             \
     NULL,                                                                                                             \
     NULL}
RCS_INLINE rcs_host_decoder_config rcs_host_decoder_config_init(void) {
    rcs_host_decoder_config value = RCS_HOST_DECODER_CONFIG_INIT;
    return value;
}


/* Share with capture source ENCODED: the next packet, copied, in order (from
 * one thread at a time). Its codec, format, color, width and height are the stream's
 * (rcs_stream_info; RCS_EVENT_STREAM when they change); the bitstream as for
 * rcs_encoder_output_push. INVALID_ARGUMENT says what is wrong with it;
 * INVALID_STATE while no stream runs (no viewer yet: a stream starts with
 * RCS_EVENT_KEYFRAME) and for a stream's first packet that is no keyframe.
 * The SDK sends every packet it took. */
RCSDK_API rcs_result rcs_session_push_encoded_video(rcs_session session, const rcs_encoded_packet* packet);

/* Share with capture source ENCODED: RCS_OK when a new picture may go now;
 * QUEUE_FULL while the path still holds about two pictures of video, or the
 * bitrate's budget is spent: skip encoding this picture, as the SDK's own
 * capture does. INVALID_STATE while no stream runs. */
RCSDK_API rcs_result rcs_session_video_ready(rcs_session session);

/* Compressed sound, borrowed for the duration of on_audio: one packet,
 * 48 kHz stereo. OPUS: one Opus packet (RFC 6716). AAC: one raw AAC-LC access
 * unit, no ADTS header; config holds the AudioSpecificConfig. PCM: 16-bit
 * little-endian samples, channels interleaved. sequence counts the packets of
 * one format from 0 (it starts again when the sound restarts): a gap is lost
 * sound. timestamp_ns is when its first sample was captured, on the sending
 * end's monotonic clock (the sharer's: the one of the pictures'
 * rcs_encoded_packet.timestamp_ns). */
typedef struct rcs_encoded_audio {
    uint32_t struct_size;
    /* SENDING: this end's, before it is sent; RECEIVING: the other end's, as
     * it arrived (RCS_EXT_RECEIVE_AUDIO, rcs_send_audio_config). */
    rcs_audio_direction direction;
    rcs_viewer_id viewer_id; /* Share: the viewer it goes to or comes from. View: 0. */
    rcs_audio_codec codec;
    uint32_t sample_rate; /* 48000 */
    uint32_t channels;    /* 2 */
    uint32_t frames;      /* Sample frames it decodes to. */
    uint64_t sequence;
    uint64_t timestamp_ns;
    const uint8_t* data;
    uint32_t data_size;
    const uint8_t* config; /* AAC: the AudioSpecificConfig; otherwise NULL. */
    uint32_t config_size;
} rcs_encoded_audio;
#define RCS_ENCODED_AUDIO_INIT {sizeof(rcs_encoded_audio), RCS_AUDIO_SENDING, 0, 0, 48000u, 2u, 0, 0, 0, NULL, 0, NULL, 0}
RCS_INLINE rcs_encoded_audio rcs_encoded_audio_init(void) {
    rcs_encoded_audio value = RCS_ENCODED_AUDIO_INIT;
    return value;
}

/* The encoded packets, to the host (to record, forward or audit what is
 * shared without encoding it again), on either end; fixed for a run. At
 * least one of on_video and on_audio. A packet is valid during the call:
 * copy what is kept. Both kinds carry the sharer's capture time on its
 * clock, so pictures and sound line up as they were captured. The two come
 * from different threads, so their calls may overlap (so may a sharer's
 * on_audio for different viewers); each kind comes in order.
 *
 * Share: on_video gets every video packet of the encodings it asks for
 * (encodings) before it is sent, on the encoder's thread (one encoding serves
 * its viewers; each stream and encoding has its own thread,
 * rcs_encoded_packet.stream and encoding). on_audio gets every
 * sound packet before it is sent, on the thread that sends that viewer's
 * sound: each viewer's sound is encoded for it in the codec it negotiated,
 * so every viewer that receives sound has packets of its own (viewer_id).
 * decode is a viewer's; a sharer ignores it.
 *
 * View: on_video gets each video packet received, before it is decoded and
 * in the order decoding takes them, on the decoding thread (each stream's its
 * own; never the connection's: a slow on_video holds up the picture, and the
 * receive queue then drops what waited too long,
 * rcs_video_tuning_config.queue_frames and max_queue_age_ms, resuming at a
 * keyframe with after_gap; when the sharer moves this end to another
 * encoding, the packets go on from that one's keyframe, maybe in another
 * codec or size: rcs_encoded_packet.encoding). on_audio gets each
 * sound packet received, before it is decoded, on the sound thread: in
 * order, leaving out, as decoding does, one that came after later ones and
 * those a stalled thread dropped. To start a file at a keyframe the host asks
 * for one (rcs_session_request_keyframe).
 *   decode 1  Decode and deliver as usual besides (the default).
 *   decode 0  The packets only, e.g. to record without watching: no video
 *             decoder opens and no sound is decoded or played, so on_frame,
 *             on_gpu_frame and on_audio (rcs_session_callbacks) must stay
 *             NULL, there is no RCS_EVENT_CODEC, and on_video is required.
 *             The offer is what the host stores: rcs_decoder_config's codecs
 *             and formats as listed, whatever this system decodes, each at
 *             the largest picture its codec allows (a host codec id needs no
 *             RCS_EXT_HOST_DECODER, and there must be none), and
 *             rcs_receive_audio_config's codecs; its enabled still turns the
 *             sound on and off, and playback does nothing.
 * Recording someone is the host's decision, and so is telling them: the SDK
 * shows and sends nothing about it. */
typedef struct rcs_encoded_output_config {
    rcs_extension header;
    uint32_t decode; /* View: 1 or 0, see above. */
    /* Share: the encodings on_video gets (RCS_EXT_ENCODINGS), as (1u <<
     * encoding) bits, below 1u << RCS_MAX_ENCODINGS; default 1u, the main
     * encoding's alone. A view ignores it. */
    uint32_t encodings;
    void* user_data;
    void (*on_video)(const rcs_encoded_packet* packet, void* user_data);
    void (*on_audio)(const rcs_encoded_audio* packet, void* user_data);
} rcs_encoded_output_config;
#define RCS_ENCODED_OUTPUT_CONFIG_INIT                                                                                 \
    {{RCS_EXT_ENCODED_OUTPUT, sizeof(rcs_encoded_output_config), NULL}, 1u, 1u, NULL, NULL, NULL}
RCS_INLINE rcs_encoded_output_config rcs_encoded_output_config_init(void) {
    rcs_encoded_output_config value = RCS_ENCODED_OUTPUT_CONFIG_INIT;
    return value;
}

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_HOST_CODEC_H */
