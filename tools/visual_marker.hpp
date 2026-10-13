// Optical response marker shared by the latency targets and the probe.
//
// A target draws 32 black/white bars between 20% and 80% of the screen width,
// from 42% to 58% of its height: a 16-bit sync word, an 8-bit counter and the
// counter's complement. Pressing Space advances the counter.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace marker {

inline uint32_t bits(uint8_t id) {
    return 0xb39c0000u | (uint32_t(id) << 8) | uint8_t(~id);
}

// Rows the decoder reads, as a fraction of the frame height.
inline uint32_t row(uint32_t height) {
    return uint32_t(height * 0.48);
}

// Decodes the counter from one row of luma/gray values (0..255); -1 if absent.
// Bars are located from the sync pattern, so letterboxing and scaling are fine.
inline int decode(const uint8_t* gray, uint32_t width) {
    if (!gray || width < 160) {
        return -1;
    }
    const auto white = [&](uint32_t x) {
        return gray[x] >= 185;
    };
    struct Run {
        uint32_t start, end;
        bool white;
    };
    Run runs[7]{};
    unsigned count = 0;
    uint32_t start = 0;
    bool current = white(0);
    for (uint32_t x = 1; x <= width; ++x) {
        const bool next = x < width && white(x);
        if (x < width && next == current) {
            continue;
        }
        if (count == 7) {
            for (unsigned j = 0; j < 6; ++j) {
                runs[j] = runs[j + 1];
            }
            count = 6;
        }
        runs[count++] = {start, x, current};
        start = x;
        current = next;
        // Sync word 1011 0011 1001 1100: runs of 1,1,2,2,3,2,3 bars starting white.
        if (count < 7 || !runs[0].white || !runs[2].white || !runs[4].white || !runs[6].white) {
            continue;
        }
        const double unit = double(runs[6].start - runs[0].start) / 11;
        if (unit < 2 || runs[0].start + 32 * unit > width) {
            continue;
        }
        static constexpr int lengths[] = {1, 1, 2, 2, 3, 2, 3};
        bool shape = true;
        for (unsigned j = 0; j < 7; ++j) {
            shape &= std::abs(double(runs[j].end - runs[j].start) - unit * lengths[j]) <= unit * .3 + 1;
        }
        if (!shape) {
            continue;
        }
        uint32_t value = 0;
        bool valid = true;
        for (unsigned j = 0; j < 32 && valid; ++j) {
            const uint8_t g = gray[uint32_t(runs[0].start + (j + .5) * unit)];
            valid = g <= 70 || g >= 185; // Reject blurred transitions.
            value = (value << 1) | (g >= 185);
        }
        if (valid && (value >> 16) == 0xb39c && uint8_t(value) == uint8_t(~(value >> 8))) {
            return int((value >> 8) & 255);
        }
    }
    return -1;
}

// Averages three BGRA rows into gray values and decodes them.
inline int decodeBgra(const uint8_t* pixels, uint32_t width, uint32_t height, size_t stride) {
    if (!pixels || height < 100) {
        return -1;
    }
    std::vector<uint8_t> gray(width);
    const uint8_t* first = pixels + size_t(row(height) - 1) * stride;
    for (uint32_t x = 0; x < width; ++x) {
        unsigned sum = 0;
        for (unsigned r = 0; r < 3; ++r) {
            const uint8_t* p = first + r * stride + x * 4;
            sum += p[0] + p[1] + p[2];
        }
        gray[x] = uint8_t(sum / 9);
    }
    return decode(gray.data(), width);
}

} // namespace marker
