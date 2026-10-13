#include "protocol/stall_watch.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rcsdk::protocol {

using namespace std::chrono_literals;

namespace {

constexpr auto decideWithin = 50ms; // A gap's end without a send time waits this long for one that has.
constexpr size_t mostStalls = 64;   // Kept for the pace: enough for 10 s of a radio's beat.
constexpr std::chrono::milliseconds shortestPeriod = 200ms, longestPeriod = 3s;

uint32_t milliseconds(StallWatch::Clock::duration d) {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

} // namespace

std::chrono::microseconds StallWatch::queued(uint32_t sentUs, uint64_t arrivedNs, Clock::time_point now) {
    const uint32_t raw = uint32_t(arrivedNs / 1000) - sentUs;
    delay_ = previousRaw_ ? delay_ + int32_t(raw - *previousRaw_) : raw;
    previousRaw_ = raw;
    if (lowest_.empty() || now - lowest_.back().first >= 1s) {
        lowest_.emplace_back(now, delay_);
    }
    lowest_.back().second = std::min(lowest_.back().second, delay_);
    while (now - lowest_.front().first > window) {
        lowest_.pop_front();
    }
    int64_t lowest = delay_;
    for (const auto& [time, value] : lowest_) {
        lowest = std::min(lowest, value);
    }
    return std::chrono::microseconds(delay_ - lowest);
}

void StallWatch::arrived(uint64_t number, std::optional<uint32_t> sentUs, uint64_t arrivedNs, Clock::time_point now) {
    const auto waited = sentUs ? std::optional(queued(*sentUs, arrivedNs, now)) : std::nullopt;
    if (open_ && (waited || now > open_->until)) {
        if (waited && *waited * 2 >= open_->length) {
            stalled(open_->start, open_->length);
        }
        open_.reset();
    }
    if (last_ && now - *last_ >= minimumGap) {
        const Gap gap{*last_, now - *last_, now + decideWithin};
        if (number > highest_ + 2) { // Sent during the gap, and lost.
            stalled(gap.start, gap.length);
        } else if (!waited) {
            open_ = gap;
        } else if (*waited * 2 >= gap.length) {
            stalled(gap.start, gap.length);
        }
    }
    last_ = now;
    highest_ = std::max(highest_, number);
}

void StallWatch::restart() noexcept {
    previousRaw_.reset();
    delay_ = 0;
    lowest_.clear();
    last_.reset();
    open_.reset();
}

void StallWatch::stalled(Clock::time_point start, Clock::duration length) {
    stalls_.emplace_back(start, milliseconds(length));
    ++total_;
    while (stalls_.size() > mostStalls) {
        stalls_.pop_front();
    }
}

StallWatch::Summary StallWatch::summary(Clock::time_point now) const {
    Summary s;
    s.total = total_;
    std::vector<Clock::time_point> starts;
    std::vector<uint32_t> lengths;
    for (const auto& [start, length] : stalls_) {
        if (now - start <= window) {
            starts.push_back(start);
            lengths.push_back(length);
        }
    }
    s.count = uint32_t(starts.size());
    if (!s.count) {
        return s;
    }
    std::sort(lengths.begin(), lengths.end());
    s.medianMs = lengths[lengths.size() / 2];
    s.longestMs = lengths.back();
    if (starts.size() < 4) {
        return s;
    }
    // Each interval between two stalls is a candidate beat. Its score: the
    // stalls with an earlier one a beat before (within 4%, at least 25 ms),
    // and half for those with one only two or three beats before (beats a
    // radio skipped; what half a beat would make of every one). The pace is
    // the shortest beat nearly as good as the best (twice the beat scores as
    // well), when most stalls are on it.
    const auto beats = [](Clock::duration beat, Clock::duration interval) {
        for (int k = 1; k <= 3; ++k) {
            if (std::chrono::abs(interval - beat * k) <= std::max<Clock::duration>(25ms, beat * k / 25)) {
                return k;
            }
        }
        return 0;
    };
    const auto score = [&](Clock::duration beat, double* refined) {
        double on = 0, sum = 0;
        size_t pairs = 0;
        for (size_t j = 1; j < starts.size(); ++j) {
            int fewest = 0;
            Clock::duration interval{};
            for (size_t i = j; i-- > 0 && starts[j] - starts[i] <= beat * 3 + 25ms;) {
                if (const int k = beats(beat, starts[j] - starts[i]); k && (!fewest || k < fewest)) {
                    fewest = k;
                    interval = starts[j] - starts[i];
                }
            }
            if (fewest) {
                on += fewest == 1 ? 1 : 0.5;
                sum += std::chrono::duration<double>(interval).count() / fewest;
                ++pairs;
            }
        }
        if (refined && pairs) {
            *refined = sum / double(pairs);
        }
        return on;
    };
    double best = 0;
    std::vector<std::pair<Clock::duration, double>> candidates;
    for (size_t i = 0; i < starts.size(); ++i) {
        for (size_t j = i + 1; j < starts.size() && j <= i + 3; ++j) { // Up to two of other causes between.
            const auto beat = starts[j] - starts[i];
            if (beat < shortestPeriod || beat > longestPeriod) {
                continue;
            }
            const double on = score(beat, nullptr);
            candidates.emplace_back(beat, on);
            best = std::max(best, on);
        }
    }
    // Most stalls on it (the first has nothing before it), and at least three.
    if (best < 3 || best < 0.6 * double(starts.size() - 1)) {
        return s;
    }
    Clock::duration chosen = longestPeriod;
    for (const auto& [beat, on] : candidates) {
        if (on >= 0.9 * best) {
            chosen = std::min(chosen, beat);
        }
    }
    double seconds = 0;
    score(chosen, &seconds);
    s.periodMs = uint32_t(std::lround(seconds * 1000));
    return s;
}

} // namespace rcsdk::protocol
