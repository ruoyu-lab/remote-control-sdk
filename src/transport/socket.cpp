#include "transport/socket.hpp"

#include "transport/socket_error.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <mswsock.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef SIO_UDP_NETRESET
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR, 15)
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <netinet/udp.h>
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103 // The kernel's (4.18), which older C libraries' headers do not name.
#endif
#endif
#ifdef __EMSCRIPTEN__
#include <condition_variable>
#include <mutex>
#endif

namespace rcsdk::transport {

void startNetworking() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started) {
        fail(RCS_ERROR_NETWORK, "Winsock initialization failed");
    }
#endif
}

void closeHandle(Handle handle) noexcept {
#ifdef _WIN32
    closesocket(handle);
#else
    ::close(handle);
#endif
}

void makeNonBlocking(Handle handle) {
#ifdef _WIN32
    u_long enabled = 1;
    const bool ok = ioctlsocket(handle, FIONBIO, &enabled) == 0;
#else
    const bool ok = fcntl(handle, F_SETFL, fcntl(handle, F_GETFL) | O_NONBLOCK) == 0;
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
#endif
    if (!ok) {
        fail(RCS_ERROR_NETWORK, "Could not configure the socket: " + lastSocketErrorText());
    }
}

bool wouldBlock() noexcept {
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
}

// ---- Endpoint -----------------------------------------------------------------

std::optional<Endpoint> Endpoint::tryParse(const std::string& text, uint16_t port) {
    std::string address = text;
    if (address.size() >= 2 && address.front() == '[' && address.back() == ']') {
        address = address.substr(1, address.size() - 2);
    }
    Endpoint endpoint;
    auto* v4 = reinterpret_cast<sockaddr_in*>(&endpoint.storage_);
    if (inet_pton(AF_INET, address.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        endpoint.length_ = sizeof(sockaddr_in);
        return endpoint;
    }
    endpoint = Endpoint();
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&endpoint.storage_);
    // Scoped link-local addresses ("fe80::1%en0") name an interface.
    const auto percent = address.find('%');
    if (inet_pton(AF_INET6, address.substr(0, percent).c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        if (percent != std::string::npos) {
            const std::string scope = address.substr(percent + 1);
#ifdef _WIN32
            v6->sin6_scope_id = ULONG(std::strtoul(scope.c_str(), nullptr, 10));
#else
            v6->sin6_scope_id = if_nametoindex(scope.c_str());
            if (!v6->sin6_scope_id) {
                v6->sin6_scope_id = uint32_t(std::strtoul(scope.c_str(), nullptr, 10));
            }
#endif
        }
        endpoint.length_ = sizeof(sockaddr_in6);
        return endpoint;
    }
    return std::nullopt;
}

Endpoint Endpoint::parse(const std::string& address, uint16_t port) {
    if (auto endpoint = tryParse(address, port)) {
        return *endpoint;
    }
    fail(RCS_ERROR_INVALID_ARGUMENT, "\"" + address + "\" is not a numeric IPv4 or IPv6 address");
}

Endpoint Endpoint::fromSockaddr(const sockaddr* address, size_t length) {
    Endpoint endpoint;
    if (address && length <= sizeof(endpoint.storage_) &&
        (address->sa_family == AF_INET || address->sa_family == AF_INET6)) {
        std::memcpy(&endpoint.storage_, address, length);
        endpoint.length_ = address->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    }
    return endpoint;
}

uint16_t Endpoint::port() const noexcept {
    if (family() == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&storage_)->sin_port);
    }
    if (family() == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage_)->sin6_port);
    }
    return 0;
}

void Endpoint::setPort(uint16_t port) noexcept {
    if (family() == AF_INET) {
        reinterpret_cast<sockaddr_in*>(&storage_)->sin_port = htons(port);
    } else if (family() == AF_INET6) {
        reinterpret_cast<sockaddr_in6*>(&storage_)->sin6_port = htons(port);
    }
}

std::string Endpoint::address() const {
    char text[INET6_ADDRSTRLEN] = {};
    if (family() == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&storage_)->sin_addr, text, sizeof(text));
    } else if (family() == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&storage_)->sin6_addr, text, sizeof(text));
    }
    return text;
}

std::string Endpoint::toString() const {
    return ipv6() ? "[" + address() + "]:" + std::to_string(port()) : address() + ":" + std::to_string(port());
}

std::vector<uint8_t> Endpoint::addressBytes() const {
    if (family() == AF_INET) {
        const auto* bytes =
            reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in*>(&storage_)->sin_addr);
        return {bytes, bytes + 4};
    }
    if (family() == AF_INET6) {
        const auto* bytes =
            reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in6*>(&storage_)->sin6_addr);
        return {bytes, bytes + 16};
    }
    return {};
}

Endpoint Endpoint::fromBytes(std::span<const uint8_t> address, uint16_t port) {
    Endpoint endpoint;
    if (address.size() == 4) {
        auto* v4 = reinterpret_cast<sockaddr_in*>(&endpoint.storage_);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        std::memcpy(&v4->sin_addr, address.data(), 4);
        endpoint.length_ = sizeof(sockaddr_in);
    } else if (address.size() == 16) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&endpoint.storage_);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        std::memcpy(&v6->sin6_addr, address.data(), 16);
        endpoint.length_ = sizeof(sockaddr_in6);
    }
    return endpoint;
}

bool Endpoint::operator==(const Endpoint& other) const noexcept {
    if (family() != other.family() || port() != other.port() || addressBytes() != other.addressBytes()) {
        return false;
    }
    return !ipv6() || reinterpret_cast<const sockaddr_in6*>(&storage_)->sin6_scope_id ==
                          reinterpret_cast<const sockaddr_in6*>(&other.storage_)->sin6_scope_id;
}

// ---- DatagramSocket -----------------------------------------------------------

#ifdef _WIN32
// Closing the socket ends its association; the event goes after it.
DatagramSocket::~DatagramSocket() {
    if (event_) {
        WSACloseEvent(event_);
    }
}

void DatagramSocket::watchable(Handle handle, long events) {
    event_ = WSACreateEvent();
    if (event_ == WSA_INVALID_EVENT || WSAEventSelect(handle, event_, events) != 0) {
        fail(RCS_ERROR_NETWORK, "Could not watch the socket: " + lastSocketErrorText());
    }
}

void DatagramSocket::watchable() {
    event_ = WSACreateEvent();
    if (event_ == WSA_INVALID_EVENT) {
        fail(RCS_ERROR_INTERNAL, "Could not create an event: " + lastSocketErrorText());
    }
}
#else
DatagramSocket::~DatagramSocket() = default;
#endif

size_t DatagramSocket::sendBatch(std::span<const Outgoing> batch) {
    size_t marked = 0;
    for (const auto& datagram : batch) {
        if (datagram.dscp) {
            marked += sendMarked(*datagram.to, datagram.datagram, datagram.dscp);
        } else {
            send(*datagram.to, datagram.datagram);
        }
    }
    return marked;
}

// ---- DatagramBatcher ----------------------------------------------------------

namespace {

bool sameAddress(const Endpoint& a, const Endpoint& b) noexcept {
    return &a == &b || (a.size() == b.size() && std::memcmp(a.get(), b.get(), a.size()) == 0);
}

} // namespace

void DatagramBatcher::send(std::span<const Outgoing> batch, std::atomic<bool>& failing, const Endpoint& local) {
    std::array<Message, maxMessages> messages;
    for (size_t next = 0; next < batch.size();) {
        // The call: messages from `next` on, runs where the system cuts them.
        const bool runs = segmenting_.load(std::memory_order_relaxed);
        size_t count = 0;
        for (size_t at = next, datagrams = 0; at < batch.size() && count < maxMessages && datagrams < maxDatagrams;) {
            const auto& first = batch[at];
            Message message{at, 1, first.datagram.size(), first.dscp};
            size_t bytes = message.segment;
            while (runs && message.segment && message.segment <= maxSegment && at + message.count < batch.size() &&
                   message.count < maxRun && datagrams + message.count < maxDatagrams) {
                const auto& more = batch[at + message.count];
                const size_t size = more.datagram.size();
                if (!size || size > message.segment || bytes + size > maxRunBytes || more.dscp != message.dscp ||
                    !sameAddress(*more.to, *first.to)) {
                    break;
                }
                bytes += size;
                ++message.count;
                if (size < message.segment) {
                    break; // A shorter one ends its run.
                }
            }
            messages[count++] = message;
            datagrams += message.count;
            at += message.count;
        }
        int error = 0;
        const int sent = system_(batch, std::span(messages).first(count), error);
        if (sent > 0) {
            next = messages[size_t(sent) - 1].first + messages[size_t(sent) - 1].count;
            failing = false;
            continue;
        }
        const Message& refused = messages[0];
        if (refused.count > 1 && (error == EIO || error == EINVAL)) {
            if (segmenting_.exchange(false)) {
                log::info("network",
                          "UDP " + local.toString() +
                              ": the system refused a run of datagrams for it to cut (UDP GSO; error " +
                              std::to_string(error) +
                              (error == EIO ? ": the network card computes no checksums" : ": a path's MTU") +
                              "); from now on the socket hands it each datagram on its own, still several "
                              "in a call (sendmmsg)");
            }
            continue; // Again, as datagrams.
        }
        if (error == EINTR) {
            continue;
        }
        if (error == EAGAIN || error == EWOULDBLOCK) {
            return; // No room for the rest either.
        }
        failing = error != ENOBUFS && error != ENOMEM;
        next = refused.first + refused.count;
    }
}

// ---- UdpSocket ----------------------------------------------------------------

#ifdef __linux__
namespace {

// The messages of a DatagramBatcher's call, through sendmmsg.
int sendMessages(Handle handle, bool v6, std::span<const Outgoing> batch,
                 std::span<const DatagramBatcher::Message> messages, int& error) {
    // Room for a run's segment size and a class, per message.
    constexpr size_t segmentSpace = CMSG_SPACE(sizeof(uint16_t)), controlSpace = segmentSpace + CMSG_SPACE(sizeof(int));
    struct alignas(cmsghdr) Control {
        uint8_t bytes[controlSpace];
    };
    std::array<mmsghdr, DatagramBatcher::maxMessages> headers{};
    std::array<iovec, DatagramBatcher::maxDatagrams> parts;
    std::array<Control, DatagramBatcher::maxMessages> controls;
    size_t part = 0;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& message = messages[i];
        auto& header = headers[i].msg_hdr;
        header.msg_name = const_cast<sockaddr*>(batch[message.first].to->get());
        header.msg_namelen = batch[message.first].to->size();
        header.msg_iov = &parts[part];
        header.msg_iovlen = message.count;
        for (size_t d = message.first; d < message.first + message.count; ++d) {
            parts[part++] = {const_cast<uint8_t*>(batch[d].datagram.data()), batch[d].datagram.size()};
        }
        uint8_t* control = controls[i].bytes;
        size_t length = 0;
        if (message.count > 1) {
            auto* segment = reinterpret_cast<cmsghdr*>(control);
            segment->cmsg_level = IPPROTO_UDP;
            segment->cmsg_type = UDP_SEGMENT;
            segment->cmsg_len = CMSG_LEN(sizeof(uint16_t));
            const auto size = uint16_t(message.segment);
            std::memcpy(CMSG_DATA(segment), &size, sizeof(size));
            length = segmentSpace;
        }
        if (message.dscp) {
            auto* mark = reinterpret_cast<cmsghdr*>(control + length);
            mark->cmsg_level = v6 ? IPPROTO_IPV6 : IPPROTO_IP;
            mark->cmsg_type = v6 ? IPV6_TCLASS : IP_TOS;
            mark->cmsg_len = CMSG_LEN(sizeof(int));
            const int value = message.dscp << 2;
            std::memcpy(CMSG_DATA(mark), &value, sizeof(value));
            length += CMSG_SPACE(sizeof(int));
        }
        if (length) {
            header.msg_control = control;
            header.msg_controllen = length;
        }
    }
    const int sent = ::sendmmsg(handle, headers.data(), unsigned(messages.size()), 0);
    if (sent < 0) {
        error = errno;
    }
    return sent;
}

} // namespace
#endif

UdpSocket::UdpSocket(const Endpoint& local)
#ifdef __linux__
    : batcher_([this](std::span<const Outgoing> batch, std::span<const DatagramBatcher::Message> messages, int& error) {
          return sendMessages(socket_.get(), v6_, batch, messages, error);
      })
#endif
{
    startNetworking();
    socket_ = Socket(::socket(local.family(), SOCK_DGRAM, IPPROTO_UDP));
    if (socket_.get() == invalidHandle) {
        fail(RCS_ERROR_NETWORK, "Could not create a UDP socket: " + lastSocketErrorText());
    }
    makeNonBlocking(socket_.get());
    const Handle handle = socket_.get();
    v6_ = local.ipv6();
    if (v6_) { // One family per socket; the caller binds both when it wants both.
        int only = 1;
        setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only), sizeof(only));
    }
    // Room for a 4K keyframe burst in either direction.
    int buffer = 4 * 1024 * 1024;
    setsockopt(handle, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
    setsockopt(handle, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
#ifdef _WIN32
    // An ICMP "port unreachable" or "time exceeded" would otherwise fail the next receive.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(handle, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
    WSAIoctl(handle, SIO_UDP_NETRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#endif
    if (bind(handle, local.get(), local.size()) != 0) {
        fail(RCS_ERROR_NETWORK, "Could not bind UDP " + local.toString() + ": " + lastSocketErrorText());
    }
    local_ = localAddress(handle);
#ifdef _WIN32
    watchable(handle, FD_READ);
#endif
#ifdef __linux__
    int segment = 0;
    socklen_t size = sizeof(segment);
    batcher_.setSegmenting(getsockopt(handle, IPPROTO_UDP, UDP_SEGMENT, &segment, &size) == 0);
    static std::atomic<bool> told{false};
    if (!batcher_.segmenting() && !told.exchange(true)) {
        log::info("network", "This system cuts no runs of datagrams (UDP GSO, Linux 4.18 and later): UDP sockets hand "
                             "it their datagrams one by one, several in a call (sendmmsg)");
    }
#endif
}

void bindToInterface(Handle handle, bool v6, uint32_t index) {
#ifdef _WIN32
    // IP_UNICAST_IF takes the index in network byte order, IPV6_UNICAST_IF in host order.
    const DWORD value = v6 ? DWORD(index) : htonl(index);
    const int result = setsockopt(handle, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_UNICAST_IF : IP_UNICAST_IF,
                                  reinterpret_cast<const char*>(&value), sizeof(value));
#elif defined(__EMSCRIPTEN__)
    (void)handle, (void)v6, (void)index;
    const int result = -1; // A browser chooses no interface (local_interface is UNSUPPORTED there).
#elif defined(__linux__)
#ifdef __ANDROID__
    if (bindToAndroidNetwork(handle, index)) {
        return;
    }
#endif
    // By name; Linux 5.7 and later let any process bind a socket that is not bound to a device yet.
    (void)v6;
    char name[IF_NAMESIZE] = {};
    const int result = if_indextoname(index, name)
                           ? setsockopt(handle, SOL_SOCKET, SO_BINDTODEVICE, name, socklen_t(std::strlen(name)))
                           : -1;
#else
    const int value = int(index);
    const int result =
        setsockopt(handle, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_BOUND_IF : IP_BOUND_IF, &value, sizeof(value));
#endif
    if (result != 0) {
        fail(RCS_ERROR_NETWORK,
             "Could not bind the socket to interface " + std::to_string(index) + ": " + lastSocketErrorText());
    }
}

Endpoint localAddress(Handle handle) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        fail(RCS_ERROR_NETWORK, "Could not read the socket's address: " + lastSocketErrorText());
    }
    return Endpoint::fromSockaddr(reinterpret_cast<const sockaddr*>(&address), size_t(length));
}

namespace {

std::atomic<bool> filtering{false};
std::mutex filterMutex;
DatagramFilter datagramFilter; // Under filterMutex.

bool passes(const Endpoint& local, const Endpoint& remote) {
    if (!filtering.load(std::memory_order_relaxed)) {
        return true;
    }
    std::lock_guard lock(filterMutex);
    return !datagramFilter || datagramFilter(local, remote);
}

} // namespace

void setDatagramFilter(DatagramFilter filter) {
    std::lock_guard lock(filterMutex);
    datagramFilter = std::move(filter);
    filtering = bool(datagramFilter);
}

std::optional<Endpoint> sourceFor(const Endpoint& remote) {
    startNetworking();
    const Socket socket(::socket(remote.family(), SOCK_DGRAM, IPPROTO_UDP));
    if (socket.get() == invalidHandle || ::connect(socket.get(), remote.get(), remote.size()) != 0) {
        return std::nullopt; // Connecting a datagram socket only picks the route.
    }
    try {
        auto source = localAddress(socket.get());
        source.setPort(0);
        return source;
    } catch (const Failure&) {
        return std::nullopt;
    }
}

Socket openTcpSocket(int family) {
    startNetworking();
    Socket socket(::socket(family, SOCK_STREAM, IPPROTO_TCP));
    if (socket.get() == invalidHandle) {
        fail(RCS_ERROR_NETWORK, "Could not create a socket: " + lastSocketErrorText());
    }
    makeNonBlocking(socket.get());
    return socket;
}

Socket connectTcp(const Endpoint& peer, const std::string& what, CancelToken cancelled,
                  std::chrono::milliseconds timeout, uint32_t interfaceIndex) {
    Socket socket = openTcpSocket(peer.family());
    if (interfaceIndex) {
        bindToInterface(socket.get(), peer.ipv6(), interfaceIndex);
    }
    if (::connect(socket.get(), peer.get(), peer.size()) != 0) {
        if (!wouldBlock()) {
            fail(RCS_ERROR_NETWORK, "Connecting to " + what + " failed: " + lastSocketErrorText());
        }
        try {
            waitReady(socket.get(), true, cancelled, Clock::now() + timeout);
        } catch (const Failure& failure) {
            if (failure.result() == RCS_ERROR_CANCELLED) {
                throw;
            }
            const auto ms = timeout.count();
            fail(RCS_ERROR_NETWORK, "No TCP connection to " + what + " within " +
                                        (ms % 1000 ? std::to_string(ms) + " ms" : std::to_string(ms / 1000) + " s") +
                                        " (nothing answers there, or a firewall drops it)");
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 || error) {
            fail(RCS_ERROR_NETWORK,
                 "Connecting to " + what + " failed: " + describeSocketError(error ? error : lastSocketError()));
        }
    }
    return socket;
}

void waitReady(Handle handle, bool write, CancelToken cancelled, Clock::time_point deadline,
               const std::atomic<bool>* stopped) {
    for (;;) {
        throwIfCancelled(cancelled);
        if (stopped) {
            throwIfCancelled(*stopped);
        }
        if (Clock::now() >= deadline) {
            fail(RCS_ERROR_NETWORK, write ? "Network operation timed out (the peer stopped taking data)"
                                          : "Network operation timed out (no data from the peer)");
        }
#ifdef _WIN32
        WSAPOLLFD item{handle, static_cast<SHORT>(write ? POLLWRNORM : POLLRDNORM), 0};
        const int ready = WSAPoll(&item, 1, 20);
#else
        pollfd item{handle, static_cast<short>(write ? POLLOUT : POLLIN), 0};
        const int ready = poll(&item, 1, 20);
#endif
        if (ready > 0) {
            return;
        }
        if (ready < 0 && !wouldBlock()) {
            fail(RCS_ERROR_NETWORK, "Socket polling failed");
        }
    }
}

void waitAny(std::span<const Readiness> sockets, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    std::vector<WSAPOLLFD> items;
    for (const auto& socket : sockets) {
        items.push_back({socket.handle, static_cast<SHORT>(socket.write ? POLLWRNORM : POLLRDNORM), 0});
    }
    WSAPoll(items.data(), ULONG(items.size()), int(timeout.count()));
#else
    std::vector<pollfd> items;
    for (const auto& socket : sockets) {
        items.push_back({socket.handle, static_cast<short>(socket.write ? POLLOUT : POLLIN), 0});
    }
    ::poll(items.data(), nfds_t(items.size()), int(timeout.count()));
#endif
}

Endpoint UdpSocket::local() const {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getsockname(socket_.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        fail(RCS_ERROR_NETWORK, "Could not read the UDP socket's address: " + lastSocketErrorText());
    }
    return Endpoint::fromSockaddr(reinterpret_cast<const sockaddr*>(&address), size_t(length));
}

bool UdpSocket::send(const Endpoint& to, std::span<const uint8_t> datagram) {
#ifndef __linux__
    std::lock_guard lock(sendMutex_);
#endif
    return sendUnlocked(to, datagram);
}

#ifdef __linux__
bool UdpSocket::sendMarked(const Endpoint& to, std::span<const uint8_t> datagram, uint8_t dscp) {
    const Outgoing marked{&to, datagram, dscp};
    return sendBatch({&marked, 1}) == 1;
}

size_t UdpSocket::sendBatch(std::span<const Outgoing> batch) {
    if (filtering.load(std::memory_order_relaxed)) {
        std::vector<Outgoing> passing; // The rest is lost on the way.
        std::copy_if(batch.begin(), batch.end(), std::back_inserter(passing),
                     [&](const Outgoing& datagram) { return passes(local_, *datagram.to); });
        batcher_.send(passing, failing_, local_);
    } else {
        batcher_.send(batch, failing_, local_);
    }
    return size_t(std::count_if(batch.begin(), batch.end(), [](const Outgoing& datagram) { return datagram.dscp; }));
}
#else
bool UdpSocket::sendMarked(const Endpoint& to, std::span<const uint8_t> datagram, uint8_t dscp) {
    std::lock_guard lock(sendMutex_);
    const bool marked = setTrafficClass(dscp << 2);
    sendUnlocked(to, datagram);
    if (marked) {
        setTrafficClass(0);
    }
    return marked;
}

bool UdpSocket::setTrafficClass(int value) {
    return setsockopt(socket_.get(), v6_ ? IPPROTO_IPV6 : IPPROTO_IP, v6_ ? IPV6_TCLASS : IP_TOS,
                      reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}
#endif

#if !defined(__linux__) && !defined(_WIN32)
size_t UdpSocket::sendBatch(std::span<const Outgoing> batch) {
    std::lock_guard lock(sendMutex_);
    size_t marked = 0;
    for (const auto& datagram : batch) {
        const bool mark = datagram.dscp && setTrafficClass(datagram.dscp << 2);
        sendUnlocked(*datagram.to, datagram.datagram);
        if (mark) {
            setTrafficClass(0);
            ++marked;
        }
    }
    return marked;
}
#endif

bool UdpSocket::sendUnlocked(const Endpoint& to, std::span<const uint8_t> datagram) {
    if (!passes(local_, to)) {
        return true; // Lost on the way.
    }
#ifdef _WIN32
    const int sent = ::sendto(socket_.get(), reinterpret_cast<const char*>(datagram.data()), int(datagram.size()), 0,
                              to.get(), to.size());
#else
    const auto sent = ::sendto(socket_.get(), datagram.data(), datagram.size(), 0, to.get(), to.size());
#endif
    // Full buffers and transient routing errors drop the datagram, as the
    // network itself may; the protocol above recovers. An error of this
    // end's that stays (no route, the address gone) is noted for it.
    if (sent == int(datagram.size())) {
        failing_ = false;
        return true;
    }
    if (sent < 0 && !wouldBlock()) {
        const int code = lastSocketError();
#ifdef _WIN32
        failing_ = code != WSAENOBUFS;
#else
        failing_ = code != ENOBUFS && code != ENOMEM;
#endif
    }
    return false;
}

namespace {

// What a datagram socket reports about an earlier datagram (an ICMP message),
// or a moment without buffers: the socket itself is fine.
bool transientDatagramError(int code) noexcept {
#ifdef _WIN32
    return code == WSAECONNRESET || code == WSAENETRESET || code == WSAEHOSTUNREACH || code == WSAENETUNREACH ||
           code == WSAENOBUFS;
#else
    return code == ECONNREFUSED || code == ECONNRESET || code == EHOSTUNREACH || code == ENETUNREACH ||
           code == EHOSTDOWN || code == ENOBUFS || code == ENOMEM;
#endif
}

} // namespace

std::optional<size_t> UdpSocket::receive(std::span<uint8_t> buffer, Endpoint& from) {
    // Errors about earlier datagrams are consumed as they are reported; a
    // bounded number of them per call keeps a flood of them from holding the caller.
    for (int skipped = 0; skipped < 64; ++skipped) {
        sockaddr_storage address{};
#ifdef _WIN32
        int length = sizeof(address);
        const int received = ::recvfrom(socket_.get(), reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                                        reinterpret_cast<sockaddr*>(&address), &length);
        if (received < 0 && WSAGetLastError() == WSAEMSGSIZE) {
            continue; // Larger than the buffer: not one of ours, dropped.
        }
#else
        iovec part{buffer.data(), buffer.size()};
        msghdr header{};
        header.msg_name = &address;
        header.msg_namelen = sizeof(address);
        header.msg_iov = &part;
        header.msg_iovlen = 1;
        const auto received = ::recvmsg(socket_.get(), &header, 0);
        if (received >= 0 && (header.msg_flags & MSG_TRUNC)) {
            continue; // Larger than the buffer: not one of ours, dropped.
        }
        const auto length = header.msg_namelen;
#endif
        if (received < 0) {
            if (wouldBlock()) {
                return std::nullopt;
            }
            const int code = lastSocketError();
            if (transientDatagramError(code)) {
                continue;
            }
            fail(RCS_ERROR_NETWORK, "Receiving on UDP " + local().toString() + " failed: " + describeSocketError(code));
        }
        from = Endpoint::fromSockaddr(reinterpret_cast<const sockaddr*>(&address), size_t(length));
        if (!passes(local_, from)) {
            continue; // Lost on the way.
        }
        return size_t(received);
    }
    return std::nullopt;
}

// ---- Waiter -------------------------------------------------------------------

#ifdef _WIN32
struct Waiter::Impl {
    std::vector<const DatagramSocket*> sockets;
    std::vector<const DatagramSocket*> attached; // Those that wake this Waiter themselves.
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~Impl() {
        CloseHandle(wake);
        CloseHandle(timer);
    }
};

Waiter::Waiter() : impl_(std::make_unique<Impl>()) {
    if (!impl_->wake || !impl_->timer) {
        fail(RCS_ERROR_INTERNAL, "Could not create the network thread's events");
    }
}

void Waiter::watch(const DatagramSocket& socket) {
    impl_->sockets.push_back(&socket);
    if (socket.attachWaiter(*this)) {
        impl_->attached.push_back(&socket);
    }
}

void Waiter::wake() noexcept {
    SetEvent(impl_->wake);
}

void Waiter::wait(Clock::time_point deadline) {
    std::vector<HANDLE> handles;
    for (const auto* socket : impl_->sockets) {
        if (socket->buffered()) {
            return;
        }
        if (socket->readable()) { // Those without one wake the Waiter themselves.
            handles.push_back(socket->readable());
        }
    }
    handles.push_back(impl_->wake);
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) {
        return;
    }
    LARGE_INTEGER due{};
    due.QuadPart =
        -std::max<LONGLONG>(1, LONGLONG(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100));
    SetWaitableTimer(impl_->timer, &due, 0, nullptr, nullptr, FALSE);
    handles.push_back(impl_->timer);
    WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, INFINITE);
    CancelWaitableTimer(impl_->timer);
    for (const auto* socket : impl_->sockets) {
        if (socket->readable()) {
            WSAResetEvent(socket->readable()); // Signalled again by the next receive while data remains.
        }
    }
}
#elif defined(__EMSCRIPTEN__)
// A browser: poll() and pipes would be carried out by the page's main thread
// (Emscripten proxies those calls), so a busy page would hold up every wait.
// The sockets here wake the waiter themselves (attachWaiter), and the waits
// are a condition variable's.
struct Waiter::Impl {
    std::vector<const DatagramSocket*> sockets;
    std::vector<const DatagramSocket*> attached;
    std::mutex mutex;
    std::condition_variable woken;
    bool signalled = false;
};

Waiter::Waiter() : impl_(std::make_unique<Impl>()) {}

void Waiter::watch(const DatagramSocket& socket) {
    impl_->sockets.push_back(&socket);
    if (socket.attachWaiter(*this)) {
        impl_->attached.push_back(&socket);
    }
}

void Waiter::wake() noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->signalled = true;
    }
    impl_->woken.notify_one();
}

void Waiter::wait(Clock::time_point deadline) {
    for (const auto* socket : impl_->sockets) {
        if (socket->buffered()) {
            return;
        }
    }
    std::unique_lock lock(impl_->mutex);
    impl_->woken.wait_until(lock, deadline, [&] { return impl_->signalled; });
    impl_->signalled = false;
}
#else
struct Waiter::Impl {
    std::vector<const DatagramSocket*> sockets;
    std::vector<const DatagramSocket*> attached; // Those that wake this Waiter themselves.
    int pipe[2] = {-1, -1};
    ~Impl() {
        for (const int end : pipe) {
            if (end >= 0) {
                ::close(end);
            }
        }
    }
};

Waiter::Waiter() : impl_(std::make_unique<Impl>()) {
    if (::pipe(impl_->pipe) != 0) {
        fail(RCS_ERROR_INTERNAL, "Could not create the network thread's wake pipe");
    }
    for (const int end : impl_->pipe) {
        fcntl(end, F_SETFL, fcntl(end, F_GETFL) | O_NONBLOCK);
    }
}

void Waiter::watch(const DatagramSocket& socket) {
    impl_->sockets.push_back(&socket);
    if (socket.attachWaiter(*this)) {
        impl_->attached.push_back(&socket);
    }
}

void Waiter::wake() noexcept {
    const uint8_t byte = 1;
    [[maybe_unused]] const auto written = ::write(impl_->pipe[1], &byte, 1);
}

void Waiter::wait(Clock::time_point deadline) {
    std::vector<pollfd> items;
    for (const auto* socket : impl_->sockets) {
        if (socket->buffered()) {
            return;
        }
        items.push_back({socket->handle(), POLLIN, 0}); // poll() skips those without one (-1).
    }
    items.push_back({impl_->pipe[0], POLLIN, 0});
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
    if (remaining.count() <= 0) {
        return;
    }
    const int timeout = int((remaining.count() + 999) / 1000);
    if (::poll(items.data(), nfds_t(items.size()), timeout) > 0 && items.back().revents) {
        uint8_t drain[64];
        while (::read(impl_->pipe[0], drain, sizeof(drain)) > 0) {
        }
    }
}
#endif

Waiter::~Waiter() {
    for (const auto* socket : impl_->attached) {
        socket->detachWaiter(*this);
    }
}

void Waiter::unwatch(const DatagramSocket& socket) {
    std::erase(impl_->sockets, &socket);
    if (std::erase(impl_->attached, &socket)) {
        socket.detachWaiter(*this);
    }
}

} // namespace rcsdk::transport
