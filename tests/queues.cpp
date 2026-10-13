// Receive-queue recovery policy and the viewer's input queue.
#include "session/input_queue.hpp"
#include "session/receive_queue.hpp"
#include "support/check.hpp"

using namespace rcsdk;
using namespace rcsdk::session;

static EncodedPacket packet(uint64_t sequence, bool keyframe) {
    EncodedPacket p;
    p.sequence = sequence;
    p.keyframe = keyframe;
    return p;
}

// The queue asked for a keyframe (not for a picture that leaves a loss out).
static bool keyframeAsked(ReceiveQueue& queue) {
    const auto asked = queue.takeRequest();
    return asked && !asked->after;
}

static void receiveQueue() {
    constexpr uint64_t ms = 1'000'000;
    ReceiveQueue queue({3, 80});
    uint64_t now = 1000 * ms;
    for (uint64_t i = 1; i <= 3; ++i) {
        queue.push(packet(i, i == 1), now);
    }
    CHECK(queue.size() == 3 && !keyframeAsked(queue));

    // Overflow drops the backlog and every packet until a keyframe.
    queue.push(packet(4, false), now);
    CHECK(queue.empty() && queue.dropped() == 4 && keyframeAsked(queue));
    queue.push(packet(5, false), now + 10 * ms);
    CHECK(queue.empty() && !keyframeAsked(queue)); // Requests are rate limited.
    queue.push(packet(6, false), now + 300 * ms);
    CHECK(keyframeAsked(queue));
    queue.push(packet(7, true), now + 310 * ms);
    auto item = queue.pop(now + 311 * ms);
    CHECK(item && item->packet.sequence == 7 && item->resetDecoder); // The decoder restarts here.

    // Normal flow does not reset the decoder.
    queue.push(packet(8, false), now + 320 * ms);
    item = queue.pop(now + 321 * ms);
    CHECK(item && item->packet.sequence == 8 && !item->resetDecoder);

    // A packet older than the age limit triggers the same recovery.
    queue.push(packet(9, false), now + 400 * ms);
    CHECK(!queue.pop(now + 500 * ms) && queue.empty() && keyframeAsked(queue));
    CHECK(queue.recoveries() == 3 && queue.overflows() == 2);

    // A replaced decoder restarts the same way: backlog dropped, next keyframe resets.
    queue.push(packet(10, true), now + 800 * ms);
    queue.push(packet(11, false), now + 801 * ms);
    queue.restartAtKeyframe(now + 802 * ms);
    CHECK(queue.empty() && keyframeAsked(queue) && queue.recoveries() == 4);
    queue.push(packet(12, false), now + 803 * ms);
    CHECK(queue.empty());
    queue.push(packet(13, true), now + 804 * ms);
    item = queue.pop(now + 805 * ms);
    CHECK(item && item->packet.sequence == 13 && item->resetDecoder);

    // A picture lost on the network: what came before stays; what follows
    // waits for a picture that leaves the loss out, asked for at once.
    ReceiveQueue lossy({16, 0});
    lossy.push(packet(1, true), now);
    lossy.push(packet(2, false), now);
    lossy.lost(now + 1 * ms);
    auto asked = lossy.takeRequest();
    CHECK(asked && asked->after == 2u && lossy.size() == 2 && !lossy.takeRequest());
    lossy.push(packet(4, false), now + 2 * ms); // It refers to the lost one.
    CHECK(lossy.size() == 2 && lossy.dropped() == 1 && !lossy.takeResumed());
    auto recovery = packet(5, false);
    recovery.recoversAfter = 2;
    lossy.push(recovery, now + 30 * ms);
    const auto resumed = lossy.takeResumed();
    CHECK(lossy.size() == 3 && resumed && resumed->method == RCS_RECOVERY_REFERENCES &&
          resumed->frozenNs == 30 * ms); // Since the last picture before the loss arrived.
    for (const uint64_t expected : {1, 2, 5}) {
        item = lossy.pop(now + 31 * ms);
        CHECK(item && item->packet.sequence == expected && !item->resetDecoder); // The decoder goes on.
    }
    // One that leaves out only what came after a later picture does not serve; a keyframe does.
    lossy.lost(now + 40 * ms);
    asked = lossy.takeRequest();
    CHECK(asked && asked->after == 5u);
    auto late = packet(7, false);
    late.recoversAfter = 6;
    lossy.push(late, now + 41 * ms);
    CHECK(lossy.empty() && !lossy.takeResumed());
    lossy.push(packet(8, true), now + 60 * ms);
    const auto keyframe = lossy.takeResumed();
    CHECK(keyframe && keyframe->method == RCS_RECOVERY_KEYFRAME && keyframe->frozenNs == 30 * ms);
    CHECK(lossy.overflows() == 0 && lossy.recoveries() == 2);
    // Lost before anything was kept: only a keyframe helps.
    ReceiveQueue fresh({16, 0});
    fresh.lost(now);
    CHECK(keyframeAsked(fresh));

    // A decoder that just opened took 150 ms over its first picture: what
    // arrived meanwhile is decoded, not dropped; it ages from then on.
    ReceiveQueue opening({16, 80});
    opening.push(packet(1, true), now);
    CHECK(opening.pop(now).has_value());
    for (uint64_t i = 2; i <= 10; ++i) {
        opening.push(packet(i, false), now + i * 16 * ms);
    }
    opening.forgiveWaiting(now + 150 * ms);
    for (uint64_t i = 2; i <= 10; ++i) {
        item = opening.pop(now + 152 * ms + i * ms);
        CHECK(item && item->packet.sequence == i);
    }
    CHECK(opening.dropped() == 0 && !opening.takeRequest());
    opening.push(packet(11, false), now + 200 * ms);
    CHECK(!opening.pop(now + 300 * ms) && keyframeAsked(opening)); // Later waits count again.

    // A Wi-Fi radio that dozed 300 ms hands over 36 pictures of a 120 FPS
    // stream at once: under the default limits they decode, none is dropped.
    ReceiveQueue burst(ReceiveLimits{});
    burst.push(packet(1, true), now);
    CHECK(burst.pop(now).has_value());
    for (uint64_t i = 2; i <= 37; ++i) {
        burst.push(packet(i, false), now + 300 * ms);
    }
    for (uint64_t i = 2; i <= 37; ++i) {
        item = burst.pop(now + 300 * ms + i * ms); // A hardware decoder: about a millisecond each.
        CHECK(item && item->packet.sequence == i);
    }
    CHECK(burst.dropped() == 0 && !burst.takeRequest());

    ReceiveQueue unlimited({2, 0}); // Age limit disabled.
    unlimited.push(packet(1, true), 0);
    CHECK(unlimited.pop(10'000 * ms).has_value());
}

static void inputQueue() {
    std::atomic<bool> cancelled{false};
    InputQueue queue;
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    for (uint32_t x = 0; x < 100; ++x) {
        move.x = x;
        CHECK(queue.push(move, 1) == RCS_OK);
    }
    auto item = queue.pop(0ms, cancelled);
    CHECK(item && item->event.x == 99 && !queue.pop(0ms, cancelled)); // Coalesced into the latest move.

    // Relative motion and smooth scrolling add up, within what one event carries.
    rcs_input_event motion = RCS_INPUT_EVENT_INIT;
    motion.type = RCS_INPUT_MOVE_RELATIVE;
    motion.dx = 3;
    motion.dy = -2;
    for (int i = 0; i < 10; ++i) {
        CHECK(queue.push(motion, 1) == RCS_OK);
    }
    motion.dx = 9'990;
    queue.push(motion, 1); // 30 + 9 990 would pass 10 000: a new event.
    item = queue.pop(0ms, cancelled);
    CHECK(item && item->event.dx == 30 && item->event.dy == -20);
    item = queue.pop(0ms, cancelled);
    CHECK(item && item->event.dx == 9'990 && !queue.pop(0ms, cancelled));
    rcs_input_event scroll = RCS_INPUT_EVENT_INIT;
    scroll.type = RCS_INPUT_SCROLL;
    scroll.wheel_y = 40;
    for (uint32_t x = 1; x <= 3; ++x) {
        scroll.x = x;
        queue.push(scroll, 1);
    }
    item = queue.pop(0ms, cancelled);
    CHECK(item && item->event.wheel_y == 120 && item->event.x == 3 && !queue.pop(0ms, cancelled));
    // Touch is never merged: each contact's down and up matter.
    rcs_input_event touch = RCS_INPUT_EVENT_INIT;
    touch.type = RCS_INPUT_TOUCH;
    touch.down = 1;
    queue.push(touch, 1);
    queue.push(touch, 1);
    CHECK(queue.pop(0ms, cancelled) && queue.pop(0ms, cancelled) && !queue.pop(0ms, cancelled));

    rcs_input_event key = RCS_INPUT_EVENT_INIT;
    key.type = RCS_INPUT_KEY;
    key.code = 4;
    for (size_t i = 0; i < InputQueue::capacity; ++i) {
        CHECK(queue.push(key, 1) == RCS_OK);
    }
    CHECK(queue.push(key, 1) == RCS_ERROR_QUEUE_FULL);
    item = queue.pop(0ms, cancelled);
    CHECK(item && item->event.type == RCS_INPUT_RELEASE_ALL && !queue.pop(0ms, cancelled));

    queue.push(key, 1);
    queue.clear();
    CHECK(!queue.pop(0ms, cancelled));
    queue.wake();
    CHECK(!queue.pop(1s, cancelled)); // Returns immediately after wake().
}

int main() {
    return runTest("queues", [] {
        receiveQueue();
        inputQueue();
    });
}
