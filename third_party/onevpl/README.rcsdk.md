# oneVPL interface headers

The declarations of Intel's Video Processing Library (oneVPL, now "Intel
VPL"): constants, structures and functions. Headers only: nothing is linked
and no dispatcher is built in; at run time the SDK loads the runtime the
Intel graphics driver installs (`libmfx64-gen.dll`, in the driver's store,
from the directory the display adapter's `DriverStorePathForVPL` registry
value names, where oneVPL's own dispatcher looks first) and resolves its
functions by name (`src/platform/windows/onevpl.cpp`).

- Source: Intel's libvpl, tag `v2.17.0` (API 2.17), released 2026-06-25.
  Archive `libvpl-v2.17.0.tar.gz` from
  https://github.com/intel/libvpl/archive/refs/tags/v2.17.0.tar.gz,
  SHA-256 `4de3e2faf1e8307fb282e4a43f443191810f6a6b0a484fffa7995ba1c814c6ec`.
- Files, each unchanged from the archive's `api/vpl/` (with their SHA-256):
  - `mfxcommon.h` `c290e6ae15f35436097f0b605c02566fa67094cf449d090520d23c7e3fa194a8`
  - `mfxdefs.h` `9cdfeca6651e3d994d068389e30c6235c6970e018be98d2bacc89202175775ff`
  - `mfximplcaps.h` `afcc8e46ffc67655b4374ef4d3bd4f4c91478dca2f7cba790e7a3cf6bea9d5a3`
  - `mfxmemory.h` `2d751e9f590d536f0c59452d1d7323fe2a801402a9bd8a7725c89fcf5dcd7cf1`
  - `mfxsession.h` `e141900442754d11806d199ee29ad065b7e7677044eb9b3f496cb10206c49926`
  - `mfxstructures.h` `1a1fd6c0bc3a406cb41ffb1f079cad1304bdbd4c8ed43127a3f9d9be8308c305`
  - `mfxvideo.h` `571f6607e1ad16bfacd16733b2188acea40f4837b7df336a7bf85ade059d17d2`
  - `LICENSE` (the repository's) `bf1cfac2e2792b6e1e995ce103d70796aecaf2ec7e4c5fe5474f7acec7b4a677`
- Left out: the other API headers (none of their declarations is used), the
  dispatcher (`libvpl/`: the SDK finds the runtime the way the dispatcher's
  low-latency path does, in a few dozen lines), examples, tools and
  documentation. The
  experimental API (`ONEVPL_EXPERIMENTAL`) is not used.
- Runtime: the Intel graphics driver's oneVPL runtime, which Intel ships for
  Gen12 (Tiger Lake) and later GPUs; older GPUs have only the Media SDK
  runtime, which the SDK does not load (their encoders and decoders stay
  with Media Foundation). Verified with driver 32.0.101.8860 (runtime
  26.05.06) on the integrated GPU of a Core Ultra 7 270K Plus, encoding and
  decoding.

To update: download the archive of a newer tag, compare the files, replace
them, and update the checksums here and in `THIRD_PARTY_NOTICES.md`. Newer
headers keep older runtimes working (oneVPL keeps its interface
compatible within version 2).

License: MIT (`LICENSE`, and in `THIRD_PARTY_NOTICES.md`).
