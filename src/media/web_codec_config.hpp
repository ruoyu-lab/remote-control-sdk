// What a browser's WebCodecs VideoDecoder is configured with for the SDK's
// packets: the codec string and the decoder configuration record
// ("description"), from a stream's configuration (EncodedPacket::config:
// parameter sets, 'vpcC', the AV1 sequence header). The packets then go to
// it unchanged (length-prefixed NAL units, VP frames, OBUs).
#pragma once

#include "media/av1.hpp"
#include "media/nal.hpp"
#include "media/sampling.hpp"
#include "media/vpx.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace rcsdk::web {

struct DecoderSetup {
    std::string codec;               // E.g. "avc1.64002a".
    std::vector<uint8_t> description; // avcC, hvcC or av1C; none for VP8 and VP9.
};

// H.264: the AVC decoder configuration record (ISO/IEC 14496-15 5.3.3.1)
// with 4-byte lengths, as the packets carry them; nullopt without an SPS
// and a PPS.
inline std::optional<DecoderSetup> h264Setup(std::span<const uint8_t> config) {
    std::vector<std::span<const uint8_t>> sps, pps;
    for (const auto nal : nal::units(config)) {
        const unsigned type = nal::h264::type(nal);
        if (type == nal::h264::sps && nal.size() >= 4) {
            sps.push_back(nal);
        } else if (type == nal::h264::pps) {
            pps.push_back(nal);
        }
    }
    if (sps.empty() || pps.empty() || sps.size() > 31 || pps.size() > 255) {
        return std::nullopt;
    }
    DecoderSetup setup;
    char codec[16];
    std::snprintf(codec, sizeof(codec), "avc1.%02x%02x%02x", sps[0][1], sps[0][2], sps[0][3]);
    setup.codec = codec;
    auto& d = setup.description;
    d = {1, sps[0][1], sps[0][2], sps[0][3], 0xFF, uint8_t(0xE0 | sps.size())};
    const auto append = [&](std::span<const uint8_t> nal) {
        d.push_back(uint8_t(nal.size() >> 8));
        d.push_back(uint8_t(nal.size()));
        d.insert(d.end(), nal.begin(), nal.end());
    };
    for (const auto nal : sps) {
        append(nal);
    }
    d.push_back(uint8_t(pps.size()));
    for (const auto nal : pps) {
        append(nal);
    }
    return setup;
}

// HEVC: the codec string (ISO/IEC 14496-15 E.3) and the HEVC decoder
// configuration record (8.3.3.1) with 4-byte lengths, from the VPS, SPS and
// PPS; nullopt without all three.
inline std::optional<DecoderSetup> hevcSetup(std::span<const uint8_t> config) {
    std::vector<std::span<const uint8_t>> sets[3]; // VPS, SPS, PPS.
    for (const auto nal : nal::units(config)) {
        const unsigned type = nal::hevc::type(nal);
        if (type >= nal::hevc::vps && type <= nal::hevc::pps) {
            sets[type - nal::hevc::vps].push_back(nal);
        }
    }
    if (sets[0].empty() || sets[1].empty() || sets[2].empty() || sets[1][0].size() < 15) {
        return std::nullopt;
    }
    // profile_tier_level's general part, after the SPS's first byte.
    detail::RbspReader r(sets[1][0], 2);
    r.read(8);
    const unsigned space = r.read(2), tier = r.read(1), profile = r.read(5);
    uint32_t compatibility = 0;
    for (int i = 0; i < 32; ++i) {
        compatibility = compatibility << 1 | r.read(1);
    }
    uint8_t constraints[6];
    for (auto& byte : constraints) {
        byte = uint8_t(r.read(8));
    }
    const unsigned level = r.read(8);
    const auto sampling = hevcSampling(sets[1][0]);
    DecoderSetup setup;
    uint32_t reversed = 0;
    for (int i = 0; i < 32; ++i) {
        reversed |= ((compatibility >> i) & 1u) << (31 - i);
    }
    char text[32];
    std::snprintf(text, sizeof(text), "hvc1.%s%u.%X.%c%u", space ? std::string(1, char('A' + space - 1)).c_str() : "",
                  profile, reversed, tier ? 'H' : 'L', level);
    setup.codec = text;
    int last = 5;
    while (last >= 0 && !constraints[last]) {
        --last;
    }
    for (int i = 0; i <= last; ++i) {
        std::snprintf(text, sizeof(text), ".%X", constraints[i]);
        setup.codec += text;
    }
    auto& d = setup.description;
    d = {1, uint8_t(space << 6 | tier << 5 | profile)};
    for (int shift = 24; shift >= 0; shift -= 8) {
        d.push_back(uint8_t(compatibility >> shift));
    }
    d.insert(d.end(), std::begin(constraints), std::end(constraints));
    d.insert(d.end(), {uint8_t(level), 0xF0, 0x00, 0xFC, uint8_t(0xFC | sampling.chroma),
                       uint8_t(0xF8 | (sampling.bitDepth - 8)), uint8_t(0xF8 | (sampling.bitDepth - 8)), 0, 0,
                       0x0F /* one temporal layer, nested; 4-byte lengths */, 3});
    for (unsigned kind = 0; kind < 3; ++kind) {
        d.push_back(uint8_t(0x80 | (nal::hevc::vps + kind)));
        d.push_back(uint8_t(sets[kind].size() >> 8));
        d.push_back(uint8_t(sets[kind].size()));
        for (const auto nal : sets[kind]) {
            d.push_back(uint8_t(nal.size() >> 8));
            d.push_back(uint8_t(nal.size()));
            d.insert(d.end(), nal.begin(), nal.end());
        }
    }
    return setup;
}

// AV1: the codec string (AV1 Codec ISO Media File Format Binding, "av01")
// and the AV1 codec configuration record, from the sequence header OBU.
inline std::optional<DecoderSetup> av1Setup(std::span<const uint8_t> config) {
    if (config.empty()) {
        return std::nullopt;
    }
    const auto info = av1::sequenceInfo(config);
    const unsigned depth = info.twelveBit ? 12 : info.highBitDepth ? 10 : 8;
    char text[32];
    std::snprintf(text, sizeof(text), "av01.%u.%02u%c.%02u", info.profile, info.level, info.tier ? 'H' : 'M', depth);
    return DecoderSetup{text, av1::codecConfiguration(config)};
}

// VP8 and VP9: the codec string (VP Codec ISO Media File Format Binding:
// "vp8", or "vp09." and the record's fields, the long form for every
// stream), from the 'vpcC' record; WebCodecs takes no description.
inline std::optional<DecoderSetup> vpSetup(rcs_codec codec, std::span<const uint8_t> config) {
    if (config.empty()) {
        return std::nullopt;
    }
    const vpx::Config c = vpx::readConfig(config);
    if (codec == RCS_CODEC_VP8) {
        return DecoderSetup{"vp8", {}};
    }
    char text[40];
    std::snprintf(text, sizeof(text), "vp09.%02u.%02u.%02u.%02u.%02u.%02u.%02u.%02u", c.profile, c.level, c.bitDepth,
                  c.chroma, unsigned(c.color.primaries), unsigned(c.color.transfer), unsigned(c.color.matrix),
                  c.fullRange ? 1u : 0u);
    return DecoderSetup{text, {}};
}

inline std::optional<DecoderSetup> decoderSetup(rcs_codec codec, std::span<const uint8_t> config) {
    switch (codec) {
    case RCS_CODEC_H264:
        return h264Setup(config);
    case RCS_CODEC_HEVC:
        return hevcSetup(config);
    case RCS_CODEC_VP8:
    case RCS_CODEC_VP9:
        return vpSetup(codec, config);
    case RCS_CODEC_AV1:
        return av1Setup(config);
    default:
        return std::nullopt;
    }
}

} // namespace rcsdk::web
