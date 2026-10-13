// The viewer's playback buffer between the network and the output device.
//
// One producer (the thread decoding arrivals) and one consumer (the device's
// render callback) share a lock-free ring. The buffer aims at a target delay
// that follows the network's jitter: the spread of arrival times against the
// sharer's capture clock over the last eight seconds, plus one packet, one
// device pull and the host's margin (default 2 ms), never below 1 ms or above
// the host's max_delay_ms, unless one device pull and one packet alone take
// more. Eight seconds, because a wireless receiver's radio naps every few
// seconds (100 ms and more): a buffer that shrank between the naps would run
// dry at each, and then drop what piled up. Its longer naps come further
// apart: the jitter an underrun showed is held for 30 s of playback, twice as
// long with each underrun soon after (up to ten minutes). For the
// first three seconds, before the buffer knows the path, it assumes 40 ms
// (at most a quarter of max_delay_ms). A pull never reads sound that has not
// arrived: what is missing is made up (see below).
//
// The sharer's and viewer's sample clocks differ slightly (tens of parts per
// million), so the level drifts. Every half second the consumer looks at the
// lowest level a pull found: close to running dry, it repeats what is
// missing; more than the jitter above that, it drops to the middle. At any
// device period this happens every several seconds, not on every pull. The buffer
// so holds what the path needs and no more: on a LAN, about one device pull
// plus a few milliseconds. Both cross-fade over the
// change, so neither clicks. When playback starts, or the level is more than
// 20 ms above the target (sound piled up during a stall), it jumps straight
// to the target. A single short pull (a late packet) repeats the sound just
// played, cross-faded; a second in a row plays silence and primes again.
#pragma once

#include "media/audio.hpp"

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <span>
#include <vector>

namespace rcsdk::audio {

class Playout {
public:
    // `marginUs`: what the buffer keeps above one pull and the jitter.
    Playout(uint32_t maxDelayMs, uint32_t marginUs);

    // Producer. `captureNs` is the sharer's clock for the first sample;
    // `arrivalNs` the local clock when the packet arrived.
    void push(std::span<const float> samples, uint64_t captureNs, uint64_t arrivalNs);

    // Consumer: fills `frames` interleaved stereo frames. Real-time safe: no
    // locks, no allocation.
    void pull(float* out, uint32_t frames) noexcept;

    // The sound comes over another path now, or over the same after a gap
    // (rcs_paths_config): the jitter it measured and the underruns of the gap
    // say nothing of what comes, which it learns afresh, as at the start
    // (instead of holding the gap as jitter for up to ten minutes). Any thread.
    void discontinuity() noexcept { restart_.store(true, std::memory_order_relaxed); }

    struct Counters {
        uint64_t underruns = 0, droppedFrames = 0, concealedFrames = 0, playedFrames = 0;
        uint64_t adjustments = 0; // Drift corrections (drops and repeats), each audible if frequent.
        uint32_t bufferedFrames = 0, targetFrames = 0;
    };
    Counters counters() const noexcept;

private:
    static constexpr size_t capacityFrames = 1u << 17; // 2.7 s
    static constexpr uint32_t minimumTarget = audioRate / 1000; // 1 ms

    uint32_t level() const noexcept;
    float sample(uint64_t frame, uint32_t channel) const noexcept;
    void updateTarget(uint64_t captureNs, uint64_t arrivalNs) noexcept;

    const uint32_t maxFrames_, margin_;
    std::vector<float> ring_;
    std::atomic<uint64_t> written_{0}, read_{0}; // Frames.
    std::atomic<uint32_t> target_;

    // Producer: arrival spread over the last sixteen half-second buckets.
    struct Bucket {
        int64_t lowest = INT64_MAX, highest = INT64_MIN;
    };
    std::array<Bucket, 16> buckets_{};
    uint64_t bucketStartNs_ = 0;
    size_t bucket_ = 0;
    std::atomic<uint32_t> packetFrames_{0}; // The level dips by one packet between arrivals.
    std::atomic<uint32_t> spread_{0};       // Arrival jitter over the last eight seconds.
    std::atomic<bool> restart_{false};      // discontinuity(): the producer forgets its buckets...
    std::atomic<bool> forget_{false};       // ...and then the consumer what it holds.

    // Consumer.
    uint32_t heldSpread_;  // Jitter an underrun showed (or assumed at the start)...
    uint64_t heldUntil_;   // ...until this many frames have played...
    uint64_t hold_;        // ...for this long after the latest underrun.
    bool playing_ = false;
    bool started_ = false;    // Played since priming ended.
    uint32_t shortPulls_ = 0; // Consecutive pulls the buffer could not fill.
    uint32_t windowMin_ = UINT32_MAX; // The lowest level a pull found in this window...
    uint32_t windowFrames_ = 0;       // ...of half a second of pulls.
    std::atomic<uint64_t> underruns_{0}, dropped_{0}, concealed_{0}, played_{0}, adjustments_{0};
};

} // namespace rcsdk::audio
