// Mutually authenticated, encrypted record channel over a Stream.
//
// Handshake (see handshake.hpp): ephemeral P-256 key agreement bound to the
// credentials (a shared secret, device keys), so only the right peer can
// complete it, and recorded traffic stays secret even if a secret leaks later
// (forward secrecy).
// Records are crypto::Aead with a strictly increasing sequence number, so
// replayed, reordered or tampered records are rejected. A record carries at
// most 64 MiB, more than any encoded picture.
#pragma once

#include "crypto/crypto.hpp"
#include "transport/credentials.hpp"
#include "transport/handshake.hpp"
#include "transport/stream.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace rcsdk::transport {

// Above any encoded picture. Encoders stay at about a picture's uncompressed
// size at most (raw I_PCM blocks and their headers: about 1% more), and the
// largest picture a stream may be (35 651 584 pixels, 8192x4352) is 53.5 MB
// as NV12; at 500 Mbit/s and 1 FPS a picture's whole budget, 62.5 MB, still
// fits. The hosts' own packets are held to it (protocol::maxVideoBytes). At
// most 64 MiB, a UDP message's fragments at the smallest datagram stay
// within their 16-bit numbers. A receiver holds at most one record of it
// before it is authenticated.
inline constexpr size_t maxRecord = 64 * 1024 * 1024;

class SecureChannel {
public:
    using Side = transport::Side;

    // Runs the handshake; throws Failure (AUTHENTICATION, NETWORK, UNSUPPORTED
    // for another protocol version) when the peer fails it. Server: a peer
    // refused for its device key first gets `refusal(reason)` as a record
    // under the agreed keys; a peer of another protocol version gets this
    // end's (versionNotice), and a PIN hello when no attempt is left the
    // PIN notice.
    SecureChannel(std::unique_ptr<Stream> stream, Side side, const Credentials& credentials,
                  const std::function<crypto::Bytes(const std::string&)>& refusal = {});
    // Over a handshake run elsewhere (a server working on several at once).
    SecureChannel(std::unique_ptr<Stream> stream, TrafficKeys keys);

    // Thread-safe.
    void send(std::span<const uint8_t> payload);
    // The last record of the connection: sent even when the run is cancelled,
    // waiting at most 250 ms for the peer's socket.
    void sendFinal(std::span<const uint8_t> payload);
    // Single reader. Fails if nothing arrives within the idle timeout.
    crypto::Bytes receive();

    uint64_t bytesTransferred() const noexcept { return stream_->bytesTransferred(); }
    // When the last record went out (any thread; never waits for a send under way).
    Clock::time_point lastSend() const noexcept { return lastSend_.load(); }
    // The record receive() returned last, for its reader: receiving it from its
    // header on (not the wait before it), decrypting it.
    uint64_t lastReceiveNs() const noexcept { return receiveNs_; }
    uint64_t lastDecryptNs() const noexcept { return decryptNs_; }
    // The device key the peer proved in the handshake, if it has one.
    const std::optional<crypto::PublicKey>& peerKey() const noexcept { return peerKey_; }

private:
    void send(std::span<const uint8_t> payload, bool final);
    void handshake(Side side, const Credentials& credentials,
                   const std::function<crypto::Bytes(const std::string&)>& refusal);

    std::unique_ptr<Stream> stream_;
    std::unique_ptr<crypto::Aead> sendKeys_, receiveKeys_;
    std::mutex sendMutex_;
    uint64_t sendSequence_ = 0, receiveSequence_ = 0;
    bool sendBroken_ = false; // A send failed part-way: nothing can follow on this stream.
    std::atomic<Clock::time_point> lastSend_{Clock::now()};
    uint64_t receiveNs_ = 0, decryptNs_ = 0;
    std::optional<crypto::PublicKey> peerKey_;
};

} // namespace rcsdk::transport
