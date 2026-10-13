// Java through the host's VM (rcs_android_initialize): the system services
// Android offers only to Java. Every call throws Failure, UNSUPPORTED before
// the host handed over its VM.
#pragma once

#include "util/util.hpp"

#include <jni.h>

#include <string>
#include <utility>

namespace rcsdk::platform::android {

// This thread's JNIEnv, attached to the host's VM (and detached when a thread
// of the SDK's that attached ends).
JNIEnv* env();
// The application context (a global reference that lives as long as the process).
jobject context();
// Whether the host handed over its VM.
bool hasJava() noexcept;

// A local reference, deleted when it goes.
template <class T = jobject>
class Local {
public:
    Local() = default;
    Local(JNIEnv* env, T value) noexcept : env_(env), value_(value) {}
    Local(Local&& other) noexcept : env_(other.env_), value_(std::exchange(other.value_, nullptr)) {}
    Local& operator=(Local&& other) noexcept {
        std::swap(env_, other.env_);
        std::swap(value_, other.value_);
        return *this;
    }
    ~Local() {
        if (value_) {
            env_->DeleteLocalRef(value_);
        }
    }
    T get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    JNIEnv* env_ = nullptr;
    T value_ = nullptr;
};

// Throws Failure(result) naming `what` and the Java exception when one is
// pending (and clears it).
void check(JNIEnv* env, rcs_result result, const std::string& what);
// The class `name` ("android/media/AudioManager"); Failure(UNSUPPORTED) when this system has none.
Local<jclass> findClass(JNIEnv* env, const char* name);
jmethodID method(JNIEnv* env, jclass type, const char* name, const char* signature);
std::string text(JNIEnv* env, jstring value);
Local<jstring> javaString(JNIEnv* env, const std::string& value);
// context.getSystemService(name), e.g. "audio"; Failure(UNSUPPORTED) when there is none.
Local<> systemService(JNIEnv* env, const char* name);

} // namespace rcsdk::platform::android
