#include "audio/mixer.hpp"

#include <algorithm>

namespace rcsdk::audio {
namespace {

constexpr uint64_t msNs = 1'000'000;
constexpr uint64_t firstQuietNs = 60 * msNs;  // Before a source paused between buffers at all.
constexpr uint64_t quietMarginNs = 20 * msNs; // Beyond its longest pause.
constexpr uint64_t stopNs = 500 * msNs;       // A longer pause is a stop: what it left is stale.
constexpr uint64_t spanNs = 2000 * msNs;
constexpr uint32_t aheadLimit = audioRate / 100; // 10 ms
constexpr uint32_t crossFade = audioRate / 500;  // 2 ms
constexpr size_t mostFrames = audioRate;         // Held at most: a second.

uint64_t since(uint64_t nowNs, uint64_t thenNs) noexcept {
    return nowNs > thenNs ? nowNs - thenNs : 0;
}

} // namespace

bool Mixer::expected(const Source& source, uint64_t nowNs) const noexcept {
    const uint64_t longest = std::max(source.gapNs[0], source.gapNs[1]);
    return since(nowNs, source.lastNs) <= (longest ? longest + quietMarginNs : firstQuietNs);
}

void Mixer::drop(Source& source, size_t frames) noexcept {
    source.pending.erase(source.pending.begin(), source.pending.begin() + ptrdiff_t(frames * audioChannels));
    counters_.droppedFrames += frames;
}

void Mixer::restartWindow() noexcept {
    window_ = 0;
    for (auto& source : sources_) {
        source.lowest = UINT32_MAX;
    }
}

void Mixer::push(uint32_t index, std::span<const float> samples, float gain, uint64_t nowNs, std::vector<float>& out) {
    auto& source = sources_[index];
    const uint64_t gap = since(nowNs, source.lastNs);
    if (!source.joined || gap > stopNs) { // Joins, or comes back after a stop: what it left is stale.
        drop(source, source.frames());
        source.joined = true;
        source.gapNs[0] = source.gapNs[1] = 0;
        source.spanStartNs = nowNs;
        source.ahead = 0;
        restartWindow();
    } else {
        if (nowNs - source.spanStartNs >= spanNs) {
            source.gapNs[1] = source.gapNs[0];
            source.gapNs[0] = 0;
            source.spanStartNs = nowNs;
        }
        source.gapNs[0] = std::max(source.gapNs[0], gap);
    }
    source.lastNs = nowNs;

    const size_t start = source.pending.size();
    source.pending.resize(start + samples.size());
    std::transform(samples.begin(), samples.end(), source.pending.begin() + ptrdiff_t(start),
                   [gain](float sample) { return sample * gain; });
    if (source.frames() > mostFrames) {
        drop(source, source.frames() - mostFrames);
    }
    // Ahead of the others for good: set back by that much, the sound before
    // the cut faded into the sound after it.
    if (source.ahead && source.frames() > crossFade) {
        const size_t cut = std::min<size_t>(source.ahead, source.frames() - crossFade);
        auto& p = source.pending;
        for (size_t i = 0; i < crossFade * audioChannels; ++i) {
            const float a = float(i / audioChannels + 1) / float(crossFade + 1);
            p[cut * audioChannels + i] = p[i] * (1 - a) + p[cut * audioChannels + i] * a;
        }
        drop(source, cut);
        ++counters_.drops;
        source.ahead = 0;
    }

    // Out: as far as every source still expected has arrived; one that
    // stopped gives what it left, unless that is stale.
    size_t frames = source.frames();
    uint32_t coming = 0;
    bool together = true;
    for (auto& s : sources_) {
        if (!s.joined) {
            continue;
        }
        if (&s == &source || expected(s, nowNs)) {
            frames = std::min(frames, s.frames());
            ++coming;
        } else {
            if (since(nowNs, s.lastNs) > stopNs) {
                drop(s, s.frames());
            }
            together = false;
        }
    }
    if (frames) {
        const size_t at = out.size();
        out.resize(at + frames * audioChannels, 0.0f);
        for (auto& s : sources_) {
            const size_t n = std::min(frames, s.frames()) * audioChannels;
            for (size_t i = 0; i < n; ++i) {
                out[at + i] += s.pending[i];
            }
            s.pending.erase(s.pending.begin(), s.pending.begin() + ptrdiff_t(n));
        }
        std::for_each(out.begin() + ptrdiff_t(at), out.end(), [](float& v) { v = std::clamp(v, -1.0f, 1.0f); });
        counters_.mixedFrames += frames;
    }

    // How far each is ahead, while all come.
    if (coming < 2 || !together) {
        restartWindow();
        return;
    }
    for (auto& s : sources_) {
        if (s.joined) {
            s.lowest = std::min(s.lowest, uint32_t(s.frames()));
        }
    }
    window_ += uint32_t(frames);
    if (window_ >= audioRate) {
        for (auto& s : sources_) {
            if (s.joined && s.lowest >= aheadLimit) {
                s.ahead = s.lowest;
            }
        }
        restartWindow();
    }
}

void Mixer::leave(uint32_t index) noexcept {
    auto& source = sources_[index];
    if (!source.joined) {
        return;
    }
    drop(source, source.frames());
    source.joined = false;
    source.ahead = 0;
    restartWindow();
}

Mixer::Counters Mixer::counters() const noexcept {
    Counters counters = counters_;
    for (uint32_t i = 0; i < sources; ++i) {
        counters.heldFrames[i] = uint32_t(sources_[i].frames());
    }
    return counters;
}

void Mixer::reset() noexcept {
    sources_ = {};
    window_ = 0;
    counters_ = {};
}

} // namespace rcsdk::audio
