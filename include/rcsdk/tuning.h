/* Remote Control SDK: tuning.
 *
 * The engine's parameters, for the few hosts that need them: how adaptive
 * bitrate decides, how video leaves, the decode queue, the sound's periods and
 * buffers, the network's queues and timeouts. Their defaults suit the common
 * case, and each preset (rcs_preset) gives them its own values; a host that
 * chains one of these extensions takes every field of it, so start from
 * rcs_preset_defaults. Read PERFORMANCE.md before changing them: each field
 * says what it costs. Fields of the other role are ignored. */
#ifndef RCSDK_TUNING_H
#define RCSDK_TUNING_H

#include <rcsdk/rcsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Video -------------------------------------------------------------- */

/* speed: how much time per picture the encoder spends on quality at the
 * bitrate, where it offers the choice (the host's encoder gets it in its
 * settings; encoders without the choice keep their own, logged).
 *   FASTEST       The least delay: NVENC's preset P1, Media Foundation's
 *                 quality-vs-speed 0, VideoToolbox's speed over quality,
 *                 MediaCodec at its highest operating rate (the hardware as
 *                 fast as it goes, not paced to the frame rate: 1080p in
 *                 5-8 ms on a Snapdragon 8 Elite, not 12-14; more power),
 *                 libvpx's real-time speed 9 (VP9) and -10 (VP8), libaom's
 *                 10, OpenH264's low complexity.
 *   BALANCED      The encoder's middle: NVENC P4, Media Foundation 50,
 *                 VideoToolbox's own default, MediaCodec paced to the frame
 *                 rate, libvpx 8 and -8, libaom 9, OpenH264's medium
 *                 complexity.
 *   BEST_QUALITY  The best picture the bitrate gives, at more time per
 *                 picture: NVENC P7, Media Foundation 100, VideoToolbox's
 *                 quality over speed, MediaCodec as BALANCED, libvpx 7 and
 *                 -6, libaom 8, OpenH264's high complexity.
 * NVENC keeps its ultra-low-latency tuning with each. Changing it while
 * running restarts the encoder. */
typedef uint32_t rcs_encoder_speed;
enum { RCS_ENCODER_FASTEST = 1, RCS_ENCODER_BALANCED = 2, RCS_ENCODER_BEST_QUALITY = 3 };

/* content: what the shared pictures mostly are, for the encoders that have
 * tools for either: the codec libraries built into the SDK (rcs_backend);
 * hardware encoders have none and ignore it.
 *   AUTO    DETAIL for displays, regions, windows and applications; MOTION
 *           for the host's pictures (RCS_CAPTURE_HOST).
 *   DETAIL  Text and user interfaces: libvpx's screen content mode, libaom's
 *           screen content tools (palette, intra block copy on keyframes).
 *           OpenH264 has none it can use (its screen mode makes a keyframe of
 *           every large change) and encodes either the same way.
 *   MOTION  Video, games, cameras.
 * software_threads: the threads each software encoder and decoder of those
 * libraries may use, 1..64; 0: the SDK's choice, half the logical
 * processors, at most 8. A machine running many sessions (cloud desktops)
 * keeps each one's CPU within bounds with it.
 * Changing either while running restarts the encoder with a keyframe, as
 * speed does; a decoder takes software_threads from its next stream on. */
typedef uint32_t rcs_content_hint;
enum { RCS_CONTENT_AUTO = 0, RCS_CONTENT_DETAIL = 1, RCS_CONTENT_MOTION = 2 };

/* Share: repairing lost video on datagram paths (UDP, P2P, RELAY, and HOST
 * with datagrams). Each picture's fragments go in groups of 64, each group
 * followed by Reed-Solomon parity fragments: as many lost fragments of a
 * group as it has parity come back as soon as the group arrived, without
 * asking again (a round trip); more are asked for again as before, and a
 * picture that still does not come is given up (RCS_EVENT_RECOVERY).
 *   OFF    No parity.
 *   AUTO   None while the viewer's reports show no loss; with loss, three
 *          times the loss rate, at least 10% and at most fec_percent; none
 *          again 5 s after the last loss. A LAN pays nothing.
 *   FIXED  fec_percent always, e.g. for paths known to lose.
 * fec_percent: parity fragments per 100 video fragments (at least one per
 * group), 1..100; AUTO's upper bound. Parity counts in the bitrate: the
 * encoder gets the adaptive target less the parity (not below the minimum).
 * What it costs and repairs: rcs_network_stats (fec_percent, fec_sent,
 * fec_recovered, resend_recovered, frames_lost). Over TCP AUTO takes none:
 * what TCP loses is what this end's own queue could not hold, which parity
 * would only fill more. Changes apply from the next picture. */
typedef uint32_t rcs_fec_mode;
enum { RCS_FEC_OFF = 0, RCS_FEC_AUTO = 1, RCS_FEC_FIXED = 2 };

/* Share: what gives way when the bitrate does not carry the stream: adaptive
 * bitrate holds pictures back while the encoder's output is over its target,
 * and every mode while the path is behind.
 *   FRAME_RATE  Fewer pictures a second; the size stays, text stays sharp:
 *               desktops, documents, code. The default.
 *   SIZE        Smaller pictures in steps (75, 50, 38 and 25 percent of each
 *               side), not below min_size_percent (25..100): a step down
 *               while pictures were held back more than 30 percent of the
 *               time over 2 s; a step back up, 10 s after the last step,
 *               once the bitrate rose 1.8 times since the step down, or
 *               once 10 s passed held back less than 5 percent while the
 *               encoder used less than 40 percent of the bitrate. Each
 *               step starts the stream again (a keyframe; RCS_EVENT_STREAM
 *               with RCS_STREAM_SIZE): video, games.
 *   BALANCED    As SIZE from half the time held back: the frame rate gives
 *               way first.
 * rcs_video_send_stats.size_percent and limited_ms show it. */
typedef uint32_t rcs_degradation;
enum { RCS_DEGRADE_FRAME_RATE = 1, RCS_DEGRADE_SIZE = 2, RCS_DEGRADE_BALANCED = 3 };

/* Share: ref_invalidation, a picture lost on the network that neither parity
 * nor asking again repaired. 1 (default): an encoder that can
 * (rcs_codec_implementation.ref_invalidation) drops the lost picture and
 * those after it from its references, and the next picture, of normal size,
 * refers to older ones the viewer has: the viewer skips the pictures in
 * between and goes on (RCS_EVENT_RECOVERY, method REFERENCES). Others, and
 * pictures lost too long ago for the encoder to still hold what came
 * before, restart from a keyframe, as with 0. A keyframe is several times a
 * normal picture's size: on a lossy path it is the more likely to be lost
 * again, and its burst pushes the bitrate down. */

/* Share: how adaptive bitrate decides (RCS_RATE_ADAPTIVE). About every 50 ms in
 * which video arrives the viewer reports what arrived and how late; the
 * sharer measures the queueing on the path (the one-way delay above its lowest
 * of the last 10 s), the loss over the last second and the arrival rate, and
 * applies these rules:
 *   overuse  queueing that lasted (the least of the reports over
 *            overuse_persist_ms, so that a congested queue counts and a
 *            wireless receiver's nap of 100 ms and more does not) above
 *            overuse_queue_ms and rising, queueing that lasted above
 *            severe_queue_ms, or loss above overuse_loss_permille: down to
 *            decrease_percent of what arrived (at most halving, at least 5%
 *            down), at most once per decrease_interval_ms; queueing while the
 *            sharer sends less than half the bitrate (a still screen) is not
 *            its own and does not count;
 *   clear    queueing in the latest report below clear_queue_ms and loss
 *            below clear_loss_permille:
 *            up by probe_increase_percent per second until the first overuse
 *            (on paths of 100 ms round trip and more; a shorter path shows
 *            the result sooner and so probes faster, by 100 ms / round trip,
 *            at most 8 times: a LAN goes from 4 to 20 Mbit/s in about half a
 *            second instead of 3.5 s), then by recovery_increase_percent up to 90% of the rate the
 *            latest overuse began at, and by increase_percent above it;
 *            probe again after 10 s above it without overuse. After an
 *            overuse, while the sharer sends less than half the bitrate: hold;
 *   neither  hold.
 * The defaults suit wired and Wi-Fi paths, loss up to the clear threshold
 * being what parity and resending repair. A path whose delay swings on its
 * own (cellular, satellite) wants higher queue thresholds or a longer
 * persistence; a host sharing the path with its own traffic may want the video
 * to yield sooner. Changes apply at once. Data messages yield to video while
 * the path is not clear by these same thresholds. A host may decide instead
 * (rcsdk/rate_control.h).
 *
 * How fast video leaves (UDP, P2P, RELAY and HOST; FIXED as well): while the
 * path is clear by the thresholds above, each picture within frame_spread_us
 * of being encoded, in batches of at most a millisecond's worth, never faster
 * than max_pacing_bps; a 1080p picture of 40 KB crosses a LAN in about 1 ms.
 * On a path the viewer measures to be slower (its pictures arrive spaced by
 * a bottleneck: Wi-Fi, a slow uplink; rcs_network_stats.path_bps), a picture's
 * first path_burst_us of the path's time leaves at once and the rest at
 * path_pacing_percent of the path's rate: a large picture (a keyframe, a
 * scrolled page) then waits at the sharer, where sound overtakes it, instead
 * of in the path's queue (a Wi-Fi access point's), where sound waits behind
 * it, and arrives no later, since the bottleneck spaces it anyway. A 1080p60
 * picture at 20 Mbit/s fits in 8 ms of a 2.4 GHz Wi-Fi and still leaves at
 * once; a gigabit LAN delivers too fast to measure and is not limited. A
 * shorter burst leaves sound less to wait behind, and gives an access point
 * smaller aggregates (less of its airtime for the video); a percentage closer
 * to 110 leaves less in the access point's queue, a higher one finds a path
 * that got faster sooner. path_pacing_percent 0: no such limit.
 * Otherwise, so that a queue on the path is not fed faster than it drains, at
 * 1.5 times the bitrate (FIXED: 4 times, at least 20 Mbit/s), also within
 * max_pacing_bps. These thresholds decide it even where the host decides the
 * bitrate (rcsdk/rate_control.h). A path whose bottleneck or policer drops
 * bursts the reports do not show as loss soon enough wants a longer spread or
 * a lower maximum; a maximum below 1.5 times the bitrate holds the video
 * itself back.
 *
 * start_bitrate_bps: where ADAPTIVE starts; 0: 4 Mbit/s (within the
 * limits). Changing it while running moves the bitrate there at once.
 *
 * path_change_percent: when a viewer's connection moves to a path of another
 * address (rcs_paths_config, RCS_EVENT_PATH), what was measured on the old
 * one says nothing of the new one: its bitrate starts again at this percent
 * of where it was (0..100, default 50; 0: at start_bitrate_bps) and probes up
 * as at the start. A path whose address kept its host and changed only its
 * port (a NAT that remapped) keeps the bitrate.
 *
 * input_boost_ms: for this long after each remote input event the OS
 * accepted, capture and encode at twice the frame rate, so the input's effect
 * reaches the viewer sooner (LAN: about 4 ms less at 60 and 120 FPS).
 * 0..1000; 0 off.
 *
 * keyframe_interval_ms: 0, keyframes at the start and when a viewer needs one
 * (after loss); 100..60 000, also at least this often, e.g. for paths that
 * lose whole pictures; each keyframe costs several frames' worth of bitrate.
 * keyframe_min_interval_ms: a viewer that joins, lost a picture or fell
 * behind needs a keyframe, which every viewer of its encoding gets
 * (RCS_EXT_ENCODINGS); while others watch that encoding, requests within this
 * interval of its last keyframe wait for the next one, so that viewers
 * joining together share one.
 *
 * Share, several viewers (rcs_viewers_config, RCS_EXT_ENCODINGS):
 * viewer_keyframe_interval_ms: a keyframe one viewer's loss or backlog
 * causes, at most this often while others are in the session (alone on its
 * encoding too: its keyframes take the uplink they share, and one its path
 * lost again is not got over by asking for more); its requests meanwhile wait
 * for the next keyframe there is (keyframes_waited). A single viewer gets
 * them as it asks.
 * loss_keyframes_move: a viewer that caused this many within a minute while
 * others watched its encoding moves to a lower encoding
 * (RCS_VIEWER_VIDEO_LOSS); 0 never.
 * move_down_ms: how long a viewer's path carries less than its encoding
 * sends before it moves down (one that dropped video a second behind moves
 * at once). move_up_ms: how long it carries what the encoding above sends,
 * with a quarter to spare, before it moves back up.
 * lower_idle_ms: AUTO stops a lower encoding no viewer used for this long.
 *
 * View, the decode queue: when it is full or its oldest packet is older than
 * max_queue_age_ms, the queued reference chain is dropped and a keyframe is
 * requested (RCS_EVENT_RECOVERY). Dependent pictures are never decoded
 * without their references. The age limit bounds latency at any frame rate;
 * the frame limit only caps memory: what a stalled path (a Wi-Fi radio's
 * doze, a scan) hands over at once decodes in a few milliseconds, where a
 * keyframe would take a round trip. Waiting this end caused itself does not
 * count: before a decoder is open, while it opens, and during a step of
 * decoding and handing over that took more than half the age limit (a phone's
 * codec opening on its first packet, the first picture's conversion). */
typedef struct rcs_video_tuning_config {
    rcs_extension header;
    /* Share: adaptive bitrate's rules. */
    uint32_t overuse_queue_ms;          /* 2..1000 */
    uint32_t severe_queue_ms;           /* overuse_queue_ms..5000 */
    uint32_t clear_queue_ms;            /* 1..overuse_queue_ms - 1 */
    uint32_t overuse_persist_ms;        /* 0..2000; 0: each report alone. */
    uint32_t overuse_loss_permille;     /* 2..1000 */
    uint32_t clear_loss_permille;       /* 1..overuse_loss_permille - 1 */
    uint32_t decrease_percent;          /* 10..95 */
    uint32_t decrease_interval_ms;      /* 50..10 000 */
    uint32_t probe_increase_percent;    /* Per second, 0..1000. */
    uint32_t increase_percent;          /* Per second, 0..1000. */
    uint32_t recovery_increase_percent; /* Per second, 0..1000. */
    uint32_t start_bitrate_bps;
    uint32_t path_change_percent; /* 0..100 */
    /* Share: what gives way when the bitrate does not carry the stream. */
    rcs_degradation degradation;
    uint32_t min_size_percent; /* 25..100 */
    /* Share: how video leaves. */
    uint32_t frame_spread_us;     /* 0..100 000; 0: each picture as fast as max_pacing_bps allows. */
    uint32_t max_pacing_bps;      /* 1 Mbit/s..4000 Mbit/s */
    uint32_t path_pacing_percent; /* 110..1000; 0: no limit. */
    uint32_t path_burst_us;       /* 0..100 000 */
    /* Share: repairing lost video. */
    rcs_fec_mode fec_mode;
    uint32_t fec_percent;      /* 1..100 */
    uint32_t ref_invalidation; /* 0 or 1 */
    /* Share: the encoder. */
    uint32_t input_boost_ms;
    rcs_encoder_speed speed;
    rcs_content_hint content;
    uint32_t keyframe_interval_ms;
    uint32_t keyframe_min_interval_ms; /* 100..60 000 */
    /* Share: several viewers. */
    uint32_t viewer_keyframe_interval_ms; /* 100..60 000 */
    uint32_t loss_keyframes_move;         /* 0..60 */
    uint32_t move_down_ms;                /* 200..30 000 */
    uint32_t move_up_ms;                  /* 1000..120 000 */
    uint32_t lower_idle_ms;               /* 1000..600 000 */
    /* Both: the software codec libraries. */
    uint32_t software_threads; /* 0..64 */
    /* View: the decode queue. */
    uint32_t queue_frames;     /* 1..256 */
    uint32_t max_queue_age_ms; /* 0 disables the age limit; at most 2000. */
} rcs_video_tuning_config;
#define RCS_VIDEO_TUNING_CONFIG_INIT                                                                                   \
    {{RCS_EXT_VIDEO_TUNING, sizeof(rcs_video_tuning_config), NULL},                                                    \
     30u,                                                                                                              \
     60u,                                                                                                              \
     10u,                                                                                                              \
     150u,                                                                                                             \
     100u,                                                                                                             \
     50u,                                                                                                              \
     85u,                                                                                                              \
     300u,                                                                                                             \
     50u,                                                                                                              \
     8u,                                                                                                               \
     25u,                                                                                                              \
     0u,                                                                                                               \
     50u,                                                                                                              \
     RCS_DEGRADE_FRAME_RATE,                                                                                           \
     50u,                                                                                                              \
     1000u,                                                                                                            \
     800000000u,                                                                                                       \
     110u,                                                                                                             \
     8000u,                                                                                                            \
     RCS_FEC_AUTO,                                                                                                     \
     50u,                                                                                                              \
     1u,                                                                                                               \
     0u,                                                                                                               \
     RCS_ENCODER_FASTEST,                                                                                              \
     RCS_CONTENT_AUTO,                                                                                                 \
     0u,                                                                                                               \
     500u,                                                                                                             \
     2000u,                                                                                                            \
     3u,                                                                                                               \
     2000u,                                                                                                            \
     10000u,                                                                                                           \
     10000u,                                                                                                           \
     0u,                                                                                                               \
     64u,                                                                                                              \
     80u}
RCS_INLINE rcs_video_tuning_config rcs_video_tuning_config_init(void) {
    rcs_video_tuning_config value = RCS_VIDEO_TUNING_CONFIG_INIT;
    return value;
}

/* ---- Sound -------------------------------------------------------------- */

/* Both roles: each part applies to the direction it tunes.
 *
 * Sending the machine's sound (source SYSTEM), capture: capture_backend, one of
 * rcs_capabilities.audio_capture_backends or AUTO; capture_period_frames,
 * sample frames per capture period, 0 for the backend's default. Only
 * CORE_AUDIO_TAP takes a period: 64..4800, default 240 (5 ms); smaller periods
 * cut delay and cost CPU. Opus packets follow it. Other backends refuse a
 * nonzero value with UNSUPPORTED.
 *
 * Sending any sound, redundancy: 1 (default) over UDP, P2P, RELAY and HOST,
 * each packet carries the one before once more where both fit in one
 * datagram (Opus and AAC at common bitrates; not PCM), so that a packet lost
 * alone costs nothing (rcs_audio_receive_stats.recovered_packets): the sound
 * takes twice its bitrate on the path. 0: each packet once. Never over TCP,
 * which loses only what this end's own queue could not hold.
 *
 * Receiving, playback (rcs_receive_audio_config.playback 1; a sharer's for
 * each viewer):
 *   max_delay_ms            The most sound the buffer holds, 20..1000 ms
 *                           (default 200: a wireless receiver's radio naps of
 *                           100 ms and more, as a buffer of jitter); beyond it
 *                           the oldest is dropped so the delay cannot grow.
 *                           The buffer holds what the path's jitter of the
 *                           last eight seconds needs, a few milliseconds on a
 *                           LAN. At least one device period and one packet are
 *                           held; a playback period (or Windows queue) longer
 *                           than max_delay_ms is INVALID_ARGUMENT.
 *   buffer_margin_us        What the buffer keeps above one device period plus
 *                           the network's measured jitter, 0..100 000 us
 *                           (default 2 000). Less is lower delay and more
 *                           underruns when the network or the CPU hiccups.
 *   playback_period_frames  Sample frames the output device takes per
 *                           period, 0 for the default: macOS 256 (5.3 ms),
 *                           32..4096 within the device's range; Windows the
 *                           engine's period (usually 10 ms), another value
 *                           only where the device offers shorter shared-mode
 *                           periods (a 48 kHz float mix), else playback
 *                           pauses and says so.
 *   playback_queue_frames   Windows: sample frames kept queued in the output
 *                           device, 0 for one and a half periods; at least one
 *                           period. macOS: must be 0. */
typedef struct rcs_audio_tuning_config {
    rcs_extension header;
    rcs_audio_capture_backend capture_backend;
    uint32_t capture_period_frames;
    uint32_t redundancy;
    uint32_t max_delay_ms;
    uint32_t buffer_margin_us;
    uint32_t playback_period_frames;
    uint32_t playback_queue_frames;
} rcs_audio_tuning_config;
#define RCS_AUDIO_TUNING_CONFIG_INIT                                                                                   \
    {{RCS_EXT_AUDIO_TUNING, sizeof(rcs_audio_tuning_config), NULL}, RCS_AUDIO_CAPTURE_AUTO, 0u, 1u, 200u, 2000u, 0u, 0u}
RCS_INLINE rcs_audio_tuning_config rcs_audio_tuning_config_init(void) {
    rcs_audio_tuning_config value = RCS_AUDIO_TUNING_CONFIG_INIT;
    return value;
}

/* ---- The network -------------------------------------------------------- */

/* rcs_network_tuning_config.ice_addresses: kinds of address ICE offers, and
 * takes from the peer, beyond those of the interfaces (bits). */
typedef uint32_t rcs_ice_addresses;
enum { RCS_ICE_ADDRESSES_LOOPBACK = 1 };

/* Both ends unless a field says otherwise.
 *   local_interface        Use only this network interface, e.g. Wi-Fi
 *                          beside Ethernet, or not a VPN: P2P and RELAY on
 *                          both ends, UDP and TCP on the viewer (a sharer
 *                          listens on rcs_connect.address, which may be the
 *                          interface's). macOS: its BSD name ("en0");
 *                          Windows: the adapter's name ("WLAN") or index;
 *                          Android: its name ("wlan0"), the sockets marked for
 *                          its network (android_setsocknetwork) with the Java
 *                          VM, else bound with SO_BINDTODEVICE (Linux 5.7 or
 *                          later kernels let applications; whether Android
 *                          routes them so is not known). While it is not up
 *                          with a usable address, a running connection's path
 *                          is lost (RCS_PATH_LOST) and waits for it
 *                          (rcs_paths_config). A change moves a running
 *                          connection at once.
 *                          Empty: every interface.
 *   wifi_low_latency       1 (default): while this end is connected, the
 *                          system's Wi-Fi without its power saving, which holds
 *                          what arrives for the device until its radio next
 *                          wakes (100 ms and more, every few seconds), so that
 *                          pictures and sound arrive in bursts and adaptive
 *                          bitrate takes the delay for congestion. Android: a
 *                          low-latency Wi-Fi lock (the host declares
 *                          android.permission.WAKE_LOCK and hands over its
 *                          Java VM; Android applies it while the application
 *                          is in front with the screen on); Windows: WLAN
 *                          media streaming mode, background scans off. Apple's
 *                          systems have no such setting
 *                          (rcs_capabilities.wifi_low_latency). Whether it
 *                          holds: rcs_network_stats.wifi_low_latency; why not:
 *                          the log. 0: Wi-Fi as the system runs it.
 *   audio_dscp             UDP, P2P and RELAY over UDP: the DSCP (1..63) that
 *                          sound datagrams carry in their IP header, so that
 *                          an access point puts them in a Wi-Fi queue ahead
 *                          of the video (WMM; 46, EF: voice or video
 *                          priority; video stays best effort), as routers
 *                          that honour it do. 0 (default): unmarked. A
 *                          trade, not a gain everywhere: on a 2.4 GHz home
 *                          mesh under 20-40 Mbit/s of video the playback
 *                          buffer ran dry half as often with EF, but 2-5% of
 *                          the sound was lost beyond what redundancy repairs
 *                          (the access point drops what it would otherwise
 *                          hold), against 0-1% unmarked (PERFORMANCE.md).
 *                          Networks with managed QoS (enterprise, a policy
 *                          that expects EF for voice) want it; some police
 *                          EF to a small rate. Routers may rewrite or strip
 *                          marks; the home router tested passed EF and
 *                          cleared AF41 and CS1. Every system marks without
 *                          privileges (Windows too). Sound actually marked:
 *                          rcs_network_stats.marked_packets. Connections made
 *                          after a change use it.
 *   send_queue_bytes       Data channels (rcs_session_send_data): 1 MiB..1 GiB
 *                          may wait to be sent; rcs_session_send_data refuses
 *                          what does not fit (QUEUE_FULL).
 *   in_flight_bytes        4 KiB..16 MiB of data messages sent and not yet
 *                          acknowledged at a time. More carries more on long
 *                          paths; input sent behind it waits longer.
 *   reconnect_window_ms    View, rcs_reconnect_config.reconnect: 1 000..3 600 000
 *                          of trying; 0 keeps trying until stopped.
 *   reconnect_interval_ms  View: 100..60 000 between attempts.
 *   connect_timeout_ms     How long a viewer's handshake may take before the
 *                          attempt fails (a sharer's handshakes too), how long
 *                          a TCP connection to the sharer may take to connect
 *                          (each one a TCP viewer makes), and a TCP or TLS
 *                          connection to the TURN server: 1 000..60 000.
 *   idle_timeout_ms        A connection that hears nothing from the other end
 *                          for this long has lost its path (RCS_PATH_LOST; it
 *                          is kept as rcs_paths_config says, or ends with
 *                          RCS_ERROR_NETWORK): 1 000..60 000.
 *   path_stall_ms          A path that brings nothing for this long (pings go
 *                          every 250 ms) is looked past (RCS_PATH_CHANGING):
 *                          the video, sound and data stop, a viewer's held keys
 *                          and buttons are released, a standby path takes over:
 *                          200..10 000, default 1 000, longer than the naps of a
 *                          Wi-Fi radio saving power (100 ms and more).
 *   path_hold_ms           Share: how long a viewer whose path is lost is kept
 *                          (its id, admission, modules and messages in flight)
 *                          for it to come back over another path: 1 000..3 600 000,
 *                          default 30 000. A viewer keeps its own connection for
 *                          up to reconnect_window_ms.
 *   standby_probe_ms       How often a standby path (rcs_paths_config.standby)
 *                          is probed, which keeps its NAT mapping open and
 *                          measures it: 1 000..60 000, default 15 000 (cellular
 *                          carriers' NATs often drop a UDP mapping after 30 s).
 *   ice_gather_ms          P2P and RELAY: how long candidates are gathered
 *                          (STUN, TURN) before the signal goes out: 200..10 000.
 *   ice_connect_ms         P2P and RELAY: how long the checks may take to find
 *                          a path before the attempt fails (RCS_ERROR_NETWORK):
 *                          1 000..120 000. A host that falls back to RELAY on
 *                          its own (the SDK never does) learns sooner with a
 *                          shorter one.
 *   ice_addresses          P2P: kinds of address offered as candidates beyond
 *                          the interfaces' (rcs_ice_addresses bits; 0 by
 *                          default): RCS_ICE_ADDRESSES_LOOPBACK, this machine's
 *                          127.0.0.1 and ::1, so that two ends on one machine
 *                          connect without a network (tests). Whatever the
 *                          peer's signal names is probed, so each end leaves
 *                          out (and logs; a failure lists them) candidates no
 *                          peer is at: unspecified, multicast, broadcast and
 *                          reserved addresses, cloud metadata services
 *                          (169.254.169.254, 100.100.100.200, 168.63.129.16,
 *                          fd00:ec2::254); loopback and link-local ones unless
 *                          it has a candidate of that scope itself (this
 *                          option, or a STUN or TURN server there); more than
 *                          four of one IP address.
 *   turn_allocate_ms       RELAY: how long the TURN server may take to grant
 *                          the allocation (the request is resent every
 *                          500 ms meanwhile): 1 000..30 000. */
typedef struct rcs_network_tuning_config {
    rcs_extension header;
    char local_interface[64];
    uint32_t wifi_low_latency;
    uint32_t audio_dscp;
    uint32_t send_queue_bytes;
    uint32_t in_flight_bytes;
    uint32_t reconnect_window_ms;
    uint32_t reconnect_interval_ms;
    uint32_t connect_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t ice_gather_ms;
    uint32_t ice_connect_ms;
    rcs_ice_addresses ice_addresses;
    uint32_t turn_allocate_ms;
    uint32_t path_stall_ms;
    uint32_t path_hold_ms;
    uint32_t standby_probe_ms;
} rcs_network_tuning_config;
#define RCS_NETWORK_TUNING_CONFIG_INIT                                                                                 \
    {{RCS_EXT_NETWORK_TUNING, sizeof(rcs_network_tuning_config), NULL},                                                \
     {0},                                                                                                              \
     1u,                                                                                                               \
     0u,                                                                                                               \
     67108864u,                                                                                                        \
     65536u,                                                                                                           \
     30000u,                                                                                                           \
     1000u,                                                                                                            \
     5000u,                                                                                                            \
     5000u,                                                                                                            \
     1500u,                                                                                                            \
     15000u,                                                                                                           \
     0u,                                                                                                               \
     5000u,                                                                                                            \
     1000u,                                                                                                            \
     30000u,                                                                                                           \
     15000u}
RCS_INLINE rcs_network_tuning_config rcs_network_tuning_config_init(void) {
    rcs_network_tuning_config value = RCS_NETWORK_TUNING_CONFIG_INIT;
    return value;
}

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_TUNING_H */
