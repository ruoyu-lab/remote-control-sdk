// The host's Java VM and application context (rcs_android_initialize), and
// what the rest of platform/android needs to call into Java.
#include "platform/android/jni.hpp"

#include "platform/platform.hpp"
#include "util/log.hpp"

#include <pthread.h>

#include <mutex>

namespace rcsdk::platform {
namespace android {
namespace {

std::mutex mutex;
JavaVM* vm = nullptr;
jobject application = nullptr; // A global reference, never deleted: hosts hand over the same one.

// Detaches a thread this file attached when it ends.
pthread_key_t attachedKey;
const bool keyMade = [] {
    return pthread_key_create(&attachedKey,
                              [](void* machine) { static_cast<JavaVM*>(machine)->DetachCurrentThread(); }) == 0;
}();

JNIEnv* attach(JavaVM* machine) {
    JNIEnv* result = nullptr;
    const jint state = machine->GetEnv(reinterpret_cast<void**>(&result), JNI_VERSION_1_6);
    if (state == JNI_OK) {
        return result;
    }
    if (state != JNI_EDETACHED || machine->AttachCurrentThread(&result, nullptr) != JNI_OK) {
        fail(RCS_ERROR_INTERNAL, "Could not attach this thread to the Java VM");
    }
    if (keyMade) {
        pthread_setspecific(attachedKey, machine);
    }
    return result;
}

} // namespace

bool hasJava() noexcept {
    std::lock_guard lock(mutex);
    return vm != nullptr;
}

JNIEnv* env() {
    JavaVM* machine = nullptr;
    {
        std::lock_guard lock(mutex);
        machine = vm;
    }
    if (!machine) {
        fail(RCS_ERROR_UNSUPPORTED, "The host has not handed over its Java VM (rcs_android_initialize)");
    }
    return attach(machine);
}

jobject context() {
    std::lock_guard lock(mutex);
    if (!application) {
        fail(RCS_ERROR_UNSUPPORTED, "The host has not handed over its Java VM (rcs_android_initialize)");
    }
    return application;
}

void check(JNIEnv* env, rcs_result result, const std::string& what) {
    if (!env->ExceptionCheck()) {
        return;
    }
    Local<jthrowable> thrown(env, env->ExceptionOccurred());
    env->ExceptionClear();
    std::string reason;
    if (Local<jclass> type{env, env->FindClass("java/lang/Throwable")}) {
        const jmethodID toString = env->GetMethodID(type.get(), "toString", "()Ljava/lang/String;");
        Local<jstring> description(env, static_cast<jstring>(env->CallObjectMethod(thrown.get(), toString)));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        } else {
            reason = text(env, description.get());
        }
    }
    fail(result, what + (reason.empty() ? std::string(" failed in Java") : ": " + reason));
}

Local<jclass> findClass(JNIEnv* env, const char* name) {
    Local<jclass> type(env, env->FindClass(name));
    check(env, RCS_ERROR_UNSUPPORTED, std::string("This system has no ") + name);
    return type;
}

jmethodID method(JNIEnv* env, jclass type, const char* name, const char* signature) {
    const jmethodID id = env->GetMethodID(type, name, signature);
    check(env, RCS_ERROR_UNSUPPORTED, std::string("This system has no method ") + name + signature);
    return id;
}

std::string text(JNIEnv* env, jstring value) {
    if (!value) {
        return {};
    }
    const char* bytes = env->GetStringUTFChars(value, nullptr);
    if (!bytes) { // Java ran out of memory; its exception says so.
        check(env, RCS_ERROR_DEVICE, "Reading a Java string");
        fail(RCS_ERROR_DEVICE, "Reading a Java string failed");
    }
    std::string result = bytes;
    env->ReleaseStringUTFChars(value, bytes);
    return result;
}

Local<jstring> javaString(JNIEnv* env, const std::string& value) {
    Local<jstring> result(env, env->NewStringUTF(value.c_str()));
    check(env, RCS_ERROR_INTERNAL, "Making a Java string");
    return result;
}

Local<> systemService(JNIEnv* env, const char* name) {
    const auto type = findClass(env, "android/content/Context");
    const jmethodID get = method(env, type.get(), "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    Local<> service(env, env->CallObjectMethod(context(), get, javaString(env, name).get()));
    check(env, RCS_ERROR_UNSUPPORTED, std::string("Context.getSystemService(\"") + name + "\")");
    if (!service) {
        fail(RCS_ERROR_UNSUPPORTED, std::string("This system has no \"") + name + "\" service");
    }
    return service;
}

} // namespace android

void setJava(void* javaVm, void* context) {
    auto* machine = static_cast<JavaVM*>(javaVm);
    JNIEnv* env = android::attach(machine);
    const auto type = android::findClass(env, "android/content/Context");
    if (!env->IsInstanceOf(static_cast<jobject>(context), type.get())) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "context is no android.content.Context");
    }
    const jmethodID getApplication =
        android::method(env, type.get(), "getApplicationContext", "()Landroid/content/Context;");
    android::Local<> application(env, env->CallObjectMethod(static_cast<jobject>(context), getApplication));
    android::check(env, RCS_ERROR_INVALID_ARGUMENT, "Context.getApplicationContext()");
    // An application's own context may have none of its own yet (while it is made).
    const jobject chosen = application ? application.get() : static_cast<jobject>(context);
    {
        std::lock_guard lock(android::mutex);
        if (!android::application || !env->IsSameObject(android::application, chosen)) {
            android::application = env->NewGlobalRef(chosen);
        }
        android::vm = machine;
    }
    // Listing the codecs opens each and decodes probes, some 0.6 s on a
    // Snapdragon 8 Elite: done now, while the application starts, and not
    // when it first connects (that waits for it if it still runs).
    static std::once_flag warming;
    std::call_once(warming, [] { log::thread([] { codecImplementations(); }).detach(); });
}

std::pair<void*, void*> java() {
    std::lock_guard lock(android::mutex);
    if (!android::vm) {
        fail(RCS_ERROR_INVALID_STATE, "The host has not handed over its Java VM yet (rcs_android_initialize)");
    }
    return {android::vm, android::application};
}

} // namespace rcsdk::platform
