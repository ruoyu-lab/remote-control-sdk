// TLS to a server through Schannel (transport/tls.hpp), over the SDK's own
// nonblocking TCP socket. The handshake runs in connectTls(), within the
// connection's time and cancellation; afterwards records are encrypted and
// decrypted as the TURN socket writes and reads, never waiting. A message
// after the handshake (TLS 1.3 session tickets and key updates, a TLS 1.2
// renegotiation) goes back through InitializeSecurityContext as Schannel asks
// (SEC_I_RENEGOTIATE); until that ends nothing new is encrypted.
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS          // SCH_CREDENTIALS and TLS_PARAMETERS (Windows 10 1809).
#define CERT_CHAIN_PARA_HAS_EXTRA_FIELDS // CERT_CHAIN_PARA.dwUrlRetrievalTimeout.

#include "transport/tls.hpp"

#include "platform/windows/win.hpp"
#include "transport/framed_socket.hpp"
#include "transport/socket_error.hpp"
#include "util/log.hpp"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201) // subauth.h has nameless structs.
#endif
#include <subauth.h> // UNICODE_STRING, which SCH_CREDENTIALS uses.
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <security.h>
#include <schannel.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

namespace rcsdk::transport {
namespace {

constexpr size_t readChunk = 16 * 1024;

std::string codeOf(SECURITY_STATUS status) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(status));
    return std::string("SSPI ") + code;
}

std::string describe(SECURITY_STATUS status) {
    switch (status) {
    case SEC_E_ALGORITHM_MISMATCH:
        return "no TLS version (1.2 or later) or cipher in common with the server (" + codeOf(status) + ")";
    case SEC_E_ILLEGAL_MESSAGE:
        return "the server sent what TLS does not allow there, or an alert (" + codeOf(status) + ")";
    case SEC_E_INVALID_TOKEN:
        return "the server does not speak TLS there (" + codeOf(status) + ")";
    case SEC_E_MESSAGE_ALTERED:
    case SEC_E_DECRYPT_FAILURE:
        return "a record failed its integrity check (" + codeOf(status) + ")";
    case SEC_E_APPLICATION_PROTOCOL_MISMATCH:
        return "the server takes no application protocol this end offers (ALPN stun.turn) (" + codeOf(status) + ")";
    default:
        return codeOf(status);
    }
}

// What the system's check of the certificate found, or nothing for a failure
// of another kind.
std::optional<TrustProblem> certificateFailure(SECURITY_STATUS status) {
    switch (status) {
    case SEC_E_UNTRUSTED_ROOT:
    case CERT_E_UNTRUSTEDROOT:
    case CERT_E_CHAINING:
        return TrustProblem::untrusted;
    case SEC_E_WRONG_PRINCIPAL:
    case CERT_E_CN_NO_MATCH:
        return TrustProblem::name;
    case SEC_E_CERT_EXPIRED:
    case CERT_E_EXPIRED:
        return TrustProblem::dates;
    case SEC_E_CERT_WRONG_USAGE:
    case CERT_E_WRONG_USAGE:
        return TrustProblem::use;
    case SEC_E_CERT_UNKNOWN:
    case CERT_E_REVOKED:
    case CRYPT_E_REVOKED:
        return TrustProblem::other;
    default:
        return std::nullopt;
    }
}

// The application protocols offered (SEC_APPLICATION_PROTOCOLS): one ALPN
// list with `protocol`, laid out as the structure is.
std::vector<uint8_t> alpnList(const std::string& protocol) {
    const size_t length = protocol.size();
    const auto listSize = static_cast<unsigned short>(1 + length); // The length byte, then the name.
    const auto extension = static_cast<unsigned int>(SecApplicationProtocolNegotiationExt_ALPN);
    const auto listsSize = static_cast<unsigned int>(sizeof(extension) + sizeof(listSize) + listSize);
    std::vector<uint8_t> bytes;
    const auto append = [&](const void* value, size_t size) {
        const auto* data = static_cast<const uint8_t*>(value);
        bytes.insert(bytes.end(), data, data + size);
    };
    append(&listsSize, sizeof(listsSize));
    append(&extension, sizeof(extension));
    append(&listSize, sizeof(listSize));
    bytes.push_back(uint8_t(length));
    append(protocol.data(), length);
    return bytes;
}

// What Schannel allocated for an InitializeSecurityContext (ISC_REQ_ALLOCATE_MEMORY).
struct Output {
    SecBuffer buffers[3] = {
        {0, SECBUFFER_TOKEN, nullptr}, {0, SECBUFFER_ALERT, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
    SecBufferDesc desc{SECBUFFER_VERSION, 3, buffers};
    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    ~Output() {
        for (const auto& buffer : buffers) {
            if (buffer.pvBuffer) {
                FreeContextBuffer(buffer.pvBuffer);
            }
        }
    }
    std::span<const uint8_t> token() const {
        return buffers[0].pvBuffer ? std::span(static_cast<const uint8_t*>(buffers[0].pvBuffer), buffers[0].cbBuffer)
                                   : std::span<const uint8_t>();
    }
};

class SchannelStream final : public ByteStream {
public:
    SchannelStream(Socket socket, const TlsSettings& settings, std::string what)
        : socket_(std::move(socket)), settings_(settings), what_(std::move(what)),
          target_(platform::win::wide(settings.serverName, settings.nameSetting.c_str())) {}
    ~SchannelStream() override {
        if (hasContext_) {
            DeleteSecurityContext(&context_);
        }
        if (hasCredentials_) {
            FreeCredentialsHandle(&credentials_);
        }
    }
    SchannelStream(const SchannelStream&) = delete;
    SchannelStream& operator=(const SchannelStream&) = delete;

    // The handshake, by `deadline`.
    void handshake(CancelToken cancelled, Clock::time_point deadline) {
        acquireCredentials();
        auto alpn = alpnList(settings_.alpn);
        SecBuffer offer{ULONG(alpn.size()), SECBUFFER_APPLICATION_PROTOCOLS, alpn.data()};
        SecBufferDesc offers{SECBUFFER_VERSION, 1, &offer};
        SECURITY_STATUS status;
        {
            Output output;
            ULONG attributes = 0;
            status = InitializeSecurityContextW(&credentials_, nullptr, target(), flags(), 0, 0, &offers, 0, &context_,
                                                &output.desc, &attributes, nullptr);
            if (status != SEC_I_CONTINUE_NEEDED) {
                failHandshake(status);
            }
            hasContext_ = true;
            const auto token = output.token();
            sending_.assign(token.begin(), token.end());
        }
        for (;;) {
            while (!flush()) {
                await(true, cancelled, deadline);
            }
            if (status == SEC_E_OK) {
                break;
            }
            throwIfCancelled(cancelled);
            if (status == SEC_E_INCOMPLETE_MESSAGE || received_.empty()) {
                while (!receive()) {
                    await(false, cancelled, deadline);
                }
            }
            status = step();
        }
        checkCertificate(deadline);
        opened();
    }

    size_t writeSome(std::span<const uint8_t> bytes) override {
        if (!flush() || renegotiating_) {
            return 0; // Records still wait for the socket, or Schannel for its handshake.
        }
        const size_t size = std::min<size_t>(bytes.size(), sizes_.cbMaximumMessage);
        sending_.resize(sizes_.cbHeader + size + sizes_.cbTrailer);
        uint8_t* record = sending_.data();
        std::memcpy(record + sizes_.cbHeader, bytes.data(), size);
        SecBuffer buffers[4] = {{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, record},
                                {ULONG(size), SECBUFFER_DATA, record + sizes_.cbHeader},
                                {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, record + sizes_.cbHeader + size},
                                {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
        const SECURITY_STATUS status = EncryptMessage(&context_, 0, &message, 0);
        if (status != SEC_E_OK) {
            sending_.clear();
            fail(RCS_ERROR_NETWORK, "Encrypting for " + what_ + " failed (" + describe(status) + ")");
        }
        // The trailer may be shorter than the most it can be.
        sending_.resize(size_t(buffers[0].cbBuffer) + buffers[1].cbBuffer + buffers[2].cbBuffer);
        flush();
        return size;
    }

    // One read from the socket at most: records without application data
    // (empty ones, post-handshake messages) cannot keep it here, and what
    // remains is buffered().
    size_t readSome(std::span<uint8_t> buffer) override {
        for (bool received = false;;) {
            if (plainBegin_ < plain_.size()) {
                const size_t taken = std::min(buffer.size(), plain_.size() - plainBegin_);
                std::memcpy(buffer.data(), plain_.data() + plainBegin_, taken);
                plainBegin_ += taken;
                if (plainBegin_ == plain_.size()) {
                    plain_.clear();
                    plainBegin_ = 0;
                }
                return taken;
            }
            if (renegotiating_ ? renegotiate() : decrypt()) {
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
        return plainBegin_ < plain_.size() || (!received_.empty() && !stalled_) || callNow_;
    }

    Endpoint local() const override { return localAddress(socket_.get()); }

    // `rest` is encrypted after what waits (unless a renegotiation waits for
    // the server: nothing can be then), then close_notify; the server's own
    // close is awaited, as closing with its data unread would reset the
    // connection under what the server has not read yet.
    void close(std::span<const uint8_t> rest, Clock::time_point deadline) override {
        static const std::atomic<bool> never{false};
        if (callNow_) {
            renegotiate();
        }
        while (!rest.empty() && !renegotiating_) {
            if (const size_t taken = writeSome(rest)) {
                rest = rest.subspan(taken);
            } else {
                waitReady(socket_.get(), true, never, deadline);
            }
        }
        DWORD type = SCHANNEL_SHUTDOWN;
        SecBuffer control{sizeof(type), SECBUFFER_TOKEN, &type};
        SecBufferDesc controls{SECBUFFER_VERSION, 1, &control};
        if (ApplyControlToken(&context_, &controls) == SEC_E_OK) {
            Output output;
            ULONG attributes = 0;
            const SECURITY_STATUS status =
                InitializeSecurityContextW(&credentials_, &context_, target(), flags(), 0, 0, nullptr, 0, &context_,
                                           &output.desc, &attributes, nullptr);
            if (status == SEC_E_OK || status == SEC_I_CONTEXT_EXPIRED) {
                const auto token = output.token();
                sending_.insert(sending_.end(), token.begin(), token.end());
            }
        }
        while (!flush()) {
            waitReady(socket_.get(), true, never, deadline);
        }
        ::shutdown(socket_.get(), SD_SEND);
        for (;;) {
            uint8_t chunk[readChunk];
            const int result = ::recv(socket_.get(), reinterpret_cast<char*>(chunk), int(sizeof(chunk)), 0);
            if (result == 0 || (result < 0 && !wouldBlock())) {
                return;
            }
            if (result < 0) {
                waitReady(socket_.get(), false, never, deadline);
            }
        }
    }

private:
    ULONG flags() const noexcept {
        return ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
               ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS | ISC_REQ_MANUAL_CRED_VALIDATION;
    }
    // The name for SNI and Schannel's own name check; none with a pin and no name.
    SEC_WCHAR* target() noexcept { return target_.empty() ? nullptr : target_.data(); }

    void acquireCredentials() {
        TLS_PARAMETERS parameters{};
        parameters.grbitDisabledProtocols = ~DWORD(SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT);
        SCH_CREDENTIALS credentials{};
        credentials.dwVersion = SCH_CREDENTIALS_VERSION;
        // No client certificate, no weak algorithms; the certificate is this
        // end's to check (checkCertificate).
        credentials.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO | SCH_CRED_MANUAL_CRED_VALIDATION;
        credentials.cTlsParameters = 1;
        credentials.pTlsParameters = &parameters;
        TimeStamp expiry{};
        const SECURITY_STATUS status =
            AcquireCredentialsHandleW(nullptr, const_cast<SEC_WCHAR*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr,
                                      &credentials, nullptr, nullptr, &credentials_, &expiry);
        if (status != SEC_E_OK) {
            fail(RCS_ERROR_UNSUPPORTED, "Schannel offers no TLS 1.2 or later client here (" + codeOf(status) +
                                            "; it needs Windows 10 1809 or later)");
        }
        hasCredentials_ = true;
    }

    // One InitializeSecurityContext with all received_ holds (perhaps nothing):
    // keeps what it leaves and queues its token. SEC_E_OK, SEC_I_CONTINUE_NEEDED,
    // or SEC_E_INCOMPLETE_MESSAGE (received_ untouched: more must arrive).
    SECURITY_STATUS step() {
        SecBuffer input[2] = {
            {ULONG(received_.size()), SECBUFFER_TOKEN, received_.empty() ? nullptr : received_.data()},
            {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc inputs{SECBUFFER_VERSION, 2, input};
        Output output;
        ULONG attributes = 0;
        const SECURITY_STATUS status =
            InitializeSecurityContextW(&credentials_, &context_, target(), flags(), 0, 0, &inputs, 0, &context_,
                                       &output.desc, &attributes, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            stalled_ = true;
            return status;
        }
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
            failHandshake(status);
        }
        const auto token = output.token();
        sending_.insert(sending_.end(), token.begin(), token.end());
        keepExtra(input[1]);
        return status;
    }

    // Keeps the last bytes of received_ that a call left unread.
    void keepExtra(const SecBuffer& buffer) {
        if (buffer.BufferType == SECBUFFER_EXTRA && buffer.cbBuffer) {
            received_.erase(received_.begin(), received_.end() - ptrdiff_t(buffer.cbBuffer));
        } else {
            received_.clear();
        }
    }

    [[noreturn]] void failHandshake(SECURITY_STATUS status) const {
        if (const auto problem = certificateFailure(status)) {
            fail(RCS_ERROR_AUTHENTICATION, trustProblem(*problem, settings_, what_, codeOf(status)));
        }
        fail(RCS_ERROR_NETWORK,
             (open_ ? "The TLS connection to " + what_ + " failed: " : "No TLS connection to " + what_ + ": ") +
                 describe(status));
    }

    // With a pin, the certificate the server presented must be that one;
    // without, the system must trust it for serverName. Checked here, not by
    // Schannel: its own check may fetch missing intermediate certificates
    // for 15 s and more, which nothing cuts short; this one fetches them
    // within what is left of the handshake's time (none at `deadline` {}: only
    // what is cached, without waiting).
    void checkCertificate(Clock::time_point deadline) {
        PCCERT_CONTEXT certificate = nullptr;
        const SECURITY_STATUS status =
            QueryContextAttributesW(&context_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate);
        if (status != SEC_E_OK || !certificate) {
            fail(RCS_ERROR_AUTHENTICATION, "The certificate of " + what_ + " cannot be read (" + codeOf(status) + ")");
        }
        std::string problem;
        if (settings_.certificateSha256) {
            problem = pinProblem(std::span(certificate->pbCertEncoded, certificate->cbCertEncoded),
                                 *settings_.certificateSha256, what_, settings_.pinSetting);
        } else {
            problem = trustedProblem(certificate, deadline);
        }
        CertFreeCertificateContext(certificate);
        if (!problem.empty()) {
            fail(RCS_ERROR_AUTHENTICATION, problem);
        }
    }

    // Why the system does not trust `certificate` for serverName (no
    // revocation lookups: networks that pass little but TLS block them); empty when it does.
    // The SSL policy takes the subject's common name where a certificate has
    // no subject alternative name; as on the other systems, that is refused.
    std::string trustedProblem(PCCERT_CONTEXT certificate, Clock::time_point deadline) {
        if (!CertFindExtension(szOID_SUBJECT_ALT_NAME2, certificate->pCertInfo->cExtension,
                               certificate->pCertInfo->rgExtension)) {
            return trustProblem(TrustProblem::use, settings_, what_, "no subject alternative name");
        }
        LPSTR serverAuth[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
        CERT_CHAIN_PARA chainPara{};
        chainPara.cbSize = sizeof(chainPara);
        chainPara.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        chainPara.RequestedUsage.Usage.cUsageIdentifier = 1;
        chainPara.RequestedUsage.Usage.rgpszUsageIdentifier = serverAuth;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        chainPara.dwUrlRetrievalTimeout = DWORD(std::clamp<long long>(left, 1, 60'000));
        const DWORD flags = deadline == Clock::time_point{} ? CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL : 0;
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (!CertGetCertificateChain(nullptr, certificate, nullptr, certificate->hCertStore, &chainPara, flags,
                                     nullptr, &chain)) {
            return trustProblem(TrustProblem::other, settings_, what_, codeOf(SECURITY_STATUS(GetLastError())));
        }
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
        ssl.cbSize = sizeof(ssl);
        ssl.dwAuthType = AUTHTYPE_SERVER;
        ssl.pwszServerName = target();
        CERT_CHAIN_POLICY_PARA policy{};
        policy.cbSize = sizeof(policy);
        policy.pvExtraPolicyPara = &ssl;
        CERT_CHAIN_POLICY_STATUS verdict{};
        verdict.cbSize = sizeof(verdict);
        const BOOL checked = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &verdict);
        CertFreeCertificateChain(chain);
        const auto error = SECURITY_STATUS(checked ? verdict.dwError : GetLastError());
        if (!error) {
            return {};
        }
        return trustProblem(certificateFailure(error).value_or(TrustProblem::other), settings_, what_, codeOf(error));
    }

    // After a handshake: the record sizes, and (the first time) what it agreed on.
    void opened() {
        const SECURITY_STATUS status = QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
        if (status != SEC_E_OK) {
            fail(RCS_ERROR_NETWORK, "Schannel has no record sizes for " + what_ + " (" + codeOf(status) + ")");
        }
        if (open_) {
            return;
        }
        open_ = true;
        SecPkgContext_ConnectionInfo connection{};
        QueryContextAttributesW(&context_, SECPKG_ATTR_CONNECTION_INFO, &connection);
        SecPkgContext_ApplicationProtocol protocol{};
        QueryContextAttributesW(&context_, SECPKG_ATTR_APPLICATION_PROTOCOL, &protocol);
        const bool alpn = protocol.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success;
        log::info("network", std::string(connection.dwProtocol & SP_PROT_TLS1_3_CLIENT ? "TLS 1.3" : "TLS 1.2") +
                                 " to " + what_ +
                                 (settings_.certificateSha256
                                      ? ", the pinned certificate"
                                      : ", a certificate this system trusts for \"" + settings_.serverName + "\"") +
                                 (alpn ? ", ALPN " + std::string(reinterpret_cast<const char*>(protocol.ProtocolId),
                                                                 protocol.ProtocolIdSize)
                                       : ", no ALPN"));
    }

    // Decrypts the record at the start of received_ into plain_; false when
    // no whole one waits there.
    bool decrypt() {
        if (received_.empty() || stalled_) {
            return false;
        }
        SecBuffer buffers[4] = {{ULONG(received_.size()), SECBUFFER_DATA, received_.data()},
                                {0, SECBUFFER_EMPTY, nullptr},
                                {0, SECBUFFER_EMPTY, nullptr},
                                {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
        const SECURITY_STATUS status = DecryptMessage(&context_, &message, 0, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            stalled_ = true;
            return false;
        }
        if (status == SEC_I_CONTEXT_EXPIRED) {
            fail(RCS_ERROR_NETWORK, "The TLS connection to " + what_ + " was closed");
        }
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) {
            fail(RCS_ERROR_NETWORK, "Decrypting what " + what_ + " sent failed (" + describe(status) + ")");
        }
        SecBuffer extra{0, SECBUFFER_EMPTY, nullptr};
        for (const auto& buffer : buffers) {
            if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer) { // Decrypted in place: copied first.
                const auto* data = static_cast<const uint8_t*>(buffer.pvBuffer);
                plain_.insert(plain_.end(), data, data + buffer.cbBuffer);
            } else if (buffer.BufferType == SECBUFFER_EXTRA) {
                extra = buffer;
            }
        }
        keepExtra(extra);
        // A message other than data: Schannel takes it (and what follows) through
        // InitializeSecurityContext, at once, even with nothing more to give.
        renegotiating_ = callNow_ = status == SEC_I_RENEGOTIATE;
        return true;
    }

    // The handshake Schannel asked for, as far as what arrived takes it; false
    // when it waits for the server.
    bool renegotiate() {
        if (!callNow_ && (received_.empty() || stalled_)) {
            return false;
        }
        callNow_ = false;
        const SECURITY_STATUS status = step();
        flush();
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            return false;
        }
        if (status == SEC_E_OK) {
            renegotiating_ = false;
            checkCertificate({}); // A renegotiation may bring another certificate: checked from the cache.
            opened();
        }
        return true;
    }

    // Appends what the socket has; false when nothing arrived.
    bool receive() {
        uint8_t chunk[readChunk];
        const int result = ::recv(socket_.get(), reinterpret_cast<char*>(chunk), int(sizeof(chunk)), 0);
        if (result > 0) {
            received_.insert(received_.end(), chunk, chunk + result);
            stalled_ = false;
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

    // Writes what sending_ holds as far as the socket takes it; true once all went.
    bool flush() {
        size_t written = 0;
        while (written < sending_.size()) {
            const int chunk = int(std::min(sending_.size() - written, size_t(INT_MAX)));
            const int result =
                ::send(socket_.get(), reinterpret_cast<const char*>(sending_.data() + written), chunk, 0);
            if (result > 0) {
                written += size_t(result);
            } else if (result < 0 && wouldBlock()) {
                break;
            } else {
                fail(RCS_ERROR_NETWORK, "Sending to " + what_ + " failed: " + lastSocketErrorText());
            }
        }
        sending_.erase(sending_.begin(), sending_.begin() + ptrdiff_t(written));
        return sending_.empty();
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
    std::wstring target_;
    CredHandle credentials_{};
    CtxtHandle context_{};
    bool hasCredentials_ = false, hasContext_ = false;
    bool open_ = false; // The first handshake ended.
    SecPkgContext_StreamSizes sizes_{};
    std::vector<uint8_t> received_; // From the socket, not yet decrypted or taken by a handshake.
    bool stalled_ = false;          // received_ holds no whole record: more must arrive.
    std::vector<uint8_t> plain_;    // Decrypted, not yet read.
    size_t plainBegin_ = 0;
    std::vector<uint8_t> sending_; // For the socket, in order.
    bool renegotiating_ = false;   // From SEC_I_RENEGOTIATE until its handshake ends.
    bool callNow_ = false;         // That handshake's first call is due, input or not.
};

} // namespace

std::unique_ptr<ByteStream> connectTls(const Endpoint& server, const TlsSettings& settings, const std::string& what,
                                       CancelToken cancelled, std::chrono::milliseconds timeout,
                                       uint32_t interfaceIndex) {
    const auto deadline = Clock::now() + timeout;
    auto stream = std::make_unique<SchannelStream>(
        connectStreamTcp(server, what + " over TLS", cancelled, timeout, interfaceIndex), settings, what);
    stream->handshake(cancelled, deadline);
    return stream;
}

} // namespace rcsdk::transport
