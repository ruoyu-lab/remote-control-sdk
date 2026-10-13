// The certificate check of TLS on Apple systems (tls_connection.mm), on its own for tests.
#pragma once

#include "transport/tls.hpp"

#include <Security/Security.h>

#include <string>

namespace rcsdk::transport {

// Why the server's certificate in `trust` fails `settings`; empty when it
// passes. With a pin only the leaf counts; otherwise the trust store (or the
// anchors set on `trust`) and the server name, without fetching anything from
// the network (no revocation lookups, no missing intermediates: networks that
// pass little but TLS block those too). What the connection's verify block
// decides by.
std::string certificateProblem(SecTrustRef trust, const TlsSettings& settings, const std::string& what);

} // namespace rcsdk::transport
