#include "screen_projection.hpp"

#include "qt_input.hpp"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QPointer>

#include <android/hardware_buffer_jni.h>

#include <algorithm>
#include <mutex>

namespace {

constexpr auto shareClass = "local/rcsdk/qmldemo/ScreenShare";
constexpr auto inputClass = "local/rcsdk/qmldemo/RemoteInputService";

// The projection the Java side's calls go to; none once it went.
std::mutex mutex;
ScreenProjection* current = nullptr;

jobject context() {
    static const QJniObject application = QNativeInterface::QAndroidApplication::context();
    return application.object();
}

} // namespace

// ScreenShare's native methods, on the Java side's threads.
struct ProjectionNatives {
    static void picture(JNIEnv* env, jclass, jlong handle, jobject buffer) {
        std::lock_guard lock(mutex);
        if (current && jlong(current) == handle) {
            if (AHardwareBuffer* hardware = AHardwareBuffer_fromHardwareBuffer(env, buffer)) {
                current->session_.pushHardwareBuffer(hardware); // Refused while no viewer watches.
            }
        }
    }

    static void sound(JNIEnv* env, jclass, jlong handle, jfloatArray samples, jint frames) {
        std::lock_guard lock(mutex);
        if (current && jlong(current) == handle) {
            auto* data = static_cast<float*>(env->GetPrimitiveArrayCritical(samples, nullptr));
            current->session_.pushAudio(data, uint32_t(frames));
            env->ReleasePrimitiveArrayCritical(samples, data, JNI_ABORT);
        }
    }

    static void state(JNIEnv*, jclass, jlong handle, jboolean running, jstring reason) {
        const QString why = reason ? QJniObject(reason).toString() : QString();
        std::lock_guard lock(mutex);
        if (current && jlong(current) == handle) {
            QMetaObject::invokeMethod(
                current,
                [self = QPointer(current), running = running != JNI_FALSE, why] {
                    if (!self) {
                        return;
                    }
                    self->running_ = running;
                    emit self->runningChanged();
                    if (!running && !why.isEmpty()) {
                        emit self->ended(why);
                    }
                },
                Qt::QueuedConnection);
        }
    }
};

ScreenProjection::ScreenProjection(RemoteSession& session, QObject* parent) : QObject(parent), session_(session) {
    static const bool registered = [] {
        static const JNINativeMethod methods[] = {
            {"nativePicture", "(JLandroid/hardware/HardwareBuffer;)V",
             reinterpret_cast<void*>(&ProjectionNatives::picture)},
            {"nativeSound", "(J[FI)V", reinterpret_cast<void*>(&ProjectionNatives::sound)},
            {"nativeState", "(JZLjava/lang/String;)V", reinterpret_cast<void*>(&ProjectionNatives::state)},
        };
        QJniEnvironment env;
        return env.registerNativeMethods(shareClass, methods, 3);
    }();
    Q_UNUSED(registered);
    std::lock_guard lock(mutex);
    current = this;
}

ScreenProjection::~ScreenProjection() {
    stop();
    std::lock_guard lock(mutex);
    current = nullptr;
}

void ScreenProjection::start(int longSide, bool sound) {
    QJniObject::callStaticMethod<void>(shareClass, "start", "(Landroid/content/Context;JIZ)V", context(),
                                       jlong(this), jint(longSide), jboolean(sound));
}

void ScreenProjection::stop() {
    QJniObject::callStaticMethod<void>(shareClass, "stop", "(Landroid/content/Context;)V", context());
}

// ---- ScreenInput -------------------------------------------------------------------

bool ScreenInput::enabled() {
    return QJniObject::callStaticMethod<jboolean>(inputClass, "enabled", "()Z");
}

void ScreenInput::openSettings() {
    QJniObject::callStaticMethod<void>(inputClass, "openSettings", "(Landroid/content/Context;)V", context());
}

void ScreenInput::execute(const rcs_input_event& event, const rcs_stream_info& stream) {
    // Across the stream's picture, then across the screen where it lies in it.
    float u = lastU_, v = lastV_;
    if (stream.width > 1 && stream.height > 1 && stream.content_width > 1 && stream.content_height > 1) {
        const double x = event.x / 65535.0 * (stream.width - 1), y = event.y / 65535.0 * (stream.height - 1);
        u = float(std::clamp((x - stream.content_x) / (stream.content_width - 1), 0.0, 1.0));
        v = float(std::clamp((y - stream.content_y) / (stream.content_height - 1), 0.0, 1.0));
    }
    const auto call = [](const char* name) { QJniObject::callStaticMethod<void>(inputClass, name, "()V"); };
    const auto at = [&](const char* name) {
        QJniObject::callStaticMethod<void>(inputClass, name, "(FF)V", jfloat(u), jfloat(v));
    };
    switch (event.type) {
    case RCS_INPUT_MOVE:
        lastU_ = u;
        lastV_ = v;
        if (pressed_) {
            at("move");
        }
        break;
    case RCS_INPUT_BUTTON:
        lastU_ = u;
        lastV_ = v;
        if (event.code == RCS_BUTTON_LEFT) {
            pressed_ = event.down != 0;
            at(pressed_ ? "press" : "release");
        } else if (event.down) { // The other buttons as the system's keys.
            call(event.code == RCS_BUTTON_MIDDLE    ? "home"
                 : event.code == RCS_BUTTON_FORWARD ? "recents"
                                                    : "back");
        }
        break;
    case RCS_INPUT_WHEEL:
    case RCS_INPUT_SCROLL: { // A swipe: a line is 8% of the screen; scrolling up moves the page down.
        const double lines = event.type == RCS_INPUT_WHEEL ? 1.0 : 1.0 / 120;
        QJniObject::callStaticMethod<void>(inputClass, "swipe", "(FFFF)V", jfloat(u), jfloat(v),
                                           jfloat(-event.wheel_x * lines * 0.08), jfloat(event.wheel_y * lines * 0.08));
        break;
    }
    case RCS_INPUT_KEY: {
        const demo::UsKey key = demo::usKey(event.code);
        if (key.qt == Qt::Key_Shift) {
            shift_ = event.down != 0;
        } else if (event.down) {
            switch (key.qt) {
            case Qt::Key_Escape:
                call("back");
                break;
            case Qt::Key_Return:
                call("enter");
                break;
            case Qt::Key_Backspace:
                call("backspace");
                break;
            default:
                if (const char typed = shift_ ? key.shifted : key.text) {
                    QJniObject::callStaticMethod<void>(inputClass, "type", "(Ljava/lang/String;)V",
                                                       QJniObject::fromString(QString(QChar(typed))).object());
                }
                break;
            }
        }
        break;
    }
    case RCS_INPUT_TEXT: {
        const char32_t code = event.code;
        QJniObject::callStaticMethod<void>(inputClass, "type", "(Ljava/lang/String;)V",
                                           QJniObject::fromString(QString::fromUcs4(&code, 1)).object());
        break;
    }
    case RCS_INPUT_TOUCH: // The first contact, and a pen, as a finger.
    case RCS_INPUT_PEN: {
        const bool touching = event.down && !(event.type == RCS_INPUT_PEN && (event.pen & RCS_PEN_LEAVE));
        if (event.type == RCS_INPUT_TOUCH && pressed_ && event.code != contact_) {
            break;
        }
        lastU_ = u;
        lastV_ = v;
        if (touching && !pressed_) {
            pressed_ = true;
            contact_ = event.type == RCS_INPUT_TOUCH ? event.code : 0;
            at("press");
        } else if (touching) {
            at("move");
        } else if (pressed_) {
            pressed_ = false;
            at("release");
        }
        break;
    }
    case RCS_INPUT_RELEASE_ALL:
        if (pressed_) {
            pressed_ = false;
            u = lastU_;
            v = lastV_;
            at("release");
        }
        shift_ = false;
        break;
    default:
        break;
    }
}
