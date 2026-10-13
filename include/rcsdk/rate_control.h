/* Remote Control SDK: the host's own bitrate decisions.
 *
 * The SDK measures the path; RCS_EXT_RATE_DECIDER hands each viewer's report
 * and the rules' proposal to the host, which may decide otherwise. The rules
 * themselves are tuning (rcsdk/tuning.h). */
#ifndef RCSDK_RATE_CONTROL_H
#define RCSDK_RATE_CONTROL_H

#include <rcsdk/rcsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One report of the viewer, as adaptive bitrate saw it (on_rate_report):
 * what it says, what the sharer made of the reports so far, and this end's
 * own state. Borrowed for the duration of the callback. */
typedef struct rcs_rate_report {
    uint32_t struct_size;
    uint64_t timestamp_ns;   /* Local monotonic clock, as in rcs_event. */
    rcs_viewer_id viewer_id; /* The viewer whose connection it measures. */
    /* The report: one window in which video arrived. The counts are of every
     * datagram of the connection (video, sound, messages, resent pieces). */
    uint32_t window_us;
    uint32_t expected_packets;
    uint32_t received_packets;
    uint64_t received_bytes;
    /* The window's lowest of its video (a picture's first datagram, which no
     * datagram of the same picture queues ahead of); arbitrary origin, only
     * changes count. */
    int64_t one_way_delay_us;
    /* The path, from the reports so far. */
    uint32_t queue_delay_us; /* one_way_delay_us above its lowest of the last 10 s. */
    uint32_t loss_permille;  /* Smoothed over the recent reports. */
    uint64_t received_bps;   /* Smoothed arrival rate, every datagram counted. */
    uint64_t round_trip_ns;
    /* Paths of another address the connection moved to so far (RCS_EVENT_PATH):
     * with each, the values above start over from the new path's reports (a
     * report begun on the old path is not given), and one_way_delay_us may jump. */
    uint32_t path_changes;
    /* This end. */
    uint64_t encoder_bps;     /* What the encoder produced lately. */
    uint64_t send_backlog_ns; /* Video queued here and not sent yet, as time at the current pacing. */
    uint32_t current_bps;     /* The target until now. */
    uint32_t minimum_bps;     /* The limits every decision is kept within. */
    uint32_t maximum_bps;
} rcs_rate_report;

/* What on_rate_report decides. */
typedef struct rcs_rate_decision {
    uint32_t struct_size;
    /* In: what the rules (rcs_video_tuning_config) propose. Out: the target
     * from now on, kept within the limits. */
    uint32_t bitrate_bps;
} rcs_rate_decision;

/* Share, adaptive bitrate: the host decides the bitrate. on_rate_report gets
 * each report of a viewer (about every 50 ms while video flows), with what
 * the SDK measured and, in
 * decision->bitrate_bps, the bitrate its rules propose for that viewer's
 * connection; change that to decide otherwise (a ceiling while the host's
 * own traffic needs the path, an algorithm of its own), or leave it to
 * only watch. On the connection's network thread, one report at a time
 * (also of several viewers): return promptly. Changes of the target of 5%
 * or more are reported as RCS_EVENT_RATE, as the rules' own. Not called
 * with a fixed bitrate. */
typedef struct rcs_rate_decider_config {
    rcs_extension header;
    void* user_data;
    void (*on_rate_report)(const rcs_rate_report* report, rcs_rate_decision* decision, void* user_data);
} rcs_rate_decider_config;
#define RCS_RATE_DECIDER_CONFIG_INIT {{RCS_EXT_RATE_DECIDER, sizeof(rcs_rate_decider_config), NULL}, NULL, NULL}
RCS_INLINE rcs_rate_decider_config rcs_rate_decider_config_init(void) {
    rcs_rate_decider_config value = RCS_RATE_DECIDER_CONFIG_INIT;
    return value;
}

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_RATE_CONTROL_H */
