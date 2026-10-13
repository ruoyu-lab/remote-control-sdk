// Byte helpers shared by the wire formats: big-endian integers, the order
// every format of the SDK (and STUN) uses, and lowercase hex for random
// identifiers and fingerprints.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rcsdk {

// Appends the low `width` bytes of `value`, most significant first.
inline void appendBigEndian(std::vector<uint8_t>& out, uint64_t value, size_t width) {
    for (size_t i = width; i > 0; --i) {
        out.push_back(uint8_t(value >> ((i - 1) * 8)));
    }
}

// The same, over `width` bytes of `out` from `offset`; the caller checked they are there.
inline void storeBigEndian(std::span<uint8_t> out, size_t offset, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        out[offset + i] = uint8_t(value >> ((width - 1 - i) * 8));
    }
}

// `width` bytes (at most 8) of `bytes` from `offset`; the caller checked they are there.
inline uint64_t loadBigEndian(std::span<const uint8_t> bytes, size_t offset, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
        value = (value << 8) | bytes[offset + i];
    }
    return value;
}

inline std::string hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

} // namespace rcsdk
