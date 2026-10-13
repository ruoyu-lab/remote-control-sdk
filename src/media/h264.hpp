// H.264 sequence parameter set rewriting. An SPS it cannot parse throws
// Failure(RCS_ERROR_CODEC).
#pragma once

#include "util/util.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace rcsdk::h264 {
namespace detail {
struct Reader {
    std::vector<uint8_t> bytes;
    size_t position = 0;
    uint32_t read(unsigned count) {
        if (count > 32 || position + count > bytes.size() * 8) {
            fail(RCS_ERROR_CODEC, "Truncated H.264 SPS");
        }
        uint32_t value = 0;
        while (count--) {
            value = (value << 1) | ((bytes[position / 8] >> (7 - position % 8)) & 1);
            ++position;
        }
        return value;
    }
    uint32_t ue() {
        unsigned zeros = 0;
        while (!read(1)) {
            if (++zeros > 31) {
                fail(RCS_ERROR_CODEC, "H.264 SPS Exp-Golomb overflow");
            }
        }
        return static_cast<uint32_t>((uint64_t{1} << zeros) - 1 + read(zeros));
    }
    int64_t se() {
        const auto v = ue();
        return v & 1 ? (static_cast<int64_t>(v) + 1) / 2 : -static_cast<int64_t>(v) / 2;
    }
    void trailing() {
        if (!read(1) || bytes.size() * 8 - position > 7) {
            fail(RCS_ERROR_CODEC, "Invalid H.264 SPS trailing bits");
        }
        while (position < bytes.size() * 8) {
            if (read(1)) {
                fail(RCS_ERROR_CODEC, "Invalid H.264 SPS padding");
            }
        }
    }
};
struct Writer {
    std::vector<uint8_t> bytes;
    size_t position = 0;
    void bit(uint32_t value) {
        if (position % 8 == 0) {
            bytes.push_back(0);
        }
        bytes.back() |= (value & 1) << (7 - position % 8);
        ++position;
    }
    void ue(uint32_t value) {
        const uint64_t code = static_cast<uint64_t>(value) + 1;
        unsigned length = 0;
        for (auto n = code; n; n >>= 1) {
            ++length;
        }
        for (unsigned i = 1; i < length; ++i) {
            bit(0);
        }
        for (unsigned i = length; i > 0; --i) {
            bit(static_cast<uint32_t>(code >> (i - 1)));
        }
    }
    void prefix(const Reader& source, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            bit((source.bytes[i / 8] >> (7 - i % 8)) & 1);
        }
    }
};
inline void hrd(Reader& r) {
    const auto count = r.ue();
    if (count > 31) {
        fail(RCS_ERROR_CODEC, "Invalid H.264 HRD count");
    }
    r.read(8);
    for (uint32_t i = 0; i <= count; ++i) {
        r.ue();
        r.ue();
        r.read(1);
    }
    r.read(20);
}
inline void vuiPrefix(Reader& r) {
    if (r.read(1)) {
        if (r.read(8) == 255) {
            r.read(32);
        }
    }
    if (r.read(1)) {
        r.read(1);
    }
    if (r.read(1)) {
        r.read(4);
        if (r.read(1)) {
            r.read(24);
        }
    }
    if (r.read(1)) {
        r.ue();
        r.ue();
    }
    if (r.read(1)) {
        r.read(32);
        r.read(32);
        r.read(1);
    }
    const bool nal = r.read(1);
    if (nal) {
        hrd(r);
    }
    const bool vcl = r.read(1);
    if (vcl) {
        hrd(r);
    }
    if (nal || vcl) {
        r.read(1);
    }
    r.read(1);
}
} // namespace detail

// Only for an encoder explicitly configured to produce no reordered pictures.
// Preserves picture syntax and existing VUI color/timing/HRD fields. Adds or
// replaces only the decoded-picture buffering/reordering declaration (H.264 E.1).
inline std::vector<uint8_t> noReorderingSps(std::span<const uint8_t> nal) {
    using namespace detail;
    if (nal.size() < 4 || nal.size() > 4096 || (nal[0] & 0x9f) != 7) {
        fail(RCS_ERROR_CODEC, "Invalid H.264 SPS NAL");
    }
    Reader r;
    unsigned zeros = 0;
    for (size_t i = 1; i < nal.size(); ++i) {
        const auto value = nal[i];
        if (zeros == 2 && value == 3) {
            if (i + 1 >= nal.size() || nal[i + 1] > 3) {
                fail(RCS_ERROR_CODEC, "Invalid H.264 emulation prevention");
            }
            zeros = 0;
            continue;
        }
        r.bytes.push_back(value);
        zeros = value == 0 ? std::min(2u, zeros + 1) : 0;
    }
    const auto profile = r.read(8);
    r.read(16);
    r.ue();
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        const auto chroma = r.ue();
        if (chroma > 3) {
            fail(RCS_ERROR_CODEC, "Invalid H.264 chroma format");
        }
        if (chroma == 3) {
            r.read(1);
        }
        r.ue();
        r.ue();
        r.read(1);
        if (r.read(1)) {
            for (unsigned list = 0; list < (chroma == 3 ? 12u : 8u); ++list) {
                if (!r.read(1)) {
                    continue;
                }
                int64_t last = 8, next = 8;
                for (unsigned i = 0; i < (list < 6 ? 16u : 64u); ++i) {
                    if (next) {
                        next = (last + r.se() + 256) % 256;
                    }
                    if (next < 0) {
                        fail(RCS_ERROR_CODEC, "Invalid H.264 scaling delta");
                    }
                    if (next) {
                        last = next;
                    }
                }
            }
        }
    } else if (profile != 66 && profile != 77 && profile != 88) {
        fail(RCS_ERROR_CODEC, "Unsupported H.264 SPS profile");
    }
    r.ue();
    const auto poc = r.ue();
    if (poc == 0) {
        r.ue();
    } else if (poc == 1) {
        r.read(1);
        r.se();
        r.se();
        const auto count = r.ue();
        if (count > 255) {
            fail(RCS_ERROR_CODEC, "Invalid H.264 POC cycle");
        }
        for (uint32_t i = 0; i < count; ++i) {
            r.se();
        }
    } else if (poc != 2) {
        fail(RCS_ERROR_CODEC, "Invalid H.264 POC type");
    }
    const auto references = r.ue();
    if (references > 16) {
        fail(RCS_ERROR_CODEC, "Invalid H.264 reference count");
    }
    r.read(1);
    r.ue();
    r.ue();
    if (!r.read(1)) {
        r.read(1);
    }
    r.read(1);
    if (r.read(1)) {
        for (unsigned i = 0; i < 4; ++i) {
            r.ue();
        }
    }
    const auto vuiAt = r.position;
    const bool present = r.read(1);
    Writer w;
    uint32_t buffer = std::max(1u, references);
    if (present) {
        vuiPrefix(r);
        const auto restrictionAt = r.position;
        if (r.read(1)) {
            r.read(1);
            for (unsigned i = 0; i < 4; ++i) {
                r.ue();
            }
            w.prefix(r, r.position);
            r.ue();
            buffer = std::max(buffer, r.ue());
            if (buffer > 16) {
                fail(RCS_ERROR_CODEC, "Invalid H.264 DPB size");
            }
        } else {
            w.prefix(r, restrictionAt);
            w.bit(1);
            w.bit(1);
            w.ue(2);
            w.ue(1);
            w.ue(16);
            w.ue(16);
        }
    } else {
        w.prefix(r, vuiAt);
        w.bit(1);
        for (unsigned i = 0; i < 8; ++i) {
            w.bit(0);
        }
        w.bit(1);
        w.bit(1);
        w.ue(2);
        w.ue(1);
        w.ue(16);
        w.ue(16);
    }
    r.trailing();
    w.ue(0);
    w.ue(buffer);
    w.bit(1);
    std::vector<uint8_t> output{nal[0]};
    zeros = 0;
    for (const auto value : w.bytes) {
        if (zeros == 2 && value <= 3) {
            output.push_back(3);
            zeros = 0;
        }
        output.push_back(value);
        zeros = value == 0 ? std::min(2u, zeros + 1) : 0;
    }
    return output;
}
} // namespace rcsdk::h264
