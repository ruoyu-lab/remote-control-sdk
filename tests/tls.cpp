// How a TURN server's TLS certificate is checked: the pin, the wording of the
// system's verdicts, the server's address and default port, and (Apple
// systems, BearSSL) the check itself, on a chain of the test's own authority
// at dates of its choice; with BearSSL also the system's authorities, read
// from its files. The connection end to end: tls_relay.cpp.
#include "support/check.hpp"

#include "crypto/crypto.hpp"
#include "transport/ice.hpp"
#include "transport/tls.hpp"
#include "util/bytes.hpp"

#ifdef RCSDK_CRYPTO_BEARSSL
#include "transport/tls_bearssl.hpp"
#elif defined(__APPLE__)
#include "platform/apple/tls_connection.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace rcsdk;
using namespace rcsdk::transport;

namespace {

// An authority, and the certificate it issued for turn.rcsdk.test, valid
// 2026-01-01 to 2027-12-01 (P-256, server authentication), as base64 DER.
constexpr char authorityBase64[] = "MIIBozCCAUmgAwIBAgIUJExGQKSRbO3MT3H2eJAFxmwsr4MwCgYIKoZIzj0EAwIw"
                                   "HzEdMBsGA1UEAwwUcmNzZGsgdGVzdCBhdXRob3JpdHkwHhcNMjYwMTAxMDAwMDAw"
                                   "WhcNNDYwMTAxMDAwMDAwWjAfMR0wGwYDVQQDDBRyY3NkayB0ZXN0IGF1dGhvcml0"
                                   "eTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABI6kXmkY78c06nD2YqcVZgePqSiF"
                                   "McNEX3Si+4K46eoy4ipwJtKuH1awHh4evIdoh03bydCvs0Yx75hRxz5Wz0GjYzBh"
                                   "MB0GA1UdDgQWBBQXrYQGuOhFhjGkcgGwJ+u+sA4GODAfBgNVHSMEGDAWgBQXrYQG"
                                   "uOhFhjGkcgGwJ+u+sA4GODAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIB"
                                   "BjAKBggqhkjOPQQDAgNIADBFAiBYVu6kiLMUlb+41jddr6om0FHX+vnpu/R6zHyt"
                                   "jfPRaAIhAKIm+9yCxsMs1LdiN8C8bpMtdXetYwocmt1g2lCdLpG1";
constexpr char leafBase64[] = "MIIBuzCCAWGgAwIBAgIBAjAKBggqhkjOPQQDAjAfMR0wGwYDVQQDDBRyY3NkayB0"
                              "ZXN0IGF1dGhvcml0eTAeFw0yNjAxMDEwMDAwMDBaFw0yNzEyMDEwMDAwMDBaMBox"
                              "GDAWBgNVBAMMD3R1cm4ucmNzZGsudGVzdDBZMBMGByqGSM49AgEGCCqGSM49AwEH"
                              "A0IABB7j/FmJKShSknGhkySaHFsMbMvTCIz6IYe5VgXQulRo8R2x3mK3r6+jS4tW"
                              "TN1kaDUvRPBU5M2vUn8Y5ZPC35SjgZIwgY8wDAYDVR0TAQH/BAIwADAOBgNVHQ8B"
                              "Af8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwEwGgYDVR0RBBMwEYIPdHVybi5y"
                              "Y3Nkay50ZXN0MB0GA1UdDgQWBBSZrKQkdqqDHtzWomhQsdHqkbdvPDAfBgNVHSME"
                              "GDAWgBQXrYQGuOhFhjGkcgGwJ+u+sA4GODAKBggqhkjOPQQDAgNIADBFAiEAz44D"
                              "iC546PulMbkuvlLtAV5nEMx+9/iJL2WMcuM9gm4CIGV/jjRTOs/pgvblzytxnCdu"
                              "NqlUHuDQkGDnuB7zgWfF";
// A second authority of the test's, and two certificates it issued for
// turn.rcsdk.test (valid 2026-01-01 to 2027-12-01) that are no TLS server's:
// one for client authentication alone (its extended key usage), one that
// names the server in its subject alone (no subject alternative name).
constexpr char authority2Base64[] = "MIIBpzCCAU2gAwIBAgIUMmcuqmfKoHFx8G2cK+qv/38EiXMwCgYIKoZIzj0EAwIw"
                                    "ITEfMB0GA1UEAwwWcmNzZGsgdGVzdCBhdXRob3JpdHkgMjAeFw0yNjAxMDEwMDAw"
                                    "MDBaFw00NjAxMDEwMDAwMDBaMCExHzAdBgNVBAMMFnJjc2RrIHRlc3QgYXV0aG9y"
                                    "aXR5IDIwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATByq/SU9HFJU48nzajh336"
                                    "wxkuG6LrROBDY9hHkxrCNnFg9tHnF9KafVpzJd2OnwTkv+GdaRaDEMCOiiuUt/xQ"
                                    "o2MwYTAdBgNVHQ4EFgQUUGpGr+/IY/d21+U3VloBK2Ayml0wHwYDVR0jBBgwFoAU"
                                    "UGpGr+/IY/d21+U3VloBK2Ayml0wDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8E"
                                    "BAMCAQYwCgYIKoZIzj0EAwIDSAAwRQIgU3XR3zmVfBqEEW0CUARx9c7NwyhWCtn/"
                                    "sTPnGF+tWEQCIQCn3BL9igy6fyqK3CzyhGK/i+gWUYXzb8cHLHbh09G8eA";
constexpr char clientOnlyBase64[] = "MIIBvTCCAWOgAwIBAgIBAzAKBggqhkjOPQQDAjAhMR8wHQYDVQQDDBZyY3NkayB0"
                                    "ZXN0IGF1dGhvcml0eSAyMB4XDTI2MDEwMTAwMDAwMFoXDTI3MTIwMTAwMDAwMFow"
                                    "GjEYMBYGA1UEAwwPdHVybi5yY3Nkay50ZXN0MFkwEwYHKoZIzj0CAQYIKoZIzj0D"
                                    "AQcDQgAEtVSV7JFPF0WVRX4otlAFpNGWrrm01uFD4gZYPw8iDZZczT92E4WL06TF"
                                    "Sxs2J4tys2XCMS8ntjd9fV4khvkmxqOBkjCBjzAMBgNVHRMBAf8EAjAAMA4GA1Ud"
                                    "DwEB/wQEAwIHgDATBgNVHSUEDDAKBggrBgEFBQcDAjAaBgNVHREEEzARgg90dXJu"
                                    "LnJjc2RrLnRlc3QwHQYDVR0OBBYEFMYy566UUkklIEpCICmdi8WL7TDkMB8GA1Ud"
                                    "IwQYMBaAFFBqRq/vyGP3dtflN1ZaAStgMppdMAoGCCqGSM49BAMCA0gAMEUCIFxF"
                                    "+HCI67jBcT0VmdFh+7e0fIAlaFitaMtyTxjkEBT1AiEA7rGydwCPupBKeIedM4ma"
                                    "wmb2TvNjodi9OpWdPdXuv4k";
constexpr char subjectOnlyBase64[] = "MIIBnjCCAUWgAwIBAgIBBDAKBggqhkjOPQQDAjAhMR8wHQYDVQQDDBZyY3NkayB0"
                                     "ZXN0IGF1dGhvcml0eSAyMB4XDTI2MDEwMTAwMDAwMFoXDTI3MTIwMTAwMDAwMFow"
                                     "GjEYMBYGA1UEAwwPdHVybi5yY3Nkay50ZXN0MFkwEwYHKoZIzj0CAQYIKoZIzj0D"
                                     "AQcDQgAEtKh02VY4aQXrIk8LfRknSiIMHKTLDylQpVrNdybBERFsQBtTJ6+GsL2R"
                                     "XWB+mHq6WMXYS8trASDVrD4fla4fy6N1MHMwDAYDVR0TAQH/BAIwADAOBgNVHQ8B"
                                     "Af8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwEwHQYDVR0OBBYEFMILS4Fckst1"
                                     "kxHwbNcau7DxN8oEMB8GA1UdIwQYMBaAFFBqRq/vyGP3dtflN1ZaAStgMppdMAoG"
                                     "CCqGSM49BAMCA0cAMEQCIAUOGWp93lfDDRSG8dQVmcjOR9DG30B+IleU/POq7uXL"
                                     "AiAguh8qcOrs84yv6IXNJz2zSPP1p0M+rXuxNHB8SOUlhw";
// `openssl x509 -outform der | openssl dgst -sha256` of the leaf.
constexpr char leafSha256[] = "c6451f1d72af2435bb66f92564a7fe82103b80f111e6707be3184a42760654cb";
constexpr char server[] = "the TURN server 192.0.2.1:5349";

Bytes fromBase64(std::string_view text) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Bytes bytes;
    uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        buffer = buffer << 6 | uint32_t(std::strchr(alphabet, c) - alphabet);
        if ((bits += 6) >= 8) {
            bytes.push_back(uint8_t(buffer >> (bits -= 8)));
        }
    }
    return bytes;
}

bool says(const std::string& text, std::initializer_list<std::string> words) {
    const bool all = std::all_of(words.begin(), words.end(), [&](const auto& w) { return text.find(w) != text.npos; });
    if (!all) {
        std::fprintf(stderr, "unexpected: %s\n", text.c_str());
    }
    return all;
}

// The pin is the SHA-256 of the certificate's DER, compared whole.
void pins() {
    const auto leaf = fromBase64(leafBase64);
    const auto pin = crypto::sha256(leaf);
    CHECK(hex(pin) == leafSha256);
    CHECK(pinProblem(leaf, pin, server).empty());
    auto other = pin;
    other[31] ^= 0x80;
    CHECK(says(pinProblem(leaf, other, server),
               {server, hex(pin), hex(other), "turn_certificate_sha256", "a renewed certificate"}));
}

// What each verdict tells the host.
void wording() {
    const TlsSettings settings{"turn.example.com", std::nullopt};
    CHECK(says(trustProblem(TrustProblem::untrusted, settings, server, "code 1"),
               {"The certificate of " + std::string(server), "authority", "turn_certificate_sha256", "(code 1)"}));
    CHECK(says(trustProblem(TrustProblem::name, settings, server, "code 2"),
               {"\"turn.example.com\"", "turn_tls_server_name", "(code 2)"}));
    CHECK(says(trustProblem(TrustProblem::dates, settings, server, "code 3"), {"expired", "clock"}));
    CHECK(says(trustProblem(TrustProblem::other, settings, server, "code 4"), {"refused", "(code 4)"}));
}

// What a connection checks the server by: the name given, else the host of
// turn_server (with a port or without) unless it is an address; the pin
// unless it is all zero.
void settings() {
    rcs_ice_servers_config config = RCS_ICE_SERVERS_CONFIG_INIT;
    config.turn_transport = RCS_TURN_TLS;
    for (const char* server : {"turn.example.com", "turn.example.com:443"}) {
        std::strcpy(config.turn_server, server);
        const auto tls = tlsSettings(config);
        CHECK(tls.serverName == "turn.example.com" && !tls.certificateSha256);
    }
    for (const char* server : {"192.0.2.1", "192.0.2.1:443", "[2001:db8::1]", "[2001:db8::1]:443"}) {
        std::strcpy(config.turn_server, server);
        CHECK(tlsSettings(config).serverName.empty());
    }
    std::strcpy(config.turn_tls_server_name, "relay.example.net");
    config.turn_certificate_sha256[5] = 9;
    const auto tls = tlsSettings(config);
    CHECK(tls.serverName == "relay.example.net" && tls.certificateSha256 && (*tls.certificateSha256)[5] == 9);
}

// "host:port", a default port per way of reaching the server.
void addresses() {
    CHECK(ice::resolve("127.0.0.1").at(0).port() == 3478);
    CHECK(ice::resolve("127.0.0.1", 5349).at(0).port() == 5349);
    CHECK(ice::resolve("127.0.0.1:443", 5349).at(0).port() == 443);
    CHECK(ice::resolve("[::1]", 5349).at(0).toString() == "[::1]:5349");
    const auto named = ice::splitHostPort("turn.example.com", 5349);
    CHECK(named && named->host == "turn.example.com" && named->port == 5349);
    const auto bracketed = ice::splitHostPort("[2001:db8::1]:443", 5349);
    CHECK(bracketed && bracketed->host == "2001:db8::1" && bracketed->port == 443);
    CHECK(!ice::splitHostPort("turn.example.com:44x", 5349) && !ice::splitHostPort("turn.example.com:0", 5349));
    CHECK(!ice::splitHostPort(":443", 5349) && !ice::splitHostPort("[::1", 5349));
}

#if defined(RCSDK_CRYPTO_BEARSSL) || defined(__APPLE__)
#ifdef RCSDK_CRYPTO_BEARSSL
// certificateProblem() for `leafOf` issued by `authorityOf`, the authority trusted or not, at `date`.
std::string problemAt(const TlsSettings& settings, bool trusted, std::time_t date, const char* leafOf = leafBase64,
                      const char* authorityOf = authorityBase64) {
    const std::vector<Bytes> chain{fromBase64(leafOf), fromBase64(authorityOf)};
    std::vector<Bytes> authorities; // Only it: what the system trusts stays out of the test.
    if (trusted) {
        authorities.push_back(fromBase64(authorityOf));
    }
    return certificateProblem(chain, settings, server, authorities, date);
}
#else
SecCertificateRef certificate(const char* base64) {
    const auto der = fromBase64(base64);
    CFDataRef data = CFDataCreate(nullptr, der.data(), CFIndex(der.size()));
    SecCertificateRef value = SecCertificateCreateWithData(nullptr, data);
    CFRelease(data);
    CHECK(value);
    return value;
}

// certificateProblem() for `leafOf` issued by `authorityOf`, the authority trusted or not, at `date`.
std::string problemAt(const TlsSettings& settings, bool trusted, std::time_t date, const char* leafOf = leafBase64,
                      const char* authorityOf = authorityBase64) {
    SecCertificateRef leaf = certificate(leafOf), authority = certificate(authorityOf);
    const void* chain[] = {leaf, authority};
    CFArrayRef certificates = CFArrayCreate(nullptr, chain, 2, &kCFTypeArrayCallBacks);
    SecPolicyRef policy = SecPolicyCreateBasicX509();
    SecTrustRef trust = nullptr;
    CHECK(SecTrustCreateWithCertificates(certificates, policy, &trust) == errSecSuccess);
    if (trusted) { // Only it: what the system trusts stays out of the test.
        const void* anchor[] = {authority};
        CFArrayRef anchors = CFArrayCreate(nullptr, anchor, 1, &kCFTypeArrayCallBacks);
        CHECK(SecTrustSetAnchorCertificates(trust, anchors) == errSecSuccess);
        CFRelease(anchors);
    }
    CFDateRef when = CFDateCreate(nullptr, CFAbsoluteTime(date) - kCFAbsoluteTimeIntervalSince1970);
    CHECK(SecTrustSetVerifyDate(trust, when) == errSecSuccess);
    const std::string problem = certificateProblem(trust, settings, server);
    for (CFTypeRef owned : {CFTypeRef(when), CFTypeRef(trust), CFTypeRef(policy), CFTypeRef(certificates),
                            CFTypeRef(authority), CFTypeRef(leaf)}) {
        CFRelease(owned);
    }
    return problem;
}
#endif

constexpr std::time_t valid = 1782864000;   // 2026-07-01
constexpr std::time_t expired = 1843430400; // 2028-06-01
constexpr std::time_t early = 1748736000;   // 2025-06-01

// The verdicts a TLS connection's check of the certificate hands to the host.
void trustEvaluation() {
    const TlsSettings named{"turn.rcsdk.test", std::nullopt};
    CHECK(problemAt(named, true, valid).empty());
    CHECK(says(problemAt({"other.rcsdk.test", std::nullopt}, true, valid), {"is not for \"other.rcsdk.test\""}));
    CHECK(says(problemAt(named, true, expired), {"has expired or is not valid yet"}));
    CHECK(says(problemAt(named, true, early), {"has expired or is not valid yet"}));
    CHECK(says(problemAt(named, false, valid), {"does not lead to an authority this system trusts"}));
    // A pin takes exactly that certificate, whatever its authority, name or dates.
    std::array<uint8_t, 32> pin{};
    const auto leaf = fromBase64(leafBase64);
    pin = crypto::sha256(leaf);
    CHECK(problemAt({"", pin}, false, expired).empty());
    pin[0] ^= 1;
    CHECK(says(problemAt({"turn.rcsdk.test", pin}, true, valid), {"not the pinned turn_certificate_sha256"}));
}

// A certificate of a trusted authority, for the name, within its dates, but
// no TLS server's, is refused on every system: one for client authentication
// alone (BearSSL's minimal engine reads no extended key usage), and one that
// names the server in its subject alone (BearSSL and Windows took the common
// name; Apple's systems do not). A pin still takes exactly its certificate.
void serverUse() {
    const TlsSettings named{"turn.rcsdk.test", std::nullopt};
    CHECK(
        says(problemAt(named, true, valid, clientOnlyBase64, authority2Base64), {"is not a TLS server's certificate"}));
    const std::string subjectOnly = problemAt(named, true, valid, subjectOnlyBase64, authority2Base64);
#ifdef RCSDK_CRYPTO_BEARSSL
    CHECK(says(subjectOnly, {"is not a TLS server's certificate", "subject alone"}));
#else
    CHECK(says(subjectOnly, {"is not for \"turn.rcsdk.test\""}));
#endif
    const auto pin = crypto::sha256(fromBase64(clientOnlyBase64));
    CHECK(problemAt({"", pin}, false, valid, clientOnlyBase64, authority2Base64).empty());
}
#endif

#ifdef RCSDK_CRYPTO_BEARSSL
// The system's authorities, from its files (every system BearSSL serves has
// some: Android's store, a Linux distribution's bundle, /etc/ssl/cert.pem on
// macOS); the test's own authority is none of them.
void systemStore() {
    const auto authorities = systemAuthorities();
    std::printf("%zu authorities in this system's files\n", authorities.size());
    CHECK(!authorities.empty());
    const std::vector<Bytes> chain{fromBase64(leafBase64), fromBase64(authorityBase64)};
    CHECK(says(certificateProblem(chain, {"turn.rcsdk.test", std::nullopt}, server, authorities, valid),
               {"does not lead to an authority this system trusts", "BearSSL error 62"}));
}

// Android's store as files of the test's own: the system's authorities count
// unless the user turned them off; those the user added never do (SA-13: an
// application of Android 7 and later trusts them only where its network
// security configuration says so).
void androidStore() {
    namespace fs = std::filesystem;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("rcsdk-test-cacerts-" + std::to_string(now));
    const fs::path system = root / "system", user = root / "user";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    for (const auto& directory : {system, user / "cacerts-added", user / "cacerts-removed"}) {
        fs::create_directories(directory);
    }
    const auto place = [](const fs::path& file) {
        std::string text = "-----BEGIN CERTIFICATE-----\n";
        for (std::string_view rest = authorityBase64; !rest.empty();) {
            const auto line = rest.substr(0, 64);
            text.append(line).push_back('\n');
            rest.remove_prefix(line.size());
        }
        std::ofstream(file) << text << "-----END CERTIFICATE-----\n";
    };
    const std::vector<Bytes> chain{fromBase64(leafBase64), fromBase64(authorityBase64)};
    const TlsSettings settings{"turn.rcsdk.test", std::nullopt};
    place(user / "cacerts-added" / "5c3d1e2a.0");
    CHECK(androidAuthorities(system, user).empty());
    CHECK(says(certificateProblem(chain, settings, server, androidAuthorities(system, user), valid),
               {"does not lead to an authority this system trusts"}));
    place(system / "5c3d1e2a.0");
    CHECK(androidAuthorities(system, user).size() == 1);
    CHECK(certificateProblem(chain, settings, server, androidAuthorities(system, user), valid).empty());
    place(user / "cacerts-removed" / "5c3d1e2a.0");
    CHECK(androidAuthorities(system, user).empty());
    fs::remove_all(root, ignored);
}
#endif

} // namespace

int main() {
    return runTest("tls", [] {
        pins();
        wording();
        settings();
        addresses();
#if defined(RCSDK_CRYPTO_BEARSSL) || defined(__APPLE__)
        trustEvaluation();
        serverUse();
#endif
#ifdef RCSDK_CRYPTO_BEARSSL
        systemStore();
        androidStore();
#endif
    });
}
