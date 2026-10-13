// TURN over TLS end to end: two sessions relay through the tests' TURN server
// behind a TLS terminator (support/tls_terminator.py) with a self-signed
// certificate. The right pin connects and streams; a wrong pin, and the
// system's trust (which a self-signed certificate never passes), fail with
// RCS_ERROR_AUTHENTICATION; a pin changed while running applies from the next
// connection.
//
//   rcsdk_test_tls_relay --python P --openssl O --script S
//       On this machine (ctest; not on Windows): makes the certificate with O
//       and runs the terminator with P over TLS 1.2 and over what P offers
//       (TLS 1.3 where its ssl module has it, and the SDK's TLS: not BearSSL).
//       Skips (77) without P or O.
//   rcsdk_test_tls_relay --serve ADDRESS [--port N] [--python P --openssl O --script S]
//       Serves TURN over TLS on ADDRESS:N (default 5349) for another machine
//       until its input ends (Ctrl+D), and prints the line to run there.
//   rcsdk_test_tls_relay --remote HOST:PORT --pin HEX
//       The checks against such a server, e.g. on Windows against a Mac.
#include "support/check.hpp"
#include "support/test_pattern.hpp"
#include "support/test_tone.hpp"
#include "support/tls_terminator.hpp"
#include "support/turn_server.hpp"

#include "util/bytes.hpp"

#include <rcsdk/rcsdk.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr int skipped = 77; // ctest's SKIP_RETURN_CODE.
constexpr char turnUser[] = "rcsdk", turnPassword[] = "tls-test", serverName[] = "turn.rcsdk.test";

struct Log {
    std::mutex mutex;
    std::vector<std::string> lines;
    bool contains(const std::string& text) {
        std::lock_guard lock(mutex);
        return std::any_of(lines.begin(), lines.end(), [&](const auto& line) { return line.find(text) != line.npos; });
    }
    void clear() {
        std::lock_guard lock(mutex);
        lines.clear();
    }
};

Log& sdkLog() {
    static Log log;
    return log;
}

template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

rcs_acceleration testAcceleration() {
    const auto caps = rcsdk::capabilities();
    const uint32_t h264 = 1u << RCS_CODEC_H264;
    return (caps.hardware_encoders & caps.hardware_decoders & h264) ? RCS_ACCELERATION_HARDWARE
                                                                    : RCS_ACCELERATION_SOFTWARE;
}

bool h264Here() {
    const auto caps = rcsdk::capabilities();
    const uint32_t h264 = 1u << RCS_CODEC_H264;
    return (caps.hardware_encoders & caps.hardware_decoders & h264) ||
           (caps.software_encoders & caps.software_decoders & h264);
}

std::optional<rcsdk::CertificateSha256> parsePin(const std::string& text) {
    rcsdk::CertificateSha256 pin{};
    if (text.size() != 64 || text.find_first_not_of("0123456789abcdefABCDEF") != text.npos) {
        return std::nullopt;
    }
    for (size_t i = 0; i < pin.size(); ++i) {
        pin[i] = uint8_t(std::stoul(text.substr(2 * i, 2), nullptr, 16));
    }
    return pin;
}

// One end relaying over TLS: pinned to `pin`, or (none) by the system's trust
// and the name `name`.
rcsdk::SessionConfig tlsConfig(rcs_role role, const uint8_t (&key)[32], const std::string& server,
                               std::optional<rcsdk::CertificateSha256> pin, const std::string& name = {}) {
    rcsdk::SessionConfig c(role);
    c.setKey(key).setCodecs({RCS_CODEC_H264}).setAcceleration(testAcceleration());
    c.setRelay(server, turnUser, turnPassword, RCS_TURN_TLS).setKeepListening(false).setReconnect(false);
    if (pin) {
        c.setRelayCertificate(*pin);
    }
    if (!name.empty()) {
        c.setRelayServerName(name);
    }
    if (role == RCS_ROLE_SHARE) {
        c.setVideo(640, 360, 30, 2'000'000);
    } else {
        c.setAudioPlayback(false);
    }
    return c;
}

// A sharer and a viewer that hand each other their signals.
struct Pair {
    rcsdk::Session* share = nullptr;
    rcsdk::Session* view = nullptr;
    std::atomic<int> frames{0};
    rcsdk::Callbacks shareCallbacks() {
        rcsdk::Callbacks callbacks;
        callbacks.onSignal = [this](const std::string& text, uint64_t) {
            view->signal(text);
        };
        return callbacks;
    }
    rcsdk::Callbacks viewCallbacks() {
        rcsdk::Callbacks callbacks;
        callbacks.onSignal = [this](const std::string& text, uint64_t) {
            share->signal(text, 1);
        };
        callbacks.onFrame = [this](const rcs_frame&) {
            ++frames;
        };
        return callbacks;
    }
};

// A viewer alone fails before it needs a sharer: with `result`, its reason
// saying `words`.
void refused(rcsdk::SessionConfig config, rcs_result result, const char* words) {
    rcsdk::Callbacks callbacks;
    callbacks.onSignal = [](const std::string&, uint64_t) {
    };
    rcsdk::Session view(config, callbacks);
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }, 10s));
    const auto failure = view.failure();
    std::printf("refused (%d): %s\n", int(failure.result), failure.message.c_str());
    CHECK(failure.result == result && failure.message.find(words) != std::string::npos);
}

// What the SDK logged about its TLS connections.
void printTlsLog() {
    auto& log = sdkLog();
    std::lock_guard lock(log.mutex);
    for (const auto& line : log.lines) {
        if (line.find("TLS 1.") != std::string::npos) {
            std::printf("logged: %s\n", line.c_str());
        }
    }
}

// The checks, against TURN over TLS at `server` whose certificate has the
// SHA-256 `pin`; `version` is what both ends must log ("TLS 1.3"; empty:
// whichever they agree on).
void checks(const uint8_t (&key)[32], const std::string& server, const rcsdk::CertificateSha256& pin,
            const std::string& version) {
    // The pinned certificate: the ends connect and stream through the relay.
    {
        sdkLog().clear();
        Pair pair;
        rcsdk::Session share(tlsConfig(RCS_ROLE_SHARE, key, server, pin), pair.shareCallbacks());
        rcsdk::Session view(tlsConfig(RCS_ROLE_VIEW, key, server, pin), pair.viewCallbacks());
        pair.share = &share;
        pair.view = &view;
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 10s));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return pair.frames >= 20; }, 15s));
        const std::string path = view.stats<rcs_network_stats>().path;
        std::printf("path: %s\n", path.c_str());
        printTlsLog();
        CHECK(path.find("relay") != std::string::npos && path.find("over TLS") != std::string::npos);
        CHECK(sdkLog().contains(" to the TURN server ") && sdkLog().contains(", the pinned certificate"));
        CHECK(version.empty() || sdkLog().contains(version + " to the TURN server"));
        CHECK(view.stop() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }, 5s));
    }
    // Another pin: refused, saying both.
    auto wrong = pin;
    wrong[31] ^= 1;
    refused(tlsConfig(RCS_ROLE_VIEW, key, server, wrong), RCS_ERROR_AUTHENTICATION, "turn_certificate_sha256");
    // The system's trust: a self-signed certificate leads to no authority.
    refused(tlsConfig(RCS_ROLE_VIEW, key, server, std::nullopt, serverName), RCS_ERROR_AUTHENTICATION,
            "does not lead to an authority");
    // A pin changed while the sharer runs: the stream goes on; once its viewer
    // left, the next viewer's attempt meets a certificate the sharer no
    // longer accepts, and with no viewer to serve the run ends.
    {
        Pair pair;
        auto shareConfig = tlsConfig(RCS_ROLE_SHARE, key, server, pin);
        shareConfig.setKeepListening(true);
        rcsdk::Session share(shareConfig, pair.shareCallbacks());
        rcsdk::Session view(tlsConfig(RCS_ROLE_VIEW, key, server, pin), pair.viewCallbacks());
        pair.share = &share;
        pair.view = &view;
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 10s));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return pair.frames >= 10; }, 15s));
        CHECK(share.update(shareConfig.setRelayCertificate(wrong).iceServers()) == RCS_OK);
        const int before = pair.frames;
        CHECK(waitFor([&] { return pair.frames >= before + 10; }, 5s));
        CHECK(view.stop() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 10s));
        rcsdk::Session next(tlsConfig(RCS_ROLE_VIEW, key, server, pin), pair.viewCallbacks());
        pair.view = &next;
        CHECK(next.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }, 15s));
        CHECK(next.stop() == RCS_OK);
        std::printf("sharer after the pin changed (%d): %s\n", int(share.failure().result),
                    share.failure().message.c_str());
        CHECK(share.failure().result == RCS_ERROR_AUTHENTICATION);
    }
}

#ifndef _WIN32
#ifdef RCSDK_CRYPTO_BEARSSL
constexpr bool tls13Here = false; // BearSSL speaks TLS 1.2 only.
#else
constexpr bool tls13Here = true;
#endif

using rcsdk::test::Terminator;
using rcsdk::test::Tools;

void makeCertificate(const Tools& tools) {
    rcsdk::test::makeCertificate(tools, "tls_relay", serverName);
}

int local(const Tools& tools) {
    if (!tools.available()) {
        std::printf("tls_relay: needs Python 3 (%s) and openssl (%s); skipped\n", tools.python.c_str(),
                    tools.openssl.c_str());
        return skipped;
    }
    makeCertificate(tools);
    return runTest("tls_relay", [&] {
        rcsdk::test::TurnServer turn(turnUser, turnPassword);
        uint8_t key[32];
        CHECK(rcs_generate_connection_key(key, 32) == RCS_OK);
        {
            Terminator tls(tools, turn.tcpAddress(), "127.0.0.1", 0, "any", "tls_relay");
            const std::string server = "127.0.0.1:" + std::to_string(tls.port);
            const uint64_t relayed = turn.relayed;
            checks(key, server, tls.pin, tls.tls13 && tls13Here ? "TLS 1.3" : "TLS 1.2");
            CHECK(turn.relayed - relayed > 40 && tls.child.printed("alpn stun.turn"));
            if (!tls.tls13) {
                std::printf("tls_relay: this Python's ssl module has no TLS 1.3; only TLS 1.2 was checked\n");
            }
        }
        // A server that speaks no TLS: a network failure, not a refused certificate.
        auto pin = rcsdk::CertificateSha256{};
        pin[0] = 1;
        refused(tlsConfig(RCS_ROLE_VIEW, key, turn.tcpAddress().toString(), pin), RCS_ERROR_NETWORK, "TLS");
        // TLS 1.2 on its own (where 1.3 is the default, the line above took 1.3).
        Terminator tls12(tools, turn.tcpAddress(), "127.0.0.1", 0, "1.2", "tls_relay");
        const std::string server = "127.0.0.1:" + std::to_string(tls12.port);
        sdkLog().clear();
        Pair pair;
        rcsdk::Session share(tlsConfig(RCS_ROLE_SHARE, key, server, tls12.pin), pair.shareCallbacks());
        rcsdk::Session view(tlsConfig(RCS_ROLE_VIEW, key, server, tls12.pin), pair.viewCallbacks());
        pair.share = &share;
        pair.view = &view;
        CHECK(share.start() == RCS_OK && waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 10s));
        CHECK(view.start() == RCS_OK && waitFor([&] { return pair.frames >= 20; }, 15s));
        printTlsLog();
        CHECK(sdkLog().contains("TLS 1.2 to the TURN server"));
        CHECK(view.stop() == RCS_OK);
    });
}

int serve(const Tools& tools, const std::string& address, uint16_t port) {
    makeCertificate(tools);
    // Only the TLS port faces the network: the TURN server behind it and its
    // relays (both ends' relays are its own) stay on loopback, where a
    // firewall for this unsigned program does not come in.
    rcsdk::test::TurnServer turn(turnUser, turnPassword, "127.0.0.1");
    Terminator tls(tools, turn.tcpAddress(), address, port, "any", "tls_relay");
    std::printf("TURN over TLS on %s:%u (TLS 1.3 %s), relays on %s\n", address.c_str(), unsigned(tls.port),
                tls.tls13 ? "offered" : "not offered: this Python's ssl module has none", turn.addressText().c_str());
    std::printf("On the other machine:\n  rcsdk_test_tls_relay --remote %s:%u --pin %s\n", address.c_str(),
                unsigned(tls.port), rcsdk::hex(tls.pin).c_str());
    std::printf("Serving until this input ends (Ctrl+D)...\n");
    std::fflush(stdout);
    for (std::string line; std::getline(std::cin, line);) {
    }
    std::printf("relayed %llu datagrams, %d allocations\n", (unsigned long long)turn.relayed.load(),
                turn.allocations.load());
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> options;
    for (int i = 1; i + 1 < argc; i += 2) {
        options[argv[i]] = argv[i + 1];
    }
    CHECK(rcsdk::setLogCallback(RCS_LOG_DEBUG, [](const rcs_log_record& record) {
              auto& log = sdkLog();
              std::lock_guard lock(log.mutex);
              log.lines.push_back(std::string(record.module) + ": " + record.message);
          }) == RCS_OK);
    if (!h264Here()) {
        std::printf("tls_relay: no H.264 encoder and decoder with one acceleration here; skipped\n");
        return skipped;
    }
    rcsdk::test::UseTestPattern pattern;
    rcsdk::test::UseTestTone tone;
    rcsdk::test::UseSilentPlayer silent;
    int result = 0;
    if (options.count("--remote")) {
        const auto pin = parsePin(options["--pin"]);
        CHECK(pin);
        result = runTest("tls_relay (remote)", [&] {
            uint8_t key[32];
            CHECK(rcs_generate_connection_key(key, 32) == RCS_OK);
            checks(key, options["--remote"], *pin, "");
        });
    } else {
#ifdef _WIN32
        std::printf("tls_relay: runs here with --remote HOST:PORT --pin HEX against a Mac's --serve; skipped\n");
        result = skipped;
#else
        Tools tools;
        tools.python = options.count("--python") ? options["--python"] : tools.python;
        tools.openssl = options.count("--openssl") ? options["--openssl"] : tools.openssl;
        tools.script = options.count("--script") ? options["--script"] : "tls_terminator.py";
        result = options.count("--serve")
                     ? serve(tools, options["--serve"],
                             uint16_t(std::stoul(options.count("--port") ? options["--port"] : "5349")))
                     : local(tools);
#endif
    }
    rcsdk::setLogCallback(RCS_LOG_DEBUG, {});
    return result;
}
