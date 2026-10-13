// Cryptography from the system (Security / CommonCrypto on macOS and iOS, CNG
// on Windows: crypto.cpp), or from BearSSL where the system has none
// (crypto_bearssl.cpp: Android, Linux; RCSDK_CRYPTO_BACKEND), its AES-CTR and
// SHA-256 on ARM64 the processor's instructions (bearssl_armv8.cpp). The SDK
// composes these primitives (composed.cpp).
#pragma once

#include "util/util.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace rcsdk::crypto {

using rcsdk::Bytes;
using Key = std::array<uint8_t, 32>;

void random(std::span<uint8_t> output);
Key sha256(std::span<const uint8_t> message);
Key hmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message);
// The same over the concatenation of `parts`, without joining them.
Key hmacSha256(std::span<const uint8_t> key, std::initializer_list<std::span<const uint8_t>> parts);
// RFC 8018 PBKDF2 with HMAC-SHA256, 32 bytes.
Key pbkdf2Sha256(std::string_view password, std::span<const uint8_t> salt, uint32_t iterations);
// For STUN message integrity (RFC 8489), which mandates HMAC-SHA1.
std::array<uint8_t, 20> hmacSha1(std::span<const uint8_t> key, std::span<const uint8_t> message);
// For a WebSocket's opening handshake (RFC 6455 Sec-WebSocket-Accept), which mandates SHA-1; no security rests on it.
std::array<uint8_t, 20> sha1(std::span<const uint8_t> message);
// For TURN long-term credentials (RFC 8489 key = MD5(username:realm:password)).
std::array<uint8_t, 16> md5(std::span<const uint8_t> message);
// RFC 5869 HKDF-SHA256.
Bytes hkdf(std::span<const uint8_t> key, std::span<const uint8_t> salt, std::span<const uint8_t> info, size_t length);
// Constant-time comparison.
bool equal(std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept;

// An ephemeral P-256 key pair for one key agreement.
class Ecdh {
public:
    static constexpr size_t publicKeySize = 65; // Uncompressed point: 0x04 || X || Y.

    Ecdh();
    ~Ecdh();
    Ecdh(const Ecdh&) = delete;
    Ecdh& operator=(const Ecdh&) = delete;

    const std::array<uint8_t, publicKeySize>& publicKey() const noexcept { return publicKey_; }
    // The shared X coordinate. Throws Failure(AUTHENTICATION) for a peer key
    // that is not a valid P-256 point.
    Key agree(std::span<const uint8_t> peerPublicKey) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::array<uint8_t, publicKeySize> publicKey_{};
};

// P-256 public key: 0x04 || X || Y; ECDSA signature: r || s (32 bytes each).
using PublicKey = std::array<uint8_t, 65>;
using Signature = std::array<uint8_t, 64>;

// Whether `key` is a P-256 point the system accepts.
bool validPublicKey(std::span<const uint8_t> key) noexcept;
// ECDSA P-256 over SHA-256(message).
bool verify(const PublicKey& key, std::span<const uint8_t> message, const Signature& signature);

// A long-term P-256 signing key: 0x04 || X || Y || d (SEC 1, 97 bytes), the
// public key followed by the private scalar.
class DeviceKey {
public:
    static constexpr size_t size = 97;
    static std::array<uint8_t, size> generate();

    // Throws Failure(INVALID_ARGUMENT) unless `key` is a P-256 key pair whose
    // halves belong together.
    explicit DeviceKey(std::span<const uint8_t> key);
    ~DeviceKey();
    DeviceKey(const DeviceKey&) = delete;
    DeviceKey& operator=(const DeviceKey&) = delete;

    const PublicKey& publicKey() const noexcept { return publicKey_; }
    // ECDSA P-256 over SHA-256(message).
    Signature sign(std::span<const uint8_t> message) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    PublicKey publicKey_{};
};

// AES-256 in counter mode: the 16-byte counter block starts at `iv` and
// increments as a big-endian 128-bit number. Encrypts and decrypts.
class AesCtr {
public:
    explicit AesCtr(const Key& key);
    ~AesCtr();
    AesCtr(const AesCtr&) = delete;
    AesCtr& operator=(const AesCtr&) = delete;
    void apply(std::span<const uint8_t, 16> iv, std::span<const uint8_t> input, std::span<uint8_t> output) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Authenticated encryption for records and datagrams: AES-256-CTR, then
// HMAC-SHA256 truncated to 128 bits over the sequence number, the associated
// data and the ciphertext. Each sequence number may seal one message per key.
class Aead {
public:
    static constexpr size_t tagSize = 16;

    Aead(const Key& cipherKey, const Key& macKey);
    // ciphertext || tag
    Bytes seal(uint64_t sequence, std::span<const uint8_t> associated, std::span<const uint8_t> plaintext) const;
    // The plaintext, or nothing when the tag does not verify.
    std::optional<Bytes> open(uint64_t sequence, std::span<const uint8_t> associated,
                              std::span<const uint8_t> sealed) const;

private:
    Key mac(uint64_t sequence, std::span<const uint8_t> associated, std::span<const uint8_t> ciphertext) const;
    AesCtr cipher_;
    Key macKey_;
};

} // namespace rcsdk::crypto
