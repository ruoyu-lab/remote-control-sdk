// Sounds a host hands over each on its own, mixed into the one sent:
// ReplayKit's applications' sound and microphone come in sample buffers of
// their own (rcs_session_push_sample_buffer), each on its own clock and in
// buffers of its own length (often 1024 frames, at 44.1 and 48 kHz), and each
// is converted to 48 kHz stereo on its own before it comes here.
//
// They are mixed as they arrive; their timestamps are not compared (they may
// come from different clocks). While both come, a frame goes out once both
// have arrived, so neither breaks up while the other's buffers come at
// another pace: the one ahead waits, by about one of the other's buffers. A
// source that stops (a muted microphone; ReplayKit may send nothing while
// the applications are quiet) holds the other up no longer than its longest
// pause between buffers over the last few seconds plus 20 ms (60 ms before it
// paused at all); then the other goes out as it comes, with what the stopped
// one left. It joins again with its next buffer. A pause of more than half a
// second is a stop: what was left from before it is stale and dropped.
//
// Their clocks differ slightly, and a buffer may go missing: then one runs
// ahead of the other for good, and what it holds back grows, which would grow
// the delay. Over each second while both come, the least each held after a
// mix is how far it is ahead; from 10 ms on, that much of it is dropped,
// cross-faded over 2 ms (with clocks 100 ppm apart: 10 ms every 100 s).
#pragma once

#include "media/audio.hpp"

#include <array>
#include <climits>
#include <cstdint>
#include <span>
#include <vector>

namespace rcsdk::audio {

class Mixer {
public:
    static constexpr uint32_t sources = 2;

    // Source `source`'s next sound, 48 kHz stereo float32, times `gain`,
    // arriving at `nowNs` (a monotonic clock); appends what goes out now to
    // `out`, held within full scale.
    void push(uint32_t source, std::span<const float> samples, float gain, uint64_t nowNs, std::vector<float>& out);
    // `source` is left out from now on, what it holds dropped: the others no
    // longer wait for it. It joins again when it is pushed.
    void leave(uint32_t source) noexcept;
    // Forgets everything, as at the start.
    void reset() noexcept;

    struct Counters {
        uint64_t mixedFrames = 0;   // Went out.
        uint64_t droppedFrames = 0; // Of a source ahead of the others, stale, or left out.
        uint64_t drops = 0;         // Times a source ahead was set back.
        uint32_t heldFrames[sources] = {}; // Each source's, now.
    };
    Counters counters() const noexcept;

private:
    struct Source {
        std::vector<float> pending; // Arrived, not yet out.
        bool joined = false;
        uint64_t lastNs = 0;         // Its latest buffer.
        uint64_t gapNs[2] = {};      // Its longest pause between buffers in this span and the last...
        uint64_t spanStartNs = 0;    // ...of two seconds each.
        uint32_t lowest = UINT32_MAX; // The least it held after a mix in this window.
        uint32_t ahead = 0;           // To drop with its next buffer.
        size_t frames() const noexcept { return pending.size() / audioChannels; }
    };

    // Whether `source` is still expected, or held others up too long.
    bool expected(const Source& source, uint64_t nowNs) const noexcept;
    void drop(Source& source, size_t frames) noexcept;
    void restartWindow() noexcept;

    std::array<Source, sources> sources_;
    uint32_t window_ = 0; // Frames out while every joined source came, since the last look.
    Counters counters_;
};

} // namespace rcsdk::audio
