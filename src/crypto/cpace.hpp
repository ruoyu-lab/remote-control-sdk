// CPace (draft-irtf-cfrg-cpace), cipher suite CPACE-X25519-SHA512, on the
// vendored Monocypher (X25519, Elligator 2, SHA-512): a password-authenticated
// key exchange. Both parties derive a generator from the password (PRS), the
// channel identifier (CI) and the session id (sid), exchange a share each
// (scalar times generator) and hash the shared point with the transcript into
// the intermediate session key (ISK). Without the password nobody can compute
// the generator: a passive observer learns nothing it could test guesses
// against, and an active attacker tests one guess per exchange.
#pragma once

#include "util/util.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace rcsdk::crypto::cpace {

using Point = std::array<uint8_t, 32>;      // An X25519 u-coordinate (generator, share, K), or a scalar.
using SessionKey = std::array<uint8_t, 64>; // ISK

// calculate_generator: the Elligator 2 map of SHA-512(generator_string(DSI
// "CPace255", prs, ci, sid)) truncated to 32 bytes and read as a field element.
Point generator(std::span<const uint8_t> prs, std::span<const uint8_t> ci, std::span<const uint8_t> sid);
// The map step alone, from the hash (its own test vector).
Point mapToCurve(std::span<const uint8_t, 32> hash);
// sample_scalar: 32 random bytes; X25519 clamps them.
Point randomScalar();
// scalar_mult: a share.
Point multiply(const Point& scalar, const Point& point);
// scalar_mult_vfy: the shared point K, or nothing when it is the neutral
// element (the peer's share has low order): the exchange must abort.
std::optional<Point> sharedSecret(const Point& scalar, const Point& peerShare);
// ISK in initiator-responder order: SHA-512(lv_cat("CPace255_ISK", sid, K) ||
// lv_cat(Ya, ADa) || lv_cat(Yb, ADb)), Ya and ADa the initiator's.
SessionKey sessionKey(std::span<const uint8_t> sid, const Point& k, const Point& ya, std::span<const uint8_t> ada,
                      const Point& yb, std::span<const uint8_t> adb);

} // namespace rcsdk::crypto::cpace
