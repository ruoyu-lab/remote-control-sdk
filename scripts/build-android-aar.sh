#!/bin/bash
# Builds RemoteControlSDK-<version>.aar for Android hosts that build with
# Gradle: the core, the codec libraries built in and the official modules as
# one static library per ABI (arm64-v8a for phones and tablets, x86_64 for
# emulators and Chromebooks; Android 10 / API 29 on) with the C and C++
# headers, as a Prefab package that the Android Gradle plugin hands to the
# host's CMake or ndk-build (android { buildFeatures { prefab = true } }):
#
#   find_package(RemoteControlSDK REQUIRED CONFIG)
#   target_link_libraries(app PRIVATE RemoteControlSDK::RemoteControl)
#
#   scripts/build-android-aar.sh [OUTPUT_DIR [ABI...]]
#       (default: build/aar, arm64-v8a x86_64)
#
# The NDK is found as for the android presets (cmake/toolchains/android.cmake);
# x86_64 needs NASM for the codec libraries' assembly (name arm64-v8a alone
# without it). The system libraries the SDK uses come with the package
# (mediandk, android, aaudio, EGL, GLESv3, log), and its manifest asks for
# INTERNET, without which nothing connects, and WAKE_LOCK, for Wi-Fi without
# power saving while connected and the power module. It holds no Java: the
# host hands over its Java VM (rcs_android_initialize) and does what only Java
# can (screen capture, input injection; docs/guide/06-quickstart-android.md).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p "${1:-build/aar}"
out=$(cd "${1:-build/aar}" && pwd)
shift || true
abis=("$@")
if [ ${#abis[@]} -eq 0 ]; then
    abis=(arm64-v8a x86_64)
fi
version=$(sed -nE 's/^#define RCSDK_VERSION_(MAJOR|MINOR|PATCH) ([0-9]+)u$/\2/p' include/rcsdk/rcsdk.h | paste -sd. -)
label=$(sed -nE 's/^#define RCSDK_VERSION_LABEL "([a-z0-9]*)"$/\1/p' include/rcsdk/rcsdk.h) # Prefab takes numbers only.
api=29

cached() { sed -nE "s/^$1:[A-Z]+=(.*)$/\1/p" "$build/CMakeCache.txt"; } # The build's settings.
package="$out/package"
module="$package/prefab/modules/RemoteControl"
rm -rf "$package"
mkdir -p "$module/include/rcsdk" "$package/classes"
for abi in "${abis[@]}"; do
    build="$out/android-${abi%-v8a}"
    cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/android.cmake" -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="android-$api" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DRCSDK_BUILD_EXAMPLES=OFF \
        -DRCSDK_BUILD_TOOLS=OFF -DRCSDK_COMPONENTS= > /dev/null
    cmake --build "$build" > /dev/null
    ndk=$(sed -nE 's/^Pkg\.Revision *= *([0-9]+)\..*$/\1/p' "$(cached ANDROID_NDK)/source.properties")
    # One library: the core, BearSSL, the codec libraries built in and every
    # module, without the debug information the NDK's flags add (a host's
    # build strips its own).
    libraries="core bearssl"
    for codec in LIBVPX:libvpx DAV1D:dav1d LIBAOM:libaom OPENH264:openh264; do
        if [ "$(cached "RCSDK_WITH_${codec%%:*}")" = ON ]; then
            libraries="$libraries ${codec#*:}"
        fi
    done
    libs="$module/libs/android.$abi"
    mkdir -p "$libs"
    {
        echo "create $libs/libRemoteControl.a"
        for library in $libraries clipboard file_transfer power text_caret thumbnail; do
            echo "addlib $build/lib/librcsdk_$library.a"
        done
        echo "save"
        echo "end"
    } | "$(cached CMAKE_AR)" -M
    "$(cached CMAKE_STRIP)" --strip-debug "$libs/libRemoteControl.a"
    cat > "$libs/abi.json" << EOF
{"abi": "$abi", "api": $api, "ndk": $ndk, "stl": "c++_static", "static": true}
EOF
done
# The headers, as hosts include them (<rcsdk/...>).
cp include/rcsdk/*.h include/rcsdk/*.hpp modules/*/include/rcsdk/*.h "$module/include/rcsdk/"
cat > "$package/prefab/prefab.json" << EOF
{"schema_version": 2, "name": "RemoteControlSDK", "version": "$version", "dependencies": []}
EOF
cat > "$module/module.json" << EOF
{"library_name": "libRemoteControl", "export_libraries": ["-lmediandk", "-landroid", "-laaudio", "-lEGL", "-lGLESv3", "-llog"]}
EOF
cat > "$package/AndroidManifest.xml" << EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="local.rcsdk">
    <uses-sdk android:minSdkVersion="$api" />
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
    <uses-permission android:name="android.permission.ACCESS_WIFI_STATE" />
    <uses-permission android:name="android.permission.WAKE_LOCK" />
</manifest>
EOF
cp LICENSE THIRD_PARTY_NOTICES.md "$package/"
# No Java classes, but the archive an AAR must hold.
mkdir -p "$package/classes/META-INF"
printf 'Manifest-Version: 1.0\r\n\r\n' > "$package/classes/META-INF/MANIFEST.MF"
(cd "$package/classes" && zip -qrX ../classes.jar META-INF)
rm -rf "$package/classes"

aar="$out/RemoteControlSDK-$version${label:+-$label}.aar"
rm -f "$aar"
(cd "$package" && zip -qrX "$aar" .)
echo "$aar (SDK $version${label:+-$label}: ${abis[*]})"
