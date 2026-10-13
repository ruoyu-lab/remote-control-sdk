// Data channels: the hosts' own messages over the connection
// (rcs_session_send_data, on_data), and the modules' (rcsdk/module.h).
// Reliable messages travel as DataParts on the data stream, one message at a
// time with a bounded amount in flight, so input sent beside them waits
// little and the video keeps its pacing; unreliable ones go as single
// datagrams. The hosts' messages and each module's wait in queues of their
// own, which take turns, one message each; this end's module list and its
// modules' hellos go before them all.
#pragma once

#include "protocol/connection.hpp"
#include "util/api.hpp"
#include "util/util.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace rcsdk::session {

class Session;

// Messages queued, until the connection's sender takes them.
class DataQueue {
public:
    // `open`: it takes messages at once (open()).
    explicit DataQueue(bool open = false) : open_(open) {}

    struct Item {
        protocol::DataPart::Kind kind = protocol::DataPart::data;
        uint8_t module = 0;   // A module's message or hello: its number (0 is the hosts' queue).
        uint32_t channel = 0; // The hosts'.
        bool unreliable = false;
        bool replace = false; // A module's (RCS_MODULE_REPLACE).
        uint64_t tag = 0;     // A module's, for sent().
        crypto::Bytes bytes;
    };
    enum class Push { queued, full, closed };
    // A message, behind those of its queue (the hosts', a module's): full when
    // `limit` bytes are queued there with it (the sender is told when there is
    // room again); closed between connections. One with `replace` takes the
    // place of its module's newest one with it.
    Push push(Item item, size_t limit);
    // Whether `bytes` more fit the hosts' queue now, or `item` its queue (none
    // when closed); full as push() would be when they do not.
    Push room(size_t bytes, size_t limit);
    Push fits(const Item& item, size_t limit);
    // The module list or a hello: before every message, a hello in place of
    // one of the same module waiting. False when closed.
    bool pushFirst(Item item);
    // A connection takes messages from now on, until clear().
    std::optional<Item> pop(std::chrono::milliseconds timeout, CancelToken cancelled);
    // True once after a refused push to queue `module` (0: the hosts'), when
    // it got below half of `limit`.
    bool roomAgain(uint8_t module, size_t limit);
    void open();
    // Drops what is queued and takes nothing until open(); true when a push
    // of the hosts' had been refused since they were last told there is room
    // (they are to be told now).
    bool clear();
    // Queued in queue `module` (0: the hosts').
    size_t bytes(uint8_t module = 0) const;

private:
    struct Queue {
        std::deque<Item> items;
        size_t bytes = 0;
        bool refused = false;
    };
    Queue& queue(uint8_t module); // Holding mutex_...
    // ...as is the one `item` takes the place of (replace), if any.
    static Item* replaced(Queue& q, const Item& item) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<Item> first_;
    std::vector<Queue> queues_; // By module number, the hosts' first.
    size_t turn_ = 0;           // The queue whose turn is next.
    bool open_ = false;
};

// Sends a queue's data over one connection, on its own thread: the session's
// (view), or a viewer's (share, `viewer`). `cancelled` ends its blocking
// calls; `cancel` sets it, as the destructor does first.
class DataSender {
public:
    DataSender(Session& session, protocol::Connection& connection, DataQueue& queue, CancelToken cancelled,
               std::function<void()> cancel, rcs_viewer_id viewer = 0);
    ~DataSender();

private:
    void run() noexcept;
    void sendReliable(const DataQueue::Item& item, size_t inFlight);
    // One part at a time, no more than `inFlight` bytes unacknowledged.
    void waitForRoom(size_t inFlight);

    Session& session_;
    protocol::Connection& connection_;
    DataQueue& queue_;
    CancelToken cancelled_;
    const std::function<void()> cancel_;
    const rcs_viewer_id viewer_;
    std::thread thread_;
};

// Puts a message back together from its parts (they arrive in order, one
// message at a time) and hands it, or a datagram, on: the hosts' to on_data,
// the modules' and the other end's module list and hellos to the session's
// modules. `queue`: the connection's, where the modules answer.
class DataReceiver {
public:
    // `viewer`: whose messages they are (share).
    DataReceiver(Session& session, DataQueue& queue, rcs_viewer_id viewer = 0)
        : session_(session), queue_(queue), viewer_(viewer) {}
    void part(const protocol::DataPart& part);
    void datagram(const protocol::DataDatagram& datagram);

private:
    Session& session_;
    DataQueue& queue_;
    const rcs_viewer_id viewer_;
    protocol::DataPart::Kind kind_ = protocol::DataPart::data;
    uint8_t module_ = 0;
    uint32_t channel_ = 0, count_ = 0, next_ = 0;
    size_t limit_ = 0; // The most the message may hold.
    crypto::Bytes bytes_;
};

} // namespace rcsdk::session
