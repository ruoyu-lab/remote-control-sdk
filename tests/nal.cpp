// NAL unit containers and the header fields the pipeline relies on.
#include "media/encoder_output.hpp"
#include "media/nal.hpp"
#include "support/check.hpp"

using namespace rcsdk;
using Bytes = std::vector<uint8_t>;

static void containers() {
    const Bytes a{0x67, 1, 2}, b{0x68, 3}, c{0x65, 0, 0, 4};
    Bytes prefixed;
    for (const auto& unit : {a, b, c}) {
        nal::appendUnit(prefixed, unit);
    }
    const auto units = nal::units(prefixed);
    CHECK(units.size() == 3 && Bytes(units[2].begin(), units[2].end()) == c);
    CHECK(throws<Failure>([&] { nal::units(std::span(prefixed).first(prefixed.size() - 1)); }));
    CHECK(throws<Failure>([&] { nal::units(Bytes{0, 0, 0, 0}); }));

    Bytes annexB;
    for (const auto& unit : units) {
        nal::appendAnnexB(annexB, unit);
    }
    annexB.insert(annexB.begin() + 4 + a.size(), 0); // A trailing zero before the next start code.
    const auto split = nal::annexBUnits(annexB);
    CHECK(split.size() == 3 && Bytes(split[0].begin(), split[0].end()) == a);
    CHECK(Bytes(split[2].begin(), split[2].end()) == c);
}

// Each packet carries the whole current set of parameter sets, also from an
// encoder that repeats some of them with ordinary pictures (Intel's H.264
// encoder writes its PPS with every one, the SPS with keyframes only).
static void parameterSetsPerPacket() {
    // HEVC: VPS 0x40, SPS 0x42, PPS 0x44, IDR_W_RADL 0x26, TRAIL_R 0x02.
    const Bytes vps{0x40, 1, 7}, sps{0x42, 1, 8}, pps{0x44, 1, 9}, newPps{0x44, 1, 10}, idr{0x26, 1, 5},
        trail{0x02, 1, 6};
    const auto stream = [](std::initializer_list<Bytes> units) {
        Bytes out;
        for (const auto& unit : units) {
            nal::appendAnnexB(out, unit);
        }
        return out;
    };
    const auto configOf = [](const EncodedPacket& p) {
        std::vector<Bytes> units;
        for (const auto unit : nal::units(p.config)) {
            units.emplace_back(unit.begin(), unit.end());
        }
        return units;
    };
    const Picture picture;
    EncoderOutput output(RCS_CODEC_HEVC, picture, 1920, 1080, 60);
    EncodedPacket key, withPps, plain;
    output.split(stream({vps, sps, pps, idr}), key);
    CHECK(key.keyframe && configOf(key) == (std::vector<Bytes>{vps, sps, pps}));
    output.split(stream({newPps, trail}), withPps);
    CHECK(!withPps.keyframe && configOf(withPps) == (std::vector<Bytes>{vps, sps, newPps}));
    output.split(stream({trail}), plain);
    CHECK(plain.config == withPps.config && Bytes(plain.data.begin() + 4, plain.data.end()) == trail);

    // Parameter sets given apart from the pictures (MediaCodec's codec-config
    // buffer) go with the pictures that follow.
    EncoderOutput separate(RCS_CODEC_HEVC, picture, 1920, 1080, 60);
    separate.configuration(stream({vps, sps, pps}));
    EncodedPacket first;
    separate.split(stream({idr}), first);
    CHECK(first.keyframe && configOf(first) == (std::vector<Bytes>{vps, sps, pps}));
}

static void headers() {
    // H.264: SPS 0x67, PPS 0x68, IDR slice 0x65, non-IDR slice 0x41, SEI 0x06.
    CHECK(nal::parameterSet(RCS_CODEC_H264, Bytes{0x67}) && nal::parameterSet(RCS_CODEC_H264, Bytes{0x68}));
    CHECK(!nal::parameterSet(RCS_CODEC_H264, Bytes{0x65}) && !nal::parameterSet(RCS_CODEC_H264, Bytes{0x06}));
    CHECK(nal::randomAccess(RCS_CODEC_H264, Bytes{0x65}) && !nal::randomAccess(RCS_CODEC_H264, Bytes{0x41}));

    // HEVC two-byte headers: VPS 0x40 01, SPS 0x42 01, PPS 0x44 01, IDR_W_RADL
    // 0x26 01, CRA 0x2a 01, TRAIL_R 0x02 01, prefix SEI 0x4e 01.
    for (const uint8_t type : {0x40, 0x42, 0x44}) {
        CHECK(nal::parameterSet(RCS_CODEC_HEVC, Bytes{type, 0x01}));
    }
    CHECK(!nal::parameterSet(RCS_CODEC_HEVC, Bytes{0x4e, 0x01}));
    CHECK(nal::randomAccess(RCS_CODEC_HEVC, Bytes{0x26, 0x01}) && nal::randomAccess(RCS_CODEC_HEVC, Bytes{0x2a, 0x01}));
    CHECK(!nal::randomAccess(RCS_CODEC_HEVC, Bytes{0x02, 0x01}) &&
          !nal::randomAccess(RCS_CODEC_HEVC, Bytes{0x40, 0x01}));
    // The same first byte means different things per codec.
    CHECK(!nal::parameterSet(RCS_CODEC_HEVC, Bytes{0x67, 0x01}) && !nal::randomAccess(RCS_CODEC_H264, Bytes{0x26}));
    CHECK(!nal::parameterSet(RCS_CODEC_H264, Bytes{}) && !nal::randomAccess(RCS_CODEC_HEVC, Bytes{}));
    // AV1 has OBUs, not NAL units: asking is a mistake, never an H.264 answer.
    CHECK(throws<Failure>([] { nal::parameterSet(RCS_CODEC_AV1, Bytes{0x67}); }));
    CHECK(throws<Failure>([] { nal::randomAccess(rcs_codec(9), Bytes{0x65}); }));
}

int main() {
    return runTest("nal", [] {
        containers();
        headers();
        parameterSetsPerPacket();
    });
}
