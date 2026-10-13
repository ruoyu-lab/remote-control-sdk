#include "transport/framed_socket.hpp"

#include "transport/socket_error.hpp"
#include "transport/websocket.hpp"
#include "util/bytes.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace rcsdk::transport {
namespace {

constexpr size_t maxOutbound = 64 * 1024;

#ifndef _WIN32
#ifdef MSG_NOSIGNAL
constexpr int sendFlags = MSG_NOSIGNAL;
#else
constexpr int sendFlags = 0;
#endif
#endif

class TcpBytes final : public ByteStream {
public:
    TcpBytes(Socket socket, std::string what) : socket_(std::move(socket)), what_(std::move(what)) {}

    size_t writeSome(std::span<const uint8_t> bytes) override {
        const auto* data = reinterpret_cast<const char*>(bytes.data());
        const int chunk = int(std::min(bytes.size(), size_t(INT_MAX)));
#ifdef _WIN32
        const int result = ::send(socket_.get(), data, chunk, 0);
#else
        const auto result = ::send(socket_.get(), data, size_t(chunk), sendFlags);
#endif
        if (result > 0) {
            wrote_ = Clock::now();
            return size_t(result);
        }
        if (result < 0 && wouldBlock()) {
            return 0;
        }
        fail(RCS_ERROR_NETWORK, "Sending to " + what_ + " failed: " + lastSocketErrorText());
    }

    size_t readSome(std::span<uint8_t> buffer) override {
#ifdef _WIN32
        const int result = ::recv(socket_.get(), reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0);
#else
        const auto result = ::recv(socket_.get(), buffer.data(), buffer.size(), 0);
#endif
        if (result > 0) {
            return size_t(result);
        }
        if (result == 0) {
            fail(RCS_ERROR_NETWORK, "The connection to " + what_ + " was closed");
        }
        if (!wouldBlock()) {
            fail(RCS_ERROR_NETWORK, "Receiving from " + what_ + " failed: " + lastSocketErrorText());
        }
        return 0;
    }

    Handle handle() const noexcept override { return socket_.get(); }
    Endpoint local() const override { return localAddress(socket_.get()); }

    // After `rest`, what went out within the last second may still be on its
    // way: closing with what the server sent unread would reset the
    // connection, which may discard the last words before the server read
    // them (Windows does). So this end's side is shut, and what the server
    // still sends is read until it closes its side (250 ms at most). One that
    // sent nothing for longer (a path long left) closes at once.
    void close(std::span<const uint8_t> rest, Clock::time_point deadline) override {
        static const std::atomic<bool> never{false};
        while (!rest.empty()) {
            if (const size_t taken = writeSome(rest)) {
                rest = rest.subspan(taken);
            } else {
                waitReady(socket_.get(), true, never, deadline);
            }
        }
        if (Clock::now() - wrote_ > 1s) {
            return;
        }
#ifdef _WIN32
        ::shutdown(socket_.get(), SD_SEND);
#else
        ::shutdown(socket_.get(), SHUT_WR);
#endif
        uint8_t scratch[4096];
        for (const auto until = std::min(deadline, Clock::now() + 250ms);;) {
            waitReady(socket_.get(), false, never, until); // Throws at the deadline...
            readSome(scratch);                             // ...and once the server closed its side.
        }
    }

private:
    Socket socket_;
    std::string what_;
    Clock::time_point wrote_{}; // When it last took something.
};

class LengthFraming final : public Framing {
public:
    Bytes wrap(std::span<const uint8_t> datagram) override {
        Bytes wire;
        appendBigEndian(wire, datagram.size(), 2);
        wire.insert(wire.end(), datagram.begin(), datagram.end());
        return wire;
    }
    void push(std::span<const uint8_t> bytes) override {
        buffer_.erase(buffer_.begin(), buffer_.begin() + ptrdiff_t(begin_));
        begin_ = 0;
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }
    std::optional<Bytes> pop(Bytes&) override {
        const auto waiting = std::span<const uint8_t>(buffer_).subspan(begin_);
        if (waiting.size() < 2) {
            return std::nullopt;
        }
        const size_t size = size_t(loadBigEndian(waiting, 0, 2));
        if (!size) {
            fail(RCS_ERROR_NETWORK, "The stream carried a datagram of no bytes");
        }
        if (waiting.size() < 2 + size) {
            return std::nullopt;
        }
        begin_ += 2 + size;
        return Bytes(waiting.begin() + 2, waiting.begin() + 2 + ptrdiff_t(size));
    }

private:
    Bytes buffer_;
    size_t begin_ = 0;
};

class WebSocketFraming final : public Framing {
public:
    Bytes wrap(std::span<const uint8_t> datagram) override {
        return websocket::frame(websocket::binary, datagram, true);
    }
    void push(std::span<const uint8_t> bytes) override { decoder_.push(bytes); }
    std::optional<Bytes> pop(Bytes& answer) override {
        try {
            while (auto event = decoder_.next()) {
                if (event->kind == websocket::binary) {
                    return std::move(event->payload);
                }
                if (event->kind == websocket::close) {
                    const uint32_t code =
                        event->payload.size() >= 2 ? uint32_t(loadBigEndian(event->payload, 0, 2)) : 0;
                    fail(RCS_ERROR_NETWORK,
                         "The WebSocket server closed the connection (code " + std::to_string(code) + ")");
                }
                const auto pong = websocket::frame(websocket::pong, event->payload, true);
                answer.insert(answer.end(), pong.begin(), pong.end());
            }
        } catch (const websocket::Violation& violation) {
            fail(RCS_ERROR_NETWORK, std::string("The WebSocket server broke RFC 6455: ") + violation.what());
        }
        return std::nullopt;
    }

private:
    websocket::Decoder decoder_{false};
};

} // namespace

std::unique_ptr<Framing> lengthFraming() {
    return std::make_unique<LengthFraming>();
}

std::unique_ptr<Framing> webSocketFraming() {
    return std::make_unique<WebSocketFraming>();
}

Socket connectStreamTcp(const Endpoint& server, const std::string& what, CancelToken cancelled,
                        std::chrono::milliseconds timeout, uint32_t interfaceIndex) {
    Socket socket = connectTcp(server, what, cancelled, timeout, interfaceIndex);
    int enabled = 1;
    setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    int buffer = 1024 * 1024; // A keyframe burst in either direction.
    setsockopt(socket.get(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    setsockopt(socket.get(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    return socket;
}

std::unique_ptr<ByteStream> tcpBytes(Socket socket, std::string what) {
    return std::make_unique<TcpBytes>(std::move(socket), std::move(what));
}

FramedSocket::FramedSocket(std::unique_ptr<ByteStream> stream, Endpoint server, std::unique_ptr<Framing> framing,
                           std::span<const uint8_t> arrived)
    : stream_(std::move(stream)), server_(std::move(server)), framing_(std::move(framing)) {
#ifdef _WIN32
    watchable(stream_->handle(), FD_READ | FD_CLOSE);
#endif
    framing_->push(arrived);
}

FramedSocket::~FramedSocket() {
    try {
        stream_->close(outbound_, Clock::now() + 1s);
    } catch (const Failure&) {
    }
}

void FramedSocket::flush() {
    size_t written = 0;
    while (written < outbound_.size()) {
        const size_t taken = stream_->writeSome(std::span<const uint8_t>(outbound_).subspan(written));
        if (!taken) {
            break; // The server takes data slower than it comes: the rest waits.
        }
        written += taken;
    }
    outbound_.erase(outbound_.begin(), outbound_.begin() + ptrdiff_t(written));
}

bool FramedSocket::queue(std::span<const uint8_t> datagram, bool& full) {
    // A datagram goes whole or not at all: part of one would break the framing.
    const auto wire = framing_->wrap(datagram);
    const auto fits = [&] { return outbound_.empty() || outbound_.size() + wire.size() <= maxOutbound; };
    if (!fits() && !full) {
        flush();
        full = !outbound_.empty();
    }
    if (!fits()) {
        return false;
    }
    outbound_.insert(outbound_.end(), wire.begin(), wire.end());
    return true;
}

bool FramedSocket::send(const Endpoint& to, std::span<const uint8_t> datagram) {
    if (to != server_) {
        return false; // Only the server is reachable this way.
    }
    bool full = false;
    const bool taken = queue(datagram, full);
    flush();
    return taken;
}

size_t FramedSocket::sendBatch(std::span<const Outgoing> batch) {
    bool full = false;
    for (const auto& datagram : batch) {
        if (*datagram.to == server_) {
            queue(datagram.datagram, full);
        }
    }
    flush();
    return 0; // Unmarked: inside the stream.
}

bool FramedSocket::buffered() const noexcept {
    return !ready_.empty() || stream_->buffered();
}

std::optional<size_t> FramedSocket::receive(std::span<uint8_t> buffer, Endpoint& from) {
    flush();
    for (;;) {
        while (!ready_.empty()) {
            auto datagram = std::move(ready_.front());
            ready_.pop_front();
            if (datagram.size() > buffer.size()) {
                continue; // Too large for the caller: dropped, as a UDP socket would.
            }
            std::memcpy(buffer.data(), datagram.data(), datagram.size());
            from = server_;
            return datagram.size();
        }
        Bytes answer;
        while (auto datagram = framing_->pop(answer)) {
            ready_.push_back(std::move(*datagram));
        }
        // A ping's pong, after what waits; none for a server that does not read them.
        if (!answer.empty() && outbound_.size() + answer.size() <= maxOutbound) {
            outbound_.insert(outbound_.end(), answer.begin(), answer.end());
            flush();
        }
        if (!ready_.empty()) {
            continue;
        }
        const size_t taken = stream_->readSome(chunk_);
        if (!taken) {
            return std::nullopt;
        }
        framing_->push(std::span<const uint8_t>(chunk_).first(taken));
    }
}

} // namespace rcsdk::transport
