// The host's own channel seen as a datagram socket (RCS_TRANSPORT_HOST): what
// the host hands in comes out whole, in order, however a stream is cut; what
// the connection sends reaches the host framed; a closing channel breaks the
// connection over it; waiters wake when something arrives. A sharer's channel
// with a route per viewer (RouteDemux): handshakes of every route on one
// socket, each route's connection on its own, closed and restarted alone, and
// bounded against floods of made-up routes.
#include "protocol/route_demux.hpp"
#include "protocol/udp_connection.hpp"
#include "support/check.hpp"
#include "transport/host_channel.hpp"

#include <random>
#include <thread>
#include <vector>

using namespace rcsdk;
using namespace rcsdk::transport;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes datagram(size_t size, uint8_t seed) {
    Bytes bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = uint8_t(seed + i * 7);
    }
    return bytes;
}

// What a socket has waiting, in order.
std::vector<Bytes> drain(HostSocket& socket) {
    std::vector<Bytes> out;
    Bytes buffer(65536);
    Endpoint from;
    while (const auto size = socket.receive(buffer, from)) {
        CHECK(from == HostSocket::placeholder());
        out.emplace_back(buffer.begin(), buffer.begin() + ptrdiff_t(*size));
    }
    return out;
}

rcs_result resultOf(const std::function<void()>& action) {
    try {
        action();
    } catch (const Failure& failure) {
        return failure.result();
    }
    return RCS_OK;
}

void datagrams() {
    std::vector<Bytes> sent;
    bool room = true;
    auto channel = std::make_shared<HostChannel>(HostKind::datagram, [&](std::span<const uint8_t> bytes) {
        sent.emplace_back(bytes.begin(), bytes.end());
        return room;
    });
    HostSocket socket(channel);
    CHECK(!socket.buffered());
    // Out: each datagram is one message, as it is; no room counts as refused.
    CHECK(socket.send(HostSocket::placeholder(), datagram(1200, 1)));
    room = false;
    CHECK(!socket.send(HostSocket::placeholder(), datagram(5, 2)));
    CHECK(sent.size() == 2 && sent[0] == datagram(1200, 1) && sent[1] == datagram(5, 2) && channel->refused() == 1);
    // In: whole and in order, the largest a peer may send included.
    channel->receive(datagram(1, 3));
    channel->receive(datagram(65535, 4));
    CHECK(socket.buffered());
    const auto got = drain(socket);
    CHECK(got.size() == 2 && got[0] == datagram(1, 3) && got[1] == datagram(65535, 4));
    CHECK(!socket.buffered());
    // Sizes a channel cannot have carried.
    CHECK(resultOf([&] { channel->receive({}); }) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(resultOf([&] { channel->receive(datagram(65536, 5)); }) == RCS_ERROR_INVALID_ARGUMENT);
    // A datagram larger than the reader's buffer is dropped, as a socket drops one.
    channel->receive(datagram(3000, 6));
    channel->receive(datagram(10, 7));
    Bytes narrow(2048);
    Endpoint from;
    const auto size = socket.receive(narrow, from);
    CHECK(size && *size == 10);
    // A session that falls behind: the datagram is dropped, the next one fits again.
    const Bytes big = datagram(65535, 8);
    size_t queued = 0;
    while (queued + big.size() <= HostChannel::maxQueued) {
        channel->receive(big);
        queued += big.size();
    }
    CHECK(resultOf([&] { channel->receive(big); }) == RCS_ERROR_QUEUE_FULL);
    CHECK(drain(socket).size() == queued / big.size());
    channel->receive(datagram(10, 9));
    CHECK(drain(socket).size() == 1);
}

void streams() {
    Bytes wire; // What the host's stream carries.
    auto channel = std::make_shared<HostChannel>(HostKind::stream, [&](std::span<const uint8_t> bytes) {
        wire.insert(wire.end(), bytes.begin(), bytes.end());
        return true;
    });
    HostSocket socket(channel);
    const std::vector<Bytes> datagrams = {datagram(1, 1), datagram(1200, 2), datagram(65535, 3), datagram(300, 4)};
    for (const auto& d : datagrams) {
        CHECK(socket.send(HostSocket::placeholder(), d));
    }
    CHECK(wire.size() == 1 + 1200 + 65535 + 300 + 4 * 2 && wire[0] == 0 && wire[1] == 1);
    // However the stream is cut, the same datagrams come out: whole pieces,
    // single bytes, random pieces.
    std::mt19937 random(7);
    for (const int cut : {0, 1, 2}) {
        size_t at = 0;
        while (at < wire.size()) {
            const size_t piece =
                cut == 0 ? wire.size() : cut == 1 ? 1 : std::uniform_int_distribution<size_t>(1, 3000)(random);
            const size_t n = std::min(piece, wire.size() - at);
            channel->receive(std::span(wire).subspan(at, n));
            at += n;
        }
        CHECK(drain(socket) == datagrams);
    }
    // An empty frame carries nothing.
    channel->receive(Bytes{0, 0, 0, 2, 9, 9});
    const auto got = drain(socket);
    CHECK(got.size() == 1 && got[0] == Bytes({9, 9}));
    // A closed stream starts over: half a frame from before is gone.
    channel->receive(Bytes{0, 5, 1, 2});
    channel->closed();
    channel->receive(Bytes{0, 1, 7});
    Bytes buffer(2048);
    Endpoint from;
    CHECK(resultOf([&] { socket.receive(buffer, from); }) == RCS_ERROR_NETWORK); // The connection ended with it.
    const auto after = drain(socket);
    CHECK(after.size() == 1 && after[0] == Bytes({7}));
}

// The connection over the channel ends with it: it reads what came before
// (the other end's goodbye may be among it, as when a host hands in the last
// datagrams and the close together), then its next read fails once.
void closing() {
    auto channel = std::make_shared<HostChannel>(HostKind::datagram, [](std::span<const uint8_t>) { return true; });
    {
        HostSocket connected(channel);
        channel->receive(datagram(10, 2));
        channel->receive(datagram(20, 3));
        channel->closed();
        CHECK(connected.buffered());
        Bytes buffer(2048);
        Endpoint from;
        for (const auto& before : {datagram(10, 2), datagram(20, 3)}) {
            const auto size = connected.receive(buffer, from);
            CHECK(size && Bytes(buffer.begin(), buffer.begin() + ptrdiff_t(*size)) == before);
        }
        CHECK(resultOf([&] { connected.receive(buffer, from); }) == RCS_ERROR_NETWORK);
        CHECK(!connected.buffered() && drain(connected).empty()); // Once.
    }
    // A later connection's socket does not inherit a close that came before it.
    channel->closed();
    HostSocket next(channel);
    channel->receive(datagram(10, 3));
    CHECK(drain(next).size() == 1);
    // A newer socket takes over: the older one hears nothing more.
    HostSocket newer(channel);
    channel->receive(datagram(10, 4));
    CHECK(drain(newer).size() == 1);
}

// The connection's thread sleeps in a Waiter: what the host hands in from
// another thread wakes it, and what arrived before it waited does not wait.
void waking() {
    auto channel = std::make_shared<HostChannel>(HostKind::datagram, [](std::span<const uint8_t>) { return true; });
    auto socket = std::make_unique<HostSocket>(channel);
    Waiter waiter;
    waiter.watch(*socket);
    for (int round = 0; round < 20; ++round) {
        std::thread host([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(round % 3));
            channel->receive(datagram(100, uint8_t(round)));
        });
        const auto started = Clock::now();
        while (!socket->buffered() && Clock::now() - started < 2s) {
            waiter.wait(Clock::now() + 2s);
        }
        host.join();
        CHECK(Clock::now() - started < 1s);
        CHECK(drain(*socket).size() == 1);
    }
    channel->receive(datagram(100, 1)); // Before the wait: no sleep.
    const auto started = Clock::now();
    waiter.wait(Clock::now() + 2s);
    CHECK(Clock::now() - started < 500ms);
    CHECK(drain(*socket).size() == 1);
}

// ---- A sharer's routes ---------------------------------------------------------------

using protocol::RouteDemux;

// A datagram as the UDP connection starts one: a hello, or data of `id`.
Bytes hello(uint8_t seed) {
    Bytes bytes = datagram(100, seed);
    bytes[0] = protocol::clientHelloKind;
    return bytes;
}
Bytes data(uint32_t id, uint8_t seed, size_t size = 100) {
    Bytes bytes = datagram(size, seed);
    bytes[0] = protocol::dataKind;
    for (size_t i = 0; i < 4; ++i) {
        bytes[1 + i] = uint8_t(id >> (24 - 8 * i));
    }
    return bytes;
}

// What a socket has waiting, in order, with the route each came from.
std::vector<std::pair<uint64_t, Bytes>> drainRoutes(DatagramSocket& socket) {
    std::vector<std::pair<uint64_t, Bytes>> out;
    Bytes buffer(65536);
    Endpoint from;
    while (const auto size = socket.receive(buffer, from)) {
        const auto route = RouteDemux::routeOf(from);
        CHECK(route.has_value());
        out.emplace_back(route.value_or(0), Bytes(buffer.begin(), buffer.begin() + ptrdiff_t(*size)));
    }
    return out;
}

// Routes stand in for addresses no peer has, and come back from them; real
// addresses are no route.
void routeAddresses() {
    for (const uint64_t route : {uint64_t(0), uint64_t(1), uint64_t(0x0123456789abcdefull), ~uint64_t(0)}) {
        CHECK(RouteDemux::routeOf(RouteDemux::endpoint(route)) == route);
    }
    CHECK(RouteDemux::endpoint(1) != RouteDemux::endpoint(2));
    CHECK(!RouteDemux::routeOf(Endpoint::parse("127.0.0.1", 0)));
    CHECK(!RouteDemux::routeOf(Endpoint::parse("::1", 0)));
    CHECK(!RouteDemux::routeOf(Endpoint::parse("100::1", 5))); // A port: no stand-in.
}

// Handshakes of every route on the handshake socket; once a connection is
// made on a route its data goes there (that which waited first), its hellos
// still to the handshakes; each route's sends reach the host with the route.
void routes() {
    std::vector<std::pair<uint64_t, Bytes>> sent;
    RouteDemux demux(RouteDemux::Kind::datagram, [&](uint64_t route, std::span<const uint8_t> bytes) {
        sent.emplace_back(route, Bytes(bytes.begin(), bytes.end()));
        return true;
    });
    auto handshakes = demux.handshakes();
    demux.receive(7, hello(1));
    demux.receive(9, hello(2));
    demux.receive(7, data(42, 3)); // Ahead of its connection.
    auto waiting = drainRoutes(*handshakes);
    CHECK(waiting.size() == 3 && waiting[0].first == 7 && waiting[1].first == 9 && waiting[2].first == 7);
    demux.receive(7, data(42, 4));
    auto seven = demux.connection(42, RouteDemux::endpoint(7));
    auto got = drainRoutes(*seven);
    CHECK(got.size() == 1 && got[0] == std::pair(uint64_t(7), data(42, 4)));
    demux.receive(7, data(42, 5));
    demux.receive(7, hello(6)); // Its peer connects anew: a handshake again.
    demux.receive(9, data(43, 7)); // No connection on 9 yet: answered as unknown.
    got = drainRoutes(*seven);
    CHECK(got.size() == 1 && got[0].second == data(42, 5));
    waiting = drainRoutes(*handshakes);
    CHECK(waiting.size() == 2 && waiting[0] == std::pair(uint64_t(7), hello(6)) && waiting[1].first == 9);
    CHECK(seven->send(RouteDemux::endpoint(7), datagram(50, 8)));
    CHECK(handshakes->send(RouteDemux::endpoint(9), datagram(60, 9)));
    CHECK(sent.size() == 2 && sent[0] == std::pair(uint64_t(7), datagram(50, 8)) && sent[1].first == 9);
    // A route closes: its connection reads what came before (a goodbye
    // among it), then fails once; the others go on.
    auto nine = demux.connection(43, RouteDemux::endpoint(9));
    demux.receive(7, data(42, 14));
    demux.closed(7);
    Bytes buffer(2048);
    Endpoint from;
    const auto last = seven->receive(buffer, from);
    CHECK(last && Bytes(buffer.begin(), buffer.begin() + ptrdiff_t(*last)) == data(42, 14));
    CHECK(resultOf([&] { seven->receive(buffer, from); }) == RCS_ERROR_NETWORK);
    demux.receive(9, data(43, 10));
    CHECK(drainRoutes(*nine).size() == 1);
    // What comes on the closed route from now on is a new peer's, for the handshakes.
    demux.receive(7, data(42, 11));
    CHECK(drainRoutes(*seven).empty() && drainRoutes(*handshakes).size() == 1);
    // A restart is the connection's news, not an end.
    const uint64_t restarts = nine->restarts();
    demux.restarted(9);
    CHECK(nine->restarts() == restarts + 1);
    demux.receive(9, data(43, 12));
    CHECK(drainRoutes(*nine).size() == 1);
    // A new connection on a route ends the one there was.
    auto again = demux.connection(44, RouteDemux::endpoint(9));
    CHECK(resultOf([&] { nine->receive(buffer, from); }) == RCS_ERROR_NETWORK);
    demux.receive(9, data(44, 13));
    CHECK(drainRoutes(*again).size() == 1);
}

// A channel that takes batches (a forwarder's TCP and WebSocket entries)
// gets each run of a batch to one route at once, in order; what it does not
// take is counted.
void routeBatches() {
    std::vector<std::pair<uint64_t, std::vector<Bytes>>> calls;
    RouteDemux demux(
        RouteDemux::Kind::datagram, [](uint64_t, std::span<const uint8_t>) { return false; },
        [&](uint64_t route, std::span<const transport::Outgoing> datagrams) {
            auto& call = calls.emplace_back(route, std::vector<Bytes>{});
            for (const auto& d : datagrams) {
                call.second.emplace_back(d.datagram.begin(), d.datagram.end());
            }
            return calls.size() == 1 ? datagrams.size() - 1 : datagrams.size();
        });
    auto handshakes = demux.handshakes();
    const auto seven = RouteDemux::endpoint(7), nine = RouteDemux::endpoint(9), alsoSeven = RouteDemux::endpoint(7);
    std::vector<Bytes> datagrams;
    for (uint8_t i = 0; i < 6; ++i) {
        datagrams.push_back(datagram(50, i));
    }
    const std::vector<transport::Outgoing> batch{{&seven, datagrams[0]}, {&alsoSeven, datagrams[1]},
                                                {&seven, datagrams[2]},  {&nine, datagrams[3]},
                                                {&nine, datagrams[4]},   {&seven, datagrams[5]}};
    CHECK(handshakes->sendBatch(batch) == 0);
    CHECK(calls.size() == 3 && calls[0].first == 7 && calls[1].first == 9 && calls[2].first == 7);
    CHECK(calls[0].second == std::vector<Bytes>(datagrams.begin(), datagrams.begin() + 3) &&
          calls[1].second == std::vector<Bytes>(datagrams.begin() + 3, datagrams.begin() + 5) &&
          calls[2].second == std::vector<Bytes>{datagrams[5]});
    CHECK(demux.refused() == 1);
}

// A stream per route, framed on its own however the pieces of different
// routes interleave; a closed route's stream starts over.
void routeStreams() {
    std::vector<std::pair<uint64_t, Bytes>> sent;
    RouteDemux demux(RouteDemux::Kind::stream, [&](uint64_t route, std::span<const uint8_t> bytes) {
        sent.emplace_back(route, Bytes(bytes.begin(), bytes.end()));
        return true;
    });
    auto handshakes = demux.handshakes();
    const auto framed = [](const Bytes& d) {
        Bytes frame{uint8_t(d.size() >> 8), uint8_t(d.size())};
        frame.insert(frame.end(), d.begin(), d.end());
        return frame;
    };
    const Bytes a = framed(hello(1)), b = framed(hello(2));
    for (size_t at = 0; at < a.size(); at += 7) { // Interleaved pieces of two routes.
        demux.receive(1, std::span(a).subspan(at, std::min<size_t>(7, a.size() - at)));
        demux.receive(2, std::span(b).subspan(at, std::min<size_t>(7, b.size() - at)));
    }
    const auto got = drainRoutes(*handshakes);
    CHECK(got.size() == 2 && got[0] == std::pair(uint64_t(1), hello(1)) && got[1] == std::pair(uint64_t(2), hello(2)));
    CHECK(handshakes->send(RouteDemux::endpoint(2), datagram(5, 3)));
    CHECK(sent.size() == 1 && sent[0].first == 2 && sent[0].second == framed(datagram(5, 3)));
    demux.receive(1, std::span(a).first(10)); // Half a frame...
    demux.closed(1);                          // ...gone with the close.
    demux.receive(1, a);
    CHECK(drainRoutes(*handshakes).size() == 1);
}

// A flood under made-up routes: each route waits with at most 64 KiB, all of
// them with 1 MiB, and at most 1024 routes wait; a real viewer's handshake on
// a route of its own still gets in once the flood stops, and a connection's
// own socket is never touched.
void routeFloods() {
    RouteDemux demux(RouteDemux::Kind::datagram, [](uint64_t, std::span<const uint8_t>) { return true; });
    auto handshakes = demux.handshakes();
    auto connected = demux.connection(5, RouteDemux::endpoint(1));
    for (int i = 0; i < 1000; ++i) { // One route sends plenty: its share is capped.
        demux.receive(2, data(9, uint8_t(i), 1200));
    }
    const auto capped = drainRoutes(*handshakes);
    CHECK(capped.size() <= RouteDemux::maxWaitingPerRoute / 1200 && capped.size() >= 50);
    size_t full = 0;
    for (uint64_t route = 100; route < 100 + 4 * RouteDemux::maxRoutes; ++route) {
        full += resultOf([&] { demux.receive(route, hello(uint8_t(route))); }) == RCS_ERROR_QUEUE_FULL;
    }
    CHECK(full > 0);
    size_t waiting = drainRoutes(*handshakes).size();
    CHECK(waiting <= RouteDemux::maxRoutes);
    demux.receive(3, hello(9)); // The flood read: room again.
    const auto after = drainRoutes(*handshakes);
    CHECK(after.size() == 1 && after[0].first == 3);
    demux.receive(1, data(5, 1));
    CHECK(drainRoutes(*connected).size() == 1);
    // Datagrams larger than a channel carries, and none at all, are the host's mistake.
    CHECK(resultOf([&] { demux.receive(1, Bytes(65536)); }) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(resultOf([&] { demux.receive(1, Bytes()); }) == RCS_ERROR_INVALID_ARGUMENT);
}

} // namespace

int main() {
    return runTest("host_channel", [] {
        datagrams();
        streams();
        closing();
        waking();
        routeAddresses();
        routes();
        routeBatches();
        routeStreams();
        routeFloods();
    });
}
