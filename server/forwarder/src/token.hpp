// The forwarder's tokens ("rcsf1.", rcsfwd.h): signed by the host's server
// with a secret it shares with the forwarder, checked here before an end
// gets anything of a room.
#pragma once

#include "util/util.hpp"

#include <rcsfwd.h>

#include <array>
#include <ctime>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace rcsdk::forwarder {

// What a token grants, checked.
struct Grant {
    std::string room;
    rcsfwd_role role = RCSFWD_ROLE_SUBSCRIBE;
    uint64_t route = 0;
    uint64_t expires = 0;
    uint32_t maxBitrate = 0;
    std::array<uint8_t, 32> audience{}; // The SHA-256 of the forwarder's public key it is for.
};

// A token for `grant`; throws INVALID_ARGUMENT for a grant no token holds.
std::string sign(const rcsfwd_secret& secret, const Grant& grant);

// The token's grant, or why it is refused: malformed, of no key held, not
// signed by it, for another forwarder than `audience` names, expired at
// `now` (Unix seconds). The reasons say which, never what the secret is.
std::variant<Grant, std::string> verify(std::string_view token, std::span<const rcsfwd_secret> secrets,
                                        const std::array<uint8_t, 32>& audience, uint64_t now);

// A grant as the host's own check filled it (rcsfwd_config.verify_token), or why it holds none.
std::variant<Grant, std::string> checked(const rcsfwd_grant& grant, uint64_t now);

// Unix time now, in seconds.
inline uint64_t unixNow() {
    return uint64_t(std::time(nullptr));
}

} // namespace rcsdk::forwarder
