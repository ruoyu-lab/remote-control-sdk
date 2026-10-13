#!/bin/sh
# Writes config/<target>/ for each architecture the SDK builds libvpx for: what
# libvpx's own configure generates there (vpx_config.h and .asm, the
# run-time CPU dispatch headers *_rtcd.h, the configuration string) and the
# list of sources its build would compile (sources.txt), plus vpx_version.h
# in config/. third_party/CMakeLists.txt compiles from these alone, so
# building the SDK needs neither perl nor libvpx's configure. Run it after
# updating libvpx, from the repository root, with the release's source tree:
#   third_party/libvpx/generate-config.sh ~/Downloads/libvpx-1.16.0
# NASM must be installed for the x86-64 targets.
set -eu
source=$(cd "$1" && pwd)
out=$(cd "$(dirname "$0")" && pwd)/config
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Real-time only (the SDK never encodes in two passes or with lookahead), 10
# and 12 bits (VP9 profiles 2 and 3), run-time CPU detection where the
# architecture has it (x86-64: SSE2 to AVX-512; arm64: dot product and int8
# matrix multiply over NEON), SVE left out (no compiler of every target
# takes it, and it gains little over NEON here), nothing but the library.
common="--enable-external-build --enable-realtime-only --enable-vp9-highbitdepth --enable-multithread
        --enable-pic --disable-examples --disable-tools --disable-docs --disable-unit-tests
        --disable-install-docs --disable-install-bins --disable-install-libs --disable-install-srcs
        --disable-webm-io --disable-libyuv --disable-debug --disable-gprof --disable-gcov"

generate() { # name, configure target, extra options
    name=$1 target=$2
    shift 2
    mkdir -p "$work/$name" "$out/$name"
    (cd "$work/$name" && "$source/configure" --target="$target" $common "$@" > configure.log 2>&1) ||
        { cat "$work/$name/configure.log"; exit 1; }
    asm=$(grep -q 'VPX_ARCH_X86_64 1' "$work/$name/vpx_config.h" && echo vpx_config.asm || true)
    (cd "$work/$name" && make libvpx_srcs.txt vpx_version.h vp8_rtcd.h vp9_rtcd.h vpx_dsp_rtcd.h \
        vpx_scale_rtcd.h $asm > make.log 2>&1) || { cat "$work/$name/make.log"; exit 1; }
    rm -f "$out/$name"/*
    cp "$work/$name"/vpx_config.h "$work/$name"/vpx_config.c "$work/$name"/*_rtcd.h "$out/$name/"
    if [ -f "$work/$name/vpx_config.asm" ]; then cp "$work/$name/vpx_config.asm" "$out/$name/"; fi
    # The compiled sources: C and NASM, not headers, the version script, or the build files listed with them.
    grep -E '\.(c|asm)$' "$work/$name/libvpx_srcs.txt" | grep -v '^vpx_config\.c$' > "$out/$name/sources.txt"
    cp "$work/$name/vpx_version.h" "$out/"
    echo "$name: $(wc -l < "$out/$name/sources.txt" | tr -d ' ') sources"
}

# One configuration per architecture: libvpx's are the same on macOS, iOS,
# Android and Linux there (the configure line aside); Windows' differ (no
# pthread.h, MSVC's inline).
runtime="--enable-runtime-cpu-detect"
generate arm64 arm64-linux-gcc $runtime --disable-sve --disable-sve2
# x86-64: configure's NASM check assembles a file, which an external build
# skips, then reads it; YASM's name skips the check (the generated files are
# the same, and NASM builds them).
generate x86_64 x86_64-linux-gcc $runtime --as=yasm
generate windows-arm64 arm64-win64-vs17 $runtime --disable-sve --disable-sve2
generate windows-x86_64 x86_64-win64-vs17 $runtime --as=nasm
generate generic generic-gnu
