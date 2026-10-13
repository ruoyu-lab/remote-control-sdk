# NVENC interface headers

`nvEncodeAPI.h` declares the interface of NVIDIA's hardware video encoder
(NVENC): constants, structures and the function table. It is a header only:
nothing is linked; at run time the SDK loads `nvEncodeAPI64.dll`, which the
NVIDIA display driver installs, and asks it for the function table.

Two versions are kept, each unchanged from its source:

- `nvEncodeAPI.h`: Video Codec SDK 13.0.
  - Source: FFmpeg's nv-codec-headers, tag `n13.0.19.1`
    (https://github.com/FFmpeg/nv-codec-headers/blob/n13.0.19.1/include/ffnvcodec/nvEncodeAPI.h),
    NVIDIA's header as published with Video Codec SDK 13.0.19.
  - SHA-256: `4fe4094541ef0f8a13249d97a8692dc5f835a6e9dd42eeadb3e2f7321d54dc7e`
  - Interface 13.0 needs an NVIDIA driver of 570 or later (Windows and
    Linux; nv-codec-headers' README and NVIDIA's SDK 13.0 Read Me). It adds
    the temporal layers (temporal SVC) of HEVC and AV1 the SDK uses.
- `12.0/nvEncodeAPI.h`: Video Codec SDK 12.0.
  - Source: FFmpeg's nv-codec-headers, tag `n12.0.16.1`
    (https://github.com/FFmpeg/nv-codec-headers/blob/n12.0.16.1/include/ffnvcodec/nvEncodeAPI.h),
    NVIDIA's header as published with Video Codec SDK 12.0.16.
  - SHA-256: `808db2a21232839ee8a6057601f1964e90b20385f6dbf080f3cd33f6470c66c4`
  - Interface 12.0 needs an NVIDIA driver of 522.25 or later on Windows
    (nv-codec-headers' README).

Why both: a driver speaks every interface version up to the highest it
reports (`NvEncodeAPIGetMaxSupportedVersion`), and the versions lay out their
structures differently (13.0 moved the HEVC and AV1 bit depths to fields of
their own and gave their old bits to other flags; `NV_ENC_INITIALIZE_PARAMS`
and `NV_ENC_LOCK_BITSTREAM` changed too), so one header cannot speak to a
driver of the other. The SDK compiles its NVENC encoder once against each
(`src/platform/windows/nvenc_api13.cpp`, `nvenc_api12.cpp`) and uses the
newest the driver speaks: drivers from 522.25 to before 570 keep NVENC,
without temporal layers in HEVC and AV1. Older drivers report a lower
version and the SDK does not offer NVENC there.

License: MIT (in each file's own header, and in `THIRD_PARTY_NOTICES.md`).
