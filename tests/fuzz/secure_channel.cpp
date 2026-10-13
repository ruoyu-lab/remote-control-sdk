// libFuzzer: the encrypted record stream (transport/secure_channel.cpp) as a
// reader takes it, record after record, and the messages in them
// (protocol::decode): what an authenticated peer's records, and an attacker's
// bytes among them, do to the channel's state (the sequence it expects) and
// to its buffers. The input spells out what the stream carries: records
// sealed with the channel's keys (the next sequence number, another one, a
// length the input gives, one byte changed), cut short, or bytes of its own.
// The reader goes on until the channel fails (Failure, as a connection would
// end); anything else is a finding.
#include "protocol/messages.hpp"
#include "transport/secure_channel.hpp"
#include "util/bytes.hpp"

#include <algorithm>
#include <cstring>

using namespace rcsdk;

namespace {

// The bytes of the stream, read as the channel asks; it ends with them.
class Recorded final : public transport::Stream {
public:
    explicit Recorded(crypto::Bytes bytes) : bytes_(std::move(bytes)) {}
    void read(std::span<uint8_t> buffer, std::chrono::milliseconds) override {
        if (bytes_.size() - at_ < buffer.size()) {
            fail(RCS_ERROR_DISCONNECTED, "The stream ended");
        }
        std::memcpy(buffer.data(), bytes_.data() + at_, buffer.size());
        at_ += buffer.size();
    }
    void write(std::span<const uint8_t>, std::chrono::milliseconds) override {}
    void writeFinal(std::span<const uint8_t>, std::chrono::milliseconds) override {}
    uint64_t bytesTransferred() const noexcept override { return at_; }

private:
    crypto::Bytes bytes_;
    size_t at_ = 0;
};

const crypto::Key cipherKey{9, 8, 7}, macKey{6, 5, 4};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const crypto::Aead peer(cipherKey, macKey);
    crypto::Bytes stream;
    uint64_t sequence = 0;
    size_t at = 0;
    const auto byte = [&] {
        return at < size ? data[at++] : uint8_t(0);
    };
    const auto bytes = [&](size_t count) {
        count = std::min(count, size - at);
        crypto::Bytes out(data + at, data + at + count);
        at += count;
        return out;
    };
    const auto record = [&](uint64_t number, std::span<const uint8_t> payload) {
        crypto::Bytes out;
        appendBigEndian(out, payload.size(), 4);
        appendBigEndian(out, number, 8);
        const auto sealed = peer.seal(number, out, payload);
        out.insert(out.end(), sealed.begin(), sealed.end());
        return out;
    };
    while (at < size) {
        switch (byte() % 6) {
        case 0: { // The next record, a payload of the input's.
            const auto r = record(++sequence, bytes(1 + byte() * 8));
            stream.insert(stream.end(), r.begin(), r.end());
            break;
        }
        case 1: { // A record numbered near the next.
            const auto r = record(sequence + 1 + int8_t(byte()), bytes(1 + byte()));
            stream.insert(stream.end(), r.begin(), r.end());
            break;
        }
        case 2: { // A header claiming a length of the input's (now and then the largest, or past it).
            static constexpr uint32_t edges[] = {0, transport::maxRecord, transport::maxRecord + 1, 0xFFFFFFFF};
            const uint8_t how = byte();
            appendBigEndian(stream, how < 8 ? edges[how % 4] : uint32_t(byte()) << (how % 12), 4);
            appendBigEndian(stream, sequence + 1, 8);
            break;
        }
        case 3: { // The next record with one byte changed.
            auto r = record(sequence + 1, bytes(1 + byte()));
            r[byte() % r.size()] ^= uint8_t(1 + byte() % 255);
            stream.insert(stream.end(), r.begin(), r.end());
            break;
        }
        case 4: { // The next record, cut short.
            auto r = record(++sequence, bytes(1 + byte()));
            r.resize(byte() % r.size());
            stream.insert(stream.end(), r.begin(), r.end());
            break;
        }
        case 5: { // Bytes of the input's own.
            const auto raw = bytes(byte());
            stream.insert(stream.end(), raw.begin(), raw.end());
            break;
        }
        }
    }
    transport::TrafficKeys keys;
    keys.send = std::make_unique<crypto::Aead>(macKey, cipherKey);
    keys.receive = std::make_unique<crypto::Aead>(cipherKey, macKey);
    transport::SecureChannel channel(std::make_unique<Recorded>(std::move(stream)), std::move(keys));
    try {
        for (;;) {
            const auto payload = channel.receive();
            try {
                protocol::decode(payload);
            } catch (const Failure&) {
                // Not a message: the connection fails.
            }
        }
    } catch (const Failure&) {
        // The channel failed, or the stream ended.
    }
    return 0;
}
