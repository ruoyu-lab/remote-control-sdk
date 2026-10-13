// What the machine's half does with the files (target SYSTEM): each request
// of the session's half, on the machine's file system (files.hpp). What is
// received is written through the receiving directory, opened afresh for
// each request, and the directories under it, a component at a time: never
// through a link, wherever another process moves things meanwhile. What is
// sent is listed and read the same way, below each entry. Its answers name
// files by their paths as items go, never by the machine's: they go to the
// other end, and to logs.
//
// Store does the work, one request at a time (tests call it directly);
// Worker runs a Store on a thread of its own, so that a slow disk never
// holds up the module's calls, and hands each answer on.
#pragma once

#include "files.hpp"
#include "messages.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace rcsdk::file_transfer {

class Store {
public:
    Store() = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    // The answer to `request`, if it has one (an item's DISCARD has none).
    // What fails in a DISCARD or COMPLETE is logged through `log`: the end
    // of a transfer is done as far as it goes (ENDED all the same).
    std::optional<Reply> handle(Request request);
    // The half stops: open files close; the partial files of transfers that
    // keep none (keep_partial 0) go.
    void shutdown();
    // Where a problem that has no answer goes (the half logs it).
    std::function<void(const std::string&)> log;
    // For tests: called with an item's path once its directories are
    // checked, before anything is made, written or renamed in them, and, as
    // a directory to send is listed, once it was looked at, before it is
    // opened (another process may change the tree there).
    std::function<void(const std::string& path)> checked;

private:
    struct Open {
        uint64_t transfer = 0;
        uint32_t item = 0;
        files::File file;
    };
    struct Partial {
        uint64_t transfer = 0;
        bool keep = false;
    };
    // What a transfer accepted still writes, on the volume of its directory.
    struct Reserved {
        uint64_t volume = 0;
        uint64_t bytes = 0;
    };

    Listed list(const ListRequest& request);
    Prepared prepare(const PrepareRequest& request);
    Reply read(const ReadRequest& request);
    Reply write(const WriteRequest& request);
    Finished finish(const FinishRequest& request);
    void discard(const DiscardRequest& request);
    void complete(const CompleteRequest& request);
    Purged purge(const PurgeRequest& request);
    // The files of `transfer` (an item of it) close.
    void close(uint64_t transfer, std::optional<uint32_t> item = std::nullopt);
    // The open file of an item, or null.
    files::File* opened(std::deque<Open>& open, uint64_t transfer, uint32_t item);
    files::File& keepOpen(std::deque<Open>& open, uint64_t transfer, uint32_t item, files::File file);

    static constexpr size_t mostOpen = 4; // Of each kind: the newest stay open.
    std::deque<Open> readers_, writers_;   // The newest first.
    // Partial files written: by receiving directory and path under it.
    std::map<std::pair<std::string, std::string>, Partial> partials_;
    // By transfer, until it ends: transfers going at once fit the free space together.
    std::map<uint64_t, Reserved> reserved_;
};

class Worker {
public:
    using Answer = std::function<void(Reply reply)>;
    Worker(Answer answer, std::function<void(const std::string&)> log);
    // The request in progress is finished, those waiting are dropped; then
    // the store shuts down.
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    void post(Request request);

private:
    void run();

    Store store_;
    const Answer answer_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Request> requests_;
    bool quit_ = false;
    std::thread thread_; // Last: it starts once the rest exists.
};

} // namespace rcsdk::file_transfer
