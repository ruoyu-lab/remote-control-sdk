// TLS to a server through BearSSL (transport/tls.hpp; RCSDK_CRYPTO_BACKEND
// bearssl), over the SDK's own nonblocking TCP socket, as Schannel on Windows:
// the handshake runs in connectTls(), within the connection's time and
// cancellation; afterwards records are encrypted and decrypted as the TURN
// socket writes and reads, never waiting. TLS 1.2 (BearSSL has no 1.3) with
// ECDHE and AES-GCM or ChaCha20-Poly1305, no renegotiation. The certificate:
// the pin, or BearSSL's check of the chain against the system's authorities
// (tls_bearssl.hpp), the name and the dates.
#include "transport/tls_bearssl.hpp"

#include "crypto/crypto.hpp"
#include "transport/framed_socket.hpp"
#include "transport/socket_error.hpp"
#include "util/log.hpp"

// clang-format off
#include "bearssl/rcsdk_bearssl_names.h" // First: the calls below name the SDK's own copy.
#include <bearssl.h>
// clang-format on

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>

namespace rcsdk::transport {
namespace {

constexpr size_t readChunk = 16 * 1024;

#ifdef MSG_NOSIGNAL
constexpr int sendFlags = MSG_NOSIGNAL;
#else
constexpr int sendFlags = 0;
#endif

// TLS 1.2 with forward secrecy and an AEAD only.
constexpr uint16_t suites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,       BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,       BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256};

// What signatures may hash with, in the handshake and in certificates: no MD5 or SHA-1.
struct Hash {
    int id;
    const br_hash_class* vtable;
};
const Hash hashes[] = {
    {br_sha256_ID, &br_sha256_vtable}, {br_sha384_ID, &br_sha384_vtable}, {br_sha512_ID, &br_sha512_vtable}};

std::string codeOf(int error) {
    return "BearSSL error " + std::to_string(error);
}

// A failure of the connection, in words.
std::string describe(int error) {
    const char* text = nullptr;
    if (error >= BR_ERR_RECV_FATAL_ALERT && error < BR_ERR_SEND_FATAL_ALERT) {
        switch (error - BR_ERR_RECV_FATAL_ALERT) {
        case BR_ALERT_HANDSHAKE_FAILURE:
        case BR_ALERT_PROTOCOL_VERSION:
        case BR_ALERT_INSUFFICIENT_SECURITY:
            text = "no TLS version (1.2) or cipher in common with the server";
            break;
        case BR_ALERT_NO_APPLICATION_PROTOCOL:
            text = "the server takes no application protocol this end offers (ALPN stun.turn)";
            break;
        default:
            text = "the server sent a fatal alert";
            break;
        }
    } else {
        switch (error) {
        case BR_ERR_UNSUPPORTED_VERSION:
        case BR_ERR_BAD_CIPHER_SUITE:
        case BR_ERR_INVALID_ALGORITHM:
            text = "no TLS version (1.2) or cipher in common with the server";
            break;
        case BR_ERR_BAD_VERSION:
        case BR_ERR_BAD_LENGTH:
        case BR_ERR_UNKNOWN_TYPE:
            text = "the server does not speak TLS there";
            break;
        case BR_ERR_BAD_MAC:
            text = "a record failed its integrity check";
            break;
        case BR_ERR_UNEXPECTED:
        case BR_ERR_BAD_HANDSHAKE:
            text = "the server sent what TLS does not allow there";
            break;
        default:
            break;
        }
    }
    return text ? std::string(text) + " (" + codeOf(error) + ")" : codeOf(error);
}

// What BearSSL's check of a chain found.
TrustProblem trustProblemOf(unsigned error) {
    switch (error) {
    case BR_ERR_X509_NOT_TRUSTED:
    case BR_ERR_X509_DN_MISMATCH:
        return TrustProblem::untrusted;
    case BR_ERR_X509_BAD_SERVER_NAME:
        return TrustProblem::name;
    case BR_ERR_X509_EXPIRED:
    case BR_ERR_X509_TIME_UNKNOWN:
        return TrustProblem::dates;
    default:
        return TrustProblem::other;
    }
}

// A DER element at the start of `der`: its tag, its contents and what
// follows it; none where no whole element is there.
struct Element {
    uint8_t tag;
    std::span<const uint8_t> contents, rest;
};
std::optional<Element> element(std::span<const uint8_t> der) {
    if (der.size() < 2) {
        return std::nullopt;
    }
    size_t length = der[1], at = 2;
    if (length & 0x80) {
        const size_t bytes = length & 0x7F;
        if (!bytes || bytes > 4 || der.size() - at < bytes) {
            return std::nullopt;
        }
        length = 0;
        for (size_t i = 0; i < bytes; ++i) {
            length = length << 8 | der[at++];
        }
    }
    if (der.size() - at < length) {
        return std::nullopt;
    }
    return Element{der[0], der.subspan(at, length), der.subspan(at + length)};
}

// Why the server's certificate (DER) is no TLS server's, where BearSSL's
// check lets it pass (its minimal engine reads no extended key usage, and
// takes the subject's common name where there is no subject alternative
// name): it must be for server authentication where it says what it is for,
// and name its server among its subject alternative names (`named`: a name
// is checked). Empty when it is a server's.
std::string useProblem(std::span<const uint8_t> certificate, bool named) {
    static constexpr uint8_t extendedKeyUsage[] = {0x55, 0x1D, 0x25}, alternativeNames[] = {0x55, 0x1D, 0x11};
    static constexpr uint8_t serverAuth[] = {0x2B, 6, 1, 5, 5, 7, 3, 1}, anyUse[] = {0x55, 0x1D, 0x25, 0};
    const auto same = [](std::span<const uint8_t> a, std::span<const uint8_t> b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    };
    const auto whole = element(certificate);
    const auto signedPart = whole ? element(whole->contents) : std::nullopt;
    if (!signedPart) {
        return "it cannot be read";
    }
    bool server = true, alternatives = false;
    for (auto fields = signedPart->contents; !fields.empty();) {
        const auto field = element(fields);
        if (!field) {
            return "it cannot be read";
        }
        fields = field->rest;
        const auto list = field->tag == 0xA3 ? element(field->contents) : std::nullopt; // [3] Extensions.
        for (auto extensions = list ? list->contents : std::span<const uint8_t>(); !extensions.empty();) {
            const auto extension = element(extensions);
            const auto id = extension ? element(extension->contents) : std::nullopt;
            auto value = id ? element(id->rest) : std::nullopt;
            if (value && value->tag == 0x01) { // Critical.
                value = element(value->rest);
            }
            if (!value || id->tag != 0x06 || value->tag != 0x04) {
                return "it cannot be read";
            }
            extensions = extension->rest;
            if (same(id->contents, alternativeNames)) {
                alternatives = true;
            } else if (same(id->contents, extendedKeyUsage)) {
                const auto uses = element(value->contents);
                server = false;
                for (auto rest = uses ? uses->contents : std::span<const uint8_t>(); !rest.empty();) {
                    const auto use = element(rest);
                    if (!use) {
                        return "it cannot be read";
                    }
                    server = server ||
                             (use->tag == 0x06 && (same(use->contents, serverAuth) || same(use->contents, anyUse)));
                    rest = use->rest;
                }
            }
        }
    }
    if (!server) {
        return "its extended key usage leaves out server authentication";
    }
    if (named && !alternatives) {
        return "it names its server in its subject alone, with no subject alternative name";
    }
    return {};
}

void appendTo(void* bytes, const void* data, size_t length) {
    const auto* begin = static_cast<const uint8_t*>(data);
    static_cast<Bytes*>(bytes)->insert(static_cast<Bytes*>(bytes)->end(), begin, begin + length);
}

// BearSSL's trust anchors, made from certificates (DER): every one an
// authority, as a trust store holds them; one BearSSL cannot read is left out.
class Anchors {
public:
    explicit Anchors(std::span<const Bytes> authorities) {
        for (const auto& certificate : authorities) {
            add(certificate);
        }
    }
    const br_x509_trust_anchor* data() const noexcept { return anchors_.data(); }
    size_t size() const noexcept { return anchors_.size(); }

private:
    void add(const Bytes& certificate) {
        Bytes name;
        br_x509_decoder_context decoder;
        br_x509_decoder_init(&decoder, appendTo, &name);
        br_x509_decoder_push(&decoder, certificate.data(), certificate.size());
        const br_x509_pkey* key = br_x509_decoder_get_pkey(&decoder);
        if (!key) {
            return;
        }
        br_x509_trust_anchor anchor{};
        anchor.flags = BR_X509_TA_CA;
        anchor.dn = {keep(name.data(), name.size()), name.size()};
        anchor.pkey.key_type = key->key_type;
        if (key->key_type == BR_KEYTYPE_RSA) {
            const auto& rsa = key->key.rsa;
            anchor.pkey.key.rsa = {keep(rsa.n, rsa.nlen), rsa.nlen, keep(rsa.e, rsa.elen), rsa.elen};
        } else {
            const auto& ec = key->key.ec;
            anchor.pkey.key.ec = {ec.curve, keep(ec.q, ec.qlen), ec.qlen};
        }
        anchors_.push_back(anchor);
    }
    unsigned char* keep(const unsigned char* bytes, size_t length) {
        storage_.emplace_back(bytes, bytes + length);
        return storage_.back().data();
    }

    std::vector<br_x509_trust_anchor> anchors_;
    std::vector<Bytes> storage_; // What anchors_ points into (a moved vector keeps its bytes where they are).
};

// The X.509 engine BearSSL's handshake calls (and certificateProblem()): with
// a pin, the server's certificate and its SHA-256 alone; otherwise BearSSL's
// own check (its "minimal" engine) against the authorities, and then what the
// server's certificate is for (useProblem).
class CertificateCheck {
public:
    CertificateCheck(const TlsSettings& settings, std::string what, std::span<const Bytes> authorities)
        : settings_(settings), what_(std::move(what)), anchors_(authorities) {
        br_x509_minimal_init(&minimal_, &br_sha256_vtable, anchors_.data(), anchors_.size());
        br_x509_minimal_set_rsa(&minimal_, br_rsa_pkcs1_vrfy_get_default());
        br_x509_minimal_set_ecdsa(&minimal_, br_ec_get_default(), br_ecdsa_vrfy_asn1_get_default());
        for (const auto& hash : hashes) {
            br_x509_minimal_set_hash(&minimal_, hash.id, hash.vtable);
        }
    }
    CertificateCheck(const CertificateCheck&) = delete;
    CertificateCheck& operator=(const CertificateCheck&) = delete;

    const br_x509_class** engine() noexcept { return &engine_.vtable; }
    // Checks the dates at `now` instead of the clock's time.
    void at(std::time_t now) {
        constexpr uint32_t daysTo1970 = 719528; // BearSSL counts days from 1 January of year 0.
        br_x509_minimal_set_time(&minimal_, uint32_t(now / 86400) + daysTo1970, uint32_t(now % 86400));
    }
    // Why the last chain failed; empty when it passed.
    const std::string& problem() const noexcept { return problem_; }

private:
    struct Engine {
        const br_x509_class* vtable; // First: BearSSL calls with its address.
        CertificateCheck* check;
    };
    static CertificateCheck& of(const br_x509_class* const* engine) {
        return *reinterpret_cast<const Engine*>(engine)->check;
    }
    bool pinned() const noexcept { return settings_.certificateSha256.has_value(); }
    const br_x509_class** minimal() noexcept { return &minimal_.vtable; }

    static void startChain(const br_x509_class** engine, const char* serverName) {
        auto& self = of(engine);
        self.problem_.clear();
        self.leaf_.clear();
        self.certificates_ = 0;
        if (!self.pinned()) {
            (*self.minimal())->start_chain(self.minimal(), serverName);
        }
    }
    static void startCert(const br_x509_class** engine, uint32_t length) {
        auto& self = of(engine);
        if (!self.pinned()) {
            (*self.minimal())->start_cert(self.minimal(), length);
        }
    }
    static void append(const br_x509_class** engine, const unsigned char* bytes, size_t length) {
        auto& self = of(engine);
        if (!self.pinned()) {
            (*self.minimal())->append(self.minimal(), bytes, length);
        }
        if (self.certificates_ == 0) {
            self.leaf_.insert(self.leaf_.end(), bytes, bytes + length);
        }
    }
    static void endCert(const br_x509_class** engine) {
        auto& self = of(engine);
        if (!self.pinned()) {
            (*self.minimal())->end_cert(self.minimal());
        }
        ++self.certificates_;
    }
    static unsigned endChain(const br_x509_class** engine) {
        auto& self = of(engine);
        return self.pinned() ? self.checkPin() : self.checkChain();
    }
    static const br_x509_pkey* publicKey(const br_x509_class* const* engine, unsigned* usages) {
        auto& self = of(engine);
        if (!self.pinned()) {
            return (*self.minimal())->get_pkey(self.minimal(), usages);
        }
        if (usages) {
            *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
        }
        return br_x509_decoder_get_pkey(&self.leafKey_);
    }

    unsigned checkPin() {
        if (certificates_ == 0) {
            problem_ = "The certificate of " + what_ + " is missing";
            return BR_ERR_X509_EMPTY_CHAIN;
        }
        problem_ = pinProblem(leaf_, *settings_.certificateSha256, what_, settings_.pinSetting);
        if (!problem_.empty()) {
            return BR_ERR_X509_NOT_TRUSTED;
        }
        br_x509_decoder_init(&leafKey_, nullptr, nullptr);
        br_x509_decoder_push(&leafKey_, leaf_.data(), leaf_.size());
        if (const int error = br_x509_decoder_last_error(&leafKey_)) {
            problem_ = "The certificate of " + what_ + " cannot be read (" + codeOf(error) + ")";
            return unsigned(error);
        }
        return 0;
    }
    unsigned checkChain() {
        const unsigned error = (*minimal())->end_chain(minimal());
        if (error) {
            const std::string detail =
                codeOf(int(error)) + (anchors_.size() ? "" : "; no authorities to check it against");
            problem_ = trustProblem(trustProblemOf(error), settings_, what_, detail);
            return error;
        }
        if (const auto use = useProblem(leaf_, !settings_.serverName.empty()); !use.empty()) {
            problem_ = trustProblem(TrustProblem::use, settings_, what_, use);
            return BR_ERR_X509_FORBIDDEN_KEY_USAGE;
        }
        return 0;
    }

    static const br_x509_class vtable;
    Engine engine_{&vtable, this};
    const TlsSettings settings_;
    const std::string what_;
    Anchors anchors_;
    br_x509_minimal_context minimal_;
    br_x509_decoder_context leafKey_; // The pinned certificate's key, for the handshake.
    Bytes leaf_;                      // The server's certificate.
    size_t certificates_ = 0;
    std::string problem_;
};

const br_x509_class CertificateCheck::vtable = {sizeof(Engine), startChain, startCert, append,
                                                endCert,        endChain,   publicKey};

class BearStream final : public ByteStream {
public:
    BearStream(Socket socket, const TlsSettings& settings, std::string what, std::span<const Bytes> authorities)
        : socket_(std::move(socket)), settings_(settings), what_(std::move(what)), check_(settings, what_, authorities),
          buffer_(BR_SSL_BUFSIZE_BIDI) {
        br_ssl_engine_context* engine = &client_.eng;
        br_ssl_client_zero(&client_);
        br_ssl_engine_set_versions(engine, BR_TLS12, BR_TLS12);
        br_ssl_engine_set_suites(engine, suites, std::size(suites));
        br_ssl_engine_set_default_ecdsa(engine); // ECDHE too.
        br_ssl_engine_set_default_rsavrfy(engine);
        for (const auto& hash : hashes) {
            br_ssl_engine_set_hash(engine, hash.id, hash.vtable);
        }
        br_ssl_engine_set_prf_sha256(engine, &br_tls12_sha256_prf);
        br_ssl_engine_set_prf_sha384(engine, &br_tls12_sha384_prf);
        br_ssl_engine_set_default_aes_gcm(engine);
        br_ssl_engine_set_default_chapol(engine);
        br_ssl_engine_set_x509(engine, check_.engine());
        protocols_[0] = settings_.alpn.c_str();
        br_ssl_engine_set_protocol_names(engine, protocols_, 1);
        br_ssl_engine_add_flags(engine, BR_OPT_NO_RENEGOTIATION);
        br_ssl_engine_set_buffer(engine, buffer_.data(), buffer_.size(), 1);
        std::array<uint8_t, 32> seed;
        crypto::random(seed);
        br_ssl_engine_inject_entropy(engine, seed.data(), seed.size());
        // SNI, and the name the certificate must be for: none with a pin and no name.
        if (!br_ssl_client_reset(&client_, settings_.serverName.empty() ? nullptr : settings_.serverName.c_str(), 0)) {
            failHandshake();
        }
    }
    BearStream(const BearStream&) = delete;
    BearStream& operator=(const BearStream&) = delete;

    // The handshake, by `deadline`.
    void handshake(CancelToken cancelled, Clock::time_point deadline) {
        for (;;) {
            if (!flush()) {
                await(true, cancelled, deadline);
                continue;
            }
            const unsigned state = br_ssl_engine_current_state(&client_.eng);
            if (state & BR_SSL_CLOSED) {
                failHandshake();
            }
            if (state & BR_SSL_SENDAPP) {
                break; // Application data may go: the handshake ended.
            }
            throwIfCancelled(cancelled);
            if (!feed() && !receive()) {
                await(false, cancelled, deadline);
            }
        }
        opened();
    }

    size_t writeSome(std::span<const uint8_t> bytes) override {
        if (!flush()) {
            return 0; // Records still wait for the socket.
        }
        throwIfClosed();
        size_t room = 0;
        unsigned char* plain = br_ssl_engine_sendapp_buf(&client_.eng, &room);
        if (!plain) {
            return 0;
        }
        const size_t size = std::min(room, bytes.size());
        std::memcpy(plain, bytes.data(), size);
        br_ssl_engine_sendapp_ack(&client_.eng, size);
        br_ssl_engine_flush(&client_.eng, 0);
        flush();
        return size;
    }

    // One read from the socket at most: records without application data
    // (empty ones, alerts) cannot keep it here, and what remains is buffered().
    size_t readSome(std::span<uint8_t> buffer) override {
        for (bool received = false;;) {
            size_t length = 0;
            if (const unsigned char* plain = br_ssl_engine_recvapp_buf(&client_.eng, &length)) {
                const size_t taken = std::min(length, buffer.size());
                std::memcpy(buffer.data(), plain, taken);
                br_ssl_engine_recvapp_ack(&client_.eng, taken);
                return taken;
            }
            flush(); // What BearSSL answers with: an alert, its close_notify.
            throwIfClosed();
            if (feed()) {
                continue;
            }
            if (received || !receive()) {
                return 0;
            }
            received = true;
        }
    }

    Handle handle() const noexcept override { return socket_.get(); }

    bool buffered() const noexcept override {
        const unsigned state = br_ssl_engine_current_state(&client_.eng);
        return (state & BR_SSL_RECVAPP) || (!received_.empty() && (state & BR_SSL_RECVREC));
    }

    Endpoint local() const override { return localAddress(socket_.get()); }

    // `rest` is encrypted after what waits, then close_notify; the server's
    // own close is awaited, as closing with its data unread would reset the
    // connection under what the server has not read yet.
    void close(std::span<const uint8_t> rest, Clock::time_point deadline) override {
        static const std::atomic<bool> never{false};
        while (!rest.empty()) {
            if (const size_t taken = writeSome(rest)) {
                rest = rest.subspan(taken);
            } else {
                waitReady(socket_.get(), true, never, deadline);
            }
        }
        br_ssl_engine_close(&client_.eng);
        while (!flush()) {
            waitReady(socket_.get(), true, never, deadline);
        }
        ::shutdown(socket_.get(), SHUT_WR);
        for (;;) {
            uint8_t chunk[readChunk];
            const auto result = ::recv(socket_.get(), chunk, sizeof(chunk), 0);
            if (result == 0 || (result < 0 && !wouldBlock())) {
                return;
            }
            if (result < 0) {
                waitReady(socket_.get(), false, never, deadline);
            }
        }
    }

private:
    [[noreturn]] void failHandshake() const {
        if (!check_.problem().empty()) {
            fail(RCS_ERROR_AUTHENTICATION, check_.problem());
        }
        fail(RCS_ERROR_NETWORK,
             "No TLS connection to " + what_ + ": " + describe(br_ssl_engine_last_error(&client_.eng)));
    }

    // Once the connection ended: the server closed it, or it failed.
    void throwIfClosed() const {
        if (!(br_ssl_engine_current_state(&client_.eng) & BR_SSL_CLOSED)) {
            return;
        }
        const int error = br_ssl_engine_last_error(&client_.eng);
        if (error == BR_ERR_OK) {
            fail(RCS_ERROR_NETWORK, "The TLS connection to " + what_ + " was closed");
        }
        fail(RCS_ERROR_NETWORK, "The TLS connection to " + what_ + " failed: " + describe(error));
    }

    void opened() {
        open_ = true;
        const char* alpn = br_ssl_engine_get_selected_protocol(&client_.eng);
        log::info("network", "TLS 1.2 to " + what_ +
                                 (settings_.certificateSha256
                                      ? ", the pinned certificate"
                                      : ", a certificate this system trusts for \"" + settings_.serverName + "\"") +
                                 (alpn ? ", ALPN " + std::string(alpn) : ", no ALPN"));
    }

    // Hands BearSSL what arrived, as far as it takes it now; false when it took nothing.
    bool feed() {
        size_t fed = 0;
        while (fed < received_.size()) {
            size_t room = 0;
            unsigned char* records = br_ssl_engine_recvrec_buf(&client_.eng, &room);
            if (!records) {
                break;
            }
            const size_t length = std::min(room, received_.size() - fed);
            std::memcpy(records, received_.data() + fed, length);
            fed += length;
            br_ssl_engine_recvrec_ack(&client_.eng, length);
        }
        received_.erase(received_.begin(), received_.begin() + ptrdiff_t(fed));
        return fed != 0;
    }

    // Appends what the socket has; false when nothing arrived.
    bool receive() {
        uint8_t chunk[readChunk];
        const auto result = ::recv(socket_.get(), chunk, sizeof(chunk), 0);
        if (result > 0) {
            received_.insert(received_.end(), chunk, chunk + result);
            return true;
        }
        if (result == 0) {
            fail(RCS_ERROR_NETWORK, open_ ? "The TLS connection to " + what_ + " was closed"
                                          : what_ + " closed the connection during the TLS handshake");
        }
        if (!wouldBlock()) {
            fail(RCS_ERROR_NETWORK, "Receiving from " + what_ + " failed: " + lastSocketErrorText());
        }
        return false;
    }

    // Writes the records BearSSL has as far as the socket takes them; true once all went.
    bool flush() {
        for (;;) {
            size_t length = 0;
            const unsigned char* records = br_ssl_engine_sendrec_buf(&client_.eng, &length);
            if (!records) {
                return true;
            }
            const auto result = ::send(socket_.get(), records, length, sendFlags);
            if (result > 0) {
                br_ssl_engine_sendrec_ack(&client_.eng, size_t(result));
            } else if (result < 0 && wouldBlock()) {
                return false;
            } else {
                fail(RCS_ERROR_NETWORK, "Sending to " + what_ + " failed: " + lastSocketErrorText());
            }
        }
    }

    // The handshake waits for the socket (to read, or to write), until `deadline`.
    void await(bool write, CancelToken cancelled, Clock::time_point deadline) const {
        try {
            waitReady(socket_.get(), write, cancelled, deadline);
        } catch (const Failure& failure) {
            if (failure.result() == RCS_ERROR_CANCELLED) {
                throw;
            }
            fail(RCS_ERROR_NETWORK,
                 "No TLS handshake with " + what_ + " in time (it does not answer one, or does not speak TLS there)");
        }
    }

    Socket socket_;
    const TlsSettings settings_;
    const std::string what_;
    const char* protocols_[1] = {}; // settings_.alpn, as BearSSL takes it.
    CertificateCheck check_;
    br_ssl_client_context client_;
    std::vector<unsigned char> buffer_; // BearSSL's, for records both ways.
    std::vector<uint8_t> received_;     // From the socket, not yet taken by BearSSL.
    bool open_ = false;                 // The handshake ended.
};

// The certificates (DER) of the PEM text in `file`; none when it cannot be read.
void readCertificates(const std::filesystem::path& file, std::vector<Bytes>& out) {
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text += '\n'; // A last line without its newline ends all the same.
    br_pem_decoder_context decoder;
    br_pem_decoder_init(&decoder);
    Bytes object;
    bool certificate = false;
    for (std::string_view rest = text; !rest.empty();) {
        rest.remove_prefix(br_pem_decoder_push(&decoder, rest.data(), rest.size()));
        switch (br_pem_decoder_event(&decoder)) {
        case BR_PEM_BEGIN_OBJ:
            certificate = std::strcmp(br_pem_decoder_name(&decoder), "CERTIFICATE") == 0;
            object.clear();
            br_pem_decoder_setdest(&decoder, certificate ? appendTo : nullptr, &object);
            break;
        case BR_PEM_END_OBJ:
            if (certificate) {
                out.push_back(std::move(object));
            }
            break;
        case BR_PEM_ERROR: // That object is left out.
        default:
            break;
        }
    }
}

// The files in `directory`; none when it cannot be read.
std::vector<std::filesystem::path> filesIn(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::directory_iterator file(directory, error), end; !error && file != end;
         file.increment(error)) {
        files.push_back(file->path());
    }
    return files;
}

} // namespace

std::vector<Bytes> androidAuthorities(const std::filesystem::path& system, const std::filesystem::path& user) {
    std::set<std::filesystem::path> removed;
    for (const auto& file : filesIn(user / "cacerts-removed")) {
        removed.insert(file.filename());
    }
    std::vector<Bytes> authorities;
    for (const auto& file : filesIn(system)) {
        if (!removed.count(file.filename())) {
            readCertificates(file, authorities);
        }
    }
    return authorities;
}

#ifdef __ANDROID__
std::vector<Bytes> systemAuthorities() {
    // Android 14 moved the system's store into the Conscrypt module, which updates reach.
    std::error_code error;
    const std::filesystem::path system = std::filesystem::exists("/apex/com.android.conscrypt/cacerts", error)
                                             ? "/apex/com.android.conscrypt/cacerts"
                                             : "/system/etc/security/cacerts";
    // What the user changed (Settings, Security, Encryption & credentials), per Android user.
    return androidAuthorities(system, "/data/misc/user/" + std::to_string(getuid() / 100000));
}
#else
std::vector<Bytes> systemAuthorities() {
    for (const char* bundle : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                               "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"}) {
        std::vector<Bytes> authorities;
        readCertificates(bundle, authorities);
        if (!authorities.empty()) {
            return authorities;
        }
    }
    return {};
}
#endif

std::string certificateProblem(std::span<const Bytes> chain, const TlsSettings& settings, const std::string& what,
                               std::span<const Bytes> authorities, std::time_t now) {
    CertificateCheck check(settings, what, authorities);
    check.at(now);
    const br_x509_class** engine = check.engine();
    (*engine)->start_chain(engine, settings.serverName.empty() ? nullptr : settings.serverName.c_str());
    for (const auto& certificate : chain) {
        (*engine)->start_cert(engine, uint32_t(certificate.size()));
        (*engine)->append(engine, certificate.data(), certificate.size());
        (*engine)->end_cert(engine);
    }
    (*engine)->end_chain(engine);
    return check.problem();
}

std::unique_ptr<ByteStream> connectTls(const Endpoint& server, const TlsSettings& settings, const std::string& what,
                                       CancelToken cancelled, std::chrono::milliseconds timeout,
                                       uint32_t interfaceIndex) {
    const auto deadline = Clock::now() + timeout;
    const auto authorities = settings.certificateSha256 ? std::vector<Bytes>() : systemAuthorities();
    auto stream = std::make_unique<BearStream>(
        connectStreamTcp(server, what + " over TLS", cancelled, timeout, interfaceIndex), settings, what, authorities);
    stream->handshake(cancelled, deadline);
    return stream;
}

} // namespace rcsdk::transport
