# Monocypher in this SDK

- Version: 4.0.3, from https://monocypher.org/download/monocypher-4.0.3.tar.gz
  (940 390 bytes, SHA-512
  `40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965`,
  as published in `monocypher-4.0.3.tar.gz.sha512` there).
- License: BSD 2-Clause or CC0 1.0, at the user's choice (`LICENCE.md`); the SDK takes the
  BSD licence (`THIRD_PARTY_NOTICES.md`).
- Taken unchanged (SHA-256):
  - `src/monocypher.c` `57eb914fc88136119bd41655cccb8c250048bf54d470540625186f8ab16f64be`
  - `src/monocypher.h` `c494da712122da7ff679fdcf318a5317e84972b6c950fe9d896212947797facd`
  - `src/optional/monocypher-ed25519.c` `60fce3578fb00b00da96490653d993c4cb427b1e1be38183285c66e04d22cc18`
    (SHA-512 lives there)
  - `src/optional/monocypher-ed25519.h` `abc4fad381879f5c29176ebe014b9189956b3dfe0a3e36459b6990bc57212380`
  - `LICENCE.md` `5f8360e4c06ddcc584bdb4b210c6af824c4bb301e6a9a521869b6d90795ca4b3`
- Left out: the tests, the documentation, the makefile and pkg-config file, the other optional
  sources.
- Used by `src/crypto/cpace.cpp` (CPace on X25519 for `RCS_SECRET_PIN`: `crypto_x25519`,
  `crypto_elligator_map`, `crypto_sha512*`).
- Built by `third_party/CMakeLists.txt`: every symbol hidden and renamed with the prefix
  `rcsdk_` (`rcsdk_monocypher_names.h`, written by `rename-symbols.sh`; the only files added
  here), so a host may link its own Monocypher.
- To update: replace these files from a newer release, update the version and checksums here
  and in `THIRD_PARTY_NOTICES.md`, build for macOS and Windows, run `rename-symbols.sh` on a
  build tree, and run the `crypto` and `secure_channel` tests (the CPace test vectors).
