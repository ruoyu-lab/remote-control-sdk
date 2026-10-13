// Key agreement shared by every transport. The magic carries the protocol
// version; ends of different versions refuse each other, and the server tells
// a client of another version its own (versionNotice), so that the client can
// say which version it met. A forwarder's hop (RCS_TRANSPORT_FORWARDER) runs
// the same handshake under a magic of its own, RCSFWD (Credentials::forwarder),
// whose version follows the session protocol's: neither end takes the
// other's hello, and their keys never match.
//
//   client -> server  ClientHello  magic(8) secret(1) nonce_c(32) ephemeral_c(65)
//                                  cpace_c(32)
//   server -> client  ServerHello  secret(1) nonce_s(32) ephemeral_s(65) cpace_s(32)
//                                  salt(16) identity_s(146) finished_s(32)
//   client -> server  Finished     identity_c(146) finished_c(32)
//
// psk: the connection key, PBKDF2(password, salt), the first 32 bytes of
// CPace's ISK, or zeros (Credentials). PIN: cpace_c and cpace_s are the CPace
// shares (crypto/cpace.hpp, CPACE-X25519-SHA512) with PRS = the PIN (UTF-8),
// sid = nonce_c, CI = lv_cat(magic, "rcsdk client", "rcsdk server") and empty
// associated data, the client the initiator; with any other secret they are
// zeros. Nothing else derives from the PIN: a recorded handshake lets nobody
// test guesses, and each handshake lets the peer test one (finished_x): the
// server counts the answers it gives (PinAttempts), the client tests one
// answer per hello (ClientHandshake::finish).
// keys = HKDF(salt = psk, ECDH(ephemeral_c, ephemeral_s), "rcsdk keys" ||
// hellos), the hellos being ClientHello and ServerHello up to the salt; their
// last 32 bytes are the exporter, which nothing uses as a key itself: the
// connection id each end receives under (the first four bytes of
// HMAC(exporter, "rcsdk cid server") for the server's, "rcsdk cid client" for
// the client's) and the keys of signals about the connection derive from it.
// identity_x is sealed (crypto::Aead, x's identity keys): present(1)
// public_key(65) signature(64), the signature by x's device key over a role
// label and everything before it, so it binds the key to this exchange; a
// passive observer learns no device key. finished_x = HMAC(finished key of x,
// everything before it) proves that x holds the psk and saw the same messages.
// The ephemeral keys are discarded after the handshake (forward secrecy).
#pragma once

#include "crypto/cpace.hpp"
#include "crypto/crypto.hpp"
#include "transport/credentials.hpp"
#include "util/util.hpp"

#include <array>
#include <memory>
#include <optional>
#include <span>

namespace rcsdk::transport {

inline constexpr uint8_t protocolMagic[8] = {'R', 'C', 'S', 'N', 'E', 'T', 0, 1};
inline constexpr size_t protocolMagicSize = sizeof(protocolMagic);
inline constexpr uint16_t protocolVersion = uint16_t(protocolMagic[6] << 8 | protocolMagic[7]);
inline constexpr uint8_t forwarderMagic[8] = {'R', 'C', 'S', 'F', 'W', 'D', protocolMagic[6], protocolMagic[7]};
inline constexpr size_t identitySize = 1 + 65 + 64;
inline constexpr size_t sealedIdentitySize = identitySize + crypto::Aead::tagSize;
inline constexpr size_t cpaceShareSize = 32;
inline constexpr size_t clientHelloSize = 8 + 1 + 32 + crypto::Ecdh::publicKeySize + cpaceShareSize;
inline constexpr size_t serverHelloSize =
    1 + 32 + crypto::Ecdh::publicKeySize + cpaceShareSize + 16 + sealedIdentitySize + 32;
inline constexpr size_t finishedSize = sealedIdentitySize + 32;

// Which end of a handshake: the server answers hellos (the sharer, an agent
// link's session side), the client sends them.
enum class Side { server, client };

struct TrafficKeys {
    std::unique_ptr<crypto::Aead> send, receive;
    std::optional<crypto::PublicKey> peerKey; // The device key the peer proved, if it has one.
    // The connection ids datagrams carry: those this end sends (the peer's)
    // and those it receives.
    uint32_t sendId = 0, receiveId = 0;
    crypto::Key exporter{};
};

// What a server answers a hello of another protocol version with instead of
// a ServerHello: its own magic, of the hello's protocol (the session's or the
// forwarder's). It is no larger than any hello (nothing larger is reflected
// to a forged source), and a ServerHello, which starts with the kind of
// secret, never starts like it.
std::span<const uint8_t> versionNotice(std::span<const uint8_t> hello = {}) noexcept;
// The protocol version `bytes` name when they start with the magic of another
// version of this protocol or the forwarder's (a hello, a version notice);
// nothing for this version's and for anything else.
std::optional<uint16_t> otherProtocolVersion(std::span<const uint8_t> bytes) noexcept;
// Failure(UNSUPPORTED) naming both versions; `peer` is the peer's.
[[noreturn]] void versionMismatch(uint16_t peer);

// What a server answers a PIN hello with instead of a ServerHello once no
// attempt is left (PinLocked): "RCSPIN" and the version, as large as the
// version notice. Unauthenticated like it: a forger on the path can only make
// the client give up, which dropping its packets does as well.
std::span<const uint8_t> pinLockedNotice() noexcept;
// Throws Failure(AUTHENTICATION) when `bytes` are the PIN notice.
void checkPinLockedNotice(std::span<const uint8_t> bytes);

// Client side: builds the hello, then checks the server's and answers it.
// `credentials` must outlive it.
class ClientHandshake {
public:
    explicit ClientHandshake(const Credentials& credentials);
    const crypto::Bytes& hello() const noexcept { return hello_; }
    // Throws Failure(AUTHENTICATION) when the server does not hold the secret,
    // UntrustedPeer when it proves no device key this end trusts (another, or
    // none); the handshake stays as it was, so another answer may still be
    // tried. A PIN's, once one answer reached the PIN's test (spent()): that
    // test is a guess for whoever answered, from the server's address or
    // not, so every later answer fails untested.
    crypto::Bytes finish(std::span<const uint8_t> serverHello, TrafficKeys& keys);
    // PIN: whether an answer was tested; no other one will be.
    bool spent() const noexcept { return spent_; }
    // Whether finishing with this answer runs PBKDF2 (a password, and a salt
    // whose key is not at hand): what a forged answer could make it repeat.
    bool costly(std::span<const uint8_t> serverHello) const;

private:
    const Credentials& credentials_;
    crypto::Ecdh ephemeral_;
    crypto::cpace::Point pinScalar_{}; // PIN
    bool spent_ = false;               // PIN
    crypto::Bytes hello_;
};

// Server side: answers a hello, then checks the client's finished message.
// `credentials` must outlive it.
class ServerHandshake {
public:
    // Checks `clientHello` before it costs a key pair: Failure(UNSUPPORTED)
    // for another protocol version, AUTHENTICATION for a malformed one. Then
    // PinLocked for a PIN hello when no attempt is left: a PIN hello holds an
    // attempt until this ends, a failure once answered (a share of low order
    // is refused before, at no cost); finish() proving the client returns it.
    ServerHandshake(const Credentials& credentials, std::span<const uint8_t> clientHello);
    // The ServerHello.
    const crypto::Bytes& answer() const noexcept { return answer_; }
    // Throws Failure(AUTHENTICATION) when the client does not hold the secret;
    // this end's state stays as it was, so the real client's finished message
    // may still follow a forged one. UntrustedPeer, when it proves no device
    // key this end trusts (another, or none), comes with `keys` set: the
    // caller may tell the client it was refused under them (only this end
    // could), then drops it.
    void finish(std::span<const uint8_t> clientFinished, TrafficKeys& keys) const;

private:
    const Credentials& credentials_;
    crypto::Bytes transcript_; // The hello, checked before the key pair below; then up to finished_s.
    crypto::Ecdh ephemeral_;
    std::unique_ptr<PinAttempts::Attempt> pinAttempt_;
    crypto::Bytes keyMaterial_, answer_;
};

} // namespace rcsdk::transport
