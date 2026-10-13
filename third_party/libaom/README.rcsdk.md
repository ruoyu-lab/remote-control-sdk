# libaom in this SDK

- Version: 3.15.2, from https://storage.googleapis.com/aom-releases/libaom-3.15.2.tar.gz
  (SHA-256 `67bb54b245f33ed98600e08269e6139986e48114e14042474ddd8885801dfddc`, 6 629 974
  bytes; the release publishes a PGP signature, not a checksum).
- Taken: `aom/`, `aom_dsp/`, `aom_mem/`, `aom_ports/`, `aom_scale/`, `aom_util/`, `av1/`,
  `common/`, and from `third_party/`: `fastfeat/` (BSD 3-Clause), `vector/` (MIT), `x86inc/`
  (x264's x86 assembly macros, ISC); `LICENSE`, `PATENTS`, `AUTHORS`, `README.md`, unchanged;
  only the C and NASM sources some configuration below compiles (and every header), and
  `aom_ports/float.asm`, which libaom's CMake adds for Windows x86-64 alone.
- Left out: the decoder's sources, every encoder source that only two-pass, good-quality or
  all-intra encoding uses, the RISC-V code, the Highway (C++) SIMD, `apps/`, `examples/`,
  `test/`, `tools/`, `stats/`, the documentation, the other `third_party/` libraries
  (googletest, highway, libwebm, libyuv) and the build system (`CMakeLists.txt`, the `*.cmake`
  lists, the `*_rtcd_defs.pl` scripts, the export lists).
- Added: `config/` (written by `generate-config.sh` with libaom's own CMake, one configuration
  per architecture: `arm64` for macOS, iOS, Android, Linux and Windows, `x86_64` likewise, and
  `generic`, the portable C; each with `config/aom_config.h` (and `.asm` on x86-64), the
  run-time CPU dispatch headers, `aom_version.h`, `aom_config.c` and `sources.txt`, which names
  each source's instruction set), `CMakeLists.txt`, and `rcsdk_libaom_names.h` / `.asm` (every
  global symbol renamed with the prefix `rcsdk_`, written by `third_party/rename-symbols.sh`).
- Configured for: the AV1 encoder alone, real-time encoding only, 8 and 10 bits, threads,
  run-time CPU detection (x86-64: SSE2 to AVX2, with NASM; arm64: NEON, dot product, int8
  matrix multiply, CRC32; SVE left out), no denoiser, no libyuv, no WebM.
- To update: replace these directories and files from a newer release, run
  `generate-config.sh` on its source tree (on a Mac, NASM installed), remove the sources no
  `config/*/sources.txt` names (keep the `.asm` files others `%include`), build for macOS,
  Windows (LLVM-MinGW x64) and Android, run `third_party/rename-symbols.sh libaom` on those
  build trees, update the version and checksum here and in `THIRD_PARTY_NOTICES.md`, and run
  the `codec_roundtrip` test.
