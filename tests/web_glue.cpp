// The browser package's options (web/glue.cpp's rcsw_create), apart from a
// page: the JSON rcsdk.mjs passes makes the session it says. Through a
// forwarder (its key's SHA-256 in hex, the token, refreshToken): FORWARDER
// over the page's channel (RCS_FORWARDER_HOST, datagrams), no address, the
// page asked for new tokens; a SHA-256 that is not 32 bytes in hex or a
// token too long refused, saying so. Without it: HOST, as before.
// WebAssembly only: the glue is the browser's.
#include "../web/glue.cpp"
#include "support/check.hpp"

namespace {

const uint8_t key[32] = {7, 7, 7};

int create(const std::string& json) {
    return rcsw_create(json.c_str(), key, sizeof(key));
}

// The session `id` made (none: the check failed already), checked by `check` and destroyed.
template <class Check>
void with(int id, Check check) {
    std::shared_ptr<WebSession> s;
    {
        std::lock_guard lock(sessionsMutex);
        if (!sessions.count(id)) {
            return;
        }
        s = sessions.at(id);
        sessions.erase(id);
    }
    check(*s);
    CHECK(rcs_session_destroy(s->session) == RCS_OK);
}

} // namespace

int main() {
    return runTest("web_glue", [] {
        const std::string sha(64, 'a'), token = "rcsf1.payload.signature";
        const auto through = [&](const std::string& keySha256, const std::string& tokenText) {
            return create(R"({"identity":"key","channel":"datagram","forwarder":{"keySha256":")" + keySha256 +
                          R"(","token":")" + tokenText + R"(","refresh":true}})");
        };
        const int id = through(sha, token);
        CHECK(id > 0);
        with(id, [&](WebSession& s) {
            CHECK(s.forwarder);
            rcs_forwarder_config f = RCS_FORWARDER_CONFIG_INIT;
            rcs_host_transport_config channel = RCS_HOST_TRANSPORT_CONFIG_INIT;
            CHECK(rcs_session_get_settings(s.session, &f.header) == RCS_OK);
            CHECK(rcs_session_get_settings(s.session, &channel.header) == RCS_OK);
            CHECK(f.link == RCS_FORWARDER_HOST && std::string(f.token) == token && f.refresh_token &&
                  f.user_data == &s && f.forwarder_key_sha256[0] == 0xaa && f.forwarder_key_sha256[31] == 0xaa);
            CHECK(channel.host_channel == RCS_HOST_CHANNEL_DATAGRAM && channel.on_transport_send);
            rcs_forwarder_stats stats = RCS_FORWARDER_STATS_INIT;
            CHECK(rcs_session_get_stats(s.session, RCS_STATS_FORWARDER, &stats) == RCS_OK);
        });
        CHECK(through("aa", token) == RCS_ERROR_INVALID_ARGUMENT && lastError().find("keySha256") != std::string::npos);
        CHECK(through(sha, std::string(RCS_FORWARDER_TOKEN_SIZE, 't')) == RCS_ERROR_INVALID_ARGUMENT &&
              lastError().find("forwarder.token") != std::string::npos);
        CHECK(through(sha, "") == RCS_ERROR_INVALID_ARGUMENT && lastError().find("token") != std::string::npos);
        // Straight to the sharer over the page's channel.
        const int host = create(R"({"identity":"key"})");
        CHECK(host > 0);
        with(host, [](WebSession& s) {
            rcs_forwarder_stats stats = RCS_FORWARDER_STATS_INIT;
            CHECK(!s.forwarder && rcs_session_get_stats(s.session, RCS_STATS_FORWARDER, &stats) ==
                                      RCS_ERROR_UNSUPPORTED);
        });
    });
}
