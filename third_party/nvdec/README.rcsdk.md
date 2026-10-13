# NVDEC interface headers

The declarations of NVIDIA's hardware video decoder interface (NVDEC, also
called CUVID) and of the part of the CUDA driver interface it needs. Headers
only: nothing is linked; at run time the SDK loads `nvcuvid.dll` and
`nvcuda.dll`, which the NVIDIA display driver installs, and resolves their
functions by name (`src/platform/windows/nvdec.cpp`).

- Source: FFmpeg's nv-codec-headers, tag `n13.0.19.1`
  (https://github.com/FFmpeg/nv-codec-headers/tree/n13.0.19.1/include/ffnvcodec),
  the same tag as `third_party/nvenc/nvEncodeAPI.h`. Archive
  `nv-codec-headers-n13.0.19.1.tar.gz` from
  https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/n13.0.19.1.tar.gz,
  SHA-256 `c293161bd92dac53f2f8fab08abcd4f96773c3644a57b23c2260b4be2311ab2f`.
- Files, each unchanged from the archive:
  - `dynlink_cuviddec.h` (NVIDIA's `cuviddec.h` of Video Codec SDK 13.0.19 as
    FFmpeg publishes it: function pointer types instead of prototypes),
    SHA-256 `1244165ea5521d4be865995eb1c49b8910f755d0ca35004e1c4010ef7f612143`
  - `dynlink_nvcuvid.h` (NVIDIA's `nvcuvid.h`, the parser, likewise),
    SHA-256 `2141dade75f3178520092dce2c02df9b47c4b19036b56bb9316bd83a07bf16f1`
  - `dynlink_cuda.h` (FFmpeg's subset of the CUDA driver interface),
    SHA-256 `c970d5817120ea481ba29b9c603bdc9a386985d9fc98a47f6588030dcdba87c1`
- Left out: `dynlink_loader.h` (FFmpeg's loader; the SDK resolves the
  functions itself) and `nvEncodeAPI.h` (in `third_party/nvenc`).
- The one function the SDK needs that these headers do not declare,
  `cuGraphicsD3D11RegisterResource` (the CUDA toolkit's `cudaD3D11.h`), is
  declared in `nvdec.cpp` by its documented signature.
- Driver: the interface has been stable since Video Codec SDK 8; the SDK
  needs `cuDeviceGetLuid` (CUDA 10 drivers) and a GPU that decodes 4:4:4
  (HEVC: Turing, GeForce RTX 20, and later). Verified with driver 616.92 on
  an RTX 4090.

To update: download the archive of the tag matching `third_party/nvenc`,
compare the three files, replace them, and update the checksums here and in
`THIRD_PARTY_NOTICES.md`.

License: MIT (in each file's own header, and in `THIRD_PARTY_NOTICES.md`).
