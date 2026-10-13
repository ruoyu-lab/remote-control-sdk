// Cancellable TCP byte streams and the listening socket that accepts them.
#pragma once

#include "transport/socket.hpp"
#include "transport/stream.hpp"
#include "util/util.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace rcsdk::transport {

class TcpStream final : public Stream {
public:
    // View side; through interface `interfaceIndex` only (0: any).
    static std::unique_ptr<TcpStream> connect(const std::string& address, uint16_t port, CancelToken cancelled,
                                              std::chrono::milliseconds timeout, uint32_t interfaceIndex = 0);
    // A connected stream socket of any family (a Unix domain socket for a local
    // endpoint); the stream owns it.
    static std::unique_ptr<TcpStream> adopt(Handle socket, CancelToken cancelled);
    ~TcpStream() override;

    void read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) override;
    void write(std::span<const uint8_t> buffer, std::chrono::milliseconds timeout) override;
    void writeFinal(std::span<const uint8_t> buffer, std::chrono::milliseconds timeout) override;

    // Without waiting, for one thread serving several streams: what arrived
    // (at most `buffer`), what the socket took; 0 when nothing could move.
    // Throw like read and write when the peer closed or the connection failed.
    size_t readSome(std::span<uint8_t> buffer);
    size_t writeSome(std::span<const uint8_t> buffer);
    // Shuts this end's side: the peer reads the end of the stream after what
    // was written. Reading goes on.
    void finish() noexcept;
    Handle handle() const noexcept;
    // Ends this stream's transfers as its token would, whatever the token: one
    // stream of several that run on the same token.
    void cancel() noexcept;
    // The other end's address.
    Endpoint peer() const;

    uint64_t bytesTransferred() const noexcept override;

private:
    friend class TcpListenSocket;
    struct Impl;
    explicit TcpStream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// A peer came, and the process has no descriptor for it now (macOS resets
// that peer; other systems keep it in the port's backlog).
class NoDescriptor : public Failure {
public:
    using Failure::Failure;
};

// Share side: binds address:port exclusively. Destroying it closes the port.
class TcpListenSocket {
public:
    TcpListenSocket(const std::string& address, uint16_t port);
    ~TcpListenSocket();
    TcpListenSocket(const TcpListenSocket&) = delete;
    TcpListenSocket& operator=(const TcpListenSocket&) = delete;

    // Waits for the next peer; its stream observes `cancelled` as well.
    std::unique_ptr<TcpStream> accept(CancelToken cancelled);
    // The next peer if one is waiting, else null. Throws NoDescriptor while
    // the process has no descriptor for it.
    std::unique_ptr<TcpStream> tryAccept(CancelToken cancelled);
    Handle handle() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rcsdk::transport
