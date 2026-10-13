// AV1 bitstream helpers (AV1 specification, section 5): OBU splitting and the
// sequence header fields that ISO-BMFF's AV1CodecConfigurationRecord (av1C)
// carries. EncodedPacket for AV1: data holds a temporal unit's OBUs without
// temporal delimiters or sequence headers; config holds the sequence header
// OBU. Every OBU has obu_has_size_field set (low-overhead format). Malformed
// input throws Failure(RCS_ERROR_CODEC); nothing reads outside what it is given.
#pragma once

#include "util/util.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace rcsdk::av1 {

enum : unsigned { obuSequenceHeader = 1, obuTemporalDelimiter = 2, obuPadding = 15 };

struct Obu {
    unsigned type = 0;
    std::span<const uint8_t> bytes;   // The whole OBU, header included...
    std::span<const uint8_t> payload; // ...and what follows its header and size.
};

// Splits low-overhead OBUs.
inline std::vector<Obu> obus(std::span<const uint8_t> buffer) {
    std::vector<Obu> result;
    size_t offset = 0;
    while (offset < buffer.size()) {
        const size_t begin = offset;
        const uint8_t header = buffer[offset++];
        if ((header & 0x80) || !(header & 0x02)) { // Forbidden bit, or no size field.
            fail(RCS_ERROR_CODEC, "Invalid AV1 OBU header");
        }
        if (header & 0x04) { // Extension header.
            if (offset >= buffer.size()) {
                fail(RCS_ERROR_CODEC, "Truncated AV1 OBU");
            }
            ++offset;
        }
        uint64_t size = 0;
        for (unsigned i = 0;; ++i) { // leb128
            if (i == 8 || offset >= buffer.size()) {
                fail(RCS_ERROR_CODEC, "Invalid AV1 OBU size");
            }
            const uint8_t byte = buffer[offset++];
            size |= uint64_t(byte & 0x7f) << (7 * i);
            if (!(byte & 0x80)) {
                break;
            }
        }
        if (size > buffer.size() - offset) {
            fail(RCS_ERROR_CODEC, "AV1 OBU exceeds the buffer");
        }
        result.push_back({unsigned(header >> 3) & 15, buffer.subspan(begin, offset + size - begin),
                          buffer.subspan(offset, size)});
        offset += size;
    }
    return result;
}

// The fields of av1C (AV1-ISOBMFF section 2.3) taken from a sequence header.
struct SequenceInfo {
    unsigned profile = 0, level = 0, tier = 0;
    bool highBitDepth = false, twelveBit = false, monochrome = false;
    unsigned subsamplingX = 1, subsamplingY = 1, chromaSamplePosition = 0;
};

namespace detail {
class Bits {
public:
    explicit Bits(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    uint32_t read(unsigned count) {
        uint32_t value = 0;
        while (count--) {
            if (position_ >= bytes_.size() * 8) {
                fail(RCS_ERROR_CODEC, "Truncated AV1 sequence header");
            }
            value = (value << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1);
            ++position_;
        }
        return value;
    }
    bool flag() { return read(1) != 0; }
    void uvlc() {
        unsigned zeros = 0;
        while (!flag()) {
            if (++zeros >= 32) {
                fail(RCS_ERROR_CODEC, "Invalid AV1 uvlc");
            }
        }
        read(zeros);
    }

private:
    std::span<const uint8_t> bytes_;
    size_t position_ = 0;
};
} // namespace detail

// Parses a sequence header OBU: the whole buffer, header included.
inline SequenceInfo sequenceInfo(std::span<const uint8_t> obu) {
    const auto split = obus(obu);
    if (split.size() != 1 || split[0].type != obuSequenceHeader) {
        fail(RCS_ERROR_CODEC, "Not one AV1 sequence header");
    }
    detail::Bits bits(split[0].payload);
    SequenceInfo info;
    info.profile = bits.read(3);
    bits.read(1); // still_picture
    const bool reduced = bits.flag();
    bool decoderModel = false;
    unsigned delayLength = 0;
    if (reduced) {
        info.level = bits.read(5);
    } else {
        if (bits.flag()) { // timing_info_present_flag
            bits.read(32);
            bits.read(32);
            if (bits.flag()) { // equal_picture_interval
                bits.uvlc();
            }
            decoderModel = bits.flag();
            if (decoderModel) {
                delayLength = bits.read(5) + 1;
                bits.read(32);
                bits.read(5);
                bits.read(5);
            }
        }
        const bool initialDisplayDelay = bits.flag();
        const unsigned operatingPoints = bits.read(5) + 1;
        for (unsigned i = 0; i < operatingPoints; ++i) {
            bits.read(12); // operating_point_idc
            const unsigned level = bits.read(5);
            const unsigned tier = level > 7 ? bits.read(1) : 0;
            if (i == 0) {
                info.level = level;
                info.tier = tier;
            }
            if (decoderModel && bits.flag()) {
                bits.read(delayLength);
                bits.read(delayLength);
                bits.read(1);
            }
            if (initialDisplayDelay && bits.flag()) {
                bits.read(4);
            }
        }
    }
    const unsigned widthBits = bits.read(4) + 1, heightBits = bits.read(4) + 1;
    bits.read(widthBits);
    bits.read(heightBits);
    if (!reduced && bits.flag()) { // frame_id_numbers_present_flag
        bits.read(4);
        bits.read(3);
    }
    bits.read(3); // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter
    if (!reduced) {
        bits.read(4); // interintra, masked compound, warped motion, dual filter
        const bool orderHint = bits.flag();
        if (orderHint) {
            bits.read(2); // jnt_comp, ref_frame_mvs
        }
        unsigned screenContentTools = 2;
        if (!bits.flag()) { // seq_choose_screen_content_tools
            screenContentTools = bits.read(1);
        }
        if (screenContentTools > 0 && !bits.flag()) { // seq_choose_integer_mv
            bits.read(1);
        }
        if (orderHint) {
            bits.read(3);
        }
    }
    bits.read(3); // superres, cdef, restoration
    // color_config()
    info.highBitDepth = bits.flag();
    if (info.profile == 2 && info.highBitDepth) {
        info.twelveBit = bits.flag();
    }
    info.monochrome = info.profile == 1 ? false : bits.flag();
    unsigned primaries = 2, transfer = 2, matrix = 2;
    if (bits.flag()) { // color_description_present_flag
        primaries = bits.read(8);
        transfer = bits.read(8);
        matrix = bits.read(8);
    }
    if (info.monochrome) {
        bits.read(1); // color_range
        info.subsamplingX = info.subsamplingY = 1;
        return info;
    }
    if (primaries == 1 && transfer == 13 && matrix == 0) { // sRGB, 4:4:4
        info.subsamplingX = info.subsamplingY = 0;
        return info;
    }
    bits.read(1); // color_range
    if (info.profile == 0) {
        info.subsamplingX = info.subsamplingY = 1;
    } else if (info.profile == 1) {
        info.subsamplingX = info.subsamplingY = 0;
    } else if (info.twelveBit) {
        info.subsamplingX = bits.read(1);
        info.subsamplingY = info.subsamplingX ? bits.read(1) : 0;
    } else {
        info.subsamplingX = 1;
        info.subsamplingY = 0;
    }
    if (info.subsamplingX && info.subsamplingY) {
        info.chromaSamplePosition = bits.read(2);
    }
    return info;
}

// AV1CodecConfigurationRecord for a sequence header OBU.
inline std::vector<uint8_t> codecConfiguration(std::span<const uint8_t> sequenceHeader) {
    const auto info = sequenceInfo(sequenceHeader);
    std::vector<uint8_t> record{
        0x81, // marker, version 1
        uint8_t(info.profile << 5 | info.level),
        uint8_t(info.tier << 7 | info.highBitDepth << 6 | info.twelveBit << 5 | info.monochrome << 4 |
                info.subsamplingX << 3 | info.subsamplingY << 2 | info.chromaSamplePosition),
        0, // No initial presentation delay.
    };
    record.insert(record.end(), sequenceHeader.begin(), sequenceHeader.end());
    return record;
}

} // namespace rcsdk::av1
