// Keeping an Android device awake with PowerManager's wake locks, through the
// Java VM the host handed the SDK (rcs_android_initialize): SYSTEM keeps the
// CPU running (PARTIAL_WAKE_LOCK), DISPLAY the screen on too
// (SCREEN_BRIGHT_WAKE_LOCK). The application declares
// android.permission.WAKE_LOCK; `adb shell dumpsys power` lists the locks
// under "rcsdk:" and the session's reason.
#include "power_request.hpp"

#include <rcsdk/android.h>

#include <jni.h>
#include <pthread.h>

namespace rcsdk::power {
namespace {

constexpr jint partialWakeLock = 0x1, screenBrightWakeLock = 0xa;

pthread_key_t attachedKey;
const bool keyMade = [] {
    return pthread_key_create(&attachedKey, [](void* vm) { static_cast<JavaVM*>(vm)->DetachCurrentThread(); }) == 0;
}();

struct Java {
    JNIEnv* env = nullptr;
    jobject context = nullptr;
};

Java java() {
    void* vm = nullptr;
    void* context = nullptr;
    if (rcs_android_get_java(&vm, &context) != RCS_OK) {
        throw Refused(RCS_ERROR_UNSUPPORTED, "The host has not handed over its Java VM (rcs_android_initialize): "
                                             "wake locks are Java's");
    }
    auto* machine = static_cast<JavaVM*>(vm);
    JNIEnv* env = nullptr;
    if (machine->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (machine->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            throw Refused(RCS_ERROR_INTERNAL, "Could not attach this thread to the Java VM");
        }
        if (keyMade) {
            pthread_setspecific(attachedKey, machine);
        }
    }
    return {env, static_cast<jobject>(context)};
}

// Throws Refused naming what Java threw; a SecurityException is a missing permission.
void check(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) {
        return;
    }
    jthrowable thrown = env->ExceptionOccurred();
    env->ExceptionClear();
    const bool security = env->IsInstanceOf(thrown, env->FindClass("java/lang/SecurityException"));
    env->DeleteLocalRef(thrown);
    if (security) {
        throw Refused(RCS_ERROR_PERMISSION_REQUIRED, std::string(what) + ": the application does not declare "
                                                                         "android.permission.WAKE_LOCK");
    }
    throw Refused(RCS_ERROR_INTERNAL, std::string(what) + " failed in Java");
}

class WakeLock final : public Request {
public:
    WakeLock(rcs_keep_awake mode, const std::string& reason) {
        const auto [env, context] = java();
        if (env->PushLocalFrame(16) != 0) {
            env->ExceptionClear();
            throw Refused(RCS_ERROR_INTERNAL, "Java has no room for local references");
        }
        struct Pop {
            JNIEnv* env;
            ~Pop() { env->PopLocalFrame(nullptr); }
        } pop{env};
        jclass contextType = env->FindClass("android/content/Context");
        jobject manager = env->CallObjectMethod(
            context, env->GetMethodID(contextType, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"),
            env->NewStringUTF("power"));
        check(env, "Context.getSystemService(\"power\")");
        jclass managerType = env->FindClass("android/os/PowerManager");
        jobject lock = env->CallObjectMethod(
            manager,
            env->GetMethodID(managerType, "newWakeLock", "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;"),
            mode == RCS_KEEP_AWAKE_DISPLAY ? screenBrightWakeLock : partialWakeLock,
            env->NewStringUTF(("rcsdk:" + reason).c_str()));
        check(env, "PowerManager.newWakeLock");
        jclass lockType = env->FindClass("android/os/PowerManager$WakeLock");
        env->CallVoidMethod(lock, env->GetMethodID(lockType, "acquire", "()V"));
        check(env, "Acquiring a wake lock");
        lock_ = env->NewGlobalRef(lock);
        release_ = env->GetMethodID(lockType, "release", "()V");
    }

    ~WakeLock() override {
        try {
            JNIEnv* env = java().env;
            env->CallVoidMethod(lock_, release_);
            env->ExceptionClear();
            env->DeleteGlobalRef(lock_);
        } catch (const Refused&) { // Only before rcs_android_initialize, when no lock was made.
        }
    }

private:
    jobject lock_ = nullptr;
    jmethodID release_ = nullptr;
};

} // namespace

std::unique_ptr<Request> systemRequest(rcs_keep_awake mode, const std::string& reason) {
    return std::make_unique<WakeLock>(mode, reason);
}

bool systemAvailable() {
    void* vm = nullptr;
    void* context = nullptr;
    return rcs_android_get_java(&vm, &context) == RCS_OK;
}

} // namespace rcsdk::power
