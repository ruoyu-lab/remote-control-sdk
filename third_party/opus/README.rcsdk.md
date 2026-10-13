# libopus in this SDK

- Version: 1.6.1, from https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
  (SHA-256 `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`, as published in
  `SHA256SUMS.txt` there).
- Taken unchanged: `celt/`, `silk/`, `src/`, `include/`, the `*_sources.mk` / `*_headers.mk`
  source lists, `COPYING`, `AUTHORS`, `README`, `package_version`.
- Left out: `dnn/` (neural features: deep PLC, DRED, OSCE; about 22 MB of model data),
  `tests/` and the unit tests under `celt/` and `silk/`, `doc/`, the demo and compare
  programs, the Autotools, Meson and CMake build files, the ARMv7 assembly converter.
- Built by `third_party/CMakeLists.txt`: a float build without the DNN features, NEON on
  arm64 and SSE/SSE2 on x86-64, every symbol hidden and renamed with the prefix `rcsdk_`
  (`rcsdk_opus_names.h`, written by `rename-symbols.sh`; the only files added here).
- To update: replace these directories and files from a newer release, update the
  version and checksum here and in `THIRD_PARTY_NOTICES.md`, build for macOS and Windows,
  run `rename-symbols.sh` on both build trees, and run the `audio` test.
