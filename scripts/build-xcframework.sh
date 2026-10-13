#!/bin/bash
# Builds RemoteControl.xcframework for iOS hosts: the core, the codec
# libraries built in and the official modules as one static library for
# devices (arm64) and the simulator (arm64, x86_64), with the C headers and a
# module map, so that Swift imports them (import RemoteControlSDK) and
# Objective-C or C++ include <rcsdk/rcsdk.h>.
#
#   scripts/build-xcframework.sh [OUTPUT_DIR]     (default: build/xcframework)
#
# The host links the frameworks the library uses: Foundation, UIKit,
# CoreVideo, CoreMedia, VideoToolbox, Security, Network, AudioToolbox,
# AVFoundation, ImageIO, CoreGraphics; and libc++.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/xcframework}"
version=$(sed -nE 's/^#define RCSDK_VERSION_(MAJOR|MINOR|PATCH) ([0-9]+)u$/\2/p' include/rcsdk/rcsdk.h | paste -sd. -)
label=$(sed -nE 's/^#define RCSDK_VERSION_LABEL "([a-z0-9]*)"$/\1/p' include/rcsdk/rcsdk.h)
version="$version${label:+-$label}"

build() { # $1: sysroot (iphoneos, iphonesimulator), $2: architectures
    local dir="$out/$1"
    cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$1" -DCMAKE_OSX_ARCHITECTURES="$2" -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF -DRCSDK_BUILD_EXAMPLES=OFF -DRCSDK_BUILD_TOOLS=OFF -DRCSDK_COMPONENTS= > /dev/null
    cmake --build "$dir" > /dev/null
    # One library: the core, the codec libraries built in and every module.
    local codecs=()
    for codec in LIBVPX:libvpx DAV1D:dav1d LIBAOM:libaom OPENH264:openh264; do
        if grep -q "^RCSDK_WITH_${codec%%:*}:BOOL=ON$" "$dir/CMakeCache.txt"; then
            codecs+=("$dir/lib/librcsdk_${codec#*:}.a")
        fi
    done
    libtool -static -o "$dir/libRemoteControl.a" "$dir/lib/librcsdk_core.a" "${codecs[@]}" \
        "$dir"/lib/librcsdk_{clipboard,file_transfer,power,text_caret,thumbnail}.a 2> /dev/null
    # The headers, as hosts include them (<rcsdk/...>), and the module map.
    rm -rf "$dir/Headers" && mkdir -p "$dir/Headers/rcsdk"
    cp include/rcsdk/*.h include/rcsdk/*.hpp modules/*/include/rcsdk/*.h "$dir/Headers/rcsdk/"
    {
        echo "// The C interface of the Remote Control SDK $version for Swift: import RemoteControlSDK."
        echo "module RemoteControlSDK {"
        for header in "$dir"/Headers/rcsdk/*.h; do
            echo "    header \"rcsdk/$(basename "$header")\""
        done
        echo "    link \"c++\""
        echo "    export *"
        echo "}"
    } > "$dir/Headers/module.modulemap"
}

build iphoneos arm64
build iphonesimulator "arm64;x86_64"
rm -rf "$out/RemoteControl.xcframework"
xcodebuild -create-xcframework \
    -library "$out/iphoneos/libRemoteControl.a" -headers "$out/iphoneos/Headers" \
    -library "$out/iphonesimulator/libRemoteControl.a" -headers "$out/iphonesimulator/Headers" \
    -output "$out/RemoteControl.xcframework" > /dev/null
cp LICENSE THIRD_PARTY_NOTICES.md "$out/RemoteControl.xcframework/" # Hosts ship both with their app.
echo "$out/RemoteControl.xcframework (SDK $version)"
