/* Remote Control SDK: what Android hands over.
 *
 * The SDK is native code; what Android offers only to Java it reaches through
 * the host's Java VM: the microphone permission and the list of sound
 * devices, and for the official modules the clipboard (ClipboardManager) and
 * keeping the device awake (PowerManager). The host hands over its process's
 * JavaVM and its application's Context once, before using those (e.g. from
 * JNI_OnLoad; in Qt QJniEnvironment::javaVM() and
 * QNativeInterface::QAndroidApplication::context()). Until then they are
 * UNSUPPORTED with that reason; the rest of the SDK needs no Java.
 *
 * Pictures arrive as hardware buffers: an ImageReader's Image that
 * MediaProjection (the whole screen or one app), a camera or the host's own
 * renderer drew (Image.getHardwareBuffer(), then
 * AHardwareBuffer_fromHardwareBuffer), pushed as they come. Sound is pushed
 * with rcs_session_push_audio: AudioPlaybackCapture's AudioRecord reads
 * 48 kHz stereo float as it takes it.
 *
 * Other systems have none of these: UNSUPPORTED there. */
#ifndef RCSDK_ANDROID_H
#define RCSDK_ANDROID_H

#include <rcsdk/rcsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* java_vm: this process's JavaVM*; context: a jobject of any Context of the
 * application, whose application context the SDK keeps (a global reference).
 * Any thread. Again: replaces what it holds. INVALID_ARGUMENT for NULLs or a
 * context that is no Context. */
RCSDK_API rcs_result rcs_android_initialize(void* java_vm, void* context);

/* What the host handed over, for modules that use Java: the JavaVM* and the
 * application's Context, a global reference the SDK holds for the rest of the
 * process. INVALID_STATE before rcs_android_initialize. */
RCSDK_API rcs_result rcs_android_get_java(void** java_vm, void** context);

typedef struct rcs_hardware_buffer_info {
    uint32_t struct_size;
    /* The degrees the picture is turned clockwise to stand upright: 0, 90,
     * 180 or 270 (a camera's sensor orientation; MediaProjection's pictures
     * stand). */
    uint32_t rotation;
} rcs_hardware_buffer_info;
#define RCS_HARDWARE_BUFFER_INFO_INIT {sizeof(rcs_hardware_buffer_info), 0u}
RCS_INLINE rcs_hardware_buffer_info rcs_hardware_buffer_info_init(void) {
    rcs_hardware_buffer_info value = RCS_HARDWARE_BUFFER_INFO_INIT;
    return value;
}

/* Share with capture source HOST, running: the next picture, an
 * AHardwareBuffer* of any size that the GPU samples (usage
 * AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE; RGB or YCbCr, which the GPU
 * converts as the buffer's data space says). The SDK turns it upright and
 * fits it into the stream's size, its shape kept and the rest black, on the
 * GPU into a buffer of its own before it returns: the host hands its buffer
 * back to its producer (Image.close()) right after. rcs_stream_info says
 * where it lies (content_*) and how large it is (display_width,
 * display_height: what a viewer asking for the sharer's native size gets);
 * RCS_EVENT_STREAM reports when that changes, e.g. the device turned. The
 * newest picture not yet encoded is replaced, as with
 * rcs_session_push_gpu_frame. INVALID_STATE while no stream runs or with
 * another source; INVALID_ARGUMENT for a buffer the GPU cannot sample or
 * another rotation; DEVICE when the GPU cannot draw it; a viewing session
 * UNSUPPORTED. */
RCSDK_API rcs_result rcs_session_push_hardware_buffer(rcs_session session, const void* hardware_buffer,
                                                      const rcs_hardware_buffer_info* info);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_ANDROID_H */
