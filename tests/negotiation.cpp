// Codec offers and the codec both ends derive from them.
#include "codecs/codecs.hpp"
#include "media/levels.hpp"
#include "session/checks.hpp"
#include "session/negotiation.hpp"
#include "session/stream_plan.hpp"
#include "support/check.hpp"

using namespace rcsdk;
using namespace rcsdk::session;
using Codecs = std::vector<rcs_codec>;

// Media Foundation implementations, one per bit of each mask, taking what their codec takes.
static std::vector<platform::CodecImplementation> implementations(uint32_t hardwareEncoders, uint32_t softwareEncoders,
                                                                  uint32_t hardwareDecoders,
                                                                  uint32_t softwareDecoders) {
    std::vector<platform::CodecImplementation> result;
    const auto add = [&](uint32_t mask, rcs_codec_direction direction, rcs_acceleration acceleration) {
        for (const rcs_codec codec : {RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_AV1}) {
            if (mask & (1u << codec)) {
                const auto limit = codecLimit(codec);
                result.push_back({codec, direction, acceleration, RCS_BACKEND_MEDIA_FOUNDATION, 0, limit.width,
                                  limit.height, defaultPictureFormats, "test"});
            }
        }
    };
    add(hardwareEncoders, RCS_CODEC_ENCODER, RCS_ACCELERATION_HARDWARE);
    add(softwareEncoders, RCS_CODEC_ENCODER, RCS_ACCELERATION_SOFTWARE);
    add(hardwareDecoders, RCS_CODEC_DECODER, RCS_ACCELERATION_HARDWARE);
    add(softwareDecoders, RCS_CODEC_DECODER, RCS_ACCELERATION_SOFTWARE);
    return result;
}

int main() {
    return runTest("negotiation", [] {
        constexpr uint32_t h264 = 1u << RCS_CODEC_H264, hevc = 1u << RCS_CODEC_HEVC, av1 = 1u << RCS_CODEC_AV1;
        // Hardware HEVC and software H.264 encoding; hardware H.264 and AV1 decoding, software H.264 and HEVC.
        const auto caps = implementations(hevc, h264, h264 | av1, h264 | hevc);
        rcs_encoder_config encoder = RCS_ENCODER_CONFIG_INIT;
        rcs_decoder_config decoder = RCS_DECODER_CONFIG_INIT;
        const auto each = [&](auto change) {
            change(encoder);
            change(decoder);
        };

        // Only listed codecs are offered, in the host's order, for the configured acceleration.
        each([](auto& c) {
            c.codecs[0] = RCS_CODEC_AV1;
            c.codecs[1] = RCS_CODEC_HEVC;
            c.codecs[2] = RCS_CODEC_H264;
        });
        CHECK((encoderOffer(encoder, caps) == Codecs{RCS_CODEC_HEVC}));
        CHECK((decoderOffer(decoder, caps) == Codecs{RCS_CODEC_AV1, RCS_CODEC_H264}));
        each([](auto& c) { c.acceleration = RCS_ACCELERATION_SOFTWARE; });
        CHECK((encoderOffer(encoder, caps) == Codecs{RCS_CODEC_H264}));
        CHECK((decoderOffer(decoder, caps) == Codecs{RCS_CODEC_HEVC, RCS_CODEC_H264}));
        // ...and backend: implementations of another backend are not offered.
        decoder.backend = RCS_BACKEND_VIDEOTOOLBOX;
        CHECK(decoderOffer(decoder, caps).empty());
        // Two backends for one codec (NVENC next to Media Foundation): AUTO
        // offers what either encodes, a named backend only its own.
        {
            auto both = caps;
            both.push_back(
                {RCS_CODEC_H264, RCS_CODEC_ENCODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_NVENC, 0, 4096, 4096,
                 defaultPictureFormats, "nv"});
            rcs_encoder_config two = RCS_ENCODER_CONFIG_INIT;
            two.codecs[0] = RCS_CODEC_HEVC;
            two.codecs[1] = RCS_CODEC_H264;
            CHECK((encoderOffer(two, both) == Codecs{RCS_CODEC_HEVC, RCS_CODEC_H264}));
            two.backend = RCS_BACKEND_NVENC;
            CHECK((encoderOffer(two, both) == Codecs{RCS_CODEC_H264}));
            two.backend = RCS_BACKEND_MEDIA_FOUNDATION;
            CHECK((encoderOffer(two, both) == Codecs{RCS_CODEC_HEVC}));
            CHECK(encoderOffer(two, caps).size() == 1);
        }
        // A codec the system lacks is never replaced by one the host did not list.
        encoder.acceleration = RCS_ACCELERATION_HARDWARE;
        encoder.codecs[0] = RCS_CODEC_H264;
        encoder.codecs[1] = encoder.codecs[2] = 0;
        CHECK(encoderOffer(encoder, caps).empty());

        // The host's own codec is offered whatever the acceleration and backend.
        encoder.codecs[0] = RCS_CODEC_HOST_FIRST;
        encoder.codecs[1] = RCS_CODEC_H264;
        CHECK((encoderOffer(encoder, caps, 1u << RCS_CODEC_HOST_FIRST) == Codecs{RCS_CODEC_HOST_FIRST}));
        CHECK((encoderOffer(encoder, caps, (1u << RCS_CODEC_HOST_FIRST) | (1u << RCS_CODEC_H264)) ==
               Codecs{RCS_CODEC_HOST_FIRST, RCS_CODEC_H264}));
        CHECK(encoderOffer(encoder, caps).empty());
        CHECK(codecMask(encoder.codecs) == ((1u << RCS_CODEC_HOST_FIRST) | (1u << RCS_CODEC_H264)));

        // The sharer's order decides; the viewer's order does not matter.
        const Codecs share{RCS_CODEC_HEVC, RCS_CODEC_H264}, view{RCS_CODEC_H264, RCS_CODEC_HEVC};
        CHECK(negotiatedCodec(share, view) == RCS_CODEC_HEVC);
        CHECK(negotiatedCodec(view, share) == RCS_CODEC_H264);
        CHECK(negotiatedCodec(share, Codecs{RCS_CODEC_AV1, RCS_CODEC_H264}) == RCS_CODEC_H264);
        CHECK(negotiatedCodec(share, Codecs{RCS_CODEC_AV1}) == 0);
        CHECK(negotiatedCodec(Codecs{}, view) == 0);

        // What the peer offered, for the host and for error messages.
        CHECK(codecMask(share) == (h264 | hevc));
        CHECK(codecMask(Codecs{}) == 0);
        CHECK(codecList(share) == "HEVC, H.264");
        CHECK(codecList(Codecs{}) == "none");

        // Audio: the listed codecs this system has, in the host's order.
        using AudioCodecs = std::vector<rcs_audio_codec>;
        const rcs_send_audio_config audio = RCS_SEND_AUDIO_CONFIG_INIT; // Opus, AAC, PCM.
        const uint32_t withoutAac = (1u << RCS_AUDIO_OPUS) | (1u << RCS_AUDIO_PCM);
        CHECK((audioOffer(audio.codecs, withoutAac | (1u << RCS_AUDIO_AAC)) ==
               AudioCodecs{RCS_AUDIO_OPUS, RCS_AUDIO_AAC, RCS_AUDIO_PCM}));
        CHECK((audioOffer(audio.codecs, withoutAac) == AudioCodecs{RCS_AUDIO_OPUS, RCS_AUDIO_PCM}));

        // An offer carries the role's settings and the platform; the peer sees it as rcs_peer_info.
        SessionConfig viewer;
        viewer.role = RCS_ROLE_VIEW;
        viewer.receiveAudio.enabled = 0; // Offered all the same: sound can be turned on during the run.
        viewer.sendAudio.enabled = 0;
        const auto offer = offerOf(viewer);
        CHECK(!offer.receivesAudio && !offer.sendsAudio && !offer.receiveAudio.empty() && !offer.sendAudio.empty() &&
              offer.platform == capabilities().platform);
        CHECK(offer.largest.size() == offer.codecs.size());
        CHECK(offer.formats.size() == offer.codecs.size());
        constexpr uint32_t sdr8 = 1u << RCS_PICTURE_420_8, sdr10 = 1u << RCS_PICTURE_420_10,
                           full444 = 1u << RCS_PICTURE_444_8, hdr10 = 1u << RCS_PICTURE_HDR10_420;
        const auto peer = peerOf({Codecs{RCS_CODEC_HEVC}, {{4096, 2304}}, {sdr8 | full444}, AudioCodecs{RCS_AUDIO_PCM},
                                  AudioCodecs{RCS_AUDIO_OPUS}, true, false, RCS_PLATFORM_WINDOWS},
                                 std::nullopt);
        CHECK(peer.struct_size == sizeof(rcs_peer_info) && peer.platform == RCS_PLATFORM_WINDOWS);
        CHECK(peer.codecs == hevc && peer.send_audio_codecs == (1u << RCS_AUDIO_PCM) &&
              peer.receive_audio_codecs == (1u << RCS_AUDIO_OPUS) && peer.sends_audio == 1 && !peer.receives_audio);
        CHECK(peer.codec_limits[0].codec == RCS_CODEC_HEVC && peer.codec_limits[0].max_width == 4096 &&
              peer.codec_limits[0].max_height == 2304 && peer.codec_limits[0].picture_formats == (sdr8 | full444) &&
              peer.codec_limits[1].codec == 0);

        // Picture formats: what the host listed, in each codec as its first
        // implementation (or the host's own codec) takes it, within what the
        // capture produces; a codec that takes none of them is not offered.
        {
            auto formatted = caps; // Hardware HEVC encoding, hardware H.264 and AV1 decoding...
            for (auto& i : formatted) {
                if (i.codec == RCS_CODEC_HEVC && i.direction == RCS_CODEC_ENCODER) {
                    i.pictureFormats = sdr8 | sdr10 | full444 | hdr10;
                }
                if (i.codec == RCS_CODEC_AV1) {
                    i.pictureFormats = sdr8 | sdr10 | hdr10;
                }
            }
            const Codecs listed{RCS_CODEC_HEVC, RCS_CODEC_AV1, RCS_CODEC_H264};
            const rcs_picture_format wanted[]{RCS_PICTURE_444_8, RCS_PICTURE_420_10, RCS_PICTURE_420_8};
            CHECK((pictureFormats(listed, wanted, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_AUTO, RCS_CODEC_ENCODER,
                                  formatted) == std::vector<uint32_t>{full444 | sdr10 | sdr8, 0, 0}));
            CHECK((pictureFormats(listed, wanted, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_AUTO, RCS_CODEC_DECODER,
                                  formatted) == std::vector<uint32_t>{0, sdr10 | sdr8, sdr8}));
            // The capture produces only 4:2:0 8-bit; the host's own HEVC encoder takes 4:4:4 too.
            CHECK((pictureFormats(listed, wanted, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_AUTO, RCS_CODEC_ENCODER,
                                  formatted, 0, 0, sdr8) == std::vector<uint32_t>{sdr8, 0, 0}));
            CHECK((pictureFormats(listed, wanted, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_AUTO, RCS_CODEC_ENCODER,
                                  formatted, hevc, sdr8 | full444) == std::vector<uint32_t>{sdr8 | full444, 0, 0}));

            // A sharer's offer on a machine: the codecs with a format in common, each with its formats.
            agent::Machine machine;
            machine.implementations = formatted;
            machine.capabilities.picture_formats = sdr8 | full444; // No 10-bit capture here.
            SessionConfig share;
            share.role = RCS_ROLE_SHARE;
            share.encoder.codecs[0] = RCS_CODEC_HEVC;
            share.encoder.codecs[1] = RCS_CODEC_H264;
            share.encoder.formats[0] = RCS_PICTURE_444_8;
            share.encoder.formats[1] = RCS_PICTURE_420_8;
            auto shared = offerOf(share, machine);
            CHECK((shared.codecs == Codecs{RCS_CODEC_HEVC}) &&
                  (shared.formats == std::vector<uint32_t>{full444 | sdr8}));
            share.encoder.formats[0] = RCS_PICTURE_420_10;
            share.encoder.formats[1] = 0;
            CHECK(offerOf(share, machine).codecs.empty()); // Listed, encoded, but not captured.
            share.capture.source = RCS_CAPTURE_ENCODED; // The host's bitstream: what it lists.
            share.encoder.formats[1] = RCS_PICTURE_HDR10_444;
            shared = offerOf(share, machine);
            CHECK((shared.codecs == Codecs{RCS_CODEC_HEVC, RCS_CODEC_H264}) &&
                  (shared.formats == std::vector<uint32_t>{sdr10 | (1u << RCS_PICTURE_HDR10_444),
                                                           sdr10 | (1u << RCS_PICTURE_HDR10_444)}));
            share.capture.source = RCS_CAPTURE_HOST; // BGRA8 pictures: SDR only.
            machine.capabilities.picture_formats = allPictureFormats;
            share.encoder.formats[1] = RCS_PICTURE_HDR10_420;
            CHECK((offerOf(share, machine).formats == std::vector<uint32_t>{sdr10}));
        }

        // A codec counts as common only with a picture format both take;
        // several viewers take what all of them take.
        {
            protocol::CodecOffer sharer{Codecs{RCS_CODEC_HEVC, RCS_CODEC_H264}, {{8192, 8192}, {4096, 4096}},
                                        {full444, full444 | sdr8}};
            protocol::CodecOffer viewer{Codecs{RCS_CODEC_H264, RCS_CODEC_HEVC}, {{4096, 4096}, {8192, 8192}},
                                        {full444 | sdr8, sdr8}};
            CHECK(negotiatedCodec(sharer, viewer) == RCS_CODEC_H264); // HEVC: no format in common.
            viewer.formats[0] = sdr10;
            CHECK(negotiatedCodec(sharer, viewer) == 0);
            CHECK(formatMismatch(sharer, viewer) ==
                  "No picture format in common: in HEVC the sharer sends 4:4:4 8-bit and the viewer takes 4:2:0 "
                  "8-bit; in H.264 the sharer sends 4:2:0 8-bit, 4:4:4 8-bit and the viewer takes 4:2:0 10-bit");
            const protocol::CodecOffer viewers[]{
                {Codecs{RCS_CODEC_HEVC, RCS_CODEC_H264}, {{8192, 8192}, {4096, 4096}}, {full444 | sdr8, sdr8}},
                {Codecs{RCS_CODEC_H264, RCS_CODEC_HEVC}, {{4096, 2304}, {8192, 8192}}, {full444, sdr8 | sdr10}}};
            const auto common = commonOffer(viewers);
            CHECK((common.codecs == Codecs{RCS_CODEC_HEVC}) && (common.formats == std::vector<uint32_t>{sdr8}));
        }

        // The largest picture each codec takes: the host's own codec takes all
        // its codec takes; the system's, its first implementation's.
        {
            std::vector<platform::CodecImplementation> sized{
                {RCS_CODEC_HEVC, RCS_CODEC_DECODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_MEDIA_FOUNDATION, 0, 5120,
                 2880, defaultPictureFormats, "first"},
                {RCS_CODEC_HEVC, RCS_CODEC_DECODER, RCS_ACCELERATION_SOFTWARE, RCS_BACKEND_MEDIA_FOUNDATION, 0, 8192,
                 8192, defaultPictureFormats, "software"},
                {RCS_CODEC_H264, RCS_CODEC_DECODER, RCS_ACCELERATION_SOFTWARE, RCS_BACKEND_MEDIA_FOUNDATION, 0, 4096,
                 2304, defaultPictureFormats, "software"}};
            const Codecs listed{RCS_CODEC_HEVC, RCS_CODEC_H264};
            CHECK((largestPictures(listed, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_AUTO, RCS_CODEC_DECODER, sized) ==
                   std::vector<PictureSize>{{5120, 2880}, {4096, 4096}})); // No hardware H.264 here: its codec's.
            CHECK((largestPictures(listed, RCS_ACCELERATION_SOFTWARE, RCS_BACKEND_AUTO, RCS_CODEC_DECODER, sized) ==
                   std::vector<PictureSize>{{8192, 8192}, {4096, 2304}}));
            CHECK((largestPictures(listed, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_AUTO, RCS_CODEC_DECODER, sized,
                                   1u << RCS_CODEC_HEVC) == std::vector<PictureSize>{{8192, 8192}, {4096, 4096}}));
        }

        // HARDWARE_FIRST: a codec is offered when either acceleration decodes
        // it, with the formats both take, at the smaller largest picture
        // where the software decoder adds formats; each stream is decoded in
        // hardware where it takes the format, else in software.
        {
            const uint32_t f420 = 1u << RCS_PICTURE_420_8, f420_10 = 1u << RCS_PICTURE_420_10,
                           f444 = 1u << RCS_PICTURE_444_8, f444_10 = 1u << RCS_PICTURE_444_10;
            const std::vector<platform::CodecImplementation> windows{
                {RCS_CODEC_HEVC, RCS_CODEC_DECODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_MEDIA_FOUNDATION, 1, 8192,
                 4352, f420 | f420_10, "HEVC on the GPU"},
                {RCS_CODEC_HEVC, RCS_CODEC_DECODER, RCS_ACCELERATION_SOFTWARE, RCS_BACKEND_MEDIA_FOUNDATION, 0, 4096,
                 2304, f420 | f420_10 | f444 | f444_10, "HEVC on the CPU"},
                {RCS_CODEC_AV1, RCS_CODEC_DECODER, RCS_ACCELERATION_SOFTWARE, RCS_BACKEND_MEDIA_FOUNDATION, 0, 4096,
                 2304, f420, "AV1 on the CPU"},
                {RCS_CODEC_H264, RCS_CODEC_DECODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_MEDIA_FOUNDATION, 1, 4096,
                 4096, f420, "H.264 on the GPU"},
                {RCS_CODEC_H264, RCS_CODEC_DECODER, RCS_ACCELERATION_SOFTWARE, RCS_BACKEND_MEDIA_FOUNDATION, 0, 4096,
                 2304, f420, "H.264 on the CPU"}};
            const Codecs listed{RCS_CODEC_HEVC, RCS_CODEC_AV1, RCS_CODEC_H264};
            const rcs_picture_format wanted[]{RCS_PICTURE_444_8, RCS_PICTURE_420_8};
            constexpr auto first = RCS_ACCELERATION_HARDWARE_FIRST;
            CHECK((codecOffer(listed, first, RCS_BACKEND_AUTO, RCS_CODEC_DECODER, windows) == listed));
            CHECK((codecOffer(listed, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_AUTO, RCS_CODEC_DECODER, windows) ==
                   Codecs{RCS_CODEC_HEVC, RCS_CODEC_H264}));
            CHECK((pictureFormats(listed, wanted, first, RCS_BACKEND_AUTO, RCS_CODEC_DECODER, windows) ==
                   std::vector<uint32_t>{f444 | f420, f420, f420}));
            CHECK((pictureFormats(listed, wanted, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_AUTO, RCS_CODEC_DECODER,
                                  windows) == std::vector<uint32_t>{f420, 0, f420}));
            // HEVC: the software decoder adds 4:4:4, so its smaller limit;
            // AV1: software only; H.264: nothing added, the hardware one's.
            CHECK((largestPictures(listed, first, RCS_BACKEND_AUTO, RCS_CODEC_DECODER, windows) ==
                   std::vector<PictureSize>{{4096, 2304}, {4096, 2304}, {4096, 4096}}));
            rcs_decoder_config decoder = RCS_DECODER_CONFIG_INIT;
            decoder.acceleration = first;
            CHECK(codecs::streamAcceleration(decoder.acceleration, decoder.backend, RCS_CODEC_HEVC, RCS_CODEC_DECODER, RCS_PICTURE_420_8, 0, 0, 0, windows) == RCS_ACCELERATION_HARDWARE);
            CHECK(codecs::streamAcceleration(decoder.acceleration, decoder.backend, RCS_CODEC_HEVC, RCS_CODEC_DECODER, RCS_PICTURE_444_8, 0, 0, 0, windows) == RCS_ACCELERATION_SOFTWARE);
            CHECK(codecs::streamAcceleration(decoder.acceleration, decoder.backend, RCS_CODEC_AV1, RCS_CODEC_DECODER, RCS_PICTURE_420_8, 0, 0, 0, windows) == RCS_ACCELERATION_SOFTWARE);
            CHECK(codecs::streamAcceleration(decoder.acceleration, decoder.backend, RCS_CODEC_H264, RCS_CODEC_DECODER, RCS_PICTURE_444_8, 0, 0, 0, windows) ==
                  RCS_ACCELERATION_HARDWARE); // Neither takes it: hardware, whose decoder then says why.
            decoder.acceleration = RCS_ACCELERATION_SOFTWARE;
            CHECK(codecs::streamAcceleration(decoder.acceleration, decoder.backend, RCS_CODEC_HEVC, RCS_CODEC_DECODER, RCS_PICTURE_420_8, 0, 0, 0, windows) == RCS_ACCELERATION_SOFTWARE);
        }

        // The stream: the sharer's own settings unless the viewer asks, within the policy.
        SessionConfig sharer;
        sharer.role = RCS_ROLE_SHARE;
        // Offers that take what their codecs take.
        const auto offering = [](Codecs codecs, uint32_t formats = defaultPictureFormats) {
            protocol::CodecOffer offer;
            for (const rcs_codec codec : codecs) {
                offer.codecs.push_back(codec);
                offer.largest.push_back({codecLimit(codec).width, codecLimit(codec).height});
                offer.formats.push_back(formats);
            }
            return offer;
        };
        const auto both = offering({RCS_CODEC_H264, RCS_CODEC_HEVC});
        const platform::DisplayMode display{2560, 1440, 144};
        using Request = protocol::StreamRequest;
        const auto plan = [](rcs_codec codec, uint32_t width, uint32_t height, uint32_t rate, uint32_t bitrate) {
            return StreamPlan{codec, {}, width, height, rate, bitrate, {}};
        };
        CHECK((planStream(sharer, both, both, {}, display) == plan(RCS_CODEC_H264, 1920, 1080, 60, 20'000'000)));
        CHECK((planStream(sharer, both, both, Request{3840, 2160, 240, 400'000'000, RCS_CODEC_HEVC}, display) ==
               plan(RCS_CODEC_HEVC, 2560, 1440, 144, 200'000'000))); // The display's size and rate, 200 Mbit/s.
        CHECK((planStream(sharer, both, both, Request{3000, 1000, 0, 0, 0}, display) ==
               plan(RCS_CODEC_H264, 2560, 852, 60, 20'000'000))); // Scaled to fit, aspect kept, even.
        CHECK(
            planStream(sharer, both, offering({RCS_CODEC_H264}), Request{0, 0, 0, 0, RCS_CODEC_HEVC}, display).codec ==
            RCS_CODEC_H264); // A codec one end lacks is not used.
        // Unknown display: what the codec takes, and 60 FPS.
        CHECK((planStream(sharer, both, both, Request{3840, 2160, 120, 0, 0}, {}) ==
               plan(RCS_CODEC_H264, 3840, 2160, 60, 20'000'000)));

        // Beyond 4096x2160: what the codec takes, either way round.
        const platform::DisplayMode fiveK{5120, 2880, 60}, portrait{2160, 3840, 60};
        CHECK((planStream(sharer, both, both, Request{5120, 2880, 0, 0, RCS_CODEC_HEVC}, fiveK) ==
               plan(RCS_CODEC_HEVC, 5120, 2880, 60, 20'000'000))); // HEVC takes 5K as it is.
        CHECK((planStream(sharer, both, both, Request{5120, 2880, 0, 0, 0}, fiveK) ==
               plan(RCS_CODEC_H264, 4096, 2304, 60, 20'000'000))); // H.264: 4096 wide, 36 864 macroblocks.
        CHECK((planStream(sharer, both, both, Request{2160, 3840, 0, 0, 0}, portrait) ==
               plan(RCS_CODEC_H264, 2160, 3840, 60, 20'000'000))); // A portrait display's own size, in H.264 too.
        const auto square = planStream(sharer, both, both, Request{3840, 3840, 0, 0, 0}, {}); // The macroblocks bind.
        CHECK(square.width == square.height && square.width >= 3056 && square.width % 2 == 0 &&
              takes(codecLimit(RCS_CODEC_H264), square.width, square.height));
        CHECK((planStream(sharer, both, both, Request{8192, 4352, 0, 0, RCS_CODEC_HEVC}, {}) ==
               plan(RCS_CODEC_HEVC, 8192, 4352, 60, 20'000'000))); // The most a stream may be.
        // The viewer's decoder, and the sharer's encoder, take less: the request fits them.
        auto smallDecoder = both;
        smallDecoder.largest[1] = {4096, 2304};
        CHECK((planStream(sharer, both, smallDecoder, Request{5120, 2880, 0, 0, RCS_CODEC_HEVC}, fiveK) ==
               plan(RCS_CODEC_HEVC, 4096, 2304, 60, 20'000'000)));
        CHECK((planStream(sharer, smallDecoder, both, Request{2880, 5120, 0, 0, RCS_CODEC_HEVC}, {}) ==
               plan(RCS_CODEC_HEVC, 1296, 2304, 60, 20'000'000)));
        // What the sharer's encoder takes a second in a format (NVENC's H.264
        // 4:4:4 at level 6.0, its 4:2:0 at 6.2): the frame rate fits it, the
        // size stays; without a machine (or a rate) nothing is fitted.
        {
            agent::Machine nvenc;
            platform::CodecImplementation h264{RCS_CODEC_H264, RCS_CODEC_ENCODER, RCS_ACCELERATION_HARDWARE,
                                               RCS_BACKEND_NVENC, 1, 4096, 4096,
                                               defaultPictureFormats | (1u << RCS_PICTURE_444_8), "NVENC H.264"};
            h264.maxPixelRates[RCS_PICTURE_420_8] = levelPixelRate(RCS_CODEC_H264, 62);
            h264.maxPixelRates[RCS_PICTURE_444_8] = levelPixelRate(RCS_CODEC_H264, 60);
            nvenc.implementations = {h264};
            const auto full = offering({RCS_CODEC_H264}, defaultPictureFormats | (1u << RCS_PICTURE_444_8));
            const platform::DisplayMode uhd160{3840, 2160, 160};
            Request ask{3840, 2160, 160, 0, RCS_CODEC_H264};
            ask.format = RCS_PICTURE_444_8;
            const auto fitted = planStream(sharer, full, full, ask, uhd160, &nvenc);
            CHECK(fitted.codec == RCS_CODEC_H264 && fitted.picture.format == RCS_PICTURE_444_8 &&
                  fitted.width == 3840 && fitted.height == 2160 && fitted.framesPerSecond == 128 &&
                  fitted.encoderLimited == 160);
            CHECK(planStream(sharer, full, full, ask, uhd160).framesPerSecond == 160); // No machine: as asked.
            ask.format = RCS_PICTURE_420_8;
            const auto plain = planStream(sharer, full, full, ask, uhd160, &nvenc);
            CHECK(plain.framesPerSecond == 160 && !plain.encoderLimited);
            nvenc.implementations[0].maxPixelRates = {}; // Nothing said: nothing fitted.
            ask.format = RCS_PICTURE_444_8;
            CHECK(planStream(sharer, full, full, ask, uhd160, &nvenc).framesPerSecond == 160);
            // Levels as the standards count them: H.264 in whole macroblocks.
            CHECK(levelPixels(RCS_CODEC_H264, 1920, 1080) == 120 * 68 * 256 && levelPixels(RCS_CODEC_HEVC, 1920, 1080) ==
                  1920 * 1080);
            CHECK(levelPixelRate(RCS_CODEC_H264, 52) == 2'073'600ull * 256 && levelPixelRate(RCS_CODEC_H264, 70) == 0 &&
                  levelPixelRate(RCS_CODEC_HEVC, 183) == 2'139'095'040 && levelPixelRate(RCS_CODEC_AV1, 13) == 534'773'760 &&
                  levelPixelRate(RCS_CODEC_AV1, 2) == 0);
        }
        // The sharer's own size is never made smaller: the first codec both
        // offer that takes it; with none, refused, saying which limit.
        {
            SessionConfig large = sharer;
            large.shareVideo.width = 5120;
            large.shareVideo.height = 2880;
            CHECK((planStream(large, both, both, {}, fiveK) == plan(RCS_CODEC_HEVC, 5120, 2880, 60, 20'000'000)));
            const auto refused = planStream(large, both, offering({RCS_CODEC_H264}), {}, fiveK);
            CHECK(refused.codec == RCS_CODEC_H264 && refused.width == 5120 &&
                  refused.refusal.find("5120x2880 is more than H.264 takes: 4096 a side and 36 864 macroblocks "
                                       "(e.g. 4096x2304)") != std::string::npos);
            const auto hevcFirst = offering({RCS_CODEC_HEVC, RCS_CODEC_H264});
            CHECK((planStream(large, hevcFirst, both, {}, fiveK) == plan(RCS_CODEC_HEVC, 5120, 2880, 60, 20'000'000)));
            CHECK(planStream(large, hevcFirst, smallDecoder, {}, fiveK)
                      .refusal.find("the viewer's HEVC decoder takes: 4096 wide and 2304 high") != std::string::npos);
            auto smallEncoder = hevcFirst;
            smallEncoder.largest[0] = {4096, 2304};
            CHECK(planStream(large, smallEncoder, both, {}, fiveK)
                      .refusal.find("the sharer's HEVC encoder takes: 4096 wide and 2304 high") != std::string::npos);
            // A request it follows replaces it: requests are fitted, never refused.
            CHECK((planStream(large, both, both, Request{5120, 2880, 0, 0, 0}, fiveK) ==
                   plan(RCS_CODEC_H264, 4096, 2304, 60, 20'000'000)));
            CHECK(sizeProblem(RCS_CODEC_H264, 2304, 4096, {4096, 4096}, {}).empty());
            CHECK(!sizeProblem(RCS_CODEC_H264, 4096, 2320, {4096, 4096}, {}).empty()); // 37 120 macroblocks.
        }

        sharer.shareVideo.max_width = 1280;
        sharer.shareVideo.max_height = 720;
        sharer.shareVideo.max_frames_per_second = 30;
        sharer.shareVideo.max_bitrate_bps = 8'000'000;
        CHECK((planStream(sharer, both, both, Request{1920, 1080, 60, 50'000'000, 0}, display) ==
               plan(RCS_CODEC_H264, 1280, 720, 30, 8'000'000)));
        sharer.shareVideo.follow_requests = 0;
        CHECK((planStream(sharer, both, both, Request{640, 360, 30, 1'000'000, RCS_CODEC_HEVC}, display) ==
               plan(RCS_CODEC_H264, 1920, 1080, 60, 20'000'000)));
        CHECK(planStream(sharer, offering({RCS_CODEC_AV1}), both, {}, display).codec == 0);

        // The picture format: the viewer's request where both take it, else
        // the sharer's first both take; matrix and range the sharer's, HDR10's
        // matrix BT.2020's.
        {
            SessionConfig formats;
            formats.role = RCS_ROLE_SHARE;
            formats.encoder.formats[0] = RCS_PICTURE_444_8;
            formats.encoder.formats[1] = RCS_PICTURE_HDR10_420;
            formats.encoder.formats[2] = RCS_PICTURE_420_8;
            formats.encoder.matrix = RCS_MATRIX_BT601;
            formats.encoder.range = RCS_RANGE_FULL;
            const auto offers = offering({RCS_CODEC_HEVC, RCS_CODEC_H264}, full444 | hdr10 | sdr8);
            const auto picture = [&](const protocol::CodecOffer& viewer, Request request = {}) {
                const auto planned = planStream(formats, offers, viewer, request, display);
                return std::pair(planned.codec, planned.picture);
            };
            CHECK((picture(offering({RCS_CODEC_HEVC}, full444 | sdr8)) ==
                   std::pair(rcs_codec(RCS_CODEC_HEVC), Picture{RCS_PICTURE_444_8, RCS_MATRIX_BT601, RCS_RANGE_FULL})));
            CHECK((picture(offering({RCS_CODEC_HEVC}, hdr10 | sdr8)) ==
                   std::pair(rcs_codec(RCS_CODEC_HEVC),
                             Picture{RCS_PICTURE_HDR10_420, RCS_MATRIX_BT2020, RCS_RANGE_FULL})));
            Request ask;
            ask.format = RCS_PICTURE_420_8;
            CHECK(picture(offering({RCS_CODEC_HEVC}, full444 | sdr8), ask).second.format == RCS_PICTURE_420_8);
            ask.format = RCS_PICTURE_420_10; // Neither takes it: the sharer's choice.
            CHECK(picture(offering({RCS_CODEC_HEVC}, full444 | sdr8), ask).second.format == RCS_PICTURE_444_8);
            // HEVC has no format in common with this viewer, H.264 has.
            auto mixed = offering({RCS_CODEC_HEVC, RCS_CODEC_H264}, sdr10);
            mixed.formats[1] = sdr8;
            CHECK((picture(mixed) ==
                   std::pair(rcs_codec(RCS_CODEC_H264), Picture{RCS_PICTURE_420_8, RCS_MATRIX_BT601, RCS_RANGE_FULL})));
            ask = {};
            ask.codec = RCS_CODEC_HEVC; // Asked for, but without a common format there.
            CHECK(picture(mixed, ask).first == RCS_CODEC_H264);
            formats.shareVideo.follow_requests = 0;
            ask.format = RCS_PICTURE_420_8;
            CHECK(picture(offering({RCS_CODEC_HEVC}, full444 | sdr8), ask).second.format == RCS_PICTURE_444_8);
        }
    });
}
