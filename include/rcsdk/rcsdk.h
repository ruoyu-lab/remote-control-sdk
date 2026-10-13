/*
 * Remote Control SDK - public C interface.
 *
 * A session needs only its role and network settings; everything else has a
 * working default and is customised by chaining optional extension structures
 * to the configuration (rcs_extension). Initialise every structure with its
 * *_INIT macro, or the function of the same name in lower case (RCS_X_INIT:
 * rcs_x_init()) where macros do not reach, as in Swift. All functions are
 * thread-safe unless documented otherwise.
 *
 * The SDK never substitutes a capability silently: no hardware-to-software
 * codec switch, no codec the host did not list, no CPU image the host did not
 * ask for. What the configuration asks for is used, or the call fails with a
 * result the host can act on (see rcs_get_last_call_error and
 * rcs_session_get_failure for the reason). Where the SDK adapts on its own,
 * as the macOS encoder does, it reports the change as an event.
 *
 * Versions: until 1.0 any release may change this interface and the wire
 * protocol, without keeping the old ones. Build against the headers of the
 * library you ship, and run the same version on both ends: a peer speaking
 * another protocol version is refused. Each structure begins with struct_size
 * (extensions: in their rcs_extension header), and the library accepts only
 * the sizes of its own headers, so a structure from other headers fails with
 * RCS_ERROR_INVALID_ARGUMENT instead of being misread.
 */
#ifndef RCSDK_RCSDK_H
#define RCSDK_RCSDK_H

#include <stddef.h>
#include <stdint.h>

#if defined(RCSDK_STATIC)
#define RCSDK_API
#elif defined(_WIN32)
#if defined(RCSDK_BUILDING_CORE)
#define RCSDK_API __declspec(dllexport)
#else
#define RCSDK_API __declspec(dllimport)
#endif
#else
#define RCSDK_API __attribute__((visibility("default")))
#endif

/* The initialisers' functions (rcs_x_init), defined in the headers. */
#if defined(_MSC_VER) && !defined(__cplusplus)
#define RCS_INLINE static __inline
#else
#define RCS_INLINE static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The version of these headers. The build reads it from here. */
#define RCSDK_VERSION_MAJOR 0u
#define RCSDK_VERSION_MINOR 1u
#define RCSDK_VERSION_PATCH 0u
/* Before a release: its stage ("alpha", "beta", "rc1"); empty in a release. */
#define RCSDK_VERSION_LABEL "beta"
#define RCSDK_VERSION ((RCSDK_VERSION_MAJOR << 16) | (RCSDK_VERSION_MINOR << 8) | RCSDK_VERSION_PATCH)

/* ------------------------------------------------------------------------ */
/* Results                                                                  */
/* ------------------------------------------------------------------------ */

typedef int32_t rcs_result;
enum {
    RCS_OK = 0,
    /* The call */
    RCS_ERROR_INVALID_ARGUMENT = -1, /* A value no version defines, or out of range; the reason says which. */
    RCS_ERROR_INVALID_STATE = -2,    /* Not in the object's current state (e.g. not running). */
    RCS_ERROR_UNSUPPORTED = -3,      /* This system, or this role, cannot do it. */
    RCS_ERROR_CALLBACK_THREAD = -4,  /* A lifecycle call from one of the object's own callbacks. */
    RCS_ERROR_WRONG_THREAD = -5,     /* Must be called on the application's main thread. */
    RCS_ERROR_BUFFER_TOO_SMALL = -6, /* A list or text buffer cannot hold the result. */
    RCS_ERROR_QUEUE_FULL = -7,       /* A send queue cannot take more now; the session says when it can. */
    RCS_ERROR_TIMEOUT = -8,
    RCS_ERROR_CANCELLED = -9,
    RCS_ERROR_INTERNAL = -10,
    /* The system */
    RCS_ERROR_PERMISSION_REQUIRED = -11,
    RCS_ERROR_CAPTURE = -12,
    RCS_ERROR_CODEC = -13,
    /* A system resource is missing, held by another program or failed: an
     * audio device, the clipboard (a module's, e.g. rcsdk/clipboard.h). */
    RCS_ERROR_DEVICE = -14,
    /* The peer and the path to it */
    RCS_ERROR_NETWORK = -15,
    RCS_ERROR_AUTHENTICATION = -16,
    RCS_ERROR_PROTOCOL = -17,     /* The peer sent something this version does not accept. */
    RCS_ERROR_DISCONNECTED = -18, /* The peer closed the connection: it stopped, or its process ended. */
    RCS_ERROR_RESTARTING = -19,   /* The peer closed the connection and said it will be back (RCS_STOP_RESTART). */
    /* The sharer serves as many viewers as it takes, or cannot serve this one
     * beside those it serves (rcs_viewers_config); the reason says which. */
    RCS_ERROR_BUSY = -20,
    RCS_ERROR_REPLACED = -21, /* Another agent took this one's place at its session (rcs_agent). */
    /* The sharer's host did not let this viewer in (on_viewer_join,
     * rcs_session_admit_viewer), or did not decide in time, or did not let it
     * do what it asked (a module's, e.g. rcsdk/power.h); the message gives its
     * reason. */
    RCS_ERROR_REFUSED = -22
};

/* ------------------------------------------------------------------------ */
/* Enumerations                                                             */
/* ------------------------------------------------------------------------ */

typedef uint32_t rcs_role;
enum { RCS_ROLE_SHARE = 1, RCS_ROLE_VIEW = 2 };

typedef uint32_t rcs_state;
enum {
    RCS_STATE_IDLE = 0,
    RCS_STATE_STARTING = 1,
    RCS_STATE_LISTENING = 2,    /* Share: waiting for a viewer to connect; again after one left (rcs_reconnect_config). */
    RCS_STATE_CONNECTING = 3,   /* View: connecting to the sharer. */
    RCS_STATE_RUNNING = 4,
    RCS_STATE_RECONNECTING = 5, /* View: the path broke; connecting again (rcs_reconnect_config). */
    RCS_STATE_STOPPED = 6,
    RCS_STATE_FAILED = 7
};

/* Video formats, by family: ITU-T and MPEG's (H.264, HEVC), then the VP and
 * AV1 line (VP8, VP9, AV1). These are the standard ones, with the bitstreams
 * rcs_encoded_packet describes: either end may use the system's codec, one of
 * the codec libraries built into the SDK (rcs_backend) or the host's own
 * (RCS_EXT_HOST_ENCODER, RCS_EXT_HOST_DECODER). VP8 is 4:2:0 8-bit only and
 * follows the bitrate less closely than VP9: for peers that have nothing
 * else. HOST_FIRST..HOST_LAST are formats of the hosts' own: the SDK
 * negotiates and carries them as opaque bytes, both ends need a host codec
 * that handles them, and what each means is for the hosts to agree on. 6..15
 * are kept for standard formats to come. Codec lists hold up to
 * RCS_MAX_CODECS. */
typedef uint32_t rcs_codec;
enum {
    RCS_CODEC_H264 = 1,
    RCS_CODEC_HEVC = 2,
    RCS_CODEC_VP8 = 3,
    RCS_CODEC_VP9 = 4,
    RCS_CODEC_AV1 = 5,
    RCS_CODEC_HOST_FIRST = 16,
    RCS_CODEC_HOST_LAST = 31
};
#define RCS_MAX_CODECS 8

/* Which codec implementations a session uses.
 *   HARDWARE        GPU / media engine codecs only.
 *   SOFTWARE        CPU codecs only: the system's and the libraries built
 *                   into the SDK (rcs_backend), e.g. machines without a usable
 *                   GPU, or formats no hardware here takes.
 *   HARDWARE_FIRST  For each stream, the hardware implementation where one
 *                   takes its codec, picture format and size, else the
 *                   software one, for encoders and decoders alike: e.g. VP9
 *                   4:4:4 where the GPU encodes VP9 4:2:0 only, HEVC 4:4:4 on
 *                   a Windows machine whose GPUs decode it with neither
 *                   NVDEC nor oneVPL (RCS_BACKEND_NVDEC, ONEVPL), where only
 *                   Windows' software decoder decodes it, or any codec on a
 *                   machine without hardware encoders. The end offers what
 *                   either takes. A sharer's hardware is the GPU driving the
 *                   shared display, as with HARDWARE. Chosen when a stream
 *                   starts (RCS_EVENT_CODEC says which); one that fails is
 *                   never replaced by the other.
 * Nothing switches between hardware and software by itself. */
typedef uint32_t rcs_acceleration;
enum {
    RCS_ACCELERATION_HARDWARE = 0,
    RCS_ACCELERATION_SOFTWARE = 1,
    RCS_ACCELERATION_HARDWARE_FIRST = 2
};

/* What a codec implementation runs on: an interface of the system's, or a
 * codec library built into the SDK. Independent of the codec and of the
 * acceleration: one backend may offer hardware and software implementations
 * of several codecs, and one GPU may be reachable through several backends
 * (an NVIDIA GPU: NVENC, NVDEC and Media Foundation; an Intel GPU: oneVPL
 * and Media Foundation, for encoding and decoding). */
typedef uint32_t rcs_backend;
enum {
    /* The SDK picks: for each codec, the implementation
     * rcs_list_codec_implementations lists first for the GPU in question
     * (encoding on an NVIDIA GPU, NVENC; on an Intel GPU, oneVPL; on others,
     * Media Foundation); with HARDWARE_FIRST, a stream whose picture format
     * that hardware decoder does not take, the first listed that does
     * (Windows: HEVC 4:4:4 on NVDEC, else on oneVPL; VP9 4:4:4 on oneVPL). */
    RCS_BACKEND_AUTO = 0,
    /* The system's codecs. */
    RCS_BACKEND_MEDIA_FOUNDATION = 1, /* Windows: Media Foundation transforms on Direct3D 11 (any GPU's). */
    RCS_BACKEND_NVENC = 2,            /* Windows, encoding: NVIDIA's encoder interface, driver 522.25 or later. */
    RCS_BACKEND_VIDEOTOOLBOX = 3,     /* macOS and iOS. */
    RCS_BACKEND_MEDIACODEC = 4,       /* Android: the system's MediaCodec (NDK AMediaCodec). */
    RCS_BACKEND_WEBCODECS = 5,        /* A browser, decoding: its WebCodecs VideoDecoder. */
    RCS_BACKEND_NVDEC = 6,            /* Windows, decoding: NVIDIA's decoder interface, for 4:4:4 (HEVC). */
    RCS_BACKEND_ONEVPL = 7,           /* Windows: oneVPL, the Intel graphics driver's runtime. */
    /* Codec libraries built into the SDK: software, the same on every
     * system (not in a browser). A build may leave one out (RCSDK_WITH_LIBVPX
     * and the like): its implementations are then not listed, and choosing
     * its backend is UNSUPPORTED with that reason. */
    RCS_BACKEND_OPENH264 = 8,         /* H.264 (Cisco's OpenH264): built in where the system has no H.264 of its own. */
    RCS_BACKEND_LIBVPX = 9,           /* VP8 and VP9 (the WebM project's libvpx). */
    RCS_BACKEND_LIBAOM = 10,          /* AV1 encoding (AOMedia's libaom, real-time mode). */
    RCS_BACKEND_DAV1D = 11,           /* AV1 decoding (VideoLAN's dav1d). */
    RCS_BACKEND_HOST = 12             /* Reported only: the host's own codec (RCS_EXT_HOST_ENCODER / _DECODER). */
};

typedef uint32_t rcs_codec_direction;
enum { RCS_CODEC_ENCODER = 1, RCS_CODEC_DECODER = 2 };

typedef uint32_t rcs_permission;
enum { RCS_PERMISSION_UNAVAILABLE = 0, RCS_PERMISSION_REQUIRED = 1, RCS_PERMISSION_GRANTED = 2 };

typedef uint32_t rcs_platform;
/* IOS: iPadOS too. WEB: the SDK compiled to WebAssembly, in a browser.
 * UNKNOWN: a system without a platform layer of the SDK's. */
enum {
    RCS_PLATFORM_UNKNOWN = 0,
    RCS_PLATFORM_MACOS = 1,
    RCS_PLATFORM_WINDOWS = 2,
    RCS_PLATFORM_IOS = 3,
    RCS_PLATFORM_ANDROID = 4,
    RCS_PLATFORM_WEB = 5
};

/* Audio formats. All are 48 kHz stereo. OPUS (libopus, built into
 * the SDK) is the default: packets as long as capture delivers sound (2.5 to
 * 20 ms, see capture_period_frames), about 2.5 ms of codec delay, packet
 * loss concealment. AAC is AAC-LC from the system's codec (about 21 ms
 * packets, 40-60 ms of codec delay). PCM is 16-bit linear, uncompressed:
 * 1536 kbit/s, no codec delay. */
typedef uint32_t rcs_audio_codec;
enum { RCS_AUDIO_OPUS = 1, RCS_AUDIO_AAC = 2, RCS_AUDIO_PCM = 3 };
#define RCS_MAX_AUDIO_CODECS 3

/* How the sharer captures the sound its machine plays.
 *   AUTO                 This system's best: Core Audio on macOS 14.2 or
 *                        later, else ScreenCaptureKit; WASAPI on Windows.
 *   SCREENCAPTUREKIT     Every macOS this library runs on. Delivers 20 ms at a time.
 *   CORE_AUDIO_TAP       macOS 14.2 or later: a process tap on a private
 *                        aggregate device, any period (default 5 ms).
 *   WASAPI_LOOPBACK      Windows. Delivers one engine period (usually 10 ms)
 *                        at a time. */
typedef uint32_t rcs_audio_capture_backend;
enum {
    RCS_AUDIO_CAPTURE_AUTO = 0,
    RCS_AUDIO_CAPTURE_SCREENCAPTUREKIT = 1,
    RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP = 2,
    RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK = 3
};

/* What a stream's pictures are: chroma sampling, bit depth and dynamic range.
 * Both ends list the ones they take (rcs_encoder_config.formats,
 * rcs_decoder_config.formats) and negotiate them along with the codec; each
 * codec implementation says which it encodes or decodes
 * (rcs_codec_implementation.picture_formats). Masks are
 * (1u << rcs_picture_format) bits.
 *   420_8       4:2:0, 8 bits per sample, SDR: every codec and implementation
 *               (the default) but NVDEC, which decodes only the 4:4:4 that
 *               Media Foundation's hardware decoders cannot; VP8's only one,
 *               VP9 profile 0.
 *   420_10      4:2:0, 10 bits, SDR: gradients without banding. HEVC Main 10,
 *               VP9 profile 2, AV1 Main.
 *   444_8       4:4:4, 8 bits, SDR: colour at every pixel, for coloured text
 *               and thin lines. H.264 High 4:4:4 Predictive, HEVC Main 4:4:4,
 *               VP9 profile 1, AV1 High.
 *   444_10      4:4:4, 10 bits, SDR: HEVC Main 4:4:4 10, VP9 profile 3, AV1
 *               High.
 *   HDR10_420   HDR10: BT.2020 primaries, the SMPTE ST 2084 (PQ) transfer and
 *               the BT.2020 matrix, 4:2:0, 10 bits. HEVC Main 10, VP9 profile
 *               2, AV1 Main.
 *   HDR10_444   HDR10 with 4:4:4 sampling: HEVC Main 4:4:4 10, VP9 profile 3,
 *               AV1 High.
 * SDR pictures have BT.709's primaries and transfer, as desktops' sRGB content
 * is carried; their matrix and range are the sharer's (rcs_encoder_config). */
typedef uint32_t rcs_picture_format;
enum {
    RCS_PICTURE_420_8 = 1,
    RCS_PICTURE_420_10 = 2,
    RCS_PICTURE_444_8 = 3,
    RCS_PICTURE_444_10 = 4,
    RCS_PICTURE_HDR10_420 = 5,
    RCS_PICTURE_HDR10_444 = 6
};
#define RCS_MAX_PICTURE_FORMATS 8

/* What a picture's values mean (rcs_color), as streams, frames and packets
 * report it. Matrix and range are those of YCbCr pictures: LIMITED is 16..235
 * (8 bits) or 64..940 (10 bits) for luma, FULL the whole range. */
typedef uint32_t rcs_color_primaries;
enum { RCS_PRIMARIES_BT709 = 1, RCS_PRIMARIES_BT2020 = 2 };
typedef uint32_t rcs_color_transfer;
enum { RCS_TRANSFER_BT709 = 1, RCS_TRANSFER_PQ = 2 }; /* SDR; SMPTE ST 2084. */
typedef uint32_t rcs_color_matrix;
enum { RCS_MATRIX_BT709 = 1, RCS_MATRIX_BT601 = 2, RCS_MATRIX_BT2020 = 3 }; /* BT.2020: non-constant luminance. */
typedef uint32_t rcs_color_range;
enum { RCS_RANGE_LIMITED = 1, RCS_RANGE_FULL = 2 };
typedef struct rcs_color {
    rcs_color_primaries primaries;
    rcs_color_transfer transfer;
    rcs_color_matrix matrix; /* YCbCr pictures; 0 for RGB ones (BGRA8). */
    rcs_color_range range;   /* YCbCr pictures; 0 for RGB ones. */
} rcs_color;

/* How a picture lies in memory (rcs_frame, rcs_gpu_frame_info).
 *   BGRA8    8-bit B, G, R, A bytes; SDR.
 *   RGB10A2  10-bit R, G, B and 2-bit A in a 32-bit word, R in the lowest
 *            bits (DXGI_FORMAT_R10G10B10A2_UNORM): Windows HDR10 capture, PQ
 *            with BT.2020 primaries.
 *   NV12     4:2:0, 8 bits: a Y plane, then a plane of interleaved Cb, Cr at
 *            half width and height (CVPixelBuffer 420v / 420f, DXGI NV12).
 *   P010     4:2:0, 10 bits: as NV12 with 16-bit samples, the value in their
 *            top 10 bits (x420 / xf20, DXGI P010).
 *   NV24     4:4:4, 8 bits: a Y plane, then a plane of interleaved Cb, Cr at
 *            full size (444v / 444f).
 *   P410     4:4:4, 10 bits: as NV24 with 16-bit samples, the value in their
 *            top 10 bits (x444 / xf44). */
typedef uint32_t rcs_pixel_format;
enum {
    RCS_PIXEL_BGRA8 = 1,
    RCS_PIXEL_RGB10A2 = 2,
    RCS_PIXEL_NV12 = 3,
    RCS_PIXEL_P010 = 4,
    RCS_PIXEL_NV24 = 5,
    RCS_PIXEL_P410 = 6
};

/* How much is logged (see Logging): each level includes the ones above it. */
typedef uint32_t rcs_log_level;
enum { RCS_LOG_ERROR = 1, RCS_LOG_WARNING = 2, RCS_LOG_INFO = 3, RCS_LOG_DEBUG = 4 };

/* ------------------------------------------------------------------------ */
/* Library and system queries                                              */
/* ------------------------------------------------------------------------ */

/* The loaded library's RCSDK_VERSION, and as text ("major.minor.patch", then
 * "-" and RCSDK_VERSION_LABEL before a release: "0.1.0-beta"). A host works
 * only with the library of its headers' version; where the library is found
 * at run time (a DLL beside the application), compare rcs_version_number()
 * with RCSDK_VERSION (rcs_header_version()) before anything else. */
RCS_INLINE uint32_t rcs_header_version(void) {
    return RCSDK_VERSION;
}
RCSDK_API uint32_t rcs_version_number(void);
RCSDK_API const char* rcs_version(void);
RCSDK_API const char* rcs_result_message(rcs_result result);
/* Why the previous SDK call on this thread failed, e.g. which extension field
 * is out of range; empty when it succeeded or gave no specific reason. Read it
 * right after the failing call. */
RCSDK_API rcs_result rcs_get_last_call_error(char* buffer, uint32_t capacity);
/* For the public functions of libraries built on the SDK (modules,
 * rcsdk/supervisor.h, a host's own): what rcs_get_last_call_error says next on
 * this thread, reason (NULL: none) when result is a failure, nothing for
 * RCS_OK. Returns result. */
RCSDK_API rcs_result rcs_set_last_call_error(rcs_result result, const char* reason);

/* What this system can do. The codec masks summarise the implementations
 * below as (1u << codec) bits, e.g. (1u << RCS_CODEC_H264). */
typedef struct rcs_capabilities {
    uint32_t struct_size;
    rcs_platform platform;
    /* The picture: the capture sources it offers, as (1u << rcs_capture_source)
     * bits: DISPLAY, REGION and APPLICATION where it can capture the screen,
     * WINDOW where it can capture single windows too (Windows 10 1903 or
     * later), HOST and ENCODED always. */
    uint32_t capture_sources;
    /* The picture formats its capture produces for its own encoders, as
     * (1u << rcs_picture_format) bits: a sharer offers those that both this
     * and an encoder take (rcs_codec_implementation.picture_formats). */
    uint32_t picture_formats;
    uint32_t hardware_encoders; /* Codecs each acceleration can encode... */
    uint32_t software_encoders;
    uint32_t hardware_decoders; /* ...and decode. */
    uint32_t software_decoders;
    /* Nonzero when software decoding still yields native GPU surfaces: on
     * macOS, iOS and Android in the stream's layout; on Windows BGRA8
     * textures, uploaded from the CPU images. See on_gpu_frame. */
    uint32_t software_decoding_gpu_frames;
    /* The GPU images the host may push as its own pictures
     * (rcs_session_push_gpu_frame, rcsdk/gpu.h), as (1u << rcs_gpu_resource)
     * bits: CVPixelBuffers on macOS and iOS, AHardwareBuffers on Android; 0
     * where it takes none. */
    uint32_t pushed_gpu_frames;
    /* The sound: the backends that capture what it plays, as
     * (1u << rcs_audio_capture_backend) bits (0: none)... */
    uint32_t audio_capture_backends;
    /* ...whether they can leave this application's own sound out (every macOS
     * this library runs on; Windows 10 build 20348 / Windows 11 or later)... */
    uint32_t audio_exclusion;
    /* ...and the audio codecs it encodes and decodes, as (1u << rcs_audio_codec)
     * bits: Opus and PCM always, AAC where the system has it. */
    uint32_t audio_encoders;
    uint32_t audio_decoders;
    /* Control: the remote input this system executes, as (1u << rcs_input_type)
     * bits (0: none). TOUCH and PEN on Windows 10 1809 or later; PEN on macOS
     * as tablet events; SECURE_ATTENTION as secure_attention says. */
    uint32_t input_types;
    /* Nonzero when this process can capture and control the system's secure
     * desktops, the sign-in screen, the lock screen and UAC prompts
     * (rcs_capture_config.secure_desktop): Windows, running as LocalSystem in a
     * user session... */
    uint32_t secure_desktop;
    /* ...and when it can send Ctrl+Alt+Del (RCS_INPUT_SECURE_ATTENTION):
     * Windows, a service in session 0, or a process an rcs_supervisor started
     * (as LocalSystem or as the user: the supervisor sends it on the helper's
     * behalf), with the system policy SoftwareSASGeneration allowing services
     * (1 or 3). The sharing session's process sends it, also when its agent is
     * another process. */
    uint32_t secure_attention;
    /* The network: the transports it carries, as (1u << rcs_transport) bits:
     * every one on the native systems; in a browser (WEB), which has no
     * sockets of its own, HOST and FORWARDER (over the page's channel:
     * forwarder_links). */
    uint32_t transports;
    /* Nonzero where this system's Wi-Fi can be kept out of power saving
     * while connected (rcs_network_tuning_config.wifi_low_latency): Android
     * and Windows. */
    uint32_t wifi_low_latency;
    /* What this system says of its networks (rcs_list_networks, rcs_path_info),
     * as RCS_NETWORK_FACT_* bits: their kind, whether they are metered, whether
     * the user wants data saved. 0 in a browser. */
    uint32_t network_facts;
    /* What this system says of its Wi-Fi (rcs_list_wifi), as RCS_WIFI_FACT_*
     * bits: macOS all of them; iOS only whether AWDL is up (PEER_TO_PEER);
     * Windows and Android CHANNEL, SIGNAL and RATE. 0 in a browser
     * (rcs_list_wifi is UNSUPPORTED). */
    uint32_t wifi_facts;
    /* How FORWARDER reaches the forwarder here, as (1u << rcs_forwarder_link)
     * bits: every link on the native systems; in a browser HOST only (the
     * page's channel, e.g. a WebSocket to the forwarder's WebSocket entry). */
    uint32_t forwarder_links;
} rcs_capabilities;
enum { RCS_NETWORK_FACT_KIND = 1u << 0, RCS_NETWORK_FACT_METERED = 1u << 1, RCS_NETWORK_FACT_CONSTRAINED = 1u << 2 };
enum {
    RCS_WIFI_FACT_CHANNEL = 1u << 0, /* band and channel */
    RCS_WIFI_FACT_WIDTH = 1u << 1,
    RCS_WIFI_FACT_SIGNAL = 1u << 2,
    RCS_WIFI_FACT_NOISE = 1u << 3,
    RCS_WIFI_FACT_RATE = 1u << 4,
    RCS_WIFI_FACT_PEER_TO_PEER = 1u << 5
};
#define RCS_CAPABILITIES_INIT {sizeof(rcs_capabilities), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_capabilities rcs_capabilities_init(void) {
    rcs_capabilities value = RCS_CAPABILITIES_INIT;
    return value;
}

/* Hardware probing may take tens of milliseconds; cache the result. */
RCSDK_API rcs_result rcs_get_capabilities(rcs_capabilities* capabilities);

/* One codec implementation this system offers, listed best first within a
 * codec, direction and acceleration (RCS_BACKEND_AUTO takes the first for the
 * GPU in question). A session selects among them through rcs_encoder_config
 * and rcs_decoder_config: codec, acceleration and backend. Which hardware
 * encoder a sharer uses also depends on the GPU driving its display. */
typedef struct rcs_codec_implementation {
    rcs_codec codec;
    rcs_codec_direction direction;
    rcs_acceleration acceleration;
    rcs_backend backend;
    uint64_t adapter_luid; /* Windows hardware implementations: the GPU's LUID; else 0. */
    /* The largest picture it takes: at most max_width wide and max_height
     * high, within the codec's own limit (rcs_share_video_config.width). As
     * the system says: Windows hardware decoders as the GPU's driver answers
     * for decoders of such sizes, NVENC, NVDEC and oneVPL as their drivers
     * report; Windows' software H.264 and HEVC decoders 4096x2304, as
     * documented; where the
     * system says nothing (VideoToolbox, Media Foundation's encoders, the AV1
     * extension's software decoder) the codec's limit. A size it cannot open
     * after all fails the stream with CODEC, naming the size. */
    uint32_t max_width, max_height;
    /* The most it takes a second in each picture format (by rcs_picture_format;
     * 0 unused), as width x height x frames per second: the highest level it
     * encodes that format at (H.264 counted in 16x16 macroblocks). NVENC: the
     * level its driver reports, for 4:4:4 the highest it starts at (RTX 4090,
     * driver 616.92: H.264 4:2:0 at level 6.2, 4:4:4 at 6.0, 3840x2160 up to
     * 128 frames a second; HEVC both at 6.2); 0 where the system says nothing
     * (it may still not take every rate). A sharer's stream asks no more of
     * its encoder: the frame rate is lowered to fit
     * (rcs_stream_info.frames_per_second), the size kept. */
    uint64_t max_pixels_per_second[8];
    /* The picture formats it encodes or decodes, as (1u << rcs_picture_format)
     * bits, as its system says or documents it; for the decoders of macOS,
     * iOS and Android, which say no more than the codec, as decoding a
     * keyframe of each format here shows. A decoder of 10-bit pictures
     * decodes HDR10 too (PQ is how the values are read, not how they are
     * coded); an encoder's HDR10 needs a capture that produces it
     * (rcs_capabilities.picture_formats). */
    uint32_t picture_formats;
    /* Encoders: 1 where it can drop pictures from its references, so that
     * a picture lost on the network costs a normal picture, not a keyframe
     * (RCS_RECOVERY_REFERENCES): the codec libraries but OpenH264, NVENC
     * where its driver says so, oneVPL's (a stream of temporal layers
     * recovers with a keyframe there), VideoToolbox's hardware encoders (in
     * their low-latency mode; not the pipelined one, see rcs_codec_event). Media
     * Foundation's encoders say it only once opened (asking each here would
     * take some 150 ms): 0 here, and rcs_codec_event.ref_invalidation of the
     * stream that uses one. */
    uint32_t ref_invalidation;
    /* Encoders: the most temporal layers it makes (rcs_scalability), 1 where
     * it makes none; decoders: 0. libvpx (VP8, VP9) and libaom (AV1): 3;
     * VideoToolbox's hardware H.264 and HEVC encoders: 2 (H.264 not in its
     * pipelined mode, see rcs_codec_event); NVENC: where the driver says so
     * (up to 3), H.264 from driver 522.25, HEVC and AV1 from driver 570 (its
     * interface 13.0); oneVPL: as its runtime takes them (up to 3). Media
     * Foundation's and MediaCodec's encoders say it
     * only once opened (at most 2; MediaCodec's HEVC and AV1 3): 1 here, and
     * rcs_codec_event.temporal_layers of the stream that uses one. */
    uint32_t temporal_layers;
    char name[64]; /* UTF-8, e.g. "NVIDIA HEVC Encoder MFT". */
} rcs_codec_implementation;

typedef struct rcs_display_info {
    /* Stays while the display stays connected, whatever happens to the others:
     * macOS its CGDirectDisplayID, Windows N of its GDI device name \\.\DISPLAYN. */
    uint32_t display_id;
    /* As the desktop shows it: a display turned to portrait is taller than wide. */
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint32_t refresh_hz; /* 0 where the system does not say. */
    /* How far the display is turned, clockwise, as its settings say: 0, 90
     * (its bottom edge now on the left, the picture drawn on it a quarter
     * turn the other way: Windows' "Portrait"), 180 or 270. Capture and
     * remote input work in the desktop's orientation all the same; a change
     * is a display change (RCS_EVENT_DISPLAYS). */
    uint32_t rotation_degrees;
    uint32_t is_main;
    /* Where it sits on the desktop, in the desktop's own units (macOS:
     * points; Windows: pixels of the virtual screen), its top left corner
     * from the main display's: for laying out the displays, or the streams
     * that show them, as the machine does. */
    int32_t desktop_x, desktop_y;
    uint32_t desktop_width, desktop_height;
} rcs_display_info;

/* A window capture can share (RCS_CAPTURE_WINDOW), as it was when listed. */
typedef struct rcs_window_info {
    /* Windows: its HWND; macOS: its CGWindowID. Stays while the window exists. */
    uint64_t window_id;
    uint32_t process_id; /* The process it belongs to: RCS_CAPTURE_APPLICATION shares all of its windows. */
    /* Where it shows: the display that shows most of it (0 while it shows on
     * none), its place on that display in the display's pixels from its top
     * left (it may reach past the display's edges)... */
    uint32_t display_id;
    int32_t x, y;
    /* ...and its size in pixels, as capture sees it (minimized: as it is when
     * restored, at 0, 0 of no display). */
    uint32_t width, height;
    uint32_t minimized;    /* Nonzero while minimized: capture of it waits until it is restored. */
    uint32_t focused;      /* Nonzero for the window that has the keyboard focus. */
    char title[256];       /* UTF-8, possibly empty; macOS: empty without screen recording permission. */
    char application[128]; /* UTF-8: its application's name. */
} rcs_window_info;

/* Lists use two passes: NULL/0 returns the count; a small capacity returns
 * BUFFER_TOO_SMALL. item_size is sizeof the element type. */
RCSDK_API rcs_result rcs_list_codec_implementations(rcs_codec_implementation* items, uint32_t item_size,
                                                    uint32_t capacity, uint32_t* out_count);
RCSDK_API rcs_result rcs_list_displays(rcs_display_info* displays, uint32_t item_size, uint32_t capacity,
                                       uint32_t* out_count);
/* The windows a user would pick from to share, front to back: on Windows the
 * ones Alt+Tab shows, minimized ones included; on macOS the ones on screen
 * (the current Spaces), this application's own included. Windows come and go
 * all the time: a second pass may find more (BUFFER_TOO_SMALL; ask with some
 * room to spare). Needs no permission. */
RCSDK_API rcs_result rcs_list_windows(rcs_window_info* windows, uint32_t item_size, uint32_t capacity,
                                      uint32_t* out_count);
RCSDK_API rcs_result rcs_generate_connection_key(uint8_t* key, uint32_t size);
/* A PIN of `digits` decimal digits (4..32) from the system's random generator,
 * NUL-terminated (RCS_SECRET_PIN): BUFFER_TOO_SMALL when capacity is below
 * digits + 1. */
RCSDK_API rcs_result rcs_generate_pin(char* out, uint32_t capacity, uint32_t digits);

/* Device keys (rcs_device_identity_config): a long-term P-256 key pair that proves
 * which device an end is. The private key is 0x04 || X || Y || d (SEC 1):
 * its public key followed by the private scalar; keep it secret, e.g. in the
 * system's keychain or credential store. The public key is 0x04 || X || Y,
 * the first RCS_PUBLIC_KEY_SIZE bytes of the private key; hand it to the
 * peers that should trust this device. The SDK stores neither. */
#define RCS_DEVICE_KEY_SIZE 97u
#define RCS_PUBLIC_KEY_SIZE 65u
/* size: RCS_DEVICE_KEY_SIZE. */
RCSDK_API rcs_result rcs_generate_device_key(uint8_t* device_key, uint32_t size);
/* The public key of a device key; INVALID_ARGUMENT when it is not a P-256 key
 * pair whose halves belong together. Sizes: RCS_DEVICE_KEY_SIZE and RCS_PUBLIC_KEY_SIZE. */
RCSDK_API rcs_result rcs_device_public_key(const uint8_t* device_key, uint32_t device_key_size, uint8_t* public_key,
                                           uint32_t public_key_size);

RCSDK_API rcs_permission rcs_screen_capture_permission(void);
/* Whether this process may inject input at all: macOS, Accessibility;
 * Windows, GRANTED in a user's session and UNAVAILABLE in session 0 (a
 * service has no desktop). A lock screen or a UAC prompt is no matter of
 * permission: it pauses a granted control (rcs_control_config). */
RCSDK_API rcs_permission rcs_input_permission(void);
/* Show the OS permission prompt. Call on the application's main thread. */
RCSDK_API rcs_result rcs_request_screen_capture_permission(void);
RCSDK_API rcs_result rcs_request_input_permission(void);
/* Whether this process may record from microphones (RCS_AUDIO_MICROPHONE):
 * macOS and iOS, the user's answer to the system's prompt; Windows, the
 * privacy settings for desktop programs; Android, the RECORD_AUDIO runtime
 * permission (UNAVAILABLE before rcs_android_initialize). */
RCSDK_API rcs_permission rcs_microphone_permission(void);
/* macOS: shows the system's prompt the first time (on the application's main
 * thread; WRONG_THREAD otherwise) and returns PERMISSION_REQUIRED until the
 * user answered; iOS the same from any thread; after that, and on Windows,
 * only the system's settings change the answer: RCS_OK when granted, else
 * PERMISSION_REQUIRED. Android: the answer as it is (an activity of the
 * host's asks the user: requestPermissions); UNSUPPORTED before
 * rcs_android_initialize. */
RCSDK_API rcs_result rcs_request_microphone_permission(void);

/* A sound device. device_id stays while the device stays connected (Windows:
 * while it stays installed); a device that both records and plays is listed
 * twice, once each way. iOS lists the outputs of the current route (the
 * system chooses it) and the inputs the audio session's category offers. */
typedef struct rcs_audio_device_info {
    uint32_t device_id; /* Never 0. */
    uint32_t input;     /* 1 records (a microphone), 0 plays. */
    uint32_t is_default; /* The system's default input or output now. */
    char name[128];     /* UTF-8, e.g. "MacBook Pro Microphone". */
} rcs_audio_device_info;
/* The sound devices, inputs then outputs (rcs_send_audio_config.device). */
RCSDK_API rcs_result rcs_list_audio_devices(rcs_audio_device_info* devices, uint32_t item_size, uint32_t capacity,
                                            uint32_t* out_count);

/* iOS: who sets up the application's AVAudioSession, which every session of
 * the process and the host share. All of the SDK's sound on iOS runs on one
 * audio engine of the application: players and microphones of every session
 * together, so that a microphone's echo cancellation hears what the SDK plays.
 *   SDK   The SDK sets the audio session up for that engine (the default):
 *         the category for what plays and records now (playback; with a
 *         microphone playAndRecord, mode voiceChat with voice processing),
 *         activated with the first part and deactivated after the last
 *         (letting other applications play again). The system's
 *         interruptions (a call, Siri, another application) pause the
 *         sound with the reason (RCS_EVENT_AUDIO); it resumes after.
 *   HOST  The SDK never touches the audio session; the host sets it up
 *         before sound starts. Sound that the host's setup cannot carry
 *         pauses with the reason; nothing is changed behind the host.
 * With SDK:
 *   mix_with_others       1 plays along with other applications' sound; 0
 *                         (the default) has them stop while the SDK plays.
 *   default_to_speaker    While a microphone records, 1 (the default) plays
 *                         on the speaker, 0 on the receiver (an iPhone held
 *                         to the ear) unless headphones are on.
 *   bluetooth_microphone  1 (the default) lets a Bluetooth headset's
 *                         microphone record (its sound then plays at call
 *                         quality); 0 keeps Bluetooth to playback.
 * Applies at once, while sessions run too: changing to HOST leaves the audio
 * session as it is for the host; changing to SDK sets it up for what plays
 * and records now. Other systems have no such setting: both calls are
 * UNSUPPORTED. */
typedef uint32_t rcs_audio_session_manager;
enum { RCS_AUDIO_SESSION_SDK = 1, RCS_AUDIO_SESSION_HOST = 2 };
typedef struct rcs_audio_session_config {
    uint32_t struct_size;
    rcs_audio_session_manager manager;
    uint32_t mix_with_others;
    uint32_t default_to_speaker;
    uint32_t bluetooth_microphone;
} rcs_audio_session_config;
#define RCS_AUDIO_SESSION_CONFIG_INIT {sizeof(rcs_audio_session_config), RCS_AUDIO_SESSION_SDK, 0u, 1u, 1u}
RCS_INLINE rcs_audio_session_config rcs_audio_session_config_init(void) {
    rcs_audio_session_config value = RCS_AUDIO_SESSION_CONFIG_INIT;
    return value;
}
RCSDK_API rcs_result rcs_set_audio_session_config(const rcs_audio_session_config* config);
RCSDK_API rcs_result rcs_get_audio_session_config(rcs_audio_session_config* config);

/* ---- The network -------------------------------------------------------- */

/* What a network of this machine is, by what carries it. A VPN is no kind of
 * its own: a tunnel says RCS_NETWORK_FLAG_VPN, and its kind is that of the
 * network it runs over where the system says (else UNKNOWN). */
typedef uint32_t rcs_network_kind;
enum {
    RCS_NETWORK_UNKNOWN = 0, /* The system does not say: an interface it does not know, the host's channel. */
    RCS_NETWORK_ETHERNET = 1,
    RCS_NETWORK_WIFI = 2,
    RCS_NETWORK_CELLULAR = 3,
    RCS_NETWORK_OTHER = 4 /* USB tethering, a Thunderbolt bridge, a virtual machine's network. */
};
/* What the system says of a network, as bits (rcs_capabilities.network_facts:
 * which ones this system can say; DEFAULT every system with networks of its
 * own says, VPN those that say KIND).
 *   METERED      Charged by use or capped: cellular, a phone's hotspot, a
 *                network the user marked metered, roaming.
 *   CONSTRAINED  The user wants data saved: Apple's Low Data Mode, Android's
 *                Data Saver for this application on a metered network,
 *                Windows near or over its data limit.
 *   DEFAULT      The network the system sends through now (where the system
 *                says no more: the one its route to the internet takes).
 *   VPN          A tunnel (WireGuard, IPsec, the system's VPN), told by its
 *                interface: Apple's by name (utun, ipsec, ppp: a guess),
 *                Windows' by type (a TAP adapter passes for Ethernet),
 *                Android's as the system says. */
enum {
    RCS_NETWORK_FLAG_METERED = 1u << 0,
    RCS_NETWORK_FLAG_CONSTRAINED = 1u << 1,
    RCS_NETWORK_FLAG_DEFAULT = 1u << 2,
    RCS_NETWORK_FLAG_VPN = 1u << 3
};
typedef struct rcs_network_info {
    char name[64];  /* UTF-8: "en0", "Wi-Fi", "wlan0", "rmnet_data0". */
    uint32_t index; /* The interface's index (rcs_network_tuning_config.local_interface takes it). */
    rcs_network_kind kind;
    uint32_t flags;      /* RCS_NETWORK_FLAG_* bits. */
    uint32_t ipv4, ipv6; /* Nonzero when it has a usable IPv4 / IPv6 address. */
} rcs_network_info;
/* This machine's networks that are up with a usable address, the default
 * first (two passes, as the other lists). UNSUPPORTED in a browser. */
RCSDK_API rcs_result rcs_list_networks(rcs_network_info* networks, uint32_t item_size, uint32_t capacity,
                                       uint32_t* out_count);
/* A Wi-Fi network this machine is connected to, as its system says now
 * (rcs_capabilities.wifi_facts: what it says; 0 where it says nothing). */
typedef uint32_t rcs_wifi_band;
enum { RCS_WIFI_BAND_UNKNOWN = 0, RCS_WIFI_BAND_2_4_GHZ = 1, RCS_WIFI_BAND_5_GHZ = 2, RCS_WIFI_BAND_6_GHZ = 3 };
typedef struct rcs_wifi_info {
    char name[64];  /* UTF-8, as rcs_network_info.name and rcs_path_info.interface_name. */
    uint32_t index; /* As rcs_network_info.index. */
    rcs_wifi_band band;
    uint32_t channel;
    uint32_t width_mhz;
    int32_t signal_dbm; /* Received signal strength (RSSI). */
    int32_t noise_dbm;
    uint32_t rate_mbps; /* What this machine sends at now. */
    /* Apple: 1 while AWDL is up, the peer-to-peer Wi-Fi of AirDrop, AirPlay,
     * Handoff, Sidecar and Universal Control. It keeps to channels 6
     * (2.4 GHz), 44 and 149 (5 GHz); while it is up, a network on any other
     * channel loses the radio to it for tens of milliseconds about every
     * 1.05 s (other_channel: 1), and everything stops that long, both ways
     * (RCS_EVENT_STALLS). A network on one of those channels, or on wire,
     * does not; nor with AWDL down (sudo ifconfig awdl0 down: what uses it
     * stops working, and macOS brings it up again when something asks). */
    uint32_t peer_to_peer;
    uint32_t other_channel;
} rcs_wifi_info;
/* This machine's Wi-Fi networks that it is connected to (two passes, as the
 * other lists). UNSUPPORTED in a browser. */
RCSDK_API rcs_result rcs_list_wifi(rcs_wifi_info* networks, uint32_t item_size, uint32_t capacity,
                                   uint32_t* out_count);
/* The host's own watcher (Android's NetworkCallback, Qt's
 * QNetworkInformation) saw the network change before the SDK did: the
 * sessions that run look at their paths at once (rcs_paths_config). Optional
 * (each system's own changes reach the SDK by themselves, Android's within
 * half a second); any thread. */
RCSDK_API rcs_result rcs_notify_network_change(void);

/* ------------------------------------------------------------------------ */
/* Session configuration                                                    */
/* ------------------------------------------------------------------------ */

/* Header of every extension structure. Chain extensions through next, in any
 * order, each type at most once. An extension of the other role is
 * INVALID_ARGUMENT, never ignored; a field this system has no use for (another
 * platform's) is refused by start() with UNSUPPORTED.
 *
 * Every extension of the entry and feature layers (this header) and the
 * tuning ones (rcsdk/tuning.h) can be replaced with rcs_session_update while
 * the session runs, without reconnecting. Most take effect at once: the
 * picture or the sound restarts with the new settings where they need it.
 * RCS_EXT_DEVICE_IDENTITY, RCS_EXT_ICE_SERVERS' servers and credentials and
 * RCS_EXT_NETWORK_TUNING's interface and timeouts apply from the next
 * connection (a sharer listening again, a viewer reconnecting);
 * RCS_EXT_ICE_SERVERS' turn_transport is fixed for a run. The replacing layer's extensions are fixed for a run.
 *
 * The tuning extensions live in rcsdk/tuning.h and those of the replacing
 * layer in their own headers (rcsdk/host_codec.h, rcsdk/rate_control.h,
 * rcsdk/host_transport.h, rcsdk/gpu.h, rcsdk/agent.h), so that a host that
 * needs none of them never reads them. */
typedef uint32_t rcs_extension_type;
enum {
    /* Sharing */
    RCS_EXT_SHARE_VIDEO = 1, /* rcs_share_video_config  The stream, its bitrate, requests it follows */
    RCS_EXT_ENCODER = 2,     /* rcs_encoder_config      Codecs and formats offered, implementation */
    RCS_EXT_CAPTURE = 3,     /* rcs_capture_config      Where the picture comes from */
    RCS_EXT_STREAMS = 4,     /* rcs_streams_config      More displays, each a stream of its own */
    RCS_EXT_CONTROL = 5,     /* rcs_control_config      Remote control */
    RCS_EXT_VIEWERS = 6,     /* rcs_viewers_config      How many viewers at once, and how they share the stream */
    RCS_EXT_ENCODINGS = 7,   /* rcs_encodings_config    More encodings of a stream, for viewers that cannot take the main one */
    /* Viewing */
    RCS_EXT_VIEW_REQUEST = 8, /* rcs_view_request_config What to ask the sharer for */
    RCS_EXT_DECODER = 9,      /* rcs_decoder_config      Codecs and formats offered, implementation */
    /* Sound, both ways */
    RCS_EXT_SEND_AUDIO = 10,    /* rcs_send_audio_config    The sound this end sends */
    RCS_EXT_RECEIVE_AUDIO = 11, /* rcs_receive_audio_config The sound this end receives and plays */
    /* Both */
    RCS_EXT_ICE_SERVERS = 12,     /* rcs_ice_servers_config     P2P and RELAY: STUN and TURN servers */
    RCS_EXT_RECONNECT = 13,       /* rcs_reconnect_config       What a lost connection does to the run */
    RCS_EXT_BACKGROUND = 14,      /* rcs_background_config      What pauses while the application is in the background (iOS) */
    RCS_EXT_DEVICE_IDENTITY = 15, /* rcs_device_identity_config This end's device key and the keys it trusts */
    RCS_EXT_LOG = 16,             /* rcs_log_config             How much this session logs */
    RCS_EXT_MODULE = 17,          /* rcs_module_config          A module this session uses, and its settings; any number */
    RCS_EXT_PATHS = 18,           /* rcs_paths_config           What a change of network does to the connection */
    RCS_EXT_FORWARDER = 19,       /* rcs_forwarder_config       FORWARDER: which forwarder, and the host's token for its room */
    /* Tuning (rcsdk/tuning.h) */
    RCS_EXT_VIDEO_TUNING = 20,   /* rcs_video_tuning_config   Bitrate rules, pacing, encoder, decode queue */
    RCS_EXT_AUDIO_TUNING = 21,   /* rcs_audio_tuning_config   Sound periods and buffers */
    RCS_EXT_NETWORK_TUNING = 22, /* rcs_network_tuning_config Queues, interface, reconnect pace, timeouts */
    /* The host replaces a part (each in its own header) */
    RCS_EXT_HOST_ENCODER = 23,   /* rcs_host_encoder_config   The host's own encoder; fixed for a run (rcsdk/host_codec.h) */
    RCS_EXT_HOST_DECODER = 24,   /* rcs_host_decoder_config   The host's own decoder; fixed for a run (rcsdk/host_codec.h) */
    RCS_EXT_ENCODED_OUTPUT = 25, /* rcs_encoded_output_config The encoded packets, to the host; both roles (rcsdk/host_codec.h) */
    RCS_EXT_RATE_DECIDER = 26,   /* rcs_rate_decider_config   The host decides the bitrate (rcsdk/rate_control.h) */
    RCS_EXT_HOST_TRANSPORT = 27, /* rcs_host_transport_config RCS_TRANSPORT_HOST's channel (rcsdk/host_transport.h) */
    RCS_EXT_GPU_DEVICE = 28,     /* rcs_gpu_device_config     Decode on the host's GPU device (rcsdk/gpu.h) */
    RCS_EXT_AGENT_LINK = 29      /* rcs_agent_link_config     Capture, encoding and input in another process (rcsdk/agent.h) */
};
typedef struct rcs_extension {
    rcs_extension_type type;
    uint32_t struct_size; /* Of the whole extension structure. */
    const struct rcs_extension* next;
} rcs_extension;

/* ---- Sharing ------------------------------------------------------------ */

/* How the bitrate follows the network.
 *   DEFAULT   ADAPTIVE.
 *   FIXED     Always bitrate_bps.
 *   ADAPTIVE  Between min_bitrate_bps and bitrate_bps, from what the viewer
 *             reports arriving: a bottleneck's queue or loss lowers it within
 *             a fraction of a second; a clear path raises it (the rules and
 *             their defaults: rcs_video_tuning_config, rcsdk/tuning.h; the host may
 *             decide instead: rcsdk/rate_control.h). Over TCP the queue it sees
 *             includes what waits in TCP's own buffers.
 * Each change is reported (RCS_EVENT_RATE) and in rcs_video_send_stats. */
typedef uint32_t rcs_rate_mode;
enum { RCS_RATE_DEFAULT = 0, RCS_RATE_FIXED = 1, RCS_RATE_ADAPTIVE = 2 };

/* The picture this end sends, and how far it follows the viewer. Without it:
 * 1920x1080, 60 FPS, 20 Mbit/s, adaptive bitrate,
 * requests followed up to the display's own size and refresh rate and
 * 200 Mbit/s. Each change of the stream restarts the encoder with a keyframe
 * and is announced to both ends (RCS_EVENT_STREAM). */
typedef struct rcs_share_video_config {
    rcs_extension header;
    /* The stream, unless the viewer asks for another. The display keeps its
     * aspect ratio inside the frame (letterboxing). Even, at least 16x16, at
     * most 8192 a side and 35 651 584 pixels (8192x4352, 4352x8192). What the
     * codecs take, either way round: H.264 4096 a side and 36 864 macroblocks
     * (4096x2304, 2304x4096); HEVC, AV1 and the hosts' own codecs all of it;
     * an encoder or the viewer's decoder may take less
     * (rcs_codec_implementation.max_width, rcs_peer_info.codec_limits). This
     * size is never made smaller: start() and rcs_session_update refuse one no
     * codec of this end (and, while connected, of the viewer) takes with
     * UNSUPPORTED; a connection uses the first codec both ends offer that
     * takes it, and one where none does ends with UNSUPPORTED on both ends,
     * naming the first common codec and its limit. */
    uint32_t width;
    uint32_t height;
    uint32_t frames_per_second; /* 1..240 */
    uint32_t bitrate_bps;       /* 128 kbit/s..500 Mbit/s: FIXED, the bitrate; ADAPTIVE, the most. */
    /* The bitrate. */
    rcs_rate_mode rate_mode;
    uint32_t min_bitrate_bps; /* ADAPTIVE: at least this, 100 kbit/s..bitrate_bps; 0: 500 kbit/s (at most bitrate_bps). */
    /* The viewer's requests (rcs_view_request_config). The limits bound
     * requests only, never the fields above; a size beyond them, or beyond
     * what the codec, this end's encoder or the viewer's decoder takes, is
     * scaled down to fit, keeping its aspect ratio. */
    uint32_t follow_requests;       /* 1: apply them within the limits; 0: keep this end's settings. */
    uint32_t max_width;             /* Even, 16..8192; 0: the display's width in pixels... */
    uint32_t max_height;            /* ...and height. */
    uint32_t max_frames_per_second; /* 0: the display's refresh rate (60 where the system does not say). */
    uint32_t max_bitrate_bps;       /* 0: 200 Mbit/s. */
} rcs_share_video_config;
#define RCS_SHARE_VIDEO_CONFIG_INIT                                                                                    \
    {{RCS_EXT_SHARE_VIDEO, sizeof(rcs_share_video_config), NULL},                                                    \
     1920u,                                                                                                            \
     1080u,                                                                                                            \
     60u,                                                                                                              \
     20000000u,                                                                                                        \
     RCS_RATE_DEFAULT,                                                                                                 \
     0u,                                                                                                               \
     1u,                                                                                                               \
     0u,                                                                                                               \
     0u,                                                                                                               \
     0u,                                                                                                               \
     0u}
RCS_INLINE rcs_share_video_config rcs_share_video_config_init(void) {
    rcs_share_video_config value = RCS_SHARE_VIDEO_CONFIG_INIT;
    return value;
}

/* Codec negotiation, for both ends: each offers the codecs[] it lists (most
 * preferred first, unused entries 0) that this system implements with its
 * acceleration and backend, each with the largest picture it takes there
 * (rcs_codec_implementation.max_width) and the picture formats of formats[]
 * it takes there: those of the implementation (the first that matches, as
 * for the size), and a sharer's capture's (rcs_capabilities.picture_formats);
 * a codec with none of them is not offered. start() fails with UNSUPPORTED
 * when nothing is. The sharer encodes the first codec of its offer that the
 * viewer also offers with a picture format in common and that takes the
 * sharer's own size (rcs_share_video_config.width), unless the viewer asks for
 * another both have (rcs_view_request_config), at a size the codec, the
 * sharer's encoder and the viewer's decoder take; then the first of the
 * sharer's formats[] both take in that codec, unless the viewer asks for
 * another both take.
 * Without a common codec, or without a common picture format, both ends fail
 * with UNSUPPORTED, the reason naming both offers (rcs_peer_info). Listing a
 * second codec or format is how a host accepts an alternative; the SDK never
 * adds one. Only codecs and formats are negotiated: each end picks its own
 * implementation. Either end may change its lists while running; one without
 * anything the peer offered is UNSUPPORTED. */

/* The encoder. Without it: H.264, 8-bit 4:2:0 SDR pictures with the BT.709
 * matrix and limited range, hardware, the platform's backend, the fastest
 * encoding, keyframes only when needed. Nearly every GPU encodes H.264; HEVC,
 * VP9 and AV1 need less bitrate for the same picture where both ends have them
 * (rcs_capabilities), and the libraries built into the SDK encode VP8, VP9 and
 * AV1 on the CPU of any system (acceleration SOFTWARE or HARDWARE_FIRST: a
 * core or so for 1080p60). A codec the host's encoder handles
 * (RCS_EXT_HOST_ENCODER) is offered whatever the acceleration and backend; a
 * host codec id needs one (or capture source ENCODED).
 *
 * formats[]: the picture formats this end sends, most preferred first (see
 * rcs_picture_format); a viewer receives only those it lists itself. Capture
 * source HOST (BGRA8 pictures) takes SDR formats only. With source ENCODED
 * every listed format is offered, and the host's packets must be what the
 * stream is.
 *
 * matrix and range: how pictures are written as YCbCr. matrix: SDR pictures'
 * BT709 or BT601 (HDR10 always uses BT.2020's; BT2020 here is
 * INVALID_ARGUMENT); range: LIMITED or FULL, for every format. Every decoder
 * takes them, and the viewer is told (rcs_stream_info.color, every frame's
 * rcs_gpu_frame_info.color). */
typedef struct rcs_encoder_config {
    rcs_extension header;
    rcs_codec codecs[RCS_MAX_CODECS];                     /* At least one; no duplicates. */
    rcs_picture_format formats[RCS_MAX_PICTURE_FORMATS]; /* At least one; no duplicates. */
    rcs_color_matrix matrix;
    rcs_color_range range;
    rcs_acceleration acceleration;
    /* RCS_BACKEND_AUTO or one listed by rcs_list_codec_implementations. One
     * the capturing GPU does not offer (NVENC with a display on another
     * vendor's GPU) fails the stream with UNSUPPORTED, never another backend. */
    rcs_backend backend;
} rcs_encoder_config;
#define RCS_ENCODER_CONFIG_INIT                                                                                        \
    {{RCS_EXT_ENCODER, sizeof(rcs_encoder_config), NULL},                                                              \
     {RCS_CODEC_H264, 0u, 0u},                                                                                         \
     {RCS_PICTURE_420_8, 0u},                                                                                          \
     RCS_MATRIX_BT709,                                                                                                 \
     RCS_RANGE_LIMITED,                                                                                                \
     RCS_ACCELERATION_HARDWARE,                                                                                        \
     RCS_BACKEND_AUTO}
RCS_INLINE rcs_encoder_config rcs_encoder_config_init(void) {
    rcs_encoder_config value = RCS_ENCODER_CONFIG_INIT;
    return value;
}

/* Where the picture comes from (rcs_capabilities.capture_sources says which
 * this system offers).
 *   DISPLAY      A display, as the local user sees it (default).
 *   REGION       A rectangle of a display (region_*), e.g. the part of a large
 *                display that matters, or one the host's user drew.
 *   WINDOW       One window (window_id, see rcs_list_windows) wherever it is,
 *                also where other windows cover it or past a display's edge;
 *                nothing else shows, not even the menus and dialogs of its
 *                application.
 *   APPLICATION  Every window of one process (process_id) on a display, as
 *                they appear there, menus and dialogs included; the desktop
 *                and other applications' windows stay black. Where another
 *                application's window covers one of them, macOS still shows
 *                it; Windows shows black there too.
 *   HOST         The host's own frames, pushed with
 *                rcs_session_push_video_frame: a rendered scene, a virtual
 *                display, another program's output.
 *   ENCODED      The host's own bitstream, already encoded (a game engine's
 *                or a cloud renderer's output, a camera's H.264), pushed with
 *                rcs_session_push_encoded_video: the SDK neither captures nor
 *                encodes. It offers every codec rcs_encoder_config lists
 *                (host codecs included) and tells the host when a keyframe is
 *                due (RCS_EVENT_KEYFRAME), the bitrate (RCS_EVENT_RATE) and
 *                whether the path has room now (rcs_session_video_ready).
 *                rcs_encoder_config.acceleration is what the viewers hear of
 *                the host's encoder (rcs_stream_info.encoder_acceleration),
 *                HARDWARE_FIRST as HARDWARE. Not with an agent in another
 *                process, nor with RCS_EXT_HOST_ENCODER. */
typedef uint32_t rcs_capture_source;
enum {
    RCS_CAPTURE_DISPLAY = 0,
    RCS_CAPTURE_REGION = 1,
    RCS_CAPTURE_WINDOW = 2,
    RCS_CAPTURE_APPLICATION = 3,
    RCS_CAPTURE_HOST = 4,
    RCS_CAPTURE_ENCODED = 5
};

/* What is captured. Without it: the main display, this application's windows
 * included. Changing it while running restarts capture on the new source, and
 * remote input follows; a display rcs_session_list_displays does not list is
 * INVALID_ARGUMENT, and so is a region that is not inside its display.
 *
 * A shared window or application that is minimized or hidden, or (APPLICATION)
 * has no window on the display, pauses capture (RCS_CAPTURE_PAUSE_HIDDEN)
 * until it shows again. One that is gone, a window closed or an application
 * that quit (also before the run started), pauses it for good
 * (RCS_CAPTURE_PAUSE_GONE): the run and its viewer stay, and the host shares
 * something else (rcs_session_update) or stops. The picture follows a window's
 * size, its aspect ratio kept inside the frame (RCS_STREAM_CONTENT);
 * rcs_stream_info.display_* is the window's or the region's own size.
 *
 * Remote input (rcs_control_config) maps across what is shared: the display,
 * the region, or the window where it is now. With WINDOW and APPLICATION it
 * stays inside: a press or a wheel turn first brings the shared window (the
 * application's window under the pointer) to the front when another window
 * covers that point, and is not executed (RCS_ERROR_INVALID_STATE) where none
 * of what is shared is under the pointer or it cannot come to the front. Key
 * presses and text are executed only while the shared application has the
 * keyboard focus (a press on it gives it the focus) and, with WINDOW, the
 * shared window is its front window (Windows: or a dialog the window owns).
 * Keys that act beyond it whatever has the focus are not executed while input
 * is confined (RCS_ERROR_INVALID_STATE): media keys, and the system's own
 * shortcuts: on Windows the Windows key, Alt+Tab, Alt+Esc, Ctrl+Esc,
 * Ctrl+Shift+Esc and Print Screen; on macOS Command-Tab, Command-Space,
 * Command-Option-Esc, Control-Command-Q, Shift-Command-Q, Shift-Command-3 to
 * 6 and Control with an arrow key. Releases always are. A shortcut another
 * application or the user registered for the whole system (and on macOS
 * Control-Space, the input source's) still reaches its owner when typed
 * into the shared application, as it would at the machine: the confinement
 * keeps input where the viewer sees it, and is no limit on what a viewer
 * allowed to type can do through that application.
 *
 * exclude_current_process (DISPLAY, REGION) hides every window of this
 * process from the viewer, also those it opens while sharing (a notification,
 * a dialog, a menu); the local screen still shows them. Windows hides each new
 * window at the next desktop update and withholds pictures that may still
 * show it; a visible window it cannot hide fails the run with
 * RCS_ERROR_CAPTURE instead of being shared.
 *
 * secure_desktop (Windows) makes capture and remote input follow the desktop
 * that receives input: the sign-in screen, the lock screen, UAC prompts and the
 * Ctrl+Alt+Del screen, which Windows shows on a desktop of their own that
 * ordinary processes cannot capture or control. Off (default): capture pauses
 * while such a desktop is up (RCS_EVENT_CAPTURE) and control pauses with it.
 * On: the picture and the viewer's input go to whatever the user at the
 * machine sees; the viewer then sees passwords being typed (masked) and can
 * answer prompts, so only enable it where the viewer is trusted to do that.
 * Needs rcs_capabilities.secure_desktop (the process running as LocalSystem in
 * a user session: a service's helper started by rcs_supervisor); where the
 * process cannot, the run fails with UNSUPPORTED or PERMISSION_REQUIRED
 * instead of falling back to pausing. RCS_EVENT_DESKTOP tells which desktop is
 * shown. */
typedef struct rcs_capture_config {
    rcs_extension header;
    rcs_capture_source source;
    /* DISPLAY, REGION, APPLICATION: the display (HOST, ENCODED: the one remote
     * input goes to); 0 selects the main display as it is when capture or
     * control starts; see rcs_session_list_displays. */
    uint32_t display_id;
    /* REGION: the rectangle in the display's pixels from its top left, at
     * least 16x16. A later mode change that leaves part of it off the display
     * clips it; none of it on the display pauses capture (HIDDEN). */
    uint32_t region_x, region_y, region_width, region_height;
    uint64_t window_id;               /* WINDOW: rcs_window_info.window_id. */
    uint32_t process_id;              /* APPLICATION: rcs_window_info.process_id. */
    uint32_t exclude_current_process; /* DISPLAY, REGION: 1 hides this application's windows from the viewer. */
    uint32_t secure_desktop;          /* DISPLAY, REGION, Windows: 1 follows secure desktops (see above). */
} rcs_capture_config;
#define RCS_CAPTURE_CONFIG_INIT                                                                                        \
    {{RCS_EXT_CAPTURE, sizeof(rcs_capture_config), NULL}, RCS_CAPTURE_DISPLAY, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}
RCS_INLINE rcs_capture_config rcs_capture_config_init(void) {
    rcs_capture_config value = RCS_CAPTURE_CONFIG_INIT;
    return value;
}

/* More streams: up to RCS_MAX_STREAMS pictures at once, each a display of
 * its own, e.g. to show a machine's displays side by side or as its desktop
 * lays them out. Without it, one stream: RCS_EXT_CAPTURE's. With it, that is
 * stream 0, which then shares a display (source DISPLAY), and displays[] adds
 * streams 1, 2, 3 in order, each a whole display. Each stream is captured and
 * encoded on its own, at the size and frame rate the settings give
 * (rcs_share_video_config, the main viewer's request) fitted to its display,
 * in the codec and picture format of stream 0. The video bitrate (a fixed one,
 * or the adaptive target) is shared among the streams by weights[] (stream 0
 * first; all 0: by each stream's pixels); a stream with nothing new to show
 * leaves its share to the others while it stays still.
 *
 * Each stream has its own announcement (RCS_EVENT_STREAM and
 * rcs_session_get_stream_info: its number and where its display sits on the
 * sharer's desktop, to lay the pictures out as the displays are), frames
 * (rcs_frame.stream), codec, capture and recovery events (rcs_event.stream)
 * and statistics (rcs_session_get_stream_stats). Remote input names the
 * stream its position is on (rcs_input_event.stream); the pointer, sent
 * apart, says which stream it is over (rcs_cursor_event.stream). A viewer may
 * take only some of them (rcs_view_request_config.streams): a stream no viewer
 * takes is not captured.
 *
 * A display named twice, the one stream 0 shares, or one that is not
 * connected (rcs_session_list_displays) is INVALID_ARGUMENT, and so is stream
 * 0 sharing anything but a display. A display disconnected while it is shared
 * pauses its stream as stream 0's would (RCS_CAPTURE_PAUSE_DESKTOP, then the
 * run fails): a host that follows RCS_EVENT_DISPLAYS takes it out of
 * displays[] first. Changing displays[] while running starts and stops the
 * streams it adds and removes; the others go on. */
#define RCS_MAX_STREAMS 4u
typedef struct rcs_streams_config {
    rcs_extension header;
    /* Streams 1, 2, 3: rcs_display_info.display_id each; unused entries 0,
     * after the used ones. */
    uint32_t displays[RCS_MAX_STREAMS - 1];
    uint32_t weights[RCS_MAX_STREAMS]; /* 0..1000 each; all 0: by pixels. */
} rcs_streams_config;
#define RCS_STREAMS_CONFIG_INIT {{RCS_EXT_STREAMS, sizeof(rcs_streams_config), NULL}, {0u, 0u, 0u}, {0u, 0u, 0u, 0u}}
RCS_INLINE rcs_streams_config rcs_streams_config_init(void) {
    rcs_streams_config value = RCS_STREAMS_CONFIG_INIT;
    return value;
}

/* Who executes the viewer's input.
 *   SYSTEM  This machine's mouse and keyboard (default); needs input
 *           permission (rcs_input_permission).
 *   HOST    The host, through on_input; nothing reaches the system. */
typedef uint32_t rcs_input_target;
enum { RCS_INPUT_SYSTEM = 0, RCS_INPUT_HOST = 1 };

/* Remote control. Without it: not allowed. While allowed, the viewer may send
 * input once the sharer announces the grant (RCS_EVENT_CONTROL on both ends).
 * With target SYSTEM the grant also follows the system: a lock screen, a UAC
 * prompt on the secure desktop or withdrawn permission pauses it (held keys
 * are released) and it resumes by itself. Revoking it releases every key and
 * button the viewer holds. on_input sees every event first.
 *
 * host_input_types (target SYSTEM) hands some types to the host, which
 * executes them in on_input while the system executes the rest: e.g.
 * RCS_INPUT_PEN for a drawing program of the host's own, keys and the mouse
 * by the system. The viewer learns that the sharer takes them
 * (rcs_peer_info.input_types). RCS_INPUT_RELEASE_ALL always goes to both.
 * Changing it while running releases what the system and the host hold, as a
 * change of target does. Where text typed now would go on the machine, for
 * the viewer's input method: the text caret module (rcsdk/text_caret.h). */
typedef struct rcs_control_config {
    rcs_extension header;
    /* 1 lets the viewer control. SYSTEM: PERMISSION_REQUIRED without input
     * permission. With several viewers (rcs_viewers_config): what each gets
     * when it joins (on_viewer_join may decide otherwise), and a change sets
     * every connected viewer's (rcs_session_set_viewer_control: one viewer's). */
    uint32_t allowed;
    rcs_input_target target;
    /* SYSTEM: the types the host executes, as (1u << rcs_input_type) bits
     * (RCS_INPUT_RELEASE_ALL goes to both whatever this says); nonzero needs
     * on_input. The system's checks (input permission, what it executes)
     * concern the types left to it: with every type handed over it needs
     * nothing, as with target HOST. */
    uint32_t host_input_types;
} rcs_control_config;
#define RCS_CONTROL_CONFIG_INIT {{RCS_EXT_CONTROL, sizeof(rcs_control_config), NULL}, 0u, RCS_INPUT_SYSTEM, 0u}
RCS_INLINE rcs_control_config rcs_control_config_init(void) {
    rcs_control_config value = RCS_CONTROL_CONFIG_INIT;
    return value;
}

/* How many viewers a sharer serves at once, and how they share the stream.
 * Without it: up to 64 at once; one more that connects meanwhile is refused
 * with RCS_ERROR_BUSY.
 *
 * Each stream is captured once and encoded once for the viewers that take its
 * main encoding, and the packets go to each. A viewer that cannot take that
 * encoding gets a lower one of the same capture instead (RCS_EXT_ENCODINGS: by
 * default the same size at half the frame rate, started when a viewer needs
 * it), so that it does not drag the others down: its path carries less than
 * the encoding actually sends, it keeps losing pictures (each a keyframe for
 * all), it does not decode the encoding's codec, or the host put it there
 * (rcs_session_set_viewer_video). RCS_EVENT_VIEWER_VIDEO says who moved and
 * why. The main viewer - the one that joined first and is still connected,
 * unless the host chose another (rcs_session_set_main_viewer) - decides what
 * the main encoding is: its request (rcs_view_request_config: size, frame
 * rate, bitrate, codec, the pointer) within this end's settings; when it
 * leaves, the next takes over and the stream may restart. The codec is this
 * end's first that every viewer of the main encoding decodes: a viewer that
 * decodes none in use gets a lower encoding in a codec it decodes, or where
 * none can be given (RCS_ENCODINGS_SINGLE, no codec of this end's it decodes)
 * is refused (BUSY, the reason names the codecs); one that stops offering it
 * while others watch moves the same way or is disconnected (BUSY), instead of
 * the stream changing under them. Sound is encoded for each viewer in the
 * codec it negotiated, from one capture.
 *
 * bitrate_from: with adaptive bitrate each viewer's connection measures its
 * own path, and the main encoding follows
 *   QUORUM   the highest target at least quorum_percent of its viewers reach
 *            (default): a viewer whose path carries less than the encoding
 *            actually sends (not its target: a desktop's picture is mostly
 *            far below it) for rcs_video_tuning_config.move_down_ms, or that
 *            dropped video a second behind, moves to a lower encoding; where
 *            none can run, as MINIMUM. Most viewers falling short together is
 *            the sharer's own uplink: no one moves, the bitrate comes down;
 *   MINIMUM  the lowest of their targets: every viewer keeps up and the
 *            slowest sets the quality for all;
 *   MAIN     the main viewer's: a slower viewer moves as with QUORUM, or
 *            where none can run gets what its path carries and, more than a
 *            second behind, skips to the next keyframe.
 * A lower encoding's bitrate follows the lowest target of its viewers. A
 * viewer moves back up once its path carried what the encoding above sends,
 * with room to spare, for move_up_ms. A viewer counts once its path was
 * measured. The viewer that holds an encoding
 * back is RCS_EVENT_RATE's viewer_id (the main encoding's) and shows in
 * rcs_session_get_viewer_stats; the host may move it, let it hold no one back
 * (rcs_session_set_viewer_video) or disconnect it.
 *
 * max_viewers: how many the host lets in at once. 0 (the default) is 64;
 * otherwise 1..RCS_MAX_VIEWERS. Each viewer costs the sharing machine
 * what docs/guide/07-sharing.md lists (threads, descriptors, memory, CPU,
 * its upload; over RELAY an allocation on the TURN server); on_viewer_join refuses whom
 * the host will not take, rcs_session_disconnect_viewer sends one away. A
 * viewer the system has no thread or descriptor for does not get in (it hears
 * RCS_ERROR_BUSY where it can still be told, else its attempt times out); the
 * run and the other viewers go on, and the log says why. Over P2P and RELAY
 * each viewer's path is found by an ICE exchange of its own
 * (rcs_session_signal).
 *
 * While running: raising max_viewers applies at once; lowering it below the
 * viewers connected refuses newcomers and disconnects nobody. */
typedef uint32_t rcs_bitrate_from;
enum { RCS_BITRATE_FROM_QUORUM = 0, RCS_BITRATE_FROM_MINIMUM = 1, RCS_BITRATE_FROM_MAIN = 2 };
#define RCS_MAX_VIEWERS 256u
typedef struct rcs_viewers_config {
    rcs_extension header;
    uint32_t max_viewers; /* 0 (64), or 1..RCS_MAX_VIEWERS */
    rcs_bitrate_from bitrate_from;
    uint32_t quorum_percent; /* QUORUM: 1..100, default 50; 100 takes the lowest target, as MINIMUM. */
    /* How long a viewer waits for the host's decision (RCS_ADMIT_LATER)
     * before it is refused (RCS_ERROR_TIMEOUT): 1 000..3 600 000 ms, or 0 for
     * no limit; default 60 000. */
    uint32_t admission_timeout_ms;
} rcs_viewers_config;
#define RCS_VIEWERS_CONFIG_INIT                                                                                        \
    {{RCS_EXT_VIEWERS, sizeof(rcs_viewers_config), NULL}, 0u, RCS_BITRATE_FROM_QUORUM, 50u, 60000u}
RCS_INLINE rcs_viewers_config rcs_viewers_config_init(void) {
    rcs_viewers_config value = RCS_VIEWERS_CONFIG_INIT;
    return value;
}

/* More encodings of each stream, from the same capture, for the viewers that
 * cannot take the main one (rcs_viewers_config), and the temporal layers of
 * each. Without it: AUTO, at most two, no layers.
 *
 * mode:
 *   AUTO    One encoding while every viewer takes it, as without groups; a
 *           lower one starts when a viewer needs it and stops once none used
 *           it for rcs_video_tuning_config.lower_idle_ms. One the system
 *           cannot start (its encoder sessions are all in use: NVENC on a
 *           GeForce card has 8 to 12 per machine, every program's together)
 *           is reported (RCS_EVENT_ENCODING with the reason,
 *           rcs_group_stats.encodings_refused); its viewers stay where they
 *           were, and it is tried again 30 s later.
 *   SINGLE  One encoding for all: a viewer that cannot take it skips to
 *           keyframes (MAIN) or holds the others back (QUORUM is MINIMUM
 *           then), and one that decodes none of its codecs is refused (BUSY).
 *   FIXED   count encodings run while any viewer watches, used or not; one
 *           that cannot start fails the run, saying why (the host asked for
 *           it).
 * count: the most encodings of a stream, the main one included,
 * 1..RCS_MAX_ENCODINGS; default 2.
 * lower[]: encodings 1 and 2:
 *   frames_per_second  At most this many: 0 is half the main encoding's
 *                      (encoding 2: a quarter); else 1..240, at most the
 *                      main encoding's.
 *   max_pixel_rate     Width x height x frames per second at most: the main
 *                      encoding's size, made smaller in the steps of
 *                      rcs_video_tuning_config.degradation (75, 50, 38 and 25
 *                      percent of each side) until it fits. 0 is 1920 x 1080
 *                      x 30, so that a lower encoding of a 4K60 stream costs
 *                      its encoder an eighth of the main one, not half; else
 *                      at least 16 x 16 x 1.
 *   codecs             All 0: this end's (rcs_encoder_config.codecs), the
 *                      first every viewer of the encoding decodes; else
 *                      those, most preferred first, each one this end offers
 *                      (INVALID_ARGUMENT otherwise), e.g. H.264 alone for
 *                      viewers that decode nothing else.
 *   scalability        Its temporal layers, as scalability below.
 * scalability: the main encoding's temporal layers (names as W3C's
 * WebRTC-SVC): L1T1 (default) none; L1T2 two, every other picture in the
 * upper one; L1T3 three, a quarter of the pictures in layer 0, a quarter in
 * layer 1, half in layer 2. A picture refers to pictures of its own layer and
 * those below only, so a viewer that falls short may get the lower layers
 * alone (half or a quarter of the frame rate, at once and without a
 * keyframe) before it moves to a lower encoding, and gets them all again
 * once its path carries them (RCS_EVENT_VIEWER_VIDEO; rcs_viewer_video.layers
 * fixes it). The encoder makes as many of them as it can
 * (rcs_codec_implementation.temporal_layers): rcs_codec_event and
 * RCS_EVENT_ENCODING say how many (temporal_layers), the viewers'
 * rcs_stream_info too (layers). Layers cost the viewers of every layer: at
 * the same bitrate each picture is worse, by 2 to 4 dB of PSNR mostly on
 * this SDK's own measurements of screen content (tools/rcsdk_encoder_layers,
 * docs/PERFORMANCE.md), so no encoder layers unless
 * asked. The host's own bitstream (capture source
 * ENCODED) has the layers its packets say (rcs_encoded_packet.temporal_id),
 * as scalability declares them.
 * Each encoding's keyframes are its own: a viewer that joins, loses a picture
 * or falls behind costs the viewers of its encoding a keyframe, not the
 * others. A viewer moves between encodings at a keyframe of the one it goes
 * to; its picture keeps going (its RCS_EVENT_STREAM says RCS_STREAM_ENCODING;
 * a new codec or size opens a new decoder there). Each encoding has its own
 * encoder statistics (rcs_session_get_encoding_stats) and events
 * (RCS_EVENT_ENCODING; the main encoding's are RCS_EVENT_STREAM and
 * RCS_EVENT_CODEC as before).
 *
 * Capture source ENCODED (the host's own bitstream) has one encoding: AUTO
 * keeps it at that and FIXED with count above 1 is UNSUPPORTED. The host's
 * own encoder (RCS_EXT_HOST_ENCODER) is opened once per encoding
 * (rcs_host_encoder_settings.encoding). The host gets the main encoding's
 * packets (RCS_EXT_ENCODED_OUTPUT) unless it asks for others.
 *
 * While running: a change applies at the stream's next turn; encodings beyond
 * a lower count stop, and their viewers move up. */
typedef uint32_t rcs_encodings_mode;
enum { RCS_ENCODINGS_AUTO = 0, RCS_ENCODINGS_SINGLE = 1, RCS_ENCODINGS_FIXED = 2 };
typedef uint32_t rcs_scalability;
enum { RCS_SCALABILITY_L1T1 = 1, RCS_SCALABILITY_L1T2 = 2, RCS_SCALABILITY_L1T3 = 3 };
#define RCS_MAX_ENCODINGS 3u
typedef struct rcs_encoding_setting {
    uint32_t frames_per_second;
    uint32_t max_pixel_rate;
    rcs_codec codecs[RCS_MAX_CODECS];
    rcs_scalability scalability;
} rcs_encoding_setting;
typedef struct rcs_encodings_config {
    rcs_extension header;
    rcs_encodings_mode mode;
    uint32_t count;
    rcs_encoding_setting lower[RCS_MAX_ENCODINGS - 1];
    rcs_scalability scalability;
} rcs_encodings_config;
#define RCS_ENCODINGS_CONFIG_INIT                                                                                      \
    {{RCS_EXT_ENCODINGS, sizeof(rcs_encodings_config), NULL},                                                          \
     RCS_ENCODINGS_AUTO,                                                                                               \
     2u,                                                                                                               \
     {{0u, 0u, {0}, RCS_SCALABILITY_L1T1}, {0u, 0u, {0}, RCS_SCALABILITY_L1T1}},                                       \
     RCS_SCALABILITY_L1T1}
RCS_INLINE rcs_encodings_config rcs_encodings_config_init(void) {
    rcs_encodings_config value = RCS_ENCODINGS_CONFIG_INIT;
    return value;
}

/* A sharer's viewers are numbered 1, 2, ... in the order they join a run
 * (from rcs_session_start to its end), never reused within it; 0 is no
 * viewer in particular. */
typedef uint32_t rcs_viewer_id;

/* ---- Viewing ------------------------------------------------------------ */

/* How the viewer gets the sharer's mouse pointer.
 *   IN_VIDEO  Drawn into the picture as the sharer's screen shows it; the
 *             host does nothing (default).
 *   SEPARATE  Left out of the picture and sent on its own: RCS_EVENT_CURSOR
 *             reports its position, visibility and shape, and the host draws
 *             it, e.g. as its own system cursor over the view (which then
 *             follows local mouse moves at once) or as an overlay. Moving the
 *             pointer then costs no video. */
typedef uint32_t rcs_cursor_mode;
enum { RCS_CURSOR_IN_VIDEO = 0, RCS_CURSOR_SEPARATE = 1 };

/* What the viewer asks the sharer for: the picture, the sound and the
 * pointer. Without it, or with a field 0: the sharer's choice. The sharer
 * applies requests within its policy (rcs_share_video_config) and announces
 * what it sends (RCS_EVENT_STREAM, rcs_stream_info: display_width and
 * display_height are the size a request for the native resolution names). */
typedef struct rcs_view_request_config {
    rcs_extension header;
    /* The picture. A size as rcs_share_video_config.width takes, both or
     * neither; the sharer scales one beyond its limits, or beyond what the
     * codec, its encoder or this end's decoder takes, down to fit, keeping
     * the aspect ratio. */
    uint32_t width;
    uint32_t height;
    uint32_t frames_per_second; /* 1..240 */
    uint32_t bitrate_bps;       /* 128 kbit/s..500 Mbit/s; the most where the bitrate adapts. */
    rcs_codec codec;            /* One of this end's rcs_decoder_config codecs[]. */
    rcs_picture_format format;  /* One of this end's rcs_decoder_config formats[]. */
    /* The streams to receive (rcs_streams_config), as (1u << stream) bits; 0:
     * all the sharer sends. One the sharer does not send is ignored. */
    uint32_t streams;
    /* The sound. */
    rcs_audio_codec audio_codec; /* One of this end's rcs_receive_audio_config codecs[]. */
    /* 16 000..510 000; the sharer keeps it within what its codec takes (on
     * Windows AAC: the nearest of 96 000, 128 000, 160 000, 192 000). */
    uint32_t audio_bitrate_bps;
    /* The pointer; not the sharer's choice, so never 0 by default. */
    rcs_cursor_mode cursor;
} rcs_view_request_config;
#define RCS_VIEW_REQUEST_CONFIG_INIT                                                                                   \
    {{RCS_EXT_VIEW_REQUEST, sizeof(rcs_view_request_config), NULL}, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,              \
     RCS_CURSOR_IN_VIDEO}
RCS_INLINE rcs_view_request_config rcs_view_request_config_init(void) {
    rcs_view_request_config value = RCS_VIEW_REQUEST_CONFIG_INIT;
    return value;
}

/* The decoder and its queue. Without it: H.264, 8-bit 4:2:0 SDR pictures,
 * hardware, the platform's backend, an SDK-made device, 64 frames / 80 ms.
 * Codecs and picture formats negotiate as described at rcs_encoder_config.
 *
 * formats[]: the picture formats the host takes. A stream of another format
 * never reaches it, so list only those its renderer handles: GPU frames come
 * in the format's own layout (rcs_gpu_frame_info.format: P010 for 10-bit
 * 4:2:0, NV24 and P410 for 4:4:4), with the values PQ-coded for HDR10;
 * Windows' software decoders give BGRA8 textures whatever the format. CPU
 * frames (on_frame) are BGRA8 SDR, converted from any SDR format; HDR10 with
 * on_frame is UNSUPPORTED, unless the host's decoder handles every listed
 * codec. The order is a habit only: the sharer's order decides.
 *
 * acceleration HARDWARE_FIRST (see rcs_acceleration): a codec is offered
 * with the formats its first hardware and its first software decoder take
 * together, at the smaller of their largest pictures where the software one
 * adds formats. */
/* A codec the host's decoder handles (RCS_EXT_HOST_DECODER) is offered
 * whatever the acceleration and backend; a host codec id needs one. */
typedef struct rcs_decoder_config {
    rcs_extension header;
    rcs_codec codecs[RCS_MAX_CODECS];                     /* At least one; no duplicates. */
    rcs_picture_format formats[RCS_MAX_PICTURE_FORMATS]; /* At least one; no duplicates. */
    rcs_acceleration acceleration;
    rcs_backend backend;
} rcs_decoder_config;
#define RCS_DECODER_CONFIG_INIT                                                                                        \
    {{RCS_EXT_DECODER, sizeof(rcs_decoder_config), NULL},                                                              \
     {RCS_CODEC_H264, 0u, 0u},                                                                                         \
     {RCS_PICTURE_420_8, 0u},                                                                                          \
     RCS_ACCELERATION_HARDWARE,                                                                                        \
     RCS_BACKEND_AUTO}
RCS_INLINE rcs_decoder_config rcs_decoder_config_init(void) {
    rcs_decoder_config value = RCS_DECODER_CONFIG_INIT;
    return value;
}

/* ---- Sound, both ways ---------------------------------------------------- */

/* Each end sends sound (RCS_EXT_SEND_AUDIO) and receives the other's
 * (RCS_EXT_RECEIVE_AUDIO), each direction on its own: the sharer's machine to
 * its viewers, and a viewer's microphone (or the host's sound) to the sharer,
 * e.g. someone helping who talks while they work. Without the extensions a
 * sharer sends its machine's sound and receives none; a viewer receives and
 * plays the sharer's and sends none.
 *
 * A direction flows while its sender sends and its receiver receives, in a
 * codec both have: the sender's list in its order, the receiver's (a viewer
 * may ask the sharer for one: rcs_view_request_config). Each end offers its
 * codecs (those this system has) whether or not it enables sound, so a
 * direction can be turned on later in the run. When both ends start a
 * direction and have no codec in common, both fail with UNSUPPORTED; an end
 * that has it off is no failure: both report it (RCS_EVENT_AUDIO, with its
 * direction). Sound never stops the picture: when it cannot be captured or
 * played (no device, a device removed, a setting the device cannot do, no
 * microphone permission), it pauses, RCS_EVENT_AUDIO says why, and it resumes
 * by itself once it can. Changing it while running restarts that direction.
 *
 * A sharer receives each viewer's sound on its own: played together on the
 * system's output, or each through on_audio (viewer_id). Viewers do not hear
 * each other.
 *
 * Echo. A viewer's microphone hears what its speakers play of the sharer's
 * sound: voice processing (on by default) cancels that where the system has
 * it (RCS_EVENT_AUDIO's voice_processing says whether it runs). A sharer that
 * plays its viewers' sound while it sends what its machine plays would send
 * them their own voices back: playing them takes the sharer's own sound out
 * of what it sends (exclude_current_process, needing audio_exclusion, with
 * the agent in the session's process), the host's sound instead (source
 * HOST), or the host's own playback (rcs_receive_audio_config.playback 0);
 * otherwise start is UNSUPPORTED and says so. */

/* Where the sound comes from.
 *   SYSTEM      Everything this machine plays, by capture_backend. A
 *               sharer's only: a viewer would send back what it plays of the
 *               sharer's sound (UNSUPPORTED).
 *   HOST        The host's own sound, pushed with rcs_session_push_audio,
 *               or as sample buffers (rcsdk/apple.h): the applications'
 *               sound and the microphone, mixed.
 *   MICROPHONE  A microphone (device, voice_processing); needs the
 *               microphone permission (rcs_microphone_permission). A
 *               sharer's agent records it on its machine. */
typedef uint32_t rcs_audio_source;
enum { RCS_AUDIO_SYSTEM = 0, RCS_AUDIO_HOST = 1, RCS_AUDIO_MICROPHONE = 2 };

/* The sound this end sends. RCS_SEND_AUDIO_CONFIG_INIT: on, Opus, AAC, PCM in
 * that order, 128 kbit/s, every sound the machine plays (this application's
 * too), captured by this system's best backend at its default period; a
 * viewer that sends sets source MICROPHONE or HOST.
 *
 * Where the delay goes, sharer to viewer: the capture period (plus up to one
 * more, as sound falls into it), about 2.5 ms of Opus, the network, the
 * receiver's buffer (rcs_audio_tuning_config) and its output device. */
typedef struct rcs_send_audio_config {
    rcs_extension header;
    uint32_t enabled;
    /* At least one; no duplicates; unused entries 0. While enabled, one this
     * system encodes (rcs_capabilities.audio_encoders), else UNSUPPORTED. */
    rcs_audio_codec codecs[RCS_MAX_AUDIO_CODECS];
    /* OPUS: 16 000..510 000; a packet takes at most 1024 bytes, so 20 ms
     * packets (ScreenCaptureKit capture) carry at most about 400 kbit/s. AAC:
     * 64 000..320 000 at a constant bitrate; the encoders of Windows and
     * Android take only 96 000, 128 000, 160 000 and 192 000 and run at the
     * nearest of them (rcs_audio_send_stats.bitrate_bps). Ignored for PCM. */
    uint32_t bitrate_bps;
    rcs_audio_source source;
    /* SYSTEM: 1 leaves this application's own sounds out, e.g. when it plays
     * the peer's sound itself, which would echo back. CORE_AUDIO_TAP and
     * WASAPI_LOOPBACK also leave out the processes it started (e.g. Chromium
     * or Electron helpers); not, on macOS, XPC services launchd starts for it.
     * SCREENCAPTUREKIT leaves out this process only. Needs audio_exclusion
     * (rcs_capabilities); where the system cannot, sound pauses and says so. */
    uint32_t exclude_current_process;
    /* MICROPHONE: the device (rcs_list_audio_devices, an input), 0 for the
     * system's default input, followed when it changes. */
    uint32_t device;
    /* MICROPHONE: 1 asks the system to cancel the echo of what this machine
     * plays and to suppress noise (macOS: voice processing; Windows: the
     * device's own, where its driver has it); 0 records the microphone as it
     * is (a headset, music). */
    uint32_t voice_processing;
    /* HOST from Core Media sample buffers (rcs_session_push_sample_buffer,
     * rcsdk/apple.h), which ReplayKit hands over each sound on its own: the
     * volume of what the device or the applications play
     * (RCS_SAMPLE_BUFFER_AUDIO) and of the microphone
     * (RCS_SAMPLE_BUFFER_MICROPHONE) in the one sound sent, in percent,
     * 0..400 (100: as it is; the mix is held within full scale). 0 leaves
     * that one out: its buffers are taken and dropped, and the other is
     * sent as it comes. Both 100 by default: someone sharing a phone's
     * screen usually talks over what it shows. Applies at once, without
     * restarting the sound. */
    uint32_t sample_buffer_audio_percent;
    uint32_t sample_buffer_microphone_percent;
} rcs_send_audio_config;
#define RCS_SEND_AUDIO_CONFIG_INIT                                                                                     \
    {{RCS_EXT_SEND_AUDIO, sizeof(rcs_send_audio_config), NULL},                                                        \
     1u,                                                                                                               \
     {RCS_AUDIO_OPUS, RCS_AUDIO_AAC, RCS_AUDIO_PCM},                                                                   \
     128000u,                                                                                                          \
     RCS_AUDIO_SYSTEM,                                                                                                 \
     0u,                                                                                                               \
     0u,                                                                                                               \
     1u,                                                                                                               \
     100u,                                                                                                             \
     100u}
RCS_INLINE rcs_send_audio_config rcs_send_audio_config_init(void) {
    rcs_send_audio_config value = RCS_SEND_AUDIO_CONFIG_INIT;
    return value;
}

/* The sound this end receives and plays. RCS_RECEIVE_AUDIO_CONFIG_INIT: on,
 * Opus, AAC, PCM offered, played on the system's default output device
 * (following it when it changes) with a buffer that follows the network.
 * These defaults balance delay against robustness; every part can be tuned
 * (rcs_audio_tuning_config). A sharer receives each viewer's sound. */
typedef struct rcs_receive_audio_config {
    rcs_extension header;
    uint32_t enabled;
    /* At least one; no duplicates; unused entries 0. While enabled, one this
     * system decodes (rcs_capabilities.audio_decoders; any for a viewer that
     * records only, rcs_encoded_output_config.decode 0), else UNSUPPORTED. */
    rcs_audio_codec codecs[RCS_MAX_AUDIO_CODECS];
    /* 1 plays on the system's default output device; 0 leaves playback to
     * the host, which gets the sound through on_audio (called in both cases). */
    uint32_t playback;
} rcs_receive_audio_config;
#define RCS_RECEIVE_AUDIO_CONFIG_INIT                                                                                     \
    {{RCS_EXT_RECEIVE_AUDIO, sizeof(rcs_receive_audio_config), NULL},                                                      \
     1u,                                                                                                               \
     {RCS_AUDIO_OPUS, RCS_AUDIO_AAC, RCS_AUDIO_PCM},                                                                   \
     1u}
RCS_INLINE rcs_receive_audio_config rcs_receive_audio_config_init(void) {
    rcs_receive_audio_config value = RCS_RECEIVE_AUDIO_CONFIG_INIT;
    return value;
}

/* ---- Both --------------------------------------------------------------- */

/* What a session pauses (rcs_session_pause, rcs_background_config,
 * RCS_EVENT_PAUSE), as bits.
 *   VIDEO  A viewer stops decoding and delivering pictures and closes its
 *          decoders, and its sharer stops sending it pictures: its streams
 *          end there (RCS_EVENT_STREAM with RCS_STREAM_ENDED), and a stream
 *          no viewer takes is no longer captured or encoded. A sharer stops
 *          capturing, encoding and sending pictures to every viewer: their
 *          streams end, and they hear that the sharer paused
 *          (rcs_pause_event.by_peer). Resuming, the streams are announced
 *          again and start from a keyframe.
 *   AUDIO  A viewer stops playing the sharer's sound (and, with
 *          RCS_AUDIO_SESSION_SDK, no longer holds the audio session), and the
 *          sharer stops sending it; a sharer stops sending its sound to every
 *          viewer. Resuming starts it again as set now.
 * The connection, input, data, modules and the other direction's sound go on. */
enum { RCS_PAUSE_VIDEO = 1u << 0, RCS_PAUSE_AUDIO = 1u << 1 };

/* What the session pauses by itself while the application is in the
 * background (iOS: no scene of it in front; a backgrounded application may
 * not use the GPU, and the system invalidates its decoders and encoders),
 * resumed when it comes back: RCS_PAUSE_* bits. RCS_BACKGROUND_CONFIG_INIT:
 * RCS_PAUSE_VIDEO. 0 leaves it to the host (rcs_session_pause), e.g. a viewer
 * that records without decoding (rcs_encoded_output_config.decode 0) in the
 * background. Systems without that state (macOS, Windows) never pause by it. */
typedef struct rcs_background_config {
    rcs_extension header;
    uint32_t pause;
} rcs_background_config;
#define RCS_BACKGROUND_CONFIG_INIT {{RCS_EXT_BACKGROUND, sizeof(rcs_background_config), NULL}, RCS_PAUSE_VIDEO}
RCS_INLINE rcs_background_config rcs_background_config_init(void) {
    rcs_background_config value = RCS_BACKGROUND_CONFIG_INIT;
    return value;
}

/* How the two ends reach each other (rcs_session_config.connect.transport);
 * both must use the same, and it is fixed for a run. Default UDP.
 *   UDP  The sharer listens on connect.address:port, the viewer sends
 *        there, over UDP datagrams (IPv4 or IPv6): video is paced, lost
 *        pieces are resent selectively and stale ones dropped, input travels
 *        apart from video, and the bitrate follows the path. Needs the port
 *        reachable, e.g. on a LAN.
 *   TCP  The same addresses and the same datagrams over a TCP connection
 *        the viewer makes (another one when its network changes:
 *        rcs_paths_config): TCP's own retransmission holds up what follows a
 *        lost packet. For networks that pass TCP only.
 *   P2P  Direct UDP across NATs, found with ICE over signals the host carries
 *        through its own channel (on_signal, rcs_session_signal): a viewer
 *        signals first, at each connection; the sharer answers each viewer's
 *        signal with one of its own, the path to that viewer alone (its own
 *        candidates and port), so that it serves several viewers as over UDP.
 *        Covers LANs, IPv6, and most home and mobile NATs with a STUN server;
 *        when no direct path exists, the attempt fails (RCS_ERROR_NETWORK, the
 *        reason in rcs_session_get_failure) and the host decides what to do
 *        next, e.g. a relay. Needs on_signal.
 *   RELAY  Like P2P, but every datagram passes the host's TURN server
 *        (RFC 8656, e.g. coturn): works whenever both ends reach that server,
 *        over UDP or, where only TCP or only TLS passes, over one TCP or TLS
 *        connection to it (turn_transport). Each path has an allocation of its
 *        own: a sharer holds one per viewer. The relay sees only ciphertext.
 *        Needs on_signal and turn_server (rcs_ice_servers_config).
 *   HOST  Over a channel the host already has to the other end: its own
 *        server's connection, a WebRTC data channel, QUIC, an SSH tunnel.
 *        The SDK hands each datagram to the host's on_transport_send and takes the
 *        peer's through rcs_session_transport_receive; the connection inside
 *        is the UDP one (handshake, end-to-end encryption, selective resends,
 *        pacing, adaptive bitrate), so the channel and any server on it see
 *        only ciphertext. host_channel says what the channel is. The host
 *        pairs the ends: a sharer waits (LISTENING) for its viewers'
 *        handshakes, each over the channel's route to that viewer, as many
 *        as max_viewers; a viewer sends its handshake at start. Needs
 *        rcs_host_transport_config (rcsdk/host_transport.h).
 *   FORWARDER  Through a forwarder (server/forwarder, rcsfwd.h), for groups:
 *        the sharer sends each picture, and each packet of its sound, once,
 *        sealed with a stream key that only the viewers it let in hold, and
 *        the forwarder gives each viewer its copy, with resends, parity,
 *        pacing and adaptive bitrate of its own, moving a viewer whose path
 *        falls far behind to a lower encoding at once (RCS_EXT_ENCODINGS); a
 *        viewer that comes later starts on the newest keyframe the forwarder
 *        kept. Everything else (handshake, admission, control, input, data,
 *        modules, the viewer's sound, the pointer, the stream's
 *        announcements) is each viewer's own end-to-end connection with the
 *        sharer, which the forwarder carries as it is and can neither read
 *        nor change: it can drop, delay and reorder, as any relay. Both ends
 *        connect to the forwarder (rcs_forwarder_config.link: over UDP or TCP
 *        at connect.address and connect.port, a WebSocket, or the host's own
 *        channel to it: a browser's); neither opens a port, finds a path or
 *        needs signalling. A sharer is LISTENING once
 *        the forwarder took it into its room; a viewer whose room has no
 *        sharer fails as one with no sharer listening (NETWORK). Needs
 *        rcs_forwarder_config.
 *   For P2P and RELAY, connect.address is ignored and connect.port is the
 *   local UDP port (0: any): a viewer's; a sharer's paths take one each, the
 *   first free from connect.port up, among the max_viewers + 32 ports there
 *   (rcs_viewers_config). HOST uses neither. */
typedef uint32_t rcs_transport;
enum { RCS_TRANSPORT_UDP = 0, RCS_TRANSPORT_TCP = 1, RCS_TRANSPORT_P2P = 2, RCS_TRANSPORT_RELAY = 3,
       RCS_TRANSPORT_HOST = 4, RCS_TRANSPORT_FORWARDER = 5 };
/* RELAY: how this end reaches the TURN server; the relay forwards UDP between
 * the ends either way. TCP is for networks that pass TCP only (e.g. a coturn
 * listening on 443); the path then carries video as datagrams inside one TCP
 * stream, with TCP's own retransmission underneath. TLS is the same stream
 * inside TLS 1.2 or later ("turns"), for networks that look at what passes
 * and let little but HTTPS through: the system's TLS (macOS Network.framework;
 * Windows Schannel, from Windows 10 1809, TLS 1.3 from Windows 11), offering
 * the application protocol (ALPN) "stun.turn" so that a server on 443 can tell
 * TURN from HTTPS. How the server's certificate is checked:
 * turn_certificate_sha256 and turn_tls_server_name. */
typedef uint32_t rcs_turn_transport;
enum { RCS_TURN_UDP = 0, RCS_TURN_TCP = 1, RCS_TURN_TLS = 2 };
#define RCS_MAX_STUN_SERVERS 4
/* P2P and RELAY: the servers ICE uses (both ends). They may change while
 * running and apply from the next connection, e.g. new time-limited TURN
 * credentials for a sharer that keeps listening; turn_transport is fixed for a
 * run (changing it while running is INVALID_STATE). */
typedef struct rcs_ice_servers_config {
    rcs_extension header;
    /* P2P: STUN servers, "host:port" (a name, IPv4, or [IPv6]); unused entries
     * empty. They tell each end its public address; the list is redundancy: a
     * server that does not resolve or answer is left out (and logged). Without
     * any, only addresses of this machine's interfaces are offered (LAN, IPv6). */
    char stun_servers[RCS_MAX_STUN_SERVERS][128];
    /* RELAY: the TURN server ("host:port"; without a port 3478, over TLS
     * 5349), how to reach it, and its long-term credentials, e.g.
     * time-limited ones the host's backend issues per session. With a
     * username the server must ask for them: an allocation granted without
     * (anyone on the path could answer so) is refused
     * (RCS_ERROR_AUTHENTICATION, saying so); a server without
     * authentication takes an empty username. */
    char turn_server[128];
    rcs_turn_transport turn_transport;
    char turn_username[128];
    char turn_password[128];
    /* RCS_TURN_TLS: how the TURN server proves itself. Both stay empty with
     * another turn_transport (INVALID_ARGUMENT otherwise: a pin nothing checks
     * would leave the server unverified).
     * turn_certificate_sha256: all zero (the default): the system's trust store
     * and the server's name, as a browser checks them (Android: the system's
     * authorities the user did not turn off; never those a user installed,
     * which an application trusts from Android 7 only where its network
     * security configuration says so). Otherwise the SHA-256
     * of the server's own certificate (its DER, e.g. `openssl x509 -outform
     * der | openssl dgst -sha256`), and exactly that certificate is accepted,
     * whatever its chain, name or dates: a relay with a self-signed one. A
     * renewed certificate has another SHA-256, so the host updates the pin
     * with it. A TLS-inspecting proxy whose authority the system trusts passes
     * the system's check but never a pin.
     * turn_tls_server_name: the name sent to the server (SNI) and, without a
     * pin, the one its certificate must be for; a DNS name, not an address.
     * Empty: the host part of turn_server. A turn_server that is an IP address,
     * or whose host is no DNS name, needs a pin or a name (INVALID_ARGUMENT
     * otherwise: no name to check).
     * A certificate that fails ends the attempt with RCS_ERROR_AUTHENTICATION,
     * the reason saying which check: the pin, the chain, the name, the dates. */
    uint8_t turn_certificate_sha256[32];
    char turn_tls_server_name[128];
} rcs_ice_servers_config;
#define RCS_ICE_SERVERS_CONFIG_INIT                                                                                    \
    {{RCS_EXT_ICE_SERVERS, sizeof(rcs_ice_servers_config), NULL}, {{0}}, {0}, RCS_TURN_UDP, {0}, {0}, {0}, {0}}
RCS_INLINE rcs_ice_servers_config rcs_ice_servers_config_init(void) {
    rcs_ice_servers_config value = RCS_ICE_SERVERS_CONFIG_INIT;
    return value;
}

/* FORWARDER: how an end reaches the forwarder, for networks that drop UDP;
 * the hop inside is the same each way, authenticated end to end with the
 * forwarder's key.
 *   UDP        Datagrams to connect.address:connect.port (its listen_udp).
 *   TCP        One TCP connection to connect.address:connect.port (its
 *              listen_tcp), each datagram behind its length: TCP's
 *              retransmission underneath, so a loss holds up what follows.
 *   WEBSOCKET  One WebSocket (RFC 6455) to websocket_url: "ws://" to its
 *              listen_websocket, or "wss://" to a TLS-terminating proxy in
 *              front of it (the forwarder speaks no TLS), for networks that
 *              let little but HTTPS through. TLS is the system's (as TURN
 *              over TLS: rcs_ice_servers_config), offering ALPN "http/1.1";
 *              the proxy's certificate is checked by the system's trust store
 *              and the URL's host name, or tls_certificate_sha256. Only what
 *              the host gave goes out: no proxy settings of the system are
 *              used (a network that allows only its HTTP proxy is not
 *              reached: HOST is for that).
 *   HOST       The host's own channel to the forwarder, as RCS_TRANSPORT_HOST
 *              carries a session (RCS_EXT_HOST_TRANSPORT, required with it;
 *              its route is 0 on both ends): DATAGRAM, each datagram one
 *              message, as the forwarder's WebSocket entry takes them (a
 *              browser page's WebSocket to it, or to a TLS proxy in front of
 *              it; the browser checks the certificate) or its UDP port does;
 *              STREAM, each datagram behind its length, as its TCP entry
 *              takes them (e.g. through an HTTP proxy's CONNECT). The hop
 *              sends datagrams of up to 1200 bytes and the forwarder too:
 *              host_max_datagram is 0 or at least 1200. One connection to an
 *              entry is one hop: the host calls rcs_session_transport_closed
 *              when it ends, and the end connects anew over the next channel
 *              the host opens (a sharer that keeps listening, a viewer as it
 *              reconnects). The browser's only link (forwarder_links of
 *              rcs_capabilities). */
typedef uint32_t rcs_forwarder_link;
enum { RCS_FORWARDER_UDP = 0, RCS_FORWARDER_TCP = 1, RCS_FORWARDER_WEBSOCKET = 2, RCS_FORWARDER_HOST = 3 };

/* FORWARDER (both ends; required with it, INVALID_ARGUMENT without it or
 * with another transport): which forwarder, and the host's token for its
 * room. The forwarder carries the run for viewers the sharer let in, never
 * more: the stream key goes end to end, after the sharer's admission.
 *
 * forwarder_key_sha256: the SHA-256 of the forwarder's public key (its
 *   identity: 65 bytes, 0x04 || X || Y; rcsfwd_public_key_sha256), which it
 *   proves in the handshake. Another key, or none, fails the connection
 *   (AUTHENTICATION) before the token goes out. All zeros: INVALID_ARGUMENT.
 * link: how this end reaches it (above), fixed for a run. UDP and TCP:
 *   connect.address is its address (IPv4, IPv6, or a name, resolved at each
 *   connection: the first address it resolves to), connect.port 1..65 535.
 *   WEBSOCKET and HOST: connect.address stays empty and connect.port 0. A
 *   link this system cannot use is UNSUPPORTED at start (a browser's sockets:
 *   rcs_capabilities.forwarder_links).
 * websocket_url: WEBSOCKET: "ws://" or "wss://", then the host (a name,
 *   IPv4, or [IPv6]), an optional ":port" (default 80 and 443), and the
 *   path ("/" when none; a query may follow), at most 255 bytes, no
 *   fragment, user or spaces. Empty with another link.
 * tls_certificate_sha256: "wss://": the SHA-256 of the proxy's own
 *   certificate (its DER), for one the system does not trust (self-signed),
 *   as turn_certificate_sha256; all zero (the default): the system's trust
 *   store and the URL's host, which must then be a name. All zero with
 *   another link.
 * token: what the host's server gave this end for the forwarder's room
 *   ("rcsf1." tokens, rcsfwd.h; opaque here), UTF-8, NUL-terminated, 1 to
 *   RCS_FORWARDER_TOKEN_SIZE - 1 bytes. A sharer's publishes to the room it
 *   names, a viewer's subscribes to it; the room's one sharer is the first
 *   that publishes. The forwarder refuses a token that is malformed, signed
 *   by no key it holds, for another forwarder, expired, of the other role
 *   or for a room it cannot serve, and a second sharer, a full room or a
 *   full forwarder: the run fails with RCS_ERROR_REFUSED (BUSY: no room),
 *   the forwarder's reason in rcs_session_get_failure. The route a viewer's
 *   token names is its rcs_viewer_info.route at the sharer: the forwarder
 *   vouches for it, nothing end to end does (who a viewer is, its device key
 *   and the connection's secret say).
 * refresh_token, user_data: optional; before each connection to the
 *   forwarder but the session's first (a viewer reconnecting, a sharer that
 *   keeps listening reaching it again, a forwarder that restarted), on the
 *   run's thread: the host may write a fresh token into `token` (`capacity`
 *   bytes, NUL-terminated; it holds the one in use) and return nonzero, which
 *   applies as rcs_session_update_config would (an invalid one fails the run
 *   with INVALID_ARGUMENT); 0 keeps the one in use. It may take its time
 *   (ask the host's server): the attempt waits. Tokens expire; a run that
 *   lives longer than its token needs this, or the host's own updates.
 * key_rotation_ms: share: the stream key also changes this often; 0 (the
 *   default): only when a viewer leaves (and a step on at each keyframe);
 *   otherwise 1 000..86 400 000.
 * from_admission: share: 0 (the default): a viewer let in gets the key of
 *   the newest keyframe the forwarder keeps of what it takes, and starts on
 *   it at once: it opens what was sealed from there, at most the forwarder's
 *   cache_ms (rcsfwd.h) before it was let in. 1: it opens nothing sealed
 *   before then, and starts at the next keyframe (the forwarder asks the
 *   sharer for it).
 * Changes apply from the next connection to the forwarder; key_rotation_ms
 * and from_admission at once; link is fixed for a run (INVALID_STATE). */
#define RCS_FORWARDER_TOKEN_SIZE 1024u
typedef struct rcs_forwarder_config {
    rcs_extension header;
    uint8_t forwarder_key_sha256[32];
    rcs_forwarder_link link;
    char websocket_url[256];
    uint8_t tls_certificate_sha256[32];
    char token[RCS_FORWARDER_TOKEN_SIZE];
    int32_t (*refresh_token)(char* token, uint32_t capacity, void* user_data);
    void* user_data;
    uint32_t key_rotation_ms;
    uint32_t from_admission;
} rcs_forwarder_config;
#define RCS_FORWARDER_CONFIG_INIT                                                                                      \
    {{RCS_EXT_FORWARDER, sizeof(rcs_forwarder_config), NULL}, {0}, RCS_FORWARDER_UDP, {0}, {0}, {0}, NULL, NULL, 0u, 0u}
RCS_INLINE rcs_forwarder_config rcs_forwarder_config_init(void) {
    rcs_forwarder_config value = RCS_FORWARDER_CONFIG_INIT;
    return value;
}

/* What a run does when its connection ends. Without it: a sharer listens for
 * the next viewer; a viewer whose path broke reconnects (for 30 seconds, once
 * a second: reconnect_window_ms and reconnect_interval_ms of
 * rcs_network_tuning_config).
 *
 * A connection ends because the peer closed it (it stopped, or its process
 * ended: RCS_ERROR_DISCONNECTED), because it closed it and said it will be
 * back (rcs_session_stop_with_reason with RCS_STOP_RESTART:
 * RCS_ERROR_RESTARTING), because the path failed (RCS_ERROR_NETWORK: nothing
 * arrived for idle_timeout_ms of rcs_network_tuning_config, and no path came
 * back while the connection was kept, rcs_paths_config; a sharer that no
 * longer knows the connection), or because the peer broke the protocol
 * (RCS_ERROR_PROTOCOL). The state event that follows carries the reason. A
 * path that changes or breaks for a while is no end of the connection
 * (rcs_paths_config, RCS_EVENT_PATH): a viewer goes to RECONNECTING meanwhile
 * and back to RUNNING on the same connection, without new PEER or STREAM
 * events.
 *   Share  keep_listening: whatever a viewer or the path to it causes - it
 *          left, its connection failed, a P2P check with it failed, it offers
 *          no codec in common - takes the run back to LISTENING as at start
 *          instead of ending it; the state event carries the reason. Each
 *          viewer starts afresh: offers, stream, control. Failures of this end
 *          (capture, codecs, its agent, its port; P2P and RELAY: its TURN
 *          server, its interface) end the run while no viewer is connected.
 *          With several viewers (rcs_viewers_config) one that leaves is
 *          reported (RCS_EVENT_VIEWER) and the others stay; this applies when
 *          the last one leaves. One that could not join while others watch
 *          is logged, the others stay.
 *   View   reconnect: when the path breaks (RCS_ERROR_NETWORK) or the
 *          sharer says it is restarting (RCS_ERROR_RESTARTING) after a
 *          connection ran, the run goes to RECONNECTING (it is there already
 *          while a lost path was kept, rcs_paths_config) and connects again,
 *          as at start (P2P and RELAY: on_signal again, the sharer's answer
 *          through rcs_session_signal), every reconnect_interval_ms
 *          (at once after RCS_ERROR_RESTARTING, and once the sharer said it
 *          no longer knows the connection) until it runs again or
 *          reconnect_window_ms have passed since the break, also while waiting
 *          for the sharer's answer; then it ends with RCS_ERROR_NETWORK. The
 *          settings, requests and the stream's start with a keyframe are as at
 *          start. A sharer that closed the connection (RCS_ERROR_DISCONNECTED)
 *          is gone, and one that broke the protocol is not tried again: the
 *          run ends. A first connection that fails ends the run as well, also
 *          one the sharer refuses as busy (RCS_ERROR_BUSY); while
 *          reconnecting, busy is tried again within the window (the sharer may
 *          still hold this end's lost connection for a few seconds).
 * Every connection counts in rcs_network_stats.connections. */
typedef struct rcs_reconnect_config {
    rcs_extension header;
    uint32_t keep_listening;        /* Share: 1 listens again; 0 ends the run. */
    uint32_t reconnect;             /* View: 1 reconnects (rcs_network_tuning_config: for how long, how often); 0 ends the run. */
} rcs_reconnect_config;
#define RCS_RECONNECT_CONFIG_INIT {{RCS_EXT_RECONNECT, sizeof(rcs_reconnect_config), NULL}, 1u, 1u}
RCS_INLINE rcs_reconnect_config rcs_reconnect_config_init(void) {
    rcs_reconnect_config value = RCS_RECONNECT_CONFIG_INIT;
    return value;
}

/* What a change of network does to the connection (both ends, each its own).
 * Without it: the connection follows either end onto a new path, every
 * network may carry it, the system's routes choose the network, and no
 * standby path is kept.
 *
 * UDP, TCP, P2P, RELAY and HOST keep their connection when an end's network
 * changes (Wi-Fi to cellular and back, a cable plugged in, a VPN, a NAT that
 * remaps, a new address): the same keys, viewer, admission and messages in
 * flight, over another path (RCS_EVENT_PATH). The transport stays what it
 * is: P2P finds its new path among direct candidates and never turns to a
 * relay; RELAY stays with its TURN server (RFC 8016 mobility where the server
 * grants it, else a new allocation); UDP between direct addresses; TCP over a
 * new TCP connection the viewer makes to the sharer's address when its own
 * breaks or stalls or its network changes (the sharer's port finds the
 * connection by its id, and the connection proves the new one with its keys);
 * HOST over the host's channel (rcs_session_transport_restarted); FORWARDER
 * moves each end's hop to the forwarder as UDP does over its UDP link, and
 * as HOST does over its HOST link where the forwarder sees the same peer
 * (rcsdk/host_transport.h); otherwise, and over TCP or a WebSocket, a broken
 * hop ends the connection (rcs_reconnect_config). A path that needs new
 * holes in the NATs (P2P, RELAY) is found through the host's signalling:
 * on_signal may hand over a signal at any time of the run (a sharer's with
 * its viewer's route), which the other end passes to rcs_session_signal as
 * any other; such a signal is sealed with the
 * connection's keys, so the host carries it but can neither change nor make
 * one. UDP and TCP have no signalling: a sharer that moved behind a NAT that
 * filters is not found again (P2P crosses NATs).
 *
 * While no path reaches the other end (RCS_PATH_LOST, after idle_timeout_ms
 * of silence), both keep the connection: a viewer reports RECONNECTING
 * (RCS_ERROR_NETWORK) for up to reconnect_window_ms since the break and is
 * RUNNING again on the same connection when a path comes back, without a new
 * RCS_EVENT_PEER or STREAM; a sharer keeps the viewer for path_hold_ms
 * (rcs_network_tuning_config), sends it nothing meanwhile and gives its place
 * to a new viewer that needs it. A sharer that no longer knows the connection
 * (it restarted, or let it go) says so, and the viewer connects anew at once
 * (rcs_reconnect_config).
 *
 *   migrate          1 (default): as above. 0: a new address of the peer is a
 *                    broken path, and a lost path ends the connection after
 *                    idle_timeout_ms (RCS_ERROR_NETWORK; the viewer reconnects).
 *                    One end with 0 makes it so for both.
 *   excluded_networks  The networks this end does not use, as
 *                    (1u << rcs_network_kind) bits, e.g.
 *                    (1u << RCS_NETWORK_CELLULAR); 0 (default): all. A VPN is
 *                    of the kind of the network it runs over. This end's
 *                    sockets are bound to the interface of one allowed
 *                    network (the system's default where it is allowed), so
 *                    that nothing goes over an excluded one even for a moment;
 *                    when that network goes or is excluded, the connection
 *                    moves to another allowed one, and while none is left it
 *                    is lost at once (RCS_PATH_LOST says why) and waits as for
 *                    a lost path. UNSUPPORTED where nothing is this end's to
 *                    bind (HOST and a forwarder's link HOST, whose networks
 *                    the host chooses; a sharer over UDP or TCP, which is
 *                    reached where it listens) and where the system does not say
 *                    which network is which (rcs_capabilities.network_facts
 *                    without KIND: Android without its Java VM,
 *                    rcs_android_initialize). INVALID_ARGUMENT with
 *                    rcs_network_tuning_config.local_interface, which chooses
 *                    the one interface itself.
 *   policy           FOLLOW_SYSTEM (default): when the system's default
 *                    network changes (Wi-Fi back, a cable plugged in), the
 *                    connection moves to it once a path over it works (with
 *                    nothing excluded, its unbound sockets follow the default
 *                    by themselves). STAY: its sockets are bound to the
 *                    network it uses (as above), and it moves only when that
 *                    network goes or is excluded: no back and forth while a
 *                    network comes and goes. A sharer that listens (UDP, TCP)
 *                    stays where it listens either way.
 *   standby          1: another network that is allowed, Ethernet, Wi-Fi or
 *                    cellular, keeps a proven path ready beside the one in use
 *                    (a probe of a few dozen bytes every standby_probe_ms keeps
 *                    its NAT open), which takes the traffic as soon as the path
 *                    in use is silent for path_stall_ms, before the system
 *                    gives up on its network; once that path answers again,
 *                    the traffic goes back to it. Over P2P and RELAY the
 *                    standby path is found through the host's signalling as a
 *                    new path is; over TCP it is a second TCP connection to
 *                    the sharer. It costs the probes' bytes (metered_bytes)
 *                    and, on a phone, a cellular radio that wakes for each
 *                    probe; it works only where the system lets this process
 *                    use that network beside the default one (Android lets an
 *                    application use cellular beside Wi-Fi once it asked for
 *                    it, ConnectivityManager.requestNetwork, which the SDK does
 *                    not do: there the standby path over cellular is likely
 *                    not found; not verified). A VPN in use is not bypassed:
 *                    beside it no standby path is kept. RCS_PATH_STANDBY says
 *                    when one is ready, and why none is. UNSUPPORTED over HOST
 *                    and FORWARDER, and on a sharer over UDP or TCP. 0
 *                    (default).
 *   restore_control  Share: 1: a viewer whose path was lost has its control
 *                    back once the path is back. 0 (default): losing the path
 *                    revokes its control (RCS_EVENT_CONTROL), and the host
 *                    grants it again (rcs_session_set_viewer_control) if it
 *                    wants: the user may have walked away meanwhile. A path
 *                    that only stalls (RCS_PATH_CHANGING) keeps the grant
 *                    either way; what the viewer held is released, and input
 *                    it sent before the stall is dropped.
 * Changes apply at once: excluding the network in use moves the connection
 * off it (SETTINGS). A value no version defines is INVALID_ARGUMENT. */
typedef uint32_t rcs_path_policy;
enum { RCS_PATH_POLICY_FOLLOW_SYSTEM = 0, RCS_PATH_POLICY_STAY = 1 };
typedef struct rcs_paths_config {
    rcs_extension header;
    uint32_t migrate;
    uint32_t excluded_networks;
    rcs_path_policy policy;
    uint32_t standby;
    uint32_t restore_control;
} rcs_paths_config;
#define RCS_PATHS_CONFIG_INIT                                                                                          \
    {{RCS_EXT_PATHS, sizeof(rcs_paths_config), NULL}, 1u, 0u, RCS_PATH_POLICY_FOLLOW_SYSTEM, 0u, 0u}
RCS_INLINE rcs_paths_config rcs_paths_config_init(void) {
    rcs_paths_config value = RCS_PATHS_CONFIG_INIT;
    return value;
}

/* How much this session logs (see Logging). Without it, or with max_level 0,
 * as much as rcs_set_log_callback's max_level; otherwise up to max_level, more
 * or less than the rest of the process: DEBUG for the one session under
 * investigation, ERROR for one that should stay quiet. A change applies at
 * once. An agent of another process takes it too (rcs_agent_config). */
typedef struct rcs_log_config {
    rcs_extension header;
    rcs_log_level max_level; /* 0, or RCS_LOG_ERROR..RCS_LOG_DEBUG. */
} rcs_log_config;
#define RCS_LOG_CONFIG_INIT {{RCS_EXT_LOG, sizeof(rcs_log_config), NULL}, 0u}
RCS_INLINE rcs_log_config rcs_log_config_init(void) {
    rcs_log_config value = RCS_LOG_CONFIG_INIT;
    return value;
}

/* ---- Modules ------------------------------------------------------------ */

/* A module this session uses (rcsdk/module.h): a feature beside the core,
 * the SDK's own (e.g. rcsdk/clipboard.h) or anyone else's. One per module,
 * any number in a chain. The module is registered first (rcs_register_module)
 * and checks its settings: INVALID_ARGUMENT or UNSUPPORTED with its reason.
 * The SDK copies `size` bytes of `settings`; what they point to (a callback's
 * user_data) stays the host's and must outlive the session. rcs_session_update
 * with it gives the module new settings; a session uses the modules it was
 * created with. Where it works, and where not and why: RCS_EVENT_MODULE. */
typedef struct rcs_module_config {
    rcs_extension header;
    const char* module;   /* Its id. */
    const void* settings; /* The module's own structure, as its header defines it; NULL with size 0: none. */
    uint32_t size;        /* 0..RCS_MODULE_SETTINGS_MAX (rcsdk/module.h) */
} rcs_module_config;
#define RCS_MODULE_CONFIG_INIT {{RCS_EXT_MODULE, sizeof(rcs_module_config), NULL}, NULL, NULL, 0u}
RCS_INLINE rcs_module_config rcs_module_config_init(void) {
    rcs_module_config value = RCS_MODULE_CONFIG_INIT;
    return value;
}

/* ---- The core: how to reach the other end and prove who this end is ------ */

/* How the two ends prove who they are (rcs_session_config.identity).
 * rcs_session_set_identity changes it while running, from the next handshake
 * (a sharer listening for viewers, a viewer reconnecting).
 *
 * secret: what both ends know; each handshake proves it without revealing it.
 *   KEY       key (default): 32 random bytes the host hands to both ends.
 *   PASSWORD  password: both ends derive a key from it with PBKDF2-SHA256
 *             (600 000 rounds, a salt the sharer picks per run). Anyone who
 *             can reach the sharer, or records a handshake, may guess offline
 *             at that cost per guess: use at least 10 random characters
 *             (50 bits); a short PIN falls within minutes.
 *   PIN       password holds a short PIN, 4..32 characters (digits are
 *             typical; rcs_generate_pin), proven with a password-authenticated
 *             key exchange (CPace on X25519): a recorded handshake lets nobody
 *             test a single guess, and each handshake lets the peer test one.
 *             The sharer counts them: after pin_attempts failed PIN handshakes
 *             it refuses PIN handshakes (each failure and the last one are
 *             RCS_EVENT_PIN) until the host sets the identity again, so a
 *             6-digit PIN and 5 attempts give a guesser 1 chance in 200 000.
 *             A handshake fails once the sharer has answered it (the answer
 *             lets the client test its guess) and it ends without the
 *             client's proof; a hello refused before an answer costs
 *             nothing, and over UDP a sharer answers only a viewer that
 *             proved it receives at its address (a round trip more), so
 *             hellos with forged source addresses take no attempt. A
 *             guesser who uses the attempts up locks the right PIN out as
 *             well, until the host sets the identity again.
 *             The viewer tests one answer per connection, the first that
 *             reaches the PIN's test: a wrong one fails the connection with
 *             RCS_ERROR_AUTHENTICATION, which ends the run (no reconnect),
 *             so whoever answers in the sharer's place (a fake sharer,
 *             someone on the path) tests one guess per run of the viewer.
 *             An answer forged that way ends the attempt as a wrong PIN
 *             does. For a code that a person reads on one screen and types
 *             on the other; a new one per session.
 *   NONE      nothing shared: the device keys alone authenticate both ends,
 *             so both need rcs_device_identity_config's device_key and
 *             trusted_keys. */

typedef uint32_t rcs_secret;
enum { RCS_SECRET_KEY = 0, RCS_SECRET_PASSWORD = 1, RCS_SECRET_PIN = 2, RCS_SECRET_NONE = 3 };
#define RCS_MAX_PASSWORD 128
#define RCS_MAX_TRUSTED_KEYS 1024
typedef struct rcs_identity {
    rcs_secret secret;
    uint8_t key[32];                 /* KEY: random, shared by both ends (rcs_generate_connection_key), never a password. */
    char password[RCS_MAX_PASSWORD]; /* PASSWORD (not empty), PIN (4..32 characters): UTF-8, NUL-terminated. */
    /* PIN, share: failed PIN handshakes allowed, 1..1000 (default 5), counted
     * per run from start or from the last rcs_session_set_identity (the same
     * PIN too). A handshake under way holds an attempt until it ends. */
    uint32_t pin_attempts;
} rcs_identity;

/* A device identity (both ends, optional; rcs_session_update changes it,
 * from the next handshake).

 * device_key: this end's device key (rcs_generate_device_key), optional:
 *   the end signs each handshake with it, and the peer learns its public key
 *   (rcs_peer_info.public_key), sealed from passive observers.
 * trusted_keys: the peer must prove one of these public keys; with none, any
 *   device key or none is accepted and reported. A peer with another key is
 *   refused (RCS_ERROR_AUTHENTICATION): a sharer drops it and keeps listening;
 *   a viewer's run fails, its rcs_peer_info.public_key holding the key the
 *   sharer proved. The handshake says which side uses what when the ends
 *   differ. */
typedef struct rcs_device_identity_config {
    rcs_extension header;
    uint32_t has_device_key;
    uint8_t device_key[RCS_DEVICE_KEY_SIZE];
    uint32_t trusted_key_count; /* 0..RCS_MAX_TRUSTED_KEYS */
    /* trusted_key_count public keys of RCS_PUBLIC_KEY_SIZE bytes each, one
     * after another; copied by rcs_session_create and rcs_session_update. */
    const uint8_t* trusted_keys;
} rcs_device_identity_config;
#define RCS_DEVICE_IDENTITY_CONFIG_INIT                                                                                \
    {{RCS_EXT_DEVICE_IDENTITY, sizeof(rcs_device_identity_config), NULL}, 0u, {0}, 0u, NULL}
RCS_INLINE rcs_device_identity_config rcs_device_identity_config_init(void) {
    rcs_device_identity_config value = RCS_DEVICE_IDENTITY_CONFIG_INIT;
    return value;
}

/* How this end reaches the other (see rcs_transport). UDP and TCP: a numeric
 * IPv4 or IPv6 address, which a sharer binds and a viewer connects to, and
 * its port; P2P and RELAY: the local port only; HOST: neither; FORWARDER:
 * the forwarder's address (or name) and port over its UDP and TCP links,
 * neither over the others (rcs_forwarder_config). */
typedef struct rcs_connect {
    rcs_transport transport;
    char address[64];
    uint32_t port;
} rcs_connect;

/* A preset: the settings of the extensions it covers (rcs_preset_defaults
 * lists them) that the host does not chain. An
 * extension the host chains is the host's entirely, field by field (the
 * preset does not fill in fields of it): start one from
 * rcs_preset_defaults to change a field of a preset. Each *_INIT macro holds
 * BALANCED's values. What each preset sets is listed in API-DESIGN.md, and
 * rcs_session_get_settings shows what a session uses now.
 *   BALANCED       The common case (default): H.264 in hardware, 1920x1080,
 *                  60 FPS, 20 Mbit/s adaptive.
 *   LOW_LATENCY    Interaction on a LAN: a higher bitrate cap, looser pacing,
 *                  capture boosted after input, the shortest queues.
 *   LOW_BANDWIDTH  Mobile hotspots and long paths: at most 1280x720 at
 *                  30 FPS, a lower start and cap, adaptive bitrate that backs
 *                  off sooner.
 *   QUALITY        Text and design work: 4:4:4 formats listed before 4:2:0
 *                  (used where both ends take them, else the next listed), a
 *                  higher bitrate, fewer keyframes. */
typedef uint32_t rcs_preset;
enum { RCS_PRESET_BALANCED = 0, RCS_PRESET_LOW_LATENCY = 1, RCS_PRESET_LOW_BANDWIDTH = 2, RCS_PRESET_QUALITY = 3 };

typedef struct rcs_session_config {
    uint32_t struct_size;
    uint32_t version; /* RCSDK_VERSION of the headers the host was built with. */
    rcs_role role;
    rcs_preset preset;
    rcs_connect connect;
    rcs_identity identity;
    const rcs_extension* extensions; /* Optional chain; copied by rcs_session_create. */
} rcs_session_config;
#define RCS_SESSION_CONFIG_INIT                                                                                        \
    {sizeof(rcs_session_config), RCSDK_VERSION, RCS_ROLE_VIEW, RCS_PRESET_BALANCED,                                  \
     {RCS_TRANSPORT_UDP, "127.0.0.1", 47990u}, {RCS_SECRET_KEY, {0}, {0}, 5u}, NULL}
RCS_INLINE rcs_session_config rcs_session_config_init(void) {
    rcs_session_config value = RCS_SESSION_CONFIG_INIT;
    return value;
}

/* Fills `extension` (its header's type and struct_size set, e.g. from its
 * *_INIT macro) with `preset`'s settings for it; `next` is kept. Presets have
 * settings for RCS_EXT_SHARE_VIDEO, ENCODER, SEND_AUDIO, VIEW_REQUEST and
 * the three of rcsdk/tuning.h; INVALID_ARGUMENT for another type, a preset or
 * type no version defines, and a size other than this version's. */
RCSDK_API rcs_result rcs_preset_defaults(rcs_preset preset, rcs_extension* extension);

/* ------------------------------------------------------------------------ */
/* Frames and packets delivered to callbacks                                */
/* ------------------------------------------------------------------------ */

/* A data message between the hosts (rcs_session_send_data, on_data). The
 * channel is the host's own number for what the message is (text, a file,
 * a command); the SDK carries any value. Reliable messages arrive whole and
 * in the order sent, across channels, up to RCS_DATA_MAX_MESSAGE bytes.
 * RCS_DATA_UNRELIABLE ones are never resent and may arrive out of order or
 * not at all (one datagram each; over TCP lost only where the path takes far
 * less than is sent): for what the next message supersedes anyway, e.g. a pointer being
 * dragged. At most RCS_DATA_MAX_DATAGRAM bytes.
 *
 * viewer_id, share: sending, 0 sends to every viewer (all of them or none: one
 * whose queue cannot take it refuses it for all), an id to that viewer only;
 * on_data names the viewer that sent it. View: always 0. */
typedef uint32_t rcs_data_flags;
enum { RCS_DATA_UNRELIABLE = 1u << 0 };
#define RCS_DATA_MAX_MESSAGE (16u << 20)
#define RCS_DATA_MAX_DATAGRAM 1024u
typedef struct rcs_data_message {
    uint32_t struct_size;
    rcs_viewer_id viewer_id;
    uint32_t channel;
    rcs_data_flags flags;
    uint32_t size;       /* 1..RCS_DATA_MAX_MESSAGE */
    const uint8_t* data; /* Borrowed: copied by rcs_session_send_data; valid during on_data. */
} rcs_data_message;
#define RCS_DATA_MESSAGE_INIT {sizeof(rcs_data_message), 0, 0, 0, 0, NULL}
RCS_INLINE rcs_data_message rcs_data_message_init(void) {
    rcs_data_message value = RCS_DATA_MESSAGE_INIT;
    return value;
}


/* CPU image, BGRA8. Borrowed: valid only during the callback. */
typedef struct rcs_frame {
    uint32_t struct_size;
    uint32_t stream; /* rcs_streams_config: which; 0 without it. */
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    rcs_pixel_format format;
    uint64_t sequence;
    uint64_t timestamp_ns; /* Local monotonic clock. */
    const uint8_t* data;
    uint64_t data_size;
} rcs_frame;
#define RCS_FRAME_INIT {sizeof(rcs_frame), 0, 0, 0, 0, RCS_PIXEL_BGRA8, 0, 0, NULL, 0}
RCS_INLINE rcs_frame rcs_frame_init(void) {
    rcs_frame value = RCS_FRAME_INIT;
    return value;
}

/* Decoded sound, borrowed for the duration of on_audio: frames
 * sample frames of channels interleaved float32 samples (-1..1). */
typedef struct rcs_audio_frame {
    uint32_t struct_size;
    rcs_viewer_id viewer_id; /* on_audio on a sharer: whose sound; otherwise 0. */
    uint32_t sample_rate;    /* 48000 */
    uint32_t channels;    /* 2 */
    uint32_t frames;
    uint64_t sequence;
    uint64_t timestamp_ns; /* Local monotonic clock: when it arrived. */
    const float* samples;
} rcs_audio_frame;
#define RCS_AUDIO_FRAME_INIT {sizeof(rcs_audio_frame), 0, 48000u, 2u, 0, 0, 0, NULL}
RCS_INLINE rcs_audio_frame rcs_audio_frame_init(void) {
    rcs_audio_frame value = RCS_AUDIO_FRAME_INIT;
    return value;
}


/* Compressed video, borrowed for the duration of the callback.
 * H.264/HEVC: data and config are NAL units each prefixed by a 4-byte
 * big-endian length; config holds the parameter sets (SPS/PPS, VPS/SPS/PPS).
 * VP8 and VP9: data holds one frame as the encoder wrote it (VP9: a
 * superframe where it bundles several, with its index); config holds the
 * stream's VP codec configuration record ('vpcC' of the VP Codec ISO Media
 * File Format Binding, version 1, 12 bytes: profile, level, bit depth,
 * chroma subsampling, full range, colour primaries, transfer, matrix, no
 * initialization data), which the SDK writes from the stream's keyframes and
 * colour; packets a host pushes may leave it empty (the SDK writes it), one it
 * gives must be what the keyframe says.
 * AV1: data holds the frame's OBUs in low-overhead format (each with a size
 * field), without temporal delimiters; config holds the sequence header OBU.
 * Every packet of a standard codec the SDK hands out carries the stream's
 * config, a keyframe's included (host codecs: as the host's encoder made
 * it). keyframe marks pictures decodable on their own. after_gap marks the
 * packet a viewer's RCS_EXT_ENCODED_OUTPUT gets after packets that were lost
 * or dropped (RCS_EVENT_RECOVERY says why): the stream resumes there, at a
 * keyframe or at a picture that refers only to pictures before the lost ones
 * (RCS_RECOVERY_REFERENCES), so what was written before it serves. It is 0 in every other packet the SDK hands out (the host's
 * decoder hears of drops through reset) and ignored in those the host pushes.
 * sequence and timestamp_ns are the sharer's: the picture's number and when
 * it was captured, on the sharer's monotonic clock (sound packets use the
 * same clock, rcs_encoded_audio). format and color are the stream's
 * (rcs_stream_info): the bitstream's chroma format, bit depth and colour
 * description are those.
 * encoding is which of the stream's encodings the packet belongs to
 * (RCS_EXT_ENCODINGS; 0 the main one): a viewer's packets change encoding at
 * a keyframe when the sharer moves it, and with it may change codec and size
 * (a file written from them starts again there). temporal_id is the
 * picture's temporal layer, discardable 1 when no later picture of its layer
 * or a lower one refers to it (losing it needs no recovery). A viewer that gets
 * fewer layers of its encoding (rcs_stream_info.received_layers) gets no
 * picture of the layers above, whose sequence numbers it skips. The SDK reads
 * them from standard codecs' bitstreams where those say them (H.264:
 * nal_ref_idc, and an SVC prefix unit's temporal_id; HEVC:
 * nuh_temporal_id_plus1 and the sub-layer non-reference types; AV1: the OBU
 * extension's temporal_id; VP9: whether a frame refreshes a reference slot);
 * the rest is the encoder's (the SDK's encoders say it: H.264 without SVC
 * prefix units, AV1's discardable, VP9's layer and VP8's both), and a host's
 * own codec says them in the packets it pushes. A standard packet the host
 * pushes whose values differ from what its bitstream says is taken as the
 * bitstream says (logged once). */
typedef struct rcs_encoded_packet {
    uint32_t struct_size;
    uint32_t stream;   /* rcs_streams_config: which; 0 without it. Pushed packets: 0. */
    uint32_t encoding; /* Pushed packets: 0; the host's encoder's: taken as opened. */
    rcs_codec codec;
    rcs_picture_format format;
    rcs_color color;
    uint32_t width;
    uint32_t height;
    uint32_t keyframe;
    uint32_t temporal_id; /* 0..7 */
    uint32_t discardable;
    uint32_t after_gap;
    uint64_t sequence;
    uint64_t timestamp_ns;
    const uint8_t* data;
    uint64_t data_size;
    const uint8_t* config;
    uint32_t config_size;
} rcs_encoded_packet;
/* A packet of the default picture format and colour (8-bit 4:2:0, BT.709, limited range). */
#define RCS_ENCODED_PACKET_INIT                                                                                        \
    {sizeof(rcs_encoded_packet),                                                                                       \
     0,                                                                                                                \
     0,                                                                                                                \
     0,                                                                                                                \
     RCS_PICTURE_420_8,                                                                                                \
     {RCS_PRIMARIES_BT709, RCS_TRANSFER_BT709, RCS_MATRIX_BT709, RCS_RANGE_LIMITED},                                   \
     0,                                                                                                                \
     0,                                                                                                                \
     0,                                                                                                                \
     0,                                                                                                                \
     0,                                                                                                                \
     0,                                                                                                                \
     0,                                                                                                                \
     0,                                                                                                                \
     NULL,                                                                                                             \
     0,                                                                                                                \
     NULL,                                                                                                             \
     0}
RCS_INLINE rcs_encoded_packet rcs_encoded_packet_init(void) {
    rcs_encoded_packet value = RCS_ENCODED_PACKET_INIT;
    return value;
}


/* ------------------------------------------------------------------------ */
/* Remote input                                                             */
/* ------------------------------------------------------------------------ */

/* Remote input, by type (the fields each uses; the rest stay 0):
 *
 * The pointer. Positions x, y are absolute, 0..65535 across what is shared
 * (the display, the region, the window).
 *   MOVE           x, y.
 *   MOVE_RELATIVE  dx, dy: motion in the shared machine's pixels, as a mouse
 *                  reports it, for programs that hide the pointer and read
 *                  its motion (games, 3D views). Sharing a display it moves
 *                  like that machine's own mouse; sharing a region, a window or
 *                  an application the pointer stays inside it.
 *   BUTTON         x, y, code (rcs_mouse_button), down.
 *   WHEEL          x, y, wheel_x, wheel_y: whole lines, as a notched wheel turns.
 *   SCROLL         x, y, wheel_x, wheel_y: 1/120 of a line, for touchpads and
 *                  high-resolution wheels (smooth scrolling). Positive is right
 *                  and up for both.
 * The keyboard.
 *   KEY            code, down, repeat (auto-repeat). The code is a USB HID usage
 *                  naming the key's place: keyboard page usages as they are
 *                  (US labels: A=4, Enter=40, LeftControl=224, LeftShift=225,
 *                  LeftAlt=226, LeftGui/Command=227), and media keys as
 *                  RCS_KEY_CONSUMER | consumer page usage (Play/Pause 0xCD, Next
 *                  0xB5, Previous 0xB6, Stop 0xB7, Mute 0xE2, Volume Up 0xE9,
 *                  Volume Down 0xEA; Windows also Back 0x224, Forward 0x225,
 *                  Refresh 0x227; macOS also Brightness Up 0x6F, Down 0x70). The
 *                  shared machine's layout decides what a key types.
 *   TEXT           code: one Unicode scalar value (not a surrogate; tab, line
 *                  feed and 0x20 on), typed as it is whatever the layout and
 *                  the held keys: what an input method composed, emoji, a word
 *                  pasted as typing. No key state.
 * Touch and pen, at x, y like the pointer (Windows 10 1809 or later; macOS:
 * the pen only, as tablet events; see rcs_capabilities.input_types):
 *   TOUCH          code: the contact, 0..9; down: touching. A contact's first
 *                  event with down 1 puts it down, the next ones move it, down
 *                  0 lifts it. pressure.
 *   PEN            down: the tip touches; 0: it hovers in range. pressure,
 *                  tilt_x, tilt_y, rotation, pen (rcs_pen_flags).
 * Control.
 *   RELEASE_ALL       lifts every key, button, contact and the pen it holds.
 *   SECURE_ATTENTION  Ctrl+Alt+Del, the secure attention sequence: one event,
 *                     no state, executed by the machine that is shared
 *                     (Windows: the sign-in and lock screens, the security
 *                     options screen; see rcs_capabilities.secure_attention
 *                     for who can send it). PERMISSION_REQUIRED when the system
 *                     policy does not let services simulate it, UNSUPPORTED
 *                     where the process cannot or the system has no such thing
 *                     (macOS). Any viewer with control may send it, as any
 *                     key: a host that does not want it drops it in on_input,
 *                     or takes the type itself (host_input_types) to decide
 *                     each one.
 * What the shared system cannot execute is reported in
 * rcs_input_stats.last_result (UNSUPPORTED); the host's on_input sees every
 * event first and may drop it. */
typedef uint32_t rcs_input_type;
enum {
    /* The pointer */
    RCS_INPUT_MOVE = 1,
    RCS_INPUT_MOVE_RELATIVE = 2,
    RCS_INPUT_BUTTON = 3,
    RCS_INPUT_WHEEL = 4,
    RCS_INPUT_SCROLL = 5,
    /* The keyboard */
    RCS_INPUT_KEY = 6,
    RCS_INPUT_TEXT = 7,
    /* Touch and pen */
    RCS_INPUT_TOUCH = 8,
    RCS_INPUT_PEN = 9,
    /* Control */
    RCS_INPUT_RELEASE_ALL = 10,
    RCS_INPUT_SECURE_ATTENTION = 11
};
typedef uint32_t rcs_mouse_button;
enum { RCS_BUTTON_LEFT = 1, RCS_BUTTON_RIGHT = 2, RCS_BUTTON_MIDDLE = 3, RCS_BUTTON_BACK = 4, RCS_BUTTON_FORWARD = 5 };
/* KEY codes on the HID consumer page (media keys): RCS_KEY_CONSUMER | usage. */
#define RCS_KEY_CONSUMER 0x000C0000u
/* PEN: BARREL, the barrel button is held (a press is a secondary click);
 * ERASER, the eraser end is in use; LEAVE, the pen leaves the range (the
 * last event of a stroke and its hovering). */
typedef uint32_t rcs_pen_flags;
enum { RCS_PEN_BARREL = 1, RCS_PEN_ERASER = 2, RCS_PEN_LEAVE = 4 };
typedef struct rcs_input_event {
    uint32_t struct_size;
    rcs_input_type type;
    /* Absolute, 0..65535 across the picture of `stream` (rcs_streams_config;
     * 0 without it): its display, region or window. */
    uint32_t x, y;
    uint32_t stream;
    int32_t dx, dy;  /* MOVE_RELATIVE: pixels, -10 000..10 000. */
    /* BUTTON: rcs_mouse_button; KEY: HID usage; TEXT: Unicode scalar value; TOUCH: contact. */
    uint32_t code;
    uint32_t down;   /* BUTTON, KEY, TOUCH, PEN. */
    uint32_t repeat; /* KEY auto-repeat. */
    int32_t wheel_x; /* WHEEL: lines, -1000..1000; SCROLL: 1/120 lines, -120 000..120 000; positive is right... */
    int32_t wheel_y; /* ...and up. */
    uint32_t pressure;      /* TOUCH, PEN: 1..1024; 0: not reported. */
    int32_t tilt_x, tilt_y; /* PEN: degrees from upright, -90..90; positive is right and toward the user. */
    uint32_t rotation;      /* PEN: degrees clockwise, 0..359. */
    rcs_pen_flags pen;      /* PEN. */
    /* Share, on_input: the viewer that sent it (0: none, a release of the
     * session's own). Not sent: rcs_session_send_input takes 0 only. */
    rcs_viewer_id viewer_id;
} rcs_input_event;
#define RCS_INPUT_EVENT_INIT {sizeof(rcs_input_event), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_input_event rcs_input_event_init(void) {
    rcs_input_event value = RCS_INPUT_EVENT_INIT;
    return value;
}

/* ------------------------------------------------------------------------ */
/* Session information                                                      */
/* ------------------------------------------------------------------------ */

/* An HDR10 stream's static metadata: the display it was made on (SMPTE ST
 * 2086, as HEVC's and AV1's mastering display metadata carry it) and how
 * bright its content gets (CTA-861.3). Each value 0 where unknown: no system
 * reports the shared display's metadata to the SDK yet, so the fields are 0
 * for now; content light is not measured. For DXGI_HDR_METADATA_HDR10 divide
 * max_luminance by 10 000 (it takes whole cd/m2 there). */
typedef struct rcs_hdr_metadata {
    /* CIE 1931 x and y of the primaries and the white point, in units of 0.00002. */
    uint16_t red_x, red_y, green_x, green_y, blue_x, blue_y, white_x, white_y;
    uint32_t max_luminance, min_luminance; /* In units of 0.0001 cd/m2. */
    uint16_t max_content_light;            /* MaxCLL, cd/m2. */
    uint16_t max_frame_average_light;      /* MaxFALL, cd/m2. */
} rcs_hdr_metadata;

/* What flows now: the stream the sharer sends and the sound. */
typedef struct rcs_stream_info {
    uint32_t struct_size;
    uint32_t stream; /* Which (rcs_streams_config); 0 without it. */
    /* Which of the sharer's encodings of the stream this is (RCS_EXT_ENCODINGS;
     * 0 the main one), and how many it runs now. A viewer gets one of them and
     * may be moved to another while it watches (RCS_STREAM_ENCODING); a
     * sharer's stream is its main encoding. */
    uint32_t encoding, encodings;
    /* The encoding's temporal layers (rcs_scalability; 1: none), and how many
     * of them this end gets: a viewer that falls short may get fewer, at
     * frames_per_second / 2 (or 4) for each layer left out (RCS_STREAM_ENCODING);
     * a sharer's are the same. */
    uint32_t layers, received_layers;
    /* The picture: its codec, format and what its values mean... */
    rcs_codec codec;
    rcs_picture_format format;
    rcs_color color;
    rcs_hdr_metadata hdr; /* ...and, HDR10, its metadata (all 0 for SDR). */
    rcs_acceleration encoder_acceleration; /* The sharer's. */
    uint32_t width, height;
    uint32_t frames_per_second;
    uint32_t bitrate_bps; /* FIXED: the bitrate; ADAPTIVE: its maximum (RCS_EVENT_RATE: the current target). */
    /* Where the shared picture sits inside the encoded frame (letterboxing). */
    uint32_t content_x, content_y, content_width, content_height;
    /* What is shared: the size in pixels of the display, the region or the
     * window (APPLICATION: its display), and the display's refresh rate (0:
     * unknown, e.g. HOST frames). */
    uint32_t display_width, display_height, display_refresh_hz;
    /* A display: where it sits on the sharer's desktop (rcs_display_info
     * desktop_*), to lay several streams out as the displays are; 0 for other
     * sources. */
    int32_t desktop_x, desktop_y;
    uint32_t desktop_width, desktop_height;
    /* The pointer: in the picture, or sent apart (RCS_EVENT_CURSOR). One
     * picture serves every viewer, so the main viewer's request decides
     * (rcs_viewers_config); another viewer may get what it did not ask for. */
    rcs_cursor_mode cursor;
    /* The sound; 0 while none flows. A sharer with several viewers: the main
     * viewer's (each viewer's in RCS_EVENT_STREAM with its viewer_id). */
    rcs_audio_codec audio_codec;
    uint32_t audio_bitrate_bps;
} rcs_stream_info;
#define RCS_STREAM_INFO_INIT                                                                                           \
    {sizeof(rcs_stream_info), 0, 0, 0, 0, 0, 0, 0, {0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 0, 0, 0, 0, \
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_stream_info rcs_stream_info_init(void) {
    rcs_stream_info value = RCS_STREAM_INFO_INIT;
    return value;
}

/* A video codec an end offers, the largest picture it takes in it and the
 * picture formats: a sharer's encoder, a viewer's decoder
 * (rcs_codec_implementation.max_width and picture_formats, within the formats
 * its host listed; the codec's own limit for the host's own codecs). */
typedef struct rcs_codec_limit {
    rcs_codec codec;
    uint32_t max_width, max_height;
    uint32_t picture_formats; /* As (1u << rcs_picture_format) bits. */
} rcs_codec_limit;

/* The other end, once connected: what it offers now (it may change its
 * codecs while running) and its platform. A sharer with several viewers: the
 * main viewer's (each viewer's in rcs_viewer_info). */
typedef struct rcs_peer_info {
    uint32_t struct_size;
    rcs_platform platform;
    uint32_t codecs; /* Video codecs, as (1u << rcs_codec) bits... */
    /* ...in its order of preference, with the largest picture each takes;
     * unused entries 0. A sharer keeps the stream within the viewer's. */
    rcs_codec_limit codec_limits[RCS_MAX_CODECS];
    /* Sound, each way, as (1u << rcs_audio_codec) bits: the codecs it sends
     * (encodes) and receives (decodes), and whether it sends and receives
     * now (rcs_send_audio_config, rcs_receive_audio_config). */
    uint32_t send_audio_codecs;
    uint32_t receive_audio_codecs;
    uint32_t sends_audio;
    uint32_t receives_audio;
    /* A sharer: the remote input it takes, as (1u << rcs_input_type) bits: what
     * its machine executes (rcs_capabilities.input_types) and the types its
     * host executes (rcs_control_config.host_input_types), every type when its
     * host executes all input (RCS_INPUT_HOST). 0 for a viewer. Whether
     * control is granted is RCS_EVENT_CONTROL's. */
    uint32_t input_types;
    /* The device key it proved in the handshake (rcs_device_identity_config), if it
     * has one; set before the connection is refused for an untrusted key. */
    uint32_t has_public_key;
    uint8_t public_key[RCS_PUBLIC_KEY_SIZE];
} rcs_peer_info;
#define RCS_PEER_INFO_INIT {sizeof(rcs_peer_info), 0, 0, {{0, 0, 0, 0}}, 0, 0, 0, 0, 0, 0, {0}}
RCS_INLINE rcs_peer_info rcs_peer_info_init(void) {
    rcs_peer_info value = RCS_PEER_INFO_INIT;
    return value;
}

/* The machine a sharing session captures, as its agent reports it: the agent
 * in the session's own process (always attached), or the one attached through
 * rcs_agent_link_config (attached 0 and the rest empty before the first one;
 * after one left, the last one's, with why it left). What the machine can do
 * (codecs, sound backends) decides what the settings can be;
 * rcs_get_capabilities describes the calling process only. */
typedef struct rcs_agent_info {
    uint32_t struct_size;
    uint32_t attached;
    /* Not attached: why the last agent left. RCS_OK when it closed the link
     * (its process ended, it stopped); its error when it failed (e.g.
     * RCS_ERROR_CAPTURE); RCS_ERROR_NETWORK when the link broke. RCS_OK and
     * empty while attached. */
    rcs_result result;
    char message[128]; /* UTF-8, cut short to fit. */
    /* The agent's process as the operating system reports it, never as the
     * agent says: */
    uint32_t process_id;
    uint32_t os_session; /* Windows: the session it runs in; 0 elsewhere. */
    char account[128];   /* UTF-8: Windows "DOMAIN\user" (the SID where it has no name); elsewhere the user name. */
    /* UTF-8: the path of the program the process runs; decide by it, not by
     * looking process_id up, which may name another process by then. macOS
     * takes it from the connection's audit token, as the agent connected and
     * again after its handshake: one that handed its connection to another
     * process or ran another program in between is refused before the host
     * hears of it. Windows takes it from the process, opened as it is told.
     * Empty where the system cannot say for sure (Android, iOS), where
     * the session's account may not read it (Windows) or where it does not
     * fit. */
    char program[1024];
    rcs_platform platform;
    uint32_t capture_sources; /* The capture sources it offers (rcs_capabilities)... */
    uint32_t picture_formats; /* ...and the picture formats its capture produces. */
    /* The remote input the session takes: what the agent executes
     * (rcs_capabilities.input_types) and the types the host executes
     * (rcs_control_config.host_input_types)... */
    uint32_t input_types;
    uint32_t hardware_encoders; /* ...the codecs it encodes, as (1u << rcs_codec) bits... */
    uint32_t software_encoders;
    rcs_permission screen_permission; /* ...its permissions (rcs_input_permission: not whether input is taken now)... */
    rcs_permission input_permission;
    uint32_t displays;       /* ...how many displays it sees (rcs_session_list_displays)... */
    uint32_t secure_desktop; /* ...and whether it can follow secure desktops (rcs_capabilities). */
} rcs_agent_info;
#define RCS_AGENT_INFO_INIT {sizeof(rcs_agent_info), 0, RCS_OK, {0}, 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_agent_info rcs_agent_info_init(void) {
    rcs_agent_info value = RCS_AGENT_INFO_INIT;
    return value;
}

/* What the machine shows (Windows and macOS; RCS_DESKTOP_UNKNOWN elsewhere and
 * while the system does not say). USER is the user's own desktop. SECURE is a
 * screen of the system's: on Windows the sign-in screen, the lock screen, a
 * UAC prompt or the Ctrl+Alt+Del screen, which Windows does not tell apart,
 * and a screen saver Windows runs on a desktop of its own; on macOS the login
 * window or the lock screen. user_logged_on says whether someone is signed in
 * (a lock screen or prompt) or not (the sign-in screen, the login window). On
 * macOS it is what the agent's own window-server session shows: an agent
 * outside one (a LaunchDaemon) or in a session another user's has replaced on
 * the console (fast user switching) reports UNKNOWN. A viewer gets it too
 * (RCS_EVENT_DESKTOP). */
typedef uint32_t rcs_desktop_kind;
enum { RCS_DESKTOP_UNKNOWN = 0, RCS_DESKTOP_USER = 1, RCS_DESKTOP_SECURE = 2 };
typedef struct rcs_desktop_info {
    uint32_t struct_size;
    rcs_desktop_kind kind;
    uint32_t user_logged_on;
} rcs_desktop_info;
#define RCS_DESKTOP_INFO_INIT {sizeof(rcs_desktop_info), RCS_DESKTOP_UNKNOWN, 0}
RCS_INLINE rcs_desktop_info rcs_desktop_info_init(void) {
    rcs_desktop_info value = RCS_DESKTOP_INFO_INIT;
    return value;
}

/* Remote control as one end sees it. A sharer with several viewers: one
 * viewer's (rcs_viewer_info.control, RCS_EVENT_CONTROL with its viewer_id);
 * rcs_session_get_control_info gives the main viewer's, or without a viewer
 * what one that joins gets. */
typedef struct rcs_control_info {
    uint32_t struct_size;
    /* Share: the host allows the viewer remote control (rcs_control_config,
     * on_viewer_join, rcs_session_set_viewer_control). */
    uint32_t allowed;
    /* View: the sharer currently grants control. Share: allowed, and (target
     * SYSTEM) the OS currently accepts injected input. While a lock screen or
     * a UAC prompt holds the input desktop, or input permission is withdrawn,
     * the grant is paused (held keys are released) and resumes by itself. */
    uint32_t granted;
} rcs_control_info;
#define RCS_CONTROL_INFO_INIT {sizeof(rcs_control_info), 0, 0}
RCS_INLINE rcs_control_info rcs_control_info_init(void) {
    rcs_control_info value = RCS_CONTROL_INFO_INIT;
    return value;
}

/* One viewer of a sharer (rcs_viewers_config): RCS_EVENT_VIEWER when it joins
 * and leaves, rcs_session_list_viewers while it is connected, on_viewer_join
 * before it joins. */
typedef struct rcs_viewer_info {
    uint32_t struct_size;
    rcs_viewer_id viewer_id;
    /* 1 while it is connected; 0 in on_viewer_join and once it left, with why:
     * RCS_ERROR_DISCONNECTED when it stopped or the host disconnected it,
     * RCS_ERROR_NETWORK when its path broke, RCS_ERROR_BUSY when it no longer
     * decodes the stream the others watch, RCS_OK when this end stopped. */
    uint32_t joined;
    rcs_result result;
    char message[128];  /* UTF-8, cut short to fit; empty while connected. */
    uint64_t joined_ns; /* When it joined, on the clock of rcs_event.timestamp_ns. */
    /* Where it connects from (UDP and TCP: its address and port; P2P and RELAY:
     * the path's other end; HOST: empty). */
    char address[64];
    uint32_t port;
    /* The host's route to it: P2P and RELAY, the one the host passed with
     * its signal (rcs_session_signal); HOST, its channel's
     * (rcs_session_transport_receive), e.g. which of the host's users or
     * connections it is; FORWARDER, the one its token names, as the
     * forwarder says (rcs_forwarder_config); 0 otherwise. */
    uint64_t route;
    rcs_peer_info peer; /* Its platform, what it offers, the device key it proved. */
    /* Whether its request decides the stream (rcs_viewers_config). */
    uint32_t is_main;
    rcs_control_info control; /* Its remote control. */
} rcs_viewer_info;
#define RCS_VIEWER_INFO_INIT                                                                                           \
    {sizeof(rcs_viewer_info), 0, 0, RCS_OK, {0}, 0, {0}, 0, 0, RCS_PEER_INFO_INIT, 0, RCS_CONTROL_INFO_INIT}
RCS_INLINE rcs_viewer_info rcs_viewer_info_init(void) {
    rcs_viewer_info value = RCS_VIEWER_INFO_INIT;
    return value;
}

/* Whether a viewer gets in.
 *   REFUSE  It does not: its run ends with RCS_ERROR_REFUSED and `reason`.
 *   ADMIT   It joins.
 *   LATER   (on_viewer_join only) The host decides later, with
 *           rcs_session_admit_viewer, e.g. once the user answered a dialog.
 *           Meanwhile the viewer waits, connected (RCS_EVENT_ADMISSION), and
 *           gets no picture, sound, input or module messages; it holds one of
 *           max_viewers' places. Without a decision within
 *           rcs_viewers_config.admission_timeout_ms it is refused. */
typedef uint32_t rcs_admission;
enum { RCS_REFUSE = 0, RCS_ADMIT = 1, RCS_ADMIT_LATER = 2 };

/* How one viewer gets the picture (rcs_viewers_config, RCS_EXT_ENCODINGS):
 * the host's say over what the SDK would choose.
 *   counts_for_bitrate  1 (default): its path counts for the bitrate of the
 *                       encoding it gets; 0: it gets what its path carries
 *                       and holds no one back (a recorder, a viewer the host
 *                       would rather not wait for): more than a second
 *                       behind, it skips to keyframes.
 *   encoding            RCS_ENCODING_ANY (default): the SDK moves it as its
 *                       path, its losses and its decoder say; 0..count - 1:
 *                       it stays on that encoding (AUTO starts it), as long
 *                       as it decodes that encoding's codec (else it gets one
 *                       it decodes, RCS_VIEWER_VIDEO_DECODER).
 *   layers              0 (default): the SDK gives it fewer of its encoding's
 *                       temporal layers (rcs_scalability) while its path
 *                       falls short, before it moves it to a lower encoding,
 *                       and all of them again once its path carries them;
 *                       1..3: that many, or all its encoding has where it has
 *                       fewer, whatever its path does (e.g. 1 for a recorder
 *                       that wants half the frame rate). */
#define RCS_ENCODING_ANY 0xFFFFFFFFu
typedef struct rcs_viewer_video {
    uint32_t struct_size;
    uint32_t counts_for_bitrate;
    uint32_t encoding;
    uint32_t layers;
} rcs_viewer_video;
#define RCS_VIEWER_VIDEO_INIT {sizeof(rcs_viewer_video), 1u, RCS_ENCODING_ANY, 0u}
RCS_INLINE rcs_viewer_video rcs_viewer_video_init(void) {
    rcs_viewer_video value = RCS_VIEWER_VIDEO_INIT;
    return value;
}

/* What on_viewer_join decides, filled in with what happens without it, or
 * what rcs_session_admit_viewer gives. */
typedef struct rcs_viewer_decision {
    uint32_t struct_size;
    rcs_admission admission; /* In: RCS_ADMIT. */
    /* In: rcs_control_config.allowed. Out: whether it may control. */
    uint32_t control;
    char reason[128]; /* REFUSE: what the viewer is told (UTF-8; may be empty). */
    /* ADMIT: how it gets the picture from the start, as
     * rcs_session_set_viewer_video would set it. In: RCS_VIEWER_VIDEO_INIT. */
    rcs_viewer_video video;
} rcs_viewer_decision;
#define RCS_VIEWER_DECISION_INIT {sizeof(rcs_viewer_decision), RCS_ADMIT, 0u, {0}, RCS_VIEWER_VIDEO_INIT}
RCS_INLINE rcs_viewer_decision rcs_viewer_decision_init(void) {
    rcs_viewer_decision value = RCS_VIEWER_DECISION_INIT;
    return value;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                   */
/* ------------------------------------------------------------------------ */

typedef uint32_t rcs_event_type;
enum {
    /* The session */
    RCS_EVENT_STATE = 1,  /* state: the session's state changed. */
    RCS_EVENT_PEER = 2,   /* peer: connected to the other end, or it changed what it offers. */
    RCS_EVENT_VIEWER = 3, /* viewer, share: a viewer joined or left (rcs_viewers_config). */
    RCS_EVENT_PIN = 4,    /* pin, share: a PIN handshake failed (RCS_SECRET_PIN). */
    RCS_EVENT_ADMISSION = 5, /* admission: a viewer waits for the sharer's host, got in, or did not. */
    /* The picture */
    RCS_EVENT_STREAM = 6,    /* stream: the stream began or changed; `changes` says what. */
    RCS_EVENT_CODEC = 7,     /* codec: an encoder or decoder opened, or the encoder changed its mode. */
    RCS_EVENT_CAPTURE = 8,   /* capture, share: capture paused, and why, or resumed. */
    RCS_EVENT_DISPLAYS = 9,  /* displays, share: displays were added, removed or changed (rcs_session_list_displays). */
    RCS_EVENT_RECOVERY = 10, /* recovery: the stream restarts from a keyframe. */
    RCS_EVENT_KEYFRAME = 11, /* keyframe, share with capture source ENCODED: the next packet must be a keyframe. */
    RCS_EVENT_RATE = 12,     /* rate, share: adaptive bitrate changed the video bitrate. */
    RCS_EVENT_CURSOR = 13,   /* cursor, view with RCS_CURSOR_SEPARATE: the pointer moved, changed or hid. */
    RCS_EVENT_ENCODING = 14, /* encoding, share: a lower encoding of a stream started, stopped or could not start. */
    RCS_EVENT_VIEWER_VIDEO = 15, /* viewer_video, share: a viewer moved to another encoding, and why. */
    /* The sound */
    RCS_EVENT_AUDIO = 16, /* audio: sound started, paused or resumed. */
    /* Control */
    RCS_EVENT_CONTROL = 17, /* control: remote control was granted, paused or revoked. */
    /* Data */
    RCS_EVENT_DATA = 18, /* data: the send queue has room again after it refused a message. */
    /* The agent */
    RCS_EVENT_AGENT = 19, /* agent, share: an agent attached (rcs_agent_link_config) or left; the session's own is
                             reported once, attached, when the run starts. */
    /* The machine's desktop */
    RCS_EVENT_DESKTOP = 20, /* desktop: the machine shows another desktop (the user's, or a secure one). */
    /* Modules */
    RCS_EVENT_MODULE = 21, /* module: a module works with a peer or not, or said something of its own. */
    /* Pausing */
    RCS_EVENT_PAUSE = 22, /* pause: what is paused changed (rcs_session_pause, rcs_background_config, a viewer's sharer). */
    /* The network */
    RCS_EVENT_PATH = 23, /* path: the connection's path is changing, changed or lost, or its standby path came or went. */
    RCS_EVENT_STALLS = 24 /* stalls: short stalls of the path came at a steady pace, changed pace or stopped. */
};

/* What a stream event reports as changed, as bits; all of them when the
 * stream begins. */
enum {
    RCS_STREAM_CODEC = 1u << 0,
    RCS_STREAM_FORMAT = 1u << 1, /* The picture format, its colour or its HDR metadata. */
    RCS_STREAM_SIZE = 1u << 2,
    RCS_STREAM_FRAME_RATE = 1u << 3,
    RCS_STREAM_BITRATE = 1u << 4,
    RCS_STREAM_CONTENT = 1u << 5, /* Letterboxing. */
    RCS_STREAM_DISPLAY = 1u << 6,
    RCS_STREAM_AUDIO = 1u << 7, /* Sound's codec or bitrate, or it started or stopped. */
    RCS_STREAM_CURSOR = 1u << 8, /* The pointer moved into the picture or out of it. */
    /* The stream stopped (rcs_streams_config changed, or no viewer takes it):
     * info is RCS_STREAM_INFO_INIT but for its number. */
    RCS_STREAM_ENDED = 1u << 9,
    /* View: the sharer moved this end to another of its encodings, runs more
     * or fewer, or sends this end more or fewer of the encoding's temporal
     * layers (encoding, encodings, layers, received_layers). */
    RCS_STREAM_ENCODING = 1u << 10
};
typedef struct rcs_stream_event {
    uint32_t changes;
    rcs_stream_info info;
} rcs_stream_event;

typedef struct rcs_displays_event {
    uint32_t count; /* Displays now; rcs_session_list_displays lists them. */
} rcs_displays_event;

typedef uint32_t rcs_recovery_reason;
enum {
    RCS_RECOVERY_BACKLOG = 1,   /* View: the decode queue overflowed or aged; its backlog was dropped. */
    RCS_RECOVERY_DECODER = 2,  /* View: the decoder failed and was replaced. */
    RCS_RECOVERY_REQUESTED = 3, /* A keyframe was asked for: by the viewer, or by the host (rcs_session_request_keyframe). */
    /* A picture was lost on the network and could not be repaired. View:
     * reported once decoding goes on (method, frozen_ns). Share: a viewer
     * gave up on one (viewer_id), reported with the packet the stream goes on
     * from (method). */
    RCS_RECOVERY_LOSS = 4
};

/* How the stream goes on after it.
 *   KEYFRAME    From the next keyframe.
 *   REFERENCES  From the next picture the encoder made after it dropped the
 *               lost picture and the ones after it from its references
 *               (reference invalidation, rcs_video_tuning_config.
 *               ref_invalidation): a normal-sized picture, not a keyframe.
 *               Encoders that can: rcs_codec_implementation.ref_invalidation. */
typedef uint32_t rcs_recovery_method;
enum { RCS_RECOVERY_KEYFRAME = 1, RCS_RECOVERY_REFERENCES = 2 };

/* A viewer's admission (RCS_ADMIT_LATER), on both ends; on the sharer the
 * event's viewer_id says which viewer.
 *   WAITING   The sharer's host decides whether the viewer gets in;
 *             timeout_ms is how long it may take (0: no limit). The viewer's
 *             state stays CONNECTING; the sharer does not list it
 *             (rcs_session_list_viewers) until it is admitted.
 *   ADMITTED  It got in; on the viewer the stream follows.
 *   REFUSED   It did not: result RCS_ERROR_REFUSED, message the host's
 *             reason, or RCS_ERROR_TIMEOUT when the host did not decide in
 *             time. On the viewer the run ends with it.
 *   GONE      Sharer: a waiting viewer left before the host decided (result
 *             and message say why); rcs_session_admit_viewer for it is
 *             INVALID_STATE. */
typedef uint32_t rcs_admission_state;
enum { RCS_ADMISSION_WAITING = 1, RCS_ADMISSION_ADMITTED = 2, RCS_ADMISSION_REFUSED = 3, RCS_ADMISSION_GONE = 4 };
typedef struct rcs_admission_event {
    rcs_admission_state state;
    uint32_t timeout_ms;
    rcs_result result;
    char message[128]; /* UTF-8, cut short to fit. */
} rcs_admission_event;

/* A PIN handshake failed (RCS_SECRET_PIN): the client did not prove the PIN
 * (another PIN, or it left or went silent after the answer). attempts_left 0:
 * the sharer refuses PIN handshakes until the host calls rcs_session_set_identity. */
typedef struct rcs_pin_event {
    uint32_t failures;      /* Since the run started or the identity was last set. */
    uint32_t attempts_left; /* rcs_identity.pin_attempts minus failures. */
} rcs_pin_event;

typedef struct rcs_data_event {
    uint64_t queued_bytes; /* Waiting to be sent now. */
} rcs_data_event;

/* A module's (rcs_module_config): the SDK's own kinds below, or the module's
 * own (RCS_MODULE_EVENT_OWN and up), whose payload its header defines.
 *   ACTIVE      It works with viewer_id's end (a viewer's sharer: 0) now:
 *               that end uses it, in the same version.
 *   INACTIVE    It does not, on that connection: result UNSUPPORTED, and the
 *               message says why (the other end does not use it, or another
 *               version of it).
 *   NO_MACHINE  Share: the agent of another process cannot run its machine's
 *               half (rcsdk/module.h): result UNSUPPORTED, and the message
 *               says why (that process did not register it, or another
 *               version). The module goes on without it. */
typedef uint32_t rcs_module_event_kind;
enum { RCS_MODULE_ACTIVE = 1, RCS_MODULE_INACTIVE = 2, RCS_MODULE_NO_MACHINE = 3, RCS_MODULE_EVENT_OWN = 256 };
typedef struct rcs_module_event {
    const char* module; /* Its id. */
    rcs_module_event_kind kind;
    rcs_result result;
    char message[128];   /* UTF-8, cut short to fit; empty for RCS_MODULE_ACTIVE. */
    const void* payload; /* The module's own kinds: as its header says. */
    uint32_t size;
} rcs_module_event;

typedef struct rcs_state_event {
    rcs_state value;
    /* FAILED: why; see rcs_session_get_failure. LISTENING after the last
     * viewer left (or one that came could not be served) and RECONNECTING: why
     * the connection ended (RCS_ERROR_DISCONNECTED, RCS_ERROR_RESTARTING or
     * RCS_ERROR_NETWORK). Otherwise RCS_OK. */
    rcs_result result;
} rcs_state_event;

/* Whether capture is paused, and why. The run stays connected and the viewer
 * keeps the last picture; capture resumes by itself once the reason passes,
 * except GONE. */
typedef uint32_t rcs_capture_pause;
enum {
    RCS_CAPTURE_PAUSE_NONE = 0, /* Capturing. */
    /* The desktop cannot be captured now: a lock screen, a secure desktop
     * (UAC), a display that sleeps or changes its mode, or one disconnected
     * (for 10 s at most: then capture fails). */
    RCS_CAPTURE_PAUSE_DESKTOP = 1,
    /* What is shared does not show: the window minimized or hidden, none of
     * the application's windows on the display, the region off the display. */
    RCS_CAPTURE_PAUSE_HIDDEN = 2,
    /* The window was closed or the application quit: capture waits for the
     * host to share something else (rcs_session_update). */
    RCS_CAPTURE_PAUSE_GONE = 3,
    RCS_CAPTURE_PAUSE_AGENT = 4 /* No agent is attached (rcs_agent_link_config). */
};
typedef struct rcs_capture_event {
    rcs_capture_pause paused; /* NONE: capture resumed. */
} rcs_capture_event;

typedef struct rcs_codec_event {
    rcs_codec_direction direction;
    rcs_codec codec;
    rcs_acceleration acceleration;
    rcs_backend backend;
    /* Encoders (decoders: 0): works one frame at a time with the least
     * latency. macOS hardware encoding starts so and switches once to its
     * pipelined hardware encoder when that cannot sustain the frame rate (4K60
     * on an M4 Pro): about 15 ms more per frame, the full frame rate. H.264
     * starts pipelined there beyond its highest level's 2 073 600 macroblocks
     * a second (4096x2160 above 60 FPS, 4096x2304 above 56). */
    uint32_t low_latency;
    /* Encoders (decoders: 0): drops pictures a viewer missed from its
     * references instead of making a keyframe (rcs_codec_implementation.
     * ref_invalidation; rcs_video_tuning_config.ref_invalidation turns it
     * off). */
    uint32_t ref_invalidation;
    /* Encoders (decoders: 0): the temporal layers it makes, at most those
     * asked for (RCS_EXT_ENCODINGS scalability), as many as it can (1: none;
     * macOS H.264 none once it switched to its pipelined encoder). */
    uint32_t temporal_layers;
    /* The implementation, as listed by rcs_list_codec_implementations; for
     * the host's own codec (backend RCS_BACKEND_HOST), the name it gave, and
     * the acceleration and low_latency it said. */
    char name[64];
} rcs_codec_event;

typedef struct rcs_recovery_event {
    rcs_recovery_reason reason;
    rcs_recovery_method method;
    /* View, LOSS: how long the picture stood still: from the arrival of the
     * last picture before the loss to that of the one decoding went on with. */
    uint64_t frozen_ns;
} rcs_recovery_event;

/* Why a keyframe is due (RCS_EVENT_KEYFRAME). */
typedef uint32_t rcs_keyframe_reason;
enum {
    RCS_KEYFRAME_START = 1,     /* A stream starts (a viewer connected, or the stream changed). */
    RCS_KEYFRAME_REQUESTED = 2, /* The viewer lost a picture, or the host asked (rcs_session_request_keyframe). */
    RCS_KEYFRAME_INTERVAL = 3   /* rcs_video_tuning_config.keyframe_interval_ms passed. */
};
typedef struct rcs_keyframe_event {
    rcs_keyframe_reason reason;
} rcs_keyframe_event;

typedef struct rcs_rate_event {
    uint32_t bitrate_bps;    /* The video's new target. */
    uint32_t loss_permille;  /* Datagrams lost on the path recently. */
    uint64_t received_bps;   /* What reached the viewer. */
    uint32_t queue_delay_ms; /* Queueing on the path. */
    uint32_t round_trip_ms;
} rcs_rate_event;

/* A lower encoding of stream_index (RCS_EXT_ENCODINGS) started (running 1:
 * its codec, size, frame rate, first bitrate and the temporal layers its
 * encoder makes), stopped (running 0, result
 * RCS_OK: no viewer used it for lower_idle_ms, or the settings no longer have
 * it), or could not start or failed (running 0: result and message say why,
 * e.g. RCS_ERROR_UNSUPPORTED when the system has no encoder session to
 * spare). The main encoding's starts are RCS_EVENT_STREAM and RCS_EVENT_CODEC;
 * encoding 0 here is a change of the main encoding that its encoder did not
 * take while the stream ran (another codec, size, frame rate or format:
 * those fields, with the bitrate it would have started at; result and message
 * say why): the stream goes on as it was (running 1, the viewers see no
 * change), and the same settings are not tried again until they change.
 * What changes what is shared (rcs_capture_config) and fails ends the run. */
typedef struct rcs_encoding_event {
    uint32_t encoding;
    uint32_t running;
    rcs_codec codec;
    uint32_t width, height, frames_per_second, bitrate_bps, temporal_layers;
    rcs_result result;
    char message[128]; /* UTF-8, cut short to fit; empty while running. */
} rcs_encoding_event;

/* Why a viewer gets another encoding, or more or fewer of its temporal
 * layers (RCS_EVENT_VIEWER_VIDEO).
 *   JOINED    The one it starts with: not the main one (its decoder, or the
 *             host's choice in its decision).
 *   PATH      Its path carried less than what it gets sends (down: a layer
 *             fewer where its encoding has more than it gets, else the
 *             encoding below), or again what a layer more or the encoding
 *             above sends (up).
 *   LOSS      It caused rcs_video_tuning_config.loss_keyframes_move keyframes
 *             of its encoding within a minute.
 *   DECODER   It does not decode the codec, format or size of the one it had.
 *   HOST      rcs_session_set_viewer_video.
 *   ENCODING  The one it had stopped or could not start (RCS_EVENT_ENCODING). */
typedef uint32_t rcs_viewer_video_reason;
enum {
    RCS_VIEWER_VIDEO_JOINED = 1,
    RCS_VIEWER_VIDEO_PATH = 2,
    RCS_VIEWER_VIDEO_LOSS = 3,
    RCS_VIEWER_VIDEO_DECODER = 4,
    RCS_VIEWER_VIDEO_HOST = 5,
    RCS_VIEWER_VIDEO_ENCODING = 6
};
typedef struct rcs_viewer_video_event {
    uint32_t encoding; /* The one it gets from now on... */
    rcs_viewer_video_reason reason;
    uint32_t target_bps; /* What its path carries now, as adaptive bitrate measured it (0: not yet). */
    uint32_t layers;     /* ...and the temporal layers of it it gets (1: layer 0 alone). */
} rcs_viewer_video_event;

/* Which way sound goes, as this end sees it. */
typedef uint32_t rcs_audio_direction;
enum { RCS_AUDIO_SENDING = 1, RCS_AUDIO_RECEIVING = 2 };

typedef struct rcs_audio_event {
    rcs_audio_direction direction; /* A sharer's are each about one viewer (viewer_id). */
    uint32_t active;       /* Sound flows (sending: is captured and sent; receiving: arrives and plays). */
    rcs_audio_codec codec; /* Active: the codec in use; else 0. */
    rcs_result result;     /* Not active: why, e.g. RCS_ERROR_CAPTURE; RCS_OK when an end has audio off. */
    char message[128];     /* UTF-8, e.g. "No output device"; empty when active. */
    /* Sending from a microphone with voice_processing: 1 while the system
     * cancels echo and suppresses noise, 0 where it does not (it records
     * without). */
    uint32_t voice_processing;
} rcs_audio_event;

/* The pointer's look. type names the standard cursor it is, where it is one
 * (a host may draw its own version of that); pixels always hold the image. */
typedef uint32_t rcs_cursor_type;
enum {
    RCS_CURSOR_CUSTOM = 0, /* An application's own image. */
    RCS_CURSOR_ARROW = 1,
    RCS_CURSOR_TEXT = 2,     /* I-beam. */
    RCS_CURSOR_HAND = 3,     /* A link. */
    RCS_CURSOR_WAIT = 4,     /* Busy. */
    RCS_CURSOR_PROGRESS = 5, /* Arrow with busy indicator. */
    RCS_CURSOR_CROSSHAIR = 6,
    RCS_CURSOR_MOVE = 7,
    RCS_CURSOR_RESIZE_EW = 8,
    RCS_CURSOR_RESIZE_NS = 9,
    RCS_CURSOR_RESIZE_NWSE = 10,
    RCS_CURSOR_RESIZE_NESW = 11,
    RCS_CURSOR_NOT_ALLOWED = 12,
    RCS_CURSOR_GRAB = 13,     /* Open hand. */
    RCS_CURSOR_GRABBING = 14, /* Closed hand. */
    RCS_CURSOR_HELP = 15
};
/* In the shared display's pixels: scale by the view's size over
 * rcs_stream_info.display_width to draw it as the sharer sees it. */
typedef struct rcs_cursor_shape {
    uint32_t id; /* The same image keeps its id for the run. */
    rcs_cursor_type type;
    uint32_t width, height;
    uint32_t hotspot_x, hotspot_y; /* The pixel that points. */
    uint32_t stride_bytes;
    const uint8_t* pixels; /* BGRA8, straight alpha. */
} rcs_cursor_shape;

/* How many pointer images a viewing host keeps: the last ones it was given. */
#define RCS_MAX_CURSOR_SHAPES 32

typedef struct rcs_cursor_event {
    uint32_t visible; /* Shown, and on a shared display. */
    uint32_t stream;  /* The stream whose display it is on (rcs_streams_config); 0 without it. */
    uint32_t x, y;    /* The hotspot, absolute 0..65535 across that stream's display (as input). */
    uint32_t shape_id;
    /* The image, when shape_id is not among the last RCS_MAX_CURSOR_SHAPES
     * images this run reported: the first time it shows, or again after that
     * many others; NULL otherwise. The host keeps the last
     * RCS_MAX_CURSOR_SHAPES images it was given, dropping the oldest given
     * first, and needs no more, however many a sharer sends. */
    const rcs_cursor_shape* shape;
} rcs_cursor_event;

/* Borrowed for the duration of on_event. The member named after the type is
 * valid: RCS_EVENT_STATE -> state, RCS_EVENT_STREAM -> stream, ...
 *
 * viewer_id, share: the viewer the event concerns - PEER, VIEWER, ADMISSION,
 * CONTROL, DATA, AUDIO, MODULE, VIEWER_VIDEO, PATH and STALLS its own; STREAM when
 * only its sound changed; RATE the viewer
 * whose path holds the bitrate where it is; RECOVERY and KEYFRAME the viewer
 * that asked. 0 on every other event, and always on the view side.
 *
 * stream_index: the stream (rcs_streams_config) a STREAM, CODEC, CAPTURE,
 * RECOVERY, KEYFRAME, CURSOR or ENCODING event concerns; 0 on every other
 * event, and without it. */
/* What is paused now (RCS_PAUSE_* bits), and on whose account: the host's
 * (rcs_session_pause) and the application's background (rcs_background_config)
 * each pause their own, and resuming one leaves what the other paused; a
 * viewer also hears what its sharer paused for every viewer (by_peer; a
 * sharer's is 0). paused is all of them together. */
typedef struct rcs_pause_event {
    uint32_t paused;
    uint32_t by_host;
    uint32_t by_background;
    uint32_t by_peer;
} rcs_pause_event;

/* How a path reaches the other end. */
typedef uint32_t rcs_path_route;
enum {
    RCS_ROUTE_NONE = 0,
    RCS_ROUTE_DIRECT = 1, /* To the other end's own address (UDP, TCP; P2P's host candidates). */
    RCS_ROUTE_NAT = 2,    /* P2P through the NATs (reflexive candidates). */
    RCS_ROUTE_RELAY = 3,  /* Through the TURN server. */
    RCS_ROUTE_HOST = 4,   /* Over the host's channel. */
    RCS_ROUTE_FORWARDER = 5 /* Through the forwarder (RCS_TRANSPORT_FORWARDER). */
};
/* A path of the connection, as this end sees it. */
typedef struct rcs_path_info {
    rcs_path_route route;
    rcs_turn_transport relay_transport; /* RELAY: how this end reaches its TURN server. */
    rcs_network_kind network;           /* The network this end sends over (UNKNOWN: the system does not say). */
    uint32_t network_flags;             /* Its RCS_NETWORK_FLAG_* bits. */
    char interface_name[32];            /* UTF-8; empty where unknown. */
    /* "192.168.1.5:50000", "[2001:db8::5]:50000"; empty over the host's
     * channel. RELAY: this end's on the way to the TURN server, and the other
     * end's relayed one (or its own, through this end's allocation). */
    char local_address[64];
    char remote_address[64];
} rcs_path_info;
/* A change of the connection's path (rcs_paths_config). On a sharer, viewer_id says whose.
 *   CHANGING  This end looks for a path: its network changed and the path did
 *             not answer, or the path was silent for path_stall_ms
 *             (rcs_network_tuning_config). The video, sound and data stop
 *             until it is back; control messages wait.
 *   CHANGED   The traffic goes over `to` (from `from`); after a stall that
 *             passed, the path may be the same. interruption_ns: from the last
 *             datagram over `from` to the first over `to` (0 when nothing was
 *             missed). signaled: 1 when the path `to` was found through the
 *             host's signalling (on_signal), now or, a standby path, before.
 *   LOST      Nothing came for idle_timeout_ms, or no network is left that this
 *             end may use: the connection is kept (a viewer: RECONNECTING; a
 *             sharer: path_hold_ms), `result` and `message` say why.
 *   STANDBY   The standby path (rcs_paths_config.standby) is ready over `to`
 *             (result OK), or there is none (to.route NONE): it took over from
 *             a stalled path, or standby was turned off (both result OK), or
 *             `result` and `message` say why not.
 * Causes:
 *   NETWORK    This end's network changed (the system said so, or the host:
 *              rcs_notify_network_change), or its socket failed. STANDBY: the
 *              networks there are for one.
 *   STALLED    The path went silent: CHANGING; CHANGED: it came back, or the
 *              standby path took over from it, or back again; STANDBY: the
 *              standby path stopped answering, or took over.
 *   PEER       The other end came from another address and had proven it first
 *              (or took the standby path).
 *   REBINDING  The other end came from another address without knowing it: a
 *              NAT changed its mapping (a guess: it did not prove the path first).
 *   SETTINGS   rcs_paths_config: the network in use was excluded, the policy
 *              moved the connection to the system's default network, or
 *              standby was turned off.
 *   HOST       rcs_session_transport_restarted. */
typedef uint32_t rcs_path_change;
enum { RCS_PATH_CHANGING = 1, RCS_PATH_CHANGED = 2, RCS_PATH_LOST = 3, RCS_PATH_STANDBY = 4 };
typedef uint32_t rcs_path_cause;
enum {
    RCS_PATH_CAUSE_NETWORK = 1,
    RCS_PATH_CAUSE_STALLED = 2,
    RCS_PATH_CAUSE_PEER = 3,
    RCS_PATH_CAUSE_REBINDING = 4,
    RCS_PATH_CAUSE_SETTINGS = 5,
    RCS_PATH_CAUSE_HOST = 6
};
typedef struct rcs_path_event {
    rcs_path_change change;
    rcs_path_cause cause;
    rcs_path_info from, to;
    uint64_t interruption_ns;
    uint32_t signaled;
    rcs_result result;
    char message[128]; /* UTF-8, cut short to fit. */
} rcs_path_event;

/* Short stalls of the path (rcs_network_stats: stalls) began to come at a
 * steady pace, changed it, or no longer come so (period_ms 0). from_peer 0:
 * in what this end receives; 1: in what the other end receives, as it said
 * (a sharer: viewer_id's). Where this end's own path goes over Wi-Fi, `wifi`
 * is that network now (rcs_list_wifi; its name empty otherwise): with
 * other_channel 1 and a pace near 1050 ms, AWDL takes the radio (see
 * rcs_wifi_info); at the other end, that end's host knows its own. */
typedef struct rcs_stalls_event {
    uint32_t from_peer;
    uint32_t period_ms;
    uint32_t stall_ms; /* The median one's length... */
    uint32_t stalls;   /* ...of those of the last 10 s. */
    rcs_path_info path; /* This end's path now. */
    rcs_wifi_info wifi;
} rcs_stalls_event;

typedef struct rcs_event {
    uint32_t struct_size;
    rcs_event_type type;
    uint64_t timestamp_ns; /* Local monotonic clock. */
    rcs_viewer_id viewer_id;
    uint32_t stream_index;
    union {
        rcs_state_event state;
        rcs_peer_info peer;
        rcs_viewer_info viewer;
        rcs_pin_event pin;
        rcs_admission_event admission;
        rcs_stream_event stream;
        rcs_codec_event codec;
        rcs_capture_event capture;
        rcs_displays_event displays;
        rcs_recovery_event recovery;
        rcs_keyframe_event keyframe;
        rcs_rate_event rate;
        rcs_cursor_event cursor;
        rcs_encoding_event encoding;
        rcs_viewer_video_event viewer_video;
        rcs_audio_event audio;
        rcs_control_info control;
        rcs_data_event data;
        rcs_module_event module;
        rcs_agent_info agent;
        rcs_desktop_info desktop;
        rcs_pause_event pause;
        rcs_path_event path;
        rcs_stalls_event stalls;
    };
} rcs_event;

/* ------------------------------------------------------------------------ */
/* Statistics                                                               */
/* ------------------------------------------------------------------------ */

/* Counters accumulate per run; durations describe the most recent item, in
 * nanoseconds on the local clock. Each section has its own structure;
 * requesting a section the session's role does not produce is UNSUPPORTED.
 * A sharer's NETWORK, AUDIO_SEND, DATA and INPUT belong to its viewers'
 * connections: rcs_session_get_stats adds up the counters of every viewer of
 * the run (those that left too; the sound's captured_frames is the one
 * capture's), and its gauges (the path, round trip, loss, queueing, received
 * rate, the last_* values, the codec and encoder) are the main viewer's; one
 * viewer's are rcs_session_get_viewer_stats'. CAPTURE, VIDEO_SEND and
 * VIDEO_RECEIVE belong to a stream (rcs_streams_config): rcs_session_get_stats
 * gives stream 0's, rcs_session_get_stream_stats each one's; VIDEO_SEND is
 * the main encoding's, rcs_session_get_encoding_stats has each one's.
 * VIEWER_VIDEO belongs to a viewer alone (rcs_session_get_viewer_stats). */
typedef uint32_t rcs_stats_section;
enum {
    RCS_STATS_CAPTURE = 1,       /* rcs_capture_stats        Share */
    RCS_STATS_VIDEO_SEND = 2,    /* rcs_video_send_stats     Share */
    RCS_STATS_AUDIO_SEND = 3,    /* rcs_audio_send_stats     Both: the sound this end sends (a sharer: to all its viewers) */
    RCS_STATS_VIDEO_RECEIVE = 4, /* rcs_video_receive_stats  View */
    RCS_STATS_AUDIO_RECEIVE = 5, /* rcs_audio_receive_stats  Both: the sound this end receives (a sharer: from all) */
    RCS_STATS_NETWORK = 6,       /* rcs_network_stats        Share, view */
    RCS_STATS_INPUT = 7,         /* rcs_input_stats          Share, view */
    RCS_STATS_DATA = 8,          /* rcs_data_stats           Share, view */
    RCS_STATS_GROUP = 9,         /* rcs_group_stats          Share: the viewers and the encodings they get */
    RCS_STATS_VIEWER_VIDEO = 10, /* rcs_viewer_video_stats   Share, one viewer's (rcs_session_get_viewer_stats) */
    RCS_STATS_FORWARDER = 11     /* rcs_forwarder_stats      FORWARDER, both: the hop, the stream key and the pictures sealed */
};

/* With capture source HOST, captured and overwritten count pushed frames. */
typedef struct rcs_capture_stats {
    uint32_t struct_size;
    rcs_capture_pause paused; /* Whether capture waits now, and for what (as RCS_EVENT_CAPTURE says). */
    uint64_t captured_frames;
    uint64_t overwritten_frames; /* Captured frames replaced before encoding. */
    uint64_t interruptions;      /* How often capture paused. */
} rcs_capture_stats;
#define RCS_CAPTURE_STATS_INIT {sizeof(rcs_capture_stats), 0, 0, 0, 0}
RCS_INLINE rcs_capture_stats rcs_capture_stats_init(void) {
    rcs_capture_stats value = RCS_CAPTURE_STATS_INIT;
    return value;
}

typedef struct rcs_video_send_stats {
    uint32_t struct_size;
    char encoder[64];         /* The implementation in use, */
    uint32_t low_latency;     /* whether it works one frame at a time (as in the codec event), */
    uint32_t temporal_layers; /* the temporal layers it makes (1: none), */
    uint32_t bitrate_bps; /* and the target it is given now (adaptive: as the path allows; an encoder that
                             refuses a change keeps the previous one and says so in the log). */
    /* Each side's percentage of the stream's size it runs at now: below 100
     * while rcs_video_tuning_config.degradation SIZE or BALANCED made it smaller. */
    uint32_t size_percent;
    /* Time pictures were held back this run: adaptive bitrate's budget spent,
     * or the path behind (the frame rate fell). */
    uint64_t limited_ms;
    uint64_t encoded_frames;
    uint64_t keyframes;
    /* Pictures encoded after the encoder dropped ones a viewer missed from
     * its references (rcs_video_tuning_config.ref_invalidation), instead of
     * a keyframe. */
    uint64_t invalidations;
    uint64_t encoded_bytes;
    uint64_t base_layer_bytes; /* Of them, those of temporal layer 0 (all without layers). */
    uint64_t preview_frames;   /* Frames given to on_frame / on_gpu_frame. */
    uint64_t last_encode_ns;   /* Submit to packet. */
    uint64_t last_pipeline_ns; /* Capture to packet. */
} rcs_video_send_stats;
#define RCS_VIDEO_SEND_STATS_INIT {sizeof(rcs_video_send_stats), {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_video_send_stats rcs_video_send_stats_init(void) {
    rcs_video_send_stats value = RCS_VIDEO_SEND_STATS_INIT;
    return value;
}

typedef struct rcs_audio_send_stats {
    uint32_t struct_size;
    uint32_t active;
    rcs_audio_codec codec;
    char encoder[64]; /* E.g. "libopus 1.6.1". */
    /* What the sound takes on the path: the codec's bitrate, twice it while
     * each packet carries the one before (rcs_audio_tuning_config.redundancy). */
    uint32_t bitrate_bps;
    rcs_audio_capture_backend capture_backend; /* The backend in use (AUTO resolved)... */
    uint32_t capture_period_frames;            /* ...and the frames it delivers at a time. */
    uint64_t captured_frames; /* Sample frames (48 000 per second). */
    uint64_t encoded_packets;
    uint64_t encoded_bytes;
    uint64_t interruptions; /* How often audio paused. */
    uint64_t last_encode_ns;
} rcs_audio_send_stats;
#define RCS_AUDIO_SEND_STATS_INIT {sizeof(rcs_audio_send_stats), 0, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_audio_send_stats rcs_audio_send_stats_init(void) {
    rcs_audio_send_stats value = RCS_AUDIO_SEND_STATS_INIT;
    return value;
}

typedef struct rcs_video_receive_stats {
    uint32_t struct_size;
    char decoder[64]; /* The implementation in use. */
    uint64_t received_packets;
    uint64_t queued_packets;
    uint64_t dropped_packets;
    uint64_t recovery_requests;
    uint64_t decoder_resets;
    /* Decoder failures recovered by a new decoder from the next keyframe. A
     * second failure before a picture decodes ends the run. */
    uint64_t decoder_recoveries;
    uint64_t decoded_frames;
    uint64_t delivered_frames;
    uint64_t last_queue_ns; /* Arrival to decode start. */
    uint64_t last_decode_ns;
    uint64_t last_pipeline_ns; /* Arrival to decoded frame. */
} rcs_video_receive_stats;
#define RCS_VIDEO_RECEIVE_STATS_INIT {sizeof(rcs_video_receive_stats), {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_video_receive_stats rcs_video_receive_stats_init(void) {
    rcs_video_receive_stats value = RCS_VIDEO_RECEIVE_STATS_INIT;
    return value;
}

typedef struct rcs_audio_receive_stats {
    uint32_t struct_size;
    uint32_t active;
    rcs_audio_codec codec;
    char decoder[64];
    char device[128];                /* The output device playing it; empty when the host plays. */
    uint32_t playback_period_frames; /* Its period in use... */
    uint32_t playback_queue_frames;  /* ...and what is kept queued in it (Windows). */
    uint32_t buffer_ms;              /* Sound buffered now... */
    uint32_t target_ms; /* ...and what the buffer aims for, from the network's jitter. */
    uint64_t received_packets;
    uint64_t lost_packets;      /* Never arrived or arrived too late, and not recovered: concealed. */
    uint64_t recovered_packets; /* Lost ones whose copy the next packet carried (redundancy): played as sent. */
    uint64_t concealed_ms;  /* Sound made up for lost packets and underruns. */
    uint64_t dropped_ms;    /* Sound dropped to keep the delay bounded (clock drift, bursts). */
    uint64_t underruns;     /* The device asked for sound the buffer did not have. */
    /* The device went without sound the buffer had: the system called for it
     * too late (a busy machine, the system's scheduling), and the listener
     * heard a click. Android: AAudio's underruns; Apple: render cycles the
     * device skipped; Windows: the device's queue found empty. 0 while the
     * host plays (on_audio). */
    uint64_t device_underruns;
    uint64_t played_frames; /* Sample frames handed to the device or on_audio. */
} rcs_audio_receive_stats;
#define RCS_AUDIO_RECEIVE_STATS_INIT {sizeof(rcs_audio_receive_stats), 0, 0, {0}, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_audio_receive_stats rcs_audio_receive_stats_init(void) {
    rcs_audio_receive_stats value = RCS_AUDIO_RECEIVE_STATS_INIT;
    return value;
}

typedef struct rcs_network_stats {
    uint32_t struct_size;
    /* UDP, P2P, RELAY and HOST: */
    char path[128];          /* E.g. "P2P 0.0.0.0:5000 <-> 203.0.113.7:6000 (srflx)", "host datagram channel". */
    uint64_t round_trip_ns;
    uint64_t received_bps;   /* Share, adaptive: what reaches the viewer (video, sound, messages). */
    uint32_t loss_permille;  /* Share, adaptive. */
    uint32_t queue_delay_us; /* Share, adaptive: queueing on the path. */
    uint64_t rate_reports;   /* Share, adaptive: the viewer's reports decided on this run... */
    uint64_t rate_overrides; /* ...and those where RCS_EXT_RATE_DECIDER changed the rules' proposal. */
    /* Share: the rate the viewer measures its pictures arriving at (the
     * path's, where a bottleneck spaces them; the upper quartile of the last
     * second, or the latest of the last 10 s), which video's pace follows
     * (rcs_video_tuning_config.path_pacing_percent); 0 while unknown. */
    uint64_t path_bps;
    /* Repairing lost video (rcs_video_tuning_config.fec_mode), this run: */
    uint32_t fec_percent;      /* Share: parity sent now, per 100 video fragments. */
    uint64_t fec_sent;         /* Share: parity fragments sent. */
    uint64_t fec_recovered;    /* View: video fragments rebuilt from parity... */
    uint64_t resend_recovered; /* ...and those that came when asked for again... */
    uint64_t frames_lost;      /* ...and pictures given up on (each an RCS_EVENT_RECOVERY). */
    /* Share: video dropped before it was sent because the path carried far
     * less than the bitrate (it waited a second here, or several MiB waited
     * to go out): how often, and the bytes. The viewer resumes at a keyframe. */
    uint64_t backlog_drops;
    uint64_t backlog_dropped_bytes;
    /* Sound datagrams sent with rcs_network_tuning_config.audio_dscp. Fewer
     * than were sent: unmarked (0), a path that carries them inside another
     * protocol (TCP, RELAY over TCP or TLS, HOST), or the system refused (the log
     * says why). */
    uint64_t marked_packets;
    /* Sound, the most within the last one to two seconds: how long what this
     * end sent waited in it before it left (behind its own traffic), and how
     * long what it received queued on the path (its one-way delay above the
     * lowest of the last 10 s: behind the other end's video at a bottleneck,
     * a Wi-Fi radio's nap). */
    uint32_t sound_wait_us;
    uint32_t sound_queue_us;
    /* The path (rcs_paths_config, RCS_EVENT_PATH), UDP, P2P, RELAY, HOST and
     * TCP: how it goes now... */
    rcs_path_route route;
    rcs_network_kind network; /* ...over which of this end's networks... */
    uint32_t network_flags;   /* ...and what the system says of it (RCS_NETWORK_FLAG_*); */
    uint32_t path_lost;       /* 1 while no path reaches the other end (from RCS_PATH_LOST until one is back); */
    uint32_t standby;                 /* 1 while a standby path is ready (rcs_paths_config.standby)... */
    rcs_network_kind standby_network; /* ...and its network. */
    uint64_t path_changes;            /* Moves to another path this run (RCS_PATH_CHANGED, also back after a stall)... */
    uint64_t last_interruption_ns;    /* ...and the latest one's interruption. */
    /* Short stalls of the path (RCS_EVENT_STALLS): nothing arrived for 30 ms
     * or more while the other end kept sending (what came after waited on
     * the path, or was lost), as when a Wi-Fi radio leaves its channel for a
     * while (AWDL, rcs_wifi_info; a scan) or this end's network thread was
     * held up. Of what this end received in the last 10 s: how many, the
     * median one's length, the longest, and their pace when they come
     * steadily (0 otherwise)... */
    uint32_t stalls;
    uint32_t stall_ms;
    uint32_t longest_stall_ms;
    uint32_t stall_period_ms;
    /* ...and the pace and median length of those in what the other end
     * receives, as it last said (it says when they start, change or stop). */
    uint32_t peer_stall_period_ms;
    uint32_t peer_stall_ms;
    uint64_t metered_bytes; /* Sent and received over metered networks, including protocol overhead. */
    /* Every transport: */
    uint64_t bytes; /* Sent and received, including protocol overhead. */
    /* 1 while this process holds the system's Wi-Fi out of power saving
     * (rcs_network_tuning_config.wifi_low_latency; Android: holds its lock,
     * which the system applies while Wi-Fi is in use); 0 where no Wi-Fi is
     * connected (Windows), the system has no such setting, or it failed (the
     * log says why). */
    uint32_t wifi_low_latency;
    /* Connections that passed the handshake this run: 1 for the first; share:
     * one per viewer served; view: one per reconnection more. */
    uint64_t connections;
    /* Share: connections dropped while listening because they failed the
     * handshake (wrong key or protocol version, silence, disconnect). */
    uint64_t rejected_connections;
    /* Share, RCS_SECRET_PIN: PIN handshakes that failed this run (each one an
     * RCS_EVENT_PIN). */
    uint64_t pin_failures;
    uint64_t last_receive_ns; /* View: reading the most recent packet. */
    uint64_t last_decrypt_ns; /* View: decrypting it. */
    /* HOST: datagrams the host's channel did not take (on_transport_send
     * returned 0) this run. */
    uint64_t host_refused;
} rcs_network_stats;
#define RCS_NETWORK_STATS_INIT                                                                                         \
    {sizeof(rcs_network_stats), {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_network_stats rcs_network_stats_init(void) {
    rcs_network_stats value = RCS_NETWORK_STATS_INIT;
    return value;
}

typedef struct rcs_input_stats {
    uint32_t struct_size;
    int32_t last_result;    /* Share: the OS's answer to the most recent event. */
    uint64_t received;      /* Share */
    uint64_t applied;       /* Share */
    uint64_t sent;          /* View */
    uint64_t last_queue_ns; /* View: send_input to sending. */
    uint64_t last_send_ns;  /* View */
} rcs_input_stats;
#define RCS_INPUT_STATS_INIT {sizeof(rcs_input_stats), 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_input_stats rcs_input_stats_init(void) {
    rcs_input_stats value = RCS_INPUT_STATS_INIT;
    return value;
}

typedef struct rcs_data_stats {
    uint32_t struct_size;
    uint64_t queued_bytes; /* Waiting to be sent. */
    uint64_t sent_messages;
    uint64_t sent_bytes;
    uint64_t received_messages;
    uint64_t received_bytes;
} rcs_data_stats;
#define RCS_DATA_STATS_INIT {sizeof(rcs_data_stats), 0, 0, 0, 0, 0}
RCS_INLINE rcs_data_stats rcs_data_stats_init(void) {
    rcs_data_stats value = RCS_DATA_STATS_INIT;
    return value;
}

/* Share: the run's viewers and the encodings they get (rcs_viewers_config,
 * RCS_EXT_ENCODINGS). The keyframes count those of every stream and
 * encoding, by why they came:
 *   joined    A viewer joined, or now takes the stream.
 *   switched  A viewer moved to the encoding.
 *   lost      A viewer lost a picture, fell behind or replaced its decoder.
 *   host      The host asked (rcs_session_request_keyframe), or
 *             keyframe_interval_ms passed.
 * A keyframe several asked for counts once, by the first reason above. */
typedef struct rcs_group_stats {
    uint32_t struct_size;
    uint32_t viewers;                       /* Connected now... */
    uint32_t viewers_on[RCS_MAX_ENCODINGS]; /* ...and on each encoding. */
    uint32_t encodings;                     /* Of stream 0, running now (1: the main one alone). */
    uint32_t main_bitrate_bps;              /* The main encoding's target, all streams together. */
    uint64_t send_bps; /* What left for all viewers over the last second: video, sound, the rest. */
    uint64_t keyframes_joined;
    uint64_t keyframes_switched;
    uint64_t keyframes_lost;
    uint64_t keyframes_host;
    uint64_t moves_down;        /* Viewers moved to a lower encoding (any reason)... */
    uint64_t moves_up;          /* ...and back up. */
    uint64_t layer_moves_down;  /* Viewers given fewer temporal layers of their encoding... */
    uint64_t layer_moves_up;    /* ...and more again. */
    uint64_t encodings_refused; /* Lower encodings the system would not start (AUTO). */
    /* Turns in which most of an encoding's viewers fell short together, taken
     * for the sharer's own uplink: the bitrate came down and no one moved. */
    uint64_t uplink_limited;
} rcs_group_stats;
#define RCS_GROUP_STATS_INIT {sizeof(rcs_group_stats), 0, {0, 0, 0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_group_stats rcs_group_stats_init(void) {
    rcs_group_stats value = RCS_GROUP_STATS_INIT;
    return value;
}

/* Share: how one viewer gets the picture. A viewer that left: as it was. */
typedef struct rcs_viewer_video_stats {
    uint32_t struct_size;
    uint32_t encoding;           /* The encoding it gets now (RCS_EXT_ENCODINGS)... */
    uint32_t layers;             /* ...and how many of its temporal layers. */
    uint32_t counts_for_bitrate; /* rcs_viewer_video. */
    uint32_t target_bps;         /* What its path carries, as adaptive bitrate measured it (0: not yet). */
    uint64_t frames_sent;        /* Pictures handed to its connection... */
    /* ...those of its encoding it did not get (waiting for a keyframe, a
     * second behind)... */
    uint64_t frames_skipped;
    /* ...and those of the temporal layers above the ones it gets (none of its
     * pictures refers to them). */
    uint64_t upper_layer_frames;
    uint64_t keyframes_caused; /* Of its encoding, for its loss or backlog while others watched it too... */
    /* ...and requests of its that waited for the next keyframe, as it had
     * caused one within viewer_keyframe_interval_ms. */
    uint64_t keyframes_waited;
    uint64_t moves_down;
    uint64_t moves_up;
    uint64_t layer_moves_down; /* Fewer temporal layers of its encoding, and more again. */
    uint64_t layer_moves_up;
} rcs_viewer_video_stats;
#define RCS_VIEWER_VIDEO_STATS_INIT {sizeof(rcs_viewer_video_stats), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_viewer_video_stats rcs_viewer_video_stats_init(void) {
    rcs_viewer_video_stats value = RCS_VIEWER_VIDEO_STATS_INIT;
    return value;
}

/* FORWARDER, both ends: the hop to the forwarder, the stream key, and the
 * pictures and sound it seals. A sharer seals each picture and each packet of
 * its sound once, under the key of `generation` at `step`: a new generation
 * (a key at random) when a viewer leaves or key_rotation_ms passed, handed to
 * the viewers that stay; a step on (the key ratcheted forward, which the
 * viewers derive themselves) at each keyframe, so that a viewer let in gets
 * the key of the newest keyframes of what it takes, which the forwarder kept
 * for it (it opens what was sealed from there: at most the forwarder's cache
 * time before it came), or of a step on from then: no viewer opens what was
 * sealed longer before it came, or after it left. A viewer opens what is
 * sealed under a key it holds; what does not verify (changed on the way),
 * whose header says other than what it seals, that came before, or under a
 * key it lacks is dropped and counted, and the stream starts over at the
 * next keyframe (sound: as a lost packet). Through a forwarder the sharer's
 * sound is encoded once for every viewer that takes its format (its
 * statistics are the session's RCS_STATS_AUDIO_SEND, not a viewer's). */
typedef struct rcs_forwarder_stats {
    uint32_t struct_size;
    uint64_t peer;           /* View: this end's number in the forwarder's room (its route at the sharer). */
    uint32_t generation;     /* The stream key: share, the one sealing; view, the latest that opened a picture... */
    uint32_t step;           /* ...and its step. */
    uint64_t rekeys;         /* Share: new generations so far (leaves, rotations). */
    uint64_t key_resends;    /* Share: keys sent again to a viewer that missed one. */
    uint64_t media_frames;   /* Share: pictures sealed and sent to the forwarder; view: opened. */
    uint64_t media_bytes;    /* ...their bytes, sealed. */
    uint64_t media_rejected; /* View: dropped: they did not verify, or their header did not match them. */
    uint64_t media_replayed; /* View: dropped: they came before. */
    uint64_t media_unkeyed;  /* View: dropped: under a key it lacks (it asked the sharer for the key). */
    uint64_t hop_round_trip_ns; /* The path to the forwarder. */
    uint64_t hop_bytes;         /* Sent and received over it. */
    /* Share: what the path to the forwarder carries now, as its adaptive
     * bitrate measures it (0 until measured): the encodings together keep
     * within it, less the sound (RCS_EVENT_RATE with viewer_id 0 when it
     * holds the main encoding back). */
    uint32_t uplink_bps;
    uint64_t uplink_limited; /* Share: turns in which it held the encodings back. */
    uint64_t sound_packets;  /* Share: packets of sound sealed and sent to the forwarder; view: opened. */
    rcs_forwarder_link link; /* How this end reaches it. */
} rcs_forwarder_stats;
#define RCS_FORWARDER_STATS_INIT {sizeof(rcs_forwarder_stats), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_forwarder_stats rcs_forwarder_stats_init(void) {
    rcs_forwarder_stats value = RCS_FORWARDER_STATS_INIT;
    return value;
}

/* A module's messages (rcs_module_config), of this run: to and from its kind
 * at the other end (rcsdk/module.h). Its own counters come from its own
 * functions. */
typedef struct rcs_module_stats {
    uint32_t struct_size;
    uint64_t queued_bytes; /* Waiting to be sent. */
    uint64_t sent_messages;
    uint64_t sent_bytes;
    uint64_t received_messages;
    uint64_t received_bytes;
} rcs_module_stats;
#define RCS_MODULE_STATS_INIT {sizeof(rcs_module_stats), 0, 0, 0, 0, 0}
RCS_INLINE rcs_module_stats rcs_module_stats_init(void) {
    rcs_module_stats value = RCS_MODULE_STATS_INIT;
    return value;
}

/* ------------------------------------------------------------------------ */
/* Session                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct rcs_session_impl* rcs_session;
/* An agent of another process (rcsdk/agent.h) and a supervisor
 * (rcsdk/supervisor.h): named here for log records. */
typedef struct rcs_agent_impl* rcs_agent;
typedef struct rcs_supervisor_impl* rcs_supervisor;

/* All callbacks run on SDK worker threads, may start before rcs_session_start
 * returns, must not throw and must return promptly. They may call getters,
 * rcs_session_update and the actions on the same session; start, stop and
 * destroy from any callback return RCS_ERROR_CALLBACK_THREAD. user_data must
 * outlive the session. The same holds for the callbacks of the replacing
 * layer's extensions (rcsdk/host_codec.h, rcsdk/rate_control.h,
 * rcsdk/host_transport.h, rcsdk/agent.h), each with its own user_data.
 *
 * on_event calls never overlap one another; dispatch to the UI thread from
 * there if needed. Events report changes; the getters return current values.
 *
 * Images go to at most one of on_gpu_frame and on_frame; setting both is
 * INVALID_ARGUMENT. on_gpu_frame receives native surfaces (software decoding
 * on Windows: BGRA8 textures, see software_decoding_gpu_frames). on_frame
 * receives CPU BGRA images, read back from the GPU when frames live there.
 * Leaving both NULL skips image delivery, e.g. for a sharer without a local
 * preview; a sharer's frame callback runs inside its capture and encode loop. */
/* A decoded picture on the GPU (on_gpu_frame; what it holds: rcsdk/gpu.h). */
typedef struct rcs_gpu_frame_impl* rcs_gpu_frame;

typedef struct rcs_session_callbacks {
    uint32_t struct_size;
    void* user_data;
    void (*on_event)(const rcs_event* event, void* user_data);
    void (*on_frame)(const rcs_frame* frame, void* user_data);
    void (*on_gpu_frame)(rcs_gpu_frame frame, void* user_data);
    /* The other end's sound, decoded, as it arrives (rcs_receive_audio_config),
     * in order, on the session's sound thread (never the connection's: a slow
     * on_audio holds up only the sound); with playback 0 the host plays it.
     * A sharer gets each viewer's (viewer_id), on a thread of each. Late or
     * lost packets are concealed before they reach it. */
    void (*on_audio)(const rcs_audio_frame* frame, void* user_data);
    /* Share: every remote input event while control is granted, before it is
     * executed; calls never overlap (also of several viewers). Target SYSTEM:
     * nonzero executes it, 0 drops it (release-all events are executed
     * regardless); the types of rcs_control_config.host_input_types the host
     * executes itself, and the return value is ignored for them. Target HOST:
     * the host executes it; the return value is ignored. A host that executes
     * input gets RCS_INPUT_RELEASE_ALL whenever it is to release what it holds
     * (control revoked, paused or moved, the viewer gone; with several viewers
     * it names the viewer and still means everything: whose a held key is
     * cannot be told). */
    int32_t (*on_input)(const rcs_input_event* event, void* user_data);
    /* A data message from the other end, on the thread that reads the
     * connection (video and input wait behind it): copy what is needed and
     * return. Never overlapping, also of several viewers. */
    void (*on_data)(const rcs_data_message* message, void* user_data);
    /* P2P and RELAY: this end's signal: deliver the ASCII text to the other
     * end, which passes it to rcs_session_signal. Any channel works (the
     * host's server, a message system it already has, an HTTP call); the text
     * holds addresses and one-time credentials, no keys. A viewer signals
     * once per connection (again when it reconnects), route 0. A sharer
     * answers each viewer's signal with one of its own: `route` is the one the
     * host passed with the viewer's (rcs_session_signal), so the host sends
     * the answer back that way without reading the text. Calls never overlap. */
    void (*on_signal)(const char* signal, uint64_t route, void* user_data);
    /* Share: a viewer passed the handshake (so its identity is proven: its
     * device key is viewer->peer.public_key) and the sharer can serve it; it
     * joins unless the host says otherwise. viewer: who it is (joined 0, its
     * viewer_id already given). decision: filled in with what happens without
     * this callback (RCS_ADMIT, control as rcs_control_config.allowed);
     * change it to refuse the viewer, to decide its control, or to decide
     * later (RCS_ADMIT_LATER, then rcs_session_admit_viewer). Calls never
     * overlap: return promptly; a host that asks its user returns
     * RCS_ADMIT_LATER. NULL admits every viewer. */
    void (*on_viewer_join)(const rcs_viewer_info* viewer, rcs_viewer_decision* decision, void* user_data);
} rcs_session_callbacks;
#define RCS_SESSION_CALLBACKS_INIT                                                                                     \
    {sizeof(rcs_session_callbacks), NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
RCS_INLINE rcs_session_callbacks rcs_session_callbacks_init(void) {
    rcs_session_callbacks value = RCS_SESSION_CALLBACKS_INIT;
    return value;
}

/* ---- Lifecycle ---------------------------------------------------------- */

/* Copies config, its extensions and callbacks. */
RCSDK_API rcs_result rcs_session_create(const rcs_session_config* config, const rcs_session_callbacks* callbacks,
                                        rcs_session* out_session);
/* Starts asynchronously; progress is reported through RCS_EVENT_STATE. A
 * stopped or failed session can be started again. A configuration this
 * system cannot carry out is refused here, the reason in
 * rcs_get_last_call_error and rcs_session_get_failure. The run lasts until
 * stop() or a failure; what a lost connection does to it is
 * rcs_reconnect_config. RCS_STATE_RUNNING is reported once the ends have
 * negotiated, before the connection's other events (codec, stream, sound,
 * control). */
RCSDK_API rcs_result rcs_session_start(rcs_session session);
/* Joins all workers; no callback of that run follows a successful return. The
 * peer is told the session stopped (it sees RCS_ERROR_DISCONNECTED). */
RCSDK_API rcs_result rcs_session_stop(rcs_session session);
/* Why a session stops, as the peer is told.
 *   NORMAL   The session is done (rcs_session_stop).
 *   RESTART  The host stops it to start it again, e.g. the program restarts for
 *            an update or its helper process moves to another login session.
 *            The peer reports RCS_ERROR_RESTARTING instead of
 *            RCS_ERROR_DISCONNECTED, and a viewer reconnects by itself
 *            (rcs_reconnect_config) where it would otherwise have given up. */
typedef uint32_t rcs_stop_reason;
enum { RCS_STOP_NORMAL = 0, RCS_STOP_RESTART = 1 };
RCSDK_API rcs_result rcs_session_stop_with_reason(rcs_session session, rcs_stop_reason reason);
/* Stops if needed and frees the session. Must not race other calls on it. */
RCSDK_API rcs_result rcs_session_destroy(rcs_session session);

/* Replaces the settings of each extension in the chain, all or none. Idle,
 * stopped or failed: any extension of the session's role; the next start
 * uses it. Running: what rcs_extension lists, effective at once or from the
 * next connection as it says (INVALID_STATE for what is fixed). A chained
 * extension is the host's from then on, the preset no longer sets it. The
 * reason for a refusal is in rcs_get_last_call_error. */
RCSDK_API rcs_result rcs_session_update(rcs_session session, const rcs_extension* changes);
/* The settings of one extension the session uses now (`extension`'s header
 * says which: type and struct_size; its next is kept), the preset's where the
 * host chained none. RCS_EXT_DEVICE_IDENTITY: trusted_key_count says how many
 * keys it trusts, trusted_keys is NULL. INVALID_ARGUMENT for RCS_EXT_MODULE
 * (a module's: rcs_session_get_module_stats and the module's own functions)
 * and types of the other role. */
RCSDK_API rcs_result rcs_session_get_settings(rcs_session session, rcs_extension* extension);
/* Another preset for the extensions the host has not chained (at create or
 * through rcs_session_update), at once, as rcs_session_update would apply
 * them. */
RCSDK_API rcs_result rcs_session_set_preset(rcs_session session, rcs_preset preset);
/* Another identity (rcs_session_config.identity), checked as at create;
 * applies from the next handshake. Also restarts the PIN count. */
RCSDK_API rcs_result rcs_session_set_identity(rcs_session session, const rcs_identity* identity);

/* ---- Queries ------------------------------------------------------------ */

RCSDK_API rcs_result rcs_session_get_state(rcs_session session, rcs_state* out_state);
/* Why the most recent start() or run failed: the result (RCS_OK when nothing
 * failed) and a description; message may be NULL with capacity 0. A codec the
 * peer cannot handle is RCS_ERROR_UNSUPPORTED, and so is a stream size no
 * codec of both ends takes (the reason names the codec and its limit, on
 * both ends); rcs_peer_info holds the peer's offer. */
RCSDK_API rcs_result rcs_session_get_failure(rcs_session session, rcs_result* out_result, char* message,
                                             uint32_t capacity);
/* stats points to the section's structure, struct_size set (*_STATS_INIT). */
RCSDK_API rcs_result rcs_session_get_stats(rcs_session session, rcs_stats_section section, void* stats);
/* A stream as it was last announced (RCS_EVENT_STREAM): 0, or one of
 * rcs_streams_config's. INVALID_ARGUMENT from RCS_MAX_STREAMS on; a stream
 * not running (yet) reads as RCS_STREAM_INFO_INIT with its number. */
RCSDK_API rcs_result rcs_session_get_stream_info(rcs_session session, uint32_t stream, rcs_stream_info* info);
RCSDK_API rcs_result rcs_session_get_peer_info(rcs_session session, rcs_peer_info* info);
RCSDK_API rcs_result rcs_session_get_control_info(rcs_session session, rcs_control_info* info);
/* The desktop the shared machine shows now (RCS_EVENT_DESKTOP reports changes;
 * a viewer has it once connected, UNKNOWN before). */
RCSDK_API rcs_result rcs_session_get_desktop_info(rcs_session session, rcs_desktop_info* info);
RCSDK_API rcs_result rcs_session_list_displays(rcs_session session, rcs_display_info* displays,
                                               uint32_t item_size, uint32_t capacity, uint32_t* out_count);
/* Share: the windows of the machine the session captures, as rcs_list_windows
 * lists them there. An agent in another process (rcs_agent_link_config) is
 * asked: INVALID_STATE while none is attached or when it leaves before it
 * answers, TIMEOUT when it does not answer within a second. UNSUPPORTED for
 * a viewing session. */
RCSDK_API rcs_result rcs_session_list_windows(rcs_session session, rcs_window_info* windows, uint32_t item_size,
                                              uint32_t capacity, uint32_t* out_count);
/* Share: the viewers connected now (rcs_viewers_config), in the order they
 * joined. UNSUPPORTED for a viewing session. */
RCSDK_API rcs_result rcs_session_list_viewers(rcs_session session, rcs_viewer_info* viewers, uint32_t item_size,
                                              uint32_t capacity, uint32_t* out_count);
/* Share: one viewer's statistics, of the sections that belong to a viewer's
 * connection: NETWORK, AUDIO_SEND (this end's sound to it), AUDIO_RECEIVE
 * (its sound to this end), DATA, INPUT and VIEWER_VIDEO (the stream's own are
 * rcs_session_get_stats': another section is INVALID_ARGUMENT). A viewer that
 * left this run: its final ones (from RCS_EVENT_VIEWER on; the 256 that left
 * last are kept, an older one is INVALID_STATE; DATA's queued_bytes is 0).
 * INVALID_ARGUMENT for an id this run never gave. Input that an agent in another process executes counts as
 * received here; its results (applied, last_result) come back for all
 * viewers together, in rcs_session_get_stats. */
RCSDK_API rcs_result rcs_session_get_viewer_stats(rcs_session session, rcs_viewer_id viewer, rcs_stats_section section,
                                                  void* stats);
/* The connection's path now and its standby path (rcs_paths_config; route
 * NONE for none), as RCS_EVENT_PATH last reported them: a viewer's (`viewer`
 * 0), or a sharer's to `viewer` (0: the main viewer). Either pointer may be
 * NULL. INVALID_STATE without a connection (a viewer before it connected, a
 * viewer that left); INVALID_ARGUMENT for an id this run never gave. */
RCSDK_API rcs_result rcs_session_get_path_info(rcs_session session, rcs_viewer_id viewer, rcs_path_info* active,
                                               rcs_path_info* standby);
/* One stream's statistics (rcs_streams_config), of the sections that belong
 * to a stream: CAPTURE and VIDEO_SEND (share), VIDEO_RECEIVE (view); another
 * section is INVALID_ARGUMENT, and so is a stream from RCS_MAX_STREAMS on.
 * rcs_session_get_stats gives stream 0's. */
RCSDK_API rcs_result rcs_session_get_stream_stats(rcs_session session, uint32_t stream, rcs_stats_section section,
                                                  void* stats);
/* Share: the encoder statistics of one encoding of a stream
 * (RCS_EXT_ENCODINGS): encoding 0 is the stream's VIDEO_SEND. INVALID_ARGUMENT
 * for a stream from RCS_MAX_STREAMS on or an encoding from RCS_MAX_ENCODINGS
 * on; INVALID_STATE for a lower encoding that does not run now; UNSUPPORTED
 * for a viewing session. */
RCSDK_API rcs_result rcs_session_get_encoding_stats(rcs_session session, uint32_t stream, uint32_t encoding,
                                                    rcs_video_send_stats* stats);
/* The messages of the session's module `module` (rcs_module_config): viewer
 * 0 the session's (a sharer's: every viewer's of the run added up), an id
 * one viewer's (share; a viewer that left as for rcs_session_get_viewer_stats).
 * INVALID_STATE when the session does not use the module. */
RCSDK_API rcs_result rcs_session_get_module_stats(rcs_session session, rcs_viewer_id viewer, const char* module,
                                                  rcs_module_stats* stats);

/* ---- Actions ------------------------------------------------------------ */

/* P2P and RELAY: the other end's signal (its on_signal text), for the run, or
 * the next one when called before start.
 *   Share: a viewer's. `route`: the host's own number for where it came from
 *          (a user, a connection, a pointer; the SDK never reads it). The
 *          sharer answers it through on_signal with the same route, finds
 *          the path to that viewer with an ICE exchange of its own (up to 32
 *          at a time, the rest wait their turn, at most 256: the one that
 *          waited longest gives way), and the viewer joins as over UDP,
 *          rcs_viewer_info.route naming the route. Each signal costs an
 *          exchange: a host that relays signals from anyone limits how often
 *          one sender may signal (only it knows who sent one).
 *   View:  the sharer's answer to this end's signal; `route` is not read.
 *          The connection takes the answer to its own signal, and of several
 *          (a sharer that restarted meanwhile) the one the sharer's
 *          connectivity checks name, before any arrive the latest.
 * Repeats, and signals left over from the peer's earlier connections, may be
 * passed as they come, in any order: a signal names the one it answers, a
 * repeat is skipped, and a sharer's attempt for a viewer signal no longer in
 * use ends when its checks time out (15 s, logged). INVALID_ARGUMENT for text
 * that is not a signal or comes from the same role; UNSUPPORTED for a session
 * of another transport. */
RCSDK_API rcs_result rcs_session_signal(rcs_session session, const char* signal, uint64_t route);
/* View: queue an input event (nonblocking). Consecutive moves are coalesced.
 * When the queue is full, pending events are replaced by release-all (so no
 * key stays held for events that were dropped) and QUEUE_FULL is returned;
 * stop capturing input when that happens. viewer_id must be 0. INVALID_STATE
 * while the session does not run or the sharer has not granted control
 * (RCS_EVENT_CONTROL); UNSUPPORTED for a sharing session. */
RCSDK_API rcs_result rcs_session_send_input(rcs_session session, const rcs_input_event* event);
/* Running: the next picture is a keyframe (view: the sharer is asked for one),
 * e.g. after the host lost its own copy of the stream. */
RCSDK_API rcs_result rcs_session_request_keyframe(rcs_session session);
/* Pauses or resumes what `what` says (RCS_PAUSE_* bits), for the host's own
 * reasons. A viewer: a window in the background or covered, its own flows. A
 * sharer: the user paused sharing, or the host's pictures stop for a while
 * (its viewers keep their last picture and hear it). Any state; a session
 * not running yet starts so. Resuming undoes the host's pause only, never
 * one of the background (rcs_background_config). RCS_EVENT_PAUSE reports
 * each change. A bit no version defines is INVALID_ARGUMENT. */
RCSDK_API rcs_result rcs_session_pause(rcs_session session, uint32_t what);
RCSDK_API rcs_result rcs_session_resume(rcs_session session, uint32_t what);
/* Share with capture source HOST, running: the next picture, BGRA8 of the
 * stream's size (rcs_stream_info width and height, RCS_EVENT_STREAM with
 * RCS_STREAM_SIZE when it changes); another size is INVALID_ARGUMENT, a
 * viewing session UNSUPPORTED. INVALID_STATE while no stream runs (before a
 * viewer's starts, while it restarts): that picture is not sent. Copied
 * (RCS_FRAME_INIT); the newest pending picture replaces an older one not yet
 * encoded. */
RCSDK_API rcs_result rcs_session_push_video_frame(rcs_session session, const rcs_frame* frame);
/* With rcs_send_audio_config.source HOST, running: sound to send (a sharer:
 * to each viewer that receives it), 48 kHz stereo interleaved float32,
 * 1..48 000 sample frames (RCS_AUDIO_FRAME_INIT; viewer_id 0). Copied and
 * handed to the session's sound thread, so the call never waits for the
 * encoder or the network; push it as it plays. INVALID_STATE with another
 * source. */
RCSDK_API rcs_result rcs_session_push_audio(rcs_session session, const rcs_audio_frame* frame);
/* Running: queues a message for the other end's on_data (see
 * rcs_data_message); nonblocking, the message is copied. Reliable messages
 * go out one after another, behind nothing but input and the sound; over UDP,
 * P2P, RELAY and HOST with rcs_network_tuning_config.in_flight_bytes on the way at a time, so
 * they take what the video leaves. QUEUE_FULL when the queue
 * (send_queue_bytes) cannot take it: wait for RCS_EVENT_DATA. Messages belong
 * to one connection: what is still queued when it ends is dropped (a host
 * waiting for room gets RCS_EVENT_DATA then), and between connections (a
 * sharer listening again, a viewer reconnecting) the call is INVALID_STATE.
 * A sharer has a queue per viewer (RCS_EVENT_DATA names it); viewer_id 0
 * queues the message in each, or in none when one has no room. A viewer_id
 * this run never gave is INVALID_ARGUMENT, one of a viewer that left
 * INVALID_STATE; a viewing session takes 0 only. */
RCSDK_API rcs_result rcs_session_send_data(rcs_session session, const rcs_data_message* message);
/* Share: disconnects one viewer; the others go on. Returns at once, also from
 * a callback: RCS_EVENT_VIEWER follows (it left with RCS_ERROR_DISCONNECTED),
 * and the viewer's run ends as when a sharer stops. INVALID_ARGUMENT for an id
 * this run never gave, INVALID_STATE for a viewer that left already. */
RCSDK_API rcs_result rcs_session_disconnect_viewer(rcs_session session, rcs_viewer_id viewer);
/* Share: whether one viewer may control, until it leaves or the next
 * RCS_EXT_CONTROL update (which sets every viewer's). Allowing it is refused
 * where rcs_control_config.allowed would be (PERMISSION_REQUIRED,
 * UNSUPPORTED); ids as for rcs_session_disconnect_viewer. */
RCSDK_API rcs_result rcs_session_set_viewer_control(rcs_session session, rcs_viewer_id viewer, uint32_t allowed);
/* Share: the viewer whose request decides the stream (rcs_viewers_config)
 * while it is connected; 0 gives that back to the one that joined first. The
 * stream restarts where the request differs. Ids as for
 * rcs_session_disconnect_viewer. */
RCSDK_API rcs_result rcs_session_set_main_viewer(rcs_session session, rcs_viewer_id viewer);
/* Share: how one viewer gets the picture from now on (rcs_viewer_video),
 * until it leaves; any thread, also from a callback. A move it makes follows
 * at its encoding's next turn (RCS_EVENT_VIEWER_VIDEO, reason HOST).
 * INVALID_ARGUMENT for video not set up with RCS_VIEWER_VIDEO_INIT,
 * counts_for_bitrate other than 0 or 1, an encoding from the stream's
 * encoding count on (RCS_EXT_ENCODINGS; SINGLE has 0 alone); ids as for
 * rcs_session_disconnect_viewer; UNSUPPORTED for a viewing session. */
RCSDK_API rcs_result rcs_session_set_viewer_video(rcs_session session, rcs_viewer_id viewer,
                                                  const rcs_viewer_video* video);
/* Share: the host's decision for viewer `viewer`, which waits since its
 * on_viewer_join said RCS_ADMIT_LATER (RCS_EVENT_ADMISSION). Any thread, any
 * time, from its on_viewer_join too. decision->admission is RCS_ADMIT or
 * RCS_REFUSE (INVALID_ARGUMENT otherwise). INVALID_STATE when no viewer
 * `viewer` waits: decided before, refused for the timeout, or gone. */
RCSDK_API rcs_result rcs_session_admit_viewer(rcs_session session, rcs_viewer_id viewer,
                                              const rcs_viewer_decision* decision);

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

/* What the SDK does and why, for the host's own logs: sessions starting and
 * ending, negotiation, the codecs opened, capture pauses, recoveries, refused
 * connections. Nothing is logged per frame.
 *
 * Each record names what it belongs to: the session, agent or supervisor
 * whose work wrote it, and the user_data of that object's callbacks, so a host
 * with several sessions files each record with its call, ticket or device
 * without a table of its own. A sharing session's own capture, encoding and input
 * belong to the session; an agent in another process logs there, as an agent.
 * PROCESS is work of no object, e.g. rcs_get_capabilities. No record names a
 * handle whose destroy function has returned. */
typedef uint32_t rcs_log_source;
enum {
    RCS_LOG_SOURCE_PROCESS = 0,
    RCS_LOG_SOURCE_SESSION = 1,
    RCS_LOG_SOURCE_AGENT = 2,
    RCS_LOG_SOURCE_SUPERVISOR = 3
};

/* Borrowed for the duration of the callback, strings included (UTF-8).
 * module names the SDK component, e.g. "session", "capture", "encoder",
 * "network". */
typedef struct rcs_log_record {
    uint32_t struct_size;
    rcs_log_level level;
    const char* module;
    const char* message;
    uint64_t timestamp_ns; /* Local monotonic clock, as in rcs_event. */
    rcs_log_source source;
    rcs_session session;       /* SESSION; otherwise NULL. */
    rcs_agent agent;           /* AGENT; otherwise NULL. */
    rcs_supervisor supervisor; /* SUPERVISOR; otherwise NULL. */
    void* user_data;           /* That object's callbacks' user_data; NULL for PROCESS. */
} rcs_log_record;

typedef void (*rcs_log_callback)(const rcs_log_record* record, void* user_data);

/* Process-wide: records go to callback from any SDK thread, one call at a
 * time; return promptly. NULL turns logging off (the default), for every
 * source. A record is passed when its level is at most max_level, or at most
 * the level of its session or agent where that has one (rcs_log_config). Once
 * this returns, the previous callback is not called again. From the log
 * callback, this, every start, stop and destroy function and rcs_agent_wait
 * return RCS_ERROR_CALLBACK_THREAD: the callback holds up every thread that
 * logs, so waiting for one there would never end. */
RCSDK_API rcs_result rcs_set_log_callback(rcs_log_level max_level, rcs_log_callback callback, void* user_data);

/* For libraries built on the SDK (rcsdk/supervisor.h, a host's own): writes
 * record as the SDK writes its own, to the callback above under its rules: it
 * is dropped when no callback is set, when its level is above max_level (or
 * the level of the session or agent it names, where that has one), and on the
 * log callback's own thread. It takes level, module, message, source and that
 * source's handle and user_data; timestamp_ns 0 is now. INVALID_ARGUMENT for a
 * record not initialised with this sizeof, a level or source no version
 * defines, a handle another source's or missing, or no module or message. */
RCSDK_API rcs_result rcs_log_write(const rcs_log_record* record);
/* Nonzero on the log callback's thread: there the SDK's functions that wait
 * for threads that log return RCS_ERROR_CALLBACK_THREAD, and a library's
 * should too. */
RCSDK_API int rcs_log_in_callback(void);

#ifdef __cplusplus
}
#endif
#endif
