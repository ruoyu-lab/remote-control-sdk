#include "transport/secure_channel.hpp"

#include "util/bytes.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace rcsdk::transport {
namespace {

using crypto::Bytes;

constexpr size_t headerSize = 4 + 8; // length, sequence
constexpr auto ioTimeout = 5s;       // Peers send a heartbeat every second.
// A real client sends its hello right after connecting.
constexpr auto helloTimeout = 2s;

} // namespace

SecureChannel::SecureChannel(std::unique_ptr<Stream> stream, Side side, const Credentials& credentials,
                             const std::function<crypto::Bytes(const std::string&)>& refusal)
    : stream_(std::move(stream)) {
    handshake(side, credentials, refusal);
}

SecureChannel::SecureChannel(std::unique_ptr<Stream> stream, TrafficKeys keys)
    : stream_(std::move(stream)), sendKeys_(std::move(keys.send)), receiveKeys_(std::move(keys.receive)),
      peerKey_(keys.peerKey) {}

void SecureChannel::handshake(Side side, const Credentials& credentials,
                              const std::function<crypto::Bytes(const std::string&)>& refusal) {
    TrafficKeys keys;
    if (side == Side::server) {
        Bytes hello(clientHelloSize);
        stream_->read(std::span(hello).first(protocolMagicSize), helloTimeout);
        if (const auto version = otherProtocolVersion(hello)) {
            try { // Tell the client which version it reached; it fails with both named.
                stream_->write(versionNotice(), 250ms);
            } catch (const Failure&) {
            }
            versionMismatch(*version);
        }
        stream_->read(std::span(hello).subspan(protocolMagicSize), helloTimeout);
        std::optional<ServerHandshake> server;
        try {
            server.emplace(credentials, hello);
        } catch (const PinLocked&) {
            try { // So that the client can say why.
                stream_->write(pinLockedNotice(), 250ms);
            } catch (const Failure&) {
            }
            throw;
        }
        stream_->write(server->answer(), ioTimeout);
        Bytes finished(finishedSize);
        stream_->read(finished, ioTimeout);
        try {
            server->finish(finished, keys);
        } catch (const UntrustedPeer& refused) {
            if (refusal) {
                // The last words before closing: sent so that closing cannot
                // reset the connection under them (Windows discards what the
                // peer has not read yet when a reset arrives).
                sendKeys_ = std::move(keys.send);
                sendFinal(refusal(refused.what()));
            }
            throw;
        }
    } else {
        ClientHandshake client(credentials);
        stream_->write(client.hello(), ioTimeout);
        // A server of another version answers with its magic instead.
        Bytes reply(serverHelloSize);
        stream_->read(std::span(reply).first(protocolMagicSize), ioTimeout);
        if (const auto version = otherProtocolVersion(reply)) {
            versionMismatch(*version);
        }
        checkPinLockedNotice(std::span(reply).first(protocolMagicSize));
        stream_->read(std::span(reply).subspan(protocolMagicSize), ioTimeout);
        stream_->write(client.finish(reply, keys), ioTimeout);
    }
    sendKeys_ = std::move(keys.send);
    receiveKeys_ = std::move(keys.receive);
    peerKey_ = keys.peerKey;
}

void SecureChannel::send(std::span<const uint8_t> payload) {
    send(payload, false);
}

void SecureChannel::sendFinal(std::span<const uint8_t> payload) {
    send(payload, true);
}

void SecureChannel::send(std::span<const uint8_t> payload, bool final) {
    if (payload.empty() || payload.size() > maxRecord) {
        fail(RCS_ERROR_INTERNAL, "A message of " + std::to_string(payload.size()) + " bytes is outside the " +
                                     std::to_string(maxRecord >> 20) + " MiB record limit");
    }
    std::lock_guard lock(sendMutex_);
    if (sendBroken_) {
        // A record was cut short with its sequence number unused: another
        // record would reuse it (and its nonce), and the peer could not tell
        // it from the rest of the broken one. The last words are left out.
        if (final) {
            return;
        }
        fail(RCS_ERROR_NETWORK, "An earlier record was cut short");
    }
    // The sequence number counts only records that went out: a send that was
    // cancelled before its first byte leaves it as it was (Stream's contract).
    const uint64_t sequence = sendSequence_ + 1;
    Bytes record;
    record.reserve(headerSize + payload.size() + crypto::Aead::tagSize);
    appendBigEndian(record, payload.size(), 4);
    appendBigEndian(record, sequence, 8);
    const auto sealed = sendKeys_->seal(sequence, record, payload);
    record.insert(record.end(), sealed.begin(), sealed.end());
    try {
        if (final) {
            stream_->writeFinal(record, 250ms);
        } else {
            stream_->write(record, ioTimeout);
        }
    } catch (const Failure& failure) {
        sendBroken_ = failure.result() != RCS_ERROR_CANCELLED; // Else nothing was written.
        throw;
    }
    sendSequence_ = sequence;
    lastSend_ = Clock::now();
}

crypto::Bytes SecureChannel::receive() {
    Bytes record(headerSize);
    stream_->read(record, ioTimeout);
    const uint64_t started = monotonicNs();
    const uint64_t size = loadBigEndian(record, 0, 4);
    const uint64_t sequence = loadBigEndian(record, 4, 8);
    if (!size || size > maxRecord || sequence != receiveSequence_ + 1) {
        fail(RCS_ERROR_NETWORK, "Invalid record length or sequence");
    }
    record.resize(headerSize + size + crypto::Aead::tagSize);
    stream_->read(std::span(record).subspan(headerSize), ioTimeout);
    const uint64_t received = monotonicNs();
    auto payload =
        receiveKeys_->open(sequence, std::span(record).first(headerSize), std::span(record).subspan(headerSize));
    if (!payload) {
        fail(RCS_ERROR_AUTHENTICATION, "Record authentication failed");
    }
    ++receiveSequence_;
    receiveNs_ = received - started;
    decryptNs_ = monotonicNs() - received;
    return std::move(*payload);
}

} // namespace rcsdk::transport
