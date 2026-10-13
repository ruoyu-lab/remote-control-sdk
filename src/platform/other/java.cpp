// Systems without Java: rcs_android_initialize has nothing to keep.
// (Android: platform/android/jni.cpp.)
#include "platform/platform.hpp"

namespace rcsdk::platform {

void setJava(void*, void*) {
    fail(RCS_ERROR_UNSUPPORTED, "Only Android has a Java VM to hand over (rcs_android_initialize)");
}

std::pair<void*, void*> java() {
    fail(RCS_ERROR_UNSUPPORTED, "Only Android has a Java VM (rcs_android_get_java)");
}

} // namespace rcsdk::platform
