// Android: Qt Quick renders with OpenGL ES, and a decoded frame is the
// decoder's hardware buffer in its own YCbCr layout. The node makes it an
// EGLImage, samples it as an external texture (the GPU converts it to RGB as
// the buffer says) and draws it with OpenGL ES calls of its own between the
// scene graph's (QSGRenderNode::render may: Qt wraps it in beginExternal()).
#include "remote_video.hpp"
#include "video_node.hpp"

#include <rcsdk/gpu.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <QQuickWindow>
#include <rhi/qrhi.h>

#include <deque>
#include <utility>

namespace {

constexpr char vertexShader[] = R"(
uniform mat4 mvp;
attribute vec2 position;
attribute vec2 coordinate;
varying vec2 sample;
void main() {
    sample = coordinate;
    gl_Position = mvp * vec4(position, 0.0, 1.0);
}
)";
constexpr char fragmentShader[] = R"(
#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES picture;
uniform float opacity;
varying vec2 sample;
void main() {
    gl_FragColor = vec4(texture2D(picture, sample).rgb * opacity, opacity);
}
)";

GLuint compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    return shader;
}

class ExternalVideoNode final : public FrameNode {
public:
    ExternalVideoNode(QQuickWindow* window, RemoteVideo* item) : window_(window), item_(item) {}
    ~ExternalVideoNode() override { releaseResources(); }

    void update(const rcsdk::GpuFrame& frame, const QRectF& target, const QRectF& source, bool smooth,
                RemoteSession* session) override {
        if (frame.handle() != pending_.handle()) {
            pending_ = frame;
            fresh_ = true;
        }
        session_ = session;
        target_ = target;
        source_ = source;
        smooth_ = smooth;
        markDirty(QSGNode::DirtyMaterial);
    }

    void render(const RenderState* state) override {
        if (!program_ && !createProgram()) {
            return;
        }
        if (auto newer = newerFrame(session_, pending_)) { // Drawn again without a synchronization.
            pending_ = std::move(newer);
            fresh_ = true;
        }
        if (fresh_) {
            fresh_ = false;
            if (pending_) {
                item_->framePresented();
                importFrame();
            } else {
                images_.clear();
            }
        }
        if (images_.empty()) {
            return;
        }
        const QSize size = renderTarget() ? renderTarget()->pixelSize() : window_->size() * window_->devicePixelRatio();
        glViewport(0, 0, size.width(), size.height());
        if (state->scissorEnabled()) {
            const QRect scissor = state->scissorRect();
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor.x(), scissor.y(), scissor.width(), scissor.height());
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // Premultiplied, for the item's opacity.
        glUseProgram(program_);
        const QMatrix4x4 mvp = *projectionMatrix() * *matrix();
        glUniformMatrix4fv(mvpLocation_, 1, GL_FALSE, mvp.constData());
        glUniform1f(opacityLocation_, float(inheritedOpacity()));
        glUniform1i(pictureLocation_, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, images_.back().texture);
        const GLint filter = smooth_ ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, filter);
        const float x0 = float(target_.left()), y0 = float(target_.top());
        const float x1 = float(target_.right()), y1 = float(target_.bottom());
        const float u0 = float(source_.left()), v0 = float(source_.top());
        const float u1 = float(source_.right()), v1 = float(source_.bottom());
        const float positions[8] = {x0, y0, x1, y0, x0, y1, x1, y1};
        const float coordinates[8] = {u0, v0, u1, v0, u0, v1, u1, v1};
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(GLuint(positionLocation_), 2, GL_FLOAT, GL_FALSE, 0, positions);
        glVertexAttribPointer(GLuint(coordinateLocation_), 2, GL_FLOAT, GL_FALSE, 0, coordinates);
        glEnableVertexAttribArray(GLuint(positionLocation_));
        glEnableVertexAttribArray(GLuint(coordinateLocation_));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(GLuint(positionLocation_));
        glDisableVertexAttribArray(GLuint(coordinateLocation_));
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        glUseProgram(0);
    }

    void releaseResources() override {
        images_.clear();
        if (program_) {
            glDeleteProgram(program_);
            program_ = 0;
        }
    }

    StateFlags changedStates() const override { return ViewportState | ScissorState | BlendState; }
    RenderingFlags flags() const override { return BoundedRectRendering; }
    QRectF rect() const override { return target_; }

private:
    // A frame as an external texture: kept (with the decoder's image) while the GPU may still read it.
    struct Image {
        rcsdk::GpuFrame frame;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        Image() = default;
        Image(Image&& other) noexcept
            : frame(std::move(other.frame)), image(std::exchange(other.image, EGL_NO_IMAGE_KHR)),
              texture(std::exchange(other.texture, 0)) {}
        ~Image() {
            if (texture) {
                glDeleteTextures(1, &texture);
            }
            if (image != EGL_NO_IMAGE_KHR) {
                destroyImage(eglGetCurrentDisplay(), image);
            }
        }
    };

    bool createProgram() {
        getClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
        createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        targetTexture =
            reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!getClientBuffer || !createImage || !destroyImage || !targetTexture) {
            item_->reportImport(QStringLiteral("This GPU's OpenGL ES cannot sample hardware buffers"));
            return false;
        }
        program_ = glCreateProgram();
        const GLuint vertex = compile(GL_VERTEX_SHADER, vertexShader);
        const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentShader);
        glAttachShader(program_, vertex);
        glAttachShader(program_, fragment);
        glLinkProgram(program_);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = 0;
        glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (!linked) {
            glDeleteProgram(program_);
            program_ = 0;
            item_->reportImport(QStringLiteral("Could not build the external texture shader"));
            return false;
        }
        mvpLocation_ = glGetUniformLocation(program_, "mvp");
        opacityLocation_ = glGetUniformLocation(program_, "opacity");
        pictureLocation_ = glGetUniformLocation(program_, "picture");
        positionLocation_ = glGetAttribLocation(program_, "position");
        coordinateLocation_ = glGetAttribLocation(program_, "coordinate");
        return true;
    }

    void importFrame() {
        const auto info = pending_.info();
        if (info.resource_type != RCS_GPU_AHARDWAREBUFFER) {
            item_->reportImport(QStringLiteral("Expected an AHardwareBuffer"));
            return;
        }
        Image image;
        image.frame = pending_;
        const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        image.image = createImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                  getClientBuffer(static_cast<AHardwareBuffer*>(info.resource)), attributes);
        if (image.image == EGL_NO_IMAGE_KHR) {
            item_->reportImport(QStringLiteral("Could not make an EGLImage of the frame"));
            return;
        }
        glGenTextures(1, &image.texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, image.texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        targetTexture(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(image.image));
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        images_.push_back(std::move(image));
        // Earlier frames may still be read by frames in flight.
        while (images_.size() > 3) {
            images_.pop_front();
        }
        item_->reportImport({});
    }

    static inline PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getClientBuffer = nullptr;
    static inline PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    static inline PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    static inline PFNGLEGLIMAGETARGETTEXTURE2DOESPROC targetTexture = nullptr;

    QQuickWindow* window_;
    RemoteVideo* item_;
    rcsdk::GpuFrame pending_;
    bool fresh_ = false;
    RemoteSession* session_ = nullptr;
    QRectF target_, source_;
    bool smooth_ = true;
    GLuint program_ = 0;
    GLint mvpLocation_ = -1, opacityLocation_ = -1, pictureLocation_ = -1, positionLocation_ = -1,
          coordinateLocation_ = -1;
    std::deque<Image> images_;
};

} // namespace

FrameNode* createFrameNode(QQuickWindow* window, RemoteVideo* item) {
    return new ExternalVideoNode(window, item);
}
