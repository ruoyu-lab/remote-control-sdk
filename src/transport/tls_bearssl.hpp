// The certificate check of TLS with BearSSL (tls_bearssl.cpp), on its own for tests.
#pragma once

#include "transport/tls.hpp"
#include "util/util.hpp"

#include <ctime>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace rcsdk::transport {

// The certificates (DER) this system trusts as authorities, from its files:
// on Android androidAuthorities() of the system's store (the updatable one of
// Android 14 and later where it is, else /system/etc/security/cacerts) and
// the user's changes (/data/misc/user/<Android user>); elsewhere the first
// bundle there is of /etc/ssl/certs/ca-certificates.crt,
// /etc/pki/tls/certs/ca-bundle.crt, /etc/ssl/ca-bundle.pem and
// /etc/ssl/cert.pem. Empty when there are none.
std::vector<Bytes> systemAuthorities();

// The authorities of Android's store `system`, less those the user turned off
// (`user`/cacerts-removed). Those the user added (cacerts-added) are left out:
// from Android 7 an application trusts them only where its network security
// configuration says so, which the SDK cannot read, and an authority anyone
// talked the user into installing must not pass for the server. A host that
// needs a private or user-installed authority pins the server's certificate
// (turn_certificate_sha256).
std::vector<Bytes> androidAuthorities(const std::filesystem::path& system, const std::filesystem::path& user);

// Why `chain` (DER, the server's certificate first) fails `settings` at
// `now` with `authorities` (DER) trusted; empty when it passes. With a pin
// only the server's certificate counts; otherwise the chain must lead to an
// authority and be for the server name, every certificate within its dates.
// What the connection decides by.
std::string certificateProblem(std::span<const Bytes> chain, const TlsSettings& settings, const std::string& what,
                               std::span<const Bytes> authorities, std::time_t now);

} // namespace rcsdk::transport
