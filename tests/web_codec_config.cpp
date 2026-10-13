// What a browser's VideoDecoder is configured with (media/web_codec_config.hpp),
// from the decode probes' configurations: codec strings and configuration records.
#include "media/web_codec_config.hpp"
#include "platform/decode_probes.hpp"
#include "support/check.hpp"

using namespace rcsdk;
namespace probes = rcsdk::platform::probes;

int main() {
    return runTest("web_codec_config", [] {
        // H.264 High 4:4:4 Predictive (profile 244): avcC with 4-byte lengths, one SPS, one PPS.
        const auto h264 = web::h264Setup(probes::h264_444_8_config);
        CHECK(h264 && h264->codec.rfind("avc1.f4", 0) == 0 && h264->codec.size() == 11);
        CHECK(h264->description[0] == 1 && h264->description[1] == 0xf4 && h264->description[4] == 0xff &&
              h264->description[5] == 0xe1);
        // HEVC Main 10 and RExt 4:4:4: the profile in the string, chroma and depth in hvcC.
        const auto main10 = web::hevcSetup(probes::hevc_420_10_config);
        CHECK(main10 && main10->codec.rfind("hvc1.2.4.L", 0) == 0);
        CHECK(main10->description[0] == 1 && (main10->description[16] & 3) == 1 && (main10->description[17] & 7) == 2);
        const auto rext = web::hevcSetup(probes::hevc_444_10_config);
        CHECK(rext && rext->codec.rfind("hvc1.4.10.L", 0) == 0 && (rext->description[16] & 3) == 3);
        CHECK(rext->description[22] == 3 && (rext->description[23] & 0x3f) == 32); // VPS, SPS, PPS arrays.
        // VP9: the long codec string from 'vpcC' (profile, level, depth, sampling, colour, range); no description.
        const auto vp9 = web::decoderSetup(RCS_CODEC_VP9, probes::vp9_444_10_config);
        CHECK(vp9 && vp9->codec.rfind("vp09.03.", 0) == 0 && vp9->codec.size() == 28 && vp9->description.empty());
        CHECK(vp9->codec.substr(10) == ".10.03.01.01.01.00"); // 10 bits, 4:4:4, BT.709, limited range.
        const auto vp9Main = web::decoderSetup(RCS_CODEC_VP9, probes::vp9_420_10_config);
        CHECK(vp9Main && vp9Main->codec.rfind("vp09.02.", 0) == 0 && vp9Main->codec.substr(10, 6) == ".10.01");
        CHECK(web::decoderSetup(RCS_CODEC_VP8, probes::vp9_420_10_config)->codec == "vp8");
        // AV1 High: profile 1 in the string, 4:4:4 (no subsampling) in av1C.
        const auto high = web::av1Setup(probes::av1_444_8_config);
        CHECK(high && high->codec.rfind("av01.1.", 0) == 0 && high->codec.substr(high->codec.size() - 3) == ".08");
        CHECK(high->description[0] == 0x81 && (high->description[1] >> 5) == 1 && (high->description[2] & 0x0c) == 0);
        // Incomplete configurations are refused, not guessed.
        CHECK(!web::h264Setup({}) && !web::hevcSetup(probes::h264_444_8_config) && !web::av1Setup({}) &&
              !web::vpSetup(RCS_CODEC_VP9, {}));
    });
}
