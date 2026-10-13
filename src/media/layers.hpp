// Temporal layers as a picture's bitstream says them (rcs_encoded_packet
// temporal_id, discardable): the layer it belongs to, and whether any later
// picture of its layer or a lower one refers to it, as far as the bitstream
// says; the encoder says the rest. Malformed NAL units, OBUs and frame
// headers throw Failure(RCS_ERROR_CODEC).
#pragma once

#include "media/av1.hpp"
#include "media/nal.hpp"
#include "media/vpx.hpp"

#include <algorithm>
#include <optional>

namespace rcsdk {

struct TemporalLayer {
    std::optional<uint32_t> id; // Unset where the bitstream does not say...
    std::optional<bool> discardable;
    bool operator==(const TemporalLayer&) const = default;
};

// H.264: the slices' nal_ref_idc (0 in every one: nothing refers to the
// picture) and the temporal_id of an SVC prefix or extension unit (types 14,
// 20; without one the layer is the encoder's: VideoToolbox, Media Foundation
// and MediaCodec write none). HEVC: nuh_temporal_id_plus1 and the sub-layer
// non-reference types (TRAIL_N, TSA_N, STSA_N, RADL_N, RASL_N and the
// reserved ones up to 14). AV1: the OBU extension's temporal_id (none: layer
// 0); whether others refer to a frame is in its header's refresh flags, which
// are not read: the encoder's. VP9: whether a frame refreshes any reference
// slot (every frame of a superframe); its layer is the encoder's. VP8 and the
// hosts' own formats: the encoder's.
inline TemporalLayer temporalLayer(const EncodedPacket& packet) {
    TemporalLayer layer;
    switch (packet.codec) {
    case RCS_CODEC_H264: {
        bool slices = false, referred = false;
        for (const auto unit : nal::units(packet.data)) {
            const unsigned type = nal::h264::type(unit);
            if ((type == 14 || type == 20) && unit.size() >= 4 && (unit[1] & 0x80)) {
                layer.id = unit[3] >> 5; // nal_unit_header_svc_extension: temporal_id in its third byte.
            } else if (type >= 1 && type <= 5) {
                slices = true;
                referred = referred || (unit[0] & 0x60);
            }
        }
        layer.discardable = slices && !referred;
        return layer;
    }
    case RCS_CODEC_HEVC: {
        bool slices = false, referred = false;
        uint32_t id = 0;
        for (const auto unit : nal::units(packet.data)) {
            if (unit.size() < 2 || !(unit[1] & 7)) {
                fail(RCS_ERROR_CODEC, "Invalid HEVC NAL unit header");
            }
            const unsigned type = nal::hevc::type(unit);
            if (type < 32) { // Video coding layer.
                slices = true;
                referred = referred || type > 14 || type % 2;
                id = std::max(id, uint32_t(unit[1] & 7) - 1);
            }
        }
        layer.id = id;
        layer.discardable = slices && !referred;
        return layer;
    }
    case RCS_CODEC_AV1: {
        uint32_t id = 0;
        for (const auto& obu : av1::obus(packet.data)) {
            if ((obu.bytes[0] & 0x04) && obu.bytes.size() > 1) { // The extension header.
                id = std::max(id, uint32_t(obu.bytes[1] >> 5));
            }
        }
        layer.id = id;
        return layer;
    }
    case RCS_CODEC_VP9: {
        bool refreshes = false;
        for (const auto frame : vpx::vp9Frames(packet.data)) {
            const auto header = vpx::vp9Header(frame);
            refreshes = refreshes || header.keyframe || header.refreshFrameFlags;
        }
        layer.discardable = !refreshes;
        return layer;
    }
    default:
        return layer;
    }
}

// The packet's layer as its bitstream says, for a standard codec, where it
// says (the rest stays as its encoder said: a host's own format keeps all of
// it); whether that differs from what the packet said before (a host's
// encoder or bitstream: worth a line in the log).
inline bool readTemporalLayer(EncodedPacket& packet) {
    if (hostCodec(packet.codec)) {
        return false;
    }
    const auto layer = temporalLayer(packet);
    const bool differed = (layer.id && *layer.id != packet.temporalId) ||
                          (layer.discardable && *layer.discardable != packet.discardable);
    packet.temporalId = layer.id.value_or(packet.temporalId);
    packet.discardable = layer.discardable.value_or(packet.discardable);
    return differed;
}

// The layer of a picture of a two-layer stream whose encoder says none
// (Media Foundation, MediaCodec): the bitstream's, else the upper one for a
// picture nothing refers to (H.264's nal_ref_idc, VP9's refresh flags);
// where the bitstream tells neither (VP8), layer 0.
inline void inferTemporalLayer(EncodedPacket& packet) {
    if (packet.keyframe) {
        return;
    }
    const auto layer = temporalLayer(packet);
    packet.discardable = layer.discardable.value_or(false);
    packet.temporalId = layer.id.value_or(packet.discardable ? 1 : 0);
}

// A picture that goes on after lost ones (EncodedPacket::recoversAfter) in a
// layer above 0 is none for a viewer of the layers below it (VideoToolbox
// makes its long-term reference's refresh in the upper layer): the next
// picture a viewer of fewer layers gets refers to nothing the lost ones were
// either (the encoder dropped them from its references), and says so too,
// down to layer 0. One per encoding, its packets in order.
class LayeredRecovery {
public:
    void apply(EncodedPacket& packet) noexcept {
        if (packet.keyframe) {
            ceiling_ = 0;
        } else if (packet.recoversAfter) {
            after_ = *packet.recoversAfter;
            ceiling_ = packet.temporalId;
        } else if (packet.temporalId < ceiling_) {
            packet.recoversAfter = after_;
            ceiling_ = packet.temporalId;
        }
    }

private:
    uint64_t after_ = 0;
    uint32_t ceiling_ = 0; // The layers below it still wait for theirs; 0: none.
};

// What a host says of a packet's layer (rcs_encoded_packet): empty when it is
// one some packet may say, else why not.
inline std::string temporalLayerProblem(uint32_t temporalId, uint32_t discardable) {
    if (temporalId > 7) {
        return "temporal_id must be 0..7";
    }
    return discardable > 1 ? "discardable must be 0 or 1" : std::string();
}

} // namespace rcsdk
