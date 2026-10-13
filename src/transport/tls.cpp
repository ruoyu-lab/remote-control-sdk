#include "transport/tls.hpp"

#include "crypto/crypto.hpp"
#include "transport/ice.hpp"
#include "util/bytes.hpp"

#include <algorithm>

namespace rcsdk::transport {

TlsSettings tlsSettings(const rcs_ice_servers_config& config) {
    TlsSettings tls;
    tls.serverName = config.turn_tls_server_name;
    if (tls.serverName.empty()) {
        const auto server = ice::splitHostPort(config.turn_server, turnTlsPort);
        if (server && !Endpoint::tryParse(server->host, 0)) {
            tls.serverName = server->host;
        }
    }
    const auto& pin = config.turn_certificate_sha256;
    if (std::any_of(std::begin(pin), std::end(pin), [](uint8_t b) { return b != 0; })) {
        tls.certificateSha256.emplace();
        std::copy(std::begin(pin), std::end(pin), tls.certificateSha256->begin());
    }
    return tls;
}

std::string pinProblem(std::span<const uint8_t> certificate, const std::array<uint8_t, 32>& pin,
                       const std::string& what, const std::string& setting) {
    const auto presented = crypto::sha256(certificate);
    if (crypto::equal(presented, pin)) {
        return {};
    }
    return "The certificate of " + what + " has the SHA-256 " + hex(presented) + ", not the pinned " + setting + " " +
           hex(pin) + " (a renewed certificate has a new one; so does that of a proxy that inspects TLS)";
}

std::string trustProblem(TrustProblem problem, const TlsSettings& settings, const std::string& what,
                         const std::string& detail) {
    std::string reason = "The certificate of " + what;
    switch (problem) {
    case TrustProblem::untrusted:
        reason += " does not lead to an authority this system trusts (self-signed, or the server does not send "
                  "its intermediate certificates); pin it with " +
                  settings.pinSetting;
        break;
    case TrustProblem::name:
        reason += " is not for \"" + settings.serverName + "\" (" + settings.nameSetting + ")";
        break;
    case TrustProblem::dates:
        reason += " has expired or is not valid yet (check this machine's clock)";
        break;
    case TrustProblem::other:
        reason += " was refused by the system";
        break;
    case TrustProblem::use:
        reason += " is not a TLS server's certificate: it must be for server authentication where it says what it "
                  "is for, and name the server among its subject alternative names";
        break;
    }
    return reason + " (" + detail + ")";
}

#if !defined(__APPLE__) && !defined(_WIN32) && !defined(RCSDK_CRYPTO_BEARSSL)
std::unique_ptr<ByteStream> connectTls(const Endpoint&, const TlsSettings&, const std::string&, CancelToken,
                                       std::chrono::milliseconds, uint32_t) {
    fail(RCS_ERROR_UNSUPPORTED, "TLS is not available on this platform");
}
#endif

} // namespace rcsdk::transport
