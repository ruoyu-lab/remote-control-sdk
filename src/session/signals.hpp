// P2P and RELAY: the signals the host passes in (rcs_session_signal), sorted
// out from nothing but the signals themselves and the host's routes.
//
// A viewer signals first, at each connection. A sharer answers each viewer
// signal with an ICE exchange of its own (connect.hpp, IceListener), which
// SignalQueue paces: up to maxRunning at a time, later ones wait their turn
// (at most maxWaiting: the one that waited longest makes room), and a signal
// seen before, a repeat, is skipped. A sharer's signal names the viewer
// signal it answers, so a viewer's connection takes the answer to its own
// (Answers), whatever else the host delivers and in whatever order.
#pragma once

#include "transport/ice.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace rcsdk::session {

// A viewer's signal, and the host's route for it (back to that viewer).
struct ViewerSignal {
    transport::ice::Description description;
    uint64_t route = 0;
};

class SignalQueue {
public:
    static constexpr size_t maxRunning = 32;
    static constexpr size_t maxWaiting = 256;
    static constexpr size_t remembered = 4096; // Signals known as seen, the latest.

    struct Added {
        bool start = false;                  // An attempt starts for it now (it counts as running).
        bool repeat = false;                 // Seen before: nothing to do.
        std::optional<ViewerSignal> dropped; // It waited longest and made room.
    };
    Added add(const ViewerSignal& signal);
    // An attempt ended: the signal whose attempt starts now, if one waits.
    std::optional<ViewerSignal> finished();
    // Those that wait, which no attempt will take.
    std::deque<ViewerSignal> takeWaiting() { return std::exchange(waiting_, {}); }

    size_t running() const noexcept { return running_; }
    size_t waiting() const noexcept { return waiting_.size(); }

private:
    std::deque<ViewerSignal> waiting_;
    size_t running_ = 0;
    std::set<std::string> seen_;        // Viewer ufrags...
    std::deque<std::string> seenOrder_; // ...in the order they came.
};

// View: the sharer's signals the host delivered.
class Answers {
public:
    static constexpr size_t kept = 16;
    void add(transport::ice::Description answer);
    // The first answer to the signal whose ufrag is `ufrag` (a connection's
    // own), taken; answers to other signals are stale and go.
    std::optional<transport::ice::Description> take(const std::string& ufrag);
    void clear() noexcept { answers_.clear(); }

private:
    std::deque<transport::ice::Description> answers_;
};

} // namespace rcsdk::session
