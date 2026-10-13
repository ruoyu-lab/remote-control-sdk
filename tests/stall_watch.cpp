// Short stalls of a path (protocol/stall_watch.hpp): a sender that sends all
// the time over a path that holds everything for a while (a Wi-Fi radio away
// on AWDL's channel every 1.05 s), loses it, or does so at random; a sender
// that pauses by itself; the pace found among stalls of other causes and
// skipped beats, never half or twice the beat.
#include "protocol/stall_watch.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

using namespace rcsdk;
using namespace rcsdk::protocol;
using namespace std::chrono_literals;
using Clock = StallWatch::Clock;

namespace {

// A sender that sends every `every` (sound and video: timestamped; one in
// four an acknowledgement without a send time) for `length`; the path takes
// 3 ms, plus up to 1 ms of jitter. `held(t)`: the path holds what is sent at
// t until the returned time (or t: no hold); `lost(t)`: what is sent at t is lost.
struct Path {
    Clock::duration every = 5ms;
    Clock::duration length = 12s;
    std::function<Clock::time_point(Clock::time_point)> held = [](Clock::time_point t) { return t; };
    std::function<bool(Clock::time_point)> lost = [](Clock::time_point) { return false; };
    std::function<bool(Clock::time_point)> sends = [](Clock::time_point) { return true; };
    uint32_t clockOffsetUs = 123456789; // The sender's clock: anything, it wraps.
};

StallWatch::Summary run(const Path& path, StallWatch& watch, Clock::time_point& end) {
    struct Arrival {
        Clock::time_point at;
        uint64_t number;
        std::optional<uint32_t> sentUs;
    };
    std::mt19937 random(7);
    const auto start = Clock::time_point{} + 1h;
    std::vector<Arrival> arrivals;
    uint64_t number = 0;
    for (auto t = start; t < start + path.length; t += path.every) {
        if (!path.sends(t)) {
            continue;
        }
        ++number;
        if (path.lost(t)) {
            continue;
        }
        const auto sentUs = uint32_t(path.clockOffsetUs + uint32_t((t - start) / 1us));
        const auto at = std::max(path.held(t), t) + 3ms + std::chrono::microseconds(random() % 1000);
        arrivals.push_back({at, number, number % 4 ? std::optional(sentUs) : std::nullopt});
    }
    std::stable_sort(arrivals.begin(), arrivals.end(), [](const Arrival& a, const Arrival& b) { return a.at < b.at; });
    for (const auto& a : arrivals) {
        watch.arrived(a.number, a.sentUs, uint64_t((a.at - start) / 1ns) + 5'000'000'000ull, a.at);
    }
    end = arrivals.back().at;
    return watch.summary(end);
}

// Holds what is sent within `hold` of each beat until the beat's hold ends.
std::function<Clock::time_point(Clock::time_point)> beating(Clock::duration beat, Clock::duration hold,
                                                            Clock::duration phase = 300ms) {
    return [=](Clock::time_point t) {
        const auto since = (t.time_since_epoch() + phase) % beat;
        return since < hold ? t - since + hold : t;
    };
}

void awdl() {
    Path path;
    path.held = beating(1049ms, 80ms);
    StallWatch watch;
    Clock::time_point end;
    const auto s = run(path, watch, end);
    std::printf("AWDL-like: %u stalls in 10 s, median %u ms, longest %u ms, pace %u ms\n", s.count, s.medianMs,
                s.longestMs, s.periodMs);
    CHECK(s.count >= 9 && s.count <= 10);
    CHECK(s.medianMs >= 75 && s.medianMs <= 90);
    CHECK(s.periodMs >= 1044 && s.periodMs <= 1054);
    // Over: they age out of the window.
    CHECK(watch.summary(end + 11s) == (StallWatch::Summary{0, 0, 0, 0, s.total}));
}

void senderPauses() {
    Path path; // A still screen: nothing for 100 ms of every second.
    path.sends = [](Clock::time_point t) { return t.time_since_epoch() % 1000ms >= 100ms; };
    StallWatch watch;
    Clock::time_point end;
    const auto s = run(path, watch, end);
    CHECK(s.count == 0 && s.total == 0);
}

void lostInStalls() {
    Path path; // A radio that drops what came while it was away.
    path.lost = [](Clock::time_point t) { return (t.time_since_epoch() + 300ms) % 1049ms < 60ms; };
    StallWatch watch;
    Clock::time_point end;
    const auto s = run(path, watch, end);
    std::printf("lost in stalls: %u stalls, median %u ms, pace %u ms\n", s.count, s.medianMs, s.periodMs);
    CHECK(s.count >= 9 && s.periodMs >= 1044 && s.periodMs <= 1054);
}

void atRandom() {
    std::mt19937 random(11);
    std::vector<Clock::duration> at; // Stalls at random, about one a second.
    for (auto t = 0ms; t < 12s; t += std::chrono::milliseconds(300 + random() % 1500)) {
        at.push_back(t);
    }
    Path path;
    path.held = [at](Clock::time_point t) {
        const auto since = t.time_since_epoch() - 1h;
        for (const auto a : at) {
            if (since >= a && since < a + 70ms) {
                return t - (since - a) + 70ms;
            }
        }
        return t;
    };
    StallWatch watch;
    Clock::time_point end;
    const auto s = run(path, watch, end);
    std::printf("at random: %u stalls, pace %u ms\n", s.count, s.periodMs);
    CHECK(s.count >= 5 && s.periodMs == 0);
}

void beatAmongOthers() {
    // The beat, one in four beats skipped, and stalls of other causes between.
    Path path;
    const auto beat = beating(1049ms, 90ms);
    path.held = [beat](Clock::time_point t) {
        const auto since = t.time_since_epoch() - 1h;
        if ((since + 300ms) / 1049ms % 4 == 3) {
            const auto other = (since + 700ms) % 2500ms; // Another cause, every 2.5 s off the beat.
            return other < 50ms ? t - other + 50ms : t;
        }
        return beat(t);
    };
    StallWatch watch;
    Clock::time_point end;
    const auto s = run(path, watch, end);
    std::printf("beat among others: %u stalls, pace %u ms\n", s.count, s.periodMs);
    CHECK(s.periodMs >= 1044 && s.periodMs <= 1054);
}

void shortBeat() {
    Path path; // A scan's beat: 40 ms every 400 ms, with acknowledgements ending some gaps.
    path.held = beating(400ms, 40ms, 0ms);
    StallWatch watch;
    Clock::time_point end;
    const auto s = run(path, watch, end);
    CHECK(s.count >= 20 && s.periodMs >= 395 && s.periodMs <= 405);
}

void restarted() {
    Path path;
    path.held = beating(1049ms, 80ms);
    path.length = 5s;
    StallWatch watch;
    Clock::time_point end;
    const auto before = run(path, watch, end).count;
    watch.restart(); // Another path: a gap over the change is no stall.
    watch.arrived(1'000'000, 0, 0, end + 500ms);
    CHECK(watch.summary(end + 500ms).count == before);
}

} // namespace

int main() {
    return runTest("stall_watch", [] {
        awdl();
        senderPauses();
        lostInStalls();
        atRandom();
        beatAmongOthers();
        shortBeat();
        restarted();
    });
}
