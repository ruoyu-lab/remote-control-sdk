#include "protocol/rate_control.hpp"

namespace rcsdk::protocol {
namespace {

constexpr auto delayHistory = 10s;
constexpr auto lossHistory = 1s;
constexpr uint32_t leastExpected = 20; // Fewer datagrams in a second say too little about loss.
constexpr auto reportInterval = 50ms;  // Of the viewer's reports while video arrives.
// An overuse within this long of the previous one belongs to the same episode.
constexpr auto overuseEpisode = 2s;
// Clear this long above where the latest overuse began: probe beyond it again.
constexpr auto forgetOveruse = 10s;
// Slow start on a path shorter than this probes faster, by this / round trip...
constexpr double probeRoundTripUs = 100'000;
constexpr double fastestProbe = 8; // ...at most this many times.
// Beyond any path and any clock: what a report says is held within them, so
// that forged values stay in range of the arithmetic.
constexpr double maxReceivedBps = 1e12;
constexpr int64_t maxDelayUs = int64_t(1) << 52; // 142 years.

} // namespace

RateController::RateController(uint32_t minimumBps, uint32_t maximumBps, uint32_t startBps, const RateRules& rules)
    : minimum_(minimumBps), maximum_(maximumBps), rules_(rules), target_(startBps ? startBps : maximumBps),
      reported_(0) {
    clamp();
    reported_ = uint32_t(target_);
}

void RateController::setLimits(uint32_t minimumBps, uint32_t maximumBps) {
    minimum_ = minimumBps;
    maximum_ = maximumBps;
    clamp();
}

void RateController::moveTo(uint32_t bitsPerSecond) {
    target_ = bitsPerSecond;
    overuseRate_ = 0;
    clamp();
    reported_ = uint32_t(target_);
}

void RateController::newPath() {
    target_ = rules_.pathChange > 0 ? target_ * rules_.pathChange : double(rules_.pathStartBps);
    overuseRate_ = 0;
    received_ = 0;
    lossPermille_ = 0;
    queueDelay_ = latestQueue_ = 0;
    minima_.clear();
    queues_.clear();
    losses_.clear();
    lastDecrease_ = lastOveruse_ = {};
    clamp();
    reported_ = uint32_t(target_);
    ++pathChanges_;
}

bool RateController::clear() const noexcept {
    return int64_t(latestQueue_) < rules_.clearQueueUs && lossPermille_ < rules_.clearLoss;
}

void RateController::clamp() noexcept {
    target_ = std::clamp(target_, double(minimum_), double(maximum()));
}

uint32_t RateController::propose(const RateReport& report, Clock::time_point now) {
    const double elapsed = lastReport_ == Clock::time_point{}
                               ? 0.05
                               : std::clamp(std::chrono::duration<double>(now - lastReport_).count(), 0.01, 0.5);
    lastReport_ = now;

    if (report.windowUs) {
        const double rate = std::min(double(report.bytes) * 8e6 / report.windowUs, maxReceivedBps);
        received_ = received_ ? uint64_t(0.8 * double(received_) + 0.2 * rate) : uint64_t(rate);
    }
    if (report.expected) { // Loss over the last second: a burst does not stand for the path.
        const uint32_t lost = report.expected - std::min(report.received, report.expected);
        losses_.push_back({now, report.expected, lost});
        while (now - losses_.front().time > lossHistory) {
            losses_.pop_front();
        }
        uint64_t expected = 0, lostTotal = 0;
        for (const auto& l : losses_) {
            expected += l.expected;
            lostTotal += l.lost;
        }
        lossPermille_ = uint32_t(lostTotal * 1000 / std::max<uint64_t>(expected, leastExpected));
    }
    const int64_t delay = std::clamp(report.delayUs, -maxDelayUs, maxDelayUs);
    minima_.emplace_back(now, delay);
    while (now - minima_.front().first > delayHistory) {
        minima_.pop_front();
    }
    int64_t base = minima_.front().second;
    for (const auto& [time, earlier] : minima_) {
        base = std::min(base, earlier);
    }
    // Queueing that lasted over the latest reports: their least.
    latestQueue_ = uint64_t(std::max<int64_t>(0, delay - base));
    queues_.push_back(latestQueue_);
    const size_t persist = size_t(std::max<int64_t>(1, (rules_.overusePersist + reportInterval - 1ns) / reportInterval));
    while (queues_.size() > persist) {
        queues_.pop_front();
    }
    const uint64_t previous = queueDelay_;
    queueDelay_ = *std::min_element(queues_.begin(), queues_.end());
    const bool rising = queueDelay_ > previous;

    const auto queue = int64_t(queueDelay_);
    const bool lossy = lossPermille_ > rules_.overuseLoss;
    // Sending little (a still screen): the path is not what holds it back.
    const bool sendingLittle = report.sentBps && double(report.sentBps) < 0.5 * target_;
    const bool queueing = (queue > rules_.overuseQueueUs && rising) || queue > rules_.severeQueueUs;
    if (lossy || (queueing && !sendingLittle)) {
        if (now - lastDecrease_ >= rules_.decreaseInterval) {
            if (now - lastOveruse_ > overuseEpisode) {
                overuseRate_ = target_; // An episode begins: the rate to come back to.
            }
            const double arrived = sendingLittle ? target_ : double(received_);
            // At most halved per step: early or lossy measurements can be far too low.
            target_ = std::max(target_ * 0.5, arrived > 0 ? std::min(target_ * 0.95, rules_.decrease * arrived)
                                                          : target_ * 0.7);
            lastDecrease_ = now;
        }
        lastOveruse_ = now;
    } else if (clear() && !(sendingLittle && overuseRate_ != 0)) {
        if (overuseRate_ != 0 && target_ >= overuseRate_ && now - lastOveruse_ > forgetOveruse) {
            overuseRate_ = 0; // Long enough above it: what the path takes may have grown.
        }
        const double probe =
            rules_.probeIncrease *
            (report.roundTripUs ? std::clamp(probeRoundTripUs / double(report.roundTripUs), 1.0, fastestProbe) : 1.0);
        const double growth = overuseRate_ == 0                ? probe
                              : target_ < overuseRate_ * 0.9 ? rules_.recoveryIncrease
                                                               : rules_.increase;
        target_ *= 1 + growth * elapsed;
    }
    clamp();
    return uint32_t(target_);
}

std::optional<uint32_t> RateController::settle(uint32_t bitsPerSecond) {
    if (bitsPerSecond != uint32_t(target_)) { // The proposal keeps its fraction.
        target_ = bitsPerSecond;
        clamp();
    }
    const double change = target_ > reported_ ? target_ - reported_ : reported_ - target_;
    if (change >= 0.05 * reported_) {
        reported_ = uint32_t(target_);
        return reported_;
    }
    return std::nullopt;
}

} // namespace rcsdk::protocol
