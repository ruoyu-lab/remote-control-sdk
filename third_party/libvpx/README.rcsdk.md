# libvpx in this SDK

- Version: 1.16.0, from https://github.com/webmproject/libvpx/archive/refs/tags/v1.16.0.tar.gz
  (SHA-256 `7a479a3c66b9f5d5542a4c6a1b7d3768a983b1e5c14c60a9396edc9b649e015c`, 5 635 379 bytes).
- Taken: `vpx/`, `vpx_dsp/`, `vpx_mem/`, `vpx_ports/`, `vpx_scale/`, `vpx_util/`, `vp8/`,
  `vp9/`, `third_party/x86inc/` (x264's x86 assembly macros, ISC), `LICENSE`, `PATENTS`,
  `AUTHORS`, `README`, unchanged; only the C and NASM sources some configuration below
  compiles (and every header).
- Left out: the MIPS, LoongArch and PowerPC code, the other sources no configuration
  compiles (two-pass and good-quality encoding, the decoder's error concealment, debug
  modes), `test/`, `examples/`, `tools/`, the other `third_party/` libraries (googletest,
  libwebm, libyuv), the documentation, and the build system (`configure`, `build/`, the
  `*.mk` lists, the `*_rtcd_defs.pl` scripts, the export lists).
- Added: `config/` (written by `generate-config.sh` from libvpx's own `configure`, one
  configuration per architecture: `arm64` for macOS, iOS, Android and Linux, `x86_64`
  likewise, `windows-arm64`, `windows-x86_64`, and `generic`, the portable C; each with
  `vpx_config.h` (and `.asm` on x86-64), the run-time CPU dispatch headers and
  `sources.txt`), `CMakeLists.txt`, and `rcsdk_libvpx_names.h` / `.asm` (every global
  symbol renamed with the prefix `rcsdk_`, written by `third_party/rename-symbols.sh`).
- Configured for: real-time encoding only, VP9's 10 and 12 bits, multithreading, run-time
  CPU detection (x86-64: SSE2 to AVX-512, with NASM; arm64: NEON, dot product, int8 matrix
  multiply; SVE left out), nothing but the library.
- To update: replace these directories and files from a newer release, run
  `generate-config.sh` on its source tree (NASM installed), build for macOS, Windows
  (LLVM-MinGW x64 and arm64) and Android, run `third_party/rename-symbols.sh libvpx` on
  those build trees, update the version and checksum here and in `THIRD_PARTY_NOTICES.md`,
  and run the `codec_roundtrip` and `vpx` tests.
