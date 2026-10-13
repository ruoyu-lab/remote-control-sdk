# BearSSL in this SDK

- Version: 0.6, from https://www.bearssl.org/bearssl-0.6.tar.gz (765 094 bytes, SHA-256
  `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`, computed on download;
  the site publishes a PGP signature, not a checksum).
- License: MIT (`LICENSE.txt`, `THIRD_PARTY_NOTICES.md`).
- Taken unchanged: `src/` (every `.c` file, `config.h`, `inner.h`) and `inc/`, `LICENSE.txt`.
- Left out: the `.t0` sources of the generated handshake, X.509 and PEM code (their `.c` output
  is in `src/`) and the T0 compiler, `tools/` (`brssl`), `test/`, `samples/`, the build files
  (`Makefile`, `mk/`, `conf/`), `Doxyfile`, `README.txt`.
- Used only with `RCSDK_CRYPTO_BACKEND=bearssl` (the default where the system has no
  cryptography: Android, Linux): `src/crypto/crypto_bearssl.cpp` (SHA-256, HMAC, PBKDF2, MD5,
  P-256 with `br_ec_p256_m31`, ECDSA with `br_ecdsa_i31_*`, AES-CTR with AES-NI where the
  processor has it, else `br_aes_ct64`) and `src/transport/tls_bearssl.cpp` (the TLS 1.2 client
  for TURN over TLS, X.509 checks, PEM). On ARM64, where BearSSL has only software, AES-CTR and
  SHA-256 are the SDK's own classes on the ARMv8 Cryptography Extension
  (`src/crypto/bearssl_armv8.cpp`: a `br_block_ctr_class` and a `br_hash_class` on
  `br_sha256_context`), where the processor has the instructions.
- Built by `third_party/CMakeLists.txt`: every source with BearSSL's own defaults
  (`src/config.h`), a static library the SDK links (only what it calls goes in), every symbol
  hidden and renamed with the prefix `rcsdk_` (`rcsdk_bearssl_names.h`, written by
  `rename-symbols.sh`; the only files added here).
- To update: replace `src/` and `inc/` from a newer release (without the `.t0` files), update
  the version and checksum here and in `THIRD_PARTY_NOTICES.md`, build with
  `-DRCSDK_CRYPTO_BACKEND=bearssl` for arm64 and x86-64, run `rename-symbols.sh` on both build
  trees, and run the tests of that build (`crypto`, `crypto_armv8`, `stun`, `tls`, `tls_relay`,
  `secure_channel` and the sessions).
