// The handshake findings of docs/SECURITY-ASSESSMENT.md, over loopback only:
// first demonstrations of what the code did, now regression tests of the
// fixed behaviour.
#include "protocol/udp_connection.hpp"
#include "support/check.hpp"
#include "transport/handshake.hpp"
#include "util/bytes.hpp"

#include <atomic>
#include <cstdio>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace rcsdk;
using namespace rcsdk::protocol;
using transport::Endpoint;
using transport::UdpSocket;

namespace {

crypto::Bytes framed(uint8_t kind, std::span<const uint8_t> body) {
    crypto::Bytes datagram{kind};
    datagram.insert(datagram.end(), body.begin(), body.end());
    return datagram;
}

// A peer made by hand on loopback.
struct RawEnd {
    UdpSocket socket;
    explicit RawEnd(uint16_t port = 0) : socket(Endpoint::parse("127.0.0.1", port)) {}
    // The next datagram of `kind` within `wait`, with its sender; empty when none came.
    crypto::Bytes await(uint8_t kind, Endpoint* sender = nullptr, Clock::duration wait = 3s) {
        std::array<uint8_t, 2048> buffer;
        const auto deadline = Clock::now() + wait;
        while (Clock::now() < deadline) {
            Endpoint from;
            if (const auto size = socket.receive(buffer, from)) {
                if (*size && buffer[0] == kind) {
                    if (sender) {
                        *sender = from;
                    }
                    return crypto::Bytes(buffer.begin(), buffer.begin() + ptrdiff_t(*size));
                }
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
        return {};
    }
};

transport::Credentials pinCredentials(const std::string& pin, uint32_t attempts,
                                      transport::PinAttempts::Report report = nullptr) {
    transport::Credentials c;
    c.secret = RCS_SECRET_PIN;
    c.password = pin;
    c.pinAttempts = std::make_shared<transport::PinAttempts>(attempts, std::move(report));
    return c;
}

// SA-01: a viewer with a PIN tests one answer per hello. Whoever answers in
// the sharer's place (someone on the path, the TURN or host-channel operator,
// a sender of spoofed datagrams) gets one guess: the viewer fails with the
// first answer whose PIN is wrong and tries none after it, the one with the
// right PIN among them.
void viewerTestsOnePinGuess() {
    const std::string pin = "7351";
    const auto viewerCredentials = pinCredentials(pin, 5);
    RawEnd sharer(47921);
    std::atomic<bool> never{false};
    auto viewer = std::async(std::launch::async, [&] {
        return UdpConnection::establish(UdpConnection::Side::client,
                                        std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)),
                                        directLink(sharer.socket.local()), viewerCredentials, {}, never);
    });
    Endpoint from;
    const auto hello = sharer.await(0xD1, &from);
    CHECK(hello.size() == 1 + transport::serverHelloSize); // As large as an answer (SA-09).
    const auto clientHello = std::span(hello).subspan(1, transport::clientHelloSize);
    // The impostor prepares one answer per guess (it does not know the PIN):
    // every 4-digit PIN from 7000 up, the right one 351st.
    constexpr int guesses = 400;
    std::vector<transport::Credentials> guessed;
    guessed.reserve(guesses);
    std::vector<std::unique_ptr<transport::ServerHandshake>> answers;
    for (int i = 0; i < guesses; ++i) {
        char text[8];
        std::snprintf(text, sizeof(text), "%04d", 7000 + i);
        guessed.push_back(pinCredentials(text, 1));
        answers.push_back(std::make_unique<transport::ServerHandshake>(guessed.back(), clientHello));
    }
    const auto started = Clock::now();
    for (int i = 0; i < guesses; ++i) {
        sharer.socket.send(from, framed(0xD2, answers[size_t(i)]->answer()));
        if (i % 50 == 49) {
            std::this_thread::sleep_for(2ms); // Stay within the socket's buffer.
        }
    }
    std::string reason;
    try {
        const auto connection = viewer.get();
        CHECK(false);
    } catch (const Failure& failure) {
        reason = failure.what();
        CHECK(failure.result() == RCS_ERROR_AUTHENTICATION && reason == "Wrong PIN or another secret");
    }
    const auto took = Clock::now() - started;
    CHECK(sharer.await(0xD3, nullptr, 500ms).empty()); // No finished message: no guess confirmed.
    std::printf("SA-01: the viewer tested the first of %d forged answers and failed after %lld ms: \"%s\"\n", guesses,
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(took).count(), reason.c_str());
}

// SA-04: a sharer answers a PIN hello only from an address that proved it
// receives there (the Retry's cookie), so hellos from forged source addresses
// take none of the PIN's attempts: strangers that never take the cookie get
// it and nothing else, and the viewer with the right PIN gets in. A stranger
// that proves its address and leaves without finishing costs an attempt (the
// answer let it test a guess).
void pinHellosProveAddress() {
    std::atomic<uint32_t> failures{0};
    const auto sharerCredentials =
        pinCredentials("246810", 5, [&](uint32_t total, uint32_t) { failures = total; });
    const auto local = Endpoint::parse("127.0.0.1", 47922);
    std::atomic<bool> stop{false};
    UdpOptions options;
    options.handshakeTimeout = 1000ms;
    auto sharer = std::async(std::launch::async, [&] {
        return UdpConnection::establish(UdpConnection::Side::server, std::make_unique<UdpSocket>(local), nullptr,
                                        transport::fixedCredentials(sharerCredentials), options, stop);
    });
    std::this_thread::sleep_for(50ms);
    const auto anyPin = pinCredentials("0000", 1);
    for (int i = 0; i < 6; ++i) {
        RawEnd stranger;
        const transport::ClientHandshake handshake(anyPin);
        stranger.socket.send(local, helloDatagram(handshake.hello()));
        CHECK(stranger.await(0xD4).size() == 1 + 16);              // A cookie...
        CHECK(stranger.await(0xD2, nullptr, 200ms).empty());       // ...and no answer.
    }
    RawEnd proven;
    {
        const transport::ClientHandshake handshake(anyPin);
        proven.socket.send(local, helloDatagram(handshake.hello()));
        const auto retry = proven.await(0xD4);
        proven.socket.send(local, helloDatagram(handshake.hello(), std::span(retry).subspan(1)));
        CHECK(proven.await(0xD2).size() == 1 + transport::serverHelloSize);
    }
    const auto viewerCredentials = pinCredentials("246810", 5);
    const auto viewer =
        UdpConnection::establish(UdpConnection::Side::client, std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)),
                                 directLink(local), viewerCredentials, {}, stop);
    CHECK(viewer != nullptr && sharer.get() != nullptr);
    CHECK(failures == 1); // The stranger that proved its address, once its attempt ended.
    std::printf("SA-04: six PIN hellos without the cookie got a Retry each and took no attempt; the viewer got in\n");
}

// SA-09: what a sharer answers a hello with is no larger than the hello (it
// is padded to the ServerHello's size), so copies of one sent with a
// victim's address reflect nothing more; a hello of the old, smaller size
// gets no answer.
void helloAnswersReflectNothing() {
    const transport::Credentials sharerCredentials = transport::Credentials::fromKey(std::array<uint8_t, 32>{9, 9, 9});
    const auto local = Endpoint::parse("127.0.0.1", 47923);
    std::atomic<bool> stop{false};
    auto sharer = std::async(std::launch::async, [&] {
        try {
            return UdpConnection::establish(UdpConnection::Side::server, std::make_unique<UdpSocket>(local), nullptr,
                                            transport::fixedCredentials(sharerCredentials), {}, stop);
        } catch (const Failure&) {
            return std::unique_ptr<UdpConnection>();
        }
    });
    std::this_thread::sleep_for(50ms);
    RawEnd sender;
    const transport::ClientHandshake client(sharerCredentials);
    const auto hello = helloDatagram(client.hello());
    size_t sent = 0, answered = 0, answerBytes = 0;
    for (int i = 0; i < 100; ++i) {
        sender.socket.send(local, hello);
        sent += hello.size();
        const auto answer = sender.await(0xD2, nullptr, 500ms);
        if (!answer.empty()) {
            ++answered;
            answerBytes += answer.size();
        }
    }
    CHECK(answered == 100);
    const double ratio = double(answerBytes) / double(sent);
    CHECK(ratio <= 1.0);
    RawEnd unpadded;
    unpadded.socket.send(local, framed(0xD1, transport::ClientHandshake(sharerCredentials).hello()));
    CHECK(unpadded.await(0xD2, nullptr, 300ms).empty());
    std::printf("SA-09: 100 copies of one %zu-byte hello drew 100 answers of %zu bytes (%.2fx)\n", hello.size(),
                answerBytes / answered, ratio);
    stop = true;
    CHECK(!sharer.get()); // Stopped, never connected.
}

} // namespace

int main() {
    return runTest("security", [] {
        viewerTestsOnePinGuess();
        pinHellosProveAddress();
        helloAnswersReflectNothing();
    });
}
