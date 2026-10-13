// A forwarder behind a TLS-terminating proxy, wss://: the tests' terminator
// (support/tls_terminator.py) with a self-signed certificate in front of the
// forwarder's WebSocket entry, as nginx or Caddy would be. A viewer over
// wss:// pinned to the certificate watches a sharer over UDP, the terminator
// having agreed to HTTP/1.1; another pin fails with AUTHENTICATION before
// the forwarder hears of it.
//
//   rcsdk_test_forwarder_tls --python P --openssl O --script S
//       Skips (77) without P or O, and on Windows (where it runs by hand
//       against a Mac's proxy).
#include "../server/forwarder/src/forwarder.hpp"
#include "../server/forwarder/src/token.hpp"
#include "support/check.hpp"
#include "support/test_pattern.hpp"
#include "support/test_tone.hpp"
#include "support/tls_terminator.hpp"

#include <rcsdk/rcsdk.hpp>

#include <map>
#include <string>

namespace {

using namespace std::chrono_literals;

constexpr int skipped = 77; // ctest's SKIP_RETURN_CODE.

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

rcsdk::SessionConfig endConfig(rcs_role role, const uint8_t (&key)[32]) {
    rcsdk::SessionConfig c(role);
    c.setKey(key).setCodecs({RCS_CODEC_H264}).setAcceleration(testAcceleration());
    c.setKeepListening(false).setReconnect(false);
    if (role == RCS_ROLE_SHARE) {
        c.setVideo(640, 360, 30, 2'000'000);
    } else {
        c.setAudioPlayback(false);
    }
    return c;
}

#ifndef _WIN32
int local(const rcsdk::test::Tools& tools) {
    if (!tools.available()) {
        std::printf("forwarder_tls: needs Python 3 (%s) and openssl (%s); skipped\n", tools.python.c_str(),
                    tools.openssl.c_str());
        return skipped;
    }
    rcsdk::test::makeCertificate(tools, "forwarder_tls", "forwarder.rcsdk.test");
    return runTest("forwarder_tls", [&] {
        rcsdk::test::UseTestPattern pattern;
        rcsdk::test::UseTestTone tone;
        rcsdk::test::UseSilentPlayer silent;
        rcsdk::forwarder::Settings s;
        const auto loopback = rcsdk::transport::Endpoint::parse("127.0.0.1", 0);
        s.listenUdp = s.listenWebSocket = loopback;
        s.identity = rcsdk::crypto::DeviceKey::generate();
        rcsfwd_secret secret{1, {}};
        rcsdk::crypto::random(secret.secret);
        s.secrets = {secret};
        const rcsdk::crypto::DeviceKey pair(s.identity);
        const auto audience = rcsdk::crypto::sha256(pair.publicKey());
        rcsdk::forwarder::Forwarder forwarder(std::move(s));
        forwarder.start();
        const auto token = [&](rcsfwd_role role, uint64_t route) {
            return rcsdk::forwarder::sign(secret,
                                          {"secure", role, route, rcsdk::forwarder::unixNow() + 600, 0, audience});
        };
        const rcsdk::transport::Endpoint entry =
            rcsdk::transport::Endpoint::parse("127.0.0.1", forwarder.port(RCSFWD_ENTRY_WEBSOCKET));
        rcsdk::test::Terminator tls(tools, entry, "127.0.0.1", 0, "any", "forwarder_tls", "http/1.1");
        const std::string url = "wss://127.0.0.1:" + std::to_string(tls.port) + "/rcs";
        uint8_t key[32];
        CHECK(rcs_generate_connection_key(key, 32) == RCS_OK);

        rcsdk::Session share(
            endConfig(RCS_ROLE_SHARE, key)
                .setForwarder("127.0.0.1", forwarder.port(RCSFWD_ENTRY_UDP), audience, token(RCSFWD_ROLE_PUBLISH, 0))
                .setViewers(2));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 10s));
        std::atomic<int> frames{0};
        rcsdk::Callbacks callbacks;
        callbacks.onFrame = [&](const rcs_frame&) {
            ++frames;
        };
        rcsdk::Session view(endConfig(RCS_ROLE_VIEW, key)
                                .setForwarderWebSocket(url, audience, token(RCSFWD_ROLE_SUBSCRIBE, 1), tls.pin),
                            callbacks);
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 20; }, 15s));
        const std::string path = view.stats<rcs_network_stats>().path;
        std::printf("forwarder_tls: %s\n", path.c_str());
        CHECK(path.find(url) != std::string::npos);
        CHECK(view.stats<rcs_forwarder_stats>().link == RCS_FORWARDER_WEBSOCKET);
        CHECK(tls.child.printed("alpn http/1.1"));
        const auto viewers = forwarder.viewers("secure");
        CHECK(viewers.size() == 1 && viewers[0].entry == RCSFWD_ENTRY_WEBSOCKET);
        CHECK(view.stop() == RCS_OK);

        // Another pin: refused before the forwarder hears of it.
        auto wrong = tls.pin;
        wrong[0] ^= 1;
        rcsdk::Session astray(
            endConfig(RCS_ROLE_VIEW, key).setForwarderWebSocket(url, audience, token(RCSFWD_ROLE_SUBSCRIBE, 2), wrong));
        CHECK(astray.start() == RCS_OK);
        CHECK(waitFor([&] { return astray.state() == RCS_STATE_FAILED; }, 10s));
        std::printf("forwarder_tls: another pin (%d): %s\n", int(astray.failure().result),
                    astray.failure().message.c_str());
        CHECK(astray.failure().result == RCS_ERROR_AUTHENTICATION &&
              astray.failure().message.find("tls_certificate_sha256") != std::string::npos);
        CHECK(share.stop() == RCS_OK);
    });
}
#endif

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> options;
    for (int i = 1; i + 1 < argc; i += 2) {
        options[argv[i]] = argv[i + 1];
    }
    if (!h264Here()) {
        std::printf("forwarder_tls: no H.264 encoder and decoder with one acceleration here; skipped\n");
        return skipped;
    }
#ifdef _WIN32
    (void)options;
    std::printf("forwarder_tls: runs on a Mac (a TLS proxy of Python's); skipped\n");
    return skipped;
#else
    rcsdk::test::Tools tools;
    tools.python = options.count("--python") ? options["--python"] : tools.python;
    tools.openssl = options.count("--openssl") ? options["--openssl"] : tools.openssl;
    tools.script = options.count("--script") ? options["--script"] : tools.script;
    return local(tools);
#endif
}
