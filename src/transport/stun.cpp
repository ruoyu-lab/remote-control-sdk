#include "transport/stun.hpp"

#include "util/bytes.hpp"

#include <algorithm>
#include <cstring>

namespace rcsdk::transport::stun {
namespace {

using crypto::Bytes;

uint16_t read16(std::span<const uint8_t> b, size_t at) {
    return uint16_t(loadBigEndian(b, at, 2));
}

bool isXor(uint16_t type) {
    return type == xorMappedAddress || type == xorPeerAddress || type == xorRelayedAddress;
}

void appendAttribute(Bytes& out, uint16_t type, std::span<const uint8_t> value) {
    appendBigEndian(out, type, 2);
    appendBigEndian(out, value.size(), 2);
    out.insert(out.end(), value.begin(), value.end());
    while (out.size() % 4) {
        out.push_back(0);
    }
}

// XOR-* addresses: the port with the cookie's high half, the address with the
// cookie and (IPv6) the transaction ID. Masking twice restores the value.
void mask(Bytes& value, const TransactionId& transaction) {
    uint8_t key[16];
    storeBigEndian(key, 0, magicCookie, 4);
    std::copy(transaction.begin(), transaction.end(), key + 4);
    for (size_t i = 2; i < value.size(); ++i) { // Port, then address.
        value[i] ^= key[i < 4 ? i - 2 : i - 4];
    }
}

// Offset of the first attribute of `type` in a well-formed message, or 0.
size_t offsetOf(std::span<const uint8_t> bytes, uint16_t type) {
    size_t at = headerSize;
    while (at + 4 <= bytes.size()) {
        const uint16_t t = read16(bytes, at), length = read16(bytes, at + 2);
        if (t == type) {
            return at;
        }
        at += 4 + ((length + 3u) & ~3u);
    }
    return 0;
}

} // namespace

TransactionId newTransaction() {
    TransactionId id{};
    crypto::random(id);
    return id;
}

const Bytes* Message::find(uint16_t type) const {
    for (const auto& [t, value] : attributes) {
        if (t == type) {
            return &value;
        }
    }
    return nullptr;
}

void Message::add(uint16_t type, std::span<const uint8_t> value) {
    attributes.emplace_back(type, Bytes(value.begin(), value.end()));
}

void Message::addString(uint16_t type, const std::string& value) {
    add(type, std::span(reinterpret_cast<const uint8_t*>(value.data()), value.size()));
}

void Message::addU32(uint16_t type, uint32_t value) {
    Bytes bytes;
    appendBigEndian(bytes, value, 4);
    add(type, bytes);
}

void Message::addU64(uint16_t type, uint64_t value) {
    Bytes bytes;
    appendBigEndian(bytes, value, 8);
    add(type, bytes);
}

void Message::addAddress(uint16_t type, const Endpoint& endpoint) {
    Bytes value{0, uint8_t(endpoint.ipv6() ? 2 : 1)};
    appendBigEndian(value, endpoint.port(), 2);
    const auto address = endpoint.addressBytes();
    value.insert(value.end(), address.begin(), address.end());
    if (isXor(type)) {
        mask(value, transaction);
    }
    add(type, value);
}

void Message::setTransaction(const TransactionId& id) {
    for (auto& [type, value] : attributes) {
        if (isXor(type)) {
            mask(value, transaction);
            mask(value, id);
        }
    }
    transaction = id;
}

std::optional<Endpoint> Message::address(uint16_t type) const {
    const Bytes* found = find(type);
    if (!found || found->size() < 4) {
        return std::nullopt;
    }
    const uint8_t family = (*found)[1];
    const size_t length = family == 1 ? 4 : family == 2 ? 16 : 0;
    if (!length || found->size() != 4 + length) {
        return std::nullopt;
    }
    Bytes value = *found;
    if (isXor(type)) {
        mask(value, transaction);
    }
    return Endpoint::fromBytes(std::span(value).subspan(4), read16(value, 2));
}

std::optional<uint32_t> Message::u32(uint16_t type) const {
    const Bytes* value = find(type);
    return value && value->size() == 4 ? std::optional(uint32_t(loadBigEndian(*value, 0, 4))) : std::nullopt;
}

std::optional<uint64_t> Message::u64(uint16_t type) const {
    const Bytes* value = find(type);
    return value && value->size() == 8 ? std::optional(loadBigEndian(*value, 0, 8)) : std::nullopt;
}

std::string Message::string(uint16_t type) const {
    const Bytes* value = find(type);
    return value ? std::string(value->begin(), value->end()) : std::string();
}

int Message::errorCode() const {
    const Bytes* value = find(stun::errorCode);
    return value && value->size() >= 4 ? ((*value)[2] & 7) * 100 + (*value)[3] : 0;
}

Bytes encode(const Message& message, std::span<const uint8_t> integrityKey, bool fingerprint) {
    Bytes out;
    appendBigEndian(out,
                    uint16_t(message.method & 0x0F80) << 2 | (message.method & 0x0070) << 1 |
                        (message.method & 0x000F) | message.klass,
                    2);
    appendBigEndian(out, 0, 2);
    appendBigEndian(out, magicCookie, 4);
    out.insert(out.end(), message.transaction.begin(), message.transaction.end());
    for (const auto& [type, value] : message.attributes) {
        appendAttribute(out, type, value);
    }
    if (!integrityKey.empty()) {
        storeBigEndian(out, 2, out.size() - headerSize + 24, 2); // Length as if MESSAGE-INTEGRITY were the last.
        const auto mac = crypto::hmacSha1(integrityKey, out);
        appendAttribute(out, messageIntegrity, mac);
    }
    if (fingerprint) {
        storeBigEndian(out, 2, out.size() - headerSize + 8, 2);
        Bytes value;
        appendBigEndian(value, crc32(out) ^ 0x5354554e, 4);
        appendAttribute(out, stun::fingerprint, value);
    }
    storeBigEndian(out, 2, out.size() - headerSize, 2);
    return out;
}

bool looksLikeStun(std::span<const uint8_t> bytes) noexcept {
    return bytes.size() >= headerSize && (bytes[0] & 0xC0) == 0 && loadBigEndian(bytes, 4, 4) == magicCookie &&
           read16(bytes, 2) % 4 == 0 && size_t(read16(bytes, 2)) + headerSize == bytes.size();
}

std::optional<Message> parse(std::span<const uint8_t> bytes) {
    if (!looksLikeStun(bytes)) {
        return std::nullopt;
    }
    Message message;
    const uint16_t type = read16(bytes, 0);
    message.klass = type & 0x0110;
    message.method = uint16_t((type & 0x3E00) >> 2 | (type & 0x00E0) >> 1 | (type & 0x000F));
    std::copy_n(bytes.begin() + 8, 12, message.transaction.begin());
    size_t at = headerSize;
    // RFC 8489 14.5: MESSAGE-INTEGRITY covers what precedes it, so everything
    // after it but FINGERPRINT is ignored; an on-path attacker could append it.
    bool afterIntegrity = false;
    while (at < bytes.size()) {
        if (at + 4 > bytes.size()) {
            return std::nullopt;
        }
        const uint16_t attribute = read16(bytes, at), length = read16(bytes, at + 2);
        if (at + 4 + length > bytes.size()) {
            return std::nullopt;
        }
        if (attribute == fingerprint && (at + 8 != bytes.size() || !verifyFingerprint(bytes))) {
            return std::nullopt; // Not the last attribute, or the message is corrupted.
        }
        if (!afterIntegrity || attribute == fingerprint) {
            message.attributes.emplace_back(
                attribute, Bytes(bytes.begin() + ptrdiff_t(at + 4), bytes.begin() + ptrdiff_t(at + 4 + length)));
        }
        afterIntegrity |= attribute == messageIntegrity;
        at += 4 + ((length + 3u) & ~3u);
    }
    return at == bytes.size() ? std::optional(std::move(message)) : std::nullopt;
}

std::optional<std::span<const uint8_t>> attributeIn(std::span<const uint8_t> bytes, uint16_t type) {
    const size_t at = offsetOf(bytes, type);
    if (!at || at + 4 + read16(bytes, at + 2) > bytes.size()) {
        return std::nullopt;
    }
    return bytes.subspan(at + 4, read16(bytes, at + 2));
}

bool verifyIntegrity(std::span<const uint8_t> bytes, std::span<const uint8_t> key) {
    const size_t at = offsetOf(bytes, messageIntegrity);
    if (!at || at + 24 > bytes.size() || read16(bytes, at + 2) != 20) {
        return false;
    }
    Bytes covered(bytes.begin(), bytes.begin() + ptrdiff_t(at));
    storeBigEndian(covered, 2, at - headerSize + 24, 2);
    const auto mac = crypto::hmacSha1(key, covered);
    return crypto::equal(mac, bytes.subspan(at + 4, 20));
}

bool verifyFingerprint(std::span<const uint8_t> bytes) {
    const size_t at = offsetOf(bytes, fingerprint);
    if (!at || at + 8 != bytes.size() || read16(bytes, at + 2) != 4) {
        return false;
    }
    return (crc32(bytes.first(at)) ^ 0x5354554e) == loadBigEndian(bytes, at + 4, 4);
}

std::optional<Message> parseCheck(std::span<const uint8_t> bytes, const std::string& localUfrag,
                                  std::span<const uint8_t> password) {
    auto message = parse(bytes);
    if (!message || message->method != binding || message->klass != request || !message->has(fingerprint) ||
        message->string(username).rfind(localUfrag + ":", 0) != 0 || !verifyIntegrity(bytes, password)) {
        return std::nullopt;
    }
    return message;
}

Bytes answerCheck(const Message& check, const Endpoint& from, std::span<const uint8_t> password) {
    Message response;
    response.method = binding;
    response.klass = success;
    response.transaction = check.transaction;
    response.addAddress(xorMappedAddress, from);
    return encode(response, password);
}

Bytes longTermKey(const std::string& username, const std::string& realm, const std::string& password) {
    const std::string text = username + ":" + realm + ":" + password;
    const auto digest = crypto::md5(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    return Bytes(digest.begin(), digest.end());
}

uint32_t crc32(std::span<const uint8_t> bytes) noexcept {
    uint32_t crc = 0xFFFFFFFF;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

} // namespace rcsdk::transport::stun
