// STUN messages (RFC 8489) with the attributes ICE (RFC 8445) and TURN
// (RFC 8656) use: what the SDK needs to find addresses, check paths and talk
// to standard STUN and TURN servers such as coturn.
#pragma once

#include "crypto/crypto.hpp"
#include "transport/socket.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rcsdk::transport::stun {

inline constexpr uint32_t magicCookie = 0x2112A442;
inline constexpr size_t headerSize = 20;

enum Method : uint16_t {
    binding = 0x001,
    allocate = 0x003,
    refresh = 0x004,
    sendMethod = 0x006,
    dataMethod = 0x007,
    createPermission = 0x008,
    channelBind = 0x009
};
enum Class : uint16_t { request = 0x0000, indication = 0x0010, success = 0x0100, error = 0x0110 };

enum Attribute : uint16_t {
    username = 0x0006,
    messageIntegrity = 0x0008,
    errorCode = 0x0009,
    channelNumber = 0x000C,
    lifetime = 0x000D,
    xorPeerAddress = 0x0012,
    data = 0x0013,
    realm = 0x0014,
    nonce = 0x0015,
    xorRelayedAddress = 0x0016,
    requestedTransport = 0x0019,
    xorMappedAddress = 0x0020,
    priority = 0x0024,
    useCandidate = 0x0025,
    fingerprint = 0x8028,
    iceControlled = 0x8029,
    iceControlling = 0x802A,
    mobilityTicket = 0x8030 // RFC 8016
};

using TransactionId = std::array<uint8_t, 12>;
TransactionId newTransaction();

struct Message {
    uint16_t method = 0;
    uint16_t klass = 0;
    TransactionId transaction{}; // Set before adding XOR-* addresses (they are masked with it), or use setTransaction.
    std::vector<std::pair<uint16_t, crypto::Bytes>> attributes; // In wire order.

    // A new transaction ID; XOR-* addresses already added are masked anew (an
    // IPv6 address's mask includes the ID).
    void setTransaction(const TransactionId& id);

    const crypto::Bytes* find(uint16_t type) const;
    bool has(uint16_t type) const { return find(type) != nullptr; }
    void add(uint16_t type, std::span<const uint8_t> value);
    void addString(uint16_t type, const std::string& value);
    void addU32(uint16_t type, uint32_t value);
    void addU64(uint16_t type, uint64_t value);
    // XOR-encoded for the XOR-* attribute types, plain otherwise.
    void addAddress(uint16_t type, const Endpoint& endpoint);
    std::optional<Endpoint> address(uint16_t type) const;
    std::optional<uint32_t> u32(uint16_t type) const;
    std::optional<uint64_t> u64(uint16_t type) const;
    std::string string(uint16_t type) const;
    // ERROR-CODE as class * 100 + number, or 0.
    int errorCode() const;
};

// Encodes `message`; with a key, appends MESSAGE-INTEGRITY (HMAC-SHA1); then
// FINGERPRINT when asked.
crypto::Bytes encode(const Message& message, std::span<const uint8_t> integrityKey = {}, bool fingerprint = true);
// The value of the first `type` attribute, inside `bytes` (no copy).
std::optional<std::span<const uint8_t>> attributeIn(std::span<const uint8_t> bytes, uint16_t type);
// A STUN message, or nothing for other bytes (first two bits, cookie,
// lengths) and for a message whose FINGERPRINT is not the last attribute or
// does not match (RFC 8489 14.7).
std::optional<Message> parse(std::span<const uint8_t> bytes);
// MESSAGE-INTEGRITY of a received message checks out with `key`.
bool verifyIntegrity(std::span<const uint8_t> bytes, std::span<const uint8_t> key);
// FINGERPRINT is present and correct.
bool verifyFingerprint(std::span<const uint8_t> bytes);
// Could be a STUN message: worth parsing.
bool looksLikeStun(std::span<const uint8_t> bytes) noexcept;

// ICE connectivity checks (RFC 8445 7.2), for the agent and for the links
// that keep answering them on the chosen path: a Binding request with
// FINGERPRINT whose USERNAME starts with "<localUfrag>:" and whose
// MESSAGE-INTEGRITY checks out with `password` (this end's ICE password).
std::optional<Message> parseCheck(std::span<const uint8_t> bytes, const std::string& localUfrag,
                                  std::span<const uint8_t> password);
// The success response to `check`: the address it came from, with integrity and FINGERPRINT.
crypto::Bytes answerCheck(const Message& check, const Endpoint& from, std::span<const uint8_t> password);

// The short-term credential key is the password itself; TURN's long-term key
// is MD5(username ":" realm ":" password).
crypto::Bytes longTermKey(const std::string& username, const std::string& realm, const std::string& password);

uint32_t crc32(std::span<const uint8_t> bytes) noexcept;

} // namespace rcsdk::transport::stun
