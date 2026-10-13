// System pastes of the other end's files (FILES), on the machine's half: a
// stand-in the system's clipboard holds (Windows' virtual files, macOS'
// placeholder, iOS' item providers, a test's fake) asks for the files as the
// system's paste wants them, through Pastes, which asks the session's half
// (WantMessage, ReadMessage, CloseMessage) and hands its answers back. Any
// thread may ask; answers come on the machine half's calls. Also where the
// module keeps files of its own (files_directory).
#pragma once

#include "messages.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace rcsdk::clipboard {

// One fetch a paste asked for: its items once listed, the answers to its
// reads, and its end, as they come (under `mutex`; `changed` and `wake`
// tell of each).
class PasteFetch {
public:
    PasteFetch(uint64_t id, uint64_t placing) : id(id), placing(placing) {}
    const uint64_t id;      // The machine's number for it.
    const uint64_t placing; // The copy placed whose files it fetches.

    std::mutex mutex;
    std::condition_variable changed;
    std::function<void()> wake; // Called too as anything comes (a stand-in that waits its own way).
    bool listed = false;
    std::vector<FileItem> items;
    std::map<uint64_t, std::pair<rcs_result, Bytes>> answers; // By tag, until taken.
    std::optional<EndedMessage> ended;

    // Waits, at most `timeout`, until `ready()` (called under the mutex).
    template <class Ready>
    bool waitFor(Ready ready, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return ready(); });
    }
};

class Pastes {
public:
    using Tell = std::function<void(const ToSession&)>;
    explicit Pastes(Tell tell) : tell_(std::move(tell)) {}
    ~Pastes();

    // The session's settings: the host's files directory (empty: the
    // module's own), and whether macOS gives Finder a placeholder.
    void configure(const std::string& directory, bool standIn);
    bool standIn() const;
    // A directory for a copy's files (named after its publication), made
    // when missing; Error(DEVICE) where none can be made.
    std::string directoryFor(const Publication& publication);

    // A paste: a fetch of placing's files (READ, or INTO `directory`).
    std::shared_ptr<PasteFetch> fetch(uint64_t placing, rcs_file_fetch_mode mode, const std::string& directory = {},
                                      bool ahead = false);
    void read(const PasteFetch& fetch, uint32_t item, uint64_t offset, uint32_t size, uint64_t tag);
    void close(const PasteFetch& fetch, rcs_result outcome);
    // A paste that could not start: the session's half reports it.
    void refused(uint64_t placing, rcs_result result, const std::string& why);
    // Whether placing's files were withdrawn (the copy is on the clipboard
    // still: its pastes fail); `revokedHook` is told as they are.
    bool revoked(uint64_t placing) const;
    void onRevoked(std::function<void(uint64_t placing)> hook);

    // The session's half's answers.
    void listed(ListedMessage listed);
    void data(DataMessage data);
    void ended(EndedMessage ended);
    void revoke(uint64_t placing);
    // The half stops: fetches not ended end (DISCONNECTED), and those asked
    // later are ended as they are made (a stand-in the system keeps may ask
    // after the half went: nothing reaches its handle then); the module's own
    // directory goes.
    void stop();

private:
    std::shared_ptr<PasteFetch> find(uint64_t request);
    void changed(PasteFetch& fetch);
    // Tells the session's half, while the half runs.
    bool tell(const ToSession& message);

    const Tell tell_;
    std::mutex telling_; // Held while telling: stop() waits for it.
    bool stopped_ = false;
    mutable std::mutex mutex_;
    uint64_t serial_ = 0;
    std::map<uint64_t, std::shared_ptr<PasteFetch>> fetches_;
    std::set<uint64_t> revoked_;
    std::function<void(uint64_t)> revokedHook_;
    std::string given_;   // The host's files directory.
    std::string own_;     // The module's own, once made.
    bool standIn_ = true;
};

} // namespace rcsdk::clipboard
