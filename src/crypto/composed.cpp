// What the SDK composes from the primitives of either backend (crypto.cpp,
// crypto_bearssl.cpp): HKDF, the constant-time comparison, the AEAD.
#include "crypto/crypto.hpp"

#include "util/bytes.hpp"

#include <algorithm>

namespace rcsdk::crypto {

Bytes hkdf(std::span<const uint8_t> key, std::span<const uint8_t> salt, std::span<const uint8_t> info, size_t length) {
    if (length > 255 * 32) {
        fail(RCS_ERROR_INTERNAL, "Invalid HKDF output length");
    }
    const Key zeroSalt{};
    const Key prk = hmacSha256(salt.empty() ? std::span<const uint8_t>(zeroSalt) : salt, key);
    Bytes output, block;
    for (uint8_t counter = 1; output.size() < length; ++counter) {
        Bytes message(block);
        message.insert(message.end(), info.begin(), info.end());
        message.push_back(counter);
        const Key next = hmacSha256(prk, message);
        block.assign(next.begin(), next.end());
        output.insert(output.end(), next.begin(), next.begin() + std::min(next.size(), length - output.size()));
    }
    return output;
}

bool equal(std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    uint8_t difference = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        difference |= a[i] ^ b[i];
    }
    return difference == 0;
}

// ---- AEAD -----------------------------------------------------------------------

Aead::Aead(const Key& cipherKey, const Key& macKey) : cipher_(cipherKey), macKey_(macKey) {}

Key Aead::mac(uint64_t sequence, std::span<const uint8_t> associated, std::span<const uint8_t> ciphertext) const {
    std::array<uint8_t, 12> header; // sequence(8) associated length(4)
    storeBigEndian(header, 0, sequence, 8);
    storeBigEndian(header, 8, associated.size(), 4);
    return hmacSha256(macKey_, {header, associated, ciphertext});
}

namespace {
// Sequence in the high half of the counter block: messages never share
// keystream, since none needs 2^64 blocks.
std::array<uint8_t, 16> counterBlock(uint64_t sequence) {
    std::array<uint8_t, 16> iv{};
    storeBigEndian(iv, 0, sequence, 8);
    return iv;
}
} // namespace

Bytes Aead::seal(uint64_t sequence, std::span<const uint8_t> associated, std::span<const uint8_t> plaintext) const {
    Bytes sealed(plaintext.size() + tagSize);
    cipher_.apply(counterBlock(sequence), plaintext, sealed);
    const Key tag = mac(sequence, associated, std::span(sealed).first(plaintext.size()));
    std::copy_n(tag.begin(), tagSize, sealed.begin() + ptrdiff_t(plaintext.size()));
    return sealed;
}

std::optional<Bytes> Aead::open(uint64_t sequence, std::span<const uint8_t> associated,
                                std::span<const uint8_t> sealed) const {
    if (sealed.size() < tagSize) {
        return std::nullopt;
    }
    const auto ciphertext = sealed.first(sealed.size() - tagSize);
    const Key tag = mac(sequence, associated, ciphertext);
    if (!equal(std::span(tag).first(tagSize), sealed.last(tagSize))) {
        return std::nullopt;
    }
    Bytes plaintext(ciphertext.size());
    cipher_.apply(counterBlock(sequence), ciphertext, plaintext);
    return plaintext;
}

} // namespace rcsdk::crypto
