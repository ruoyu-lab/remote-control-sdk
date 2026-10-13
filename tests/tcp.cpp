// RCS_TRANSPORT_TCP over loopback: the datagram connection over TCP
// connections to a sharer's port (protocol/tcp_demux.hpp). The handshakes run
// side by side; a connection goes on over a new TCP connection of its peer's;
// what moves it nowhere: another key, a replayed datagram, one of the peer's
// held back and sent over a TCP connection that cannot answer, an id the port
// does not know, a flood of TCP connections; a closed TCP connection without
// migration ends it.
#include "protocol/tcp_demux.hpp"
#include "protocol/udp_connection.hpp"
#include "support/check.hpp"
#include "support/tcp_proxy.hpp"
#include "transport/framed_socket.hpp"
#include "transport/handshake.hpp"
#include "transport/tcp_stream.hpp"
#include "util/bytes.hpp"

#include <algorithm>
#include <future>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace rcsdk;
using namespace rcsdk::protocol;
using transport::Endpoint;

namespace {

const std::array<uint8_t, 32> key{7, 7, 7};
const auto credentials = transport::Credentials::fromKey(key);
std::atomic<bool> never{false};

UdpOptions tcpOptions() {
    UdpOptions options;
    options.maxDatagram = options.maxReceived = tcpDatagram;
    return options;
}

std::unique_ptr<transport::DatagramSocket> tcpTo(const Endpoint& to) {
    return std::make_unique<transport::FramedSocket>(
        transport::tcpBytes(transport::connectStreamTcp(to, "the sharer", never, std::chrono::seconds(2), 0),
                            "the sharer"),
        to, transport::lengthFraming());
}

// A viewer's connection over a TCP connection of its own to `to`.
std::unique_ptr<UdpConnection> dial(const Endpoint& to, const transport::Credentials& own = credentials,
                                    const UdpOptions& options = tcpOptions()) {
    return UdpConnection::establish(UdpConnection::Side::client, tcpTo(to), tcpLink(to), own, options, never);
}

template <class Done>
bool waitFor(Done&& done, std::chrono::milliseconds wait = std::chrono::seconds(3)) {
    for (const auto deadline = Clock::now() + wait; !done();) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// The path changes the connection reported since, waiting up to `wait` for `count` of them.
std::vector<PathEvent> pathEvents(UdpConnection& connection, size_t count,
                                  std::chrono::milliseconds wait = std::chrono::seconds(3)) {
    std::vector<PathEvent> events;
    for (const auto deadline = Clock::now() + wait; events.size() < count && Clock::now() < deadline;) {
        for (auto& event : connection.takePathEvents()) {
            events.push_back(std::move(event));
        }
        std::this_thread::sleep_for(10ms);
    }
    return events;
}

// A sharer's TCP port: its demultiplexer and acceptor, with the credentials
// it takes now and what it rejected.
struct Port {
    explicit Port(uint16_t port, std::chrono::milliseconds handshake = std::chrono::seconds(5))
        : at(Endpoint::parse("127.0.0.1", port)), demux(at, handshake, [this](const Failure& f) { note(f); }),
          acceptor(
              demux,
              [this] {
                  std::lock_guard lock(mutex);
                  return current;
              },
              [this](const Failure& f) { note(f); }, handshake, tcpLink) {}

    void note(const Failure& failure) {
        std::lock_guard lock(mutex);
        rejected.push_back(failure.what());
    }
    size_t count(const std::string& reason) {
        std::lock_guard lock(mutex);
        return size_t(std::count_if(rejected.begin(), rejected.end(),
                                    [&](const std::string& r) { return r.find(reason) != std::string::npos; }));
    }
    // The next connection, taken on a thread of its own.
    std::future<std::unique_ptr<UdpConnection>> accepting(const UdpOptions& options = tcpOptions(),
                                                          const std::atomic<bool>& stop = never) {
        return std::async(std::launch::async, [this, options, &stop] {
            return acceptor.accept([&] { return options; }, stop);
        });
    }

    const Endpoint at;
    std::mutex mutex; // Below.
    std::vector<std::string> rejected;
    std::shared_ptr<const transport::Credentials> current = std::make_shared<const transport::Credentials>(credentials);
    TcpDemux demux;
    UdpAcceptor acceptor;
};

// Receives in the background until it goes: the inputs' epochs, and how it ended.
struct Watching {
    explicit Watching(UdpConnection& connection)
        : connection(connection), thread([this, &connection] {
              try {
                  for (;;) {
                      auto message = connection.receive();
                      std::lock_guard lock(mutex);
                      if (const auto* input = std::get_if<Input>(&message)) {
                          inputs.push_back(input->epoch);
                      }
                  }
              } catch (const Failure& failure) {
                  std::lock_guard lock(mutex);
                  ended = failure.result();
                  endedAt = Clock::now();
              }
          }) {}
    ~Watching() {
        connection.cancel();
        thread.join();
    }
    size_t count() {
        std::lock_guard lock(mutex);
        return inputs.size();
    }
    UdpConnection& connection;
    std::mutex mutex;
    std::vector<uint64_t> inputs;
    std::optional<rcs_result> ended;
    Clock::time_point endedAt;
    std::thread thread; // Last.
};

// The next datagram of `kind` on `socket` within 2 s.
crypto::Bytes await(transport::DatagramSocket& socket, uint8_t kind) {
    std::vector<uint8_t> buffer(tcpDatagram);
    for (const auto deadline = Clock::now() + std::chrono::seconds(2); Clock::now() < deadline;) {
        Endpoint from;
        if (const auto size = socket.receive(buffer, from)) {
            if (*size && buffer[0] == kind) {
                return crypto::Bytes(buffer.begin(), buffer.begin() + ptrdiff_t(*size));
            }
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    CHECK(false);
    return {};
}

// Everything that came on `socket`, and comes for `wait`, in bytes.
size_t drain(transport::DatagramSocket& socket, std::chrono::milliseconds wait) {
    std::vector<uint8_t> buffer(tcpDatagram);
    size_t bytes = 0;
    const auto deadline = Clock::now() + wait;
    for (;;) {
        Endpoint from;
        while (const auto size = socket.receive(buffer, from)) {
            bytes += *size;
        }
        if (Clock::now() >= deadline) {
            return bytes;
        }
        std::this_thread::sleep_for(10ms);
    }
}

crypto::Bytes framed(uint8_t kind, std::span<const uint8_t> body) {
    crypto::Bytes datagram{kind};
    datagram.insert(datagram.end(), body.begin(), body.end());
    return datagram;
}

// A viewer made by hand over a TCP connection of its own: it handshakes, then
// sends frames the test makes up, sealed as a real peer's are.
struct RawPeer {
    explicit RawPeer(const Endpoint& at) : server(at), socket(tcpTo(at)) {
        transport::ClientHandshake client(credentials);
        socket->send(server, helloDatagram(client.hello()));
        const auto answer = await(*socket, serverHelloKind);
        socket->send(server, framed(finishedKind, client.finish(std::span(answer).subspan(1), keys)));
    }
    crypto::Bytes seal(std::span<const uint8_t> frame) {
        crypto::Bytes datagram{dataKind};
        appendBigEndian(datagram, keys.sendId, 4);
        appendBigEndian(datagram, ++packet, 8);
        const auto sealed = keys.send->seal(packet, datagram, frame);
        datagram.insert(datagram.end(), sealed.begin(), sealed.end());
        return datagram;
    }
    // A ping, as a real peer's (it follows to new paths).
    static crypto::Bytes ping() {
        crypto::Bytes frame{1};
        appendBigEndian(frame, monotonicNs(), 8);
        frame.push_back(1);
        return frame;
    }
    Endpoint server;
    std::unique_ptr<transport::DatagramSocket> socket;
    transport::TrafficKeys keys;
    uint64_t packet = 0;
};

rcs_input_event moveEvent() {
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    return move;
}

// The port runs handshakes side by side: peers that send a hello and stall,
// or say nothing, hold no one up. It takes the credentials as they are at
// each hello, and drops (and reports) a peer when its own handshake fails or
// times out, when it is silent for 2 s, and when 16 newer ones came.
void handshakesSideBySide() {
    Port port(47924);
    std::vector<std::unique_ptr<transport::DatagramSocket>> stalled;
    for (int i = 0; i < 3; ++i) {
        stalled.push_back(tcpTo(port.at));
        stalled.back()->send(port.at, helloDatagram(transport::ClientHandshake(credentials).hello()));
    }
    const auto silent = transport::TcpStream::connect("127.0.0.1", 47924, never, std::chrono::seconds(1));
    std::this_thread::sleep_for(100ms);
    auto accepted = port.accepting();
    const auto started = Clock::now();
    auto viewer = dial(port.at);
    auto connection = accepted.get();
    CHECK(Clock::now() - started < 1s);
    viewer->send(KeyframeRequest{});
    CHECK(std::holds_alternative<KeyframeRequest>(connection->receive()));
    CHECK(port.count("") == 0); // The others still have their time.
    // The port stays open: a second viewer gets in beside the first.
    accepted = port.accepting();
    auto second = dial(port.at);
    auto secondConnection = accepted.get();
    second->send(KeyframeRequest{});
    CHECK(std::holds_alternative<KeyframeRequest>(secondConnection->receive()));
    // The silent peer is dropped when its time is up.
    CHECK(waitFor([&] { return port.count("No handshake within 2 s") == 1; }, 4s));
    // Credentials are taken at each hello: a viewer with the new key gets in,
    // one with the old key no longer does.
    const std::array<uint8_t, 32> newKey{9, 9, 9};
    {
        std::lock_guard lock(port.mutex);
        port.current = std::make_shared<const transport::Credentials>(transport::Credentials::fromKey(newKey));
    }
    accepted = port.accepting();
    CHECK(throws<Failure>([&] { dial(port.at); }));
    auto renewed = dial(port.at, transport::Credentials::fromKey(newKey));
    CHECK(accepted.get() != nullptr);
    {
        std::lock_guard lock(port.mutex);
        port.current = std::make_shared<const transport::Credentials>(credentials);
    }
    // More than 16 at once: the newest replaces the oldest, and a viewer still gets in.
    stalled.clear();
    for (int i = 0; i < 17; ++i) {
        stalled.push_back(tcpTo(port.at));
        stalled.back()->send(port.at, helloDatagram(transport::ClientHandshake(credentials).hello()));
    }
    std::this_thread::sleep_for(100ms);
    accepted = port.accepting();
    auto late = dial(port.at);
    CHECK(accepted.get() != nullptr);
    CHECK(port.count("16 newer") >= 1);
    // A peer of another protocol version learns this end's over its TCP
    // connection, and is reported with both.
    auto hello = helloDatagram(transport::ClientHandshake(credentials).hello());
    hello[1 + 7] ^= 1;
    auto other = tcpTo(port.at);
    other->send(port.at, hello);
    accepted = port.accepting();
    const auto notice = await(*other, versionKind);
    CHECK(std::equal(notice.begin() + 1, notice.end(), transport::versionNotice().begin(),
                     transport::versionNotice().end()));
    auto last = dial(port.at);
    CHECK(accepted.get() != nullptr);
    CHECK(port.count("version " + std::to_string(hello[1 + 7])) >= 1);
}

// The viewer's TCP connection breaks: the sharer's path stalls at once (its
// port says the peer's TCP connection closed), the viewer's too. The viewer
// connects anew and hands the TCP connection to its connection: the same
// connection goes on there, the sharer follows, and the inputs sent before,
// meanwhile and after arrive once each and in order. The broken one is gone
// from the port.
void movesToANewConnection() {
    Port port(47925);
    test::TcpProxy proxy(port.at);
    auto options = tcpOptions();
    options.pathStall = 500ms;
    options.idleTimeout = 3s;
    options.hold = 10s;
    auto accepted = port.accepting(options);
    auto viewer = dial(proxy.address(), credentials, options);
    auto sharer = accepted.get();
    Watching watching(*sharer);
    uint64_t epoch = 0;
    for (; epoch < 20; ++epoch) {
        viewer->send(Input{moveEvent(), epoch + 1});
    }
    CHECK(waitFor([&] { return watching.count() == 20; }));
    const auto before = sharer->peerAddress();
    const auto cut = Clock::now();
    proxy.cut();
    const auto stalled = pathEvents(*sharer, 1);
    CHECK(stalled.size() == 1 && stalled[0].change == RCS_PATH_CHANGING && stalled[0].cause == RCS_PATH_CAUSE_PEER);
    CHECK(Clock::now() - cut < 400ms); // Before path_stall: the port said so.
    const auto own = pathEvents(*viewer, 1);
    CHECK(own.size() == 1 && own[0].change == RCS_PATH_CHANGING && own[0].cause == RCS_PATH_CAUSE_NETWORK);
    for (; epoch < 40; ++epoch) { // Wait for the new path.
        viewer->send(Input{moveEvent(), epoch + 1});
    }
    viewer->addRoute(tcpTo(proxy.address()), tcpLink(proxy.address()));
    for (; epoch < 60; ++epoch) {
        viewer->send(Input{moveEvent(), epoch + 1});
        std::this_thread::sleep_for(5ms);
    }
    const auto moved = pathEvents(*viewer, 1), followed = pathEvents(*sharer, 1);
    CHECK(moved.size() == 1 && moved[0].change == RCS_PATH_CHANGED && moved[0].cause == RCS_PATH_CAUSE_NETWORK);
    CHECK(followed.size() == 1 && followed[0].change == RCS_PATH_CHANGED &&
          followed[0].cause == RCS_PATH_CAUSE_PEER);
    CHECK(sharer->peerAddress() != before && followed[0].to.remote == sharer->peerAddress());
    CHECK(waitFor([&] { return watching.count() >= 60; }));
    {
        std::lock_guard lock(watching.mutex);
        CHECK(watching.inputs.size() == 60);
        for (size_t i = 0; i < watching.inputs.size(); ++i) {
            CHECK(watching.inputs[i] == i + 1);
        }
    }
    CHECK(!viewer->status().lost && !sharer->status().lost);
    CHECK(waitFor([&] { return port.demux.connections() == 1; }));
}

// What moves a connection nowhere, over TCP connections of an attacker's:
// datagrams under its id that do not authenticate, a datagram of the peer's
// played again, and a fresh one of the peer's (held back on the way) from a
// TCP connection that cannot answer: that one is challenged there, but the
// real peer's newer traffic keeps the connection where it is. An id the port
// does not know is answered as unknown. TCP connections nothing came over
// that a connection authenticated close once their time is up.
void movesNowhere() {
    Port port(47926, std::chrono::seconds(1));
    std::atomic<bool> stop{false};
    auto accepted = port.accepting();
    RawPeer peer(port.at);
    auto connection = accepted.get();
    auto listening = port.accepting(tcpOptions(), stop); // Answers what comes for no connection.
    peer.socket->send(port.at, peer.seal(RawPeer::ping()));
    std::this_thread::sleep_for(200ms);
    const auto home = connection->peerAddress();
    CHECK(home.has_value());

    auto forged = peer.seal(RawPeer::ping());
    forged.back() ^= 1;
    auto forger = tcpTo(port.at);
    forger->send(port.at, forged);
    const auto replayed = peer.seal(RawPeer::ping());
    peer.socket->send(port.at, replayed);
    auto replayer = tcpTo(port.at);
    std::this_thread::sleep_for(50ms);
    replayer->send(port.at, replayed);
    const auto heldBack = peer.seal(RawPeer::ping());
    auto holder = tcpTo(port.at);
    holder->send(port.at, heldBack);
    size_t toHolder = drain(*holder, 100ms), toOthers = 0; // It came as the peer's newest, before the next.
    for (const auto until = Clock::now() + 1500ms; Clock::now() < until;) {
        peer.socket->send(port.at, peer.seal(RawPeer::ping())); // The real peer goes on where it was.
        toHolder += drain(*holder, 100ms);
        toOthers += drain(*forger, 0ms) + drain(*replayer, 0ms);
    }
    CHECK(toHolder > 0 && toHolder <= 3 * heldBack.size()); // A challenge went there, within the limit.
    CHECK(toOthers == 0);
    CHECK(connection->peerAddress() == home && connection->takePathEvents().empty());

    crypto::Bytes stranger{dataKind, 0x12, 0x34, 0x56, 0x78};
    stranger.resize(stranger.size() + 8 + 16, 0);
    auto lost = tcpTo(port.at);
    lost->send(port.at, stranger);
    const auto unknown = await(*lost, unknownKind);
    CHECK(unknown.size() == 5 && std::equal(unknown.begin() + 1, unknown.end(), stranger.begin() + 1));
    // The forger's, the replayer's and the stranger's close once their time is
    // up; the peer's and the one challenged belong to the connection.
    CHECK(waitFor([&] { return port.demux.connections() == 2; }, std::chrono::seconds(6)));
    CHECK(throws<Failure>([&] { drain(*forger, 100ms); }));
    stop = true;
    CHECK(throws<Failure>([&] { const auto taken = listening.get(); }));
}

// A flood of TCP connections that say nothing, or half a datagram: a real
// viewer still gets in (the newest replace the oldest of them), and moves to
// another TCP connection of its own.
void flood() {
    Port port(47927);
    std::vector<std::unique_ptr<transport::TcpStream>> flooding;
    for (int i = 0; i < 40; ++i) {
        flooding.push_back(transport::TcpStream::connect("127.0.0.1", 47927, never, std::chrono::seconds(1)));
        if (i % 2) {
            const uint8_t half[] = {0x04, 0xB0, 0xD1, 0, 0, 0};
            flooding.back()->write(half, std::chrono::seconds(1));
        }
    }
    std::this_thread::sleep_for(100ms);
    auto accepted = port.accepting();
    auto viewer = dial(port.at);
    auto sharer = accepted.get();
    CHECK(port.count("16 newer") >= 20);
    Watching watching(*sharer);
    viewer->addRoute(tcpTo(port.at), tcpLink(port.at));
    for (uint64_t epoch = 1; epoch <= 20; ++epoch) {
        viewer->send(Input{moveEvent(), epoch});
        std::this_thread::sleep_for(10ms);
    }
    const auto moved = pathEvents(*sharer, 1);
    CHECK(moved.size() == 1 && moved[0].change == RCS_PATH_CHANGED);
    CHECK(waitFor([&] { return watching.count() == 20; }));
}

// Without migration a TCP connection that closes ends its connection at
// once, on both ends, as before.
void closedWithoutMigration() {
    Port port(47928);
    test::TcpProxy proxy(port.at);
    auto options = tcpOptions();
    options.migrate = false;
    auto accepted = port.accepting(options);
    auto viewer = dial(proxy.address(), credentials, options);
    auto sharer = accepted.get();
    Watching sharing(*sharer), viewing(*viewer);
    std::this_thread::sleep_for(100ms);
    const auto cut = Clock::now();
    proxy.cut();
    CHECK(waitFor([&] {
        std::lock_guard a(sharing.mutex), b(viewing.mutex);
        return sharing.ended && viewing.ended;
    }));
    std::lock_guard a(sharing.mutex), b(viewing.mutex);
    CHECK(*sharing.ended == RCS_ERROR_NETWORK && *viewing.ended == RCS_ERROR_NETWORK);
    CHECK(sharing.endedAt - cut < 500ms && viewing.endedAt - cut < 500ms);
}

#ifndef _WIN32
// The process has no descriptor for a viewer that connects: the port neither
// fails nor spins (the system resets that viewer, or keeps it in the port's
// backlog), and takes the next viewer once a descriptor is free.
void descriptorShortage() {
    Port port(47929);
    rlimit original{};
    CHECK(getrlimit(RLIMIT_NOFILE, &original) == 0);
    const int lowest = ::dup(0);
    ::close(lowest);
    rlimit low = original;
    low.rlim_cur = rlim_t(lowest + 16);
    CHECK(setrlimit(RLIMIT_NOFILE, &low) == 0);
    std::vector<int> held;
    for (int fd; (fd = ::dup(0)) >= 0;) {
        held.push_back(fd);
    }
    // One connects meanwhile (its socket from before the shortage).
    ::close(held.back());
    held.pop_back();
    auto starving = std::async(std::launch::async, [&] {
        try {
            transport::TcpStream::connect("127.0.0.1", 47929, never, std::chrono::seconds(1));
        } catch (const Failure&) {
        }
    });
    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    std::this_thread::sleep_for(500ms);
    getrusage(RUSAGE_SELF, &after);
    const auto cpu = [](const rusage& u) {
        return std::chrono::seconds(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
               std::chrono::microseconds(u.ru_utime.tv_usec + u.ru_stime.tv_usec);
    };
    CHECK(cpu(after) - cpu(before) < 200ms); // Waits, does not spin.
    for (const int fd : held) {
        ::close(fd);
    }
    CHECK(setrlimit(RLIMIT_NOFILE, &original) == 0);
    starving.get();
    auto accepted = port.accepting();
    auto viewer = dial(port.at);
    auto sharer = accepted.get();
    viewer->send(KeyframeRequest{});
    CHECK(std::holds_alternative<KeyframeRequest>(sharer->receive()));
}
#endif

} // namespace

int main() {
    return runTest("tcp", [] {
        handshakesSideBySide();
        movesToANewConnection();
        movesNowhere();
        flood();
        closedWithoutMigration();
#ifndef _WIN32
        descriptorShortage();
#endif
    });
}
