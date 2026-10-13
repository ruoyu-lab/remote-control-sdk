// Handshake, another version's notice, records, record size, timeouts and
// listener exclusivity over loopback TCP (the agent link's channel; a
// sharer's TCP port: tcp.cpp).
#include "support/check.hpp"
#include "transport/handshake.hpp"
#include "transport/secure_channel.hpp"
#include "transport/tcp_stream.hpp"

#include <algorithm>
#include <cstring>
#include <future>
#include <thread>

using namespace rcsdk;
using namespace rcsdk::transport;

namespace {

constexpr uint16_t port = 47811;
const std::array<uint8_t, 32> key{1, 2, 3};

std::unique_ptr<SecureChannel> serve(std::array<uint8_t, 32> serverKey, const std::atomic<bool>& cancelled) {
    TcpListenSocket listener("127.0.0.1", port);
    auto stream = listener.accept(cancelled);
    return std::make_unique<SecureChannel>(std::move(stream), SecureChannel::Side::server,
                                           Credentials::fromKey(serverKey));
}

std::unique_ptr<SecureChannel> join(std::array<uint8_t, 32> clientKey, const std::atomic<bool>& cancelled) {
    std::this_thread::sleep_for(50ms); // Let the listener bind first.
    auto stream = TcpStream::connect("127.0.0.1", port, cancelled, 2s);
    return std::make_unique<SecureChannel>(std::move(stream), SecureChannel::Side::client,
                                           Credentials::fromKey(clientKey));
}

rcs_result resultOf(std::future<std::unique_ptr<SecureChannel>>& future) {
    try {
        const auto channel = future.get();
        return RCS_OK;
    } catch (const Failure& failure) {
        return failure.result();
    }
}

} // namespace

Credentials withKey(std::span<const uint8_t, 32> psk) {
    return Credentials::fromKey(psk);
}

Credentials withPassword(const std::string& password) {
    Credentials credentials;
    credentials.secret = RCS_SECRET_PASSWORD;
    credentials.password = password;
    credentials.passwordKeys = std::make_shared<PasswordKeys>();
    return credentials;
}

Credentials withPin(const std::string& pin, uint32_t attempts = 3, PinAttempts::Report report = {}) {
    Credentials credentials;
    credentials.secret = RCS_SECRET_PIN;
    credentials.password = pin;
    credentials.pinAttempts = std::make_shared<PinAttempts>(attempts, std::move(report));
    return credentials;
}

std::shared_ptr<const crypto::DeviceKey> newDevice() {
    return std::make_shared<const crypto::DeviceKey>(crypto::DeviceKey::generate());
}

// One handshake without a network; throws what either side throws.
void exchange(const Credentials& client, const Credentials& server, TrafficKeys& clientKeys, TrafficKeys& serverKeys) {
    ClientHandshake clientSide(client);
    ServerHandshake serverSide(server, clientSide.hello());
    serverSide.finish(clientSide.finish(serverSide.answer(), clientKeys), serverKeys);
}

std::string failureOf(const Credentials& client, const Credentials& server) {
    TrafficKeys c, s;
    try {
        exchange(client, server, c, s);
    } catch (const Failure& failure) {
        return failure.what();
    }
    return {};
}

// The key agreement without a network.
void handshakes() {
    const auto shared = withKey(key);
    const crypto::Bytes message{9, 8, 7};
    TrafficKeys c1, s1, c2, s2;
    exchange(shared, shared, c1, s1);
    const auto sealed = c1.send->seal(1, {}, message);
    CHECK(s1.receive->open(1, {}, sealed) == message);
    CHECK(c1.receive->open(1, {}, s1.send->seal(1, {}, message)) == message);
    CHECK(!s1.receive->open(1, {}, s1.send->seal(1, {}, message))); // Directions use separate keys.
    CHECK(!c1.peerKey && !s1.peerKey);                               // No device keys.
    // Fresh ephemeral keys: the same PSK never yields the same traffic keys twice.
    exchange(shared, shared, c2, s2);
    CHECK(!s2.receive->open(1, {}, sealed));

    // A different PSK fails on the client, which never sends its finished message.
    auto wrongKey = key;
    wrongKey[0] ^= 1;
    const auto wrong = withKey(wrongKey);
    CHECK(failureOf(wrong, shared).find("key mismatch") != std::string::npos);
    // A man in the middle without the PSK cannot substitute its own key share.
    {
        ClientHandshake client(shared), attacker(wrong);
        auto hello = client.hello();
        std::copy(attacker.hello().begin() + 41, attacker.hello().end(), hello.begin() + 41);
        ServerHandshake server(shared, hello);
        const auto reply = server.answer();
        TrafficKeys keys;
        CHECK(throws<Failure>([&] { client.finish(reply, keys); }));
        CHECK(throws<Failure>([&] { server.finish(crypto::Bytes(finishedSize, 0), keys); }));
    }
    // A hello of another protocol version: a clear version error, before it
    // costs a key pair. A malformed reply: an authentication failure.
    {
        ClientHandshake client(shared);
        auto other = client.hello();
        other[7] ^= 1;
        try {
            ServerHandshake server(shared, other);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_UNSUPPORTED);
            CHECK(std::string(failure.what()).find("version " + std::to_string(other[7])) != std::string::npos);
        }
        CHECK(throws<Failure>([&] { ServerHandshake(shared, std::span(client.hello()).first(50)); }));
        TrafficKeys keys;
        CHECK(throws<Failure>([&] { client.finish(crypto::Bytes(10), keys); }));
    }

    // A password: both ends derive the same key with the sharer's salt; another
    // password, or another secret, fails and says so.
    const auto sharer = withPassword("correct horse battery");
    const auto viewer = withPassword("correct horse battery");
    TrafficKeys cp, sp;
    exchange(viewer, sharer, cp, sp);
    CHECK(sp.receive->open(1, {}, cp.send->seal(1, {}, message)) == message);
    CHECK(failureOf(withPassword("correct horse battery!"), sharer).find("Wrong password") != std::string::npos);
    CHECK(failureOf(shared, sharer).find("authenticates with a password") != std::string::npos);

    // Device keys: each end learns the key the other proved, sealed from observers.
    auto serverDevice = shared, clientDevice = shared;
    serverDevice.device = newDevice();
    clientDevice.device = newDevice();
    TrafficKeys cd, sd;
    exchange(clientDevice, serverDevice, cd, sd);
    CHECK(cd.peerKey == serverDevice.device->publicKey() && sd.peerKey == clientDevice.device->publicKey());
    {
        ClientHandshake client(clientDevice);
        ServerHandshake server(serverDevice, client.hello());
        const auto& reply = server.answer();
        const auto& proven = serverDevice.device->publicKey();
        CHECK(std::search(reply.begin(), reply.end(), proven.begin() + 1, proven.end()) == reply.end());
    }
    // Trusted keys: the expected key passes; another key is refused, and the
    // refusal names the key it saw; no key at all is refused too.
    auto pinning = clientDevice;
    pinning.trusted = {serverDevice.device->publicKey()};
    exchange(pinning, serverDevice, cd, sd);
    pinning.trusted = {clientDevice.device->publicKey()};
    try {
        exchange(pinning, serverDevice, cd, sd);
        CHECK(false);
    } catch (const UntrustedPeer& refused) {
        CHECK(refused.key() == serverDevice.device->publicKey() && refused.result() == RCS_ERROR_AUTHENTICATION);
    }
    CHECK(failureOf(pinning, shared).find("proves no device key") != std::string::npos);
    auto guarding = serverDevice;
    guarding.trusted = {serverDevice.device->publicKey()};
    try {
        exchange(clientDevice, guarding, cd, sd);
        CHECK(false);
    } catch (const UntrustedPeer& refused) {
        CHECK(refused.key() == clientDevice.device->publicKey());
    }
    try { // A client that proves no key: refusable as well, under the agreed keys.
        exchange(shared, guarding, cd, sd);
        CHECK(false);
    } catch (const UntrustedPeer& refused) {
        CHECK(!refused.key() && sd.send);
    }
    // Device keys alone: nothing shared, both ends pin the other.
    auto server = serverDevice, client = clientDevice;
    server.secret = client.secret = RCS_SECRET_NONE;
    server.trusted = {clientDevice.device->publicKey()};
    client.trusted = {serverDevice.device->publicKey()};
    TrafficKeys cn, sn;
    exchange(client, server, cn, sn);
    CHECK(sn.receive->open(1, {}, cn.send->seal(1, {}, message)) == message);
    // An impostor with its own device key is refused by both.
    auto impostor = client;
    impostor.device = newDevice();
    CHECK(throws<UntrustedPeer>([&] { exchange(impostor, server, cn, sn); }));
    auto fakeServer = server;
    fakeServer.device = newDevice();
    CHECK(throws<UntrustedPeer>([&] { exchange(client, fakeServer, cn, sn); }));
}

// Short PINs: CPace inside the handshake, and the sharer's attempt limit.
void pins() {
    std::vector<std::pair<uint32_t, uint32_t>> reports; // failures, left
    auto sharer = withPin("482916", 3, [&](uint32_t failures, uint32_t left) { reports.emplace_back(failures, left); });
    const auto viewer = withPin("482916");
    const crypto::Bytes message{1, 2, 3};
    TrafficKeys c, s;
    exchange(viewer, sharer, c, s);
    CHECK(s.receive->open(1, {}, c.send->seal(1, {}, message)) == message);
    CHECK(reports.empty()); // A proven attempt is no failure.

    // What travels: the CPace shares, fresh in every handshake (never a fixed
    // function of the PIN), at their places; zeros with any other secret. The
    // rest is what a connection key's handshake carries: the salt is no
    // per-run salt to precompute guesses against, and the proofs need the
    // ephemeral keys as well. A recording lets nobody test a guess.
    {
        constexpr size_t clientShare = clientHelloSize - cpaceShareSize, serverShare = 1 + 32 + 65;
        ClientHandshake first(viewer), second(viewer), keyed(withKey(key));
        const auto shareOf = [](const crypto::Bytes& bytes, size_t at) {
            return crypto::Bytes(bytes.begin() + ptrdiff_t(at), bytes.begin() + ptrdiff_t(at + cpaceShareSize));
        };
        CHECK(shareOf(first.hello(), clientShare) != shareOf(second.hello(), clientShare));
        CHECK(shareOf(first.hello(), clientShare) != crypto::Bytes(cpaceShareSize, 0));
        CHECK(shareOf(keyed.hello(), clientShare) == crypto::Bytes(cpaceShareSize, 0));
        const ServerHandshake one(sharer, first.hello()), two(sharer, first.hello());
        CHECK(shareOf(one.answer(), serverShare) != shareOf(two.answer(), serverShare));
        CHECK(shareOf(one.answer(), serverShare + cpaceShareSize).size() == 32);
        CHECK(!std::equal(one.answer().begin() + serverShare + 32, one.answer().begin() + serverShare + 48,
                          two.answer().begin() + serverShare + 32)); // Salts: random per handshake.
        TrafficKeys clientKeys, serverKeys;
        one.finish(first.finish(one.answer(), clientKeys), serverKeys);
    } // `one` was proven; `two` never was: one failure.
    CHECK(reports == (std::vector<std::pair<uint32_t, uint32_t>>{{1, 2}}));

    // A wrong PIN: the client fails at the sharer's proof; the sharer fails a
    // client's proof, and counts the attempt when it ends.
    CHECK(failureOf(withPin("482917"), sharer) == "Wrong PIN or another secret");
    CHECK(reports.size() == 2 && reports.back() == std::pair(2u, 1u));
    {
        ClientHandshake client(withPin("482917"));
        ServerHandshake server(sharer, client.hello());
        TrafficKeys keys;
        try {
            server.finish(crypto::Bytes(finishedSize, 0), keys);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_AUTHENTICATION &&
                  failure.what() == std::string("Wrong PIN or another secret"));
        }
        CHECK(reports.size() == 2); // Not yet: the real client's proof may still come.
    }
    // The third failure: no attempt left, even for the right PIN.
    CHECK(reports.size() == 3 && reports.back() == std::pair(3u, 0u));
    CHECK(throws<PinLocked>([&] { ServerHandshake(sharer, ClientHandshake(viewer).hello()); }));
    // Another secret is no PIN attempt: answered so that it can say what differs.
    CHECK(failureOf(withKey(key), sharer).find("authenticates with a PIN") != std::string::npos);
    CHECK(failureOf(viewer, withKey(key)).find("this end with a PIN") != std::string::npos);
    CHECK(reports.size() == 3);
    // Setting the PIN again counts afresh; attempts of before end uncounted.
    sharer.pinAttempts->reset(2);
    {
        const ServerHandshake stale(sharer, ClientHandshake(viewer).hello());
        sharer.pinAttempts->reset(2);
        TrafficKeys cr, sr;
        exchange(viewer, sharer, cr, sr);
        // Handshakes under way hold attempts: two unproven ones reach 2.
        const ServerHandshake a(sharer, ClientHandshake(viewer).hello()), b(sharer, ClientHandshake(viewer).hello());
        CHECK(throws<PinLocked>([&] { ServerHandshake(sharer, ClientHandshake(viewer).hello()); }));
        sharer.pinAttempts->reset(2);
    }
    CHECK(reports.size() == 3); // stale, a and b ended after a reset: uncounted.
    // A share of low order: refused (CPace's identity check) before an answer,
    // which lets nobody test a guess: no failure, however many (SA-04).
    for (int hello = 0; hello < 5; ++hello) {
        ClientHandshake client(viewer);
        auto bytes = client.hello();
        std::fill(bytes.end() - ptrdiff_t(cpaceShareSize), bytes.end(), uint8_t(0));
        try {
            ServerHandshake server(sharer, bytes);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_AUTHENTICATION);
        }
    }
    CHECK(reports.size() == 3);
    {
        TrafficKeys cr, sr;
        exchange(viewer, sharer, cr, sr); // The right PIN still gets in.
    }
    // An answer the client never finishes is a failure all the same: it could
    // test its guess on the answer alone.
    {
        const ServerHandshake answered(sharer, ClientHandshake(withPin("000000")).hello());
    }
    CHECK(reports.size() == 4 && reports.back() == std::pair(1u, 1u));
    // A client tests one answer (SA-01): after an impostor's with a wrong PIN,
    // the real sharer's fails untested, so whoever answers in the sharer's
    // place tests one guess per hello. A share of low order spends nothing.
    {
        const auto real = withPin("482917"), impostor = withPin("000000"), typed = withPin("482917");
        ClientHandshake client(typed);
        const ServerHandshake guess(impostor, client.hello()), sharerSide(real, client.hello());
        auto lowOrder = guess.answer();
        std::fill_n(lowOrder.begin() + 1 + 32 + 65, cpaceShareSize, uint8_t(0));
        TrafficKeys keys;
        CHECK(throws<Failure>([&] { client.finish(lowOrder, keys); }) && !client.spent());
        CHECK(throws<Failure>([&] { client.finish(guess.answer(), keys); }) && client.spent());
        try {
            client.finish(sharerSide.answer(), keys);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_AUTHENTICATION &&
                  std::string(failure.what()).find("already tested") != std::string::npos);
        }
        ClientHandshake fresh(typed);
        const ServerHandshake answer(real, fresh.hello());
        CHECK(!fresh.finish(answer.answer(), keys).empty() && fresh.spent()); // The right PIN, first: in.
    }
    // The notice of a sharer without attempts left: an authentication failure
    // that says why; no version notice.
    try {
        checkPinLockedNotice(pinLockedNotice());
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_AUTHENTICATION &&
              std::string(failure.what()).find("no more PIN attempts") != std::string::npos);
    }
    CHECK(!otherProtocolVersion(pinLockedNotice()) && pinLockedNotice().size() == versionNotice().size());
    checkPinLockedNotice(versionNotice());
}

// A client that reaches a server of another version says so, with both
// versions.
void otherVersion() {
    static std::atomic<bool> never{false};
    const auto credentials = Credentials::fromKey(key);
    TcpListenSocket other("127.0.0.1", 47813);
    auto viewer = std::async(std::launch::async, [&] {
        return std::make_unique<SecureChannel>(TcpStream::connect("127.0.0.1", 47813, never, 1s),
                                               SecureChannel::Side::client, credentials);
    });
    auto stream = other.accept(never);
    crypto::Bytes hello(clientHelloSize);
    stream->read(hello, 2s);
    crypto::Bytes notice(versionNotice().begin(), versionNotice().end());
    notice.back() ^= 1;
    stream->write(notice, 1s);
    try {
        const auto channel = viewer.get();
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_UNSUPPORTED);
        CHECK(std::string(failure.what()).find("version " + std::to_string(notice.back())) != std::string::npos);
    }
}

// Timeouts bound the time without progress, not a whole transfer: a record
// that trickles in over a second arrives with a 300 ms timeout; silence fails
// once that passed.
void inactivityTimeouts() {
    static std::atomic<bool> never{false};
    TcpListenSocket listener("127.0.0.1", 47814);
    auto writer = TcpStream::connect("127.0.0.1", 47814, never, 1s);
    auto reader = listener.accept(never);
    auto trickle = std::async(std::launch::async, [&] {
        for (uint8_t i = 0; i < 10; ++i) {
            writer->write(crypto::Bytes{i}, 1s);
            std::this_thread::sleep_for(100ms);
        }
    });
    crypto::Bytes record(10);
    const auto started = Clock::now();
    reader->read(record, 300ms);
    CHECK(Clock::now() - started >= 800ms && record[9] == 9);
    trickle.get();
    const auto waited = Clock::now();
    CHECK(throws<Failure>([&] { reader->read(record, 300ms); }));
    CHECK(Clock::now() - waited >= 300ms && Clock::now() - waited < 1s);
}

// A stream whose next write breaks off after a few bytes, as a socket that
// fails part-way does; it keeps what was written.
class BreakingStream final : public Stream {
public:
    void read(std::span<uint8_t>, std::chrono::milliseconds) override { fail(RCS_ERROR_NETWORK, "Not read"); }
    void write(std::span<const uint8_t> buffer, std::chrono::milliseconds) override {
        if (breakNext) {
            breakNext = false;
            written.insert(written.end(), buffer.begin(), buffer.begin() + 5);
            fail(RCS_ERROR_NETWORK, "Cut short");
        }
        written.insert(written.end(), buffer.begin(), buffer.end());
    }
    void writeFinal(std::span<const uint8_t> buffer, std::chrono::milliseconds timeout) override {
        write(buffer, timeout);
    }
    uint64_t bytesTransferred() const noexcept override { return written.size(); }

    bool breakNext = false;
    crypto::Bytes written;
};

// A record cut short leaves its sequence number unused: nothing may follow it
// (another record would be sealed with the same sequence number and nonce),
// whichever thread sends next; the last words are left out.
void brokenRecords() {
    auto owned = std::make_unique<BreakingStream>();
    auto& stream = *owned;
    TrafficKeys keys;
    keys.send = std::make_unique<crypto::Aead>(crypto::Key{1}, crypto::Key{2});
    keys.receive = std::make_unique<crypto::Aead>(crypto::Key{3}, crypto::Key{4});
    SecureChannel channel(std::move(owned), std::move(keys));
    const crypto::Bytes payload{1, 2, 3};
    channel.send(payload);
    const size_t whole = stream.written.size();
    stream.breakNext = true;
    CHECK(throws<Failure>([&] { channel.send(payload); }));
    CHECK(stream.written.size() == whole + 5);
    try {
        channel.send(payload);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_NETWORK);
    }
    channel.sendFinal(payload);
    CHECK(stream.written.size() == whole + 5);
}

int main() {
    return runTest("secure_channel", [] {
        handshakes();
        pins();
        otherVersion();
        inactivityTimeouts();
        brokenRecords();
        std::atomic<bool> cancelled{false};
        {
            auto server = std::async(std::launch::async, serve, key, std::cref(cancelled));
            auto client = join(key, cancelled);
            auto host = server.get();
            const crypto::Bytes tiny{1, 2, 3};
            crypto::Bytes large(100'000);
            for (size_t i = 0; i < large.size(); ++i) {
                large[i] = uint8_t(i * 7);
            }
            client->send(tiny);
            client->send(large);
            host->send(tiny);
            CHECK(host->receive() == tiny);
            CHECK(host->receive() == large);
            CHECK(client->receive() == tiny);
            CHECK(client->bytesTransferred() > large.size());

            // Records hold any encoded picture: more than the largest stream's
            // uncompressed NV12 picture (8192x4352, 53.5 MB) passes, and the
            // sender refuses what the receiver would.
            crypto::Bytes picture(8192 * 4352 * 3 / 2 + 4096, 0x5a);
            auto received = std::async(std::launch::async, [&] { return host->receive(); });
            client->send(picture);
            CHECK(received.get() == picture);
            crypto::Bytes oversized(transport::maxRecord + 1);
            CHECK(throws<Failure>([&] { client->send(oversized); }));
            client->send(tiny); // The channel is still usable.
            CHECK(host->receive() == tiny);
        }
        {
            // A send cancelled before its first byte leaves the channel as it was
            // (the sequence number is not used up); the last words go out although
            // the run is cancelled, and follow what was sent before them.
            std::atomic<bool> serverStop{false}, clientStop{false};
            auto server = std::async(std::launch::async, serve, key, std::cref(serverStop));
            auto client = join(key, clientStop);
            auto host = server.get();
            const crypto::Bytes first{1, 2, 3}, last{9, 9};
            client->send(first);
            CHECK(host->receive() == first);
            clientStop = true;
            try {
                client->send(first);
                CHECK(false);
            } catch (const Failure& failure) {
                CHECK(failure.result() == RCS_ERROR_CANCELLED);
            }
            client->sendFinal(last);
            CHECK(host->receive() == last);
        }
        {
            // A different key: the client rejects the server's proof and hangs
            // up, which is all the server can observe.
            auto wrong = key;
            wrong[31] = 9;
            auto server = std::async(std::launch::async, serve, key, std::cref(cancelled));
            auto client = std::async(std::launch::async, join, wrong, std::cref(cancelled));
            CHECK(resultOf(client) == RCS_ERROR_AUTHENTICATION);
            CHECK(resultOf(server) == RCS_ERROR_DISCONNECTED);
        }
        {
            // A second listener cannot take over a port that is in use.
            std::atomic<bool> stop{false};
            TcpListenSocket first("127.0.0.1", port);
            CHECK(throws<Failure>([&] { TcpListenSocket second("127.0.0.1", port); }));
            auto waiting = std::async(std::launch::async, [&] { return first.accept(stop); });
            std::this_thread::sleep_for(100ms);
            stop = true;
            CHECK(throws<Failure>([&] { const auto stream = waiting.get(); })); // Cancelled.
        }
        CHECK(throws<Failure>([&] { TcpStream::connect("not-an-address", port, cancelled, 1s); }));
    });
}
