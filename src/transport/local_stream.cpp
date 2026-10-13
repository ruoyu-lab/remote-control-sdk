#include "transport/local_stream.hpp"

#include "transport/socket.hpp"
#include "transport/socket_error.hpp"
#include "transport/tcp_stream.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "platform/windows/win.hpp"

#include <aclapi.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
// macOS names a socket's peer by its audit token, which tells one process
// apart from another that reuses its ID or the program it ran before exec;
// iOS has no public libproc.
#if defined(__APPLE__) && TARGET_OS_OSX
#define RCSDK_PEER_TOKEN 1
#include <bsm/libbsm.h>
#include <libproc.h>
#endif
#endif

namespace rcsdk::transport {

#ifdef _WIN32

namespace win = platform::win;

std::string endpointProblem(const std::string& endpoint) {
    if (endpoint.empty() || endpoint.size() > 200 || endpoint.find('\\') != std::string::npos ||
        endpoint.find('/') != std::string::npos) {
        return "A pipe name is 1..200 characters, without \\ or /";
    }
    return {};
}

namespace {

constexpr DWORD pipeBuffer = 256 * 1024;

std::wstring pipeName(const std::string& endpoint) {
    if (const auto problem = endpointProblem(endpoint); !problem.empty()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, problem);
    }
    return L"\\\\.\\pipe\\" + win::wide(endpoint, "A pipe name");
}

// ---- Who is at the other end -------------------------------------------------------------------

// The account a SID names, "DOMAIN\user"; the SID itself where it has no name.
std::string accountOf(PSID sid) {
    DWORD nameSize = 0, domainSize = 0;
    SID_NAME_USE use{};
    LookupAccountSidW(nullptr, sid, nullptr, &nameSize, nullptr, &domainSize, &use);
    std::wstring name(nameSize, L'\0'), domain(domainSize, L'\0');
    if (nameSize && LookupAccountSidW(nullptr, sid, name.data(), &nameSize, domain.data(), &domainSize, &use)) {
        name.resize(nameSize);
        domain.resize(domainSize);
        return win::narrow(domain.empty() ? name : domain + L"\\" + name);
    }
    return win::narrow(win::sidText(sid));
}

void describe(PSID sid, bool administrator, LocalPeer& peer) {
    peer.user = win::narrow(win::sidText(sid));
    peer.account = accountOf(sid);
    peer.privileged = IsWellKnownSid(sid, WinLocalSystemSid) || administrator;
}

// An administrator now: the token has the Administrators group enabled (it is
// elevated, or UAC is off), not for denying only.
bool administrator(HANDLE token) {
    const auto buffer = win::tokenInformation(token, TokenGroups);
    if (buffer.empty()) {
        return false;
    }
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        const auto& group = groups->Groups[i];
        if ((group.Attributes & SE_GROUP_ENABLED) && !(group.Attributes & SE_GROUP_USE_FOR_DENY_ONLY) &&
            IsWellKnownSid(group.Sid, WinBuiltinAdministratorsSid)) {
            return true;
        }
    }
    return false;
}

// The image `process` runs; empty where this process may not read it.
std::string programOf(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD size = DWORD(path.size());
    if (!process || !QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
        return {};
    }
    path.resize(size);
    return win::narrow(path);
}

void describeToken(HANDLE token, LocalPeer& peer) {
    const auto buffer = win::tokenInformation(token, TokenUser);
    if (buffer.empty()) {
        const DWORD error = GetLastError();
        fail(RCS_ERROR_AUTHENTICATION, "The account of process " + std::to_string(peer.processId) +
                                           " cannot be read (" + win::errorText(error) + ")");
    }
    describe(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid, administrator(token), peer);
}

// The client of a connected pipe instance, as the system recorded it when it
// opened the pipe: identifying it is all the client allows, and all this needs.
LocalPeer clientOf(HANDLE pipe) {
    LocalPeer peer;
    ULONG process = 0, session = 0;
    if (!GetNamedPipeClientProcessId(pipe, &process) || !GetNamedPipeClientSessionId(pipe, &session)) {
        const DWORD error = GetLastError();
        fail(RCS_ERROR_AUTHENTICATION, "The pipe's client cannot be told (" + win::errorText(error) + ")");
    }
    peer.processId = process;
    peer.osSession = session;
    if (!ImpersonateNamedPipeClient(pipe)) {
        const DWORD error = GetLastError();
        fail(RCS_ERROR_AUTHENTICATION, "The account of process " + std::to_string(process) +
                                           " cannot be read (" + win::errorText(error) + ")");
    }
    HANDLE token = nullptr;
    const BOOL opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token);
    const DWORD error = GetLastError();
    if (!RevertToSelf()) {
        std::terminate(); // This thread must not go on as its client.
    }
    if (!opened) {
        fail(RCS_ERROR_AUTHENTICATION, "The account of process " + std::to_string(process) +
                                           " cannot be read (" + win::errorText(error) + ")");
    }
    const win::Handle owned(token);
    describeToken(token, peer);
    peer.program = programOf(win::Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process)).get());
    return peer;
}

// The process serving a pipe: its token where this process may read it. Where
// it may not (the server runs as a more privileged account), the pipe's owner,
// which only the pipe's creator can have set: its own account, or the
// administrators' group for LocalSystem and an elevated administrator.
LocalPeer serverOf(HANDLE pipe) {
    LocalPeer peer;
    ULONG process = 0, session = 0;
    if (!GetNamedPipeServerProcessId(pipe, &process) || !GetNamedPipeServerSessionId(pipe, &session)) {
        const DWORD error = GetLastError();
        fail(RCS_ERROR_AUTHENTICATION, "The pipe's server cannot be told (" + win::errorText(error) + ")");
    }
    peer.processId = process;
    peer.osSession = session;
    const win::Handle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process));
    peer.program = programOf(handle.get());
    HANDLE token = nullptr;
    if (handle && OpenProcessToken(handle.get(), TOKEN_QUERY, &token)) {
        const win::Handle owned(token);
        describeToken(token, peer);
        return peer;
    }
    if (const DWORD error = GetLastError(); error != ERROR_ACCESS_DENIED) {
        fail(RCS_ERROR_AUTHENTICATION, "The account of process " + std::to_string(process) +
                                           " cannot be read (" + win::errorText(error) + ")");
    }
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
                                         nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        fail(RCS_ERROR_AUTHENTICATION, "The owner of the pipe process " + std::to_string(process) +
                                           " serves cannot be read (" + win::errorText(status) + ")");
    }
    const std::unique_ptr<void, decltype(&LocalFree)> owned(descriptor, &LocalFree);
    describe(owner, IsWellKnownSid(owner, WinBuiltinAdministratorsSid), peer);
    return peer;
}

// ---- The stream --------------------------------------------------------------------------------

win::Handle manualEvent() {
    win::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        fail(RCS_ERROR_NETWORK, "Could not create an event: " + win::errorText(GetLastError()));
    }
    return event;
}

[[noreturn]] void failTransfer(DWORD error) {
    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA ||
        error == ERROR_HANDLE_EOF) {
        fail(RCS_ERROR_DISCONNECTED, "The peer closed the connection");
    }
    fail(RCS_ERROR_NETWORK, "Pipe transfer failed: " + win::errorText(error));
}

// One end of a pipe. A read and a write may run at once, on different threads.
// Cancellation ends a transfer before its first byte (CANCELLED); one under way
// is finished first, for at most 250 ms after the cancellation, so that a
// record is never cut short (as TcpStream does).
class PipeStream final : public Stream {
public:
    PipeStream(win::Handle pipe, CancelToken cancelled)
        : pipe_(std::move(pipe)), cancelled_(cancelled), readEvent_(manualEvent()), writeEvent_(manualEvent()) {}

    void read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) override {
        transfer(buffer.data(), buffer.size(), false, timeout);
    }
    void write(std::span<const uint8_t> buffer, std::chrono::milliseconds timeout) override {
        transfer(const_cast<uint8_t*>(buffer.data()), buffer.size(), true, timeout);
    }
    void writeFinal(std::span<const uint8_t> buffer, std::chrono::milliseconds timeout) override {
        transfer(const_cast<uint8_t*>(buffer.data()), buffer.size(), true, timeout, true);
    }
    uint64_t bytesTransferred() const noexcept override { return transferred_.load(); }

private:
    // `final`: cancellation does not end the transfer.
    void transfer(uint8_t* data, size_t size, bool write, std::chrono::milliseconds timeout, bool final = false) {
        const auto deadline = Clock::now() + timeout;
        std::optional<Clock::time_point> finishBy;
        const HANDLE event = (write ? writeEvent_ : readEvent_).get();
        for (size_t done = 0; done < size;) {
            if (!final && !done) {
                throwIfCancelled(cancelled_);
            }
            OVERLAPPED overlapped{};
            overlapped.hEvent = event;
            ResetEvent(event);
            const DWORD chunk = DWORD(std::min<size_t>(size - done, 1u << 20));
            const BOOL started = write ? WriteFile(pipe_.get(), data + done, chunk, nullptr, &overlapped)
                                       : ReadFile(pipe_.get(), data + done, chunk, nullptr, &overlapped);
            if (!started && GetLastError() != ERROR_IO_PENDING) {
                failTransfer(GetLastError());
            }
            const DWORD moved = finish(overlapped, final, done == 0, deadline, finishBy);
            if (!moved) {
                fail(RCS_ERROR_DISCONNECTED, "The peer closed the connection");
            }
            done += moved;
        }
        transferred_ += size;
    }

    // Waits for one operation of a transfer and returns the bytes it moved.
    // Cancellation and the deadline end it; what it moved before that took
    // effect still counts, so CANCELLED means that nothing of the transfer moved.
    DWORD finish(OVERLAPPED& overlapped, bool final, bool first, Clock::time_point deadline,
                 std::optional<Clock::time_point>& finishBy) {
        while (WaitForSingleObject(overlapped.hEvent, 20) != WAIT_OBJECT_0) {
            const auto now = Clock::now();
            rcs_result why = RCS_OK;
            if (!final && cancelled_.load()) {
                if (first) {
                    why = RCS_ERROR_CANCELLED;
                } else if (now >= (finishBy ? *finishBy : *(finishBy = now + 250ms))) {
                    why = RCS_ERROR_NETWORK;
                }
            }
            if (why == RCS_OK && now >= deadline) {
                why = RCS_ERROR_NETWORK;
            }
            if (why == RCS_OK) {
                continue;
            }
            CancelIoEx(pipe_.get(), &overlapped);
            DWORD moved = 0;
            if (GetOverlappedResult(pipe_.get(), &overlapped, &moved, TRUE) || moved) {
                return moved; // It completed, or moved bytes, before the cancellation took effect.
            }
            if (const DWORD error = GetLastError(); error != ERROR_OPERATION_ABORTED) {
                failTransfer(error);
            }
            if (why == RCS_ERROR_CANCELLED) {
                fail(RCS_ERROR_CANCELLED, "Operation cancelled");
            }
            fail(RCS_ERROR_NETWORK, first ? "Pipe operation timed out (no data from the peer)"
                                          : "Pipe transfer cut short (the peer takes no data)");
        }
        DWORD moved = 0;
        if (!GetOverlappedResult(pipe_.get(), &overlapped, &moved, FALSE)) {
            failTransfer(GetLastError());
        }
        return moved;
    }

    win::Handle pipe_;
    CancelToken cancelled_;
    win::Handle readEvent_, writeEvent_;
    std::atomic<uint64_t> transferred_{0};
};

} // namespace

const LocalPeer& thisProcess() {
    static const LocalPeer self = [] {
        LocalPeer peer;
        peer.processId = GetCurrentProcessId();
        DWORD session = 0;
        peer.osSession = ProcessIdToSessionId(peer.processId, &session) ? session : 0;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            fail(RCS_ERROR_INTERNAL, "This process's token cannot be read (" + win::errorText(GetLastError()) + ")");
        }
        const win::Handle owned(token);
        describeToken(token, peer);
        peer.program = programOf(GetCurrentProcess());
        return peer;
    }();
    return self;
}

namespace {

// Who has the pipe `name`, for a reason: its server, where this process may
// ask (the pipe's access setting may not let it).
std::string holderOf(const std::wstring& name) {
    const win::Handle pipe(CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
                                       SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (pipe) {
        try {
            const LocalPeer server = serverOf(pipe.get());
            return "process " + std::to_string(server.processId) + " of " + server.account;
        } catch (const Failure&) { // It is already the reason for something else.
        }
    }
    return "another process";
}

} // namespace

struct LocalListener::Impl {
    std::wstring name;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    win::Handle pending; // The instance that waits for the next client.
    bool first = true;

    ~Impl() {
        if (descriptor) {
            LocalFree(descriptor);
        }
    }

    // The first instance claims the name: another process that has it fails us.
    win::Handle create() {
        const DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        win::Handle pipe(CreateNamedPipeW(name.c_str(), flags,
                                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                          PIPE_UNLIMITED_INSTANCES, pipeBuffer, pipeBuffer, 0, &attributes));
        if (!pipe) {
            const DWORD error = GetLastError();
            // ACCESS_DENIED for the first instance: the name is taken.
            fail(RCS_ERROR_NETWORK, "Could not create the pipe " + win::narrow(name) + ": " +
                                        (first && error == ERROR_ACCESS_DENIED ? holderOf(name) + " has the name"
                                                                               : win::errorText(error)));
        }
        first = false;
        return pipe;
    }
};

LocalListener::LocalListener(const std::string& endpoint, LocalAccess access) : impl_(std::make_unique<Impl>()) {
    impl_->name = pipeName(endpoint);
    // The user this process runs as, SYSTEM and administrators; with anyUser,
    // every authenticated user too, allowed to read and write but not to add
    // instances of the pipe (FILE_CREATE_PIPE_INSTANCE), so none can serve in
    // the session's place.
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + win::wide(thisProcess().user) + L")";
    if (access == LocalAccess::anyUser) {
        sddl += L"(A;;0x0012018b;;;AU)"; // FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES
    }
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &impl_->descriptor,
                                                              nullptr)) {
        fail(RCS_ERROR_NETWORK, "Could not build the pipe's access control: " + win::errorText(GetLastError()));
    }
    impl_->attributes = {sizeof(SECURITY_ATTRIBUTES), impl_->descriptor, FALSE};
    impl_->pending = impl_->create();
}

LocalListener::~LocalListener() = default;

// While the listener lives an instance of the pipe always exists: with none,
// the name would be free for any process to take. So an instance is replaced
// by creating the next one first.
LocalConnection LocalListener::accept(CancelToken listening, CancelToken stream) {
    auto& i = *impl_;
    const win::Handle event = manualEvent();
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    if (!ConnectNamedPipe(i.pending.get(), &overlapped)) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            while (WaitForSingleObject(event.get(), 20) != WAIT_OBJECT_0) {
                if (listening.load()) {
                    CancelIoEx(i.pending.get(), &overlapped);
                    DWORD ignored = 0;
                    GetOverlappedResult(i.pending.get(), &overlapped, &ignored, TRUE);
                    fail(RCS_ERROR_CANCELLED, "Operation cancelled");
                }
            }
            DWORD ignored = 0;
            if (!GetOverlappedResult(i.pending.get(), &overlapped, &ignored, FALSE)) {
                const DWORD failed = GetLastError();
                i.pending = i.create();
                fail(failed == ERROR_NO_DATA ? RCS_ERROR_DISCONNECTED : RCS_ERROR_NETWORK,
                     "Waiting for a client failed: " + win::errorText(failed));
            }
        } else if (error != ERROR_PIPE_CONNECTED) { // That one: the client came before this call.
            i.pending = i.create();
            fail(error == ERROR_NO_DATA ? RCS_ERROR_DISCONNECTED : RCS_ERROR_NETWORK,
                 "Waiting for a client failed: " + win::errorText(error));
        }
    }
    win::Handle connected = i.create(); // Clients that come next find an instance at once.
    std::swap(connected, i.pending);
    LocalPeer peer = clientOf(connected.get());
    return {std::make_unique<PipeStream>(std::move(connected), stream), std::move(peer), [] {}};
}

LocalConnection connectLocal(const std::string& endpoint, CancelToken cancelled, std::chrono::milliseconds timeout) {
    const std::wstring name = pipeName(endpoint);
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        throwIfCancelled(cancelled);
        // Only what a client needs: GENERIC_WRITE would also ask for
        // FILE_CREATE_PIPE_INSTANCE, which the endpoint gives no other user
        // (LocalListener). The server may identify this client, never act as it.
        win::Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr,
                                     OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (pipe) {
            LocalPeer peer = serverOf(pipe.get());
            return {std::make_unique<PipeStream>(std::move(pipe), cancelled), std::move(peer), {}};
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY && Clock::now() < deadline) {
            WaitNamedPipeW(name.c_str(), 50);
            continue;
        }
        if (error == ERROR_ACCESS_DENIED) {
            fail(RCS_ERROR_PERMISSION_REQUIRED, "The system refuses access to the endpoint (its access setting "
                                                "decides who may connect)");
        }
        fail(RCS_ERROR_NETWORK, "Could not connect to the endpoint: " + win::errorText(error));
    }
}

#else

std::string endpointProblem(const std::string& endpoint) {
    if (endpoint.empty() || endpoint[0] != '/' || endpoint.back() == '/' ||
        endpoint.size() >= sizeof(sockaddr_un::sun_path)) {
        return "A socket path is absolute, names a file and is shorter than " +
               std::to_string(sizeof(sockaddr_un::sun_path)) + " characters";
    }
    return {};
}

namespace {

std::string errnoText(int code) {
    return std::generic_category().message(code) + " (" + std::to_string(code) + ")";
}

sockaddr_un addressOf(const std::string& endpoint) {
    sockaddr_un address{};
    if (const auto problem = endpointProblem(endpoint); !problem.empty()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, problem);
    }
    address.sun_family = AF_UNIX;
    std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
    return address;
}

Socket unixSocket() {
    Socket socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (socket.get() == invalidHandle) {
        fail(RCS_ERROR_NETWORK, "Could not create a socket: " + describeSocketError(lastSocketError()));
    }
    makeNonBlocking(socket.get());
    return socket;
}

std::string userName(uid_t uid) {
    const long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(size_t(size > 0 ? size : 16384));
    passwd entry{};
    passwd* found = nullptr;
    if (getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &found) == 0 && found && found->pw_name) {
        return found->pw_name;
    }
    return std::to_string(uid);
}

LocalPeer describe(pid_t process, uid_t uid) {
    LocalPeer peer;
    peer.processId = uint32_t(process);
    peer.user = std::to_string(uid);
    peer.account = userName(uid);
    peer.privileged = uid == 0;
    return peer;
}

#ifndef __EMSCRIPTEN__
[[noreturn]] void failPeer(int error) {
    if (error == ENOTCONN) {
        fail(RCS_ERROR_DISCONNECTED, "The process at the other end left before it could be told");
    }
    fail(RCS_ERROR_AUTHENTICATION, "The process at the other end cannot be told: " + errnoText(error));
}
#endif

#ifdef RCSDK_PEER_TOKEN
// The audit token of the process at the other end: the one that used the
// connection last, as it is now (its process ID and the version of it that
// each exec renews).
audit_token_t peerToken(int socket) {
    audit_token_t token{};
    socklen_t size = sizeof(token);
    if (getsockopt(socket, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &size) != 0) {
        failPeer(errno);
    }
    return token;
}
#endif

// The process at the other end of a connected socket, as the system names it:
// the client as it connected (for a client: the server as it listened). A
// listener's (`confirm`: LocalConnection::confirm for it) on macOS: the
// process that used the connection last, by its audit token, and the program
// that token's process runs.
LocalPeer peerOf(int socket, std::function<void()>* confirm = nullptr) {
    uid_t uid = 0;
    pid_t process = 0;
#if defined(__EMSCRIPTEN__)
    (void)socket;
    fail(RCS_ERROR_UNSUPPORTED, "A browser has no local endpoints");
#elif defined(__linux__)
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) {
        failPeer(errno);
    }
    uid = credentials.uid;
    process = credentials.pid;
#else
    gid_t group = 0;
    if (getpeereid(socket, &uid, &group) != 0) {
        failPeer(errno);
    }
#ifdef RCSDK_PEER_TOKEN
    if (confirm) { // A client the listener accepted (a server it connected to has no token until it accepts).
        const audit_token_t token = peerToken(socket);
        LocalPeer peer = describe(audit_token_to_pid(token), uid);
        char program[PROC_PIDPATHINFO_MAXSIZE]{};
        audit_token_t asked = token;
        if (proc_pidpath_audittoken(&asked, program, sizeof(program)) <= 0) { // Ended, or ran another program.
            failPeer(ENOTCONN);
        }
        peer.program = program;
        *confirm = [socket, token] {
            const audit_token_t now = peerToken(socket);
            const pid_t was = audit_token_to_pid(token), is = audit_token_to_pid(now);
            if (is != was || audit_token_to_pidversion(now) != audit_token_to_pidversion(token)) {
                fail(RCS_ERROR_AUTHENTICATION,
                     "Process " + std::to_string(was) + " connected, and since then " +
                         (is == was ? std::string("it runs another program")
                                    : "process " + std::to_string(is) + " used the connection"));
            }
        };
        return peer;
    }
#endif
#ifdef LOCAL_PEERPID
    socklen_t size = sizeof(process);
    if (getsockopt(socket, SOL_LOCAL, LOCAL_PEERPID, &process, &size) != 0) {
        failPeer(errno);
    }
#endif
#endif
    if (confirm) {
        *confirm = [] {};
    }
    return describe(process, uid);
}

// Who listens on the socket `name` in `directory`, for a reason: the account
// whose socket it is (connecting to ask the process would leave the
// listener a connection to drop).
std::string listenerOf(int directory, const std::string& name) {
    struct stat info {};
    if (::fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISSOCK(info.st_mode)) {
        return "a process of " + userName(info.st_uid);
    }
    return "another process";
}

// Whether accounts other than the directory's owner may add, rename or remove
// its entries: it is writable by others, or by its group (on Android only by
// another's: each application's group is its own), and no sticky bit keeps
// them from renaming or removing what is not theirs (as /tmp's does).
bool othersMayChange(const struct stat& directory) {
    if (directory.st_mode & S_ISVTX) {
        return false;
    }
#ifdef __ANDROID__
    return (directory.st_mode & S_IWOTH) || ((directory.st_mode & S_IWGRP) && directory.st_gid != getegid());
#else
    return directory.st_mode & (S_IWGRP | S_IWOTH);
#endif
}

// The endpoint's directory, opened, once it is one where no other account can
// put its own file or link where the endpoint or its lock is meant to be, nor
// rename them away: it belongs to this account or root, and no one else may
// change its entries.
int openDirectory(const std::string& endpoint) {
    const std::string path = endpoint.substr(0, std::max<size_t>(endpoint.find_last_of('/'), 1));
    const int directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": " + path + ": " + errnoText(errno));
    }
    struct stat info {};
    std::string problem;
    if (::fstat(directory, &info) != 0) {
        problem = errnoText(errno);
    } else if (info.st_uid != geteuid() && info.st_uid != 0) {
        problem = "it belongs to " + userName(info.st_uid);
    } else if (othersMayChange(info)) {
        problem = "other accounts may change its entries";
    }
    if (!problem.empty()) {
        ::close(directory);
        fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": " + path + " is no place for it (" + problem +
                                    "); use a directory only this account and root may change");
    }
    return directory;
}

// The listener's lock beside the endpoint (`name`.lock in `directory`): a file
// it holds locked while it lives. Locked here, it is still the file of that
// name (a listener that just ended removes it), and this account's own: a
// regular file with no other name.
int lockEndpoint(int directory, const std::string& endpoint, const std::string& name) {
    const std::string lockName = name + ".lock", path = endpoint + ".lock";
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int file = ::openat(directory, lockName.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (file < 0) {
            fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": " + path + ": " + errnoText(errno));
        }
        struct stat opened {};
        std::string problem;
        if (::fstat(file, &opened) != 0) {
            problem = errnoText(errno);
        } else if (opened.st_uid != geteuid()) {
            problem = "it belongs to " + userName(opened.st_uid) +
                      ": a program of that account has, or had, the endpoint's name";
        } else if (!S_ISREG(opened.st_mode) || opened.st_nlink != 1) {
            problem = "it is not a file of its own";
        }
        if (!problem.empty()) {
            ::close(file);
            fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": " + path + ": " + problem);
        }
        if (::flock(file, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            ::close(file);
            fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": " +
                                        (error == EWOULDBLOCK ? listenerOf(directory, name) + " listens there"
                                                              : path + ": " + errnoText(error)));
        }
        struct stat named {};
        if (::fstatat(directory, lockName.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 && named.st_dev == opened.st_dev &&
            named.st_ino == opened.st_ino) {
            return file;
        }
        ::close(file);
    }
    fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": " + path + " keeps being replaced");
}

} // namespace

const LocalPeer& thisProcess() {
    static const LocalPeer self = [] {
        LocalPeer peer = describe(getpid(), geteuid());
#ifdef RCSDK_PEER_TOKEN
        char program[PROC_PIDPATHINFO_MAXSIZE]{};
        if (proc_pidpath(getpid(), program, sizeof(program)) > 0) {
            peer.program = program;
        }
#endif
        return peer;
    }();
    return self;
}

struct LocalListener::Impl {
    int directory = -1;
    std::string name; // The socket's in the directory...
    bool bound = false;
    int lock = -1; // ...and its lock's, name.lock.
    Socket socket;

    ~Impl() {
        if (bound) {
            ::unlinkat(directory, name.c_str(), 0);
        }
        if (lock >= 0) {
            ::unlinkat(directory, (name + ".lock").c_str(), 0); // Before the lock is let go: see lockEndpoint.
            ::close(lock);
        }
        if (directory >= 0) {
            ::close(directory);
        }
    }
};

LocalListener::LocalListener(const std::string& endpoint, LocalAccess access) : impl_(std::make_unique<Impl>()) {
    const sockaddr_un address = addressOf(endpoint);
    auto& i = *impl_;
    i.name = endpoint.substr(endpoint.find_last_of('/') + 1);
    i.directory = openDirectory(endpoint);
    i.lock = lockEndpoint(i.directory, endpoint, i.name);
    struct stat info {};
    if (::fstatat(i.directory, i.name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISSOCK(info.st_mode)) {
            fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": it exists and is not a socket");
        }
        if (info.st_uid != geteuid()) {
            fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": a socket of " + userName(info.st_uid) +
                                        " is there: a program of that account has, or had, the endpoint's name");
        }
        ::unlinkat(i.directory, i.name.c_str(), 0); // Its lock was free: what a process that died left behind.
    }
    i.socket = unixSocket();
    if (::bind(i.socket.get(), reinterpret_cast<const sockaddr*>(&address), SUN_LEN(&address)) != 0) {
        fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": " + describeSocketError(lastSocketError()));
    }
    i.bound = true;
    // The mode decides who may connect. It is set before listen() (until then
    // a client is refused whatever the mode) on the socket bound in the
    // directory checked above, never through a link.
    if (::fstatat(i.directory, i.name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISSOCK(info.st_mode) ||
        info.st_uid != geteuid()) {
        fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": the socket was replaced as it was made");
    }
    if (::fchmodat(i.directory, i.name.c_str(), access == LocalAccess::anyUser ? 0666 : 0600, AT_SYMLINK_NOFOLLOW) !=
        0) {
        fail(RCS_ERROR_NETWORK, "Could not set the access of " + endpoint + ": " + errnoText(errno));
    }
    if (::listen(i.socket.get(), 16) != 0) {
        fail(RCS_ERROR_NETWORK, "Could not listen on " + endpoint + ": " + describeSocketError(lastSocketError()));
    }
}

LocalListener::~LocalListener() = default;

LocalConnection LocalListener::accept(CancelToken listening, CancelToken stream) {
    for (;;) {
        waitReady(impl_->socket.get(), false, listening, Clock::time_point::max());
        Socket peer(::accept(impl_->socket.get(), nullptr, nullptr));
        if (peer.get() != invalidHandle) {
            LocalConnection connection;
            connection.peer = peerOf(peer.get(), &connection.confirm);
            connection.stream = TcpStream::adopt(peer.release(), stream);
            return connection;
        }
        if (!wouldBlock() && errno != ECONNABORTED) {
            fail(RCS_ERROR_NETWORK, "Accepting a client failed: " + describeSocketError(lastSocketError()));
        }
    }
}

LocalConnection connectLocal(const std::string& endpoint, CancelToken cancelled, std::chrono::milliseconds timeout) {
    const sockaddr_un address = addressOf(endpoint);
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        throwIfCancelled(cancelled);
        Socket socket = unixSocket();
        if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), SUN_LEN(&address)) == 0) {
            LocalPeer peer = peerOf(socket.get());
            return {TcpStream::adopt(socket.release(), cancelled), std::move(peer), {}};
        }
        const int error = errno;
        if (error == EACCES || error == EPERM) {
            fail(RCS_ERROR_PERMISSION_REQUIRED, "The system refuses access to " + endpoint +
                                                    " (its access setting decides who may connect)");
        }
        // A listener whose queue is full refuses for a moment: Linux says
        // EAGAIN, macOS ECONNREFUSED (as it does where nothing listens).
        if ((error == EAGAIN || error == ECONNREFUSED || error == EINTR) && Clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
            continue;
        }
        fail(RCS_ERROR_NETWORK, "Could not connect to " + endpoint + ": " + describeSocketError(error));
    }
}

#endif

} // namespace rcsdk::transport
