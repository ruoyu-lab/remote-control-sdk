# dav1d in this SDK

- Version: 1.5.4, from https://download.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz
  (SHA-256 `686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5`, as the release's
  `.sha256` says; 1 038 852 bytes).
- Taken: `include/dav1d/`, `include/common/`, `include/compat/msvc/` (`stdatomic.h` for MSVC),
  `src/` with its `arm/` (the arm64 assembly), `x86/`, `ext/x86/` (x264's x86 assembly macros,
  ISC) and `win32/` (the thread layer on Windows), `COPYING`, `README.md`, `THANKS.md`.
- Left out: the 32-bit ARM, LoongArch, PowerPC and RISC-V code, `tools/`, `tests/`,
  `examples/`, `doc/`, `package/`, the other compatibility headers, and the build system
  (`meson.build`, `meson_options.txt`, the `.in` templates, the Windows resource file).
- Added: `CMakeLists.txt` (what `meson.build` does for the library: the checks for `config.h`,
  both bit depths, the assembly), `config.h.in` and `config.asm.in` (its `config.h` and
  `config.asm`), `include/vcs_version.h` (the version `dav1d_version()` returns), and
  `rcsdk_dav1d_names.h` (every global symbol renamed with the prefix `rcsdk_`, written by
  `third_party/rename-symbols.sh`; the assembly's own prefix is `rcsdk_dav1d_`).
- Changed: four macros that redefine a function as itself (`dav1d_log` in `src/log.h`,
  `dav1d_ref_create` in `src/ref.h`, `dav1d_mem_pool_init` in `src/mem.h`,
  `dav1d_set_thread_name` on Windows in `src/thread.h` and `src/win32/thread.c`) now name the
  function differently from the macro (`..._untracked`, `..._w`), or are gone (`dav1d_log`), so
  that the renaming header reaches those functions too.
- Configured for: 8- and 10-bit pictures, logging (the decoder's messages become the reason of
  a failure), run-time CPU detection (arm64: NEON, dot product, int8 matrix multiply, SVE and
  SVE2, as far as the assembler knows them; x86-64: SSE2 to AVX-512, with NASM), the C
  functions the assembly always replaces left out.
- To update: replace these files from a newer release, compare its `meson.build` and
  `src/meson.build` (the source lists, `config.h`'s variables) with `CMakeLists.txt` and
  `config.h.in`, reapply the change above, build for macOS, Windows (LLVM-MinGW x64) and
  Android, run `third_party/rename-symbols.sh dav1d` on those build trees, update the version and
  checksum here, in `include/vcs_version.h` and in `THIRD_PARTY_NOTICES.md`, and run the
  `codec_roundtrip` test.
