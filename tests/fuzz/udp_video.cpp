// libFuzzer: a viewer's UDP connection putting video together from its
// fragments and parity (udp_connection.cpp's receiveFragment, receiveParity,
// recover, deliverFrames; fec.cpp), with reliable messages, resend requests'
// answers and time passing beside it, over many steps: the state a peer's
// datagrams build up, not one datagram at a time. The input spells the
// steps out; each becomes datagrams sealed as the sharer's, so all of them
// get past authentication. A step that breaks the protocol ends the input as
// it would end the connection (Failure); anything else (a crash, an
// AddressSanitizer or UndefinedBehaviorSanitizer report) is a finding.
//
// Steps, by their first byte:
//   picture: a valid picture of a stream, cut into fragments of a size the
//            input gives, each group of 64 with the parity the input asks
//            for (computed as the sharer does), sent in the input's order
//            with some left out or sent twice: what parity repairs
//   video / parity: one fragment with the input's header fields and bytes
//   gone, message (reliable), raw frame (any type), time passes (and the
//   network thread's round), take what the session would receive
#include "crypto/crypto.hpp"
#include "protocol/fec.hpp"
#include "protocol/messages.hpp"
#include "protocol/udp_connection.hpp"
#include "util/bytes.hpp"

#include <cstring>

using namespace rcsdk;
using namespace rcsdk::protocol;

namespace {

// The input, read from its start; zeros once it ran out.
class Steps {
public:
    Steps(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool more() const noexcept { return at_ < size_; }
    uint8_t byte() noexcept { return at_ < size_ ? data_[at_++] : 0; }
    uint16_t u16() noexcept { return uint16_t(byte() << 8 | byte()); }
    uint32_t u32() noexcept { return uint32_t(u16()) << 16 | u16(); }
    crypto::Bytes bytes(size_t count) {
        count = std::min(count, size_ - at_);
        crypto::Bytes out(data_ + at_, data_ + at_ + count);
        at_ += count;
        return out;
    }

private:
    const uint8_t* data_;
    size_t size_, at_ = 0;
};

const crypto::Key cipherKey{1, 2, 3}, macKey{4, 5, 6};
constexpr uint32_t viewerId = 0x01020304;

// The sharer's end: seals frames as datagrams to the viewer.
struct Sharer {
    crypto::Aead keys{cipherKey, macKey};
    uint64_t packet = 0;
    crypto::Bytes seal(std::span<const uint8_t> frame) {
        crypto::Bytes datagram{dataKind};
        appendBigEndian(datagram, viewerId, 4);
        appendBigEndian(datagram, ++packet, 8);
        const auto sealed = keys.seal(packet, datagram, frame);
        datagram.insert(datagram.end(), sealed.begin(), sealed.end());
        return datagram;
    }
};

// A picture of `size` bytes (valid H.264 framing) of `stream`, as encoded on the wire.
crypto::Bytes picture(uint8_t stream, uint64_t sequence, size_t size, bool keyframe) {
    EncodedPacket packet;
    packet.stream = stream;
    packet.codec = RCS_CODEC_H264;
    packet.width = 1920;
    packet.height = 1080;
    packet.keyframe = keyframe;
    packet.sequence = sequence;
    const size_t payload = size - 4;
    packet.data = {uint8_t(payload >> 24), uint8_t(payload >> 16), uint8_t(payload >> 8), uint8_t(payload),
                   uint8_t(keyframe ? 0x65 : 0x41)};
    packet.data.resize(size, uint8_t(sequence));
    if (keyframe) {
        packet.config = {0, 0, 0, 2, 0x67, 0x42, 0, 0, 0, 2, 0x68, 0xce};
    }
    return encode(packet);
}

crypto::Bytes videoHeader(uint8_t type, uint8_t stream, uint32_t frame, uint16_t index, uint16_t count,
                          uint8_t parity) {
    crypto::Bytes header{type, stream};
    appendBigEndian(header, frame, 4);
    appendBigEndian(header, index, 2);
    appendBigEndian(header, count, 2);
    appendBigEndian(header, 0, 4); // Sent at.
    header.push_back(parity);
    return header;
}

// The fragments and parity of `body` as frame `frame` of `stream`, in sending order.
std::vector<crypto::Bytes> fragments(uint8_t stream, uint32_t frame, std::span<const uint8_t> body, size_t size,
                                     uint8_t parityPerGroup) {
    const size_t count = (body.size() + size - 1) / size;
    std::vector<crypto::Bytes> out;
    const auto piece = [&](size_t i) {
        return body.subspan(i * size, std::min(size, body.size() - i * size));
    };
    for (size_t first = 0; first < count; first += 64) {
        const size_t k = std::min<size_t>(64, count - first);
        for (size_t i = first; i < first + k; ++i) {
            auto fragment = videoHeader(5, stream, frame, uint16_t(i), uint16_t(count), parityPerGroup);
            const auto bytes = piece(i);
            fragment.insert(fragment.end(), bytes.begin(), bytes.end());
            out.push_back(std::move(fragment));
        }
        if (!parityPerGroup) {
            continue;
        }
        std::vector<std::span<const uint8_t>> group;
        for (size_t i = first; i < first + k; ++i) {
            group.push_back(piece(i));
        }
        const size_t blockSize = group.front().size(), last = group.back().size();
        const auto blocks = fec::encode(group, parityPerGroup, blockSize);
        for (size_t j = 0; j < parityPerGroup; ++j) {
            auto fragment = videoHeader(12, stream, frame, uint16_t(first / 64), uint16_t(count), parityPerGroup);
            fragment.push_back(uint8_t(j));
            appendBigEndian(fragment, last, 2);
            fragment.insert(fragment.end(), blocks[j].begin(), blocks[j].end());
            out.push_back(std::move(fragment));
        }
    }
    return out;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Steps in(data, size);
    transport::TrafficKeys keys;
    keys.send = std::make_unique<crypto::Aead>(macKey, cipherKey); // Never used: it sends nowhere.
    keys.receive = std::make_unique<crypto::Aead>(cipherKey, macKey);
    keys.sendId = 0x0A0B0C0D;
    keys.receiveId = viewerId;
    UdpOptions options;
    options.maxInbound = 64u << 10; // Reached now and then: the session falling behind is part of it.
    UdpFuzzEnd viewer(false, std::move(keys), options);
    Sharer sharer;
    auto now = Clock::now();
    std::array<uint32_t, RCS_MAX_STREAMS> frames{};
    uint64_t sequence = 0;
    try {
        const auto send = [&](std::span<const uint8_t> frame) {
            viewer.datagram(sharer.seal(frame), now);
        };
        while (in.more()) {
            switch (in.byte() % 8) {
            case 0: { // A picture, its fragments sent as the input says.
                const uint8_t stream = in.byte() % RCS_MAX_STREAMS;
                const uint32_t frame = ++frames[stream] + (in.byte() % 4 == 0 ? in.byte() % 8 : 0);
                const auto body = picture(stream, ++sequence, 16 + in.u16() % 20000, in.byte() & 1);
                const size_t fragment = 16 + in.u16() % 1200;
                const uint8_t parity = in.byte() % 5;
                for (const auto& piece : fragments(stream, frame, body, fragment, parity)) {
                    const uint8_t what = in.byte();
                    if (what % 8 == 0) {
                        continue; // Lost.
                    }
                    send(piece);
                    if (what % 8 == 1) {
                        send(piece); // Twice.
                    }
                }
                break;
            }
            case 1: { // A fragment as the input makes it.
                auto fragment = videoHeader(5, in.byte() % RCS_MAX_STREAMS, in.byte(), in.u16() % 300, in.u16() % 300,
                                            in.byte() % 8);
                const auto bytes = in.bytes(in.byte() * 4);
                fragment.insert(fragment.end(), bytes.begin(), bytes.end());
                send(fragment);
                break;
            }
            case 2: { // Parity as the input makes it.
                auto fragment = videoHeader(12, in.byte() % RCS_MAX_STREAMS, in.byte(), in.u16() % 8, in.u16() % 300,
                                            in.byte() % 8);
                fragment.push_back(in.byte() % 8);
                appendBigEndian(fragment, in.u16() % 1300, 2);
                const auto bytes = in.bytes(in.byte() * 4);
                fragment.insert(fragment.end(), bytes.begin(), bytes.end());
                send(fragment);
                break;
            }
            case 3: { // Video the sharer no longer has.
                crypto::Bytes gone{11};
                for (int n = 1 + in.byte() % 4; n > 0; --n) {
                    gone.push_back(in.byte() % RCS_MAX_STREAMS);
                    appendBigEndian(gone, in.byte(), 4);
                }
                send(gone);
                break;
            }
            case 4: { // A reliable message: a data part, numbered near the next one.
                crypto::Bytes message{3, 1};
                appendBigEndian(message, in.u16() % 64, 4);
                const auto part = encode(DataPart{DataPart::data, 0, 1, 0, 1, in.bytes(in.byte())});
                message.insert(message.end(), part.begin(), part.end());
                send(message);
                break;
            }
            case 5: // Any frame.
                send(in.bytes(in.u16() % 1400));
                break;
            case 6: // Time passes; the network thread's round.
                now += std::chrono::milliseconds(in.byte());
                viewer.tick(now);
                break;
            case 7: // The session takes what waits for it.
                while (viewer.take()) {
                }
                break;
            }
        }
        viewer.tick(now + std::chrono::seconds(2));
    } catch (const Failure&) {
        // The connection failed: the peer broke the protocol.
    }
    return 0;
}
