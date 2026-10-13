// A TCP connection to a TURN server (RFC 8656 over TCP, or over TLS on TCP),
// seen as a datagram socket whose only peer is the server: STUN messages and
// channel data carry their own lengths (channel data is padded to four bytes
// on the wire), so the TURN client, ICE and the UDP transport work on it
// unchanged. For networks that pass TCP or TLS only; the relayed transport
// between the ends stays UDP.
#pragma once

#include "transport/framed_socket.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace rcsdk::transport {

// The TURN message at the start of `bytes`: what it is (`message`: a STUN
// message, or channel data without padding) and what it takes on the wire;
// nothing while too few bytes arrived. Throws NETWORK for bytes that are not a
// TURN message.
struct TurnFrame {
    size_t message = 0, wire = 0;
};
std::optional<TurnFrame> turnFrame(std::span<const uint8_t> bytes);
// Channel data as sent over TCP: padded to a multiple of four bytes.
std::vector<uint8_t> turnWire(std::span<const uint8_t> message);

// TURN messages over the connection (framed_socket.hpp).
class TurnTcpSocket final : public FramedSocket {
public:
    // Connects within `timeout`, with `tls` TLS on the connection, its
    // handshake within the same time (tls.hpp); `interfaceIndex` 0: any
    // interface.
    static std::unique_ptr<TurnTcpSocket> connect(const Endpoint& server, CancelToken cancelled,
                                                  std::chrono::milliseconds timeout, uint32_t interfaceIndex = 0,
                                                  const TlsSettings* tls = nullptr);

private:
    TurnTcpSocket(std::unique_ptr<ByteStream> stream, Endpoint server);
};

} // namespace rcsdk::transport
