// For every codec this system can encode and decode with the same acceleration
// (the system's codecs and the libraries built into the SDK), the encoder's
// output decodes without reordering delay, using exactly that acceleration;
// its temporal layers each decode, the lower ones alone too. On Windows NVENC
// passes them through its older interface (12.0) too, and the GPU decoders
// the SDK drives directly (NVDEC, oneVPL's) decode every encoder's streams of
// their picture formats.
#include "codecs/codecs.hpp"
#include "media/av1.hpp"
#include "media/layers.hpp"
#include "media/nal.hpp"
#include "media/vpx.hpp"
#include "media/picture_size.hpp"
#include "media/sampling.hpp"
#include "platform/platform.hpp"
#include "protocol/messages.hpp"
#include "session/negotiation.hpp"
#include "session/session.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <utility>

#if defined(__APPLE__) || defined(__ANDROID__)
#include "platform/decode_probes.hpp"
#endif
#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#elif defined(_WIN32)
#include "platform/windows/adapters.hpp"
#include "platform/windows/media_foundation.hpp"
#include "platform/windows/nvenc_api.hpp"
#include "platform/windows/yuv_shader.hpp"
#include "platform/windows/d3d.hpp"

#include <cmath>
#endif

using namespace rcsdk;

#ifdef _WIN32
// NVENC's encoders through its interface 12.0 instead of the newest the
// driver speaks (nvencInterface12).
static bool nvencOlder = false;
#endif

// The system's codecs and the libraries', as the SDK lists them.
static const std::vector<platform::CodecImplementation>& implementations() {
    static const auto list = codecs::implementations();
    return list;
}
static std::unique_ptr<platform::VideoEncoder> encoderFor(const platform::EncoderConfig& config) {
#ifdef _WIN32
    if (nvencOlder && config.backend == RCS_BACKEND_NVENC) {
        return platform::win::nvenc12::api().create();
    }
#endif
    return codecs::createEncoder(config, implementations());
}
static std::unique_ptr<platform::VideoDecoder> decoderFor(const platform::DecoderConfig& config) {
    auto decoder = codecs::createDecoder(config, implementations());
    decoder->open(config);
    return decoder;
}
static platform::DecoderConfig decoding(rcs_codec codec, rcs_acceleration acceleration, bool cpuImages = false,
                                        const rcsdk::Picture& picture = {}) {
    platform::DecoderConfig config;
    config.codec = codec;
    config.acceleration = acceleration;
    config.cpuImages = cpuImages;
    config.picture = picture;
    return config;
}
static std::shared_ptr<void> deviceOn(uint64_t luid);
// A device on the GPU of the first encoder of this codec, acceleration and
// backend (Windows: Intel's VP9 encoder is on its GPU only), or none.
static std::shared_ptr<void> encoderDevice(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend) {
    for (const auto& i : implementations()) {
        if (i.codec == codec && i.direction == RCS_CODEC_ENCODER && i.acceleration == acceleration &&
            i.backend == backend) {
            return deviceOn(i.adapterLuid);
        }
    }
    return nullptr;
}
static const char* accelerationName(rcs_acceleration acceleration) {
    return acceleration == RCS_ACCELERATION_HARDWARE ? "hardware" : "software";
}
// A decoded picture as BGRA, from its CPU pixels or its surface.
static void withImage(const VideoFrame& frame, const std::function<void(const rcs_frame&)>& use) {
    if (frame.pixels) {
        use({sizeof(rcs_frame), frame.stream, frame.width, frame.height, frame.stride, RCS_PIXEL_BGRA8, 0, 0,
             frame.pixels->data(), frame.pixels->size()});
        return;
    }
    platform::withCpuImage(frame, use);
}

// A gradient, or (noise) a gradient with fine random grain, where the bitrate decides quality.
static VideoFrame testFrame(uint32_t width, uint32_t height, uint64_t sequence, bool noise = false) {
    auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * 4);
    uint32_t state = uint32_t(sequence * 2654435761u) | 1;
    for (size_t i = 0; i < pixels->size(); i += 4) {
        if (noise) { // Fine grain over a gradient: compressible, at a cost in quality.
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            (*pixels)[i] = uint8_t(i / 4 % width + (state & 31));
            (*pixels)[i + 1] = uint8_t(i / 4 / width + (state >> 8 & 31));
            (*pixels)[i + 2] = uint8_t(128 + (state >> 16 & 31));
        } else {
            (*pixels)[i] = uint8_t(i / 4 + sequence * 9);
            (*pixels)[i + 1] = uint8_t(sequence * 31);
            (*pixels)[i + 2] = 128;
        }
        (*pixels)[i + 3] = 255;
    }
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4;
    frame.pixels = std::move(pixels);
    frame.sequence = sequence;
    frame.timestampNs = monotonicNs();
    return frame;
}

// AV1: config is exactly one parsable sequence header; data holds neither
// sequence headers nor temporal delimiters.
static void checkObus(const EncodedPacket& packet, bool print) {
    const auto config = av1::obus(packet.config);
    CHECK(config.size() == 1 && config[0].type == av1::obuSequenceHeader);
    const auto info = av1::sequenceInfo(config[0].bytes);
    CHECK(info.profile == 0 && !info.highBitDepth && info.subsamplingX == 1 && info.subsamplingY == 1);
    CHECK(av1::codecConfiguration(config[0].bytes).size() == 4 + packet.config.size());
    for (const auto& obu : av1::obus(packet.data)) {
        CHECK(obu.type != av1::obuSequenceHeader && obu.type != av1::obuTemporalDelimiter);
    }
    if (print) { // A real sequence header, for the av1 unit test.
        std::printf("AV1 sequence header (level %u):", info.level);
        for (const uint8_t byte : packet.config) {
            std::printf(" %02x", byte);
        }
        std::printf("\n");
    }
}

static void roundTrip(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend,
                      const rcs_capabilities& caps) {
    constexpr uint32_t width = 640, height = 360;
    const platform::EncoderConfig config{codec, width, height, 30, 2'000'000, acceleration, backend,
                                         encoderDevice(codec, acceleration, backend)};
    auto encoder = encoderFor(config);
    encoder->open(config);
    CHECK(encoder->backend() == backend);
    auto decoder = decoderFor(decoding(codec, acceleration));
    std::atomic<bool> cancelled{false};
    // H.264: SPS, PPS. HEVC: VPS, SPS, PPS.
    const size_t parameterSets = codec == RCS_CODEC_HEVC ? 3 : 2;

    int decoded = 0, keyframes = 0;
    for (uint64_t sequence = 1; sequence <= 30; ++sequence) {
        if (sequence == 20) {
            encoder->requestKeyframe();
        }
        encoder->submit(testFrame(width, height, sequence));
        EncodedPacket packet;
        while (encoder->receive(packet, 1s)) {
            CHECK(packet.codec == codec && packet.width == width && packet.height == height);
            if (codec == RCS_CODEC_AV1) {
                checkObus(packet, sequence == 1);
            } else if (vpCodec(codec)) { // One frame; config the stream's record; the keyframe flag the header's.
                CHECK(packet.config == vpx::writeConfig(vpx::streamConfig(codec, packet.picture, width, height, 30)));
                CHECK(vpx::packetHeader(codec, packet.data).keyframe == packet.keyframe);
                CHECK(protocol::videoProblem(packet).empty());
            } else {
                const auto config = nal::units(packet.config);
                CHECK(config.size() >= parameterSets);
                for (const auto unit : config) {
                    CHECK(nal::parameterSet(codec, unit));
                }
                bool randomAccess = false;
                for (const auto unit : nal::units(packet.data)) {
                    CHECK(!nal::parameterSet(codec, unit)); // Parameter sets travel only in config.
                    randomAccess |= nal::randomAccess(codec, unit);
                }
                CHECK(randomAccess == packet.keyframe);
            }
            keyframes += packet.keyframe;
            VideoFrame frame;
            if (decoder->decode(packet, frame, cancelled)) {
                CHECK(frame.width == width && frame.height == height);
                // Hardware decoding, and software decoding where it says so, yields native surfaces.
                const bool surfaces = acceleration == RCS_ACCELERATION_HARDWARE || caps.software_decoding_gpu_frames;
                CHECK(surfaces ? bool(frame.surface) : bool(frame.pixels));
                ++decoded;
            }
            if (encoder->inFlight() == 0) {
                break;
            }
        }
    }
    CHECK(keyframes >= 2); // The first picture and the requested one.
    CHECK(decoded >= 28); // Low-latency mode: at most a frame or two of decoder delay.
    CHECK(!encoder->name().empty() && !decoder->name().empty());
    std::printf("%s %s: low-latency encoder=%d decoded=%d keyframes=%d (%s -> %s)\n", codecName(codec),
                accelerationName(acceleration), encoder->lowLatency(), decoded,
                keyframes, encoder->name().c_str(), decoder->name().c_str());
}

// A lower bitrate while running, on the implementation's GPU (`device`):
// after 30 plain pictures to settle in, of 60 with fine grain the 30 after a
// change from 2 to 0.2 Mbit/s come to less than 60% of the 30 before, and the
// encoder says it took it. (Grain from the first picture on keeps Microsoft's
// software H.264 encoder at its coarsest from its first keyframes on, both
// before the change and after.)
static void bitrateChange(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend,
                          std::shared_ptr<void> device) {
    constexpr uint32_t width = 640, height = 360;
    const platform::EncoderConfig config{codec, width, height, 30, 2'000'000, acceleration, backend, device};
    auto encoder = encoderFor(config);
    encoder->open(config);
    size_t before = 0, after = 0;
    for (uint64_t sequence = 1; sequence <= 90; ++sequence) {
        if (sequence == 61) {
            encoder->setBitrate(200'000);
        }
        auto frame = testFrame(width, height, sequence, sequence > 30);
        frame.timestampNs = sequence * 33'333'333; // 30 FPS: rate control budgets by time.
        encoder->submit(frame);
        for (EncodedPacket packet; encoder->receive(packet, 1s);) {
            if (!packet.keyframe && sequence > 30) {
                (sequence <= 60 ? before : after) += packet.data.size();
            }
            if (encoder->inFlight() == 0) {
                break;
            }
        }
    }
    std::printf("%s %s: bitrate change: %zu bytes before, %zu after (%s)\n", codecName(codec),
                accelerationName(acceleration), before, after, encoder->name().c_str());
    CHECK(after * 10 < before * 6);
    CHECK(encoder->bitrate() == 200'000); // The encoder took it.
}

// Without a request the encoder makes no keyframe after the first
// (rcs_encoder_config.keyframe_interval_ms 0): five seconds at 60 FPS.
static void keyframesWhenAsked(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend) {
    constexpr uint32_t width = 640, height = 360, fps = 60;
    const platform::EncoderConfig config{codec, width, height, fps, 2'000'000, acceleration, backend,
                                         encoderDevice(codec, acceleration, backend)};
    auto encoder = encoderFor(config);
    encoder->open(config);
    std::vector<int> keyframes; // Which pictures, counted from 1.
    int pictures = 0;
    const auto take = [&](EncodedPacket& packet) {
        ++pictures;
        if (packet.keyframe) {
            keyframes.push_back(pictures);
        }
    };
    for (uint64_t sequence = 1; sequence <= 300; ++sequence) {
        auto frame = testFrame(width, height, sequence);
        frame.timestampNs = sequence * 1'000'000'000 / fps;
        encoder->submit(frame);
        for (EncodedPacket packet; encoder->receive(packet, 0ms);) {
            take(packet);
        }
    }
    for (EncodedPacket packet; encoder->inFlight() && encoder->receive(packet, 1s);) {
        take(packet);
    }
    std::printf("%s %s: %d pictures, keyframes at", codecName(codec), accelerationName(acceleration), pictures);
    for (const int picture : keyframes) {
        std::printf(" %d", picture);
    }
    std::printf(" (%s)\n", encoder->name().c_str());
    CHECK(pictures >= 295);
    CHECK(keyframes == std::vector<int>{1});
}

// An encoder that drops pictures from its references (refInvalidation, as
// listed, `listed`; Media Foundation's say it once opened): two pictures
// "lost" on the way are left out; the next packet refers to older ones only,
// is no keyframe, and says after which (recoversAfter); a decoder that never
// saw the two decodes it and what follows, and shows what went in (BGRA, the
// mean difference per channel within a few levels). Pictures of `picture`'s
// format, decoded by `decoding` (as CPU images; by default the decoder AUTO
// picks for them). One that cannot go on from the recovery (`refused`) says
// so there: its decode fails with CODEC, it shows no picture decoded wrong.
static void referenceInvalidation(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend,
                                  std::shared_ptr<void> device, bool listed, const rcsdk::Picture& picture = {},
                                  std::optional<platform::DecoderConfig> decoding = std::nullopt,
                                  bool refused = false) {
    constexpr uint32_t width = 640, height = 360;
    const platform::EncoderConfig config{codec, width, height, 30, 8'000'000, acceleration, backend, device, picture};
    auto encoder = encoderFor(config);
    encoder->open(config);
    CHECK(encoder->refInvalidation() == listed || (!listed && backend == RCS_BACKEND_MEDIA_FOUNDATION));
    if (!encoder->refInvalidation()) {
        return;
    }
    auto settings = decoding.value_or(::decoding(codec, acceleration, true, picture));
    settings.cpuImages = true;
    auto decoder = decoderFor(settings);
    std::atomic<bool> cancelled{false};
    double worst = 0, before = 0; // After the loss, and before it (what coding alone costs).
    int decoded = 0;
    std::string differences;
    for (uint64_t sequence = 1; sequence <= 40; ++sequence) {
        if (sequence == 23) {
            encoder->invalidateAfter(20); // 21 and 22 never reached the viewer.
        }
        auto source = testFrame(width, height, sequence);
        source.timestampNs = sequence * 33'333'333;
        encoder->submit(source);
        EncodedPacket packet;
        CHECK(encoder->receive(packet, 1s));
        CHECK(sequence == 23 ? packet.recoversAfter == 20u && !packet.keyframe : !packet.recoversAfter);
        if (sequence == 21 || sequence == 22) {
            continue;
        }
        VideoFrame frame;
        if (refused && sequence == 23) {
            try {
                decoder->decode(packet, frame, cancelled);
                CHECK(false);
            } catch (const Failure& failure) {
                std::printf("%s %s %s: reference invalidation: the decoder refuses the recovery: %s (%s)\n",
                            codecName(codec), accelerationName(acceleration), pictureFormatName(picture.format),
                            failure.what(), encoder->name().c_str());
                CHECK(failure.result() == RCS_ERROR_CODEC);
                return;
            }
        }
        if (!decoder->decode(packet, frame, cancelled)) {
            continue;
        }
        ++decoded;
        CHECK(frame.width == width && frame.height == height);
        // The decoder may hold a picture back: compare with the source of the picture it gave.
        const auto shown = testFrame(width, height, frame.sequence ? frame.sequence : sequence);
        double difference = 0;
        withImage(frame, [&](const rcs_frame& image) { // BGRA, on the CPU or a surface.
            CHECK(image.format == RCS_PIXEL_BGRA8);
            const auto* pixels = static_cast<const uint8_t*>(image.data);
            for (uint32_t y = 0; y < height; y += 7) {
                for (uint32_t x = 0; x < width; x += 7) {
                    for (uint32_t c = 0; c < 3; ++c) {
                        difference += std::abs(int(pixels[size_t(y) * image.stride_bytes + x * 4 + c]) -
                                               int((*shown.pixels)[(size_t(y) * width + x) * 4 + c]));
                    }
                }
            }
        });
        difference /= double((height + 6) / 7) * ((width + 6) / 7) * 3;
        (sequence >= 23 ? worst : before) = std::max(sequence >= 23 ? worst : before, difference);
        if (sequence >= 18 && sequence <= 28) {
            char text[32];
            std::snprintf(text, sizeof(text), " %llu:%.1f", (unsigned long long)sequence, difference);
            differences += text;
        }
    }
    std::printf("%s %s %s: reference invalidation: %d pictures decoded, at most %.1f levels off before, %.1f after "
                "(%s) (%s -> %s)\n",
                codecName(codec), accelerationName(acceleration), pictureFormatName(picture.format), decoded, before,
                worst, differences.c_str(), encoder->name().c_str(), decoder->name().c_str());
    CHECK(decoded >= 35 && worst < before + 4);
}

// Temporal layers (EncoderConfig::temporalLayers), as many as the encoder
// lists (`listed`; Media Foundation's and MediaCodec's say it once opened: two
// are asked of them): each picture's layer as the session takes it
// (media/layers.hpp), keyframes in layer 0, every layer present and the top
// one discardable where the encoder says so; a decoder of every layer, and one
// of the lower layers alone, decode what they get without an error; one that
// takes more layers from a picture of layer 0 on decodes them; a picture lost
// on the way is got over with a picture of layer 0 that refers to older ones
// where the encoder drops references (refInvalidation).
static void temporalLayers(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend,
                           std::shared_ptr<void> device, uint32_t listed) {
    const bool saysOpened = backend == RCS_BACKEND_MEDIA_FOUNDATION || backend == RCS_BACKEND_MEDIACODEC;
    for (uint32_t layers = 2; layers <= (saysOpened ? 2 : listed); ++layers) {
        constexpr uint32_t width = 640, height = 360;
        platform::EncoderConfig config{codec, width, height, 30, 4'000'000, acceleration, backend, device};
        config.temporalLayers = layers;
        auto encoder = encoderFor(config);
        encoder->open(config);
        if (saysOpened && encoder->temporalLayers() == 1) {
            std::printf("%s %s: no temporal layers (%s)\n", codecName(codec), accelerationName(acceleration),
                        encoder->name().c_str());
            return;
        }
        CHECK(encoder->temporalLayers() == layers);
        std::vector<EncodedPacket> packets;
        const auto take = [&](EncodedPacket& packet) {
            packet.picture = config.picture;
            readTemporalLayer(packet);
            CHECK(packet.temporalId < layers && (!packet.keyframe || !packet.temporalId));
            CHECK(!packet.discardable || packet.temporalId); // Layer 0's are referred to.
            packets.push_back(std::move(packet));
        };
        for (uint64_t sequence = 1; sequence <= 48; ++sequence) {
            auto frame = testFrame(width, height, sequence);
            frame.timestampNs = sequence * 33'333'333;
            encoder->submit(frame);
            for (EncodedPacket packet; encoder->receive(packet, encoder->inFlight() >= 3 ? 1s : 0ms);) {
                take(packet);
            }
        }
        for (EncodedPacket packet; encoder->inFlight() && encoder->receive(packet, 2s);) {
            take(packet);
        }
        std::array<int, 3> perLayer{};
        int discardable = 0;
        for (const auto& packet : packets) {
            ++perLayer[packet.temporalId];
            discardable += packet.discardable;
        }
        for (uint32_t layer = 0; layer < layers; ++layer) {
            CHECK(perLayer[layer] > 0);
        }
        // A viewer of `taken` layers (from picture `from` on all of them)
        // decodes every picture it gets.
        const auto viewer = [&](uint32_t taken, size_t from) {
            auto decoder = decoderFor(decoding(codec, acceleration));
            std::atomic<bool> cancelled{false};
            int got = 0, decoded = 0;
            bool all = false;
            for (size_t i = 0; i < packets.size(); ++i) {
                all = all || (i >= from && !packets[i].temporalId);
                if (packets[i].temporalId >= (all ? layers : taken)) {
                    continue;
                }
                ++got;
                VideoFrame frame;
                try {
                    decoded += decoder->decode(packets[i], frame, cancelled);
                } catch (const Failure& failure) {
                    std::printf("%s %s, layers %u: picture %zu: %s\n", codecName(codec), accelerationName(acceleration),
                                taken, i, failure.what());
                    CHECK(false);
                }
            }
            CHECK(decoded >= got - 2); // At most a picture or two of decoder delay.
            return decoded;
        };
        const int every = viewer(layers, packets.size()), base = viewer(1, packets.size());
        const int middle = layers == 3 ? viewer(2, packets.size()) : 0;
        viewer(1, packets.size() / 2); // Layer 0, then all from the middle on.
        std::printf("%s %s: L1T%u: %zu pictures, per layer %d/%d/%d, %d discardable; decoded %d of all, %d of "
                    "layer 0%s (%s)\n",
                    codecName(codec), accelerationName(acceleration), layers, packets.size(), perLayer[0], perLayer[1],
                    perLayer[2], discardable, every, base,
                    layers == 3 ? (", " + std::to_string(middle) + " of layers 0 and 1").c_str() : "",
                    encoder->name().c_str());
        // A loss with references dropped (53 and 54 never reached the
        // viewers): a viewer of every layer and one of layer 0 alone each go
        // on at the first picture they get after it that says so
        // (LayeredRecovery, as the pipeline applies it), and decode on.
        if (encoder->refInvalidation()) {
            encoder->requestKeyframe(); // The viewers start at 49.
            std::array<std::unique_ptr<platform::VideoDecoder>, 2> decoders{
                decoderFor(decoding(codec, acceleration)), decoderFor(decoding(codec, acceleration))};
            std::array<bool, 2> waiting{};
            std::atomic<bool> cancelled{false};
            LayeredRecovery recovery;
            for (uint64_t sequence = 49; sequence <= 64; ++sequence) {
                if (sequence == 55) {
                    encoder->invalidateAfter(52);
                    waiting.fill(true);
                }
                auto frame = testFrame(width, height, sequence);
                frame.timestampNs = sequence * 33'333'333;
                encoder->submit(frame);
                EncodedPacket packet;
                CHECK(encoder->receive(packet, 1s));
                packet.picture = config.picture;
                readTemporalLayer(packet);
                recovery.apply(packet);
                CHECK(sequence != 49 || packet.keyframe);
                if (sequence == 53 || sequence == 54) {
                    continue;
                }
                for (size_t v = 0; v < decoders.size(); ++v) {
                    if (v == 1 && packet.temporalId) {
                        continue; // Layer 0 alone.
                    }
                    if (waiting[v] && !(packet.keyframe || packet.recoversAfter == 52u)) {
                        continue; // Refers to a lost picture, for all it says.
                    }
                    waiting[v] = false;
                    VideoFrame decoded;
                    decoders[v]->decode(packet, decoded, cancelled); // Throws where a reference is missing.
                }
            }
            CHECK(!waiting[0] && !waiting[1]);
        }
    }
}

// Pictures as large as the codec takes, beyond 4096x2160 and either way round,
// where the implementations of both directions take them
// (rcs_codec_implementation.max_width): a few each way, each a packet that
// travels as a message.
static void largeRoundTrip(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend) {
    const rcs_codec one[]{codec};
    const auto encoder = session::largestPictures(one, acceleration, backend, RCS_CODEC_ENCODER, implementations())[0];
    const auto decoder =
        session::largestPictures(one, acceleration, RCS_BACKEND_AUTO, RCS_CODEC_DECODER, implementations())[0];
    const auto limit =
        narrowed(narrowed(codecLimit(codec), encoder.width, encoder.height), decoder.width, decoder.height);
    const auto sizes = codec == RCS_CODEC_H264 ? std::vector<PictureSize>{{4096, 2304}, {2304, 4096}}
                                               : std::vector<PictureSize>{{5120, 2880}, {2880, 5120}, {7680, 4320}};
    for (const auto [width, height] : sizes) {
        if (!takes(limit, width, height)) {
            std::printf("%s %s %ux%u: more than this system's implementations take (%s); skipped\n", codecName(codec),
                        accelerationName(acceleration), width, height, limitText(limit).c_str());
            continue;
        }
        const platform::EncoderConfig config{codec, width, height, 30, 40'000'000, acceleration, backend,
                                         encoderDevice(codec, acceleration, backend)};
        auto encoding = encoderFor(config);
        encoding->open(config);
        auto decoding = decoderFor(::decoding(codec, acceleration));
        std::atomic<bool> cancelled{false};
        int decoded = 0;
        const auto take = [&](EncodedPacket& packet) {
            CHECK(packet.codec == codec && packet.width == width && packet.height == height);
            CHECK(protocol::videoProblem(packet).empty());
            VideoFrame frame;
            if (decoding->decode(packet, frame, cancelled)) {
                CHECK(frame.width == width && frame.height == height);
                ++decoded;
            }
        };
        constexpr int pictures = 6;
        for (uint64_t sequence = 1; sequence <= pictures; ++sequence) {
            encoding->submit(testFrame(width, height, sequence));
            for (EncodedPacket packet; encoding->receive(packet, 0ms);) {
                take(packet);
            }
        }
        for (EncodedPacket packet; encoding->inFlight() && encoding->receive(packet, 5s);) {
            take(packet);
        }
        CHECK(decoded >= pictures - 1); // At most a picture of decoder delay.
        std::printf("%s %s %ux%u: decoded=%d (%s -> %s)\n", codecName(codec), accelerationName(acceleration), width,
                    height, decoded,
                    encoding->name().c_str(), decoding->name().c_str());
    }
}

// A picture whose colour lives in single pixels: on the left, columns of red
// and blue one pixel wide (4:2:0 averages each pair into purple); on the
// right, patches of grey, orange and dark green. Across the top eighth, a
// ramp through all 256 greys.
static VideoFrame colourFrame(uint32_t width, uint32_t height, uint64_t sequence) {
    auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = pixels->data() + (size_t(y) * width + x) * 4;
            uint8_t b, g, r;
            if (y < height / 8) {
                b = g = r = uint8_t(x * 255 / (width - 1));
            } else if (x < width / 2) {
                const bool red = x % 2 == 0;
                b = red ? 0 : 255, g = 0, r = red ? 255 : 0;
            } else if (y < height / 3) {
                b = g = r = 128;
            } else if (y < height * 2 / 3) {
                b = 0, g = 128, r = 255;
            } else {
                b = 0, g = 100, r = 0;
            }
            p[0] = b, p[1] = g, p[2] = r, p[3] = 255;
        }
    }
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4;
    frame.pixels = std::move(pixels);
    frame.sequence = sequence;
    frame.timestampNs = sequence * 33'333'333;
    return frame;
}

// What a picture of colourFrame() decoded to BGRA keeps: its patches' colours
// and, in 4:4:4, the columns' (mean red of the red columns minus that of the
// blue ones; 4:2:0 leaves about 0).
struct Kept {
    int worstPatch = 0;
    int columns = 0;
};
static Kept kept(const rcs_frame& image) {
    const auto at = [&](uint32_t x, uint32_t y) { return image.data + size_t(y) * image.stride_bytes + x * 4; };
    Kept result;
    const struct {
        uint32_t y;
        uint8_t b, g, r;
    } patches[]{{image.height / 6, 128, 128, 128}, {image.height / 2, 0, 128, 255}, {image.height * 5 / 6, 0, 100, 0}};
    for (const auto& patch : patches) {
        const uint8_t* p = at(image.width * 3 / 4, patch.y);
        result.worstPatch = std::max({result.worstPatch, std::abs(p[0] - patch.b), std::abs(p[1] - patch.g),
                                      std::abs(p[2] - patch.r)});
    }
    int64_t difference = 0, count = 0;
    for (uint32_t y = image.height / 4; y < image.height * 3 / 4; ++y) {
        for (uint32_t x = 8; x + 9 < image.width / 2; x += 2) {
            difference += int(at(x, y)[2]) - int(at(x + 1, y)[2]);
            ++count;
        }
    }
    result.columns = int(difference / std::max<int64_t>(count, 1));
    return result;
}

// The distinct luma values of colourFrame()'s ramp (rows 4 to height / 8 - 8)
// in a decoded GPU frame of `layout`, at its samples' own depth. Its 256
// greys keep about 256 values in 10 bits, more where coding adds steps
// between them; 8 bits of limited range have 220 (16-235).
static size_t rampLevels(const VideoFrame& frame, rcs_pixel_format layout) {
    const bool wide = layout == RCS_PIXEL_P010 || layout == RCS_PIXEL_P410;
    std::set<int> levels;
    const auto scan = [&](const uint8_t* luma, size_t stride) {
        for (uint32_t y = 4; y + 8 < frame.height / 8; ++y) {
            const uint8_t* line = luma + y * stride;
            for (uint32_t x = 0; x < frame.width; ++x) {
                levels.insert(wide ? (line[2 * x] | line[2 * x + 1] << 8) >> 6 : line[x]);
            }
        }
    };
#ifdef __APPLE__
    auto buffer = static_cast<CVPixelBufferRef>(frame.surface.get());
    CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    scan(static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0)),
         CVPixelBufferGetBytesPerRowOfPlane(buffer, 0));
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
#elif defined(_WIN32)
    auto* texture = static_cast<ID3D11Texture2D*>(frame.surface.get());
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    platform::win::Com<ID3D11Device> device;
    texture->GetDevice(device.out());
    platform::win::Com<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.out());
    platform::win::Com<ID3D11Texture2D> staging;
    platform::win::check(device->CreateTexture2D(&desc, nullptr, staging.out()), "Luma readback texture");
    context->CopyResource(staging.value, texture);
    D3D11_MAPPED_SUBRESOURCE map{};
    platform::win::check(context->Map(staging.value, 0, D3D11_MAP_READ, 0, &map), "Luma readback");
    scan(static_cast<const uint8_t*>(map.pData), map.RowPitch);
    context->Unmap(staging.value, 0);
#endif
    return levels.size();
}

// Each picture format an implementation reports, really: the encoder's
// bitstream says that chroma format and bit depth; where this system decodes
// it too, the GPU frames have the format's layout and the stream's colour,
// and BGRA read from them (VideoToolbox's conversion, and the SDK's own of
// the GPU frame) keeps the patches' colours, in 4:4:4 the single pixels' and
// in 10 bits more steps of the ramp than 8 bits hold. Every format is tried
// with BT.709 limited range and BT.601 full range.
static void formatRoundTrip(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend,
                            const rcsdk::Picture& picture, std::shared_ptr<void> device) {
    constexpr uint32_t width = 640, height = 360;
    platform::EncoderConfig config{codec, width, height, 30, 20'000'000, acceleration, backend, device, picture};
    auto encoder = encoderFor(config);
    encoder->open(config);
    const auto& all = implementations();
    const bool decodable =
        std::any_of(all.begin(), all.end(), [&](const platform::CodecImplementation& i) {
            return i.codec == codec && i.direction == RCS_CODEC_DECODER && i.acceleration == acceleration &&
                   (i.pictureFormats & (1u << picture.format));
        });
    std::unique_ptr<platform::VideoDecoder> gpu, cpu;
    if (decodable) {
        gpu = decoderFor(decoding(codec, acceleration, false, picture));
        cpu = decoderFor(decoding(codec, acceleration, true, picture));
    }
    // The other acceleration's decoder too, where it takes the format: a
    // hardware encoder's stream decoded on the CPU, and the other way.
    const rcs_acceleration otherAcceleration =
        acceleration == RCS_ACCELERATION_HARDWARE ? RCS_ACCELERATION_SOFTWARE : RCS_ACCELERATION_HARDWARE;
    std::unique_ptr<platform::VideoDecoder> other;
    if (std::any_of(all.begin(), all.end(), [&](const platform::CodecImplementation& i) {
            return i.codec == codec && i.direction == RCS_CODEC_DECODER && i.acceleration == otherAcceleration &&
                   (i.pictureFormats & (1u << picture.format));
        })) {
        other = decoderFor(decoding(codec, otherAcceleration, true, picture));
    }
    std::atomic<bool> cancelled{false};
    int keyframes = 0, decoded = 0, otherDecoded = 0;
    size_t levels = 0;
    bool surfaces = false; // Whether YCbCr GPU frames came (not Windows' software decoders' BGRA8).
    Kept fromGpu{999, 0}, fromCpu{999, 0}, fromOther{999, 0};
    // What a frame shows: its CPU pixels (Windows' software decoders give
    // those whatever was asked), else read from its GPU frame.
    const auto keptOf = [](const VideoFrame& frame) {
        Kept result{999, 0};
        withImage(frame, [&](const rcs_frame& image) { result = kept(image); });
        return result;
    };
    const auto takeOther = [&](const EncodedPacket& packet) {
        VideoFrame frame;
        if (other && other->decode(packet, frame, cancelled)) {
            fromOther = keptOf(frame);
            ++otherDecoded;
        }
    };
    const auto take = [&](EncodedPacket& packet) {
        packet.picture = picture; // As the pipeline stamps it.
        takeOther(packet);
        if (packet.keyframe) {
            ++keyframes;
            CHECK((keyframeSampling(codec, packet.config, packet.data) == samplingOf(picture.format)));
            CHECK(samplingProblem(packet).empty());
        }
        if (!gpu) {
            return;
        }
        VideoFrame frame;
        if (gpu->decode(packet, frame, cancelled)) {
            if (frame.pixels) { // A system without GPU frames (Linux): BGRA8 on the CPU.
                CHECK(!session::capabilities().software_decoding_gpu_frames);
            } else {
                rcs_gpu_frame_info info = RCS_GPU_FRAME_INFO_INIT;
                CHECK(platform::describeGpuFrame(frame, info));
                if (info.format == RCS_PIXEL_BGRA8) { // Windows' software decoders and NVDEC (4:4:4) give BGRA8.
                    CHECK(session::capabilities().platform == RCS_PLATFORM_WINDOWS &&
                          (acceleration == RCS_ACCELERATION_SOFTWARE || chroma444(picture.format)) &&
                          sameColor(info.color, rgbColor(colorOf(picture))));
                } else {
                    const bool wide = bitDepth(picture.format) > 8;
                    const rcs_pixel_format layout = chroma444(picture.format)
                                                        ? (wide ? RCS_PIXEL_P410 : RCS_PIXEL_NV24)
                                                    : wide ? RCS_PIXEL_P010
                                                           : RCS_PIXEL_NV12;
                    CHECK(info.format == layout && sameColor(info.color, colorOf(picture)));
                    levels = rampLevels(frame, info.format);
                    surfaces = true;
                }
            }
            fromGpu = keptOf(frame);
            ++decoded;
        }
        if (cpu->decode(packet, frame, cancelled)) {
            fromCpu = keptOf(frame);
        }
    };
    for (uint64_t sequence = 1; sequence <= 10; ++sequence) {
        if (sequence == 6) {
            encoder->requestKeyframe();
        }
        encoder->submit(colourFrame(width, height, sequence));
        for (EncodedPacket packet; encoder->receive(packet, 0ms);) {
            take(packet);
        }
    }
    for (EncodedPacket packet; encoder->inFlight() && encoder->receive(packet, 5s);) {
        take(packet);
    }
    const std::string otherText = other ? ", " + std::string(acceleration == RCS_ACCELERATION_HARDWARE ? "software" : "hardware") +
                                              " decoder: " + std::to_string(otherDecoded) + " decoded, patches within " +
                                              std::to_string(fromOther.worstPatch) + ", columns " +
                                              std::to_string(fromOther.columns)
                                        : std::string();
    std::printf("%s %s %s, %s: %d keyframes, %s; patches within %d / %d, columns %d / %d (GPU frame / CPU image), "
                "ramp %zu levels%s (%s)\n",
                codecName(codec), accelerationName(acceleration), pictureText(picture).c_str(), backendName(backend),
                keyframes,
                decodable ? ("decoded " + std::to_string(decoded)).c_str() : "no decoder here", fromGpu.worstPatch,
                fromCpu.worstPatch, fromGpu.columns, fromCpu.columns, levels, otherText.c_str(),
                encoder->name().c_str());
    CHECK(keyframes >= 2);
    if (decodable) {
        CHECK(decoded >= 8);
        CHECK(fromGpu.worstPatch <= 16 && fromCpu.worstPatch <= 16);
        if (chroma444(picture.format)) {
            CHECK(fromGpu.columns > 150 && fromCpu.columns > 150); // Red stays red next to blue.
        }
        if (bitDepth(picture.format) > 8 && surfaces) { // BGRA8 pixels keep 256 levels at most.
            CHECK(levels > 240); // Steps 8 bits cannot tell apart.
        }
    }
    if (other) {
        CHECK(otherDecoded >= 8 && fromOther.worstPatch <= 16);
        if (chroma444(picture.format)) {
            CHECK(fromOther.columns > 150);
        }
    }
}

// The checks of an implementation on its GPU (`device`), by what it lists:
// whether it drops references, its temporal layers, its picture formats.
static void onItsGpu(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend, std::shared_ptr<void> device,
                     bool refInvalidation, uint32_t layers, uint32_t formats, const rcs_capabilities& caps) {
    bitrateChange(codec, acceleration, backend, device);
    referenceInvalidation(codec, acceleration, backend, device, refInvalidation);
    temporalLayers(codec, acceleration, backend, device, layers);
    formatRoundTrip(codec, acceleration, backend, {RCS_PICTURE_420_8, RCS_MATRIX_BT601, RCS_RANGE_FULL}, device);
    for (uint32_t format = RCS_PICTURE_420_10; format <= RCS_PICTURE_HDR10_444; ++format) {
        if ((formats & (1u << format)) && (caps.picture_formats & (1u << format))) {
            formatRoundTrip(codec, acceleration, backend, picture(format, RCS_MATRIX_BT709, RCS_RANGE_LIMITED), device);
            if (!hdr(format)) { // Each conversion path follows the matrix and range.
                formatRoundTrip(codec, acceleration, backend, picture(format, RCS_MATRIX_BT601, RCS_RANGE_FULL),
                                device);
            }
        }
    }
}

#ifdef _WIN32
// A device on the GPU of `luid` (packed; 0: none), for that GPU's encoders.
static std::shared_ptr<void> deviceOn(uint64_t luid) {
    if (!luid) {
        return nullptr;
    }
    auto adapter = platform::win::adapterWith(LUID{DWORD(luid), LONG(luid >> 32)});
    auto d3d = std::make_shared<platform::win::Device>();
    d3d->create(adapter.value);
    return std::shared_ptr<void>(d3d, d3d->device.value);
}

// Media Foundation's hardware encoders by GPU, whichever GPU drives the
// displays and whichever Windows gives programs by default: on a device of
// each GPU, and on none (CPU frames: a device of its own on the default GPU),
// an encoder opens where one is listed for that GPU, is one of those, and
// encodes; elsewhere opening fails, UNSUPPORTED, saying why: no other GPU's
// encoder stands in, and none listed that does not start (NVIDIA's while
// another GPU is the default one: win::startFailure).
static void mediaFoundationByGpu() {
    const auto adapters = platform::win::hardwareAdapters();
    for (size_t a = 0; a < adapters.size() + 1 && !adapters.empty(); ++a) {
        const bool own = a == adapters.size();
        const auto& adapter = adapters[own ? 0 : a];
        const uint64_t luid = platform::win::packLuid(adapter.luid);
        const std::string where = platform::win::narrow(adapter.description) + (own ? " (CPU frames)" : "");
        for (const rcs_codec codec : standardCodecs) {
            std::vector<std::string> listed;
            for (const auto& i : implementations()) {
                if (i.codec == codec && i.direction == RCS_CODEC_ENCODER &&
                    i.acceleration == RCS_ACCELERATION_HARDWARE && i.backend == RCS_BACKEND_MEDIA_FOUNDATION &&
                    i.adapterLuid == luid) {
                    listed.push_back(i.name);
                }
            }
            const platform::EncoderConfig config{codec, 640, 360, 30, 2'000'000, RCS_ACCELERATION_HARDWARE,
                                                 RCS_BACKEND_MEDIA_FOUNDATION, own ? nullptr : deviceOn(luid)};
            try {
                auto encoder = encoderFor(config);
                encoder->open(config);
                encoder->submit(testFrame(640, 360, 1));
                EncodedPacket packet;
                CHECK(encoder->receive(packet, 1s) && packet.keyframe);
                std::printf("%s on %s: %s\n", codecName(codec), where.c_str(), encoder->name().c_str());
                CHECK(std::find(listed.begin(), listed.end(), encoder->name()) != listed.end());
            } catch (const Failure& failure) {
                std::printf("%s on %s: %s\n", codecName(codec), where.c_str(), failure.what());
                CHECK(listed.empty() && failure.result() == RCS_ERROR_UNSUPPORTED);
            }
        }
    }
}

// NVENC through its interface 12.0, as a driver from before 570 (which speaks
// no 13.0) gets it, where this driver speaks 13.0 too (a driver speaks every
// version up to its own): the checks above again, by what 12.0 lists (HEVC
// and AV1 without temporal layers).
static void nvencInterface12(const rcs_capabilities& caps) {
    if (!platform::win::nvenc13::api().loaded()) {
        std::printf("NVENC: the driver speaks no interface 13.0; the checks above went through 12.0\n");
        return;
    }
    const auto& older = platform::win::nvenc12::api();
    std::set<uint64_t> gpus;
    for (const auto& i : implementations()) {
        if (i.backend != RCS_BACKEND_NVENC || !gpus.insert(i.adapterLuid).second) {
            continue;
        }
        const auto device = deviceOn(i.adapterLuid);
        platform::win::NvencAdapter gpu;
        CHECK(older.loaded() && older.probe(static_cast<ID3D11Device*>(device.get()), &gpu));
        nvencOlder = true;
        for (const rcs_codec codec : {RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_AV1}) {
            if (!(gpu.codecs & (1u << codec))) {
                continue;
            }
            std::printf("%s through NVENC's interface %s:\n", codecName(codec), older.name);
            CHECK(codec == RCS_CODEC_H264 || gpu.temporalLayers[codec] == 1);
            roundTrip(codec, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_NVENC, caps);
            onItsGpu(codec, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_NVENC, device,
                     (gpu.refInvalidation & (1u << codec)) != 0, gpu.temporalLayers[codec], gpu.formats[codec], caps);
        }
        nvencOlder = false;
    }
}

// The GPU decoders the SDK drives directly (`backend`: NVDEC, oneVPL's), on
// their GPUs. AUTO in hardware takes them for 4:4:4 there, and Media
// Foundation for 4:2:0 where Windows decodes the codec on that GPU; NVDEC
// chosen for 4:2:0 refuses. For each picture format one lists, each
// encoder's stream of it (on any GPU, or the CPU's) that drops references: a
// decoder that never saw two lost pictures goes on from the recovery (or,
// where it cannot, says so); a reset decoder waits for the next keyframe and
// decodes from it; pictures of 3840x2160 (from hardware encoders) decode,
// each with its colours.
static void gpuDecoding(rcs_backend backend) {
    for (const auto& n : implementations()) {
        if (n.backend != backend || n.direction != RCS_CODEC_DECODER) {
            continue;
        }
        const auto device = deviceOn(n.adapterLuid);
        for (const uint32_t format : {RCS_PICTURE_420_8, RCS_PICTURE_420_10, RCS_PICTURE_444_8, RCS_PICTURE_444_10}) {
            if (!(n.pictureFormats & (1u << format))) {
                continue;
            }
            const auto picture = rcsdk::picture(rcs_picture_format(format), RCS_MATRIX_BT709, RCS_RANGE_LIMITED);
            auto settings = decoding(n.codec, RCS_ACCELERATION_HARDWARE, false, picture);
            settings.device = device;
            if (chroma444(picture.format)) {
                CHECK(decoderFor(settings)->backend() == backend);
            }
            settings.backend = backend;
            for (const auto& e : implementations()) {
                if (e.codec != n.codec || e.direction != RCS_CODEC_ENCODER || !(e.pictureFormats & (1u << format))) {
                    continue;
                }
                const auto encoding = deviceOn(e.adapterLuid);
                // Intel's HEVC decoder decodes the recovery pictures of NVIDIA's Media Foundation
                // encoder (long-term references, the pictures since lost) wrong and says so
                // (driver 32.0.101.8860): oneVPL's refuses them.
                const bool refused = backend == RCS_BACKEND_ONEVPL && n.codec == RCS_CODEC_HEVC &&
                                     e.backend == RCS_BACKEND_MEDIA_FOUNDATION &&
                                     e.name.find("NVIDIA") != std::string::npos;
                referenceInvalidation(n.codec, e.acceleration, e.backend, encoding, e.refInvalidation, picture,
                                      settings, refused);
                // A reset decoder: nothing until the keyframe asked for at 9, then each picture.
                for (const PictureSize size : {PictureSize{640, 360}, PictureSize{3840, 2160}}) {
                    if ((size.width > 640 && e.acceleration != RCS_ACCELERATION_HARDWARE) ||
                        !takes(narrowed(codecLimit(n.codec), e.maxWidth, e.maxHeight), size.width, size.height) ||
                        !takes(narrowed(codecLimit(n.codec), n.maxWidth, n.maxHeight), size.width, size.height)) {
                        continue;
                    }
                    const platform::EncoderConfig config{n.codec,        size.width, size.height, 30,     20'000'000,
                                                         e.acceleration, e.backend,  encoding,    picture};
                    auto encoder = encoderFor(config);
                    encoder->open(config);
                    auto decoder = decoderFor(settings);
                    std::atomic<bool> cancelled{false};
                    int decoded = 0, afterReset = 0;
                    for (uint64_t sequence = 1; sequence <= 12; ++sequence) {
                        if (sequence == 9) {
                            encoder->requestKeyframe();
                        }
                        if (sequence == 7) {
                            decoder->reset();
                        }
                        encoder->submit(colourFrame(size.width, size.height, sequence));
                        EncodedPacket packet;
                        CHECK(encoder->receive(packet, 2s));
                        packet.picture = picture;
                        if (sequence == 7 || sequence == 8) {
                            continue; // What a reset decoder skips: no keyframe yet.
                        }
                        VideoFrame frame;
                        if (decoder->decode(packet, frame, cancelled)) {
                            CHECK(frame.width == size.width && frame.height == size.height);
                            Kept shown{999, 0};
                            withImage(frame, [&](const rcs_frame& image) { shown = kept(image); });
                            CHECK(shown.worstPatch <= 16 && (!chroma444(picture.format) || shown.columns > 150));
                            ++decoded;
                            afterReset += sequence >= 9;
                        }
                    }
                    std::printf("%s %s %ux%u: %d of 10 decoded, %d of 4 after a reset (%s -> %s)\n", codecName(n.codec),
                                pictureFormatName(format), size.width, size.height, decoded, afterReset,
                                encoder->name().c_str(), decoder->name().c_str());
                    CHECK(decoded == 10 && afterReset == 4);
                }
            }
        }
        // 4:2:0 with AUTO: Media Foundation's where Windows decodes the codec on this GPU, else oneVPL's.
        auto settings = decoding(n.codec, RCS_ACCELERATION_HARDWARE);
        settings.device = device;
        const bool windows = platform::win::gpuDecodedFormats(static_cast<ID3D11Device*>(device.get()), n.codec) &&
                             platform::win::windowsDecodes(n.codec);
        if (windows || (n.pictureFormats & (1u << RCS_PICTURE_420_8))) {
            CHECK(decoderFor(settings)->backend() == (windows ? RCS_BACKEND_MEDIA_FOUNDATION : backend));
        }
        if (!(n.pictureFormats & (1u << RCS_PICTURE_420_8))) { // NVDEC.
            settings.backend = backend;
            try {
                decoderFor(settings);
                CHECK(false);
            } catch (const Failure& failure) {
                std::printf("%s with 4:2:0: %s\n", backendName(backend), failure.what());
                CHECK(failure.result() == RCS_ERROR_UNSUPPORTED);
            }
        }
    }
}
#else
static std::shared_ptr<void> deviceOn(uint64_t) {
    return nullptr;
}
#endif

// Every decoder of a codec takes every encoder's stream of it, in 4:2:0
// 8-bit, which all take: the libraries' decoders the system's encoders'
// streams (OpenH264 a hardware encoder's H.264, dav1d NVENC's AV1) and the
// other way, each on the GPU it is on.
static void crossDecoding(rcs_codec codec) {
    constexpr uint32_t width = 640, height = 360;
    const rcsdk::Picture picture{RCS_PICTURE_420_8, RCS_MATRIX_BT709, RCS_RANGE_LIMITED};
    struct Implementation {
        rcs_acceleration acceleration;
        rcs_backend backend;
        uint64_t adapter;
        std::string name;
        bool operator==(const Implementation& o) const {
            return acceleration == o.acceleration && backend == o.backend && adapter == o.adapter;
        }
    };
    std::vector<Implementation> encoders, decoders;
    for (const auto& i : implementations()) {
        if (i.codec == codec && (i.pictureFormats & (1u << picture.format))) {
            auto& list = i.direction == RCS_CODEC_ENCODER ? encoders : decoders;
            const Implementation entry{i.acceleration, i.backend, i.adapterLuid, i.name};
            if (std::find(list.begin(), list.end(), entry) == list.end()) {
                list.push_back(entry);
            }
        }
    }
    std::atomic<bool> cancelled{false};
    for (const auto& e : encoders) {
        platform::EncoderConfig config{codec, width, height, 30, 8'000'000, e.acceleration, e.backend,
                                       deviceOn(e.adapter), picture};
        auto encoder = encoderFor(config);
        encoder->open(config);
        std::vector<EncodedPacket> packets;
        const auto take = [&](EncodedPacket& packet) {
            packet.picture = picture; // As the pipeline stamps it.
            packets.push_back(std::move(packet));
        };
        for (uint64_t sequence = 1; sequence <= 10; ++sequence) {
            encoder->submit(colourFrame(width, height, sequence));
            for (EncodedPacket packet; encoder->receive(packet, 0ms);) {
                take(packet);
            }
        }
        for (EncodedPacket packet; encoder->inFlight() && encoder->receive(packet, 5s);) {
            take(packet);
        }
        for (const auto& d : decoders) {
            auto settings = decoding(codec, d.acceleration, true, picture);
            settings.backend = d.backend;
            settings.device = deviceOn(d.adapter);
            auto decoder = decoderFor(settings);
            int decoded = 0, worst = 0;
            for (const auto& packet : packets) {
                VideoFrame frame;
                if (decoder->decode(packet, frame, cancelled)) {
                    ++decoded;
                    withImage(frame, [&](const rcs_frame& image) { worst = std::max(worst, kept(image).worstPatch); });
                }
            }
            std::printf("%s cross-decoding: %s -> %s: %d of %zu decoded, patches within %d\n", codecName(codec),
                        e.name.c_str(), d.name.c_str(), decoded, packets.size(), worst);
            CHECK(decoded + 2 >= int(packets.size()) && worst <= 16);
        }
    }
}

#ifdef _WIN32
// The 4:4:4 inputs of NVENC and Media Foundation's encoders (yuv_shader.hpp):
// red, blue, grey and white written as AYUV and Y410 and read back are Y, Cb
// and Cr of the matrix and range asked for, at 8 or 10 bits (H.273's
// formulas, within a step), each where its layout keeps it.
static void yuvShaderConversion() {
    namespace win = platform::win;
    using Layout = win::YuvShader::Layout;
    win::Device d3d;
    d3d.create();
    const uint8_t bgra[]{0, 0, 255, 255, 255, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255};
    auto sourceDesc = win::textureDesc(4, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    const D3D11_SUBRESOURCE_DATA data{bgra, sizeof(bgra), 0};
    win::Com<ID3D11Texture2D> source;
    win::check(d3d.device->CreateTexture2D(&sourceDesc, &data, source.out()), "YCbCr test source");
    win::YuvShader shader;
    for (const auto& [layout, name] : {std::pair{Layout::ayuv, "AYUV"}, std::pair{Layout::y410, "Y410"}}) {
        if (!win::YuvShader::supported(d3d.device.value, layout)) {
            std::printf("%s: this GPU stores no such texture from a shader; skipped\n", name);
            continue;
        }
        win::Com<ID3D11Texture2D> target, staging;
        auto targetDesc = win::YuvShader::targetDesc(layout, 4, 1);
        win::check(d3d.device->CreateTexture2D(&targetDesc, nullptr, target.out()), "YCbCr test target");
        auto stagingDesc = targetDesc;
        stagingDesc.BindFlags = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        win::check(d3d.device->CreateTexture2D(&stagingDesc, nullptr, staging.out()), "YCbCr test readback");
        const unsigned bits = layout == Layout::ayuv ? 8 : 10;
        const rcs_picture_format format = bits > 8 ? RCS_PICTURE_444_10 : RCS_PICTURE_444_8;
        for (const rcsdk::Picture picture : {rcsdk::Picture{format, RCS_MATRIX_BT709, RCS_RANGE_LIMITED},
                                             rcsdk::Picture{format, RCS_MATRIX_BT601, RCS_RANGE_FULL}}) {
            shader.draw(d3d.device.value, d3d.context.value, source.value, 4, 1, target.value, layout, picture);
            d3d.context->CopyResource(staging.value, target.value);
            D3D11_MAPPED_SUBRESOURCE map{};
            win::check(d3d.context->Map(staging.value, 0, D3D11_MAP_READ, 0, &map), "YCbCr test map");
            const auto* bytes = static_cast<const uint8_t*>(map.pData);
            // Y, Cb, Cr of pixel i as stored.
            const auto stored = [&](int i, int channel) {
                if (layout == Layout::ayuv) {
                    return int(bytes[4 * i + 2 - channel]); // V, U, Y, A.
                }
                uint32_t word;
                std::memcpy(&word, bytes + 4 * i, 4);
                return int(word >> (channel == 0 ? 10 : channel == 1 ? 0 : 20) & 0x3ff);
            };
            const double kr = picture.matrix == RCS_MATRIX_BT601 ? 0.299 : 0.2126;
            const double kb = picture.matrix == RCS_MATRIX_BT601 ? 0.114 : 0.0722;
            const bool limited = picture.range == RCS_RANGE_LIMITED;
            const double unit = double(1u << (bits - 8)), top = double((1u << bits) - 1);
            int worst = 0;
            for (int i = 0; i < 4; ++i) {
                const double b = bgra[4 * i] / 255.0, g = bgra[4 * i + 1] / 255.0, r = bgra[4 * i + 2] / 255.0;
                const double y = kr * r + (1 - kr - kb) * g + kb * b;
                const double u = (b - y) / (2 * (1 - kb)), v = (r - y) / (2 * (1 - kr));
                const double chroma = limited ? 224 * unit : top;
                const double expected[]{limited ? 219 * unit * y + 16 * unit : top * y, chroma * u + 128 * unit,
                                        chroma * v + 128 * unit};
                for (int c = 0; c < 3; ++c) {
                    worst = std::max(worst, int(std::lround(std::abs(stored(i, c) - expected[c]))));
                }
            }
            d3d.context->Unmap(staging.value, 0);
            std::printf("%s %s: largest difference %d\n", name, pictureText(picture).c_str(), worst);
            CHECK(worst <= 1);
        }
    }
}
#endif

#if defined(__APPLE__) || defined(__ANDROID__)
// Each decoder's picture formats that decode probes found (decode_probes.hpp),
// again through the pipeline's decoder: the probe's mid-grey keyframe
// becomes a GPU frame of the format's layout and a grey CPU image, read from
// the GPU frame too (where Android's GPU draws what the CPU cannot map).
static void probedFormats() {
    namespace p = platform::probes;
    const auto grey = [](const rcs_frame& image) {
        int worst = 0;
        for (uint32_t y = 0; y < image.height; y += 7) {
            for (uint32_t x = 0; x < image.width; x += 7) {
                const uint8_t* pixel = image.data + size_t(y) * image.stride_bytes + x * 4;
                for (int c = 0; c < 3; ++c) {
                    worst = std::max(worst, std::abs(int(pixel[c]) - 129));
                }
            }
        }
        return worst;
    };
    for (const auto& i : platform::codecImplementations()) {
        if (i.direction != RCS_CODEC_DECODER) {
            continue;
        }
        std::printf("%s %s decoder %s: %s\n", codecName(i.codec),
                    i.acceleration == RCS_ACCELERATION_HARDWARE ? "hardware" : "software", i.name.c_str(),
                    pictureFormatList(i.pictureFormats).c_str());
        for (const auto& probe : p::all) {
            if (probe.codec != i.codec || !(i.pictureFormats & (1u << probe.format))) {
                continue;
            }
            EncodedPacket packet;
            packet.codec = probe.codec;
            packet.picture.format = probe.format;
            packet.width = p::width;
            packet.height = p::height;
            packet.keyframe = true;
            packet.config.assign(probe.config.begin(), probe.config.end());
            packet.data.assign(probe.data.begin(), probe.data.end());
            for (const bool cpuImages : {false, true}) {
                platform::DecoderConfig config;
                config.codec = i.codec;
                config.acceleration = i.acceleration;
                config.picture = packet.picture;
                config.cpuImages = cpuImages;
                auto decoder = platform::createVideoDecoder(config);
                decoder->open(config);
                std::atomic<bool> cancelled{false};
                VideoFrame frame;
                bool decoded = false;
                for (int attempt = 0; attempt < 10 && !decoded; ++attempt) { // The same keyframe again.
                    packet.sequence = uint64_t(attempt);
                    decoded = decoder->decode(packet, frame, cancelled);
                }
                CHECK(decoded);
                int worst = 999;
                platform::withCpuImage(frame, [&](const rcs_frame& image) { worst = grey(image); });
                std::printf("  %s, %s: grey within %d", pictureFormatName(probe.format),
                            cpuImages ? "CPU image" : "GPU frame", worst);
                CHECK(worst <= 8);
                if (!cpuImages) {
                    rcs_gpu_frame_info info = RCS_GPU_FRAME_INFO_INIT;
                    CHECK(platform::describeGpuFrame(frame, info));
                    const bool wide = bitDepth(probe.format) > 8;
                    const rcs_pixel_format layout = chroma444(probe.format) ? (wide ? RCS_PIXEL_P410 : RCS_PIXEL_NV24)
                                                    : wide                  ? RCS_PIXEL_P010
                                                                            : RCS_PIXEL_NV12;
                    std::printf(", layout %u", unsigned(info.format));
                    CHECK(info.format == layout);
                }
                std::printf("\n");
            }
        }
    }
}
#endif

int main() {
    return runTest("codec_roundtrip", [] {
#if defined(__APPLE__) || defined(__ANDROID__)
        probedFormats();
#endif
#ifdef _WIN32
        yuvShaderConversion();
#endif
        const auto& caps = session::capabilities();
        for (const rcs_acceleration acceleration : {RCS_ACCELERATION_HARDWARE, RCS_ACCELERATION_SOFTWARE}) {
            const bool hardware = acceleration == RCS_ACCELERATION_HARDWARE;
            const uint32_t available = hardware ? caps.hardware_encoders & caps.hardware_decoders
                                                : caps.software_encoders & caps.software_decoders;
            for (const rcs_codec codec : standardCodecs) {
                if ((available & (1u << codec)) == 0) {
                    std::printf("%s %s: not available on this system; skipped\n", codecName(codec),
                                hardware ? "hardware" : "software");
                    continue;
                }
                // Each backend that encodes it (NVENC next to Media Foundation).
                std::vector<rcs_backend> backends;
                for (const auto& i : implementations()) {
                    if (i.codec == codec && i.direction == RCS_CODEC_ENCODER && i.acceleration == acceleration &&
                        std::find(backends.begin(), backends.end(), i.backend) == backends.end()) {
                        backends.push_back(i.backend);
                    }
                }
                for (const rcs_backend backend : backends) {
                    roundTrip(codec, acceleration, backend, caps);
                    keyframesWhenAsked(codec, acceleration, backend);
                    largeRoundTrip(codec, acceleration, backend);
                    // The picture formats the backend's implementation on each GPU
                    // reports, there (Windows: NVIDIA's and Intel's encoders).
                    std::set<uint64_t> gpus;
                    for (const auto& i : implementations()) {
                        if (i.codec != codec || i.direction != RCS_CODEC_ENCODER || i.acceleration != acceleration ||
                            i.backend != backend || !gpus.insert(i.adapterLuid).second) {
                            continue;
                        }
                        onItsGpu(codec, acceleration, backend, deviceOn(i.adapterLuid), i.refInvalidation,
                                 i.temporalLayers, i.pictureFormats, caps);
                    }
                }
            }
        }
#ifdef _WIN32
        mediaFoundationByGpu();
        nvencInterface12(caps);
        gpuDecoding(RCS_BACKEND_NVDEC);
        gpuDecoding(RCS_BACKEND_ONEVPL);
#endif
        for (const rcs_codec codec : standardCodecs) {
            crossDecoding(codec);
        }
        // A codec without an encoder or decoder for the acceleration fails at
        // open(), instead of being substituted or failing at the first frame.
        const auto unsupported = [](const auto& open) {
            try {
                open();
            } catch (const Failure& failure) {
                return failure.result() == RCS_ERROR_UNSUPPORTED;
            }
            return false;
        };
        for (const rcs_acceleration acceleration : {RCS_ACCELERATION_HARDWARE, RCS_ACCELERATION_SOFTWARE}) {
            const bool hardware = acceleration == RCS_ACCELERATION_HARDWARE;
            for (const rcs_codec codec : standardCodecs) {
                if (((hardware ? caps.hardware_encoders : caps.software_encoders) & (1u << codec)) == 0) {
                    CHECK(unsupported([&] {
                        const platform::EncoderConfig config{
                            codec, 640, 360, 30, 2'000'000, acceleration, RCS_BACKEND_AUTO, nullptr};
                        encoderFor(config)->open(config);
                    }));
                }
                if (((hardware ? caps.hardware_decoders : caps.software_decoders) & (1u << codec)) == 0) {
                    CHECK(unsupported([&] { decoderFor(decoding(codec, acceleration)); }));
                }
            }
        }
    });
}
