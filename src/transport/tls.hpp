// TLS 1.2 or later from the system, or TLS 1.2 from BearSSL where the system
// has none (RCSDK_CRYPTO_BACKEND), the client side only, under TURN over TLS
// (RFC 8656 "turns") and a forwarder's WebSocket behind a TLS proxy
// ("wss"): a nonblocking byte stream that framed_socket.hpp frames datagrams
// in, as it does a TCP connection.
//
// macOS and iOS (platform/apple/tls_connection.mm): Network.framework. It owns its socket and
// works on a dispatch queue, so the stream keeps what arrives in memory and
// makes a pipe readable for the network thread's Waiter, and it finds the
// interface the host restricts the session to through a path monitor (it takes
// no interface index). Secure Transport on the SDK's own socket would need
// neither, but it is deprecated ("no longer supported") and stops at TLS 1.2.
// Windows (platform/windows/tls_connection.cpp): Schannel encrypts and decrypts buffers
// over the SDK's own TCP socket (connectTcp), so waiting, the interface and
// cancellation are those of TURN over TCP.
// BearSSL (transport/tls_bearssl.cpp: Android, Linux; Apple systems too, to
// test it): the same as Schannel; the system's trust store is its certificate
// files (tls_bearssl.hpp).
#pragma once

#include "transport/socket.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace rcsdk::transport {

// A nonblocking connection to one server: TCP, or TLS on it. Neither direction
// ever waits.
class ByteStream {
public:
    virtual ~ByteStream() = default;
    // How much of `bytes` the connection took now (0: none).
    virtual size_t writeSome(std::span<const uint8_t> bytes) = 0;
    // What arrived, at most `buffer`; 0 when nothing did. Throws NETWORK once
    // the server closed the connection or it broke.
    virtual size_t readSome(std::span<uint8_t> buffer) = 0;
    // Waiters watch it: readable while readSome() has something (Windows: the
    // TCP socket under the stream, whose event the TURN socket owns).
    virtual Handle handle() const noexcept = 0;
    // Bytes wait in the stream where waiting on handle() does not see them.
    virtual bool buffered() const noexcept { return false; }
    virtual Endpoint local() const = 0;
    // The last words, before the stream is destroyed: what it holds, then
    // `rest`, go out as far as the server takes them by `deadline`, whether or
    // not the run was cancelled (TLS: then close_notify). The last message is
    // often the one that matters: a goodbye the TURN server must relay.
    virtual void close(std::span<const uint8_t> rest, Clock::time_point deadline) = 0;
};

// The application protocols offered (ALPN, RFC 7443): a server sharing 443
// with HTTPS tells TURN by it; a WebSocket's opening is HTTP/1.1.
inline constexpr char turnAlpn[] = "stun.turn", httpAlpn[] = "http/1.1";

// How a TLS server proves itself (rcs_ice_servers_config, rcs_forwarder_config).
struct TlsSettings {
    // Sent to the server (SNI) and, without a pin, the name its certificate
    // must be for. Empty only with a pin: then none is sent.
    std::string serverName;
    // The SHA-256 of the certificate (DER) the server must present, whatever
    // its chain, name or dates; none: the system's trust store and serverName.
    std::optional<std::array<uint8_t, 32>> certificateSha256;
    // The application protocol offered.
    std::string alpn = turnAlpn;
    // Where the host sets the pin and the name: reasons say what to change.
    std::string pinSetting = "turn_certificate_sha256";
    std::string nameSetting = "turn_tls_server_name, or the host of turn_server";
};
// RCS_TURN_TLS: turn_tls_server_name, else the host of turn_server unless that
// is an address; turn_certificate_sha256 unless all zero.
TlsSettings tlsSettings(const rcs_ice_servers_config& config);
// TURN over TLS where turn_server names no port (RFC 8656).
inline constexpr uint16_t turnTlsPort = 5349;

// TLS 1.2 or later to `server`, through the interface `interfaceIndex` (0:
// any), handshake included, within `timeout`; `what` names the server in
// reasons ("the TURN server 192.0.2.1:443"). Throws AUTHENTICATION when the
// certificate fails `settings` (the reason says which check), NETWORK when no
// TLS connection comes about, UNSUPPORTED where this system cannot, CANCELLED.
std::unique_ptr<ByteStream> connectTls(const Endpoint& server, const TlsSettings& settings, const std::string& what,
                                       CancelToken cancelled, std::chrono::milliseconds timeout,
                                       uint32_t interfaceIndex);

// Why a certificate fails, worded alike on both systems (AUTHENTICATION).
// Empty when `certificate` (DER) is the pinned one, else why not; `setting`
// is where the host pinned it.
std::string pinProblem(std::span<const uint8_t> certificate, const std::array<uint8_t, 32>& pin,
                       const std::string& what, const std::string& setting = "turn_certificate_sha256");
// What the system's check found; `detail` is its own word for it (a code).
// `use`: no TLS server's certificate, on every system: where it says what it
// is for (extended key usage), not server authentication; or it names its
// server in its subject alone, with no subject alternative name (the common
// name is not taken, as Apple's systems and browsers do not).
enum class TrustProblem { untrusted, name, dates, other, use };
std::string trustProblem(TrustProblem problem, const TlsSettings& settings, const std::string& what,
                         const std::string& detail);

} // namespace rcsdk::transport
