// Message encoding round trips and rejection of malformed input.
#include "media/nal.hpp"
#include "platform/platform.hpp"
#include "protocol/messages.hpp"
#include "util/util.hpp"
#include "support/check.hpp"

#include <rcsdk/view_geometry.hpp>

#include <cstring>

using namespace rcsdk;
using namespace rcsdk::protocol;

static EncodedPacket packet() {
    EncodedPacket p;
    p.codec = RCS_CODEC_H264;
    p.width = 1920;
    p.height = 1080;
    p.keyframe = true;
    p.sequence = 42;
    p.timestampNs = 123456789;
    nal::appendUnit(p.config, std::vector<uint8_t>{0x67, 1, 2, 3});
    nal::appendUnit(p.config, std::vector<uint8_t>{0x68, 4});
    nal::appendUnit(p.data, std::vector<uint8_t>{0x65, 9, 9, 9, 9});
    return p;
}

int main() {
    return runTest("protocol", [] {
        // Round trips.
        const auto video = std::get<EncodedPacket>(decode(encode(packet())));
        CHECK(video.width == 1920 && video.keyframe && video.sequence == 42 && video.timestampNs == 123456789);
        CHECK(video.config == packet().config && video.data == packet().data && !video.recoversAfter);
        // A picture made after the encoder dropped later ones from its references says after which.
        auto recovering = packet();
        recovering.keyframe = false;
        recovering.recoversAfter = 40;
        CHECK(std::get<EncodedPacket>(decode(encode(recovering))).recoversAfter == 40u);
        recovering.recoversAfter = 42; // Not before itself.
        CHECK(throws<Failure>([&] { decode(encode(recovering)); }));
        // Its encoding and temporal layer, and whether it is discardable (RCS_EXT_ENCODINGS).
        auto layered = packet();
        layered.stream = 2;
        layered.encoding = 2;
        layered.temporalId = 7;
        layered.discardable = true;
        const auto got = std::get<EncodedPacket>(decode(encode(layered)));
        CHECK(got.stream == 2 && got.encoding == 2 && got.temporalId == 7 && got.discardable);
        CHECK(!video.discardable && !video.encoding && !video.temporalId);
        layered.encoding = RCS_MAX_ENCODINGS;
        CHECK(throws<Failure>([&] { decode(encode(layered)); }));
        layered.encoding = 0;
        layered.temporalId = 8;
        CHECK(throws<Failure>([&] { decode(encode(layered)); }));
        auto flagged = encode(packet());
        flagged[1 + 1 + 1 + 1 + 4 + 3 + 4 + 4] |= 8; // A flag no version defines.
        CHECK(throws<Failure>([&] { decode(flagged); }));

        // A packet's picture: format, matrix and range travel; any other is refused.
        auto picture = packet();
        picture.picture = {RCS_PICTURE_HDR10_444, RCS_MATRIX_BT2020, RCS_RANGE_FULL};
        CHECK((std::get<EncodedPacket>(decode(encode(picture))).picture == picture.picture));
        for (const Picture wrong : {Picture{0, RCS_MATRIX_BT709, RCS_RANGE_LIMITED},
                                    Picture{RCS_PICTURE_HDR10_444 + 1, RCS_MATRIX_BT2020, RCS_RANGE_LIMITED},
                                    Picture{RCS_PICTURE_HDR10_420, RCS_MATRIX_BT709, RCS_RANGE_LIMITED},
                                    Picture{RCS_PICTURE_444_8, RCS_MATRIX_BT2020, RCS_RANGE_LIMITED},
                                    Picture{RCS_PICTURE_420_8, RCS_MATRIX_BT601, 3}}) {
            picture.picture = wrong;
            CHECK(throws<Failure>([&] { decode(encode(picture)); }));
            CHECK(!videoProblem(picture).empty());
        }

        rcs_stream_info info = RCS_STREAM_INFO_INIT;
        info.encodings = 1;
        info.layers = info.received_layers = 1;
        info.codec = RCS_CODEC_H264;
        info.format = RCS_PICTURE_420_8;
        info.color = colorOf(Picture{});
        info.width = 1920;
        info.height = 1080;
        info.frames_per_second = 60;
        info.bitrate_bps = 20'000'000;
        info.content_y = 60;
        info.content_width = 1920;
        info.content_height = 960;
        const auto stream = std::get<StreamAnnouncement>(decode(encode(StreamAnnouncement{info}))).info;
        CHECK(stream.content_y == 60 && stream.content_height == 960 && stream.bitrate_bps == 20'000'000);
        CHECK(stream.encoding == 0 && stream.encodings == 1);
        {
            // The encoding a viewer gets, of those the sharer runs (RCS_EXT_ENCODINGS).
            rcs_stream_info lower = info;
            lower.encoding = 2;
            lower.encodings = 3;
            const auto got = std::get<StreamAnnouncement>(decode(encode(StreamAnnouncement{lower}))).info;
            CHECK(got.encoding == 2 && got.encodings == 3);
            for (const auto& [encoding, encodings] : {std::pair{0u, 0u}, {1u, 1u}, {0u, RCS_MAX_ENCODINGS + 1}}) {
                lower.encoding = encoding;
                lower.encodings = encodings;
                CHECK(throws<Failure>([&] { decode(encode(StreamAnnouncement{lower})); }));
            }
            // Its temporal layers, and how many of them the viewer gets.
            rcs_stream_info layered = info;
            layered.layers = 3;
            layered.received_layers = 1;
            const auto taken = std::get<StreamAnnouncement>(decode(encode(StreamAnnouncement{layered}))).info;
            CHECK(taken.layers == 3 && taken.received_layers == 1);
            for (const auto& [layers, received] : {std::pair{0u, 0u}, {4u, 1u}, {2u, 3u}, {2u, 0u}}) {
                layered.layers = layers;
                layered.received_layers = received;
                CHECK(throws<Failure>([&] { decode(encode(StreamAnnouncement{layered})); }));
            }
        }
        CHECK(stream.encoder_acceleration == RCS_ACCELERATION_HARDWARE);
        info.encoder_acceleration = RCS_ACCELERATION_SOFTWARE;
        CHECK(std::get<StreamAnnouncement>(decode(encode(StreamAnnouncement{info}))).info.encoder_acceleration ==
              RCS_ACCELERATION_SOFTWARE);
        info.encoder_acceleration = RCS_ACCELERATION_SOFTWARE + 1;
        CHECK(throws<Failure>([&] { decode(encode(StreamAnnouncement{info})); }));
        info.encoder_acceleration = RCS_ACCELERATION_HARDWARE;
        // Whether the pointer is in the picture (one picture serves every viewer).
        info.cursor = RCS_CURSOR_SEPARATE;
        CHECK(std::get<StreamAnnouncement>(decode(encode(StreamAnnouncement{info}))).info.cursor ==
              RCS_CURSOR_SEPARATE);
        info.cursor = RCS_CURSOR_SEPARATE + 1;
        CHECK(throws<Failure>([&] { decode(encode(StreamAnnouncement{info})); }));
        info.cursor = RCS_CURSOR_IN_VIDEO;
        // The picture: format and colour, and an HDR10 stream's metadata.
        {
            rcs_stream_info hdr = info;
            hdr.codec = RCS_CODEC_HEVC;
            hdr.format = RCS_PICTURE_HDR10_420;
            hdr.color = colorOf({RCS_PICTURE_HDR10_420, RCS_MATRIX_BT2020, RCS_RANGE_LIMITED});
            hdr.hdr = {34000, 16000, 13250, 34500, 7500, 3000, 15635, 16450, 10'000'000, 50, 1000, 400};
            const auto got = std::get<StreamAnnouncement>(decode(encode(StreamAnnouncement{hdr}))).info;
            CHECK(got.format == RCS_PICTURE_HDR10_420 && sameColor(got.color, hdr.color) && sameHdr(got.hdr, hdr.hdr));
            CHECK(got.color.primaries == RCS_PRIMARIES_BT2020 && got.color.transfer == RCS_TRANSFER_PQ &&
                  got.color.matrix == RCS_MATRIX_BT2020);
            hdr.hdr.white_x = 50'001; // Beyond CIE x 1.
            CHECK(throws<Failure>([&] { decode(encode(StreamAnnouncement{hdr})); }));
            rcs_stream_info sdr = info;
            sdr.hdr.max_content_light = 1000; // SDR streams carry no HDR metadata.
            CHECK(throws<Failure>([&] { decode(encode(StreamAnnouncement{sdr})); }));
            sdr = info;
            sdr.color.matrix = RCS_MATRIX_BT2020; // Not an SDR matrix.
            CHECK(throws<Failure>([&] { decode(encode(StreamAnnouncement{sdr})); }));
            sdr = info;
            sdr.format = 0;
            CHECK(throws<Failure>([&] { decode(encode(StreamAnnouncement{sdr})); }));
            sdr = info;
            sdr.color.range = RCS_RANGE_FULL;
            sdr.color.matrix = RCS_MATRIX_BT601;
            CHECK(std::get<StreamAnnouncement>(decode(encode(StreamAnnouncement{sdr}))).info.color.range ==
                  RCS_RANGE_FULL);
        }

        const auto grant = std::get<ControlGrant>(decode(encode(ControlGrant{true, 7})));
        CHECK(grant.allowed && grant.epoch == 7);

        rcs_input_event event = RCS_INPUT_EVENT_INIT;
        event.type = RCS_INPUT_WHEEL;
        event.x = 100;
        event.wheel_y = -3;
        const auto input = std::get<Input>(decode(encode(Input{event, 9})));
        CHECK(input.epoch == 9 && input.event.wheel_y == -3 && input.event.x == 100);
        // Every field of every type travels: a pen with its pressure, tilt and flags...
        rcs_input_event pen = RCS_INPUT_EVENT_INIT;
        pen.type = RCS_INPUT_PEN;
        pen.x = 65535;
        pen.y = 1;
        pen.down = 1;
        pen.pressure = 1024;
        pen.tilt_x = -90;
        pen.tilt_y = 45;
        pen.rotation = 359;
        pen.pen = RCS_PEN_BARREL | RCS_PEN_ERASER;
        const auto penInput = std::get<Input>(decode(encode(Input{pen, 2}))).event;
        CHECK(std::memcmp(&penInput, &pen, sizeof(pen)) == 0);
        // ...relative motion, text and a media key.
        rcs_input_event motion = RCS_INPUT_EVENT_INIT;
        motion.type = RCS_INPUT_MOVE_RELATIVE;
        motion.dx = -10'000;
        motion.dy = 37;
        const auto motionInput = std::get<Input>(decode(encode(Input{motion, 2}))).event;
        CHECK(motionInput.dx == -10'000 && motionInput.dy == 37);
        rcs_input_event text = RCS_INPUT_EVENT_INIT;
        text.type = RCS_INPUT_TEXT;
        text.code = 0x1F600; // An emoji, beyond the 16-bit range.
        CHECK(std::get<Input>(decode(encode(Input{text, 2}))).event.code == 0x1F600);
        rcs_input_event media = RCS_INPUT_EVENT_INIT;
        media.type = RCS_INPUT_KEY;
        media.code = RCS_KEY_CONSUMER | 0xCD;
        CHECK(validInput(media));

        const CodecOffer offer{{RCS_CODEC_HEVC, RCS_CODEC_H264},
                               {{8192, 4352}, {4096, 2304}},
                               {(1u << RCS_PICTURE_444_8) | (1u << RCS_PICTURE_HDR10_420), defaultPictureFormats},
                               {RCS_AUDIO_OPUS},
                               {RCS_AUDIO_OPUS, RCS_AUDIO_PCM},
                               true,
                               true,
                               RCS_PLATFORM_MACOS,
                               (1u << RCS_INPUT_MOVE) | (1u << RCS_INPUT_PEN)};
        CHECK(std::get<CodecOffer>(decode(encode(offer))) == offer);

        CHECK(std::holds_alternative<KeyframeRequest>(decode(encode(KeyframeRequest{}))));

        // Malformed messages are rejected, never trusted.
        const auto rejected = [](const crypto::Bytes& bytes) {
            return throws<Failure>([&] { decode(bytes); });
        };
        const Refused untrusted{RCS_ERROR_AUTHENTICATION, "not trusted"};
        CHECK(std::get<Refused>(decode(encode(untrusted))) == untrusted);
        CHECK(std::get<Refused>(decode(encode(Refused{RCS_ERROR_BUSY, "full"}))) == (Refused{RCS_ERROR_BUSY, "full"}));
        CHECK(std::get<Refused>(decode(encode(Refused{RCS_ERROR_UNSUPPORTED, std::string(600, 'x')}))).reason.size() ==
              512);
        CHECK(rejected(encode(Refused{RCS_ERROR_NETWORK, "not a refusal's result"})));
        CHECK(std::get<Refused>(decode(encode(Refused{RCS_ERROR_REFUSED, "no"}))).result == RCS_ERROR_REFUSED);
        CHECK(std::get<Refused>(decode(encode(Refused{RCS_ERROR_TIMEOUT, ""}))).result == RCS_ERROR_TIMEOUT);
        CHECK(std::get<Admission>(decode(encode(Admission{59000}))).remainingMs == 59000);
        CHECK((std::get<RefLost>(decode(encode(RefLost{2, 1, 41, true}))) == RefLost{2, 1, 41, true}));
        CHECK(!std::get<RefLost>(decode(encode(RefLost{0, 0, 41}))).gaveUp);
        CHECK(rejected(encode(RefLost{4, 0, 41, true})));                 // Beyond RCS_MAX_STREAMS...
        CHECK(rejected(encode(RefLost{0, RCS_MAX_ENCODINGS, 41, true}))); // ...and RCS_MAX_ENCODINGS.
        CHECK((std::get<KeyframeRequest>(decode(encode(KeyframeRequest{3, 2}))) == KeyframeRequest{3, 2}));
        CHECK(rejected(encode(KeyframeRequest{0, RCS_MAX_ENCODINGS})));
        CHECK((std::get<StreamEnded>(decode(encode(StreamEnded{1}))) == StreamEnded{1}));
        CHECK(rejected(encode(StreamEnded{4})));
        CHECK(rejected({19, 0, 0, 0, 0, 0, 0, 0, 0}));       // Short.
        CHECK(rejected({19, 0, 0, 0, 0, 0, 0, 0, 0, 2}));    // Neither.
        CHECK(rejected({18, 0, 0, 0}));       // Short.
        CHECK(rejected({18, 0, 0, 0, 0, 0})); // Long.
        CHECK(rejected({15, 0, 0})); // A result cut short.
        const DataPart dataPart{DataPart::data, 0, 7, 2, 5, crypto::Bytes(64 * 1024, 0xAB)};
        CHECK(std::get<DataPart>(decode(encode(dataPart))) == dataPart);
        const DataDatagram datagram{0, 9, crypto::Bytes(RCS_DATA_MAX_DATAGRAM, 0xCD)};
        CHECK(std::get<DataDatagram>(decode(encode(datagram))) == datagram);
        CHECK(rejected(encode(DataPart{DataPart::data, 0, 7, 5, 5, {1}})));    // Index beyond the count.
        CHECK(rejected(encode(DataPart{DataPart::data, 0, 7, 0, 0, {1}})));    // No parts.
        CHECK(rejected(encode(DataPart{DataPart::data, 0, 7, 0, 1, {}})));     // Empty.
        CHECK(rejected(encode(DataPart{DataPart::data, 0, 7, 0, 1, crypto::Bytes(64 * 1024 + 1)}))); // Too big.
        CHECK(rejected(encode(DataDatagram{0, 9, crypto::Bytes(RCS_DATA_MAX_DATAGRAM + 1)})));
        // A module's messages, its list and its hellos: numbered modules, no channel; only a hello is empty.
        const DataPart modulePart{DataPart::moduleMessage, 3, 0, 0, 2, {5, 6}};
        CHECK(std::get<DataPart>(decode(encode(modulePart))) == modulePart);
        const DataPart hello{DataPart::moduleHello, RCS_MODULES_MAX, 0, 0, 1, {}};
        CHECK(std::get<DataPart>(decode(encode(hello))) == hello);
        CHECK(rejected(encode(DataPart{DataPart::moduleMessage, 0, 0, 0, 1, {1}})));                // No module.
        CHECK(rejected(encode(DataPart{DataPart::moduleMessage, RCS_MODULES_MAX + 1, 0, 0, 1, {1}}))); // None such.
        CHECK(rejected(encode(DataPart{DataPart::moduleMessage, 1, 4, 0, 1, {1}})));                // A channel.
        CHECK(rejected(encode(DataPart{DataPart::moduleMessage, 1, 0, 0, 1, {}})));                 // Empty.
        CHECK(rejected(encode(DataPart{DataPart::moduleList, 1, 0, 0, 1, {1}})));                   // A list's.
        CHECK(rejected(encode(DataPart{DataPart::moduleHello, 1, 0, 0, 2, {}})));                   // Parts.
        CHECK(rejected(encode(DataPart{DataPart::Kind(4), 0, 0, 0, 1, {1}})));                      // No such kind.
        CHECK(std::get<DataDatagram>(decode(encode(DataDatagram{2, 0, {1}}))).module == 2);
        CHECK(rejected(encode(DataDatagram{2, 9, {1}})));                   // A module's has no channel.
        CHECK(rejected(encode(DataDatagram{RCS_MODULES_MAX + 1, 0, {1}})));
        // The list itself.
        const ModuleList list{{{"rcsdk.clipboard", 1, {0, 0, 0, 2, 0, 1, 0, 0}},
                               {"com.example.whiteboard-2", 7, {}},
                               {"x", 0, crypto::Bytes(RCS_MODULE_HELLO_MAX, 'h')}}};
        CHECK(decodeModuleList(encodeModuleList(list)) == list);
        CHECK(decodeModuleList(encodeModuleList({})).modules.empty());
        const auto listRejected = [](const ModuleList& l) {
            return throws<Failure>([&] { decodeModuleList(encodeModuleList(l)); });
        };
        CHECK(listRejected({{{"a", 1, {}}, {"a", 2, {}}}}));                        // Twice.
        CHECK(listRejected({{{"not an id", 1, {}}}}));                               // Not an id.
        CHECK(listRejected({{{"", 1, {}}}}));
        CHECK(listRejected({{{"a", 1, crypto::Bytes(RCS_MODULE_HELLO_MAX + 1)}}})); // A hello too long.
        CHECK(listRejected(ModuleList{std::vector<ModuleList::Entry>(RCS_MODULES_MAX + 1, {"a", 1, {}})}));
        auto trailing = encodeModuleList(list);
        trailing.push_back(0);
        CHECK(throws<Failure>([&] { decodeModuleList(trailing); }));
        CHECK(encodeModuleList(list).size() <= maxModuleListBytes);
        auto bytes = encode(packet());
        CHECK(rejected({}));
        CHECK(rejected({99}));
        CHECK(rejected(crypto::Bytes(bytes.begin(), bytes.end() - 1))); // Truncated.
        auto extra = bytes;
        extra.push_back(0);
        CHECK(rejected(extra)); // Trailing bytes.
        auto badNal = packet();
        badNal.data = {0, 0, 0, 9, 1}; // Length beyond the buffer.
        CHECK(rejected(encode(badNal)));
        auto oddSize = packet();
        oddSize.width = 1919;
        CHECK(rejected(encode(oddSize)));
        // Sizes beyond 4096x2160, as far as the codec takes them, either way round.
        auto sized = packet();
        for (const auto& [width, height, taken] : {std::tuple{4096u, 2304u, true}, std::tuple{2304u, 4096u, true},
                                                   std::tuple{4096u, 2320u, false}, std::tuple{5120u, 2880u, false}}) {
            sized.width = width;
            sized.height = height;
            CHECK(rejected(encode(sized)) != taken); // H.264: 4096 a side and 36 864 macroblocks.
        }
        sized.codec = RCS_CODEC_HEVC;
        sized.config.clear();
        nal::appendUnit(sized.config, std::vector<uint8_t>{0x40, 1}); // VPS, SPS, PPS: the boundaries only.
        nal::appendUnit(sized.config, std::vector<uint8_t>{0x42, 1});
        nal::appendUnit(sized.config, std::vector<uint8_t>{0x44, 1});
        for (const auto& [width, height, taken] : {std::tuple{8192u, 4352u, true}, std::tuple{4352u, 8192u, true},
                                                   std::tuple{8192u, 8192u, false}, std::tuple{8194u, 16u, false}}) {
            sized.width = width;
            sized.height = height;
            CHECK(rejected(encode(sized)) != taken); // 8192 a side and 35 651 584 pixels.
        }
        // A packet carries at most maxVideoBytes of data and config.
        EncodedPacket huge = packet();
        huge.codec = RCS_CODEC_HOST_FIRST;
        huge.config.clear();
        huge.data.assign(maxVideoBytes, 1);
        CHECK(videoProblem(huge).empty());
        huge.data.push_back(1);
        CHECK(videoProblem(huge).find("more data") != std::string::npos);
        auto outside = info;
        outside.content_x = 10; // Content would extend past the frame.
        CHECK(rejected(encode(StreamAnnouncement{outside})));
        // AV1: OBUs, and a config that is exactly one sequence header.
        EncodedPacket av1Packet = packet();
        av1Packet.codec = RCS_CODEC_AV1;
        av1Packet.config = {0x0a, 0x03, 0x00, 0x00, 0x00}; // Sequence header OBU.
        av1Packet.data = {0x32, 0x02, 0xab, 0xcd};         // Frame OBU.
        CHECK(std::get<EncodedPacket>(decode(encode(av1Packet))).data == av1Packet.data);
        auto badAv1 = av1Packet;
        badAv1.config = {0x32, 0x01, 0x00}; // A frame, not a sequence header.
        CHECK(rejected(encode(badAv1)));
        badAv1 = av1Packet;
        badAv1.data = {0x32, 0x05, 0xab}; // OBU size beyond the buffer.
        CHECK(rejected(encode(badAv1)));

        // Host codecs: opaque bytes, no configuration needed; never an unknown id.
        EncodedPacket own = packet();
        own.codec = RCS_CODEC_HOST_FIRST;
        own.config.clear();
        own.data = {1, 2, 3};
        const auto carried = std::get<EncodedPacket>(decode(encode(own)));
        CHECK(carried.codec == RCS_CODEC_HOST_FIRST && carried.data == own.data && carried.config.empty());
        own.codec = 15; // Kept for standard codecs to come.
        CHECK(rejected(encode(own)));
        own.codec = RCS_CODEC_HOST_LAST;
        own.data.clear();
        CHECK(rejected(encode(own))); // No data.
        auto noConfig = packet();
        noConfig.config.clear();
        CHECK(videoProblem(noConfig).find("parameter sets") != std::string::npos && rejected(encode(noConfig)));
        CHECK(videoProblem(packet()).empty());
        CHECK(std::get<CodecOffer>(decode(encode(CodecOffer{{RCS_CODEC_HOST_LAST, RCS_CODEC_H264},
                                                            {{8192, 8192}, {4096, 4096}},
                                                            {defaultPictureFormats, defaultPictureFormats}})))
                  .codecs[0] == RCS_CODEC_HOST_LAST);

        CHECK(rejected(encode(CodecOffer{})));                                 // Empty.
        const std::vector<PictureSize> fullHd{{1920, 1080}};
        const std::vector<uint32_t> one{defaultPictureFormats}, two{defaultPictureFormats, defaultPictureFormats};
        CHECK(rejected(
            encode(CodecOffer{{RCS_CODEC_H264, RCS_CODEC_H264}, {{1920, 1080}, {1920, 1080}}, two}))); // Twice.
        CHECK(rejected(encode(CodecOffer{{9}, fullHd, one}))); // Unknown codec.
        CHECK(rejected(
            encode(CodecOffer{{RCS_CODEC_H264}, fullHd, one, {}, {}, true, false, RCS_PLATFORM_WEB + 1}))); // Platform.
        CHECK(rejected(
            encode(CodecOffer{{RCS_CODEC_H264}, fullHd, one, {}, {}, true, false, RCS_PLATFORM_MACOS, 1u}))); // Input type 0.
        CHECK(rejected(
            encode(CodecOffer{{RCS_CODEC_H264}, fullHd, one, {}, {}, true, false, RCS_PLATFORM_MACOS, 1u << 12}))); // Unknown.
        // The largest picture of each codec: at least 16x16, within what the codec takes.
        CHECK(rejected(encode(CodecOffer{{RCS_CODEC_H264}, {{8192, 4352}}, one}))); // Beyond H.264's 4096 a side.
        CHECK(rejected(encode(CodecOffer{{RCS_CODEC_HEVC}, {{8194, 4352}}, one}))); // Beyond 8192 a side.
        CHECK(rejected(encode(CodecOffer{{RCS_CODEC_AV1}, {{1920, 8}}, one})));     // Below 16.
        // Its picture formats: some, all of them defined.
        CHECK(rejected(encode(CodecOffer{{RCS_CODEC_H264}, fullHd, {0}})));
        CHECK(rejected(encode(CodecOffer{{RCS_CODEC_H264}, fullHd, {1u << (RCS_PICTURE_HDR10_444 + 1)}})));
        CHECK(rejected(encode(CodecOffer{{RCS_CODEC_H264}, fullHd, {1u}}))); // Bit 0 is no format.
        CHECK(throws<Failure>([] { encode(CodecOffer{{RCS_CODEC_H264}}); })); // An offer without them is never sent.
        CHECK(throws<Failure>([&] { encode(CodecOffer{{RCS_CODEC_H264}, fullHd}); }));
        {
            std::vector<rcs_codec> nine{RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_AV1};
            for (rcs_codec codec = RCS_CODEC_HOST_FIRST; nine.size() < RCS_MAX_CODECS + 1; ++codec) {
                nine.push_back(codec);
            }
            CHECK(rejected(encode(CodecOffer{nine, std::vector<PictureSize>(nine.size(), {1920, 1080}),
                                             std::vector<uint32_t>(nine.size(), defaultPictureFormats)})));
        }
        event.type = RCS_INPUT_KEY;
        event.code = 300;
        CHECK(rejected(encode(Input{event, 1})));
        CHECK(!validInput(event));
        // What no version defines, or out of range, is refused.
        const auto invalid = [&](auto change) {
            rcs_input_event e = RCS_INPUT_EVENT_INIT;
            change(e);
            return !validInput(e) && rejected(encode(Input{e, 1}));
        };
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_SECURE_ATTENTION + 1; }));
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_BUTTON, e.code = RCS_BUTTON_FORWARD + 1; }));
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_KEY, e.code = 0x00070004; }));       // Another page.
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_KEY, e.code = RCS_KEY_CONSUMER; })); // Usage 0.
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_TEXT, e.code = 0xD800; }));          // A surrogate.
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_TEXT, e.code = 0x110000; }));
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_TEXT, e.code = 0x1B; })); // Escape.
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_MOVE_RELATIVE, e.dx = 10'001; }));
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_SCROLL, e.wheel_y = -120'001; }));
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_TOUCH, e.code = 10; }));
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_PEN, e.pressure = 1025; }));
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_PEN, e.tilt_y = 91; }));
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_PEN, e.rotation = 360; }));
        CHECK(invalid([](auto& e) { e.type = RCS_INPUT_PEN, e.pen = RCS_PEN_LEAVE << 1; }));

        // The stream announcement carries the display.
        info.display_width = 3840;
        info.display_height = 2160;
        info.display_refresh_hz = 144;
        info.encoder_acceleration = RCS_ACCELERATION_HARDWARE;
        const auto display = std::get<StreamAnnouncement>(decode(encode(StreamAnnouncement{info}))).info;
        CHECK(display.display_width == 3840 && display.display_height == 2160 && display.display_refresh_hz == 144);
        // A stream beyond 4096x2160 as far as its codec takes it.
        auto large = info;
        large.width = 5120;
        large.height = 2880;
        large.content_width = 5120;
        large.content_height = 2820;
        CHECK(rejected(encode(StreamAnnouncement{large}))); // H.264.
        large.codec = RCS_CODEC_HEVC;
        CHECK(std::get<StreamAnnouncement>(decode(encode(StreamAnnouncement{large}))).info.width == 5120);

        // Viewer settings.
        const ViewerSettings settings{
            {1280, 720, 120, 30'000'000, RCS_CODEC_HEVC, RCS_PICTURE_444_8, RCS_AUDIO_PCM, 96'000}, RCS_CURSOR_SEPARATE};
        CHECK(std::get<ViewerSettings>(decode(encode(settings))) == settings);
        CHECK(std::get<ViewerSettings>(decode(encode(ViewerSettings{}))) == ViewerSettings{});
        const ViewerSettings some{{}, RCS_CURSOR_IN_VIDEO, 0b1010};
        CHECK(std::get<ViewerSettings>(decode(encode(some))).streams == 0b1010);
        CHECK(rejected(encode(ViewerSettings{{}, RCS_CURSOR_IN_VIDEO, 1u << RCS_MAX_STREAMS})));
        const ViewerSettings paused{{}, RCS_CURSOR_IN_VIDEO, 0, RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO};
        CHECK(std::get<ViewerSettings>(decode(encode(paused))).paused == (RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO));
        CHECK(rejected(encode(ViewerSettings{{}, RCS_CURSOR_IN_VIDEO, 0, 1u << 2}))); // No such pause.
        CHECK(std::get<SharerPaused>(decode(encode(SharerPaused{RCS_PAUSE_AUDIO}))).paused == RCS_PAUSE_AUDIO);
        CHECK(rejected(encode(SharerPaused{1u << 2})));
        // The short stalls an end sees (stall_watch.hpp): a pace of 0.2..3 s or none.
        CHECK(std::get<PathStalls>(decode(encode(PathStalls{1049, 82, 9}))) == (PathStalls{1049, 82, 9}));
        CHECK(std::get<PathStalls>(decode(encode(PathStalls{}))) == PathStalls{});
        CHECK(rejected(encode(PathStalls{150, 80, 9})) && rejected(encode(PathStalls{3001, 80, 9})));
        CHECK(rejected(encode(PathStalls{1049, 10'001, 9})) && rejected(encode(PathStalls{1049, 80, 65})));
        CHECK(rejected(encode(ViewerSettings{{1280, 0, 0, 0, 0}, 0})));     // Width without height.
        CHECK(rejected(encode(ViewerSettings{{1281, 720, 0, 0, 0}, 0})));   // Odd.
        CHECK(std::get<ViewerSettings>(decode(encode(ViewerSettings{{8192, 4352, 0, 0, 0}, 0}))).request.width ==
              8192); // Any size a stream may be: the sharer fits it to the codec.
        CHECK(rejected(encode(ViewerSettings{{8192, 4354, 0, 0, 0}, 0})));  // More than any stream.
        CHECK(rejected(encode(ViewerSettings{{0, 0, 241, 0, 0}, 0})));      // Frame rate.
        CHECK(rejected(encode(ViewerSettings{{0, 0, 0, 0, 9}, 0})));        // Codec.
        CHECK(rejected(encode(ViewerSettings{{0, 0, 0, 0, 0}, 2})));        // Cursor mode.
        CHECK(rejected(encode(ViewerSettings{{0, 0, 0, 0, 0, 7}, 0})));        // Picture format.
        CHECK(rejected(encode(ViewerSettings{{0, 0, 0, 0, 0, 0, 4}, 0})));     // Audio codec.
        CHECK(rejected(encode(ViewerSettings{{0, 0, 0, 0, 0, 0, 0, 600'000}, 0}))); // Audio bitrate.
        CHECK(std::get<ViewerSettings>(decode(encode(ViewerSettings{{0, 0, 0, 0, 0, RCS_PICTURE_HDR10_444}, 0})))
                  .request.format == RCS_PICTURE_HDR10_444);
        CHECK(std::get<AudioOff>(decode(encode(AudioOff{AudioOff::receiverOff}))).reason == AudioOff::receiverOff);
        CHECK(rejected({12, 3})); // An unknown reason.
        CHECK(std::get<CodecOffer>(decode(encode(CodecOffer{
                                       {RCS_CODEC_H264}, {{1920, 1080}}, {defaultPictureFormats}, {}, {RCS_AUDIO_OPUS}, false, true})))
                  .receivesAudio);

        // Cursor images: packed pixels round trip, in parts.
        std::vector<uint8_t> pixels(64 * 64 * 4, 0);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            const size_t x = i / 4 % 64, y = i / 4 / 64;
            if (x < y) { // A flat triangle (runs) with an antialiased edge (literals).
                pixels[i] = 200;
                pixels[i + 3] = x + 1 == y ? uint8_t(y * 4) : 255;
            }
        }
        const auto packed = packPixels(pixels);
        CHECK(unpackPixels(packed, 64 * 64) == pixels);
        CHECK(packed.size() < pixels.size() / 8);
        CHECK(unpackPixels(packPixels({}), 0).empty());
        CHECK(throws<Failure>([&] { unpackPixels(packed, 64 * 64 - 1); }));
        CHECK(throws<Failure>([&] { unpackPixels(std::span(packed).first(packed.size() - 1), 64 * 64); }));
        CursorShapePart part{3, RCS_CURSOR_TEXT, 64, 64, 10, 20, uint32_t(packed.size()), 0, {}};
        part.bytes.assign(packed.begin(), packed.begin() + ptrdiff_t(std::min(maxCursorPart, packed.size())));
        const auto parsed = std::get<CursorShapePart>(decode(encode(part)));
        CHECK(parsed.id == 3 && parsed.type == RCS_CURSOR_TEXT && parsed.hotspotY == 20 && parsed.bytes == part.bytes);
        auto badPart = part;
        badPart.hotspotX = 64; // Outside the image.
        CHECK(rejected(encode(badPart)));
        badPart = part;
        badPart.width = maxCursorSize + 2;
        CHECK(rejected(encode(badPart)));
        badPart = part;
        badPart.offset = badPart.packedSize; // Past the end.
        CHECK(rejected(encode(badPart)));
        badPart = part;
        badPart.id = 0;
        CHECK(rejected(encode(badPart)));

        const CursorPosition position{true, 2, 65535, 12, 3};
        CHECK(std::get<CursorPosition>(decode(encode(position))) == position);
        CHECK(rejected(encode(CursorPosition{true, 0, 65536, 0, 1})));
        CHECK(rejected(encode(CursorPosition{true, 4, 0, 0, 1}))); // Beyond RCS_MAX_STREAMS.

        // Text into a fixed field is cut at a character's start, never inside one.
        char field[5];
        copyText(field, "abc\xC3\xA9");
        CHECK(std::string(field) == "abc");
        copyText(field, "a\xE2\x82\xAC!"); // A three-byte character that fits stays whole.
        CHECK(std::string(field) == "a\xE2\x82\xAC");
        copyText(field, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80");
        CHECK(std::string(field) == "\xF0\x9F\x98\x80");

        // A pixel a viewer points at, shown one to one, is the pixel input
        // lands on: every column of displays this wide.
        for (const uint32_t width : {1920u, 2560u, 3840u, 5120u}) {
            const auto view = rcsdk::viewGeometry(int(width), 1080, width, 1080, rcsdk::ViewScale::nativePixels);
            rcs_stream_info stream = RCS_STREAM_INFO_INIT;
            stream.width = stream.content_width = width;
            stream.height = stream.content_height = 1080;
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t nx = 0, ny = 0;
                CHECK(view.position(x, 540, width, 1080, nx, ny) && rcsdk::displayPosition(nx, ny, stream));
                CHECK(platform::pixelAt(nx, long(width)) == long(x) && platform::pixelAt(ny, 1080) == 540);
            }
        }
    });
}
