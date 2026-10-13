// TLS to a server through Network.framework (transport/tls.hpp). The
// connection runs on a dispatch queue of its own: what arrives is kept here
// and a pipe, which the network thread's Waiter watches, is readable while
// any of it (or a failure) waits; what is sent is handed over at once, up to
// about what a TCP socket's send buffer would hold.
#include "platform/apple/tls_connection.hpp"

#include "platform/apple/cf.hpp"
#include "transport/socket_error.hpp"
#include "util/log.hpp"

#import <Foundation/Foundation.h>
#import <Network/Network.h>

#include <fcntl.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace rcsdk::transport {

using platform::apple::CFRef;

namespace {

// Received and not yet read: beyond this the stream stops asking for more, and
// TCP's window holds the server back.
constexpr size_t maxInbound = 256 * 1024;
// Handed to the connection and not yet sent: about what the socket's send
// buffer holds under TURN over TCP (turn_tcp_socket.cpp).
constexpr size_t maxSending = 1024 * 1024;

// What the connection's handlers (on its queue) share with the stream.
struct Shared {
    explicit Shared(std::string server) : what(std::move(server)) {
        if (::pipe(pipe) != 0) {
            fail(RCS_ERROR_INTERNAL, "Could not create the TLS stream's wake pipe");
        }
        for (const int end : pipe) {
            fcntl(end, F_SETFL, fcntl(end, F_GETFL) | O_NONBLOCK);
            fcntl(end, F_SETFD, FD_CLOEXEC);
        }
    }
    ~Shared() {
        for (const int end : pipe) {
            ::close(end);
        }
    }
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

    // Holding mutex: the pipe is readable while something waits for readSome().
    void wake() noexcept {
        const uint8_t byte = 1;
        [[maybe_unused]] const auto written = ::write(pipe[1], &byte, 1); // Full: readable already.
    }
    void drain() noexcept {
        uint8_t bytes[64];
        while (::read(pipe[0], bytes, sizeof(bytes)) > 0) {
        }
    }

    const std::string what;
    std::mutex mutex;
    std::condition_variable changed;
    nw_connection_state_t state = nw_connection_state_invalid;
    bool ready = false;  // connectTls() returned the stream.
    std::string failure; // Why the connection did not come about, or broke.
    std::string refused; // Why the certificate was refused (the verify block).
    std::vector<uint8_t> inbound;
    size_t begin = 0;       // What of inbound was read.
    bool receiving = false; // A receive is outstanding.
    bool closed = false;    // The server closed its side.
    size_t sending = 0;     // Handed to the connection, not yet sent.
    int pipe[2] = {-1, -1};
};

std::string utf8(CFStringRef text) {
    const char* bytes = text ? [(__bridge NSString*)text UTF8String] : nullptr;
    return bytes ? bytes : "";
}

// An error of the connection, in words.
std::string describe(nw_error_t error) {
    const int code = nw_error_get_error_code(error);
    switch (nw_error_get_error_domain(error)) {
    case nw_error_domain_posix:
        return describeSocketError(code);
    case nw_error_domain_tls: {
        const char* text = "TLS failed";
        switch (code) {
        case errSSLPeerProtocolVersion:
        case errSSLPeerHandshakeFail:
        case errSSLHandshakeFail:
            text = "no TLS version (1.2 or later) or cipher in common with the server";
            break;
        case errSSLProtocol:
        case errSSLRecordOverflow:
            text = "the server does not speak TLS there";
            break;
        case errSSLClosedAbort:
        case errSSLConnectionRefused:
            text = "the server closed the connection during the TLS handshake";
            break;
        default:
            break;
        }
        return std::string(text) + " (OSStatus " + std::to_string(code) + ")";
    }
    default: {
        CFRef<CFErrorRef> cf(nw_error_copy_cf_error(error));
        CFRef<CFStringRef> text(cf ? CFErrorCopyDescription(cf.value) : nullptr);
        return utf8(text.value) + " (" + std::to_string(code) + ")";
    }
    }
}

// Why a connection waits without an error: what its path lacks.
std::string pathProblem(nw_connection_t connection, bool restricted) {
    nw_path_t path = nw_connection_copy_current_path(connection);
    switch (path ? nw_path_get_unsatisfied_reason(path) : nw_path_unsatisfied_reason_not_available) {
    case nw_path_unsatisfied_reason_local_network_denied:
        return "this app may not reach the local network (System Settings > Privacy & Security > Local Network)";
    case nw_path_unsatisfied_reason_vpn_inactive:
        return "the VPN its path needs is not connected";
    case nw_path_unsatisfied_reason_cellular_denied:
    case nw_path_unsatisfied_reason_wifi_denied:
        return "this app may not use the network that reaches it";
    default:
        return restricted ? "no path to it through local_interface" : "no path to it";
    }
}

// The interface Network.framework knows by this index, or nil when the path it
// reports by `deadline` has none: it takes an interface object, never an
// index, and only a path lists them.
nw_interface_t interfaceAt(uint32_t index, Clock::time_point deadline) {
    dispatch_queue_t queue = dispatch_queue_create("rcsdk.tls.interfaces", DISPATCH_QUEUE_SERIAL);
    nw_path_monitor_t monitor = nw_path_monitor_create();
    nw_path_monitor_set_queue(monitor, queue);
    dispatch_semaphore_t listed = dispatch_semaphore_create(0);
    __block nw_interface_t found = nil;
    nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
        nw_path_enumerate_interfaces(path, ^bool(nw_interface_t interface) {
            if (nw_interface_get_index(interface) == index) {
                found = interface;
            }
            return found == nil;
        });
        dispatch_semaphore_signal(listed);
    });
    nw_path_monitor_start(monitor);
    const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
    dispatch_semaphore_wait(listed, dispatch_time(DISPATCH_TIME_NOW, std::max<int64_t>(wait, 0)));
    nw_path_monitor_cancel(monitor);
    __block nw_interface_t result = nil;
    dispatch_sync(queue, ^{ // After an update the handler may still be in.
        result = found;
    });
    return result;
}

// Asks the connection for what arrives next, once; the completion asks again
// until the stream holds maxInbound unread (readSome() resumes it).
void receiveMore(std::shared_ptr<Shared> shared, nw_connection_t connection) {
    nw_connection_receive(
        connection, 1, 64 * 1024,
        ^(dispatch_data_t content, nw_content_context_t context, bool complete, nw_error_t error) {
            bool again = false;
            {
                std::lock_guard lock(shared->mutex);
                if (content) {
                    dispatch_data_apply(content, ^bool(dispatch_data_t, size_t, const void* bytes, size_t size) {
                        const auto* data = static_cast<const uint8_t*>(bytes);
                        shared->inbound.insert(shared->inbound.end(), data, data + size);
                        return true;
                    });
                }
                if (error) {
                    if (shared->failure.empty()) {
                        shared->failure = "Receiving from " + shared->what + " failed: " + describe(error);
                    }
                } else if (complete && (!context || nw_content_context_get_is_final(context))) {
                    shared->closed = true;
                } else {
                    again = shared->inbound.size() - shared->begin < maxInbound;
                }
                shared->receiving = again;
                shared->wake();
            }
            if (again) {
                receiveMore(shared, connection);
            }
        });
}

class NetworkStream final : public ByteStream {
  public:
    NetworkStream(nw_connection_t connection, std::shared_ptr<Shared> shared, Endpoint local)
        : connection_(connection), shared_(std::move(shared)), local_(std::move(local)) {}
    ~NetworkStream() override { nw_connection_cancel(connection_); }

    size_t writeSome(std::span<const uint8_t> bytes) override {
        {
            std::lock_guard lock(shared_->mutex);
            if (!shared_->failure.empty()) {
                fail(RCS_ERROR_NETWORK, shared_->failure);
            }
            if (shared_->sending >= maxSending) {
                return 0;
            }
        }
        send(bytes);
        return bytes.size();
    }

    // Cancelling the connection drops what it still holds: `rest` joins it
    // (beyond maxSending), and all of it is waited for.
    void close(std::span<const uint8_t> rest, Clock::time_point deadline) override {
        if (!rest.empty()) {
            send(rest);
        }
        std::unique_lock lock(shared_->mutex);
        shared_->changed.wait_until(lock, deadline, [&] { return shared_->sending == 0 || !shared_->failure.empty(); });
    }

    size_t readSome(std::span<uint8_t> buffer) override {
        size_t taken = 0;
        bool resume = false;
        {
            std::lock_guard lock(shared_->mutex);
            auto& s = *shared_;
            const size_t waiting = s.inbound.size() - s.begin;
            if (!waiting) {
                if (!s.failure.empty()) {
                    fail(RCS_ERROR_NETWORK, s.failure);
                }
                if (s.closed) {
                    fail(RCS_ERROR_NETWORK, "The TLS connection to " + s.what + " was closed");
                }
                s.drain();
                return 0;
            }
            taken = std::min(waiting, buffer.size());
            std::memcpy(buffer.data(), s.inbound.data() + s.begin, taken);
            s.begin += taken;
            if (s.begin == s.inbound.size() || s.begin >= maxInbound) { // What was read does not pile up.
                s.inbound.erase(s.inbound.begin(), s.inbound.begin() + ptrdiff_t(s.begin));
                s.begin = 0;
            }
            resume = !s.receiving && !s.closed && s.failure.empty() && s.inbound.size() - s.begin < maxInbound;
            s.receiving = s.receiving || resume;
        }
        if (resume) {
            receiveMore(shared_, connection_);
        }
        return taken;
    }

    Handle handle() const noexcept override { return shared_->pipe[0]; }

    bool buffered() const noexcept override {
        std::lock_guard lock(shared_->mutex);
        return shared_->inbound.size() > shared_->begin;
    }

    Endpoint local() const override { return local_; }

  private:
    void send(std::span<const uint8_t> bytes) {
        const size_t size = bytes.size();
        {
            std::lock_guard lock(shared_->mutex);
            shared_->sending += size;
        }
        const auto shared = shared_;
        dispatch_data_t content = dispatch_data_create(bytes.data(), size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        nw_connection_send(connection_, content, NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false, ^(nw_error_t error) {
            std::lock_guard lock(shared->mutex);
            shared->sending -= size;
            if (error && shared->failure.empty()) {
                shared->failure = "Sending to " + shared->what + " failed: " + describe(error);
                shared->wake();
            }
            shared->changed.notify_all();
        });
    }

    nw_connection_t connection_;
    std::shared_ptr<Shared> shared_;
    Endpoint local_;
};

std::string versionName(tls_protocol_version_t version) {
    return version == tls_protocol_version_TLSv13   ? "TLS 1.3"
           : version == tls_protocol_version_TLSv12 ? "TLS 1.2"
                                                    : "TLS";
}

} // namespace

std::string certificateProblem(SecTrustRef trust, const TlsSettings& settings, const std::string& what) {
    if (settings.certificateSha256) {
        CFRef<CFArrayRef> chain(SecTrustCopyCertificateChain(trust));
        if (!chain || CFArrayGetCount(chain.value) == 0) {
            return "The certificate of " + what + " is missing";
        }
        const auto leaf = static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(chain.value, 0)));
        CFRef<CFDataRef> der(SecCertificateCopyData(leaf));
        return pinProblem(std::span<const uint8_t>(CFDataGetBytePtr(der.value), size_t(CFDataGetLength(der.value))),
                          *settings.certificateSha256, what, settings.pinSetting);
    }
    CFRef<CFStringRef> name(CFStringCreateWithCString(nullptr, settings.serverName.c_str(), kCFStringEncodingUTF8));
    CFRef<SecPolicyRef> policy(SecPolicyCreateSSL(true, name.value));
    SecTrustSetPolicies(trust, policy.value);
    SecTrustSetNetworkFetchAllowed(trust, false);
    CFRef<CFErrorRef> error;
    if (SecTrustEvaluateWithError(trust, error.out())) {
        return {};
    }
    const CFIndex code = error ? CFErrorGetCode(error.value) : 0;
    CFRef<CFStringRef> text(error ? CFErrorCopyDescription(error.value) : nullptr);
    const std::string detail = "OSStatus " + std::to_string(code) + ": " + utf8(text.value);
    switch (code) {
    case errSecNotTrusted:
    case errSecCreateChainFailed:
        return trustProblem(TrustProblem::untrusted, settings, what, detail);
    case errSecHostNameMismatch:
        return trustProblem(TrustProblem::name, settings, what, detail);
    case errSecCertificateExpired:
    case errSecCertificateNotValidYet:
        return trustProblem(TrustProblem::dates, settings, what, detail);
    case errSecInvalidExtendedKeyUsage:
        return trustProblem(TrustProblem::use, settings, what, detail);
    default:
        return trustProblem(TrustProblem::other, settings, what, detail);
    }
}

std::unique_ptr<ByteStream> connectTls(const Endpoint& server, const TlsSettings& settings, const std::string& what,
                                       CancelToken cancelled, std::chrono::milliseconds timeout,
                                       uint32_t interfaceIndex) {
    const auto deadline = Clock::now() + timeout;
    auto shared = std::make_shared<Shared>(what);
    dispatch_queue_t queue = dispatch_queue_create("rcsdk.tls", DISPATCH_QUEUE_SERIAL);
    const TlsSettings checks = settings;
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(
        ^(nw_protocol_options_t tlsOptions) {
            sec_protocol_options_t options = nw_tls_copy_sec_protocol_options(tlsOptions);
            sec_protocol_options_set_min_tls_protocol_version(options, tls_protocol_version_TLSv12);
            sec_protocol_options_add_tls_application_protocol(options, checks.alpn.c_str());
            // Every connection checks the certificate under the settings of
            // the moment: a resumed session would skip it.
            sec_protocol_options_set_tls_resumption_enabled(options, false);
            if (!checks.serverName.empty()) {
                sec_protocol_options_set_tls_server_name(options, checks.serverName.c_str());
            }
            sec_protocol_options_set_verify_block(
                options,
                ^(sec_protocol_metadata_t, sec_trust_t trust, sec_protocol_verify_complete_t complete) {
                    CFRef<SecTrustRef> ref(sec_trust_copy_ref(trust));
                    std::string problem = certificateProblem(ref.value, checks, shared->what);
                    const bool accepted = problem.empty();
                    if (!accepted) {
                        std::lock_guard lock(shared->mutex);
                        shared->refused = std::move(problem);
                    }
                    complete(accepted);
                },
                queue);
        },
        ^(nw_protocol_options_t tcpOptions) {
            nw_tcp_options_set_no_delay(tcpOptions, true);
        });
    nw_parameters_set_prefer_no_proxy(parameters, true); // As TURN over TCP and Schannel: straight to the server.
    if (interfaceIndex) {
        nw_interface_t interface = interfaceAt(interfaceIndex, deadline);
        if (!interface) {
            fail(RCS_ERROR_NETWORK, "The interface of local_interface (index " + std::to_string(interfaceIndex) +
                                        ") has no path to " + what + " over TLS");
        }
        nw_parameters_require_interface(parameters, interface);
    }
    sockaddr_storage address{}; // With the length BSD keeps in it, which a parsed address lacks.
    std::memcpy(&address, server.get(), server.size());
    reinterpret_cast<sockaddr*>(&address)->sa_len = uint8_t(server.size());
    nw_connection_t connection =
        nw_connection_create(nw_endpoint_create_address(reinterpret_cast<const sockaddr*>(&address)), parameters);
    nw_connection_set_queue(connection, queue);
    nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t error) {
        std::lock_guard lock(shared->mutex);
        shared->state = state;
        if (error && shared->failure.empty()) {
            shared->failure = shared->ready ? "The TLS connection to " + shared->what + " broke: " + describe(error)
                                            : describe(error);
        }
        shared->changed.notify_all();
        shared->wake();
    });
    nw_connection_start(connection);

    // Network.framework waits for a better network instead of failing
    // (`waiting`); here that is a failure, as a refused TCP connection is.
    {
        std::unique_lock lock(shared->mutex);
        while (shared->state != nw_connection_state_ready) {
            const bool over = shared->state == nw_connection_state_waiting ||
                              shared->state == nw_connection_state_failed ||
                              shared->state == nw_connection_state_cancelled;
            if (over || cancelled.load() || Clock::now() >= deadline) {
                const std::string refused = shared->refused;
                std::string failure = shared->failure;
                lock.unlock();
                if (failure.empty()) {
                    failure = pathProblem(connection, interfaceIndex != 0);
                }
                nw_connection_cancel(connection);
                throwIfCancelled(cancelled);
                if (!refused.empty()) {
                    fail(RCS_ERROR_AUTHENTICATION, refused);
                }
                if (over) {
                    fail(RCS_ERROR_NETWORK, "No TLS connection to " + what + ": " + failure);
                }
                const auto ms = timeout.count();
                fail(RCS_ERROR_NETWORK,
                     "No TLS connection to " + what + " within " +
                         (ms % 1000 ? std::to_string(ms) + " ms" : std::to_string(ms / 1000) + " s") +
                         " (nothing answers there, a firewall drops it, or it speaks no TLS)");
            }
            shared->changed.wait_for(lock, 20ms);
        }
        shared->failure.clear(); // From a `waiting` this loop did not see before it was ready.
        shared->ready = shared->receiving = true;
    }

    Endpoint local;
    std::string version = "TLS", alpn;
    if (nw_path_t path = nw_connection_copy_current_path(connection)) {
        if (nw_endpoint_t endpoint = nw_path_copy_effective_local_endpoint(path)) {
            if (const sockaddr* address = nw_endpoint_get_address(endpoint)) {
                local = Endpoint::fromSockaddr(address, address->sa_len);
            }
        }
    }
    if (nw_protocol_metadata_t metadata =
            nw_connection_copy_protocol_metadata(connection, nw_protocol_copy_tls_definition())) {
        sec_protocol_metadata_t security = nw_tls_copy_sec_protocol_metadata(metadata);
        version = versionName(sec_protocol_metadata_get_negotiated_tls_protocol_version(security));
        if (@available(macOS 15.5, iOS 18.5, *)) {
            if (char* negotiated = const_cast<char*>(sec_protocol_metadata_copy_negotiated_protocol(security))) {
                alpn = negotiated;
                free(negotiated);
            }
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // Its replacement came with macOS 15.5 and iOS 18.5.
            if (const char* negotiated = sec_protocol_metadata_get_negotiated_protocol(security)) {
                alpn = negotiated;
            }
#pragma clang diagnostic pop
        }
    }
    log::info("network", version + " to " + what +
                             (settings.certificateSha256
                                  ? ", the pinned certificate"
                                  : ", a certificate this system trusts for \"" + settings.serverName + "\"") +
                             (alpn.empty() ? ", no ALPN" : ", ALPN " + alpn));
    receiveMore(shared, connection);
    return std::make_unique<NetworkStream>(connection, std::move(shared), local);
}

} // namespace rcsdk::transport
