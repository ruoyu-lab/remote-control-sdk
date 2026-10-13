// Drawing on the GPU with OpenGL ES, on one thread of the SDK's that holds
// the process's EGL context: the host's hardware buffers copied (and fitted
// into the stream) when pushed, so the host may hand its buffer back to its
// producer at once, the encoders' input pictures, and decoded pictures read
// as BGRA8. Every call blocks until the thread did its work (reading and
// drawing into buffers wait for the GPU; a window's picture is queued for its
// consumer); failures throw Failure(DEVICE).
#pragma once

#include "media/video.hpp"

#include <android/hardware_buffer.h>
#include <android/native_window.h>

#include <functional>
#include <memory>

namespace rcsdk::platform::android::gl {

// Whether the GPU samples `format` (an AHardwareBuffer format) as a picture,
// and as which layout: BGRA8 for 8-bit RGB, NV12 for YCbCr 4:2:0; 0 for none.
rcs_pixel_format layoutOf(uint32_t format) noexcept;

// `source` (a buffer the GPU samples) turned `rotation` degrees clockwise
// (0, 90, 180, 270) and drawn into `content` of a new width x height RGBA8
// buffer of the SDK's, the rest black. The buffer is retained (an
// AHardwareBuffer*) and goes back to a pool when the last reference goes.
std::shared_ptr<void> draw(AHardwareBuffer* source, uint32_t rotation, const Rect& content, uint32_t width,
                           uint32_t height);

// The top-left width x height of `source` (a buffer the GPU samples, such
// as a decoder's own layout the CPU cannot read) drawn as BGRA8 into a
// buffer the CPU maps, whose rows (`strideBytes` apart) `use` gets on the
// calling thread while it stays mapped. A YCbCr picture is converted as
// `yuv` (its stream's colour) says where the GPU samples YCbCr unconverted
// (GL_EXT_YUV_target), else as the buffer's data space says; null for RGB.
// One reader at a time.
void read(AHardwareBuffer* source, uint32_t width, uint32_t height, const rcs_color* yuv,
          const std::function<void(const uint8_t* bgra, uint32_t strideBytes)>& use);

// An encoder's input surface.
class Window {
public:
    // Takes a reference to `window`.
    explicit Window(ANativeWindow* window);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Fills the window with `buffer` (one the GPU samples) or with BGRA8
    // pixels, stamps the picture `presentNs` and hands it to the consumer.
    void draw(AHardwareBuffer* buffer, uint64_t presentNs);
    void draw(const uint8_t* pixels, uint32_t stride, uint32_t width, uint32_t height, uint64_t presentNs);

private:
    ANativeWindow* window_;
    void* surface_ = nullptr; // EGLSurface.
    unsigned texture_ = 0;    // For BGRA8 pixels.
};

} // namespace rcsdk::platform::android::gl
