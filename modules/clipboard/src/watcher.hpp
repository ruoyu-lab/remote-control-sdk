// The system clipboard on a thread of its own (a SystemClipboard lives
// there): looks for new copies at an interval while watched, and places the
// copies it is given. The module's machine half runs one: in a sharer's
// agent, or in a viewer's own process.
#pragma once

#include "system.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace rcsdk::clipboard {

// How the session uses this machine's clipboard (target SYSTEM).
struct Use {
    bool watch = false; // New copies are reported (SEND).
    uint32_t intervalMs = 250;
    uint32_t formats = 0;  // (1u << rcs_clipboard_format) bits read.
    uint32_t maxBytes = 0; // The most a copy read holds.
    std::string filesDirectory; // files_directory; empty: the module's own.
    bool standIn = true;        // files_stand_in.
    bool operator==(const Use&) const = default;
};

class Watcher {
public:
    // Called on the watcher's thread, one at a time.
    struct Handlers {
        // A new copy on the clipboard; when watching starts (`existing`) or
        // its settings change, the one it holds then (empty: nothing in
        // these formats).
        std::function<void(Content content, bool existing)> copied;
        // The clipboard could not be read (another program held it for a
        // second; RCS_ERROR_UNSUPPORTED: this process has none).
        std::function<void(rcs_result result, const std::string& message)> readFailed;
        // A copy given to place() was placed (RCS_OK), or why not.
        std::function<void(uint64_t id, rcs_result result, const std::string& message)> placed;
    };

    // `role`: whose clipboard it is (createClipboard); `pastes`: where its
    // stand-ins for the other end's files ask for them.
    Watcher(rcs_role role, Use use, Handlers handlers, std::shared_ptr<Pastes> pastes);
    // Waits for a look or a placing under way.
    ~Watcher();
    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    void use(const Use& use);
    // Placed in turn; the watch does not report it as a new copy.
    void place(uint64_t id, Content content);
    // The same copy again, as `content` (its files fetched ahead, placed
    // whole), if the clipboard still holds it as placed; nothing is told.
    void replace(uint64_t id, Content content);

private:
    struct Write {
        uint64_t id = 0;
        Content content;
        bool replacing = false;
    };
    void run() noexcept;
    // Holding nothing: what the clipboard holds now, if it changed (or
    // `fresh`); `existing`: the watch's first look.
    void look(const Use& use, bool fresh, bool existing);
    void write(const Write& w);
    bool open(); // The clipboard exists; else unavailable_ says why.

    const rcs_role role_;
    const Handlers handlers_;
    const std::shared_ptr<Pastes> pastes_;

    std::mutex mutex_;
    std::condition_variable wake_;
    Use use_;
    uint64_t useSerial_ = 1; // Moves on with each use().
    std::deque<Write> writes_;
    bool quit_ = false;

    // The watcher's thread's.
    std::unique_ptr<SystemClipboard> clipboard_;
    std::string unavailable_; // Why there is no clipboard, once tried.
    uint64_t seen_ = 0;       // The change count last reported or placed...
    bool seenAny_ = false;
    bool seenOwn_ = false; // ...and whether it is a copy placed here, which a look afresh leaves alone.
    uint64_t placed_ = 0;  // The copy placed last.
    int failedLooks_ = 0;  // At this change count.

    std::thread thread_; // Last.
};

} // namespace rcsdk::clipboard
