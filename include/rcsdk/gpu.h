/* Remote Control SDK: GPU frames.
 *
 * For hosts that render the decoded pictures where they are, without a copy
 * (on_gpu_frame): what a frame holds, keeping it, waiting for it, and the
 * host's own D3D11 device. */
#ifndef RCSDK_GPU_H
#define RCSDK_GPU_H

#include <rcsdk/rcsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native GPU image. The callback borrows the handle; rcs_gpu_frame_retain it to
 * keep it, then release. Retained frames stay valid after the session stops.
 *
 * D3D11: resource is an ID3D11Texture2D*, device its ID3D11Device* (multithread
 * protected). Use that device or explicit shared-resource interop.
 * macOS and iOS: resource is a CVPixelBufferRef (IOSurface backed, Metal
 * compatible). Android: resource is an AHardwareBuffer* the decoder wrote
 * in its own layout (which may be tiled or compressed), format standing for
 * the picture it holds (NV12: 4:2:0 8-bit, P010: 4:2:0 10-bit, NV24 and
 * P410: 4:4:4), for an EGLImage (eglGetNativeClientBufferANDROID) sampled as
 * GL_TEXTURE_EXTERNAL_OES, which converts it to RGB as the buffer says, or a
 * Vulkan import. The SDK holds the decoder's
 * image while the frame is retained, and the host may hold 8 at once: release
 * frames once shown (new pictures wait while it holds 8).
 * The logical image may be smaller than the GPU surface. format is its layout
 * and color what its values mean: a renderer converts YCbCr with color.matrix
 * and color.range, and shows PQ (color.transfer) values as HDR. */
typedef uint32_t rcs_gpu_resource;
enum {
    RCS_GPU_D3D11_TEXTURE2D = 1,
    RCS_GPU_CVPIXELBUFFER = 2,
    RCS_GPU_AHARDWAREBUFFER = 3,
    /* A browser: resource is the number of a WebCodecs VideoFrame on the
     * SDK's media thread, a worker of the page's (web/rcsdk.mjs draws it
     * there, or hands the VideoFrame itself to its page); format is 0, the
     * browser's own layout, which its drawImage, texImage2D and
     * importExternalTexture convert. */
    RCS_GPU_WEB_VIDEO_FRAME = 4
};
typedef struct rcs_gpu_frame_info {
    uint32_t struct_size;
    uint32_t stream; /* rcs_streams_config: which; 0 without it. */
    rcs_gpu_resource resource_type;
    rcs_pixel_format format;
    uint32_t width, height;
    uint32_t surface_width, surface_height;
    rcs_color color;
    uint64_t sequence;
    uint64_t timestamp_ns;
    void* resource;
    void* device;
} rcs_gpu_frame_info;
#define RCS_GPU_FRAME_INFO_INIT {sizeof(rcs_gpu_frame_info), 0, 0, 0, 0, 0, 0, 0, {0, 0, 0, 0}, 0, 0, NULL, NULL}
RCS_INLINE rcs_gpu_frame_info rcs_gpu_frame_info_init(void) {
    rcs_gpu_frame_info value = RCS_GPU_FRAME_INFO_INIT;
    return value;
}

RCSDK_API rcs_result rcs_gpu_frame_retain(rcs_gpu_frame frame);
RCSDK_API void rcs_gpu_frame_release(rcs_gpu_frame frame);
RCSDK_API rcs_result rcs_gpu_frame_get_info(rcs_gpu_frame frame, rcs_gpu_frame_info* info);
/* Block the CPU until the GPU work producing this frame finished (0 polls,
 * UINT32_MAX: no limit); RCS_ERROR_TIMEOUT otherwise. Renderers on the same
 * device/context do not need this. */
RCSDK_API rcs_result rcs_gpu_frame_wait(rcs_gpu_frame frame, uint32_t timeout_ms);

/* View, Windows: decode on the host's ID3D11Device, so GPU frames live there
 * (e.g. the one Qt Quick renders with) and need no copy. It must expose the D3D11 video interfaces (create it with
 * D3D11_CREATE_DEVICE_VIDEO_SUPPORT) and have multithread protection on;
 * start() and rcs_session_update fail otherwise (INVALID_ARGUMENT), and with
 * UNSUPPORTED when its GPU decodes none of codecs[] in hardware. Hardware
 * decoding, and the textures software decoding uploads (HARDWARE_FIRST,
 * SOFTWARE). The SDK holds a reference while running. Elsewhere it is
 * UNSUPPORTED (start() says so): macOS frames are IOSurfaces any Metal
 * device can use. */
typedef struct rcs_gpu_device_config {
    rcs_extension header;
    void* d3d11_device; /* ID3D11Device*, or NULL. */
} rcs_gpu_device_config;
#define RCS_GPU_DEVICE_CONFIG_INIT {{RCS_EXT_GPU_DEVICE, sizeof(rcs_gpu_device_config), NULL}, NULL}
RCS_INLINE rcs_gpu_device_config rcs_gpu_device_config_init(void) {
    rcs_gpu_device_config value = RCS_GPU_DEVICE_CONFIG_INIT;
    return value;
}

/* Share with capture source HOST, running: the next picture as a GPU image
 * of the host's own, encoded without a copy to the CPU (e.g. what ReplayKit
 * or the host's renderer made). resource_type is one this system takes
 * (rcs_capabilities.pushed_gpu_frames; none: UNSUPPORTED): macOS and iOS
 * take a CVPixelBufferRef (IOSurface-backed for the hardware encoder) in
 * BGRA8, NV12 (420v, 420f), P010, NV24 or P410, `format` saying which;
 * Android an AHardwareBuffer* the GPU can sample (usage
 * AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, as the HardwareBuffer of an
 * ImageReader that MediaProjection draws into has), BGRA8 standing for its
 * 8-bit RGB formats (R8G8B8A8, R8G8B8X8) and NV12 for YCbCr 4:2:0
 * (Y8Cb8Cr8_420); its
 * size width x height is the stream's (rcs_stream_info; another is
 * INVALID_ARGUMENT, as for rcs_session_push_video_frame, and so is
 * INVALID_STATE while no stream runs), and `color` what its values mean (a
 * YCbCr picture's matrix and range; 0s for BGRA8).
 * timestamp_ns: when it was made on the local monotonic clock (0: now). The
 * SDK retains the image until it is encoded; the newest pending picture
 * replaces an older one not yet encoded. stream, sequence, surface_* and
 * device are not read. */
RCSDK_API rcs_result rcs_session_push_gpu_frame(rcs_session session, const rcs_gpu_frame_info* frame);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_GPU_H */
