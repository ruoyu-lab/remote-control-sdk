// The last socket error as an English phrase plus its code, for error texts
// and logs (Windows' own messages are localized and not UTF-8).
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#else
#include <cerrno>
#endif

#include <string>

namespace rcsdk::transport {

inline int lastSocketError() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// The process (or the system) has no descriptor left for another socket.
inline bool outOfDescriptors(int code) noexcept {
#ifdef _WIN32
    return code == WSAEMFILE || code == WSAENOBUFS;
#else
    return code == EMFILE || code == ENFILE;
#endif
}

inline std::string describeSocketError(int code) {
    const char* text = nullptr;
    switch (code) {
#ifdef _WIN32
    case WSAECONNREFUSED:
#else
    case ECONNREFUSED:
#endif
        text = "connection refused (nothing listens there, or a firewall rejects it)";
        break;
#ifdef _WIN32
    case WSAETIMEDOUT:
#else
    case ETIMEDOUT:
#endif
        text = "timed out";
        break;
#ifdef _WIN32
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
#else
    case ENETUNREACH:
    case EHOSTUNREACH:
#endif
        text = "network or host unreachable";
        break;
#ifdef _WIN32
    case WSAECONNRESET:
#else
    case ECONNRESET:
#endif
        text = "connection reset by the peer";
        break;
#ifdef _WIN32
    case WSAEADDRINUSE:
#else
    case EADDRINUSE:
#endif
        text = "address already in use";
        break;
#ifdef _WIN32
    case WSAEADDRNOTAVAIL:
#else
    case EADDRNOTAVAIL:
#endif
        text = "address not available on this machine";
        break;
#ifdef _WIN32
    case WSAEACCES:
#else
    case EACCES:
    case EPERM:
#endif
        text = "permission denied";
        break;
#ifdef _WIN32
    case WSAEMFILE:
#else
    case EMFILE:
    case ENFILE:
#endif
        text = "no descriptor left (the process's or the system's limit)";
        break;
    default:
        text = "system error";
        break;
    }
    return std::string(text) + " (" + std::to_string(code) + ")";
}

inline std::string lastSocketErrorText() {
    return describeSocketError(lastSocketError());
}

} // namespace rcsdk::transport
