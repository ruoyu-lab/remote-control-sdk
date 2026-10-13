// Adaptive bitrate for the sharer, from the viewer's reports on what arrived.
//
// Every 50 ms in which video arrived the viewer reports one window: datagrams
// expected and received, bytes received, and the lowest one-way delay of video
// fragments (send time stamped by the sharer, arrival time by the viewer; the
// clocks need not agree, only changes count): a picture's first fragment, so
// that a picture sent at once does not count as a queue behind itself.
// Against the lowest delay of the last 10 s, the window's delay is queueing: a
// bottleneck filling up shows there long before loss.
//
// Wireless paths queue on their own: a receiver's Wi-Fi saving power gets
// what came for it held by the access point for 100 ms and more, every few
// seconds, then all at once; their loss comes and goes without any queue. So
// queueing counts as overuse only when it persists over overusePersist of
// reports (a congested queue stays, a radio's nap does not; the path is not
// clear, though, from the first report that shows it), loss is measured over
// the last second, and while the sharer sends far less than the target (a
// still screen) neither its queueing nor its arrival rate says what the path
// takes.
//
// The built-in rules (RateRules, rcs_video_tuning_config; defaults):
//  - Overuse (queueing that persisted 150 ms above 30 ms and rising, or
//    above 60 ms; loss above 10%): down to 85% of what actually arrived (of
//    the target while sending little), at most halving, at least 5% down, at
//    most once per 300 ms.
//  - Clear (the latest queueing below 10 ms, loss below 5%): up 50% per
//    second until the first overuse (slow start; faster on a path of less
//    than 100 ms round trip, whose reports show the result sooner: times
//    100 ms / round trip, at most 8), then 25% per second up to
//    90% of the rate an overuse began at, 8% per second above it; slow start
//    again after 10 s above it without overuse.
//  - Sending less than half the target after an overuse: hold, whatever the
//    queueing (before any, slow start goes on).
//  - Otherwise: hold.
// Each report yields the rules' proposal (propose); the target is what the
// caller settles on, the proposal or the host's own decision (settle). Pure
// logic, fed by the transport; the caller applies the target. The transport's
// reliable data backs off as soon as the path is not clear, well before the
// video does (udp_connection.hpp).
#pragma once

#include "util/util.hpp"

#include <rcsdk/tuning.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>

namespace rcsdk::protocol {

struct RateReport {
    uint32_t windowUs = 0;               // Viewer's time the window covers.
    uint32_t expected = 0, received = 0; // Datagrams.
    uint64_t bytes = 0;
    int64_t delayUs = 0; // Lowest one-way delay of video in the window (arbitrary origin).
    uint64_t sentBps = 0; // Sharer: what it sent lately (video, parity); 0: unknown.
    uint64_t roundTripUs = 0; // Sharer: the path's round trip as measured; 0: not yet.
};

// The thresholds and rates of the built-in rules (rcs_video_tuning_config),
// and how fast video leaves while they find the path clear (the transport's
// pacing, udp_connection.cpp).
struct RateRules {
    int64_t overuseQueueUs = 30'000, severeQueueUs = 60'000, clearQueueUs = 10'000;
    Clock::duration overusePersist = 150ms; // Queueing counts once it lasted this long (in reports).
    uint32_t overuseLoss = 100, clearLoss = 50; // Per mille.
    double decrease = 0.85;                     // Of what arrived.
    Clock::duration decreaseInterval = 300ms;
    double probeIncrease = 0.5, increase = 0.08, recoveryIncrease = 0.25; // Per second.
    // A path of another address starts at this share of the target; 0: at pathStartBps.
    double pathChange = 0.5;
    uint32_t pathStartBps = 4'000'000;
    Clock::duration frameSpread = 1ms;   // Each picture leaves within it (0: as fast as maxPacingBps allows)...
    uint64_t maxPacingBps = 800'000'000; // ...never faster than this,
    uint32_t pathPacing = 110;           // nor, beyond its first pathBurst of the path's time,
    Clock::duration pathBurst = 8ms;     // than this percent of the path's measured rate (0: no limit).
    bool operator==(const RateRules&) const = default;
};

// Parity for lost video on datagram transports (rcs_video_tuning_config.fec_mode).
struct FecSetting {
    rcs_fec_mode mode = RCS_FEC_AUTO;
    uint32_t percent = 50; // FIXED's; AUTO's most.
    bool operator==(const FecSetting&) const = default;
};

class RateController {
public:
    RateController(uint32_t minimumBps, uint32_t maximumBps, uint32_t startBps, const RateRules& rules = {});

    // A report: updates the estimate and returns the rules' proposal (within the limits).
    uint32_t propose(const RateReport& report, Clock::time_point now);
    // The target from now on, within the limits; returns it when it moved
    // noticeably (5%) from the last one returned.
    std::optional<uint32_t> settle(uint32_t bitsPerSecond);
    void setRules(const RateRules& rules) noexcept { rules_ = rules; }
    void setLimits(uint32_t minimumBps, uint32_t maximumBps);
    // Starts over at `bitsPerSecond` (within the limits), probing up quickly again.
    void moveTo(uint32_t bitsPerSecond);
    // The video moved to a path of another address, of which what was
    // measured says nothing: starts over at the rules' pathChange of the
    // target, probing up as at the start; counted.
    void newPath();
    uint32_t pathChanges() const noexcept { return pathChanges_; }

    uint32_t target() const noexcept { return uint32_t(target_); }
    uint32_t minimum() const noexcept { return minimum_; }
    uint32_t maximum() const noexcept { return std::max(minimum_, maximum_); }
    uint64_t receivedBps() const noexcept { return received_; }
    uint32_t lossPermille() const noexcept { return lossPermille_; }
    // Queueing that persisted over the latest reports (overusePersist).
    uint64_t queueDelayUs() const noexcept { return queueDelay_; }
    // Queueing in the latest report and loss below the rules' clear
    // thresholds: a queue that just began holds back increases and bursts at
    // once, though only one that lasts counts as overuse.
    bool clear() const noexcept;

private:
    void clamp() noexcept;

    uint32_t minimum_, maximum_;
    RateRules rules_;
    double target_;
    uint32_t reported_;
    uint64_t received_ = 0;     // Smoothed arrival rate, bits per second.
    uint32_t lossPermille_ = 0; // Over the last second.
    uint64_t queueDelay_ = 0;   // Persisted.
    uint64_t latestQueue_ = 0;  // In the latest report.
    double overuseRate_ = 0;    // Where the latest overuse began; 0: none yet (slow start).
    uint32_t pathChanges_ = 0;
    Clock::time_point lastDecrease_{}, lastReport_{}, lastOveruse_{};
    std::deque<std::pair<Clock::time_point, int64_t>> minima_; // Per-window minimum delays, 10 s.
    std::deque<uint64_t> queues_;                              // The latest reports' queueing.
    struct Loss {
        Clock::time_point time;
        uint32_t expected, lost;
    };
    std::deque<Loss> losses_; // The last second's reports.
};

} // namespace rcsdk::protocol
