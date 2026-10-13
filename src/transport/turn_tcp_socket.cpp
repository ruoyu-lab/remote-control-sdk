#include "transport/turn_tcp_socket.hpp"

#include "util/bytes.hpp"

namespace rcsdk::transport {
namespace {

constexpr size_t stunHeader = 20, channelHeader = 4;

class TurnFraming final : public Framing {
public:
    Bytes wrap(std::span<const uint8_t> datagram) override { return turnWire(datagram); }
    void push(std::span<const uint8_t> bytes) override {
        buffer_.erase(buffer_.begin(), buffer_.begin() + ptrdiff_t(begin_));
        begin_ = 0;
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }
    std::optional<Bytes> pop(Bytes&) override {
        const auto waiting = std::span<const uint8_t>(buffer_).subspan(begin_);
        const auto frame = turnFrame(waiting);
        if (!frame || waiting.size() < frame->wire) {
            return std::nullopt;
        }
        begin_ += frame->wire;
        return Bytes(waiting.begin(), waiting.begin() + ptrdiff_t(frame->message));
    }

private:
    Bytes buffer_;
    size_t begin_ = 0;
};

} // namespace

std::optional<TurnFrame> turnFrame(std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        return std::nullopt;
    }
    if (bytes[0] >= 0x40 && bytes[0] <= 0x7F) { // Channel data.
        if (bytes.size() < channelHeader) {
            return std::nullopt;
        }
        const size_t message = channelHeader + size_t(loadBigEndian(bytes, 2, 2));
        return TurnFrame{message, (message + 3) & ~size_t(3)};
    }
    if ((bytes[0] & 0xC0) != 0) {
        fail(RCS_ERROR_NETWORK, "The TURN server sent something that is not a TURN message");
    }
    if (bytes.size() < stunHeader) {
        return std::nullopt;
    }
    const size_t message = stunHeader + size_t(loadBigEndian(bytes, 2, 2));
    return TurnFrame{message, message};
}

std::vector<uint8_t> turnWire(std::span<const uint8_t> message) {
    std::vector<uint8_t> wire(message.begin(), message.end());
    if (!message.empty() && message[0] >= 0x40 && message[0] <= 0x7F) {
        wire.resize((wire.size() + 3) & ~size_t(3), 0);
    }
    return wire;
}

TurnTcpSocket::TurnTcpSocket(std::unique_ptr<ByteStream> stream, Endpoint server)
    : FramedSocket(std::move(stream), std::move(server), std::make_unique<TurnFraming>()) {}

std::unique_ptr<TurnTcpSocket> TurnTcpSocket::connect(const Endpoint& server, CancelToken cancelled,
                                                      std::chrono::milliseconds timeout, uint32_t interfaceIndex,
                                                      const TlsSettings* tls) {
    const std::string what = "the TURN server " + server.toString();
    std::unique_ptr<ByteStream> stream;
    if (tls) {
        stream = connectTls(server, *tls, what, cancelled, timeout, interfaceIndex);
    } else {
        stream = tcpBytes(connectStreamTcp(server, what + " over TCP", cancelled, timeout, interfaceIndex), what);
    }
    return std::unique_ptr<TurnTcpSocket>(new TurnTcpSocket(std::move(stream), server));
}

} // namespace rcsdk::transport
