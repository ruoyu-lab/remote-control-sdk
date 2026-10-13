// Byte streams between two processes of one machine, for the link between a
// session and its agent (agent/remote.hpp): a named pipe on Windows, a Unix
// domain socket elsewhere. The system's access control decides who may open
// the endpoint, and the system says which process is at the other end; what
// runs on top (the secure channel) decides who may use it.
#pragma once

#include "transport/stream.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace rcsdk::transport {

// Who may open the endpoint.
//   privileged  The user the listener runs as; on Windows also SYSTEM and
//               (elevated) administrators. Elsewhere the socket file's mode is 0600.
//   anyUser     Every user, too. On Windows they may connect but not add
//               instances of the pipe; elsewhere the mode is 0666.
enum class LocalAccess { privileged, anyUser };

// The process at the other end of a local stream, as the operating system
// reports it (never as the process says).
struct LocalPeer {
    uint32_t processId = 0;
    uint32_t osSession = 0; // Windows: the session it runs in; 0 elsewhere.
    // The account it runs as, for people (UTF-8): Windows "DOMAIN\user" (the
    // SID where the name cannot be looked up), elsewhere the user name (the
    // uid where it has none)...
    std::string account;
    // ...and for comparing: the SID (Windows) or the uid, as text.
    std::string user;
    // LocalSystem or an administrator whose token is elevated (Windows); root.
    bool privileged = false;
    // The path of the program it runs: macOS from the process's audit token,
    // which names one process as it runs one program (no other process that
    // takes its ID later, nor what it runs after exec), for a client a
    // listener accepted; Windows from the process, opened as it is told.
    // Empty where the system cannot say for sure (Linux, Android, iOS, the
    // server a macOS client reached) or this process may not read it (Windows).
    std::string program;
};

// This process, as a peer sees it.
const LocalPeer& thisProcess();

struct LocalConnection {
    std::unique_ptr<Stream> stream;
    LocalPeer peer;
    // A listener's, while `stream` lives: throws Failure(AUTHENTICATION)
    // unless the system still names `peer`'s process, running the same
    // program, at the other end. macOS names the process that used the
    // connection last (as it is now), so a client that handed its connection
    // to another process, or ran another program, after it was accepted is
    // refused once it has spoken; elsewhere the system names the process that
    // connected, and this does nothing.
    std::function<void()> confirm;
};

// The endpoint, as the host names it: Windows, the pipe's name (\\.\pipe\<name>);
// elsewhere, the socket file's absolute path, beside which the listener keeps
// "<path>.lock" locked while it lives, so that a socket file left by a process
// that died is told apart from one a process listens on. The directory must
// belong to this account or root and let no other account change its entries
// (no write access for others or, but on Android for the application's own,
// the group, unless it is sticky as /tmp is): otherwise another account could
// put its own file or link where the socket or the lock is made.
class LocalListener {
public:
    // Creates the endpoint. Throws Failure(INVALID_ARGUMENT) for a name that
    // cannot be one, Failure(NETWORK) when it cannot be created, the reason
    // saying why: another process has the name (which, where the system says),
    // the directory is not there or other accounts may change it.
    LocalListener(const std::string& endpoint, LocalAccess access);
    ~LocalListener();
    LocalListener(const LocalListener&) = delete;
    LocalListener& operator=(const LocalListener&) = delete;

    // Waits for the next client: its stream, which observes `stream` (its reads
    // and writes end when that is set), and which process it is. `listening`
    // ends the wait (CANCELLED). A client the system cannot name is closed and
    // Failure(AUTHENTICATION) thrown; the listener goes on either way.
    LocalConnection accept(CancelToken listening, CancelToken stream);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Why `endpoint` cannot be one on this system (a pipe name has no slash; a
// socket path is absolute and short), or empty when it can.
std::string endpointProblem(const std::string& endpoint);

// The stream, which observes `cancelled`, and the process that serves the
// endpoint. Throws Failure(NETWORK) when nothing listens there (a listener
// whose queue is full is tried again until `timeout`), PERMISSION_REQUIRED
// when the system refuses access, AUTHENTICATION when it cannot say who serves it.
LocalConnection connectLocal(const std::string& endpoint, CancelToken cancelled, std::chrono::milliseconds timeout);

} // namespace rcsdk::transport
