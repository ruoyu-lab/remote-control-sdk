#include "audio/playout.hpp"

#include <algorithm>
#include <cmath>

namespace rcsdk::audio {
namespace {

constexpr uint64_t bucketNs = 500'000'000;
constexpr uint32_t crossFade = audioRate / 500;    // 2 ms over a drop or repeat.
constexpr uint32_t catchUpLimit = audioRate / 50;  // Above the target by more: jump, not trim.
// After an underrun, the largest jitter measured is held for this long (of
// playback) beyond the buckets' eight seconds, and longer while jitter that
// large comes again: a Wi-Fi radio's longer naps come further apart. An
// underrun during a hold or within as long after it doubles the hold, up to
// ten minutes: a link that stalls every minute or two keeps its buffer.
constexpr uint64_t holdFrames = uint64_t(audioRate) * 30, mostHoldFrames = uint64_t(audioRate) * 600;
// Until the buffers know the path: this much jitter (at most a quarter of
// max_delay_ms) for the first seconds, when connecting sends bursts.
constexpr uint32_t startSpread = audioRate / 25;
constexpr uint64_t startFrames = uint64_t(audioRate) * 3;

} // namespace

Playout::Playout(uint32_t maxDelayMs, uint32_t marginUs)
    : maxFrames_(std::max(minimumTarget, uint32_t(uint64_t(maxDelayMs) * audioRate / 1000))),
      margin_(uint32_t(uint64_t(marginUs) * audioRate / 1'000'000)),
      ring_(capacityFrames * audioChannels), target_(std::min(4 * minimumTarget, maxFrames_)),
      heldSpread_(std::min(startSpread, maxFrames_ / 4)), heldUntil_(startFrames), hold_(holdFrames) {}

uint32_t Playout::level() const noexcept {
    return uint32_t(written_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed));
}

float Playout::sample(uint64_t frame, uint32_t channel) const noexcept {
    return ring_[(frame % capacityFrames) * audioChannels + channel];
}

void Playout::updateTarget(uint64_t captureNs, uint64_t arrivalNs) noexcept {
    // Arrival against capture: a constant offset (unrelated clocks) plus the
    // network's queueing. Its spread is the jitter the buffer must absorb.
    const int64_t delay = int64_t(arrivalNs - captureNs);
    if (!bucketStartNs_ || arrivalNs - bucketStartNs_ >= bucketNs) {
        bucket_ = (bucket_ + 1) % buckets_.size();
        buckets_[bucket_] = {};
        bucketStartNs_ = arrivalNs;
    }
    auto& current = buckets_[bucket_];
    current.lowest = std::min(current.lowest, delay);
    current.highest = std::max(current.highest, delay);
    int64_t lowest = INT64_MAX, highest = INT64_MIN;
    for (const auto& b : buckets_) {
        if (b.lowest <= b.highest) {
            lowest = std::min(lowest, b.lowest);
            highest = std::max(highest, b.highest);
        }
    }
    spread_.store(uint32_t(std::min<uint64_t>(uint64_t(highest - lowest) * audioRate / 1'000'000'000, maxFrames_)),
                  std::memory_order_relaxed);
}

void Playout::push(std::span<const float> samples, uint64_t captureNs, uint64_t arrivalNs) {
    if (restart_.exchange(false, std::memory_order_relaxed)) {
        buckets_ = {};
        bucketStartNs_ = 0;
        forget_.store(true, std::memory_order_release);
    }
    const uint64_t frames = samples.size() / audioChannels;
    packetFrames_.store(uint32_t(std::min<uint64_t>(frames, audioRate)), std::memory_order_relaxed);
    updateTarget(captureNs, arrivalNs);
    const uint64_t written = written_.load(std::memory_order_relaxed);
    const uint64_t free = capacityFrames - (written - read_.load(std::memory_order_acquire));
    const uint64_t kept = std::min(frames, free); // Full only while the device stalls.
    for (uint64_t i = 0; i < kept * audioChannels; ++i) {
        ring_[((written + i / audioChannels) % capacityFrames) * audioChannels + i % audioChannels] = samples[i];
    }
    dropped_.fetch_add(frames - kept, std::memory_order_relaxed);
    written_.store(written + kept, std::memory_order_release);
}

void Playout::pull(float* out, uint32_t frames) noexcept {
    const uint64_t read = read_.load(std::memory_order_relaxed);
    uint32_t available = level();
    // What the next pulls need: this pull, a packet's worth (the level dips
    // by one between arrivals), the jitter, a safety margin. At most
    // max_delay_ms, unless one pull and one packet alone take more: a device
    // pulling that much at a time needs it, whatever the host allowed. So the
    // target never falls below this pull, and no pull reads past what arrived.
    const uint64_t played = played_.load(std::memory_order_relaxed);
    if (forget_.exchange(false, std::memory_order_acquire)) { // As at the start.
        heldSpread_ = std::min(startSpread, maxFrames_ / 4);
        heldUntil_ = played + startFrames;
        hold_ = holdFrames;
    }
    const uint32_t measured = spread_.load(std::memory_order_relaxed);
    if (played < heldUntil_) { // Holding: the peak, kept while peaks like it recur.
        if (measured * 4 >= heldSpread_ * 3) {
            heldUntil_ = std::max(heldUntil_, played + hold_);
        }
        heldSpread_ = std::max(heldSpread_, measured);
    }
    const uint32_t spread = played < heldUntil_ ? heldSpread_ : measured;
    const uint32_t packet = packetFrames_.load(std::memory_order_relaxed);
    const uint32_t limit = std::max(maxFrames_, frames + packet);
    const uint32_t target = std::clamp(frames + packet + spread + margin_, minimumTarget, limit);
    target_.store(target, std::memory_order_relaxed);
    played_.fetch_add(frames, std::memory_order_relaxed);

    if (!playing_) { // Priming: silence until the target is buffered.
        if (available < target) {
            std::fill(out, out + size_t(frames) * audioChannels, 0.0f);
            return;
        }
        playing_ = true;
        started_ = false;
        windowMin_ = UINT32_MAX;
        windowFrames_ = 0;
    }
    if (available < frames) {
        // Underrun. Once, it is a late packet: what there is, then the sound
        // just played once more, cross-faded in, which sounds far softer than
        // a gap. Twice in a row, the sound stopped: silence, prime again.
        const bool again = shortPulls_++ > 0;
        const uint32_t missing = frames - available;
        const uint32_t fade = std::min(available, crossFade);
        for (uint32_t i = 0; i < frames; ++i) {
            for (uint32_t c = 0; c < audioChannels; ++c) {
                float value = 0.0f;
                if (i < available) {
                    value = sample(read + i, c);
                } else if (!again) {
                    value = sample(read + i - missing, c); // `missing` frames back: already played.
                    if (const uint32_t j = i - available; j < fade) {
                        const float a = float(j + 1) / float(fade + 1);
                        value = sample(read + available - fade + j, c) * (1 - a) + value * a;
                    }
                }
                out[i * audioChannels + c] = value;
            }
        }
        read_.store(read + available, std::memory_order_release);
        concealed_.fetch_add(missing, std::memory_order_relaxed);
        underruns_.fetch_add(1, std::memory_order_relaxed);
        // The jitter was larger than the buffer knew: by what was missing at
        // least (a second miss in a row: by a pull more), and by what the
        // buckets measure once the late sound arrives. Held, so that the next
        // such gap finds the sound buffered.
        heldSpread_ = std::min(maxFrames_, std::max(spread, measured) + missing + (again ? frames : 0));
        hold_ = played < heldUntil_ + hold_ ? std::min(hold_ * 2, mostHoldFrames) : holdFrames;
        heldUntil_ = played + hold_;
        if (again) {
            playing_ = false;
            shortPulls_ = 0;
        }
        return;
    }
    shortPulls_ = 0;

    // How far to move the read position: forward drops sound, back repeats
    // some. Either way the change is cross-faded over up to 2 ms.
    int64_t shift = 0;
    // Far behind (sound piled up while playback started, or after a stall):
    // straight to the target.
    const bool starting = !started_;
    started_ = true;
    if (available > limit || available > target + catchUpLimit || (starting && available > target)) {
        shift = int64_t(available - target);
        windowMin_ = UINT32_MAX;
        windowFrames_ = 0;
    } else {
        // Drift: over each half second, the lowest level a pull found. Below
        // one pull plus the margin, the next underrun is near: repeat up to the
        // middle of the band above it. Above that band (the jitter), the delay
        // is more than the path needs: drop down to its middle. With clocks tens of ppm
        // apart this happens every several seconds.
        windowMin_ = std::min(windowMin_, available);
        windowFrames_ += frames;
        if (windowFrames_ >= audioRate / 2) {
            // A packet late by the jitter may miss a pull: half the jitter
            // below the band, its other half above. A packet that misses one
            // anyway costs a short repeat, not a gap (see above).
            const uint32_t low = frames + margin_ + spread / 2;
            // Twice the jitter: the window's lowest level itself varies by up
            // to the jitter, and a narrower band would chase that noise.
            const uint32_t band = 2 * std::max({spread, margin_, audioRate / 1000});
            if (windowMin_ < low) {
                shift = -int64_t(std::min(low + band / 2 - windowMin_, audioRate / 200)); // At most 5 ms at a time.
            } else if (windowMin_ > low + band) {
                shift = int64_t(std::min(windowMin_ - (low + band / 2), available - frames));
            }
            windowMin_ = UINT32_MAX;
            windowFrames_ = 0;
        }
    }
    const uint32_t fade = shift ? std::min(frames, crossFade) : 0;
    for (uint32_t i = 0; i < frames; ++i) {
        for (uint32_t c = 0; c < audioChannels; ++c) {
            const float moved = sample(uint64_t(int64_t(read + i) + shift), c);
            if (i < fade) {
                const float a = float(i + 1) / float(fade + 1);
                out[i * audioChannels + c] = sample(read + i, c) * (1 - a) + moved * a;
            } else {
                out[i * audioChannels + c] = moved;
            }
        }
    }
    read_.store(uint64_t(int64_t(read + frames) + shift), std::memory_order_release);
    if (shift) {
        (shift > 0 ? dropped_ : concealed_).fetch_add(uint64_t(std::abs(shift)), std::memory_order_relaxed);
        adjustments_.fetch_add(1, std::memory_order_relaxed);
    }
}

Playout::Counters Playout::counters() const noexcept {
    Counters c;
    c.underruns = underruns_.load(std::memory_order_relaxed);
    c.droppedFrames = dropped_.load(std::memory_order_relaxed);
    c.concealedFrames = concealed_.load(std::memory_order_relaxed);
    c.playedFrames = played_.load(std::memory_order_relaxed);
    c.adjustments = adjustments_.load(std::memory_order_relaxed);
    c.bufferedFrames = level();
    c.targetFrames = target_.load(std::memory_order_relaxed);
    return c;
}

} // namespace rcsdk::audio
