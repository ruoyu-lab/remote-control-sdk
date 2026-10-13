// Datagrams over a byte stream to one server (TCP, or TLS on it), seen as a
// datagram socket whose only peer is that server: each datagram framed as
// the server's protocol frames it (a Framing), so that what runs on datagram
// sockets runs on it unchanged. TURN over TCP and TLS (turn_tcp_socket.hpp)
// and a forwarder's TCP and WebSocket entries (session/forwarder.hpp) use it.
#pragma once

#include "transport/socket.hpp"
#include "transport/tls.hpp"

#include <deque>
#include <memory>
#include <string>

namespace rcsdk::transport {

// How datagrams travel in a byte stream.
class Framing {
public:
    virtual ~Framing() = default;
    // The datagram as it goes on the stream.
    virtual Bytes wrap(std::span<const uint8_t> datagram) = 0;
    // The stream's next bytes; pushed only once pop() has nothing, so that
    // what waits stays below a frame and a read.
    virtual void push(std::span<const uint8_t> bytes) = 0;
    // The next datagram that arrived whole; what the stream asks to be
    // answered (a WebSocket ping) goes to `answer`. Throws NETWORK for bytes
    // that break the framing, or that end the stream.
    virtual std::optional<Bytes> pop(Bytes& answer) = 0;
};

// Each datagram behind its length (two bytes, big-endian): a forwarder's TCP entry.
std::unique_ptr<Framing> lengthFraming();
// Each datagram a binary WebSocket message (websocket.hpp), this end the
// client: a forwarder's WebSocket entry, or a proxy in front of it.
std::unique_ptr<Framing> webSocketFraming();

// The TCP connection under a framed socket, and under TLS where the SDK
// does TLS on its own socket (Windows, BearSSL): see connectTcp; without
// Nagle's delay, with room for a keyframe burst each way.
Socket connectStreamTcp(const Endpoint& server, const std::string& what, CancelToken cancelled,
                        std::chrono::milliseconds timeout, uint32_t interfaceIndex);
// That connection as a byte stream without TLS; `what` names the server in
// reasons ("the TURN server 192.0.2.1:3478").
std::unique_ptr<ByteStream> tcpBytes(Socket socket, std::string what);

class FramedSocket : public DatagramSocket {
public:
    // `arrived`: the stream's bytes read past an opening (a WebSocket's).
    FramedSocket(std::unique_ptr<ByteStream> stream, Endpoint server, std::unique_ptr<Framing> framing,
                 std::span<const uint8_t> arrived = {});
    // What waits here goes out first, for a second at most (ByteStream::close).
    ~FramedSocket() override;

    Endpoint local() const override { return stream_->local(); }
    Handle handle() const noexcept override { return stream_->handle(); }
    // `to` must be the server. Never blocks: what the connection cannot take
    // yet waits here, up to 64 KiB; a datagram beyond that is dropped
    // (false), as a full UDP socket would. Throws NETWORK once the
    // connection broke.
    bool send(const Endpoint& to, std::span<const uint8_t> datagram) override;
    // The batch's datagrams to the server framed one after another and
    // written at once. Before one that would take what waits beyond 64 KiB,
    // what waits is written as far as the connection takes it (no more in
    // this batch once it took less than all); the datagram is dropped when
    // it still does not fit, and later ones that fit go on, as send() would.
    size_t sendBatch(std::span<const Outgoing> batch) override;
    // Reads from the connection only when no whole datagram waits here: the
    // system's buffer holds the rest back.
    std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint& from) override;
    bool buffered() const noexcept override;

private:
    // Writes what waits in outbound_ as far as the connection takes it.
    void flush();
    // `datagram` framed behind what waits, unless that would then be beyond
    // 64 KiB even after flush(); `full`: a flush left something waiting, and
    // none is tried again.
    bool queue(std::span<const uint8_t> datagram, bool& full);

    std::unique_ptr<ByteStream> stream_;
    Endpoint server_;
    std::unique_ptr<Framing> framing_;
    std::deque<Bytes> ready_;        // Datagrams whole, not yet received.
    Bytes outbound_;                 // Written in order before anything new.
    Bytes chunk_ = Bytes(64 * 1024); // What one read takes.
};

} // namespace rcsdk::transport
