// The TURN client against a minimal TURN server on this machine.
//
//   rcsdk_test_turn --serve ADDRESS:PORT [--user NAME --password SECRET]
//       Serves that TURN server (UDP on ADDRESS:PORT, TCP on a port it
//       prints) until its input ends, for programs on this machine, e.g.
//       rcsdk_cli --transport relay (credentials default to host / s3cret).
#include "support/check.hpp"
#include "support/turn_server.hpp"
#include "protocol/udp_connection.hpp"
#include "transport/ice.hpp"
#include "transport/turn.hpp"
#include "transport/turn_tcp_socket.hpp"

#include <cstdio>
#include <future>
#include <iostream>

#include <thread>

using namespace rcsdk;
using namespace rcsdk::transport;

namespace {

std::atomic<bool> never{false};

struct End {
    std::unique_ptr<DatagramSocket> socketPointer;
    DatagramSocket& socket;
    TurnClient client;
    std::vector<std::pair<Endpoint, crypto::Bytes>> received;
    // Over UDP, or over a TCP connection to the server.
    End(const Endpoint& server, const std::string& user, const std::string& password, bool tcp = false)
        : socketPointer(tcp ? std::unique_ptr<DatagramSocket>(TurnTcpSocket::connect(server, never, 2s))
                            : std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0))),
          socket(*socketPointer), client(server, user, password) {}
    // Handles everything that arrived; keeps relayed data.
    void pump() {
        std::array<uint8_t, 2048> buffer;
        Endpoint from;
        while (const auto size = socket.receive(buffer, from)) {
            if (const auto data = client.receive(socket, std::span<const uint8_t>(buffer).first(*size))) {
                received.emplace_back(data->first, crypto::Bytes(data->second.begin(), data->second.end()));
            }
        }
        client.tick(socket, Clock::now());
    }
};

template <class Predicate>
bool pumpUntil(End& a, End& b, Predicate done) {
    const auto deadline = Clock::now() + 3s;
    while (!done()) {
        if (Clock::now() > deadline) {
            return false;
        }
        a.pump();
        b.pump();
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

// Pumps `end` for `duration`; what it throws, the caller sees.
void pumpFor(End& end, std::chrono::milliseconds duration) {
    for (const auto until = Clock::now() + duration; Clock::now() < until;) {
        end.pump();
        std::this_thread::sleep_for(2ms);
    }
}

// The failure pumping `end` runs into within 3 s.
Failure failureOf(End& end) {
    try {
        pumpFor(end, 3s);
    } catch (const Failure& failure) {
        return failure;
    }
    CHECK(false);
    return Failure(RCS_OK, "");
}

bool says(const Failure& failure, std::initializer_list<const char*> words) {
    const std::string text = failure.what();
    return std::all_of(words.begin(), words.end(), [&](const char* w) { return text.find(w) != std::string::npos; });
}

// A request that keeps the relay working and is refused ends the run with
// the server's reason: credentials it no longer takes as AUTHENTICATION
// (time-limited ones expire), anything else as NETWORK.
void refusals() {
    const Endpoint peer = Endpoint::parse("127.0.0.1", 5000);
    {
        test::TurnServer server("alice", "wonderland");
        server.lifetime = 2; // Refreshed after a second.
        End a(server.address(), "alice", "wonderland");
        a.client.allocate(a.socket, never, 5s);
        server.refuse(stun::refresh, 401);
        const auto failure = failureOf(a);
        CHECK(failure.result() == RCS_ERROR_AUTHENTICATION && says(failure, {"TURN", "Refresh", "401"}));
    }
    {
        test::TurnServer server("alice", "wonderland");
        End a(server.address(), "alice", "wonderland");
        a.client.allocate(a.socket, never, 5s);
        server.refuse(stun::createPermission, 403);
        a.client.open(a.socket, peer);
        const auto failure = failureOf(a);
        CHECK(failure.result() == RCS_ERROR_AUTHENTICATION && says(failure, {"TURN", "CreatePermission", "403"}));
    }
    {
        test::TurnServer server("alice", "wonderland");
        End a(server.address(), "alice", "wonderland");
        a.client.allocate(a.socket, never, 5s);
        server.refuse(stun::channelBind, 508);
        a.client.open(a.socket, peer);
        const auto failure = failureOf(a);
        CHECK(failure.result() == RCS_ERROR_NETWORK && says(failure, {"TURN", "ChannelBind", "508"}));
    }
    {
        // Refreshes follow the lifetime the server grants, counted from its
        // answer; a lost one goes again half a second later, not at expiry.
        test::TurnServer server("alice", "wonderland");
        server.lifetime = 2;
        End a(server.address(), "alice", "wonderland");
        a.client.allocate(a.socket, never, 5s);
        server.lose(stun::refresh, 1);
        pumpFor(a, 1700ms);
        CHECK(server.refreshes == 2);
        pumpFor(a, 1000ms);
        CHECK(server.refreshes == 3);
    }
    {
        // A server that grants no lifetime is asked again a second on, not
        // at every tick.
        test::TurnServer server("alice", "wonderland");
        server.lifetime = 0;
        End a(server.address(), "alice", "wonderland");
        a.client.allocate(a.socket, never, 5s);
        pumpFor(a, 1500ms);
        CHECK(server.refreshes == 1);
    }
    {
        // A stale nonce renews a request once; another for the renewed one
        // ends the relay, which would otherwise keep asking (a server, or
        // someone on the path who sees the requests, answering 438 to each).
        test::TurnServer server("alice", "wonderland");
        End a(server.address(), "alice", "wonderland");
        a.client.allocate(a.socket, never, 5s);
        server.refuse(stun::createPermission, 438);
        a.client.open(a.socket, peer);
        const auto failure = failureOf(a);
        CHECK(failure.result() == RCS_ERROR_NETWORK && says(failure, {"TURN", "CreatePermission", "438"}));
    }
}

// A server that grants an allocation without asking for the credentials
// this end has: anyone on the path could have answered so, so it is not
// taken, and the failure says why. Without a username it is taken
// (withoutAuthentication).
void unaskedCredentials() {
    test::TurnServer server("", "");
    UdpSocket socket(Endpoint::parse("127.0.0.1", 0));
    try {
        TurnClient(server.address(), "alice", "wonderland").allocate(socket, never, 1s);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_AUTHENTICATION &&
              says(failure, {"TURN", "without asking for the credentials"}));
    }
}

// An allocation on the local port already: one this end released that the
// server is still deleting (coturn takes about a second), or one an earlier
// run left behind. Allocating from the port releases it and gets a new one; a
// server that keeps it is a failure that says so.
void leftAllocations() {
    for (const bool coturn : {false, true}) {
        test::TurnServer server("alice", "wonderland");
        server.releaseDelayMs = 800;
        server.coturn = coturn;
        UdpSocket socket(Endpoint::parse("127.0.0.1", 0));
        TurnClient first(server.address(), "alice", "wonderland");
        first.allocate(socket, never, 5s);
        first.release(socket);
        TurnClient again(server.address(), "alice", "wonderland");
        const auto started = Clock::now();
        again.allocate(socket, never, 5s);
        CHECK(server.allocations == 2 && Clock::now() - started >= 700ms);
        TurnClient next(server.address(), "alice", "wonderland"); // again's is left behind.
        next.allocate(socket, never, 5s);
        CHECK(server.allocations == 3);
    }
    // A server that keeps it: a failure that says so, AUTHENTICATION when
    // releasing it needs other credentials.
    for (const int refusal : {403, 401}) {
        test::TurnServer server("alice", "wonderland");
        server.coturn = true;
        UdpSocket socket(Endpoint::parse("127.0.0.1", 0));
        TurnClient(server.address(), "alice", "wonderland").allocate(socket, never, 5s);
        server.refuse(stun::refresh, refusal);
        try {
            TurnClient(server.address(), "alice", "wonderland").allocate(socket, never, 5s);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == (refusal == 401 ? RCS_ERROR_AUTHENTICATION : RCS_ERROR_NETWORK));
            CHECK(says(failure, {"TURN", "437", "local port", std::to_string(refusal).c_str()}));
        }
    }
}

// A TURN server without authentication: no integrity either way, and the
// channel still binds.
void withoutAuthentication() {
    test::TurnServer server("", "");
    End a(server.address(), "", ""), b(server.address(), "", "");
    const auto relayA = a.client.allocate(a.socket, never, 5s), relayB = b.client.allocate(b.socket, never, 5s);
    a.client.open(a.socket, relayB);
    b.client.open(b.socket, relayA);
    CHECK(a.client.overhead(relayB) > 4); // A Send indication's framing until then.
    CHECK(pumpUntil(a, b, [&] { return a.client.overhead(relayB) == 4 && b.client.overhead(relayA) == 4; }));
    a.client.send(a.socket, relayB, crypto::Bytes{1, 2, 3});
    CHECK(pumpUntil(a, b, [&] { return !b.received.empty(); }));
    CHECK(b.received[0].first == relayA && b.received[0].second == crypto::Bytes({1, 2, 3}));
}

// A stale nonce renews requests whose XOR addresses depend on the transaction
// (IPv6): the permission and the channel are for the address asked for.
void staleNonceIpv6() {
    test::TurnServer server("alice", "wonderland");
    End a(server.address(), "alice", "wonderland");
    a.client.allocate(a.socket, never, 5s);
    const auto peer = Endpoint::parse("2001:db8::7", 7000);
    server.staleNonce();
    a.client.open(a.socket, peer);
    CHECK(pumpUntil(a, a, [&] { return server.permits(peer) && a.client.overhead(peer) == 4; }));
}

// A TCP connection to a server that stops reading: datagrams are dropped
// (false), as by a full UDP socket, without blocking the sender.
void stalledTcp() {
    auto listener = transport::openTcpSocket(AF_INET);
    const auto any = Endpoint::parse("127.0.0.1", 0);
    CHECK(::bind(listener.get(), any.get(), any.size()) == 0 && ::listen(listener.get(), 1) == 0);
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length);
    const auto server = Endpoint::fromSockaddr(reinterpret_cast<const sockaddr*>(&address), size_t(length));
    const auto socket = TurnTcpSocket::connect(server, never, 2s);
    std::this_thread::sleep_for(20ms);
    const transport::Socket accepted(::accept(listener.get(), nullptr, nullptr)); // Never read.
    const crypto::Bytes datagram(1200, 0x42);
    const auto started = Clock::now();
    bool dropped = false;
    for (int i = 0; i < 40'000 && !dropped; ++i) {
        dropped = !socket->send(server, datagram);
    }
    CHECK(dropped && Clock::now() - started < 2s);
}

} // namespace

// Two ICE agents that may only use the relay, and the UDP transport over the
// path they choose: everything passes the TURN server, end-to-end encrypted.
// `turnTransport`: how the ends reach the server.
void relayedConnection(test::TurnServer& server, rcs_turn_transport turnTransport) {
    const ice::RelaySettings settings{server.address(turnTransport), "alice", "wonderland", turnTransport};
    ice::Agent sharer(false, {}, {}, never, settings), viewer(true, {}, {}, never, settings);
    const auto offer = sharer.gather(RCS_ROLE_SHARE), answer = viewer.gather(RCS_ROLE_VIEW);
    CHECK(offer.candidates.size() == 1 && offer.candidates[0].type == ice::Type::relayed);
    auto controlled = std::async(std::launch::async, [&] { return sharer.connect(answer, 5s); });
    auto viewerPath = viewer.connect(offer, 5s);
    auto sharerPath = controlled.get();
    CHECK(viewerPath.relay && sharerPath.relay && viewerPath.remote == offer.candidates[0].address);
    CHECK(viewerPath.describe().find("relay") != std::string::npos);

    const std::array<uint8_t, 32> key{9};
    const auto credentials = transport::Credentials::fromKey(key);
    using protocol::UdpConnection;
    auto serverSide = std::async(std::launch::async, [&] {
        auto link = protocol::relayLink(std::move(sharerPath.relay), sharerPath.remote, sharerPath.localUfrag,
                                        sharerPath.localPassword, sharerPath.remoteType, sharerPath.relayTransport);
        return UdpConnection::establish(UdpConnection::Side::server, std::move(sharerPath.socket), std::move(link),
                                        credentials, {}, never);
    });
    auto link = protocol::relayLink(std::move(viewerPath.relay), viewerPath.remote, viewerPath.localUfrag,
                                    viewerPath.localPassword, viewerPath.remoteType, viewerPath.relayTransport);
    auto client = UdpConnection::establish(UdpConnection::Side::client, std::move(viewerPath.socket), std::move(link),
                                           credentials, {}, never);
    auto host = serverSide.get();
    const uint64_t before = server.relayed;
    EncodedPacket packet;
    packet.codec = RCS_CODEC_H264;
    packet.width = 1280;
    packet.height = 720;
    packet.keyframe = true;
    packet.sequence = 1;
    packet.config = {0, 0, 0, 2, 0x67, 0x42};
    packet.data = {0, 0, 0x9c, 0x40, 0x65};
    packet.data.resize(4 + 0x9c40, 0x11); // A 40 KB picture: dozens of relayed datagrams.
    host->send(packet);
    for (;;) {
        auto message = client->receive();
        if (const auto* received = std::get_if<EncodedPacket>(&message)) {
            CHECK(received->data == packet.data);
            break;
        }
    }
    host->send(protocol::ControlGrant{true, 4});
    CHECK(std::get<protocol::ControlGrant>(client->receive()).epoch == 4);
    CHECK(server.relayed - before >= 35);
}

int serve(const std::string& address, const std::string& user, const std::string& password) {
    const auto at = Endpoint::parse(address.substr(0, address.rfind(':')),
                                    uint16_t(std::stoul(address.substr(address.rfind(':') + 1))));
    test::TurnServer server(user, password, at.address(), at.port());
    std::printf("TURN on UDP %s and TCP %s, user %s, until this input ends\n", server.addressText().c_str(),
                server.tcpAddress().toString().c_str(), user.c_str());
    std::fflush(stdout);
    for (std::string line; std::getline(std::cin, line);) {
    }
    std::printf("relayed %llu datagrams, %d allocations\n", (unsigned long long)server.relayed.load(),
                server.allocations.load());
    return 0;
}

// RFC 8016: the allocation moves to another socket of the client's (another
// network) with its ticket: the relayed address, permissions and channels
// stay, and the peer still reaches it there. A server without mobility
// grants no ticket; a ticket from the same 5-tuple is refused.
void mobility() {
    test::TurnServer server("alice", "wonderland");
    End a(server.address(), "alice", "wonderland"), peer(server.address(), "alice", "wonderland");
    const auto relayed = a.client.allocate(a.socket, never, 5s);
    CHECK(a.client.mobile());
    peer.client.allocate(peer.socket, never, 5s);
    a.client.open(a.socket, peer.client.relayed());
    peer.client.open(peer.socket, relayed);
    CHECK(pumpUntil(a, peer, [&] { return server.permits(peer.client.relayed()) && server.permits(relayed); }));
    UdpSocket moved(Endpoint::parse("127.0.0.1", 0));
    a.client.move(moved);
    std::array<uint8_t, 2048> buffer;
    for (const auto deadline = Clock::now() + 3s; server.moves == 0 && Clock::now() < deadline;) {
        Endpoint from;
        while (const auto size = moved.receive(buffer, from)) {
            a.client.receive(moved, std::span<const uint8_t>(buffer).first(*size));
        }
        std::this_thread::sleep_for(2ms);
    }
    CHECK(server.moves == 1 && a.client.relayed() == relayed && server.allocations == 2);
    // The peer reaches the same relayed address, now through the new socket.
    const crypto::Bytes hello{1, 2, 3};
    peer.client.send(peer.socket, relayed, hello);
    bool arrived = false;
    for (const auto deadline = Clock::now() + 3s; !arrived && Clock::now() < deadline;) {
        Endpoint from;
        while (const auto size = moved.receive(buffer, from)) {
            if (const auto data = a.client.receive(moved, std::span<const uint8_t>(buffer).first(*size))) {
                arrived = data->first == peer.client.relayed() &&
                          crypto::Bytes(data->second.begin(), data->second.end()) == hello;
            }
        }
        peer.pump();
        std::this_thread::sleep_for(2ms);
    }
    CHECK(arrived);
    // From the same 5-tuple the ticket is refused (400).
    a.client.move(moved);
    bool refused = false;
    for (const auto deadline = Clock::now() + 3s; !refused && Clock::now() < deadline;) {
        Endpoint from;
        try {
            while (const auto size = moved.receive(buffer, from)) {
                a.client.receive(moved, std::span<const uint8_t>(buffer).first(*size));
            }
        } catch (const Failure& failure) {
            refused = says(failure, {"Refresh", "400"});
        }
        std::this_thread::sleep_for(2ms);
    }
    CHECK(refused);
    // Without mobility: no ticket.
    server.mobility = false;
    End plain(server.address(), "alice", "wonderland");
    plain.client.allocate(plain.socket, never, 5s);
    CHECK(!plain.client.mobile());
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--serve") {
        std::string user = "host", password = "s3cret";
        for (int i = 3; i + 1 < argc; i += 2) {
            const std::string option = argv[i];
            if (option == "--user") {
                user = argv[i + 1];
            } else if (option == "--password") {
                password = argv[i + 1];
            }
        }
        return serve(argv[2], user, password);
    }
    return runTest("turn", [] {
        mobility();
        refusals();
        leftAllocations();
        withoutAuthentication();
        unaskedCredentials();
        staleNonceIpv6();
        stalledTcp();
        {
            // A TURN server reached over IPv6 that relays IPv4 (coturn does
            // so without REQUESTED-ADDRESS-FAMILY): the candidates are the
            // relayed addresses' family, not the server's.
            test::TurnServer server("alice", "wonderland", "::1", 0, {}, "127.0.0.1");
            relayedConnection(server, RCS_TURN_UDP);
            relayedConnection(server, RCS_TURN_TCP);
        }
        test::TurnServer server("alice", "wonderland");
        End a(server.address(), "alice", "wonderland"), b(server.address(), "alice", "wonderland");
        const auto relayA = a.client.allocate(a.socket, never, 5s);
        const auto relayB = b.client.allocate(b.socket, never, 5s);
        CHECK(server.allocations == 2 && relayA != relayB);

        // Before the channel is bound: Send and Data indications.
        a.client.open(a.socket, relayB);
        b.client.open(b.socket, relayA);
        const crypto::Bytes hello{1, 2, 3, 4, 5};
        a.client.send(a.socket, relayB, hello);
        CHECK(pumpUntil(a, b, [&] { return !b.received.empty(); }));
        CHECK(b.received[0].first == relayA && b.received[0].second == hello);

        // Once bound (after the responses), channel data both ways.
        CHECK(pumpUntil(a, b, [&] { return server.relayed >= 1; }));
        std::this_thread::sleep_for(50ms);
        a.pump();
        b.pump();
        crypto::Bytes big(1200, 0x42);
        for (int i = 0; i < 20; ++i) {
            b.client.send(b.socket, relayA, big);
        }
        CHECK(pumpUntil(a, b, [&] { return a.received.size() >= 20; }));
        CHECK(a.received.back().first == relayB && a.received.back().second == big);

        // A stale nonce is renewed without losing the request.
        server.staleNonce();
        const auto c = std::make_unique<End>(server.address(), "alice", "wonderland");
        c->client.allocate(c->socket, never, 5s);
        CHECK(server.allocations == 3);

        // Wrong credentials: an authentication failure that says so.
        End wrong(server.address(), "alice", "nope");
        try {
            wrong.client.allocate(wrong.socket, never, 5s);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_AUTHENTICATION);
            CHECK(std::string(failure.what()).find("password") != std::string::npos);
        }
        relayedConnection(server, RCS_TURN_UDP);

        // Over TCP: an allocation, then data between a TCP end and a UDP end
        // (channel data padded on the wire), and the whole encrypted
        // connection through the relay.
        End t(server.tcpAddress(), "alice", "wonderland", true);
        const auto relayT = t.client.allocate(t.socket, never, 5s);
        CHECK(server.tcpClients == 1 && server.allocations == 6 && relayT != relayA); // a, b, c, two agents, t.
        t.client.open(t.socket, relayA);
        a.client.open(a.socket, relayT);
        a.received.clear();
        t.client.send(t.socket, relayA, hello);
        CHECK(pumpUntil(a, t, [&] { return !a.received.empty(); }));
        CHECK(a.received[0].first == relayT && a.received[0].second == hello);
        CHECK(pumpUntil(a, t, [&] { return server.relayed >= 60; }));
        std::this_thread::sleep_for(50ms);
        a.pump();
        t.pump();
        for (int i = 0; i < 20; ++i) {
            a.client.send(a.socket, relayT, big); // 1200 bytes: padded to 1204 over TCP.
        }
        CHECK(pumpUntil(a, t, [&] { return t.received.size() >= 20; }));
        CHECK(t.received.back().first == relayA && t.received.back().second == big);
        t.client.send(t.socket, relayA, crypto::Bytes{7}); // 1 byte: padded to 8.
        CHECK(pumpUntil(a, t, [&] { return a.received.size() >= 2; }));
        CHECK(a.received.back().second == crypto::Bytes{7});
        relayedConnection(server, RCS_TURN_TCP);
        CHECK(server.tcpClients == 3);

        // Nothing listening: a network failure within 5 s (TCP: at once, refused).
        End silent(Endpoint::parse("127.0.0.1", 9), "a", "b");
        CHECK(throws<Failure>([&] { silent.client.allocate(silent.socket, never, 5s); }));
        CHECK(throws<Failure>([&] { TurnTcpSocket::connect(Endpoint::parse("127.0.0.1", 9), never, 2s); }));

        // Relay mode names what it tried through the relay when no path works.
        const ice::RelaySettings settings{server.address(), "alice", "wonderland", RCS_TURN_UDP};
        ice::Agent viewer(true, {}, {}, never, settings);
        viewer.gather(RCS_ROLE_VIEW);
        ice::Description unreachable{RCS_ROLE_SHARE, "0a1b2c3d", "00112233445566778899aabbccddeeff", {}};
        unreachable.candidates.push_back({ice::Type::relayed, Endpoint::parse("127.0.0.1", 9), 255});
        try {
            viewer.connect(unreachable, 1500ms);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_NETWORK && says(failure, {"No relayed path", "TURN"}));
        }
    });
}
