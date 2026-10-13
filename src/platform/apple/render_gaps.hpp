// Core Audio renders once per device cycle and stamps each call with the
// cycle's sample time; when the call (or the system) came too late for a
// cycle, the device played without it and the next stamp jumps past it
// (a 60 ms stall: 2560-2816 frames at 48 kHz, on the output unit and on an
// AVAudioEngine source node alike). A jump of more than a second is not a
// late call, nor is the first call after restart() (a new unit or route).
#pragma once

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>

namespace rcsdk::platform::apple {

class RenderGaps {
public:
    // The real-time thread, each render call.
    void rendered(const AudioTimeStamp* time, uint32_t frames) noexcept {
        if (!time || !(time->mFlags & kAudioTimeStampSampleTimeValid)) {
            return;
        }
        const double next = next_.load(std::memory_order_relaxed);
        const double skipped = time->mSampleTime - next;
        if (next >= 0 && skipped >= 1 && skipped <= 48'000) {
            gaps_.fetch_add(1, std::memory_order_relaxed);
        }
        next_.store(time->mSampleTime + frames, std::memory_order_relaxed);
    }
    // Any thread: the next call starts counting afresh.
    void restart() noexcept { next_.store(-1, std::memory_order_relaxed); }
    uint64_t count() const noexcept { return gaps_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> next_{-1}; // Where the next call's stamp should be.
    std::atomic<uint64_t> gaps_{0};
};

} // namespace rcsdk::platform::apple
