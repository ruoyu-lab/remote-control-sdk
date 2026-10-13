// ICE (RFC 8445, a subset): finds a UDP path between two ends behind NATs.
//
// Each end gathers candidates — its interface addresses (host) and the
// addresses STUN servers see it from (server-reflexive) — and hands them, with
// random credentials, to the host as a signal. Once it has the peer's signal,
// it sends authenticated STUN Binding checks on every candidate pair, answers
// the peer's checks, and learns addresses only the peer's checks reveal (peer-
// reflexive: how symmetric NATs, such as many mobile networks, become
// reachable). The controlling end (the viewer) nominates the first pair that
// works both ways; the controlled end (the sharer) follows that nomination, so
// both choose the same path. When no pair works, connect() fails and says
// what it tried; it never turns to a relay by itself. In relay mode
// (RelaySettings) the only candidate is an address on a TURN server.
//
// What the peer's signal names is probed, so an agent leaves out (and logs)
// the candidates no peer is at: unspecified, multicast, broadcast and
// reserved addresses, cloud metadata services; loopback and link-local ones
// unless it has a candidate of that scope itself; more than four of one IP
// address. Its own loopback addresses it offers only when asked
// (RCS_ICE_ADDRESSES_LOOPBACK). It does not answer or learn the peer's checks
// from addresses it leaves out.
#pragma once

#include "transport/socket.hpp"
#include "transport/tls.hpp"
#include "transport/turn.hpp"
#include "util/api.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rcsdk::transport::ice {

enum class Type : uint8_t { host = 0, serverReflexive = 1, peerReflexive = 2, relayed = 3 };
const char* typeName(Type type) noexcept;

struct Candidate {
    Type type = Type::host;
    Endpoint address;
    uint32_t priority = 0;
};

// What one end tells the other through the host's own channel. A viewer
// signals first; a sharer's signal answers one viewer signal, which it names.
struct Description {
    rcs_role role = 0;
    std::string ufrag, password; // Random, [0-9a-f].
    std::string answers;         // A sharer's: the ufrag of the viewer's signal it answers.
    std::vector<Candidate> candidates;
};

// "rcs1." followed by base64url: plain ASCII any channel can carry.
std::string encode(const Description& description);
// Surrounding whitespace is ignored. Throws Failure(INVALID_ARGUMENT) for
// anything else (another kind of signal too), also for a sharer's signal that
// answers nothing and a viewer's that answers something.
Description decode(const std::string& signal);

// What a signal is: a connection's first exchange (encode), a restart of a
// running connection's path (rcs_paths_config), or the answer to a restart
// that no connection of its tag is there. Throws Failure(INVALID_ARGUMENT)
// for what is no signal of this version.
enum class SignalKind : uint8_t { connect = 0, restart = 1, unknown = 2 };
SignalKind signalKind(const std::string& signal);
// A restart: a new exchange of candidates for the connection `tag` names
// (from its keys: no one else can tell which connection it is), the
// sender's `generation`th, sealed with `key` (the connection's too): the host
// carries it, and can neither change nor make one. The description's
// `answers`: the ufrag of the restart it answers; empty for one that starts
// (from either role). `standby`: the path it finds is kept ready beside the
// one in use (rcs_paths_config.standby), not taken.
using SignalTag = std::array<uint8_t, 8>;
struct Restart {
    SignalTag tag{};
    uint32_t generation = 0;
    bool standby = false;
    Description description;
};
std::string encodeRestart(const Restart& restart, std::span<const uint8_t> key);
// The tag of a restart or unknown signal; throws INVALID_ARGUMENT for another.
SignalTag signalTag(const std::string& signal);
// Throws Failure: INVALID_ARGUMENT for a malformed one, AUTHENTICATION when
// its seal does not verify with `key` (changed on the way, or made by another).
Restart decodeRestart(const std::string& signal, std::span<const uint8_t> key);
// "No connection of `tag` here": not sealed (this end has no key for it).
std::string encodeUnknown(const SignalTag& tag);

// The selected path.
struct Path {
    std::unique_ptr<DatagramSocket> socket; // UDP, or the TCP (or TLS) stream to the TURN server.
    std::unique_ptr<TurnClient> relay;      // Through a TURN server, or direct when null.
    rcs_turn_transport relayTransport = RCS_TURN_UDP;
    Endpoint remote;
    Type remoteType = Type::host;
    std::string localUfrag, localPassword; // To keep answering the peer's checks.
    std::string describe() const;
};

// "host:port" (a name, IPv4, or "[v6]:port"; without a port, `defaultPort`)
// split in two; nothing for what is neither.
struct HostPort {
    std::string host; // Without brackets.
    uint16_t port = 0;
};
std::optional<HostPort> splitHostPort(const std::string& server, uint16_t defaultPort);
// The endpoints such a server resolves to; empty when it does not resolve (the
// system's reason is logged).
std::vector<Endpoint> resolve(const std::string& server, uint16_t defaultPort = 3478);

struct RelaySettings {
    Endpoint server;
    std::string username, password;
    rcs_turn_transport transport = RCS_TURN_UDP; // How to reach the server.
    TlsSettings tls;                             // RCS_TURN_TLS: how it proves itself.
    // rcs_network_tuning_config: reaching the server over TCP or TLS, and its allocation.
    std::chrono::milliseconds connectTimeout{5000};
    std::chrono::milliseconds allocateTimeout{5000};
};

// The local UDP ports an agent's sockets may take: the first free one of
// `count` from `first` up; first 0: any.
struct Ports {
    uint16_t first = 0;
    uint32_t count = 1;
};

class Agent {
public:
    // `controlling`: the viewer. `ports`: where its UDP sockets bind. With
    // `relay`, the only candidate is an address allocated on that TURN server.
    // `interfaceName`: use only that network interface (its name or index); empty: all.
    // `gatherTimeout`: how long gather() waits for the STUN servers.
    // `addresses`: rcs_ice_addresses, kinds of address offered beyond the interfaces'.
    Agent(bool controlling, std::vector<Endpoint> stunServers, Ports ports, CancelToken cancelled,
          std::optional<RelaySettings> relay = std::nullopt, std::string interfaceName = {},
          std::chrono::milliseconds gatherTimeout = std::chrono::milliseconds(1500), uint32_t addresses = 0);
    ~Agent();
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // This end's candidates, within about the gather timeout (STUN servers
    // that do not answer by then are skipped), at most the 32 a signal
    // carries. Throws NETWORK without any usable interface.
    Description gather(rcs_role role);
    // Checks pairs until one is selected, or throws NETWORK after `timeout`
    // with what was tried. `newer` returns further signals of the peer the
    // host delivered meanwhile, without waiting: the one the peer's checks
    // name is used, else the latest, and `timeout` starts again with it.
    using Signals = std::function<std::optional<Description>()>;
    Path connect(const Description& remote, std::chrono::milliseconds timeout, const Signals& newer = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rcsdk::transport::ice
