#include "transport/handshake.hpp"

#include "util/bytes.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace rcsdk::transport {
namespace {

using crypto::Bytes;

// Key derivation info: this label, then the hellos, which begin with the
// protocol magic and so bind the keys to the protocol version.
constexpr char keyLabel[] = "rcsdk keys";
constexpr std::string_view serverLabel = "rcsdk server identity", clientLabel = "rcsdk client identity";

// finished_s, finished_c, per direction cipher and MAC keys, the identity blocks' keys, then the exporter.
constexpr size_t serverFinishedKey = 0, clientFinishedKey = 32, serverCipher = 64, serverMac = 96, clientCipher = 128,
                 clientMac = 160, serverIdentityCipher = 192, serverIdentityMac = 224, clientIdentityCipher = 256,
                 clientIdentityMac = 288, exporterKey = 320, keyMaterialSize = 352;
constexpr size_t nonceOffset = protocolMagicSize + 1;                                // In the ClientHello.
constexpr size_t clientShareOffset = nonceOffset + 32 + crypto::Ecdh::publicKeySize; // cpace_c
constexpr size_t serverShareOffset = 1 + 32 + crypto::Ecdh::publicKeySize;           // cpace_s in the ServerHello
constexpr size_t saltOffset = serverShareOffset + cpaceShareSize;
constexpr size_t helloPartOfServerHello = saltOffset + 16;
constexpr std::string_view clientRole = "rcsdk client", serverRole = "rcsdk server";
constexpr uint8_t pinNotice[protocolMagicSize] = {'R', 'C', 'S', 'P', 'I', 'N', protocolMagic[6], protocolMagic[7]};

// The magic this end's handshakes run under.
std::span<const uint8_t, protocolMagicSize> magicOf(const Credentials& credentials) noexcept {
    return credentials.forwarder ? std::span(forwarderMagic) : std::span(protocolMagic);
}

const char* secretName(uint8_t secret) {
    switch (secret) {
    case RCS_SECRET_KEY:
        return "the connection key";
    case RCS_SECRET_PASSWORD:
        return "a password";
    case RCS_SECRET_PIN:
        return "a PIN";
    case RCS_SECRET_NONE:
        return "device keys only";
    default:
        return "an unknown secret";
    }
}

[[noreturn]] void mismatch(rcs_secret secret) {
    fail(RCS_ERROR_AUTHENTICATION, secret == RCS_SECRET_PASSWORD ? "Wrong password (the peer's differs)"
                                   : secret == RCS_SECRET_PIN    ? "Wrong PIN or another secret"
                                   : secret == RCS_SECRET_KEY    ? "Connection key mismatch"
                                                                 : "The handshake does not verify");
}

constexpr size_t versionOffset = 6; // The magic ends with the version, big-endian.

// The client's hello, once it is one of this protocol version.
crypto::Bytes checkedHello(const Credentials& credentials, std::span<const uint8_t> hello) {
    if (const auto version = otherProtocolVersion(hello)) {
        versionMismatch(*version);
    }
    const auto magic = magicOf(credentials);
    if (hello.size() != clientHelloSize || !std::equal(magic.begin(), magic.end(), hello.begin())) {
        fail(RCS_ERROR_AUTHENTICATION, "Malformed handshake from the peer");
    }
    return crypto::Bytes(hello.begin(), hello.end());
}

crypto::Key pskOf(const Credentials& credentials, const PasswordKeys::Salt& salt) {
    switch (credentials.secret) {
    case RCS_SECRET_PASSWORD:
        return credentials.passwordKeys->derive(credentials.password, salt);
    case RCS_SECRET_NONE:
        return crypto::Key{};
    default:
        return credentials.key;
    }
}

// CPace's generator for this PIN and session (see handshake.hpp).
crypto::cpace::Point pinGenerator(const Credentials& credentials, std::span<const uint8_t> sid) {
    Bytes ci;
    const auto magic = magicOf(credentials);
    for (const std::string_view part :
         {std::string_view(reinterpret_cast<const char*>(magic.data()), magic.size()), clientRole, serverRole}) {
        ci.push_back(uint8_t(part.size()));
        ci.insert(ci.end(), part.begin(), part.end());
    }
    const auto& pin = credentials.password;
    return crypto::cpace::generator({reinterpret_cast<const uint8_t*>(pin.data()), pin.size()}, ci, sid);
}

// The psk from CPace's shared point; Failure(AUTHENTICATION) when the peer's
// share has low order.
crypto::Key pinKey(const crypto::cpace::Point& scalar, std::span<const uint8_t> sid,
                   const crypto::cpace::Point& clientShare, const crypto::cpace::Point& serverShare, bool client) {
    const auto k = crypto::cpace::sharedSecret(scalar, client ? serverShare : clientShare);
    if (!k) {
        fail(RCS_ERROR_AUTHENTICATION, "Malformed handshake from the peer");
    }
    const auto isk = crypto::cpace::sessionKey(sid, *k, clientShare, {}, serverShare, {});
    crypto::Key psk{};
    std::copy_n(isk.begin(), psk.size(), psk.begin());
    return psk;
}

crypto::cpace::Point pointAt(std::span<const uint8_t> bytes, size_t offset) {
    crypto::cpace::Point point{};
    std::copy_n(bytes.begin() + ptrdiff_t(offset), point.size(), point.begin());
    return point;
}

Bytes deriveKeys(const crypto::Key& psk, const crypto::Ecdh& ephemeral, std::span<const uint8_t> peer,
                 std::span<const uint8_t> hellos) {
    const crypto::Key shared = ephemeral.agree(peer);
    Bytes info(keyLabel, keyLabel + sizeof(keyLabel) - 1);
    info.insert(info.end(), hellos.begin(), hellos.end());
    return crypto::hkdf(shared, psk, info, keyMaterialSize);
}

crypto::Key slice(const Bytes& material, size_t offset) {
    crypto::Key key{};
    std::copy_n(material.begin() + ptrdiff_t(offset), key.size(), key.begin());
    return key;
}

crypto::Key finished(const Bytes& material, size_t keyOffset, std::span<const uint8_t> transcript) {
    return crypto::hmacSha256(slice(material, keyOffset), transcript);
}

crypto::Aead identityKeys(const Bytes& material, bool server) {
    return crypto::Aead(slice(material, server ? serverIdentityCipher : clientIdentityCipher),
                        slice(material, server ? serverIdentityMac : clientIdentityMac));
}

Bytes labelled(std::string_view label, std::span<const uint8_t> transcript) {
    Bytes message(label.begin(), label.end());
    message.insert(message.end(), transcript.begin(), transcript.end());
    return message;
}

// This end's identity block, sealed: its device key and signature over `transcript`.
Bytes sealIdentity(const Credentials& credentials, const Bytes& material, bool server,
                   std::span<const uint8_t> transcript) {
    Bytes block(identitySize, 0);
    if (credentials.device) {
        block[0] = 1;
        const auto& key = credentials.device->publicKey();
        std::copy(key.begin(), key.end(), block.begin() + 1);
        const auto signature = credentials.device->sign(labelled(server ? serverLabel : clientLabel, transcript));
        std::copy(signature.begin(), signature.end(), block.begin() + 1 + 65);
    }
    return identityKeys(material, server).seal(0, {}, block);
}

// The peer's identity block: its device key, checked against its signature
// over `transcript` and against what this end trusts.
std::optional<crypto::PublicKey> openIdentity(const Credentials& credentials, const Bytes& material, bool peerIsServer,
                                              std::span<const uint8_t> sealed, std::span<const uint8_t> transcript) {
    const auto block = identityKeys(material, peerIsServer).open(0, {}, sealed);
    if (!block || block->size() != identitySize || (*block)[0] > 1) {
        fail(RCS_ERROR_AUTHENTICATION, "Malformed handshake from the peer");
    }
    std::optional<crypto::PublicKey> key;
    if ((*block)[0]) {
        crypto::PublicKey proven{};
        crypto::Signature signature{};
        std::copy_n(block->begin() + 1, proven.size(), proven.begin());
        std::copy_n(block->begin() + 1 + 65, signature.size(), signature.begin());
        if (!crypto::verify(proven, labelled(peerIsServer ? serverLabel : clientLabel, transcript), signature)) {
            fail(RCS_ERROR_AUTHENTICATION, "The peer's device key signature does not verify");
        }
        key = proven;
    }
    const auto& trusted = credentials.trusted;
    if (trusted.empty()) {
        return key;
    }
    if (!key) {
        throw UntrustedPeer("The peer proves no device key, and this end accepts trusted ones only", std::nullopt);
    }
    if (std::find(trusted.begin(), trusted.end(), *key) == trusted.end()) {
        throw UntrustedPeer("The peer's device key " + fingerprint(*key) + " is not one this end trusts", *key);
    }
    return key;
}

// The connection id `side`'s datagrams carry.
uint32_t connectionId(const crypto::Key& exporter, std::string_view side) {
    const std::string label = "rcsdk cid " + std::string(side);
    const auto mac =
        crypto::hmacSha256(exporter, std::span(reinterpret_cast<const uint8_t*>(label.data()), label.size()));
    return uint32_t(loadBigEndian(mac, 0, 4));
}

void install(const Bytes& material, bool server, TrafficKeys& keys) {
    auto serverKeys = std::make_unique<crypto::Aead>(slice(material, serverCipher), slice(material, serverMac));
    auto clientKeys = std::make_unique<crypto::Aead>(slice(material, clientCipher), slice(material, clientMac));
    keys.send = std::move(server ? serverKeys : clientKeys);
    keys.receive = std::move(server ? clientKeys : serverKeys);
    keys.exporter = slice(material, exporterKey);
    // A client's datagrams carry the server's id, and the other way round.
    const uint32_t serverId = connectionId(keys.exporter, "server"), clientId = connectionId(keys.exporter, "client");
    keys.sendId = server ? clientId : serverId;
    keys.receiveId = server ? serverId : clientId;
}

} // namespace

std::span<const uint8_t> versionNotice(std::span<const uint8_t> hello) noexcept {
    const bool forwarder = hello.size() >= protocolMagicSize &&
                           std::equal(forwarderMagic, forwarderMagic + versionOffset, hello.begin());
    return forwarder ? std::span<const uint8_t>(forwarderMagic) : std::span<const uint8_t>(protocolMagic);
}

std::optional<uint16_t> otherProtocolVersion(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() < protocolMagicSize || (!std::equal(protocolMagic, protocolMagic + versionOffset, bytes.begin()) &&
                                             !std::equal(forwarderMagic, forwarderMagic + versionOffset, bytes.begin()))) {
        return std::nullopt;
    }
    const auto version = uint16_t(loadBigEndian(bytes, versionOffset, 2));
    return version != protocolVersion ? std::optional(version) : std::nullopt;
}

std::span<const uint8_t> pinLockedNotice() noexcept {
    return pinNotice;
}

void checkPinLockedNotice(std::span<const uint8_t> bytes) {
    if (std::equal(bytes.begin(), bytes.end(), std::begin(pinNotice), std::end(pinNotice))) {
        fail(RCS_ERROR_AUTHENTICATION, "The sharer accepts no more PIN attempts");
    }
}

void versionMismatch(uint16_t peer) {
    fail(RCS_ERROR_UNSUPPORTED, "The peer runs protocol version " + std::to_string(peer) + ", this end " +
                                    std::to_string(protocolVersion) + "; both ends need the same SDK version");
}

ClientHandshake::ClientHandshake(const Credentials& credentials) : credentials_(credentials) {
    std::array<uint8_t, 32> nonce{};
    crypto::random(nonce);
    const auto magic = magicOf(credentials);
    hello_.assign(magic.begin(), magic.end());
    hello_.push_back(uint8_t(credentials.secret));
    hello_.insert(hello_.end(), nonce.begin(), nonce.end());
    hello_.insert(hello_.end(), ephemeral_.publicKey().begin(), ephemeral_.publicKey().end());
    crypto::cpace::Point share{};
    if (credentials.secret == RCS_SECRET_PIN) {
        pinScalar_ = crypto::cpace::randomScalar();
        share = crypto::cpace::multiply(pinScalar_, pinGenerator(credentials, nonce));
    }
    hello_.insert(hello_.end(), share.begin(), share.end());
}

Bytes ClientHandshake::finish(std::span<const uint8_t> serverHello, TrafficKeys& keys) {
    if (serverHello.size() != serverHelloSize) {
        fail(RCS_ERROR_AUTHENTICATION, "Malformed handshake from the peer");
    }
    if (serverHello[0] != credentials_.secret) {
        fail(RCS_ERROR_AUTHENTICATION, std::string("The peer authenticates with ") + secretName(serverHello[0]) +
                                           ", this end with " + secretName(uint8_t(credentials_.secret)));
    }
    if (spent_) {
        fail(RCS_ERROR_AUTHENTICATION, "Another answer to a PIN handshake that already tested one");
    }
    Bytes transcript = hello_;
    transcript.insert(transcript.end(), serverHello.begin(), serverHello.begin() + helloPartOfServerHello);
    const auto peer = serverHello.subspan(1 + 32, crypto::Ecdh::publicKeySize);
    PasswordKeys::Salt salt{};
    std::copy_n(serverHello.begin() + saltOffset, salt.size(), salt.begin());
    crypto::Key psk{};
    if (credentials_.secret == RCS_SECRET_PIN) {
        psk = pinKey(pinScalar_, std::span(hello_).subspan(nonceOffset, 32), pointAt(hello_, clientShareOffset),
                     pointAt(serverHello, serverShareOffset), true);
        spent_ = true; // A share of low order, refused above, tests nothing.
    } else {
        psk = pskOf(credentials_, salt);
    }
    const Bytes material = deriveKeys(psk, ephemeral_, peer, transcript);
    const size_t hellos = transcript.size();
    const auto sealed = serverHello.subspan(helloPartOfServerHello, sealedIdentitySize);
    transcript.insert(transcript.end(), sealed.begin(), sealed.end());
    if (!crypto::equal(finished(material, serverFinishedKey, transcript), serverHello.last(32))) {
        mismatch(credentials_.secret);
    }
    keys.peerKey = openIdentity(credentials_, material, true, sealed, std::span(transcript).first(hellos));
    transcript.insert(transcript.end(), serverHello.end() - 32, serverHello.end());
    Bytes reply = sealIdentity(credentials_, material, false, transcript);
    transcript.insert(transcript.end(), reply.begin(), reply.end());
    const crypto::Key proof = finished(material, clientFinishedKey, transcript);
    reply.insert(reply.end(), proof.begin(), proof.end());
    install(material, false, keys);
    return reply;
}

bool ClientHandshake::costly(std::span<const uint8_t> serverHello) const {
    if (credentials_.secret != RCS_SECRET_PASSWORD || serverHello.size() != serverHelloSize ||
        serverHello[0] != credentials_.secret) {
        return false;
    }
    PasswordKeys::Salt salt{};
    std::copy_n(serverHello.begin() + saltOffset, salt.size(), salt.begin());
    return !credentials_.passwordKeys->derived(credentials_.password, salt);
}

ServerHandshake::ServerHandshake(const Credentials& credentials, std::span<const uint8_t> clientHello)
    : credentials_(credentials), transcript_(checkedHello(credentials, clientHello)) {
    // A client with another secret gets an answer all the same, so it can say
    // what differs; it cannot finish.
    std::array<uint8_t, 32> nonce{};
    crypto::random(nonce);
    PasswordKeys::Salt salt{};
    if (credentials_.secret == RCS_SECRET_PASSWORD) {
        salt = credentials_.passwordKeys->salt();
    } else {
        crypto::random(salt);
    }
    crypto::Key psk{};
    crypto::cpace::Point share{};
    if (credentials_.secret != RCS_SECRET_PIN) {
        psk = pskOf(credentials_, salt);
    } else if (clientHello[protocolMagicSize] == RCS_SECRET_PIN) {
        // Held from here; a failure from the answer, which lets the client test one guess.
        pinAttempt_ = PinAttempts::take(credentials_.pinAttempts);
        const auto sid = clientHello.subspan(nonceOffset, 32);
        const auto scalar = crypto::cpace::randomScalar();
        share = crypto::cpace::multiply(scalar, pinGenerator(credentials_, sid));
        psk = pinKey(scalar, sid, pointAt(clientHello, clientShareOffset), share, false);
    } else {
        crypto::random(psk); // No CPace with a client of another secret: it learns which, and cannot finish.
    }
    transcript_.push_back(uint8_t(credentials_.secret));
    transcript_.insert(transcript_.end(), nonce.begin(), nonce.end());
    transcript_.insert(transcript_.end(), ephemeral_.publicKey().begin(), ephemeral_.publicKey().end());
    transcript_.insert(transcript_.end(), share.begin(), share.end());
    transcript_.insert(transcript_.end(), salt.begin(), salt.end());
    const auto peer = clientHello.subspan(protocolMagicSize + 1 + 32, crypto::Ecdh::publicKeySize);
    keyMaterial_ = deriveKeys(psk, ephemeral_, peer, transcript_);
    const Bytes sealed = sealIdentity(credentials_, keyMaterial_, true, transcript_);
    transcript_.insert(transcript_.end(), sealed.begin(), sealed.end());
    const crypto::Key proof = finished(keyMaterial_, serverFinishedKey, transcript_);
    transcript_.insert(transcript_.end(), proof.begin(), proof.end());
    answer_.assign(transcript_.begin() + ptrdiff_t(clientHelloSize), transcript_.end());
    if (pinAttempt_) {
        pinAttempt_->answered();
    }
}

void ServerHandshake::finish(std::span<const uint8_t> clientFinished, TrafficKeys& keys) const {
    if (clientFinished.size() != finishedSize) {
        fail(RCS_ERROR_AUTHENTICATION, "Malformed handshake from the peer");
    }
    const auto sealed = clientFinished.first(sealedIdentitySize);
    Bytes transcript = transcript_;
    transcript.insert(transcript.end(), sealed.begin(), sealed.end());
    if (!crypto::equal(finished(keyMaterial_, clientFinishedKey, transcript), clientFinished.last(32))) {
        mismatch(credentials_.secret);
    }
    if (pinAttempt_) {
        pinAttempt_->proven();
    }
    install(keyMaterial_, true, keys);
    keys.peerKey = openIdentity(credentials_, keyMaterial_, false, sealed, transcript_);
}

} // namespace rcsdk::transport
