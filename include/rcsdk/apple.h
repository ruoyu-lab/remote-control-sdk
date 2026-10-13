/* Remote Control SDK: what Core Media hands over (macOS, iOS).
 *
 * A host whose pictures and sound arrive as CMSampleBuffers, from ReplayKit
 * (in-app capture, or a broadcast upload extension sharing the whole iPhone
 * or iPad screen), ScreenCaptureKit or AVFoundation, pushes them as they come:
 * the SDK turns the picture upright and fits it into the stream on the GPU,
 * and converts the sound to what it sends, the applications' sound and the
 * microphone mixed into one. Other systems have no sample buffers: the
 * function is UNSUPPORTED there. */
#ifndef RCSDK_APPLE_H
#define RCSDK_APPLE_H

#include <rcsdk/rcsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a sample buffer holds, as ReplayKit names it (RPSampleBufferType, of
 * the same values):
 *   VIDEO       A picture (its image buffer, a CVPixelBuffer).
 *   AUDIO       Linear PCM sound of what the device or the applications play
 *               (ReplayKit's AudioApp; ScreenCaptureKit's audio).
 *   MICROPHONE  Linear PCM sound of a microphone (ReplayKit's AudioMic;
 *               ScreenCaptureKit's microphone, AVFoundation's).
 * Sound at any rate, sample format and byte order, one or two channels. */
typedef uint32_t rcs_sample_buffer_type;
enum { RCS_SAMPLE_BUFFER_VIDEO = 1, RCS_SAMPLE_BUFFER_AUDIO = 2, RCS_SAMPLE_BUFFER_MICROPHONE = 3 };

typedef struct rcs_sample_buffer_info {
    uint32_t struct_size;
    rcs_sample_buffer_type type;
    /* VIDEO: how the picture lies, as a CGImagePropertyOrientation: 1 upright,
     * 3 upside down, 6 to be turned a quarter clockwise, 8 counterclockwise;
     * 0 reads it from the buffer's ReplayKit attachment
     * (RPVideoSampleOrientationKey), upright without one. Mirrored ones (2, 4,
     * 5, 7) are UNSUPPORTED. */
    uint32_t orientation;
} rcs_sample_buffer_info;
#define RCS_SAMPLE_BUFFER_INFO_INIT {sizeof(rcs_sample_buffer_info), RCS_SAMPLE_BUFFER_VIDEO, 0u}
RCS_INLINE rcs_sample_buffer_info rcs_sample_buffer_info_init(void) {
    rcs_sample_buffer_info value = RCS_SAMPLE_BUFFER_INFO_INIT;
    return value;
}

/* Share: the next picture or piece of sound, a CMSampleBufferRef that stays
 * the host's (the SDK keeps what it needs).
 *   VIDEO  Capture source HOST: the picture (BGRA8 or NV12, as ReplayKit
 *          gives it) turned upright and fitted into the stream's size, its
 *          shape kept and the rest black, on the GPU, without a copy to the
 *          CPU. rcs_stream_info says where it lies (content_*) and how large
 *          it is (display_width, display_height: what a viewer asking for the
 *          sharer's native size gets); RCS_EVENT_STREAM reports when that
 *          changes, e.g. the device turned. The newest picture not yet
 *          encoded is replaced, as with rcs_session_push_gpu_frame.
 *   AUDIO, MICROPHONE  Send audio source HOST: each converted to 48 kHz
 *          stereo on its own (mono on both channels) and both mixed into the
 *          one sound sent as the host's, at the volumes of
 *          rcs_send_audio_config (sample_buffer_audio_percent,
 *          sample_buffer_microphone_percent; at 0 one is left out, its
 *          buffers taken and dropped). Mixed as they arrive, each on its own
 *          clock (their timestamps are not compared): while both come, the
 *          one ahead waits for the other, by about one of its buffers, so
 *          neither breaks up. One that stops (a muted microphone; ReplayKit
 *          may send nothing while the applications are quiet) holds the other
 *          up no longer than its longest recent pause between buffers plus
 *          20 ms (60 ms before it paused at all), and is mixed again when it
 *          comes back. One ahead of the other for good (their clocks differ,
 *          a buffer went missing) is set back by what it was ahead over the
 *          last second, from 10 ms on, cross-faded: the delay does not grow.
 *          Only one pushed goes out as it comes.
 * INVALID_STATE while no stream runs (VIDEO) or no sound is sent (AUDIO,
 * MICROPHONE), or with another source; INVALID_ARGUMENT for a buffer without
 * a picture or linear PCM; a viewing session UNSUPPORTED. */
RCSDK_API rcs_result rcs_session_push_sample_buffer(rcs_session session, const void* sample_buffer,
                                                    const rcs_sample_buffer_info* info);

#ifdef __cplusplus
}
#endif

#endif
