# OpenH264 in this SDK

- Version: 2.6.0, from https://github.com/cisco/openh264/archive/refs/tags/v2.6.0.tar.gz
  (SHA-256 `558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69`, 60 302 243
  bytes).
- Built by default on Linux only (`RCSDK_WITH_OPENH264`): the other systems have a software
  H.264 codec of their own; a build may turn it on anywhere.
- Patents: Cisco pays the H.264 royalties only for its own binaries that a user downloads
  separately; a build from these sources carries no such license (docs/design/CODECS-DESIGN.md).
- Taken: `codec/api/wels/`, and of `codec/common/`, `codec/processing/`, `codec/encoder/` and
  `codec/decoder/` the C++ sources and headers and the x86-64 (NASM) and arm64 assembly;
  `LICENSE`, `README.md`, `CONTRIBUTORS`.
- Left out: the 32-bit ARM, MIPS and LoongArch code, `codec/console/` (the command-line
  programs), `codec/build/`, `test/`, `testbin/`, `res/`, `module/` (the Gecko plugin),
  `autotest/`, the Windows export lists and resources (`*.def`, `*.rc`, `DllEntry.cpp`), and
  the build systems (`Makefile`, `build/`, `meson.build`, Gradle).
- Added: `CMakeLists.txt` (what `meson.build` does for the library) and
  `rcsdk_openh264_names.h` / `.asm` (every global symbol renamed with the prefix `rcsdk_`:
  the C functions and assembly by name, the C++ namespaces and classes by their outermost
  name; written by `third_party/rename-symbols.sh`).
- Changed:
  - `codec/common/x86/asm_inc.asm` (`WELS_EXTERN`) and `codec/common/x86/dct.asm` (a jump to
    another function's local label, now `JMP_LOCAL`): names go through the renaming before
    Mach-O's underscore or a local label is joined to them.
  - `codec/common/arm64/arm_arch64_common_macro.S`: the assembly functions are hidden
    (`.hidden`, `.private_extern`), as the C++ code is, so a library linking the SDK does not
    export them.
  - `codec/decoder/plus/src/welsDecoderExt.cpp` (`ReorderPicturesInDisplay`): a stream whose
    SPS says it reorders no pictures (`max_num_reorder_frames` 0, which the SDK's H.264 says)
    is shown as each picture is decoded, as Baseline is, instead of a picture later.
- To update: replace these files from a newer release, compare its `meson.build` files with
  `CMakeLists.txt`, reapply the changes above, build for macOS, Windows (LLVM-MinGW x64) and
  Android with `RCSDK_WITH_OPENH264=ON`, run `third_party/rename-symbols.sh openh264` on those
  build trees, update the version and checksum here and in `THIRD_PARTY_NOTICES.md`, and run
  the `codec_roundtrip` test in such a build.
