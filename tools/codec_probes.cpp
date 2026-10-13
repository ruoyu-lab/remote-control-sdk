// Prints the VP9 and AV1 keyframes of src/platform/decode_probes.hpp: mid-grey
// pictures in each format whose decoding the systems do not report, made by
// the libraries built into the SDK (libvpx, libaom), with the packets'
// configuration as EncodedPacket carries it ('vpcC'; the sequence header).
// Not part of the SDK.
//
//   rcsdk_codec_probes
#include "codecs/codecs.hpp"
#include "platform/decode_probes.hpp"

#include <cstdio>
#include <string>

namespace {

void printArray(const std::string& name, const std::vector<uint8_t>& bytes) {
    std::printf("inline constexpr uint8_t %s[] = {", name.c_str());
    for (size_t i = 0; i < bytes.size(); ++i) {
        std::printf("%s0x%02x", i ? (i % 16 ? ", " : ",\n    ") : "\n    ", bytes[i]);
    }
    std::printf("};\n");
}

} // namespace

int main() {
    using namespace rcsdk;
    const struct {
        rcs_codec codec;
        rcs_backend backend;
        rcs_picture_format format;
        const char* name;
        const char* profile;
    } probes[]{{RCS_CODEC_VP9, RCS_BACKEND_LIBVPX, RCS_PICTURE_420_10, "vp9_420_10", "profile 2"},
               {RCS_CODEC_VP9, RCS_BACKEND_LIBVPX, RCS_PICTURE_444_8, "vp9_444_8", "profile 1"},
               {RCS_CODEC_VP9, RCS_BACKEND_LIBVPX, RCS_PICTURE_444_10, "vp9_444_10", "profile 3"},
               {RCS_CODEC_AV1, RCS_BACKEND_LIBAOM, RCS_PICTURE_420_10, "av1_420_10", "Main"},
               {RCS_CODEC_AV1, RCS_BACKEND_LIBAOM, RCS_PICTURE_444_8, "av1_444_8", "High"},
               {RCS_CODEC_AV1, RCS_BACKEND_LIBAOM, RCS_PICTURE_444_10, "av1_444_10", "High"}};
    try {
        const auto implementations = codecs::implementations();
        for (const auto& p : probes) {
            platform::EncoderConfig config;
            config.codec = p.codec;
            config.width = platform::probes::width;
            config.height = platform::probes::height;
            config.framesPerSecond = 30;
            config.bitrate = 1'000'000;
            config.acceleration = RCS_ACCELERATION_SOFTWARE;
            config.backend = p.backend;
            config.picture = picture(p.format, RCS_MATRIX_BT709, RCS_RANGE_LIMITED);
            auto encoder = codecs::createEncoder(config, implementations);
            encoder->open(config);
            VideoFrame frame;
            frame.width = config.width;
            frame.height = config.height;
            frame.stride = frame.width * 4;
            frame.pixels = std::make_shared<std::vector<uint8_t>>(size_t(frame.stride) * frame.height, uint8_t(128));
            frame.sequence = 1;
            encoder->submit(frame);
            EncodedPacket packet;
            if (!encoder->receive(packet, std::chrono::milliseconds(0))) {
                std::fprintf(stderr, "%s: no packet\n", p.name);
                return 1;
            }
            std::printf("// %s %s (%s, %s)\n", codecName(p.codec), pictureFormatName(p.format), p.profile,
                        encoder->name().c_str());
            printArray(std::string(p.name) + "_config", packet.config);
            printArray(std::string(p.name) + "_data", packet.data);
        }
    } catch (const Failure& failure) {
        std::fprintf(stderr, "%s\n", failure.what());
        return 1;
    }
    return 0;
}
