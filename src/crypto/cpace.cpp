#include "crypto/cpace.hpp"

#include "crypto/crypto.hpp"

// clang-format off
#include "monocypher/rcsdk_monocypher_names.h" // First: the calls below name the SDK's own copy.
#include "monocypher/monocypher.h"
#include "monocypher/monocypher-ed25519.h"
// clang-format on

#include <algorithm>
#include <string_view>

namespace rcsdk::crypto::cpace {
namespace {

constexpr std::string_view dsi = "CPace255";
constexpr std::string_view iskDsi = "CPace255_ISK";
constexpr size_t hashBlock = 128; // SHA-512's input block (H.s_in_bytes).

// p = 2^255 - 19, little-endian.
constexpr Point fieldPrime = {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                              0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                              0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};

// prepend_len: the length as LEB128, then the bytes.
void appendWithLength(Bytes& out, std::span<const uint8_t> data) {
    size_t length = data.size();
    do {
        const auto low = uint8_t(length & 0x7f);
        length >>= 7;
        out.push_back(length ? uint8_t(low | 0x80) : low);
    } while (length);
    out.insert(out.end(), data.begin(), data.end());
}

std::span<const uint8_t> bytesOf(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

size_t lengthWithPrefix(size_t size) {
    size_t prefix = 1;
    while (size >>= 7) {
        ++prefix;
    }
    return prefix;
}

// a - b over 32 little-endian bytes, for a >= b.
Point subtract(const Point& a, const Point& b) {
    Point difference{};
    int borrow = 0;
    for (size_t i = 0; i < difference.size(); ++i) {
        const int value = int(a[i]) - int(b[i]) - borrow;
        borrow = value < 0;
        difference[i] = uint8_t(value);
    }
    return difference;
}

bool atLeast(const Point& a, const Point& b) {
    for (size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return true;
}

} // namespace

Point mapToCurve(std::span<const uint8_t, 32> hash) {
    // decodeUCoordinate with 255 bits, reduced modulo p.
    Point r{};
    std::copy(hash.begin(), hash.end(), r.begin());
    r[31] &= 0x7f;
    if (atLeast(r, fieldPrime)) {
        r = subtract(r, fieldPrime);
    }
    // Monocypher's map reads 254 bits (Elligator's representatives), the
    // draft's 255. The map depends on r only through r^2, so p - r maps like
    // r, and is below 2^254 whenever r is not.
    if (r[31] & 0x40) {
        r = subtract(fieldPrime, r);
    }
    Point u{};
    crypto_elligator_map(u.data(), r.data());
    return u;
}

Point generator(std::span<const uint8_t> prs, std::span<const uint8_t> ci, std::span<const uint8_t> sid) {
    // generator_string: lv_cat(DSI, PRS, zero padding, CI, sid), the padding
    // filling the first hash block after DSI and PRS.
    const size_t used = 1 + lengthWithPrefix(dsi.size()) + dsi.size() + lengthWithPrefix(prs.size()) + prs.size();
    const Bytes padding(used < hashBlock ? hashBlock - used : 0, 0);
    Bytes text;
    appendWithLength(text, bytesOf(dsi));
    appendWithLength(text, prs);
    appendWithLength(text, padding);
    appendWithLength(text, ci);
    appendWithLength(text, sid);
    std::array<uint8_t, 64> hash{};
    crypto_sha512(hash.data(), text.data(), text.size());
    crypto_wipe(text.data(), text.size());
    const Point g = mapToCurve(std::span(hash).first<32>());
    crypto_wipe(hash.data(), hash.size());
    return g;
}

Point randomScalar() {
    Point scalar{};
    random(scalar);
    return scalar;
}

Point multiply(const Point& scalar, const Point& point) {
    Point product{};
    crypto_x25519(product.data(), scalar.data(), point.data());
    return product;
}

std::optional<Point> sharedSecret(const Point& scalar, const Point& peerShare) {
    const Point k = multiply(scalar, peerShare);
    if (std::all_of(k.begin(), k.end(), [](uint8_t b) { return b == 0; })) {
        return std::nullopt;
    }
    return k;
}

SessionKey sessionKey(std::span<const uint8_t> sid, const Point& k, const Point& ya, std::span<const uint8_t> ada,
                      const Point& yb, std::span<const uint8_t> adb) {
    Bytes input;
    appendWithLength(input, bytesOf(iskDsi));
    appendWithLength(input, sid);
    appendWithLength(input, k);
    appendWithLength(input, ya);
    appendWithLength(input, ada);
    appendWithLength(input, yb);
    appendWithLength(input, adb);
    SessionKey isk{};
    crypto_sha512(isk.data(), input.data(), input.size());
    crypto_wipe(input.data(), input.size());
    return isk;
}

} // namespace rcsdk::crypto::cpace
