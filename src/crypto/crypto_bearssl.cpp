// crypto.hpp from BearSSL (RCSDK_CRYPTO_BACKEND bearssl: Android, Linux),
// with its constant-time code throughout: P-256 in 31-bit limbs, ECDSA "i31"
// (deterministic signatures, RFC 6979), AES bitsliced or with AES-NI. On
// ARM64, AES-CTR and SHA-256 are the processor's instructions where it has
// them (bearssl_armv8.cpp). The randomness is the system's.
#include "crypto/crypto.hpp"

#include "crypto/bearssl_armv8.hpp" // BearSSL, the SDK's own copy.
#include "util/bytes.hpp"
#include "util/log.hpp"
#include "util/util.hpp"

#include <algorithm>
#include <cstdlib>
#ifdef __EMSCRIPTEN__
#include <unistd.h>
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#include <cerrno>
#include <sys/random.h>
#endif

namespace rcsdk::crypto {
namespace {

using Scalar = std::array<uint8_t, 32>;

constexpr int curve = BR_EC_secp256r1;
const br_ec_impl& p256 = br_ec_p256_m31;

[[noreturn]] void cryptoFailure() {
    fail(RCS_ERROR_INTERNAL, "BearSSL cryptography failed");
}

// HMAC with `hash` over the concatenation of `parts`, into `output` (the hash's size).
void hmac(const br_hash_class* hash, std::span<const uint8_t> key,
          std::initializer_list<std::span<const uint8_t>> parts, std::span<uint8_t> output) {
    br_hmac_key_context keyContext;
    br_hmac_key_init(&keyContext, hash, key.data(), key.size());
    br_hmac_context context;
    br_hmac_init(&context, &keyContext, 0);
    for (const auto part : parts) {
        br_hmac_update(&context, part.data(), part.size());
    }
    br_hmac_out(&context, output.data());
}

// A private key of P-256: from 1 to the order less one (big-endian, as long
// as the order).
bool validScalar(std::span<const uint8_t, 32> scalar) {
    size_t length = 0;
    const unsigned char* order = p256.order(curve, &length);
    unsigned borrow = 0, any = 0;
    for (size_t i = length; i-- > 0;) {
        borrow = ((unsigned(scalar[i]) - order[i] - borrow) >> 8) & 1;
        any |= scalar[i];
    }
    return borrow && any;
}

// Random bytes until they are a private key (all but about 2^-32 are).
Scalar generateScalar() {
    Scalar scalar;
    do {
        random(scalar);
    } while (!validScalar(scalar));
    return scalar;
}

// 0x04 || X || Y of `scalar` times the generator.
PublicKey publicPoint(const Scalar& scalar) {
    PublicKey point;
    if (p256.mulgen(point.data(), scalar.data(), scalar.size(), curve) != point.size()) {
        cryptoFailure();
    }
    return point;
}

// The X coordinate of `point` times `scalar`; nothing when `point` is not one of P-256.
std::optional<Key> multiply(std::span<const uint8_t> point, const Scalar& scalar) {
    PublicKey product;
    if (point.size() != product.size()) {
        return std::nullopt;
    }
    std::copy(point.begin(), point.end(), product.begin());
    if (!p256.mul(product.data(), product.size(), scalar.data(), scalar.size(), curve)) {
        return std::nullopt;
    }
    Key x;
    std::copy_n(product.begin() + 1, x.size(), x.begin());
    return x;
}

// AES-CTR and SHA-256: the processor's instructions where it has them (AES-NI;
// ARMv8's AES and SHA-256), else BearSSL's software: AES bitsliced (64-bit
// where the processor is, and in WebAssembly, which has 64-bit arithmetic
// behind its 32-bit pointers: about twice as fast there), SHA-256 in C. Which
// is logged once.
struct Implementations {
    const br_block_ctr_class* aesCtr;
    const br_hash_class* sha256;
};

const Implementations& implementations() {
    static const Implementations chosen = [] {
#ifdef __wasm__
        constexpr bool wide = true;
#else
        constexpr bool wide = sizeof(void*) == 8;
#endif
        Implementations chosen{br_aes_x86ni_ctr_get_vtable(), armv8::sha256()};
        const char* aes = "AES-NI";
        if (!chosen.aesCtr) {
            chosen.aesCtr = armv8::aesCtr();
            aes = "ARMv8 instructions";
        }
        if (!chosen.aesCtr) {
            chosen.aesCtr = wide ? &br_aes_ct64_ctr_vtable : &br_aes_ct_ctr_vtable;
            aes = wide ? "BearSSL, bitsliced 64-bit" : "BearSSL, bitsliced 32-bit";
        }
        const char* sha256 = "ARMv8 instructions";
        if (!chosen.sha256) {
            chosen.sha256 = &br_sha256_vtable;
            sha256 = "BearSSL";
        }
        log::debug("crypto", std::string("AES-CTR: ") + aes + "; SHA-256: " + sha256);
        return chosen;
    }();
    return chosen;
}

} // namespace

void random(std::span<uint8_t> output) {
#if defined(__linux__) && !defined(__ANDROID__)
    for (size_t done = 0; done < output.size();) {
        const ssize_t got = getrandom(output.data() + done, output.size() - done, 0);
        if (got >= 0) {
            done += size_t(got);
        } else if (errno != EINTR) {
            cryptoFailure();
        }
    }
#elif defined(__EMSCRIPTEN__)
    // The browser's crypto.getRandomValues, at most 256 bytes a call.
    for (size_t done = 0; done < output.size(); done += 256) {
        if (getentropy(output.data() + done, std::min<size_t>(256, output.size() - done)) != 0) {
            cryptoFailure();
        }
    }
#else
    arc4random_buf(output.data(), output.size());
#endif
}

Key sha256(std::span<const uint8_t> message) {
    const br_hash_class* hash = implementations().sha256;
    br_sha256_context context;
    hash->init(&context.vtable);
    hash->update(&context.vtable, message.data(), message.size());
    Key output;
    hash->out(&context.vtable, output.data());
    return output;
}

// One block of output: U1 = HMAC(P, S || 1), Ui = HMAC(P, Ui-1), XORed together;
// the key's padded states are computed once.
Key pbkdf2Sha256(std::string_view password, std::span<const uint8_t> salt, uint32_t iterations) {
    br_hmac_key_context keyContext;
    br_hmac_key_init(&keyContext, implementations().sha256, password.data(), password.size());
    br_hmac_context context;
    br_hmac_init(&context, &keyContext, 0);
    br_hmac_update(&context, salt.data(), salt.size());
    const uint8_t block[4] = {0, 0, 0, 1};
    br_hmac_update(&context, block, sizeof(block));
    Key u, output;
    br_hmac_out(&context, u.data());
    output = u;
    for (uint32_t i = 1; i < iterations; ++i) {
        br_hmac_init(&context, &keyContext, 0);
        br_hmac_update(&context, u.data(), u.size());
        br_hmac_out(&context, u.data());
        for (size_t j = 0; j < output.size(); ++j) {
            output[j] ^= u[j];
        }
    }
    return output;
}

Key hmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message) {
    return hmacSha256(key, {message});
}

Key hmacSha256(std::span<const uint8_t> key, std::initializer_list<std::span<const uint8_t>> parts) {
    Key output;
    hmac(implementations().sha256, key, parts, output);
    return output;
}

std::array<uint8_t, 20> hmacSha1(std::span<const uint8_t> key, std::span<const uint8_t> message) {
    std::array<uint8_t, 20> output;
    hmac(&br_sha1_vtable, key, {message}, output);
    return output;
}

std::array<uint8_t, 20> sha1(std::span<const uint8_t> message) {
    br_sha1_context context;
    br_sha1_init(&context);
    br_sha1_update(&context, message.data(), message.size());
    std::array<uint8_t, 20> output;
    br_sha1_out(&context, output.data());
    return output;
}

std::array<uint8_t, 16> md5(std::span<const uint8_t> message) {
    br_md5_context context;
    br_md5_init(&context);
    br_md5_update(&context, message.data(), message.size());
    std::array<uint8_t, 16> output;
    br_md5_out(&context, output.data());
    return output;
}

// ---- ECDH P-256 ----------------------------------------------------------------

struct Ecdh::Impl {
    Scalar scalar = generateScalar();
};

Ecdh::Ecdh() : impl_(std::make_unique<Impl>()), publicKey_(publicPoint(impl_->scalar)) {}

Ecdh::~Ecdh() = default;

Key Ecdh::agree(std::span<const uint8_t> peer) const {
    if (peer.size() != publicKeySize || peer[0] != 0x04) {
        fail(RCS_ERROR_AUTHENTICATION, "The peer's key exchange value is malformed");
    }
    const auto shared = multiply(peer, impl_->scalar);
    if (!shared) {
        fail(RCS_ERROR_AUTHENTICATION, "The peer's key exchange value is not a P-256 point");
    }
    return *shared;
}

// ---- ECDSA P-256 ----------------------------------------------------------------

bool validPublicKey(std::span<const uint8_t> key) noexcept {
    Scalar one{};
    one.back() = 1;
    return key.size() == 65 && key[0] == 0x04 && multiply(key, one);
}

bool verify(const PublicKey& key, std::span<const uint8_t> message, const Signature& signature) {
    // BearSSL refuses an s of 0, not an r of 0.
    if (std::all_of(signature.begin(), signature.begin() + 32, [](uint8_t byte) { return byte == 0; })) {
        return false;
    }
    const Key hash = sha256(message);
    PublicKey point = key;
    const br_ec_public_key publicKey{curve, point.data(), point.size()};
    return br_ecdsa_i31_vrfy_raw(&p256, hash.data(), hash.size(), &publicKey, signature.data(), signature.size()) == 1;
}

struct DeviceKey::Impl {
    Scalar scalar;
};

std::array<uint8_t, DeviceKey::size> DeviceKey::generate() {
    const Scalar scalar = generateScalar();
    const PublicKey point = publicPoint(scalar);
    std::array<uint8_t, size> out;
    std::copy(point.begin(), point.end(), out.begin());
    std::copy(scalar.begin(), scalar.end(), out.begin() + ptrdiff_t(point.size()));
    return out;
}

DeviceKey::DeviceKey(std::span<const uint8_t> key) : impl_(std::make_unique<Impl>()) {
    const auto invalid = [] {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The device key is not a P-256 key pair (see rcs_generate_device_key)");
    };
    if (key.size() != size || key[0] != 0x04 || !validScalar(key.last<32>())) {
        invalid();
    }
    std::copy_n(key.end() - 32, 32, impl_->scalar.begin());
    // The public point must be the scalar's own, so the peer can verify what this key signs.
    if (!equal(publicPoint(impl_->scalar), key.first(publicKey_.size()))) {
        invalid();
    }
    std::copy_n(key.begin(), publicKey_.size(), publicKey_.begin());
}

DeviceKey::~DeviceKey() = default;

Signature DeviceKey::sign(std::span<const uint8_t> message) const {
    const Key hash = sha256(message);
    const br_ec_private_key privateKey{curve, impl_->scalar.data(), impl_->scalar.size()};
    Signature signature;
    if (br_ecdsa_i31_sign_raw(&p256, implementations().sha256, hash.data(), &privateKey, signature.data()) !=
        signature.size()) {
        cryptoFailure();
    }
    return signature;
}

// ---- AES-256-CTR ----------------------------------------------------------------

struct AesCtr::Impl {
    br_aes_gen_ctr_keys keys;
};

AesCtr::AesCtr(const Key& key) : impl_(std::make_unique<Impl>()) {
    implementations().aesCtr->init(&impl_->keys.vtable, key.data(), key.size());
}

AesCtr::~AesCtr() = default;

void AesCtr::apply(std::span<const uint8_t, 16> iv, std::span<const uint8_t> input, std::span<uint8_t> output) const {
    if (output.size() < input.size()) {
        fail(RCS_ERROR_INTERNAL, "AES-CTR output too small");
    }
    // BearSSL counts in the low 32 bits of the counter block; past them, the
    // 96 bits above carry here.
    std::array<uint8_t, 12> high;
    std::copy_n(iv.begin(), high.size(), high.begin());
    auto counter = uint32_t(loadBigEndian(iv, 12, 4));
    std::copy(input.begin(), input.end(), output.begin()); // BearSSL works in place.
    for (size_t done = 0; done < input.size();) {
        const uint64_t blocks = (uint64_t(1) << 32) - counter; // Until the low 32 bits wrap.
        const auto length = size_t(std::min<uint64_t>(input.size() - done, blocks * 16));
        counter = impl_->keys.vtable->run(&impl_->keys.vtable, high.data(), counter, output.data() + done, length);
        done += length;
        if (counter == 0) {
            for (size_t i = high.size(); i-- > 0 && ++high[i] == 0;) {
            }
        }
    }
}

} // namespace rcsdk::crypto
