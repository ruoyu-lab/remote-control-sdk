# Android from the NDK's own toolchain file, for Android 10 (API 29) and
# later: ANDROID_ABI arm64-v8a (the default) or x86_64 (emulators,
# Chromebooks; the codec libraries' assembly needs NASM there). The NDK is
# ANDROID_NDK (a CMake or environment variable), else ANDROID_NDK_HOME or
# ANDROID_NDK_ROOT, else the newest one under the SDK (ANDROID_HOME,
# ANDROID_SDK_ROOT, or Android Studio's ~/Library/Android/sdk and
# ~/Android/Sdk).
set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI: arm64-v8a or x86_64")
if(NOT ANDROID_ABI MATCHES "^(arm64-v8a|x86_64)$")
    message(FATAL_ERROR "The SDK builds for ANDROID_ABI arm64-v8a or x86_64, not ${ANDROID_ABI}")
endif()
set(ANDROID_PLATFORM "android-29" CACHE STRING "Oldest Android API level")
# The C++ library is linked into each program and shared library; a host that
# links the static libraries chooses its own (Qt: c++_shared).
set(ANDROID_STL "c++_static" CACHE STRING "Android C++ library")

if(NOT ANDROID_NDK)
    foreach(variable ANDROID_NDK ANDROID_NDK_HOME ANDROID_NDK_ROOT)
        if(DEFINED ENV{${variable}} AND EXISTS "$ENV{${variable}}/build/cmake/android.toolchain.cmake")
            set(ANDROID_NDK "$ENV{${variable}}")
            break()
        endif()
    endforeach()
endif()
if(NOT ANDROID_NDK)
    set(sdks "$ENV{ANDROID_HOME}" "$ENV{ANDROID_SDK_ROOT}" "$ENV{HOME}/Library/Android/sdk" "$ENV{HOME}/Android/Sdk")
    foreach(sdk IN LISTS sdks)
        if(sdk AND IS_DIRECTORY "${sdk}/ndk")
            file(GLOB versions LIST_DIRECTORIES true "${sdk}/ndk/*")
            list(SORT versions COMPARE NATURAL ORDER DESCENDING)
            foreach(version IN LISTS versions)
                if(EXISTS "${version}/build/cmake/android.toolchain.cmake")
                    set(ANDROID_NDK "${version}")
                    break()
                endif()
            endforeach()
        endif()
        if(ANDROID_NDK)
            break()
        endif()
    endforeach()
endif()
if(NOT ANDROID_NDK)
    message(FATAL_ERROR "No Android NDK found: set ANDROID_NDK to it (or ANDROID_HOME to an SDK with one)")
endif()
set(ANDROID_NDK "${ANDROID_NDK}" CACHE PATH "Android NDK")
include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
# x86_64: the codec libraries' assembly is NASM's (dav1d and libaom take
# nothing else), not the YASM the NDK names.
if(ANDROID_ABI STREQUAL "x86_64")
    find_program(RCSDK_NASM nasm)
    if(RCSDK_NASM)
        set(CMAKE_ASM_NASM_COMPILER "${RCSDK_NASM}")
    else()
        unset(CMAKE_ASM_NASM_COMPILER)
    endif()
endif()
# Bionic's libc has the threads, but not pthread_cancel, which FindThreads'
# check calls: it would settle on -pthread, which reaches NASM too.
set(CMAKE_HAVE_LIBC_PTHREAD 1 CACHE INTERNAL "Bionic's libc has the threads")
# Tests run over adb (cmake/android_run.sh), pushed without their debug
# information, on a device or emulator of their ABI.
if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${CMAKE_CURRENT_LIST_DIR}/../android_run.sh;--abi;${ANDROID_ABI};--strip;${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-strip${ANDROID_TOOLCHAIN_SUFFIX}")
endif()
