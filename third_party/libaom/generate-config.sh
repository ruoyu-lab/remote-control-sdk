#!/bin/sh
# Writes config/<architecture>/ for each architecture the SDK builds libaom
# for: what libaom's own CMake generates (config/aom_config.h and .asm, the
# run-time CPU dispatch headers *_rtcd.h, aom_version.h, the configuration
# string aom_config.c) and the sources its build compiles, each with the
# instruction set it is built for (sources.txt: "path" or "path isa").
# CMakeLists.txt compiles from these alone, so building the SDK needs neither
# perl nor libaom's CMake. Run it after updating libaom, on a Mac, from the
# repository root, with the release's source tree:
#   third_party/libaom/generate-config.sh ~/Downloads/libaom-3.15.2
# NASM must be installed for the x86-64 configuration.
set -eu
source=$(cd "$1" && pwd)
out=$(cd "$(dirname "$0")" && pwd)/config
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The AV1 encoder alone, real-time only (the SDK never encodes in two passes
# or with lookahead), 10 bits (AV1 Main and High at 10 bits), threads,
# run-time CPU detection (x86-64: SSE2 to AVX2; arm64: NEON, dot product,
# int8 matrix multiply, CRC32), SVE left out (no compiler of every target
# takes it, and no CPU the SDK is tested on has it), nothing but the library.
common="-DCONFIG_AV1_DECODER=0 -DCONFIG_REALTIME_ONLY=1 -DCONFIG_AV1_HIGHBITDEPTH=1 -DCONFIG_MULTITHREAD=1
        -DCONFIG_RUNTIME_CPU_DETECT=1 -DCONFIG_PIC=1 -DCONFIG_DENOISE=0 -DCONFIG_LIBYUV=0 -DCONFIG_WEBM_IO=0
        -DENABLE_DOCS=0 -DENABLE_EXAMPLES=0 -DENABLE_TESTS=0 -DENABLE_TOOLS=0 -DENABLE_APPS=0
        -DENABLE_SVE=0 -DENABLE_SVE2=0"

generate() { # name, extra options
    name=$1
    shift
    mkdir -p "$work/$name"
    cmake -S "$source" -B "$work/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        $common "$@" > "$work/$name/configure.log" 2>&1 || { cat "$work/$name/configure.log"; exit 1; }
    rm -rf "${out:?}/$name"
    mkdir -p "$out/$name/config"
    generated="$work/$name/config"
    # pthread.h everywhere but Windows, where libaom then uses Windows' own
    # threads (MSVC has no pthread.h; MinGW's is not wanted either).
    grep -q '^#define HAVE_PTHREAD_H 1$' "$generated/aom_config.h"
    awk '$0 == "#define HAVE_PTHREAD_H 1" { print "#ifdef _WIN32"; print "#define HAVE_PTHREAD_H 0"; print "#else";
                                           print; print "#endif"; next } { print }' \
        "$generated/aom_config.h" > "$out/$name/config/aom_config.h"
    cp "$generated"/*_rtcd.h "$generated/aom_version.h" "$out/$name/config/"
    if grep -q '^#define AOM_ARCH_X86_64 1$' "$generated/aom_config.h"; then
        cp "$generated/aom_config.asm" "$out/$name/config/"
    fi
    # The configuration string, without the paths of this machine.
    sed -e 's|"cmake [^ ]* -G \\"Ninja\\" |"|' "$generated/aom_config.c" > "$out/$name/config/aom_config.c"
    # The library's sources (not the rate control library, CMake's empty
    # placeholders or the configuration string), with the instruction set
    # of the object library libaom builds each in.
    python3 -I - "$work/$name/compile_commands.json" "$source" > "$out/$name/sources.txt" <<'EOF'
import json, re, sys
commands, root = json.load(open(sys.argv[1])), sys.argv[2] + "/"
lines = set()
for c in commands:
    target = re.search(r"CMakeFiles/([^/]+)\.dir/", c["output"]).group(1)
    if not c["file"].startswith(root) or target == "aom_av1_rc":
        continue
    isa = re.fullmatch(r"aom_(?:dsp|av1)_(?:common|encoder)_(.+?)(?:_x86)?_intrinsics", target)
    isa = isa and {"sse4": "sse4_1", "sse42": "sse4_2", "neon": None}.get(isa.group(1), isa.group(1))
    if isa not in (None, "sse2", "sse3", "ssse3", "sse4_1", "sse4_2", "avx", "avx2", "avx512", "neon_dotprod",
                   "neon_i8mm", "arm_crc32"):
        sys.exit(f"{target}: an instruction set CMakeLists.txt does not know")
    lines.add(c["file"][len(root):] + (" " + isa if isa else ""))
print("\n".join(sorted(lines)))
EOF
    echo "$name: $(wc -l < "$out/$name/sources.txt" | tr -d ' ') sources"
}

# One configuration per architecture: libaom's are the same on every system
# there but for pthread.h (above).
generate arm64 -DAOM_TARGET_CPU=arm64
generate x86_64 -DAOM_TARGET_CPU=x86_64 -DCMAKE_OSX_ARCHITECTURES=x86_64 -DENABLE_NASM=1
generate generic -DAOM_TARGET_CPU=generic
