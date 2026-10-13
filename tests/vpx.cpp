// VP8 and VP9 bitstream helpers (media/vpx.hpp) on frames the libvpx built
// into the SDK encodes here: each VP9 profile's keyframe header, other
// frames, superframe indexes, the 'vpcC' record and its level, and what
// videoProblem says of packets whose flag, size, record or bytes are wrong.
// Damaged input throws Failure(CODEC) and reads nothing beyond its end.
#include "codecs/codecs.hpp"
#include "media/encoder_output.hpp"
#include "media/sampling.hpp"
#include "media/vpx.hpp"
#include "protocol/messages.hpp"
#include "support/check.hpp"

#include <cstring>

using namespace rcsdk;

namespace {

VideoFrame grey(uint32_t width, uint32_t height, uint64_t sequence) {
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * 4, uint8_t(40 + sequence * 9));
    frame.sequence = sequence;
    return frame;
}

// Two packets of `codec` in `format`: a keyframe and the picture after it.
std::vector<EncodedPacket> encode(rcs_codec codec, rcs_picture_format format, uint32_t width, uint32_t height) {
    platform::EncoderConfig config;
    config.codec = codec;
    config.width = width;
    config.height = height;
    config.framesPerSecond = 30;
    config.bitrate = 2'000'000;
    config.acceleration = RCS_ACCELERATION_SOFTWARE;
    config.backend = RCS_BACKEND_LIBVPX;
    config.picture = picture(format, RCS_MATRIX_BT709, RCS_RANGE_LIMITED);
    const auto implementations = codecs::implementations();
    auto encoder = codecs::createEncoder(config, implementations);
    encoder->open(config);
    std::vector<EncodedPacket> packets;
    for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
        encoder->submit(grey(width, height, sequence));
        EncodedPacket packet;
        CHECK(encoder->receive(packet, std::chrono::milliseconds(0)));
        packets.push_back(std::move(packet));
    }
    return packets;
}

bool codecFailure(const std::function<void()>& work) {
    try {
        work();
    } catch (const Failure& failure) {
        return failure.result() == RCS_ERROR_CODEC;
    }
    return false;
}

} // namespace

int main() {
    return runTest("vpx", [] {
        // The level: luma samples per picture and per second.
        CHECK(vpx::vp9Level(1280, 720, 30) == 31 && vpx::vp9Level(1920, 1080, 60) == 41);
        CHECK(vpx::vp9Level(3840, 2160, 60) == 51 && vpx::vp9Level(3840, 2160, 120) == 52);
        CHECK(vpx::vp9Level(7680, 4320, 60) == 61);

        // The record: written and read back; anything but version 1 of 12 bytes refused.
        const vpx::Config config =
            vpx::streamConfig(RCS_CODEC_VP9, picture(RCS_PICTURE_HDR10_444, RCS_MATRIX_BT709, RCS_RANGE_FULL), 3840,
                              2160, 60);
        CHECK(config.profile == 3 && config.bitDepth == 10 && config.chroma == vpx::subsampling444 && config.level == 51);
        CHECK((config.color == Cicp{cicpBt2020, cicpPq, cicpBt2020}) && config.fullRange);
        auto record = vpx::writeConfig(config);
        CHECK(record.size() == vpx::configSize && vpx::readConfig(record) == config);
        for (const size_t at : {size_t(0), size_t(10)}) {
            auto broken = record;
            broken[at] ^= 1;
            CHECK(codecFailure([&] { vpx::readConfig(broken); }));
        }
        CHECK(codecFailure([&] { vpx::readConfig(std::vector<uint8_t>(record.begin(), record.end() - 1)); }));

        // Each VP9 profile: the keyframe says its profile, depth and sampling; the next frame is no keyframe.
        const struct {
            rcs_picture_format format;
            unsigned profile, bits;
            bool full;
        } profiles[]{{RCS_PICTURE_420_8, 0, 8, false},
                     {RCS_PICTURE_444_8, 1, 8, true},
                     {RCS_PICTURE_420_10, 2, 10, false},
                     {RCS_PICTURE_444_10, 3, 10, true}};
        for (const auto& p : profiles) {
            const auto packets = encode(RCS_CODEC_VP9, p.format, 320, 180);
            const auto key = vpx::vp9Header(packets[0].data);
            CHECK(key.keyframe && key.profile == p.profile && key.bitDepth == p.bits && key.fullSubsampling == p.full);
            CHECK(key.width == 320 && key.height == 180 && key.refreshFrameFlags == 0xff);
            CHECK((vpSampling(key) == samplingOf(p.format)));
            CHECK(!vpx::vp9Header(packets[1].data).keyframe && vpx::vp9Header(packets[1].data).profile == p.profile);
            for (const auto& packet : packets) {
                CHECK(protocol::videoProblem(packet).empty());
            }
        }

        // VP8: the frame tag, a keyframe's start code and size.
        const auto vp8 = encode(RCS_CODEC_VP8, RCS_PICTURE_420_8, 176, 144);
        const auto vp8Key = vpx::vp8Header(vp8[0].data);
        CHECK(vp8Key.keyframe && vp8Key.width == 176 && vp8Key.height == 144 && !vpx::vp8Header(vp8[1].data).keyframe);
        CHECK(protocol::videoProblem(vp8[0]).empty() && protocol::videoProblem(vp8[1]).empty());

        // Superframes: two frames and an index of 2-byte sizes; a size that
        // does not fit, or bytes left out, is no superframe one can read.
        const auto vp9 = encode(RCS_CODEC_VP9, RCS_PICTURE_420_8, 320, 180);
        std::vector<uint8_t> super = vp9[0].data;
        super.insert(super.end(), vp9[1].data.begin(), vp9[1].data.end());
        const uint8_t marker = 0xc0 | (1 << 3) | 1; // 2-byte sizes, 2 frames.
        super.push_back(marker);
        for (const size_t size : {vp9[0].data.size(), vp9[1].data.size()}) {
            super.push_back(uint8_t(size));
            super.push_back(uint8_t(size >> 8));
        }
        super.push_back(marker);
        const auto frames = vpx::vp9Frames(super);
        CHECK(frames.size() == 2 && frames[0].size() == vp9[0].data.size() && frames[1].size() == vp9[1].data.size());
        auto packet = vp9[0];
        packet.data = super;
        CHECK(protocol::videoProblem(packet).empty()); // A superframe whose first frame is the keyframe.
        auto wrongSize = super;
        wrongSize[super.size() - 5] ^= 1; // The first frame's size.
        CHECK(codecFailure([&] { vpx::vp9Frames(wrongSize); }));
        auto marked = std::vector<uint8_t>(vp9[1].data);
        marked.push_back(0xc0); // Looks like a marker; the index's first byte is not.
        CHECK(vpx::vp9Frames(marked).size() == 1);

        // A system encoder's VP9 frames as EncodedPacket carries them: as
        // they are, the keyframe flag from the frame, the SDK's 'vpcC'.
        const Picture sdr = picture(RCS_PICTURE_420_8, RCS_MATRIX_BT709, RCS_RANGE_LIMITED);
        EncoderOutput output(RCS_CODEC_VP9, sdr, 320, 180, 60);
        output.configuration(vp9[0].config); // A codec-config buffer is not VP's: nothing of it is taken.
        for (const auto& frame : vp9) {
            EncodedPacket split;
            output.split(frame.data, split);
            CHECK(split.data == frame.data && split.keyframe == frame.keyframe);
            CHECK(split.config == vpx::writeConfig(vpx::streamConfig(RCS_CODEC_VP9, sdr, 320, 180, 60)));
        }

        // What videoProblem says of a packet that is not what it claims.
        auto flag = vp9[1];
        flag.keyframe = true;
        CHECK(!protocol::videoProblem(flag).empty());
        auto size = vp9[0];
        size.width = 640;
        CHECK(!protocol::videoProblem(size).empty());
        auto depth = vp9[0];
        depth.picture = picture(RCS_PICTURE_420_10, RCS_MATRIX_BT709, RCS_RANGE_LIMITED);
        CHECK(!protocol::videoProblem(depth).empty()); // The record is 8-bit.
        auto noRecord = vp9[0];
        noRecord.config.clear();
        CHECK(!protocol::videoProblem(noRecord).empty());
        // Cut short within the keyframe's header (VP9: its fixed fields fit
        // in 9 bytes; VP8: 10): a reason, never a read beyond the bytes.
        for (size_t length = 0; length < 10; ++length) {
            if (length < 9) {
                auto cut = vp9[0];
                cut.data.resize(length);
                CHECK(!protocol::videoProblem(cut).empty());
            }
            auto cut8 = vp8[0];
            cut8.data.resize(length);
            CHECK(!protocol::videoProblem(cut8).empty());
        }
    });
}
