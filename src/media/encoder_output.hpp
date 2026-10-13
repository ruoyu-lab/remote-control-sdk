// What an encoder writes (Annex B; AV1 OBUs; VP8 and VP9 frames), split as
// EncodedPacket carries it: the stream's configuration (parameter sets; the
// AV1 sequence header; 'vpcC', which the SDK writes) apart from the picture.
// Each parameter set an encoder writes replaces the one of its kind; every
// packet carries the whole current set.
#pragma once

#include "media/av1.hpp"
#include "media/h264.hpp"
#include "media/nal.hpp"
#include "media/video.hpp"
#include "media/vpx.hpp"
#include "util/util.hpp"

#include <algorithm>
#include <span>
#include <vector>

namespace rcsdk {

// The configuration of a packet a host gave without one
// (rcs_encoded_packet.config empty): VP8's and VP9's 'vpcC', which the SDK
// writes from the stream (its picture, size and rate); the other standard
// codecs' from the stream's latest keyframe (`latest`), a keyframe's own
// being required. Host codecs carry what their host gives.
inline void fillConfig(EncodedPacket& packet, std::span<const uint8_t> latest, uint32_t framesPerSecond) {
    if (!packet.config.empty() || hostCodec(packet.codec)) {
        return;
    }
    if (vpCodec(packet.codec)) {
        packet.config = vpx::writeConfig(
            vpx::streamConfig(packet.codec, packet.picture, packet.width, packet.height, framesPerSecond));
    } else if (!packet.keyframe) {
        packet.config.assign(latest.begin(), latest.end());
    }
}

class EncoderOutput {
public:
    // VP8 and VP9 streams carry no configuration: the SDK's 'vpcC' for
    // pictures of `picture`, this size and rate goes with every packet.
    EncoderOutput(rcs_codec codec, const Picture& picture, uint32_t width, uint32_t height, uint32_t framesPerSecond)
        : codec_(codec) {
        if (vpCodec(codec)) {
            config_ = vpx::writeConfig(vpx::streamConfig(codec, picture, width, height, framesPerSecond));
        }
    }

    // Configuration an encoder gives apart from its pictures (MediaCodec's
    // codec-config buffers): Annex B parameter sets, or AV1's sequence
    // header, alone or in an AV1CodecConfigurationRecord (four bytes before
    // it, the first 0x81). VP8 and VP9 encoders give none the SDK takes.
    void configuration(std::span<const uint8_t> bytes) {
        if (vpCodec(codec_)) {
            return;
        }
        if (codec_ == RCS_CODEC_AV1) {
            for (const auto& obu : av1::obus(bytes.size() > 4 && bytes[0] == 0x81 ? bytes.subspan(4) : bytes)) {
                if (obu.type == av1::obuSequenceHeader) {
                    config_.assign(obu.bytes.begin(), obu.bytes.end());
                }
            }
            return;
        }
        std::vector<uint8_t> parameterSets;
        for (const auto unit : nal::annexBUnits(bytes)) {
            appendParameterSet(parameterSets, unit);
        }
        config_ = merged(parameterSets);
    }

    // Fills packet.data and packet.config; for H.264, HEVC, VP8 and VP9 also
    // packet.keyframe (an AV1 encoder says that itself). Throws
    // Failure(CODEC) while no configuration has come.
    void split(std::span<const uint8_t> stream, EncodedPacket& packet) {
        if (vpCodec(codec_)) { // A frame (VP9: or a superframe), as it is.
            packet.keyframe = vpx::packetHeader(codec_, stream).keyframe;
            packet.data.assign(stream.begin(), stream.end());
            packet.config = config_;
            return;
        }
        if (codec_ == RCS_CODEC_AV1) {
            // Temporal delimiters and padding are dropped (receivers add
            // delimiters where their decoder wants them).
            for (const auto& obu : av1::obus(stream)) {
                if (obu.type == av1::obuSequenceHeader) {
                    config_.assign(obu.bytes.begin(), obu.bytes.end());
                } else if (obu.type != av1::obuTemporalDelimiter && obu.type != av1::obuPadding) {
                    packet.data.insert(packet.data.end(), obu.bytes.begin(), obu.bytes.end());
                }
            }
            if (config_.empty()) {
                fail(RCS_ERROR_CODEC, "The encoder produced no AV1 sequence header");
            }
            packet.config = config_;
            return;
        }
        std::vector<uint8_t> parameterSets;
        for (const auto unit : nal::annexBUnits(stream)) {
            if (!nal::parameterSet(codec_, unit)) {
                packet.keyframe |= nal::randomAccess(codec_, unit);
                nal::appendUnit(packet.data, unit);
            } else {
                appendParameterSet(parameterSets, unit);
            }
        }
        // Keyframes repeat them all; some encoders repeat some with other
        // pictures too (Intel's H.264 encoder its PPS with every one), which
        // must not drop the others.
        if (!parameterSets.empty()) {
            config_ = merged(parameterSets);
        }
        if (config_.empty()) {
            fail(RCS_ERROR_CODEC, "The encoder produced no parameter sets");
        }
        packet.config = config_;
    }

private:
    void appendParameterSet(std::vector<uint8_t>& sets, std::span<const uint8_t> unit) const {
        if (codec_ == RCS_CODEC_H264 && nal::h264::type(unit) == nal::h264::sps) {
            nal::appendUnit(sets, h264::noReorderingSps(unit)); // Reordering is off; tell the receiver.
        } else {
            nal::appendUnit(sets, unit);
        }
    }

    // The configuration with each unit of `fresh` in place of the one of its
    // NAL type, or after the others where there is none.
    std::vector<uint8_t> merged(const std::vector<uint8_t>& fresh) const {
        const auto type = [&](std::span<const uint8_t> unit) {
            return codec_ == RCS_CODEC_H264 ? nal::h264::type(unit) : nal::hevc::type(unit);
        };
        std::vector<std::vector<uint8_t>> units;
        for (const auto unit : nal::units(config_)) {
            units.emplace_back(unit.begin(), unit.end());
        }
        for (const auto unit : nal::units(fresh)) {
            const auto same = std::find_if(units.begin(), units.end(), [&](const auto& u) { return type(u) == type(unit); });
            if (same != units.end()) {
                same->assign(unit.begin(), unit.end());
            } else {
                units.emplace_back(unit.begin(), unit.end());
            }
        }
        std::vector<uint8_t> result;
        for (const auto& unit : units) {
            nal::appendUnit(result, unit);
        }
        return result;
    }

    rcs_codec codec_;
    std::vector<uint8_t> config_;
};

} // namespace rcsdk
