// AES-CTR and SHA-256 on the ARMv8 Cryptography Extension (AESE / AESMC,
// SHA256H / SHA256H2 / SHA256SU0 / SHA256SU1), as BearSSL classes, for the
// BearSSL backend (crypto_bearssl.cpp) on ARM64, where BearSSL has only its
// constant-time software. Constant time too: no table lookups, no branches on
// keys or data.
#pragma once

// clang-format off
#include "bearssl/rcsdk_bearssl_names.h" // First: the declarations below name the SDK's own copy.
#include <bearssl.h>
// clang-format on

namespace rcsdk::crypto::armv8 {

// AES-CTR with 128-, 192- and 256-bit keys, as br_aes_x86ni_ctr_vtable: its
// contexts fit in a br_aes_gen_ctr_keys. Null where the processor has no AES
// instructions (or is not ARM64).
const br_block_ctr_class* aesCtr() noexcept;

// SHA-256 as br_sha256_vtable, on br_sha256_context (its state and count
// compatible with BearSSL's own: br_hmac and the like take it). Null where
// the processor has no SHA-256 instructions (or is not ARM64).
const br_hash_class* sha256() noexcept;

} // namespace rcsdk::crypto::armv8
