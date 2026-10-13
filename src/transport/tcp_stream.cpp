#include "transport/tcp_stream.hpp"

#include "transport/socket_error.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <optional>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rcsdk::transport {
namespace {

#ifndef _WIN32
// A peer that hung up is an error to report, not SIGPIPE. macOS sets
// SO_NOSIGPIPE on the socket instead.
#ifdef MSG_NOSIGNAL
constexpr int sendFlags = MSG_NOSIGNAL;
#else
constexpr int sendFlags = 0;
#endif
#endif

// Small messages (input, acknowledgements) go out at once, not held back to
// fill a segment; for TCP only (a Unix domain socket has no such option).
void noDelay(Handle handle) {
    int enabled = 1;
    setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
}

} // namespace

struct TcpStream::Impl {
    Socket socket;
    CancelToken cancelled;
    std::atomic<bool> stopped{false}; // cancel()
    std::atomic<uint64_t> transferred{0};

    Impl(Handle handle, CancelToken token) : socket(handle), cancelled(token) {
        makeNonBlocking(handle);
        int sendBuffer = 256 * 1024; // A keyframe goes out without waiting for the reader.
        setsockopt(handle, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));
    }

    // One send or receive without waiting: bytes moved, 0 when the socket
    // would block. Throws when the peer closed or the connection failed.
    template <class Byte>
    size_t once(std::span<Byte> buffer, bool write) {
        auto* bytes = reinterpret_cast<char*>(const_cast<std::remove_const_t<Byte>*>(buffer.data()));
        const int chunk = static_cast<int>(std::min(buffer.size(), static_cast<size_t>(INT_MAX)));
#ifdef _WIN32
        const int result = write ? ::send(socket.get(), bytes, chunk, 0) : ::recv(socket.get(), bytes, chunk, 0);
#else
        const auto result = write ? ::send(socket.get(), bytes, size_t(chunk), sendFlags)
                                  : ::recv(socket.get(), bytes, size_t(chunk), 0);
#endif
        if (result == 0 && chunk) {
            fail(RCS_ERROR_DISCONNECTED, "The peer closed the connection");
        }
        if (result < 0) {
            if (wouldBlock()) {
                return 0;
            }
            fail(RCS_ERROR_NETWORK, "Socket transfer failed: " + lastSocketErrorText());
        }
        transferred += size_t(result);
        return size_t(result);
    }

    // Cancellation ends a transfer before its first byte; one under way is
    // finished first (for at most 250 ms after the cancellation), so that a
    // record is never cut short and what follows it can still be understood.
    // `timeout` is the longest wait without progress; `final`: cancellation
    // does not end the transfer at all, and `timeout` bounds all of it.
    template <class Byte>
    void transfer(std::span<Byte> buffer, bool write, std::chrono::milliseconds timeout, bool final = false) {
        static const std::atomic<bool> never{false};
        auto deadline = Clock::now() + timeout;
        std::optional<Clock::time_point> finishBy;
        size_t done = 0;
        while (done < buffer.size()) {
            const bool underway = done != 0;
            const bool cancellable = !final && !underway;
            auto until = deadline;
            if (underway && (cancelled.load() || stopped.load())) {
                finishBy = finishBy ? *finishBy : Clock::now() + 250ms;
                until = std::min(deadline, *finishBy);
            }
            waitReady(socket.get(), write, cancellable ? cancelled : never, until, cancellable ? &stopped : nullptr);
            if (const size_t moved = once(buffer.subspan(done), write)) {
                done += moved;
                if (!final) {
                    deadline = Clock::now() + timeout;
                }
            }
        }
    }
};

TcpStream::TcpStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TcpStream::~TcpStream() = default;

struct TcpListenSocket::Impl {
    Socket socket;
};

TcpListenSocket::TcpListenSocket(const std::string& address, uint16_t port) {
    const auto endpoint = Endpoint::parse(address, port);
    impl_ = std::make_unique<Impl>(Impl{openTcpSocket(endpoint.family())});
    const Handle handle = impl_->socket.get();
    int enabled = 1;
#ifdef _WIN32
    // SO_REUSEADDR on Winsock can share an existing listener and route connections
    // nondeterministically; reserve the address exclusively instead.
    setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#else
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
    if (endpoint.ipv6()) {
        int only = 1;
        setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only), sizeof(only));
    }
    // Connections wait in the backlog until accepted; the sharer accepts them
    // at once and runs their handshakes side by side.
    if (bind(handle, endpoint.get(), endpoint.size()) != 0 || listen(handle, 8) != 0) {
        fail(RCS_ERROR_NETWORK,
             "Could not listen on " + address + ":" + std::to_string(port) + ": " + lastSocketErrorText());
    }
}

TcpListenSocket::~TcpListenSocket() = default;

std::unique_ptr<TcpStream> TcpListenSocket::tryAccept(CancelToken cancelled) {
    for (;;) {
        const Handle peer = ::accept(impl_->socket.get(), nullptr, nullptr);
        if (peer != invalidHandle) {
            auto impl = std::make_unique<TcpStream::Impl>(peer, cancelled);
            noDelay(peer);
            return std::unique_ptr<TcpStream>(new TcpStream(std::move(impl)));
        }
        if (wouldBlock()) {
            return nullptr;
        }
#ifdef _WIN32
        const bool aborted = WSAGetLastError() == WSAECONNRESET;
#else
        const bool aborted = errno == ECONNABORTED;
#endif
        if (outOfDescriptors(lastSocketError())) {
            throw NoDescriptor(RCS_ERROR_NETWORK, "Accepting a viewer failed: " + lastSocketErrorText());
        }
        if (!aborted) { // A peer that gave up before it was accepted is no failure of the port.
            fail(RCS_ERROR_NETWORK, "Accepting a viewer failed: " + lastSocketErrorText());
        }
    }
}

std::unique_ptr<TcpStream> TcpListenSocket::accept(CancelToken cancelled) {
    for (;;) {
        waitReady(impl_->socket.get(), false, cancelled, Clock::time_point::max());
        if (auto stream = tryAccept(cancelled)) {
            return stream;
        }
    }
}

Handle TcpListenSocket::handle() const noexcept {
    return impl_->socket.get();
}

std::unique_ptr<TcpStream> TcpStream::adopt(Handle socket, CancelToken cancelled) {
    return std::unique_ptr<TcpStream>(new TcpStream(std::make_unique<Impl>(socket, cancelled)));
}

std::unique_ptr<TcpStream> TcpStream::connect(const std::string& address, uint16_t port, CancelToken cancelled,
                                              std::chrono::milliseconds timeout, uint32_t interfaceIndex) {
    Socket socket = connectTcp(Endpoint::parse(address, port), address + ":" + std::to_string(port), cancelled,
                               timeout, interfaceIndex);
    noDelay(socket.get());
    return std::unique_ptr<TcpStream>(new TcpStream(std::make_unique<Impl>(socket.release(), cancelled)));
}

void TcpStream::read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) {
    impl_->transfer(buffer, false, timeout);
}

void TcpStream::write(std::span<const uint8_t> buffer, std::chrono::milliseconds timeout) {
    impl_->transfer(buffer, true, timeout);
}

size_t TcpStream::readSome(std::span<uint8_t> buffer) {
    return impl_->once(buffer, false);
}

size_t TcpStream::writeSome(std::span<const uint8_t> buffer) {
    return impl_->once(buffer, true);
}

void TcpStream::finish() noexcept {
#ifdef _WIN32
    ::shutdown(impl_->socket.get(), SD_SEND);
#else
    ::shutdown(impl_->socket.get(), SHUT_WR);
#endif
}

Handle TcpStream::handle() const noexcept {
    return impl_->socket.get();
}

void TcpStream::cancel() noexcept {
    impl_->stopped = true;
}

Endpoint TcpStream::peer() const {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getpeername(impl_->socket.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        fail(RCS_ERROR_NETWORK, "Could not read the peer's address: " + lastSocketErrorText());
    }
    return Endpoint::fromSockaddr(reinterpret_cast<const sockaddr*>(&address), size_t(length));
}

void TcpStream::writeFinal(std::span<const uint8_t> buffer, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    impl_->transfer(buffer, true, timeout, true);
    // Closing with the peer's data unread sends a reset, which can discard
    // what was just written. So close the sending side, then read what the
    // peer still sends until it closes too (or the time is up).
    const Handle handle = impl_->socket.get();
#ifdef _WIN32
    ::shutdown(handle, SD_SEND);
#else
    ::shutdown(handle, SHUT_WR);
#endif
    static const std::atomic<bool> never{false};
    char scratch[1024];
    try {
        for (;;) {
            waitReady(handle, false, never, deadline);
#ifdef _WIN32
            const int received = ::recv(handle, scratch, int(sizeof(scratch)), 0);
#else
            const auto received = ::recv(handle, scratch, sizeof(scratch), 0);
#endif
            if (received == 0 || (received < 0 && !wouldBlock())) {
                return;
            }
        }
    } catch (const Failure&) { // The peer did not close in time: closing now may reset.
    }
}

uint64_t TcpStream::bytesTransferred() const noexcept {
    return impl_->transferred.load();
}

} // namespace rcsdk::transport
