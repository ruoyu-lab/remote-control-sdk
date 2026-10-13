// WebSockets (RFC 6455) as far as datagrams over them need: the opening
// handshake on either side (an HTTP/1.1 GET with Upgrade, answered 101), and
// binary messages, each one datagram. A forwarder's WebSocket entry is the
// server (server/forwarder), an end that reaches it (or a proxy in front of
// it) the client (framed_socket.hpp). No extension is agreed (a request's
// Sec-WebSocket-Extensions goes unanswered: no compression), nor a
// subprotocol; text messages are refused.
//
// What the other side sends is bounded, so that it can make this end hold
// little: an opening request or response of at most maxHandshake bytes and
// maxHeaders header lines; a message of at most maxMessage bytes, however
// many frames carry it; control frames of at most 125 bytes, never split. A
// frame that breaks RFC 6455 (a client's not masked, a server's masked,
// reserved bits or opcodes, a length not in its shortest form, a
// continuation of nothing) fails with the close code that says so.
#pragma once

#include "util/util.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rcsdk::transport::websocket {

inline constexpr size_t maxHandshake = 8192;
inline constexpr size_t maxHeaders = 64;
inline constexpr size_t maxMessage = 65535; // A datagram.
inline constexpr size_t maxControl = 125;
inline constexpr size_t maxFrameHeader = 14;

enum Opcode : uint8_t { continuation = 0, text = 1, binary = 2, close = 8, ping = 9, pong = 10 };
// Close codes (RFC 6455 section 7.4.1).
enum CloseCode : uint16_t {
    normalClosure = 1000,
    goingAway = 1001,
    protocolError = 1002,
    unsupportedData = 1003,
    tooBig = 1009
};

// A frame or opening that breaks the rules: the close code (or HTTP answer)
// the other side is to get, and why.
class Violation : public Failure {
public:
    Violation(CloseCode code, const std::string& why) : Failure(RCS_ERROR_PROTOCOL, why), code_(code) {}
    CloseCode code() const noexcept { return code_; }

private:
    CloseCode code_;
};

// ---- The opening handshake ---------------------------------------------------------------

// Server: the client's opening request at the start of `bytes`, accepted:
// how many bytes it took, the answer (101) and the path it asked for. None
// while it has not all arrived.
struct Accepted {
    size_t consumed = 0;
    std::string response;
    std::string path;
};
// Throws Refused for a request that is no WebSocket opening, with the HTTP
// answer it gets before the connection closes (400, 405, 426, 431).
class Refused : public Failure {
public:
    Refused(std::string response, const std::string& why)
        : Failure(RCS_ERROR_PROTOCOL, why), response_(std::move(response)) {}
    const std::string& response() const noexcept { return response_; }

private:
    std::string response_;
};
std::optional<Accepted> accept(std::span<const uint8_t> bytes);

// Where a client goes: "ws://" or "wss://", the host (a name, IPv4 or
// [IPv6]), an optional port (80 and 443 by default), the path ("/" when none,
// a query may follow); no user, fragment, spaces or control characters, at
// most 255 bytes. Throws INVALID_ARGUMENT saying what is wrong.
struct Url {
    bool tls = false;
    std::string host; // Without brackets.
    uint16_t port = 0;
    std::string target;     // The path and query.
    std::string hostHeader; // As the Host header carries it.
};
Url parseUrl(std::string_view url);

// Client: the request for `path` (origin form, "/..." ) of `host` (the Host
// header: "name" or "name:port"), with a key of its own.
struct Opening {
    std::string request;
    std::string key;
};
Opening open(const std::string& host, const std::string& path);
// The server's answer at the start of `bytes`: how many bytes it took; none
// while it has not all arrived. Throws NETWORK for anything but a 101 that
// accepts `key` and agrees to nothing else (the reason quotes its status line).
std::optional<size_t> opened(std::span<const uint8_t> bytes, const std::string& key);

// ---- Frames ------------------------------------------------------------------------------

// A message in one frame (a client masks it with a key at random); the
// second form appends it to `out` (frames of a batch, one write).
Bytes frame(Opcode opcode, std::span<const uint8_t> payload, bool mask);
void frame(Bytes& out, Opcode opcode, std::span<const uint8_t> payload, bool mask);
// A close frame with `code`.
Bytes closeFrame(CloseCode code);

// The frames of one direction, as they arrive: a server's decoder reads a
// client's (masked), a client's a server's.
class Decoder {
public:
    explicit Decoder(bool masked) : masked_(masked) {}

    struct Event {
        Opcode kind = binary; // binary: a message (a datagram); ping: to be answered (pong, same payload); close.
        Bytes payload;
    };
    // The next bytes of the stream. Push only once next() has nothing:
    // what waits is then less than one frame.
    void push(std::span<const uint8_t> bytes);
    // The next message or control frame; none while none arrived whole.
    // Throws Violation. Pongs and empty messages are skipped.
    std::optional<Event> next();
    // Nothing more comes after a close.
    bool closed() const noexcept { return closed_; }

private:
    const bool masked_;
    Bytes buffer_;
    size_t begin_ = 0;
    std::optional<Bytes> partial_; // A message whose frames are still coming.
    bool closed_ = false;
};

} // namespace rcsdk::transport::websocket
