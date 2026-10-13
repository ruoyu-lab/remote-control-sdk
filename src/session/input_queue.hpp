// View side: bounded queue of input events waiting to be sent.
#pragma once

#include "util/api.hpp"
#include "util/util.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace rcsdk::session {

class InputQueue {
public:
    static constexpr size_t capacity = 256;

    struct Item {
        rcs_input_event event;
        uint64_t epoch;
        uint64_t enqueuedNs;
    };

    // Consecutive moves are coalesced: an absolute one replaces the one
    // before, relative motion and smooth scrolling add up (within what an
    // event can carry). When full, everything pending is replaced by a
    // release-all and QUEUE_FULL is returned.
    rcs_result push(const rcs_input_event& event, uint64_t epoch) {
        std::lock_guard lock(mutex_);
        const Item item{event, epoch, monotonicNs()};
        rcs_result result = RCS_OK;
        Item* last = items_.empty() || items_.back().event.type != event.type ? nullptr : &items_.back();
        if (event.type == RCS_INPUT_RELEASE_ALL) {
            items_.clear();
            items_.push_back(item);
        } else if (last && event.type == RCS_INPUT_MOVE) {
            *last = item;
        } else if (last && event.type == RCS_INPUT_MOVE_RELATIVE && addable(last->event.dx, event.dx, 10'000) &&
                   addable(last->event.dy, event.dy, 10'000)) {
            last->event.dx += event.dx;
            last->event.dy += event.dy;
        } else if (last && event.type == RCS_INPUT_SCROLL && addable(last->event.wheel_x, event.wheel_x, 120'000) &&
                   addable(last->event.wheel_y, event.wheel_y, 120'000)) {
            const int32_t x = last->event.wheel_x + event.wheel_x, y = last->event.wheel_y + event.wheel_y;
            *last = item; // The latest position.
            last->event.wheel_x = x;
            last->event.wheel_y = y;
        } else if (items_.size() < capacity) {
            items_.push_back(item);
        } else {
            items_.clear();
            rcs_input_event release = RCS_INPUT_EVENT_INIT;
            release.type = RCS_INPUT_RELEASE_ALL;
            items_.push_back({release, epoch, item.enqueuedNs});
            result = RCS_ERROR_QUEUE_FULL;
        }
        changed_.notify_all();
        return result;
    }

    // Waits up to `timeout` for an item, a wake() or cancellation.
    std::optional<Item> pop(std::chrono::milliseconds timeout, CancelToken cancelled) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return !items_.empty() || woken_ || cancelled.load(); });
        woken_ = false;
        if (items_.empty() || cancelled.load()) {
            return std::nullopt;
        }
        Item item = items_.front();
        items_.pop_front();
        return item;
    }

    // Drops pending input, e.g. when control is revoked.
    void clear() {
        std::lock_guard lock(mutex_);
        items_.clear();
    }

    // Wakes pop() so the sender can handle other work (keyframe requests, stop).
    void wake() {
        std::lock_guard lock(mutex_);
        woken_ = true;
        changed_.notify_all();
    }

private:
    static bool addable(int32_t a, int32_t b, int32_t limit) noexcept {
        const int64_t sum = int64_t(a) + b;
        return sum >= -limit && sum <= limit;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> items_;
    bool woken_ = false;
};

} // namespace rcsdk::session
