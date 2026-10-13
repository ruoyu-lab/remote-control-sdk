// Sockets shared by the TCP and UDP transports: handles, IPv4 / IPv6
// endpoints, UDP sockets and a waiter that wakes on datagrams, on request or
// at a deadline (Windows: a high-resolution timer; elsewhere poll(), which
// rounds up to the next millisecond; pacing allows for that).
#pragma once

#include "util/util.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rcsdk::transport {

#ifdef _WIN32
using Handle = SOCKET;
inline constexpr Handle invalidHandle = INVALID_SOCKET;
#else
using Handle = int;
inline constexpr Handle invalidHandle = -1;
#endif

void startNetworking();
void closeHandle(Handle handle) noexcept;
void makeNonBlocking(Handle handle);
// The last operation would block (or was interrupted): try again later.
bool wouldBlock() noexcept;

class Socket {
public:
    explicit Socket(Handle handle = invalidHandle) noexcept : handle_(handle) {}
    ~Socket() {
        if (handle_ != invalidHandle) {
            closeHandle(handle_);
        }
    }
    Socket(Socket&& other) noexcept : handle_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (handle_ != invalidHandle) {
                closeHandle(handle_);
            }
            handle_ = other.release();
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Handle get() const noexcept { return handle_; }
    Handle release() noexcept {
        const Handle handle = handle_;
        handle_ = invalidHandle;
        return handle;
    }

private:
    Handle handle_;
};

// An IPv4 or IPv6 address with a port.
class Endpoint {
public:
    Endpoint() = default;
    // Numeric IPv4 ("192.168.1.2") or IPv6 ("fe80::1", "[2001:db8::1]"); throws INVALID_ARGUMENT.
    static Endpoint parse(const std::string& address, uint16_t port);
    static std::optional<Endpoint> tryParse(const std::string& address, uint16_t port);
    static Endpoint fromSockaddr(const sockaddr* address, size_t length);

    bool valid() const noexcept { return length_ != 0; }
    int family() const noexcept { return int(storage_.ss_family); }
    bool ipv6() const noexcept { return family() == AF_INET6; }
    uint16_t port() const noexcept;
    void setPort(uint16_t port) noexcept;
    std::string address() const;
    // "192.168.1.2:47990" or "[2001:db8::1]:47990".
    std::string toString() const;
    // Address bytes: 4 (IPv4) or 16 (IPv6).
    std::vector<uint8_t> addressBytes() const;
    static Endpoint fromBytes(std::span<const uint8_t> address, uint16_t port);

    const sockaddr* get() const noexcept { return reinterpret_cast<const sockaddr*>(&storage_); }
    socklen_t size() const noexcept { return socklen_t(length_); }

    // Family, address, port and (IPv6) scope: fe80::1 on two interfaces is two endpoints.
    bool operator==(const Endpoint& other) const noexcept;
    bool operator!=(const Endpoint& other) const noexcept { return !(*this == other); }

private:
    sockaddr_storage storage_{};
    size_t length_ = 0;
};

class Waiter;

// A datagram of a batch (DatagramSocket::sendBatch): to `to`, marked `dscp`
// as sendMarked() marks it (0: unmarked).
struct Outgoing {
    const Endpoint* to = nullptr;
    std::span<const uint8_t> datagram;
    uint8_t dscp = 0;
};

// What sends and receives datagrams: a UDP socket, or a TCP connection to a
// TURN server that carries them framed (turn_tcp_socket.hpp). The TURN client,
// ICE and the UDP transport work on either; Waiters watch it.
class DatagramSocket {
public:
    DatagramSocket() = default;
    virtual ~DatagramSocket();
    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;

    virtual Endpoint local() const = 0;
    virtual Handle handle() const noexcept = 0;
    // Sends one datagram without blocking; false when it was dropped (no
    // room, a transient error), as the network itself may drop it. Throws
    // Failure(NETWORK) once the socket is gone.
    virtual bool send(const Endpoint& to, std::span<const uint8_t> datagram) = 0;
    // As send(), with `dscp` (1..63) in the datagram's IP header, which access
    // points (Wi-Fi's WMM) and routers queue by; true when it went out
    // marked. What carries datagrams inside another protocol (a TURN
    // server's TCP, the host's channel) sends them unmarked.
    virtual bool sendMarked(const Endpoint& to, std::span<const uint8_t> datagram, uint8_t dscp) {
        (void)dscp;
        send(to, datagram);
        return false;
    }
    // Sends `batch` in order, each datagram as send() or (with a `dscp`)
    // sendMarked() would; returns how many of the marked ones went out
    // marked. One by one here; a UDP socket on Linux hands it to the system
    // in few calls (UdpSocket), a byte stream in one write (FramedSocket, a
    // TCP port's and a forwarder entry's connections).
    virtual size_t sendBatch(std::span<const Outgoing> batch);
    // One waiting datagram, or nothing when none waits. A datagram larger than
    // `buffer` is dropped. Throws Failure(NETWORK) once the socket is gone,
    // and PeerClosed (below) where a peer's own connection under it closed.
    virtual std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint& from) = 0;
    // A datagram already read from the system waits in this object, where
    // waiting on the handle does not see it.
    virtual bool buffered() const noexcept { return false; }
    // Its last send failed for a reason that stays (no route, the address
    // gone), not for a moment without room.
    virtual bool failing() const noexcept { return false; }
    // How often the path under it was replaced by one to the same peer (the
    // host's channel, rcs_session_transport_restarted).
    virtual uint64_t restarts() const noexcept { return 0; }
    // A socket with nothing of its own to wait on (handle() invalidHandle, no
    // readable() event: a share of another socket, udp_demux.hpp) wakes the
    // Waiters that watch it instead: true when it will wake `waiter` from now
    // until detachWaiter, which the waiter calls before it ends.
    virtual bool attachWaiter(Waiter& waiter) const noexcept {
        (void)waiter;
        return false;
    }
    virtual void detachWaiter(Waiter& waiter) const noexcept { (void)waiter; }

#ifdef _WIN32
    // Set while the socket has something to read. Windows ties a socket to
    // one event (WSAEventSelect replaces an earlier association), so the
    // socket owns it and every Waiter that watches the socket shares it.
    HANDLE readable() const noexcept { return event_; }

protected:
    // Ties `handle` to the event for `events` (FD_READ; FD_CLOSE too for a
    // connection); once, from the implementation's constructor.
    void watchable(Handle handle, long events);
    // For an implementation that is no socket: an event of its own, which
    // signal() sets when something arrives (waiters reset it after each wait,
    // and buffered() tells them what still waits).
    void watchable();
    void signal() noexcept { WSASetEvent(event_); }

private:
    HANDLE event_ = nullptr;
#endif
};

// A socket that reaches its peers over connections of their own (a sharer's
// TCP port, protocol/tcp_demux.hpp) throws it from receive() once the
// connection to `peer` closed, after the datagrams that came over it: that
// address reaches no one now. The socket goes on.
class PeerClosed : public Failure {
public:
    PeerClosed(const Endpoint& peer, const std::string& why) : Failure(RCS_ERROR_NETWORK, why), peer_(peer) {}
    const Endpoint& peer() const noexcept { return peer_; }

private:
    Endpoint peer_;
};

// Makes `handle` send and receive only through the network interface `index`,
// whatever the routing table prefers (e.g. Wi-Fi beside Ethernet).
void bindToInterface(Handle handle, bool ipv6, uint32_t index);
#ifdef __ANDROID__
// Android routes a socket by the network netd marks it for, not by its
// device: the network whose interface `index` is, through ConnectivityManager
// (platform/android/network.cpp). False without the Java VM, or when no
// network has that interface.
bool bindToAndroidNetwork(Handle handle, uint32_t index);
#endif
// The address a socket is bound to. Throws Failure(NETWORK).
Endpoint localAddress(Handle handle);
// The address the system sends to `remote` from now (its route's), port 0;
// none when no route leads there. Sends nothing.
std::optional<Endpoint> sourceFor(const Endpoint& remote);
// The SDK's tests: a datagram between a UDP socket's address and `remote`
// that `filter` refuses is dropped on its way, either way, as a network that
// went away drops it (a path breaks, an address stops working); empty restores.
using DatagramFilter = std::function<bool(const Endpoint& local, const Endpoint& remote)>;
void setDatagramFilter(DatagramFilter filter);
// A nonblocking TCP socket, not yet connected or bound.
Socket openTcpSocket(int family);
// A nonblocking TCP socket connected to `peer` within `timeout`, through the
// interface `interfaceIndex` (0: any). Throws Failure(NETWORK) naming `what`
// (an address, "the TURN server ...") when it is refused or not accepted in time.
Socket connectTcp(const Endpoint& peer, const std::string& what, CancelToken cancelled,
                  std::chrono::milliseconds timeout, uint32_t interfaceIndex = 0);
// Waits until `handle` is readable (or writable), polling cancellation every
// 20 ms (`cancelled`, and `stopped` where given: a stream cancelled on its
// own); throws NETWORK at the deadline.
void waitReady(Handle handle, bool write, CancelToken cancelled, Clock::time_point deadline,
               const std::atomic<bool>* stopped = nullptr);
// Waits until one of `sockets` is readable (or, where `write`, writable), or
// `timeout` passed: for one thread serving several connections.
struct Readiness {
    Handle handle;
    bool write = false;
};
void waitAny(std::span<const Readiness> sockets, std::chrono::milliseconds timeout);

// How a batch leaves a UDP socket on Linux (Android too) in few system
// calls: sendmmsg, each message one datagram or, with UDP GSO (UDP_SEGMENT,
// Linux 4.18), a run of datagrams to one address with one mark, each as large
// as the first (the last may be smaller), which the system routes once and
// cuts apart only then (the network card, where it can); marks go as each
// message's control data. Here are the plan and what each error means; the
// system's call is the socket's (the tests play it).
class DatagramBatcher {
public:
    // A message of a call: datagrams [first, first + count) of the batch to
    // the first one's address; more than one are a run cut every `segment`
    // bytes. `dscp` as Outgoing's.
    struct Message {
        size_t first = 0, count = 1, segment = 0;
        uint8_t dscp = 0;
    };
    // Sends `messages` of `batch`: how many went (as sendmmsg answers), or
    // -1 with the first one's error (errno) in `error`.
    using System = std::function<int(std::span<const Outgoing> batch, std::span<const Message> messages, int& error)>;

    static constexpr size_t maxMessages = 64, maxDatagrams = 256; // In one call.
    // A run: at most the 64 datagrams every kernel with UDP GSO cuts, within
    // one IPv4 packet's payload, of datagrams that pass any IPv6 path (its
    // 1280-byte MTU): a larger one leaves alone, as the system fragments it.
    static constexpr size_t maxRun = 64, maxRunBytes = 65507, maxSegment = 1232;

    explicit DatagramBatcher(System system) : system_(std::move(system)) {}
    // Whether the system cuts runs (it knows UDP_SEGMENT); off at first.
    void setSegmenting(bool on) noexcept { segmenting_ = on; }
    bool segmenting() const noexcept { return segmenting_.load(); }

    // Sends `batch` in order from the socket at `local` (named in the log).
    // A run the system refuses (EIO: the network card computes no checksums;
    // EINVAL: a path's MTU) turns runs off for good, said in the log, and its
    // datagrams go again one by one. A full socket (EAGAIN) drops the rest
    // of the batch, as it drops a datagram it has no room for; any other
    // error drops the message that met it, and `failing` says whether that
    // error stays (as DatagramSocket::failing; a send that went clears it).
    void send(std::span<const Outgoing> batch, std::atomic<bool>& failing, const Endpoint& local);

private:
    System system_;
    std::atomic<bool> segmenting_{false};
};

// A bound, nonblocking UDP socket.
class UdpSocket final : public DatagramSocket {
public:
    // Binds `local` (port 0 picks one). Throws Failure(NETWORK).
    explicit UdpSocket(const Endpoint& local);
    void bindToInterface(uint32_t index) { transport::bindToInterface(socket_.get(), local().ipv6(), index); }
    Endpoint local() const override;
    Handle handle() const noexcept override { return socket_.get(); }

    // False when the system had no room for the datagram.
    bool send(const Endpoint& to, std::span<const uint8_t> datagram) override;
    // Linux: the class as the datagram's control data. Elsewhere the
    // socket's traffic class goes to `dscp` for this datagram and back:
    // Apple's systems take no per-datagram class (IP_TOS as control data is
    // dropped there), and connections that share the socket send meanwhile.
    bool sendMarked(const Endpoint& to, std::span<const uint8_t> datagram, uint8_t dscp) override;
    // Linux: in few system calls (DatagramBatcher). Apple's systems take a
    // datagram a call, the batch under one hold of the lock below: the
    // connections that share the socket take turns a batch at a time, not a
    // datagram at a time (docs/PERFORMANCE.md, the forwarder's throughput).
    // Windows: one by one, each under the lock, as before (not measured).
#ifndef _WIN32
    size_t sendBatch(std::span<const Outgoing> batch) override;
#endif
    // Errors a datagram socket gets for earlier datagrams (an ICMP message
    // about one) are skipped; any other error means the socket is gone.
    std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint& from) override;
    bool failing() const noexcept override { return failing_.load(); }

private:
    bool sendUnlocked(const Endpoint& to, std::span<const uint8_t> datagram);

    Socket socket_;
    bool v6_ = false;
    Endpoint local_; // As bound: what a datagram filter is asked about.
#ifdef __linux__
    // Nothing of the socket changes for a send, so the threads that share it
    // (connections that share a port: a sharer's viewers, a forwarder's
    // ends) send side by side.
    DatagramBatcher batcher_;
#else
    // A class set for one datagram is not another's: a marked send takes the
    // socket alone. XNU holds the socket's lock through each send, and
    // threads contending for that one cost far more than waiting here
    // (docs/PERFORMANCE.md, the forwarder's throughput).
    bool setTrafficClass(int value);
    std::mutex sendMutex_;
#endif
    std::atomic<bool> failing_{false};
};

// Sleeps until one of the watched sockets is readable, wake() is called, or a
// deadline passes. One thread waits; any thread may wake it. Watched sockets
// must outlive the waits, and those that wake their Waiters (attachWaiter)
// the Waiter.
class Waiter {
public:
    Waiter();
    ~Waiter();
    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;

    void watch(const DatagramSocket& socket);
    void unwatch(const DatagramSocket& socket);
    void wake() noexcept;
    void wait(Clock::time_point deadline);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rcsdk::transport
