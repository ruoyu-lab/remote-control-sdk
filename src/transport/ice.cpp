#include "transport/ice.hpp"

#include "transport/interfaces.hpp"
#include "transport/stun.hpp"
#include "transport/turn_tcp_socket.hpp"
#include "util/bytes.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <optional>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

namespace rcsdk::transport::ice {
namespace {

using crypto::Bytes;

constexpr auto checkInterval = 20ms; // Pacing of new checks (Ta).
constexpr auto firstRetransmit = 100ms;
// Sent at 0, 0.1, 0.3, 0.7, 1.5, 3.1 and 6.3 s: a pair fails 12.7 s after its first check.
constexpr int maxAttempts = 7;
constexpr char signalPrefix[] = "rcs1.";
// The first byte after the prefix; 3: restarts of a running connection's path.
constexpr uint8_t signalVersion = 1;
constexpr size_t sealSize = 16;         // A restart's HMAC-SHA256, cut short.
constexpr size_t maxCandidates = 32;    // In a signal.
constexpr size_t maxPeerReflexive = 32; // Learned from the peer's checks.
constexpr size_t maxPerAddress = 4;     // The peer's candidates of one IP address.
// The longest signal: a restart of the longest credentials, the most
// candidates; anything longer is refused before it is decoded.
constexpr size_t maxSignalBytes =
    2 + 8 + 4 + 1 + 1 + (1 + 64) + (1 + 128) + (1 + 64) + 1 + maxCandidates * (1 + 1 + 16 + 2 + 4) + sealSize;
constexpr size_t maxSignalText = sizeof(signalPrefix) - 1 + (maxSignalBytes * 8 + 5) / 6;

uint32_t priorityOf(Type type, uint32_t localPreference) {
    const uint32_t typePreference = type == Type::host              ? 126
                                    : type == Type::peerReflexive   ? 110
                                    : type == Type::serverReflexive ? 100
                                                                    : 0;
    return (typePreference << 24) | (localPreference << 8) | 255u;
}

uint64_t pairPriority(uint32_t controlling, uint32_t controlled) {
    return (uint64_t(std::min(controlling, controlled)) << 32) + 2 * uint64_t(std::max(controlling, controlled)) +
           (controlling > controlled ? 1 : 0);
}

std::string randomHex(size_t bytes) {
    Bytes random(bytes);
    crypto::random(random);
    return hex(random);
}

Bytes asBytes(const std::string& text) {
    return Bytes(text.begin(), text.end());
}

constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url(std::span<const uint8_t> bytes) {
    std::string text;
    uint32_t buffer = 0;
    int bits = 0;
    for (const uint8_t byte : bytes) {
        buffer = buffer << 8 | byte;
        bits += 8;
        while (bits >= 6) {
            text += alphabet[(buffer >> (bits - 6)) & 63];
            bits -= 6;
        }
    }
    if (bits) {
        text += alphabet[(buffer << (6 - bits)) & 63];
    }
    return text;
}

std::optional<Bytes> unbase64url(std::string_view text) {
    Bytes bytes;
    uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const char* at = std::strchr(alphabet, c);
        if (!c || !at) {
            return std::nullopt;
        }
        buffer = buffer << 6 | uint32_t(at - alphabet);
        bits += 6;
        if (bits >= 8) {
            bytes.push_back(uint8_t(buffer >> (bits - 8)));
            bits -= 8;
        }
    }
    return bytes;
}

[[noreturn]] void malformed(const std::string& what) {
    fail(RCS_ERROR_INVALID_ARGUMENT, "Malformed signal: " + what);
}

// Where an address is reached from. A peer's candidate is probed (STUN
// checks, a TURN permission), so one at an address no peer is at, or on this
// machine or link only, would point this end at services there.
enum class Scope : uint8_t { global, loopback, linkLocal, none };

// The scope of `address`, and its kind when it is not global. An IPv6
// address that carries an IPv4 one (mapped, NAT64's well-known prefix) is
// the IPv4 address's.
std::pair<Scope, const char*> scopeOf(const Endpoint& address) {
    auto bytes = address.addressBytes();
    const auto zeros = [&](size_t from, size_t to) {
        return std::all_of(bytes.begin() + ptrdiff_t(from), bytes.begin() + ptrdiff_t(to),
                           [](uint8_t b) { return b == 0; });
    };
    if (bytes.size() == 16) {
        constexpr uint8_t awsMetadata[16] = {0xfd, 0, 0x0e, 0xc2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x54};
        const bool mapped = zeros(0, 10) && bytes[10] == 0xff && bytes[11] == 0xff;
        const bool nat64 = bytes[0] == 0 && bytes[1] == 0x64 && bytes[2] == 0xff && bytes[3] == 0x9b && zeros(4, 12);
        if (std::equal(bytes.begin(), bytes.end(), awsMetadata)) {
            return {Scope::none, "a cloud metadata service"};
        }
        if (!mapped && !nat64) {
            if (zeros(0, 15)) {
                return bytes[15] == 1   ? std::pair{Scope::loopback, "loopback"}
                       : bytes[15] == 0 ? std::pair{Scope::none, "unspecified"}
                                        : std::pair{Scope::none, "reserved"};
            }
            if (zeros(0, 12)) {
                return {Scope::none, "reserved"}; // IPv4-compatible, deprecated.
            }
            if (bytes[0] == 0xff) {
                return {Scope::none, "multicast"};
            }
            if (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) {
                return {Scope::linkLocal, "link-local"};
            }
            return {Scope::global, nullptr};
        }
        bytes.erase(bytes.begin(), bytes.begin() + 12);
    }
    const auto is = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        return bytes[0] == a && bytes[1] == b && bytes[2] == c && bytes[3] == d;
    };
    // AWS, Google, Azure and most others; Alibaba Cloud; Azure's WireServer.
    if (is(169, 254, 169, 254) || is(100, 100, 100, 200) || is(168, 63, 129, 16)) {
        return {Scope::none, "a cloud metadata service"};
    }
    if (bytes[0] == 0) {
        return {Scope::none, "unspecified"};
    }
    if (bytes[0] == 127) {
        return {Scope::loopback, "loopback"};
    }
    if (bytes[0] == 169 && bytes[1] == 254) {
        return {Scope::linkLocal, "link-local"};
    }
    if (bytes[0] >= 224) {
        return {Scope::none, bytes[0] < 240 ? "multicast" : "broadcast or reserved"};
    }
    return {Scope::global, nullptr};
}

std::string listOf(const std::vector<Candidate>& candidates) {
    std::string text;
    for (const auto& candidate : candidates) {
        text += (text.empty() ? "" : ", ") + std::string(typeName(candidate.type)) + " " + candidate.address.toString();
    }
    return text.empty() ? "none" : text;
}

} // namespace

const char* typeName(Type type) noexcept {
    switch (type) {
    case Type::host:
        return "host";
    case Type::serverReflexive:
        return "srflx";
    case Type::peerReflexive:
        return "prflx";
    case Type::relayed:
        return "relay";
    }
    return "?";
}

std::string Path::describe() const {
    if (relay) {
        return "relay " + relay->relayed().toString() + " on TURN " + relay->server().toString() +
               (relayTransport == RCS_TURN_TCP   ? " (over TCP)"
                : relayTransport == RCS_TURN_TLS ? " (over TLS)"
                                                 : "") +
               " <-> " + remote.toString() + " (" + typeName(remoteType) + ")";
    }
    return std::string("P2P ") + (socket ? socket->local().toString() : "?") + " <-> " + remote.toString() + " (" +
           typeName(remoteType) + ")";
}

// ---- Signal ---------------------------------------------------------------------

// version(1)=1 kind(1), then by kind:
//   connect  role(1) ufrag(1+n) password(1+n) answers(1+n; empty for a
//            viewer) count(1) [type(1) length(1) address(4|16) port(2) priority(4)]...
//   restart  tag(8) generation(4) standby(1), the same as connect (answers
//            empty for one that starts, whatever the role), seal(16):
//            HMAC-SHA256 of all before it under the connection's key, cut short
//   unknown  tag(8)
namespace {

void appendDescription(Bytes& bytes, const Description& d) {
    if (d.candidates.size() > maxCandidates) { // gather() keeps the best ones.
        fail(RCS_ERROR_INTERNAL, "A signal carries at most 32 candidates");
    }
    bytes.push_back(uint8_t(d.role));
    bytes.push_back(uint8_t(d.ufrag.size()));
    bytes.insert(bytes.end(), d.ufrag.begin(), d.ufrag.end());
    bytes.push_back(uint8_t(d.password.size()));
    bytes.insert(bytes.end(), d.password.begin(), d.password.end());
    bytes.push_back(uint8_t(d.answers.size()));
    bytes.insert(bytes.end(), d.answers.begin(), d.answers.end());
    bytes.push_back(uint8_t(d.candidates.size()));
    for (const auto& c : d.candidates) {
        const auto address = c.address.addressBytes();
        bytes.push_back(uint8_t(c.type));
        bytes.push_back(uint8_t(address.size()));
        bytes.insert(bytes.end(), address.begin(), address.end());
        appendBigEndian(bytes, c.address.port(), 2);
        appendBigEndian(bytes, c.priority, 4);
    }
}

// A signal's bytes after the prefix, once they are of this version.
Bytes signalBytes(const std::string& input) {
    // Channels may add spaces or line breaks around the text.
    const auto first = input.find_first_not_of(" \t\r\n");
    const auto last = input.find_last_not_of(" \t\r\n");
    if (first != std::string::npos && last - first + 1 > maxSignalText) {
        malformed("longer than any signal (" + std::to_string(maxSignalText) + " characters)");
    }
    const std::string signal = first == std::string::npos ? std::string() : input.substr(first, last - first + 1);
    if (signal.rfind(signalPrefix, 0) != 0) {
        malformed("it does not start with \"rcs1.\"");
    }
    auto bytes = unbase64url(std::string_view(signal).substr(sizeof(signalPrefix) - 1));
    if (!bytes) {
        malformed("not base64url");
    }
    if (bytes->size() < 2 || (*bytes)[0] != signalVersion) {
        malformed("unknown version (update both ends)");
    }
    if ((*bytes)[1] > uint8_t(SignalKind::unknown)) {
        malformed("unknown kind");
    }
    return std::move(*bytes);
}

// Reads a description from `bytes` at `at` (connect: a sharer's answers a
// viewer's, a viewer's answers none; restart: either may answer one).
Description readDescription(std::span<const uint8_t> bytes, size_t& at, SignalKind kind) {
    const auto take = [&](size_t count) {
        if (at + count > bytes.size()) {
            malformed("truncated");
        }
        const auto span = bytes.subspan(at, count);
        at += count;
        return span;
    };
    Description d;
    d.role = take(1)[0];
    if (d.role != RCS_ROLE_SHARE && d.role != RCS_ROLE_VIEW) {
        malformed("unknown role");
    }
    const auto text = [&](size_t minimum, size_t maximum, const char* what) {
        const size_t length = take(1)[0];
        const auto value = take(length);
        if (length < minimum || length > maximum ||
            !std::all_of(value.begin(), value.end(), [](uint8_t c) { return std::isxdigit(c) != 0; })) {
            malformed(what);
        }
        return std::string(value.begin(), value.end());
    };
    d.ufrag = text(4, 64, "credentials");
    d.password = text(22, 128, "credentials");
    // A sharer's signal answers one of a viewer's; a viewer's answers none.
    if (kind == SignalKind::restart) {
        d.answers = text(0, 64, "the restart it answers");
        if (!d.answers.empty() && d.answers.size() < 4) {
            malformed("the restart it answers");
        }
    } else {
        d.answers = d.role == RCS_ROLE_SHARE ? text(4, 64, "the viewer signal it answers")
                                             : text(0, 0, "a viewer's signal answers none");
    }
    const size_t count = take(1)[0];
    if (count > maxCandidates) {
        malformed("too many candidates");
    }
    for (size_t i = 0; i < count; ++i) {
        Candidate c;
        const uint8_t type = take(1)[0];
        if (type > uint8_t(Type::relayed)) {
            malformed("candidate type");
        }
        c.type = Type(type);
        const size_t length = take(1)[0];
        if (length != 4 && length != 16) {
            malformed("candidate address");
        }
        const auto address = take(length);
        const auto port = take(2);
        const auto priority = take(4);
        c.address = Endpoint::fromBytes(address, uint16_t(loadBigEndian(port, 0, 2)));
        c.priority = uint32_t(loadBigEndian(priority, 0, 4));
        if (!c.address.port()) {
            malformed("candidate port");
        }
        d.candidates.push_back(c);
    }
    return d;
}

SignalTag tagAt(std::span<const uint8_t> bytes) {
    if (bytes.size() < 2 + 8) {
        malformed("truncated");
    }
    SignalTag tag{};
    std::copy_n(bytes.begin() + 2, tag.size(), tag.begin());
    return tag;
}

} // namespace

std::string encode(const Description& d) {
    Bytes bytes{signalVersion, uint8_t(SignalKind::connect)};
    appendDescription(bytes, d);
    return signalPrefix + base64url(bytes);
}

Description decode(const std::string& signal) {
    const Bytes bytes = signalBytes(signal);
    if (bytes[1] != uint8_t(SignalKind::connect)) {
        malformed("a restart of a running connection's path, not a connection's first signal");
    }
    size_t at = 2;
    auto d = readDescription(bytes, at, SignalKind::connect);
    if (at != bytes.size()) {
        malformed("trailing bytes");
    }
    return d;
}

SignalKind signalKind(const std::string& signal) {
    return SignalKind(signalBytes(signal)[1]);
}

std::string encodeRestart(const Restart& restart, std::span<const uint8_t> key) {
    Bytes bytes{signalVersion, uint8_t(SignalKind::restart)};
    bytes.insert(bytes.end(), restart.tag.begin(), restart.tag.end());
    appendBigEndian(bytes, restart.generation, 4);
    bytes.push_back(restart.standby ? 1 : 0);
    appendDescription(bytes, restart.description);
    const auto seal = crypto::hmacSha256(key, bytes);
    bytes.insert(bytes.end(), seal.begin(), seal.begin() + sealSize);
    return signalPrefix + base64url(bytes);
}

SignalTag signalTag(const std::string& signal) {
    const Bytes bytes = signalBytes(signal);
    if (bytes[1] == uint8_t(SignalKind::connect)) {
        malformed("a connection's first signal names no running connection");
    }
    return tagAt(bytes);
}

Restart decodeRestart(const std::string& signal, std::span<const uint8_t> key) {
    const Bytes bytes = signalBytes(signal);
    if (bytes[1] != uint8_t(SignalKind::restart) || bytes.size() < 2 + 8 + 4 + 1 + sealSize) {
        malformed("not a restart");
    }
    const auto sealed = std::span(bytes).first(bytes.size() - sealSize);
    const auto seal = crypto::hmacSha256(key, sealed);
    if (!crypto::equal(std::span(seal).first(sealSize), std::span(bytes).last(sealSize))) {
        fail(RCS_ERROR_AUTHENTICATION, "A restart signal whose seal does not verify (changed on the way, or made by "
                                       "another than the peer)");
    }
    Restart restart;
    restart.tag = tagAt(bytes);
    restart.generation = uint32_t(loadBigEndian(bytes, 10, 4));
    if (bytes[14] > 1) {
        malformed("standby is neither 0 nor 1");
    }
    restart.standby = bytes[14] != 0;
    size_t at = 15;
    restart.description = readDescription(sealed, at, SignalKind::restart);
    if (at != sealed.size()) {
        malformed("trailing bytes");
    }
    return restart;
}

std::string encodeUnknown(const SignalTag& tag) {
    Bytes bytes{signalVersion, uint8_t(SignalKind::unknown)};
    bytes.insert(bytes.end(), tag.begin(), tag.end());
    return signalPrefix + base64url(bytes);
}

std::optional<HostPort> splitHostPort(const std::string& server, uint16_t defaultPort) {
    std::string host = server, port = std::to_string(defaultPort);
    if (!server.empty() && server.front() == '[') {
        const auto close = server.find(']');
        if (close == std::string::npos) {
            return std::nullopt;
        }
        host = server.substr(1, close - 1);
        if (close + 1 < server.size() && server[close + 1] == ':') {
            port = server.substr(close + 2);
        }
    } else if (const auto colon = server.rfind(':');
               colon != std::string::npos && server.find(':') == colon) { // One colon: host:port.
        host = server.substr(0, colon);
        port = server.substr(colon + 1);
    }
    const unsigned long number = std::strtoul(port.c_str(), nullptr, 10);
    if (host.empty() || port.find_first_not_of("0123456789") != std::string::npos || number == 0 || number > 65535) {
        return std::nullopt;
    }
    return HostPort{host, uint16_t(number)};
}

std::vector<Endpoint> resolve(const std::string& server, uint16_t defaultPort) {
    const auto split = splitHostPort(server, defaultPort);
    if (!split) {
        return {};
    }
    if (auto numeric = Endpoint::tryParse(split->host, split->port)) {
        return {*numeric};
    }
    startNetworking();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* list = nullptr;
    if (const int error = getaddrinfo(split->host.c_str(), std::to_string(split->port).c_str(), &hints, &list);
        error != 0) {
#ifdef _WIN32
        const std::string reason = "Windows error " + std::to_string(error);
#else
        const std::string reason = gai_strerror(error);
#endif
        log::warning("network", "Resolving \"" + split->host + "\" failed: " + reason);
        return {};
    }
    std::vector<Endpoint> result;
    for (auto* entry = list; entry; entry = entry->ai_next) {
        const auto endpoint = Endpoint::fromSockaddr(entry->ai_addr, size_t(entry->ai_addrlen));
        if (endpoint.valid() && std::find(result.begin(), result.end(), endpoint) == result.end()) {
            result.push_back(endpoint);
        }
    }
    freeaddrinfo(list);
    return result;
}

// ---- Agent ----------------------------------------------------------------------

struct Agent::Impl {
    bool controlling;
    std::vector<Endpoint> stunServers;
    Ports ports;
    CancelToken cancelled;
    std::optional<RelaySettings> relaySettings;
    std::string interfaceName;
    std::chrono::milliseconds gatherTimeout;
    uint32_t extraAddresses; // rcs_ice_addresses
    std::optional<Interface> only;    // The interface the host restricted the session to.
    std::unique_ptr<TurnClient> turn; // On sockets[0] in relay mode.
    std::vector<std::unique_ptr<DatagramSocket>> sockets;
    Waiter waiter;
    std::string ufrag = randomHex(4), password = randomHex(16);
    // RFC 8445 has every check carry the sender's role and a tie-breaker, so
    // they are sent; the roles come from the signal (the viewer controls), so
    // two ends of the SDK never conflict and nothing here acts on them.
    uint64_t tieBreaker = 0;
    std::vector<Candidate> local;

    Impl(bool controllingIn, std::vector<Endpoint> servers, Ports portsIn, CancelToken cancelledIn,
         std::optional<RelaySettings> relay, std::string interfaceIn, std::chrono::milliseconds gather,
         uint32_t addressesIn)
        : controlling(controllingIn), stunServers(std::move(servers)), ports(portsIn), cancelled(cancelledIn),
          relaySettings(std::move(relay)), interfaceName(std::move(interfaceIn)), gatherTimeout(gather),
          extraAddresses(addressesIn) {
        crypto::random(std::span(reinterpret_cast<uint8_t*>(&tieBreaker), sizeof(tieBreaker)));
    }

    // The family of the candidates socket `s` serves: in relay mode the relayed
    // address's, which need not be that of the socket to the TURN server.
    bool ipv6Of(size_t s) const { return turn && s == 0 ? turn->relayed().ipv6() : sockets[s]->local().ipv6(); }

    // Datagrams from peers; through the relay they come from the peer it names.
    // `handle` returns false to leave the rest unread: once a path is chosen,
    // what follows (the peer's first handshake datagram) is the connection's.
    template <class Handle>
    void receive(Handle&& handle) {
        std::array<uint8_t, 2048> buffer;
        for (size_t s = 0; s < sockets.size(); ++s) {
            Endpoint from;
            while (const auto size = sockets[s]->receive(buffer, from)) {
                const auto bytes = std::span<const uint8_t>(buffer).first(*size);
                if (turn && s == 0) {
                    if (from == turn->server()) {
                        const auto relayed = turn->receive(*sockets[0], bytes);
                        if (relayed && !handle(s, relayed->first, relayed->second)) {
                            return;
                        }
                    }
                    continue;
                }
                if (!handle(s, from, bytes)) {
                    return;
                }
            }
        }
        if (turn) {
            turn->tick(*sockets[0], Clock::now());
        }
    }

    void sendTo(size_t s, const Endpoint& to, std::span<const uint8_t> datagram) {
        if (turn && s == 0) {
            turn->open(*sockets[0], to);
            turn->send(*sockets[0], to, datagram);
        } else {
            sockets[s]->send(to, datagram);
        }
    }

    // Relay mode: one socket to the TURN server (UDP, or a TCP or TLS
    // connection where only those pass); its allocation is the only candidate.
    void gatherRelayed() {
        const auto& settings = *relaySettings;
        if (settings.transport != RCS_TURN_UDP) {
            const bool tls = settings.transport == RCS_TURN_TLS;
            sockets.push_back(TurnTcpSocket::connect(settings.server, cancelled, settings.connectTimeout,
                                                     only ? only->index : 0,
                                                     tls ? &settings.tls : nullptr));
            log::info("network", std::string(tls ? "TLS" : "TCP") + " connection to the TURN server " +
                                     settings.server.toString());
        } else {
            sockets.push_back(bound(settings.server.ipv6() ? "::" : "0.0.0.0"));
        }
        waiter.watch(*sockets.back());
        turn = std::make_unique<TurnClient>(settings.server, settings.username, settings.password);
        const auto relayed = turn->allocate(*sockets[0], cancelled, settings.allocateTimeout);
        local.push_back({Type::relayed, relayed, priorityOf(Type::relayed, 65535)});
        log::info("network", "Local candidates: " + listOf(local));
    }

    // A UDP socket on the first port of `ports` that is free (on the interface
    // the host chose).
    std::unique_ptr<UdpSocket> bound(const char* any) const {
        const uint32_t last = ports.first ? std::min<uint32_t>(65535, ports.first + std::max(ports.count, 1u) - 1)
                                          : 0;
        for (uint32_t port = ports.first;; ++port) {
            try {
                auto socket = std::make_unique<UdpSocket>(Endpoint::parse(any, uint16_t(port)));
                if (only) {
                    socket->bindToInterface(only->index);
                }
                return socket;
            } catch (const Failure& failure) {
                if (port >= last) {
                    if (last == ports.first) {
                        throw;
                    }
                    fail(RCS_ERROR_NETWORK, "No UDP port of " + std::to_string(ports.first) + ".." +
                                                std::to_string(last) + " is free (the last: " + failure.what() + ")");
                }
            }
        }
    }

    void open(const char* any) {
        try {
            sockets.push_back(bound(any));
            waiter.watch(*sockets.back());
        } catch (const Failure& failure) {
            if (sockets.empty()) {
                throw;
            }
            log::debug("network", std::string("No IPv6 socket: ") + failure.what());
        }
    }

    void gather() {
        if (!interfaceName.empty()) {
            only = findInterface(interfaceName);
            log::info("network", "Using only interface " + only->name);
        }
        if (relaySettings) {
            gatherRelayed();
            return;
        }
        // The interface the system's default route leaves by comes first: a
        // peer that can reach this machine over several (cable and Wi-Fi on
        // one LAN) then sends where this end's unbound sockets send from,
        // not over Wi-Fi into a machine that answers by cable.
        std::vector<Endpoint> addresses;
        size_t preferred = 0; // The first ones, the default interface's.
        if (only) {
            addresses = only->addresses;
        } else {
            auto list = interfaces();
            const uint32_t index = defaultInterface(list);
            std::stable_partition(list.begin(), list.end(), [&](const Interface& i) { return i.index == index; });
            for (const auto& i : list) {
                for (const auto& address : i.addresses) {
                    if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
                        addresses.push_back(address);
                    }
                }
                if (i.index == index) {
                    preferred = addresses.size();
                }
            }
        }
        if (extraAddresses & RCS_ICE_ADDRESSES_LOOPBACK) {
            addresses.push_back(Endpoint::parse("127.0.0.1", 0));
            addresses.push_back(Endpoint::parse("::1", 0));
        }
        const auto has = [&](bool v6) {
            return std::any_of(addresses.begin(), addresses.end(), [&](const Endpoint& e) { return e.ipv6() == v6; });
        };
        if (has(false) || !only) {
            open("0.0.0.0");
        }
        if (has(true)) {
            open("::");
        }
        // Local preferences: the default interface's above the others, and
        // within each, alternating between the families, IPv6 first (RFC 8421).
        size_t rank[2][2] = {};
        for (size_t a = 0; a < addresses.size(); ++a) {
            const auto& address = addresses[a];
            const size_t group = a < preferred ? 0 : 1;
            for (const auto& socket : sockets) {
                if (socket->local().ipv6() == address.ipv6()) {
                    Endpoint candidate = address;
                    candidate.setPort(socket->local().port());
                    const size_t order =
                        group * 0x8000 + 2 * rank[group][address.ipv6()]++ + (address.ipv6() ? 0 : 1);
                    local.push_back({Type::host, candidate,
                                     priorityOf(Type::host, 65535 - uint32_t(std::min<size_t>(order, 65535)))});
                }
            }
        }

        struct Query {
            size_t socket;
            Endpoint server;
            stun::TransactionId id;
            Bytes request;
            int sent = 0;
            bool done = false;
        };
        std::vector<Query> queries;
        for (size_t s = 0; s < sockets.size(); ++s) {
            for (const auto& server : stunServers) {
                if (server.ipv6() == ipv6Of(s)) {
                    stun::Message request;
                    request.method = stun::binding;
                    request.klass = stun::request;
                    request.transaction = stun::newTransaction();
                    queries.push_back({s, server, request.transaction, stun::encode(request), 0, false});
                }
            }
        }
        const auto started = Clock::now();
        while (std::any_of(queries.begin(), queries.end(), [](const Query& q) { return !q.done; })) {
            throwIfCancelled(cancelled);
            const auto elapsed = Clock::now() - started;
            if (elapsed >= gatherTimeout) {
                break;
            }
            for (auto& query : queries) { // Sent at 0, 300 and 900 ms.
                const auto due = std::array{0ms, 300ms, 900ms};
                if (!query.done && query.sent < 3 && elapsed >= due[size_t(query.sent)]) {
                    sockets[query.socket]->send(query.server, query.request);
                    ++query.sent;
                }
            }
            receive([&](size_t s, const Endpoint& from, std::span<const uint8_t> bytes) {
                const auto message = stun::parse(bytes);
                if (!message || message->klass != stun::success) {
                    return true;
                }
                for (auto& query : queries) {
                    if (query.socket == s && query.id == message->transaction && from == query.server) {
                        query.done = true;
                        const auto mapped = message->address(stun::xorMappedAddress);
                        const bool known = mapped && std::any_of(local.begin(), local.end(), [&](const Candidate& c) {
                                               return c.address == *mapped;
                                           });
                        if (mapped && !known) {
                            const uint32_t preference = 65535 - uint32_t(std::min<size_t>(local.size(), 65535));
                            local.push_back(
                                {Type::serverReflexive, *mapped, priorityOf(Type::serverReflexive, preference)});
                        }
                    }
                }
                return true;
            });
            waiter.wait(Clock::now() + 20ms);
        }
        for (const auto& query : queries) {
            if (!query.done) {
                log::warning("network", "STUN server " + query.server.toString() + " did not answer");
            }
        }
        if (local.empty()) {
            fail(RCS_ERROR_NETWORK, "No network interface to connect from");
        }
        keepBest();
        log::info("network", "Local candidates: " + listOf(local));
    }

    // A signal carries maxCandidates: the server-reflexive ones (what crosses
    // NATs) first, then the host candidates by priority.
    void keepBest() {
        if (local.size() <= maxCandidates) {
            return;
        }
        std::stable_sort(local.begin(), local.end(), [](const Candidate& a, const Candidate& b) {
            const bool hostA = a.type == Type::host, hostB = b.type == Type::host;
            return hostA != hostB ? hostB : a.priority > b.priority;
        });
        const std::vector<Candidate> dropped(local.begin() + ptrdiff_t(maxCandidates), local.end());
        local.resize(maxCandidates);
        log::info("network",
                  "A signal carries " + std::to_string(maxCandidates) + " candidates; left out: " + listOf(dropped));
    }

    struct Pair {
        size_t socket = 0;
        Candidate remote;
        uint64_t priority = 0;
        enum State { waiting, inProgress, succeeded, failed } state = waiting;
        stun::TransactionId transaction{};
        Clock::time_point sent{};
        int attempts = 0;
        bool nominating = false;
    };

    // Why this end leaves out a peer's candidate at `address`; nothing when it
    // takes it. Loopback and link-local ones it takes where it has a
    // candidate of that scope itself (it is on that machine or link too).
    const char* leftOut(const Endpoint& address) const {
        const auto scope = scopeOf(address);
        const bool shared = std::any_of(local.begin(), local.end(),
                                        [&](const Candidate& c) { return scopeOf(c.address).first == scope.first; });
        return scope.first == Scope::global || (scope.first != Scope::none && shared) ? nullptr : scope.second;
    }

    // The priority of the local candidate socket `s` stands for: the relayed
    // one, or its host candidate.
    uint32_t socketPriority(size_t s) const {
        if (turn && s == 0) {
            return local.front().priority;
        }
        for (const auto& candidate : local) {
            if (candidate.type == Type::host && candidate.address.port() == sockets[s]->local().port() &&
                candidate.address.ipv6() == ipv6Of(s)) {
                return candidate.priority;
            }
        }
        return priorityOf(Type::host, ipv6Of(s) ? 65535 : 65534);
    }

    // Checks pairs with `initial`, the peer's signal. The host may deliver
    // more while checks run (`newer`): a repeat, a later run's, or one left
    // over from an earlier run. Only the peer's current run can check this
    // end with this run's credentials, so a signal those checks name is the
    // one to use; before any proof, the latest. A signal in use that the
    // peer's checks or answers proved stays.
    Path connect(const Description& initial, std::chrono::milliseconds timeout, const Signals& newer) {
        std::vector<Description> known{initial};
        Description remote;
        std::vector<Pair> pairs;
        std::vector<Candidate> remoteCandidates;
        std::string leftOutText; // The peer's candidates this end does not check, and why.
        Bytes remoteKey;
        std::string expectedUser;
        std::deque<size_t> triggered;
        std::optional<size_t> selected, nominated;
        bool proven = false;            // The peer's checks or answers confirmed `remote`.
        std::optional<std::string> run; // A peer ufrag its checks named that no signal known yet has.
        Clock::time_point deadline;
        const Bytes localKey = asBytes(password);

        const auto addPair = [&](size_t s, const Candidate& candidate) {
            Pair pair;
            pair.socket = s;
            pair.remote = candidate;
            const uint32_t mine = socketPriority(s);
            pair.priority =
                controlling ? pairPriority(mine, candidate.priority) : pairPriority(candidate.priority, mine);
            pairs.push_back(pair);
            return pairs.size() - 1;
        };
        // Starts over with another signal of the peer.
        const auto use = [&](const Description& description) {
            remote = description;
            remoteKey = asBytes(remote.password);
            expectedUser = ufrag + ":" + remote.ufrag;
            remoteCandidates.clear();
            leftOutText.clear();
            for (const auto& candidate : remote.candidates) {
                const auto bytes = candidate.address.addressBytes();
                const char* why = leftOut(candidate.address);
                if (!why && std::count_if(remoteCandidates.begin(), remoteCandidates.end(), [&](const Candidate& c) {
                                return c.address.addressBytes() == bytes;
                            }) >= ptrdiff_t(maxPerAddress)) {
                    why = "more of one address than a peer has";
                }
                if (why) {
                    leftOutText += (leftOutText.empty() ? "" : ", ") + candidate.address.toString() + " (" + why + ")";
                } else {
                    remoteCandidates.push_back(candidate);
                }
            }
            if (!leftOutText.empty()) {
                log::warning("network", "Left out the peer's candidates " + leftOutText);
            }
            pairs.clear();
            triggered.clear();
            nominated.reset();
            proven = false;
            for (size_t s = 0; s < sockets.size(); ++s) {
                for (const auto& candidate : remoteCandidates) {
                    if (candidate.address.ipv6() == ipv6Of(s)) {
                        addPair(s, candidate);
                    }
                }
            }
            std::stable_sort(pairs.begin(), pairs.end(),
                             [](const Pair& a, const Pair& b) { return a.priority > b.priority; });
            log::info("network", "Peer candidates: " + listOf(remoteCandidates) + "; checking " +
                                     std::to_string(pairs.size()) + " pairs");
            deadline = Clock::now() + timeout;
        };
        const auto knownRun = [&](const std::string& peerUfrag) {
            return std::find_if(known.begin(), known.end(), [&](const Description& d) { return d.ufrag == peerUfrag; });
        };
        use(initial);

        const auto sendCheck = [&](Pair& pair, bool retransmit) {
            stun::Message request;
            request.method = stun::binding;
            request.klass = stun::request;
            if (!retransmit) {
                pair.transaction = stun::newTransaction();
                pair.attempts = 0;
            }
            request.transaction = pair.transaction;
            request.addString(stun::username, remote.ufrag + ":" + ufrag);
            // As a peer-reflexive candidate of this socket's would rank (RFC 8445 7.1.1).
            request.addU32(stun::priority,
                           priorityOf(Type::peerReflexive, (socketPriority(pair.socket) >> 8) & 0xFFFF));
            request.addU64(controlling ? stun::iceControlling : stun::iceControlled, tieBreaker);
            if (pair.nominating) {
                request.add(stun::useCandidate, {});
            }
            sendTo(pair.socket, pair.remote.address, stun::encode(request, remoteKey));
            pair.state = Pair::inProgress;
            pair.sent = Clock::now();
            ++pair.attempts;
        };
        const auto findPair = [&](size_t s, const Endpoint& from) -> std::optional<size_t> {
            for (size_t i = 0; i < pairs.size(); ++i) {
                if (pairs[i].socket == s && pairs[i].remote.address == from) {
                    return i;
                }
            }
            return std::nullopt;
        };

        auto nextCheck = Clock::now();
        while (!selected) {
            throwIfCancelled(cancelled);
            if (newer) {
                while (auto description = newer()) {
                    if (knownRun(description->ufrag) != known.end()) {
                        continue; // A repeat.
                    }
                    known.push_back(*description);
                    if (!proven && (!run || *run == description->ufrag)) {
                        log::info("network", "Using the peer's newer signal");
                        use(known.back());
                    } else {
                        log::info("network", "Ignored a signal from another run of the peer");
                    }
                }
            }
            const auto now = Clock::now();
            if (now >= deadline) {
                const auto worked =
                    std::count_if(pairs.begin(), pairs.end(), [](const Pair& p) { return p.state == Pair::succeeded; });
                const char* advice =
                    run    ? ". The peer checks with a signal this end has not received; deliver its latest one"
                    : turn ? ". The peer's relayed address did not answer through the TURN server"
                           : ". Both ends may be behind NATs that block direct UDP; a relay can connect them";
                fail(RCS_ERROR_NETWORK, std::string(turn ? "No relayed path" : "No direct path") + " within " +
                                            std::to_string(timeout.count() / 1000) + " s: checked " +
                                            std::to_string(pairs.size()) + " address pairs, " + std::to_string(worked) +
                                            " answered. This end: " + listOf(local) +
                                            ". Peer: " + listOf(remoteCandidates) +
                                            (leftOutText.empty() ? "" : "; left out: " + leftOutText) + advice);
            }
            receive([&](size_t s, const Endpoint& from, std::span<const uint8_t> bytes) {
                if (!stun::looksLikeStun(bytes)) {
                    return true;
                }
                if (const auto check = stun::parseCheck(bytes, ufrag, localKey)) {
                    const std::string user = check->string(stun::username);
                    if (user != expectedUser) { // The peer's current run, with a signal not in use here.
                        const std::string peerUfrag = user.substr(ufrag.size() + 1);
                        const auto found = knownRun(peerUfrag);
                        if (found == known.end()) {
                            run = peerUfrag;
                            return true;
                        }
                        log::info("network", "The peer's checks name another of its signals; using that one");
                        use(*found);
                    }
                    proven = true;
                    auto index = findPair(s, from);
                    if (!index && leftOut(from)) {
                        return true; // Not answered: nothing of this end goes there.
                    }
                    sendTo(s, from, stun::answerCheck(*check, from, localKey));
                    if (!index) { // Seen only now: peer-reflexive.
                        const auto learned =
                            std::count_if(remoteCandidates.begin(), remoteCandidates.end(),
                                          [](const Candidate& c) { return c.type == Type::peerReflexive; });
                        if (size_t(learned) >= maxPeerReflexive) {
                            return true; // Answered, not checked back: enough addresses to try.
                        }
                        Candidate candidate{Type::peerReflexive, from,
                                            check->u32(stun::priority).value_or(priorityOf(Type::peerReflexive, 0))};
                        remoteCandidates.push_back(candidate);
                        index = addPair(s, candidate);
                        log::debug("network", "Peer-reflexive candidate " + from.toString());
                    }
                    Pair& pair = pairs[*index];
                    if (pair.state == Pair::waiting || pair.state == Pair::failed) {
                        pair.state = Pair::waiting;
                        triggered.push_back(*index);
                    }
                    if (!controlling && check->has(stun::useCandidate)) {
                        selected = index;
                    }
                    return !selected;
                }
                const auto message = stun::parse(bytes);
                if (!message || message->method != stun::binding || message->klass == stun::request ||
                    !message->has(stun::fingerprint)) {
                    return true; // Another run's check of this end, or a stranger's.
                }
                for (size_t i = 0; i < pairs.size(); ++i) {
                    Pair& pair = pairs[i];
                    if (pair.state != Pair::inProgress || pair.transaction != message->transaction) {
                        continue;
                    }
                    if (!stun::verifyIntegrity(bytes, remoteKey)) {
                        return true;
                    }
                    proven = true;
                    if (message->klass != stun::success || pair.socket != s || !(pair.remote.address == from)) {
                        pair.state = Pair::failed; // An error, or an asymmetric answer.
                        return true;
                    }
                    pair.state = Pair::succeeded;
                    if (controlling && pair.nominating) {
                        selected = i;
                    } else if (controlling && !nominated) {
                        nominated = i;
                        pair.nominating = true;
                        sendCheck(pair, false); // Nominate: the same check with USE-CANDIDATE.
                    }
                    return !selected;
                }
                return true;
            });
            if (selected) {
                break;
            }
            if (now >= nextCheck) {
                nextCheck = now + checkInterval;
                std::optional<size_t> next;
                while (!triggered.empty() && !next) {
                    if (pairs[triggered.front()].state == Pair::waiting) {
                        next = triggered.front();
                    }
                    triggered.pop_front();
                }
                for (size_t i = 0; i < pairs.size() && !next; ++i) {
                    if (pairs[i].state == Pair::waiting) {
                        next = i;
                    }
                }
                if (next) {
                    sendCheck(pairs[*next], false);
                }
            }
            for (size_t i = 0; i < pairs.size(); ++i) {
                Pair& pair = pairs[i];
                if (pair.state == Pair::inProgress && now - pair.sent >= firstRetransmit * (1 << (pair.attempts - 1))) {
                    if (pair.attempts >= maxAttempts) {
                        pair.state = Pair::failed;
                        if (nominated == i) {
                            nominated.reset();
                            pair.nominating = false;
                        }
                    } else {
                        sendCheck(pair, true);
                    }
                }
            }
            waiter.wait(std::min(nextCheck, Clock::now() + 20ms));
        }

        Pair& pair = pairs[*selected];
        Path path;
        path.remote = pair.remote.address;
        path.remoteType = pair.remote.type;
        path.localUfrag = ufrag;
        path.localPassword = password;
        path.socket = std::move(sockets[pair.socket]);
        if (turn && pair.socket == 0) {
            path.relay = std::move(turn);
            path.relayTransport = relaySettings->transport;
        }
        log::info("network", "Selected " + path.describe());
        return path;
    }
};

Agent::Agent(bool controlling, std::vector<Endpoint> stunServers, Ports ports, CancelToken cancelled,
             std::optional<RelaySettings> relay, std::string interfaceName, std::chrono::milliseconds gatherTimeout,
             uint32_t addresses)
    : impl_(std::make_unique<Impl>(controlling, std::move(stunServers), ports, cancelled, std::move(relay),
                                   std::move(interfaceName), gatherTimeout, addresses)) {}
Agent::~Agent() = default;

Description Agent::gather(rcs_role role) {
    impl_->gather();
    return {role, impl_->ufrag, impl_->password, {}, impl_->local};
}

Path Agent::connect(const Description& remote, std::chrono::milliseconds timeout, const Signals& newer) {
    if (remote.role == rcs_role(impl_->controlling ? RCS_ROLE_VIEW : RCS_ROLE_SHARE)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The peer's signal comes from the same role as this end");
    }
    return impl_->connect(remote, timeout, newer);
}

} // namespace rcsdk::transport::ice
