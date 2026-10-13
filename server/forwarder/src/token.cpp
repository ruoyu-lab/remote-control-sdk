#include "token.hpp"

#include "crypto/crypto.hpp"
#include "util/bytes.hpp"

#include <algorithm>
#include <cstring>

namespace rcsdk::forwarder {
namespace {

constexpr std::string_view prefix = "rcsf1.";
constexpr uint8_t version = 1;
constexpr size_t nonceSize = 16, audienceSize = 32;
constexpr size_t fixedSize = 1 + 4 + 1 + 8 + 8 + 4 + audienceSize + nonceSize + 1; // Before the room's bytes.
constexpr size_t maxRoom = RCSFWD_ROOM_SIZE - 1;
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url(std::span<const uint8_t> bytes) {
    std::string text;
    uint32_t bits = 0;
    int count = 0;
    for (const uint8_t byte : bytes) {
        bits = bits << 8 | byte;
        count += 8;
        while (count >= 6) {
            count -= 6;
            text.push_back(alphabet[(bits >> count) & 63]);
        }
    }
    if (count) {
        text.push_back(alphabet[(bits << (6 - count)) & 63]);
    }
    return text;
}

// The bytes, or none for text that is not canonical base64url without padding.
std::optional<Bytes> unbase64url(std::string_view text) {
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    Bytes bytes;
    uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        const char* at = std::strchr(alphabet, c);
        if (!c || !at) {
            return std::nullopt;
        }
        bits = bits << 6 | uint32_t(at - alphabet);
        count += 6;
        if (count >= 8) {
            count -= 8;
            bytes.push_back(uint8_t(bits >> count));
        }
    }
    if (bits & ((1u << count) - 1)) {
        return std::nullopt; // Bits left over that no byte holds: not the text this encoder makes.
    }
    return bytes;
}

crypto::Key macOf(const rcsfwd_secret& secret, std::span<const uint8_t> payload) {
    return crypto::hmacSha256(secret.secret, {std::span(reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size()),
                                              payload});
}

bool validRole(uint32_t role) {
    return role == RCSFWD_ROLE_PUBLISH || role == RCSFWD_ROLE_SUBSCRIBE;
}

bool validRoom(std::string_view room) {
    return !room.empty() && room.size() <= maxRoom && room.find('\0') == std::string_view::npos;
}

} // namespace

std::string sign(const rcsfwd_secret& secret, const Grant& grant) {
    if (!validRole(grant.role) || !validRoom(grant.room) ||
        std::all_of(grant.audience.begin(), grant.audience.end(), [](uint8_t b) { return !b; })) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcsfwd_grant: role PUBLISH or SUBSCRIBE, a room of 1 to 64 bytes without "
                                         "NUL, and the audience (the forwarder's rcsfwd_public_key_sha256)");
    }
    Bytes payload{version};
    appendBigEndian(payload, secret.key_id, 4);
    payload.push_back(uint8_t(grant.role));
    appendBigEndian(payload, grant.route, 8);
    appendBigEndian(payload, grant.expires, 8);
    appendBigEndian(payload, grant.maxBitrate, 4);
    payload.insert(payload.end(), grant.audience.begin(), grant.audience.end());
    uint8_t nonce[nonceSize];
    crypto::random(nonce);
    payload.insert(payload.end(), std::begin(nonce), std::end(nonce));
    payload.push_back(uint8_t(grant.room.size()));
    payload.insert(payload.end(), grant.room.begin(), grant.room.end());
    const auto mac = macOf(secret, payload);
    return std::string(prefix) + base64url(payload) + "." + base64url(mac);
}

std::variant<Grant, std::string> verify(std::string_view token, std::span<const rcsfwd_secret> secrets,
                                        const std::array<uint8_t, 32>& audience, uint64_t now) {
    if (token.substr(0, prefix.size()) != prefix) {
        return std::string("Malformed token (not an rcsf1. token)");
    }
    const auto rest = token.substr(prefix.size());
    const auto dot = rest.find('.');
    if (dot == std::string_view::npos) {
        return std::string("Malformed token (no signature)");
    }
    const auto payload = unbase64url(rest.substr(0, dot));
    const auto mac = unbase64url(rest.substr(dot + 1));
    if (!payload || !mac || mac->size() != 32 || payload->size() < fixedSize + 1 ||
        payload->size() != fixedSize + (*payload)[fixedSize - 1] || (*payload)[0] != version) {
        return std::string("Malformed token");
    }
    const auto& p = *payload;
    const uint32_t keyId = uint32_t(loadBigEndian(p, 1, 4));
    const auto secret = std::find_if(secrets.begin(), secrets.end(), [&](const auto& s) { return s.key_id == keyId; });
    if (secret == secrets.end()) {
        return "The token is signed with a key this forwarder does not hold (key " + std::to_string(keyId) + ")";
    }
    if (!crypto::equal(macOf(*secret, p), *mac)) {
        return std::string("The token's signature does not verify");
    }
    Grant grant;
    grant.role = p[5];
    grant.route = loadBigEndian(p, 6, 8);
    grant.expires = loadBigEndian(p, 14, 8);
    grant.maxBitrate = uint32_t(loadBigEndian(p, 22, 4));
    std::copy_n(p.begin() + 26, audienceSize, grant.audience.begin());
    grant.room.assign(p.begin() + ptrdiff_t(fixedSize), p.end());
    if (!validRole(grant.role) || !validRoom(grant.room)) {
        return std::string("Malformed token (role or room)");
    }
    if (grant.audience != audience) {
        return std::string("The token is for another forwarder (its audience is not this forwarder's key)");
    }
    if (grant.expires <= now) {
        return "The token expired (" + std::to_string(now - grant.expires) + " s ago)";
    }
    return grant;
}

std::variant<Grant, std::string> checked(const rcsfwd_grant& g, uint64_t now) {
    const std::string_view room(g.room, strnlen(g.room, sizeof(g.room)));
    if (g.struct_size != sizeof(g) || !validRole(g.role) || !validRoom(room) || room.size() == sizeof(g.room)) {
        return std::string("The host's check gave no grant this version defines");
    }
    if (g.expires && g.expires <= now) {
        return std::string("The token expired");
    }
    return Grant{std::string(room), g.role, g.route, g.expires, g.max_bitrate_bps};
}

} // namespace rcsdk::forwarder
