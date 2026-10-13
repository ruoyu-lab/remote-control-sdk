// Just enough of BSD sockets and Winsock for the host-relay example and its
// client: TCP and UDP to a numeric "address:port". Test software, not part of
// the SDK.
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace relay {

#ifdef _WIN32
using Handle = SOCKET;
inline constexpr Handle noSocket = INVALID_SOCKET;
#else
using Handle = int;
inline constexpr Handle noSocket = -1;
#endif

// The text that starts the relay's own messages: a viewer's hello ("RCSRELAY
// room", a line over TCP) and, over UDP, a notice that the other end changed
// ("RCSRELAY changed") or, to a sharer behind a route, that a viewer left
// ("RCSRELAY closed"). A sharer's hello is "RCSSHARE room". The SDK's
// datagrams never start with either (their first byte is 0xD1..0xD8).
inline constexpr char tag[] = "RCSRELAY ";
inline constexpr size_t tagSize = sizeof(tag) - 1;
inline constexpr char shareTag[] = "RCSSHARE ";
inline constexpr size_t shareTagSize = sizeof(shareTag) - 1;
// A sharer's connection carries every viewer's bytes, each piece behind the
// route the relay gave that viewer: over TCP route(8) length(4) bytes (length
// 0: that viewer left, or the sharer drops it), over UDP route(8) datagram;
// big-endian.
inline constexpr size_t routeHeader = 12;
inline void putNumber(std::vector<uint8_t>& out, uint64_t value, size_t width) {
    for (size_t i = width; i > 0; --i) {
        out.push_back(uint8_t(value >> ((i - 1) * 8)));
    }
}
inline uint64_t getNumber(const uint8_t* bytes, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
        value = value << 8 | bytes[i];
    }
    return value;
}

inline void startSockets() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started) {
        throw std::runtime_error("Winsock did not start");
    }
#endif
}

inline void closeSocket(Handle handle) {
    if (handle != noSocket) {
#ifdef _WIN32
        closesocket(handle);
#else
        ::close(handle);
#endif
    }
}

// Ends both directions of a connection a thread may be waiting on.
inline void shutdownSocket(Handle handle) {
    if (handle != noSocket) {
#ifdef _WIN32
        shutdown(handle, SD_BOTH);
#else
        shutdown(handle, SHUT_RDWR);
#endif
    }
}

struct Address {
    sockaddr_storage storage{};
    socklen_t length = 0;
    const sockaddr* get() const { return reinterpret_cast<const sockaddr*>(&storage); }
    bool operator==(const Address& other) const {
        return length == other.length && std::memcmp(&storage, &other.storage, length) == 0;
    }
};

// "192.168.1.2:47910" or "[2001:db8::1]:47910".
inline Address parseAddress(const std::string& text) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
        throw std::invalid_argument("expected address:port, got \"" + text + "\"");
    }
    std::string host = text.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    const int port = std::stoi(text.substr(colon + 1));
    Address address;
    auto* v4 = reinterpret_cast<sockaddr_in*>(&address.storage);
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&address.storage);
    if (inet_pton(AF_INET, host.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(uint16_t(port));
        address.length = sizeof(sockaddr_in);
    } else if (inet_pton(AF_INET6, host.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(uint16_t(port));
        address.length = sizeof(sockaddr_in6);
    } else {
        throw std::invalid_argument("\"" + host + "\" is not a numeric IPv4 or IPv6 address");
    }
    return address;
}

inline Handle openSocket(int family, int type) {
    startSockets();
    const Handle handle = ::socket(family, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    if (handle == noSocket) {
        throw std::runtime_error("cannot create a socket");
    }
    if (type == SOCK_STREAM) {
        int on = 1; // Small writes go out at once: the session paces them.
        setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
#ifdef SO_NOSIGPIPE
        setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); // A closed peer is an error, not a signal.
#endif
    }
    return handle;
}

inline void makeNonBlocking(Handle handle) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(handle, FIONBIO, &on);
#else
    fcntl(handle, F_SETFL, fcntl(handle, F_GETFL) | O_NONBLOCK);
#endif
}

// The last call would have blocked: try again later.
inline bool wouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

#ifdef _WIN32
using PollItem = WSAPOLLFD;
inline constexpr short readEvent = POLLRDNORM, writeEvent = POLLWRNORM;
inline int pollItems(PollItem* items, size_t count, int timeoutMs) { return WSAPoll(items, ULONG(count), timeoutMs); }
#else
using PollItem = pollfd;
inline constexpr short readEvent = POLLIN, writeEvent = POLLOUT;
inline int pollItems(PollItem* items, size_t count, int timeoutMs) { return ::poll(items, nfds_t(count), timeoutMs); }
#endif

// Waits until `handle` is readable or `timeoutMs` passed; true when readable.
inline bool readable(Handle handle, int timeoutMs) {
    PollItem item{handle, readEvent, 0};
    return pollItems(&item, 1, timeoutMs) > 0;
}

} // namespace relay
