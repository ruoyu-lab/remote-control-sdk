// One thread holds an EGL context (OpenGL ES 3 where the GPU has it, else 2;
// no surface of its own) and does all the SDK's drawing: a hardware buffer
// becomes an external texture through an EGLImage, which samples any layout
// the GPU takes (RGB, YCbCr converted as the buffer's data space says; a
// decoded picture read back is converted as its stream says, where the GPU
// samples YCbCr as it is); CPU pictures become a texture of their own.
// Drawing into a buffer of the SDK's goes through a framebuffer on another
// EGLImage; into an encoder, through an EGL window surface on its input
// surface, whose config is recordable.
#include "platform/android/gl.hpp"

#include "platform/platform.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rcsdk::platform::android::gl {
namespace {

constexpr char vertexShader[] = R"(
attribute vec2 position;
attribute vec2 coordinate;
varying vec2 sample;
void main() {
    sample = coordinate;
    gl_Position = vec4(position, 0.0, 1.0);
}
)";
constexpr char externalShader[] = R"(
#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES picture;
varying vec2 sample;
void main() {
    gl_FragColor = texture2D(picture, sample);
}
)";
// Read back as RGBA, an external texture's red and blue swapped make BGRA8.
constexpr char readShader[] = R"(
#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES picture;
varying vec2 sample;
void main() {
    gl_FragColor = vec4(texture2D(picture, sample).bgr, 1.0);
}
)";
// A YCbCr picture read back as BGRA8, converted as its stream says
// (`conversion`, `offset`) rather than as the buffer's data space says: a
// data space cannot say BT.709 primaries with BT.601's matrix (a screen's
// pictures, JPEG's), which a decoder then tags BT.709 (Qualcomm's AV1
// decoder). Needs GL_EXT_YUV_target, which samples Y, Cb, Cr unconverted.
constexpr char yuvVertexShader[] = R"(#version 300 es
in vec2 position;
in vec2 coordinate;
out vec2 point;
void main() {
    point = coordinate;
    gl_Position = vec4(position, 0.0, 1.0);
}
)";
constexpr char yuvReadShader[] = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
#extension GL_EXT_YUV_target : require
precision highp float;
uniform __samplerExternal2DY2YEXT picture;
uniform mat3 conversion;
uniform vec3 offset;
in vec2 point;
out vec4 color;
void main() {
    vec3 rgb = clamp(conversion * (texture(picture, point).rgb - offset), 0.0, 1.0);
    color = vec4(rgb.bgr, 1.0);
}
)";
// BGRA8 bytes uploaded as RGBA: the channels swapped back.
constexpr char pixelsShader[] = R"(
precision mediump float;
uniform sampler2D picture;
varying vec2 sample;
void main() {
    gl_FragColor = texture2D(picture, sample).bgra;
}
)";

std::string eglError(const char* what) {
    return std::string(what) + " failed (EGL error 0x" + [] {
        char text[16];
        std::snprintf(text, sizeof(text), "%x", unsigned(eglGetError()));
        return std::string(text);
    }() + ")";
}

class Worker {
public:
    static Worker& instance() {
        static auto* worker = new Worker; // Lives as long as the process, as its thread does.
        return *worker;
    }

    // Runs `work` on the thread with the context current, and waits for it.
    void run(const std::function<void()>& work) {
        std::exception_ptr failure;
        bool done = false;
        std::unique_lock lock(mutex_);
        queue_.push_back([&] {
            try {
                if (!error_.empty()) {
                    fail(RCS_ERROR_DEVICE, error_);
                }
                work();
            } catch (...) {
                failure = std::current_exception();
            }
            std::lock_guard finished(mutex_);
            done = true;
            changed_.notify_all();
        });
        changed_.notify_all();
        changed_.wait(lock, [&] { return done; });
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    EGLDisplay display() const noexcept { return display_; }
    EGLConfig config() const noexcept { return config_; }
    EGLContext context() const noexcept { return context_; }

    // No surface current: drawing into buffers of the SDK's.
    void offscreen() {
        if (!eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_)) {
            fail(RCS_ERROR_DEVICE, eglError("Making the EGL context current"));
        }
    }

    // A buffer as an external texture (bound), until the image goes.
    struct Image {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        PFNEGLDESTROYIMAGEKHRPROC destroy = nullptr;
        ~Image() {
            if (image != EGL_NO_IMAGE_KHR) {
                destroy(display, image);
            }
        }
    };
    void bind(AHardwareBuffer* buffer, GLenum target, GLuint texture, Image& image) {
        EGLClientBuffer client = getClientBuffer_(buffer);
        const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        image.display = display_;
        image.destroy = destroyImage_;
        image.image = client ? createImage_(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attributes)
                             : EGL_NO_IMAGE_KHR;
        if (image.image == EGL_NO_IMAGE_KHR) {
            fail(RCS_ERROR_DEVICE, eglError("Making an EGLImage of the hardware buffer"));
        }
        glBindTexture(target, texture);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        targetTexture_(target, static_cast<GLeglImageOES>(image.image));
        if (glGetError() != GL_NO_ERROR) {
            fail(RCS_ERROR_DEVICE, "The GPU cannot sample the hardware buffer");
        }
    }

    // How the bound texture is sampled: a hardware buffer's (external), one
    // read back as BGRA8 (converted by the GPU, or by its stream's colour:
    // readYuv), CPU pixels'.
    enum class Shader { external, read, readYuv, pixels };

    // Whether YCbCr pictures can be read back as their stream says (readYuv).
    bool readsYuv() const noexcept { return readYuv_.id != 0; }

    // readYuv's conversion from `color`'s matrix and range (8-bit code
    // values; 10-bit samples come within a fraction of a level).
    void convertAs(const rcs_color& color) {
        const rcs_color_matrix m = color.matrix;
        const double kr = m == RCS_MATRIX_BT601 ? 0.299 : m == RCS_MATRIX_BT2020 ? 0.2627 : 0.2126;
        const double kb = m == RCS_MATRIX_BT601 ? 0.114 : m == RCS_MATRIX_BT2020 ? 0.0593 : 0.0722;
        const double kg = 1 - kr - kb;
        const bool limited = color.range != RCS_RANGE_FULL;
        const double luma = limited ? 255.0 / 219 : 1, chroma = limited ? 255.0 / 224 : 1;
        // Columns: what Y, Cb and Cr each add to R, G and B.
        const GLfloat conversion[9] = {GLfloat(luma), GLfloat(luma), GLfloat(luma),
                                       0, GLfloat(-2 * kb * (1 - kb) / kg * chroma), GLfloat(2 * (1 - kb) * chroma),
                                       GLfloat(2 * (1 - kr) * chroma), GLfloat(-2 * kr * (1 - kr) / kg * chroma), 0};
        const GLfloat offset[3] = {GLfloat(limited ? 16.0 / 255 : 0), GLfloat(128.0 / 255), GLfloat(128.0 / 255)};
        glUseProgram(readYuv_.id);
        glUniformMatrix3fv(readYuv_.conversion, 1, GL_FALSE, conversion);
        glUniform3fv(readYuv_.offset, 1, offset);
    }

    // Draws the bound texture into the viewport, turned `rotation` degrees
    // clockwise; `flipped` for a window, whose first row is at the top. Of the
    // texture the part across x down (fractions of it) from the first row's
    // start is the picture.
    void quad(Shader shader, uint32_t rotation, bool flipped, float across = 1, float down = 1) {
        const Program& program = shader == Shader::external ? external_
                                 : shader == Shader::read   ? read_
                                 : shader == Shader::readYuv ? readYuv_
                                                             : pixels_;
        glUseProgram(program.id);
        glUniform1i(program.picture, 0);
        static constexpr GLfloat positions[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        GLfloat coordinates[8];
        for (int corner = 0; corner < 4; ++corner) {
            // Where the corner lies in the upright picture (across, down), and
            // so where it is in the buffer, whose first row is coordinate 0.
            const float u = (positions[corner * 2] + 1) / 2;
            const float v = flipped ? (1 - positions[corner * 2 + 1]) / 2 : (positions[corner * 2 + 1] + 1) / 2;
            float s = u, t = v;
            switch (rotation) {
            case 90:
                s = v, t = 1 - u;
                break;
            case 180:
                s = 1 - u, t = 1 - v;
                break;
            case 270:
                s = 1 - v, t = u;
                break;
            default:
                break;
            }
            coordinates[corner * 2] = s * across;
            coordinates[corner * 2 + 1] = t * down;
        }
        glVertexAttribPointer(program.position, 2, GL_FLOAT, GL_FALSE, 0, positions);
        glVertexAttribPointer(program.coordinate, 2, GL_FLOAT, GL_FALSE, 0, coordinates);
        glEnableVertexAttribArray(program.position);
        glEnableVertexAttribArray(program.coordinate);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    PFNEGLPRESENTATIONTIMEANDROIDPROC presentationTime() const noexcept { return presentationTime_; }

private:
    struct Program {
        GLuint id = 0;
        GLint position = 0, coordinate = 0, picture = 0, conversion = 0, offset = 0;
    };

    Worker() {
        thread_ = std::thread([this] { loop(); });
        thread_.detach();
    }

    void loop() {
        try {
            start();
        } catch (const Failure& failure) {
            error_ = failure.what();
        }
        std::unique_lock lock(mutex_);
        for (;;) {
            changed_.wait(lock, [&] { return !queue_.empty(); });
            auto work = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            work();
            lock.lock();
        }
    }

    void start() {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
            fail(RCS_ERROR_DEVICE, eglError("Opening the EGL display"));
        }
        // OpenGL ES 3 where the GPU has it (YCbCr sampled as it is), else 2.
        for (const EGLint version : {3, 2}) {
            const EGLint wanted[] = {EGL_RED_SIZE,
                                     8,
                                     EGL_GREEN_SIZE,
                                     8,
                                     EGL_BLUE_SIZE,
                                     8,
                                     EGL_ALPHA_SIZE,
                                     8,
                                     EGL_RENDERABLE_TYPE,
                                     version == 3 ? EGL_OPENGL_ES3_BIT_KHR : EGL_OPENGL_ES2_BIT,
                                     EGL_SURFACE_TYPE,
                                     EGL_WINDOW_BIT,
                                     EGL_RECORDABLE_ANDROID,
                                     1,
                                     EGL_NONE};
            EGLint count = 0;
            if (!eglChooseConfig(display_, wanted, &config_, 1, &count) || count < 1) {
                continue;
            }
            const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
            context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, attributes);
            if (context_ != EGL_NO_CONTEXT) {
                break;
            }
        }
        if (context_ == EGL_NO_CONTEXT) {
            fail(RCS_ERROR_DEVICE, eglError("Creating an OpenGL ES context with a recordable config"));
        }
        offscreen();
        getClientBuffer_ = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
        createImage_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroyImage_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        targetTexture_ =
            reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        presentationTime_ =
            reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(eglGetProcAddress("eglPresentationTimeANDROID"));
        if (!getClientBuffer_ || !createImage_ || !destroyImage_ || !targetTexture_ || !presentationTime_) {
            fail(RCS_ERROR_DEVICE, "The GPU's EGL lacks hardware buffer images or presentation times");
        }
        external_ = program(vertexShader, externalShader);
        read_ = program(vertexShader, readShader);
        pixels_ = program(vertexShader, pixelsShader);
        const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (extensions && std::strstr(extensions, "GL_EXT_YUV_target")) {
            readYuv_ = program(yuvVertexShader, yuvReadShader);
            readYuv_.conversion = glGetUniformLocation(readYuv_.id, "conversion");
            readYuv_.offset = glGetUniformLocation(readYuv_.id, "offset");
        }
    }

    static GLuint shader(GLenum type, const char* source) {
        const GLuint id = glCreateShader(type);
        glShaderSource(id, 1, &source, nullptr);
        glCompileShader(id);
        GLint compiled = 0;
        glGetShaderiv(id, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log[512] = {};
            glGetShaderInfoLog(id, sizeof(log), nullptr, log);
            fail(RCS_ERROR_DEVICE, std::string("The GPU cannot compile the SDK's shader: ") + log);
        }
        return id;
    }

    static Program program(const char* vertex, const char* fragment) {
        Program result;
        result.id = glCreateProgram();
        glAttachShader(result.id, shader(GL_VERTEX_SHADER, vertex));
        glAttachShader(result.id, shader(GL_FRAGMENT_SHADER, fragment));
        glLinkProgram(result.id);
        GLint linked = 0;
        glGetProgramiv(result.id, GL_LINK_STATUS, &linked);
        if (!linked) {
            fail(RCS_ERROR_DEVICE, "The GPU cannot link the SDK's shaders");
        }
        result.position = glGetAttribLocation(result.id, "position");
        result.coordinate = glGetAttribLocation(result.id, "coordinate");
        result.picture = glGetUniformLocation(result.id, "picture");
        return result;
    }

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> queue_;
    std::string error_; // Why the thread has no context.

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getClientBuffer_ = nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC targetTexture_ = nullptr;
    PFNEGLPRESENTATIONTIMEANDROIDPROC presentationTime_ = nullptr;
    Program external_, read_, readYuv_, pixels_; // readYuv_: none (0) without GL_EXT_YUV_target.
};

// The SDK's RGBA8 buffers of the latest size, reused once let go.
class Pool {
public:
    static Pool& instance() {
        static auto* pool = new Pool; // Its buffers' deleters may run at any time.
        return *pool;
    }

    std::shared_ptr<void> take(uint32_t width, uint32_t height) {
        AHardwareBuffer* buffer = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (width != width_ || height != height_) {
                clear();
                width_ = width;
                height_ = height;
            }
            if (!free_.empty()) {
                buffer = free_.back();
                free_.pop_back();
            }
        }
        if (!buffer) {
            AHardwareBuffer_Desc desc{};
            desc.width = width;
            desc.height = height;
            desc.layers = 1;
            desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                         AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
            if (AHardwareBuffer_allocate(&desc, &buffer) != 0) {
                fail(RCS_ERROR_DEVICE, "Could not allocate a " + std::to_string(width) + "x" + std::to_string(height) +
                                           " hardware buffer");
            }
        }
        return {buffer, [this, width, height](void* value) {
                    give(static_cast<AHardwareBuffer*>(value), width, height);
                }};
    }

private:
    void give(AHardwareBuffer* buffer, uint32_t width, uint32_t height) {
        {
            std::lock_guard lock(mutex_);
            if (width == width_ && height == height_ && free_.size() < 4) {
                free_.push_back(buffer);
                return;
            }
        }
        AHardwareBuffer_release(buffer);
    }

    void clear() {
        for (auto* buffer : free_) {
            AHardwareBuffer_release(buffer);
        }
        free_.clear();
    }

    std::mutex mutex_;
    uint32_t width_ = 0, height_ = 0;
    std::vector<AHardwareBuffer*> free_;
};

} // namespace

rcs_pixel_format layoutOf(uint32_t format) noexcept {
    switch (format) {
    case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
    case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
    case 5: // HAL_PIXEL_FORMAT_BGRA_8888, which some producers use.
        return RCS_PIXEL_BGRA8;
    case AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420:
        return RCS_PIXEL_NV12;
    default:
        return 0;
    }
}

std::shared_ptr<void> draw(AHardwareBuffer* source, uint32_t rotation, const Rect& content, uint32_t width,
                           uint32_t height) {
    auto target = Pool::instance().take(width, height);
    auto& worker = Worker::instance();
    worker.run([&] {
        worker.offscreen();
        GLuint textures[2] = {};
        glGenTextures(2, textures);
        GLuint framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        struct Release {
            GLuint* textures;
            GLuint framebuffer;
            ~Release() {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &framebuffer);
                glDeleteTextures(2, textures);
            }
        } release{textures, framebuffer};
        Worker::Image into, from;
        worker.bind(static_cast<AHardwareBuffer*>(target.get()), GL_TEXTURE_2D, textures[0], into);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fail(RCS_ERROR_DEVICE, "The GPU cannot draw into the SDK's hardware buffer");
        }
        glViewport(0, 0, GLsizei(width), GLsizei(height));
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        worker.bind(source, GL_TEXTURE_EXTERNAL_OES, textures[1], from);
        // A framebuffer's first row is the buffer's first: no flip.
        glViewport(GLint(content.x), GLint(content.y), GLsizei(content.width), GLsizei(content.height));
        worker.quad(Worker::Shader::external, rotation, false);
        // Done before the host may hand its buffer back to its producer.
        glFinish();
        if (glGetError() != GL_NO_ERROR) {
            fail(RCS_ERROR_DEVICE, "Drawing the hardware buffer failed");
        }
    });
    return target;
}

// Where pictures are read: a linear buffer the CPU maps, drawn into as a
// framebuffer and kept from one picture to the next (glReadPixels of a tiled
// framebuffer takes a phone longer than a frame at 1080p60). GL thread only.
struct ReadTarget {
    AHardwareBuffer* buffer = nullptr;
    std::unique_ptr<Worker::Image> image;
    GLuint textures[2] = {}; // The target, and the source's external texture.
    GLuint framebuffer = 0;
    uint32_t width = 0, height = 0, stride = 0;

    void prepare(Worker& worker, uint32_t w, uint32_t h) {
        if (buffer && w == width && h == height) {
            return;
        }
        release();
        AHardwareBuffer_Desc desc{};
        desc.width = w;
        desc.height = h;
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
        if (AHardwareBuffer_allocate(&desc, &buffer) != 0) {
            buffer = nullptr;
            fail(RCS_ERROR_DEVICE, "Could not allocate a " + std::to_string(w) + "x" + std::to_string(h) +
                                       " hardware buffer to read pictures into");
        }
        AHardwareBuffer_describe(buffer, &desc);
        width = w;
        height = h;
        stride = desc.stride;
        glGenTextures(2, textures);
        glGenFramebuffers(1, &framebuffer);
        image = std::make_unique<Worker::Image>();
        try {
            worker.bind(buffer, GL_TEXTURE_2D, textures[0], *image);
        } catch (...) {
            release(); // Not kept half built: the next picture of this size builds it again.
            throw;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            release();
            fail(RCS_ERROR_DEVICE, "The GPU cannot draw the picture for reading");
        }
    }

    void release() noexcept {
        if (framebuffer) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteTextures(2, textures);
            framebuffer = 0;
        }
        image.reset();
        if (buffer) {
            AHardwareBuffer_release(buffer);
            buffer = nullptr;
        }
        width = height = 0;
    }
};

void read(AHardwareBuffer* source, uint32_t width, uint32_t height, const rcs_color* yuv,
          const std::function<void(const uint8_t* bgra, uint32_t strideBytes)>& use) {
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(source, &desc);
    auto& worker = Worker::instance();
    static std::mutex reading;                // One target: drawn, mapped and used by one reader at a time.
    static auto& target = *new ReadTarget; // Lives as long as the GL thread, past static destruction.
    std::lock_guard lock(reading);
    worker.run([&] {
        worker.offscreen();
        target.prepare(worker, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        Worker::Image from;
        worker.bind(source, GL_TEXTURE_EXTERNAL_OES, target.textures[1], from);
        // Sampled at the pixels' centres, one to one.
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glViewport(0, 0, GLsizei(width), GLsizei(height));
        // The framebuffer's first row is the buffer's first: no flip. The
        // shader writes blue first, so the bytes are BGRA8.
        const bool asStream = yuv && worker.readsYuv();
        if (asStream) {
            worker.convertAs(*yuv);
        }
        worker.quad(asStream ? Worker::Shader::readYuv : Worker::Shader::read, 0, false,
                    float(width) / float(desc.width), float(height) / float(desc.height));
        glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, 0); // The thread's other work draws into its own surfaces.
        if (glGetError() != GL_NO_ERROR) {
            fail(RCS_ERROR_DEVICE, "Drawing the picture for reading failed");
        }
    });
    void* address = nullptr;
    if (AHardwareBuffer_lock(target.buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &address) != 0) {
        fail(RCS_ERROR_DEVICE, "Could not map the picture read from the GPU");
    }
    struct Unlock {
        AHardwareBuffer* buffer;
        ~Unlock() { AHardwareBuffer_unlock(buffer, nullptr); }
    } unlock{target.buffer};
    use(static_cast<const uint8_t*>(address), target.stride * 4);
}

Window::Window(ANativeWindow* window) : window_(window) {
    ANativeWindow_acquire(window_);
    auto& worker = Worker::instance();
    try {
        worker.run([&] {
            EGLSurface surface = eglCreateWindowSurface(worker.display(), worker.config(), window_, nullptr);
            if (surface == EGL_NO_SURFACE) {
                fail(RCS_ERROR_DEVICE, eglError("Making an EGL surface of the encoder's input"));
            }
            surface_ = surface;
        });
    } catch (...) {
        ANativeWindow_release(window_);
        throw;
    }
}

Window::~Window() {
    auto& worker = Worker::instance();
    try {
        worker.run([&] {
            worker.offscreen();
            if (texture_) {
                glDeleteTextures(1, &texture_);
            }
            eglDestroySurface(worker.display(), surface_);
        });
    } catch (const Failure&) { // Only when the thread has no context, and so made no surface.
    }
    ANativeWindow_release(window_);
}

void Window::draw(AHardwareBuffer* buffer, uint64_t presentNs) {
    auto& worker = Worker::instance();
    worker.run([&] {
        if (!eglMakeCurrent(worker.display(), surface_, surface_, worker.context())) {
            fail(RCS_ERROR_DEVICE, eglError("Making the encoder's input current"));
        }
        struct Texture {
            GLuint name = 0;
            ~Texture() { glDeleteTextures(1, &name); } // Also when binding the picture fails.
        } texture;
        glGenTextures(1, &texture.name);
        {
            Worker::Image image;
            worker.bind(buffer, GL_TEXTURE_EXTERNAL_OES, texture.name, image);
            glViewport(0, 0, ANativeWindow_getWidth(window_), ANativeWindow_getHeight(window_));
            worker.quad(Worker::Shader::external, 0, true);
        }
        worker.presentationTime()(worker.display(), surface_, EGLnsecsANDROID(presentNs));
        if (!eglSwapBuffers(worker.display(), surface_)) {
            fail(RCS_ERROR_DEVICE, eglError("Handing the picture to the encoder"));
        }
    });
}

void Window::draw(const uint8_t* pixels, uint32_t stride, uint32_t width, uint32_t height, uint64_t presentNs) {
    // OpenGL ES 2 reads rows of exactly width * 4 bytes.
    std::vector<uint8_t> packed;
    if (stride != width * 4) {
        packed.resize(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
            std::copy_n(pixels + size_t(y) * stride, size_t(width) * 4, packed.data() + size_t(y) * width * 4);
        }
        pixels = packed.data();
    }
    auto& worker = Worker::instance();
    worker.run([&] {
        if (!eglMakeCurrent(worker.display(), surface_, surface_, worker.context())) {
            fail(RCS_ERROR_DEVICE, eglError("Making the encoder's input current"));
        }
        if (!texture_) {
            glGenTextures(1, &texture_);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(width), GLsizei(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glViewport(0, 0, ANativeWindow_getWidth(window_), ANativeWindow_getHeight(window_));
        worker.quad(Worker::Shader::pixels, 0, true);
        worker.presentationTime()(worker.display(), surface_, EGLnsecsANDROID(presentNs));
        if (!eglSwapBuffers(worker.display(), surface_)) {
            fail(RCS_ERROR_DEVICE, eglError("Handing the picture to the encoder"));
        }
    });
}

} // namespace rcsdk::platform::android::gl
