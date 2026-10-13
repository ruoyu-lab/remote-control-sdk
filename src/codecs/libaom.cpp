// AV1 encoding in software: libaom (third_party/libaom), real-time mode.
//
// Rate control as libvpx's (libvpx.cpp): CBR without lookahead, no dropped
// pictures, keyframes only when asked for; the screen content tools
// (palette, intra block copy) for DETAIL. AV1 Main for 4:2:0, High for
// 4:4:4, 8 or 10 bits.
// References: the encoder is told each picture's references and refreshes
// (AV1E_SET_SVC_REF_FRAME_CONFIG). Slot 0 holds the latest picture of layer 0
// (LAST); the slots from 1 (with three layers from 2: slot 1 holds the latest
// picture of layer 1) are anchors, pictures of layer 0, one refreshed about
// every four pictures in turn, so that they reach some 24 to 28 pictures
// back. Pictures refer to LAST and the newest anchor.
// Temporal layers (EncoderConfig::temporalLayers) as libvpx's (libvpx.cpp):
// L1T2 alternates layers 0 and 1, L1T3 goes 0, 2, 1, 2 (layer 2 after layer
// 1 refers to slot 1 too), the pattern starting again at each keyframe; the
// layers' bitrates and frame rates are libaom's (AV1E_SET_SVC_PARAMS, which
// also has each OBU say its layer), shared as WebRTC shares them (60 and 40
// percent; 40, 20 and 40 percent). Pictures of the top layer refresh nothing
// and are discardable.
// Reference invalidation: a viewer that lost pictures after `after` gets a
// picture of layer 0 that refers only to LAST, or where LAST is from after
// them the newest anchor from before them, error resilient (nothing of the
// lost pictures' probabilities or motion vectors is used), and says so
// (recoversAfter); the layers' pattern starts again there. Slots from after
// them are not referred to again until refreshed; with nothing from before
// them, a keyframe.
#include "codecs/software_codec.hpp"
#include "media/encoder_output.hpp"

#include "rcsdk_libaom_names.h"
#include <aom/aom_encoder.h>
#include <aom/aomcx.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace rcsdk::codecs::libaom {
namespace {

using platform::CodecImplementation;

constexpr uint32_t formats = (1u << RCS_PICTURE_420_8) | (1u << RCS_PICTURE_420_10) | (1u << RCS_PICTURE_444_8) |
                             (1u << RCS_PICTURE_444_10) | (1u << RCS_PICTURE_HDR10_420) |
                             (1u << RCS_PICTURE_HDR10_444);

std::string implementationName() {
    std::string version = aom_codec_version_str();
    return "libaom AV1 " + (!version.empty() && version[0] == 'v' ? version.substr(1) : version);
}

[[noreturn]] void failed(aom_codec_ctx_t& context, const std::string& what) {
    const char* detail = aom_codec_error_detail(&context);
    fail(RCS_ERROR_CODEC, what + ": " + aom_codec_error(&context) + (detail ? std::string(" (") + detail + ")" : ""));
}

class Encoder final : public SoftwareEncoder {
public:
    ~Encoder() override {
        if (open_) {
            aom_codec_destroy(&context_);
        }
    }

    std::string name() const override { return implementationName(); }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_LIBAOM; }

private:
    // What a reference slot holds, and whether it may be referred to.
    struct Slot {
        uint64_t sequence = 0;
        bool valid = false;
    };
    // aom_svc_ref_frame_config's references: LAST, the one that keeps slot 0
    // refreshed (after a loss LAST may be an anchor), the latest of layer 1,
    // the anchor referred to, and the one an anchor is refreshed through.
    static constexpr int last = 0, last2 = 1, last3 = 2, golden = 3, altref = 6;
    static constexpr unsigned slots = 8, anchorInterval = 4;

    void start(const platform::EncoderConfig& c) override {
        if (c.codec != RCS_CODEC_AV1) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("libaom does not encode ") + codecName(c.codec));
        }
        if (!(formats & (1u << c.picture.format))) {
            fail(RCS_ERROR_UNSUPPORTED,
                 std::string("libaom does not encode AV1 in ") + pictureFormatName(c.picture.format));
        }
        aom_codec_iface_t* iface = aom_codec_av1_cx();
        if (aom_codec_enc_config_default(iface, &settings_, AOM_USAGE_REALTIME) != AOM_CODEC_OK) {
            fail(RCS_ERROR_CODEC, "libaom has no default encoder settings");
        }
        const bool deep = bitDepth(c.picture.format) > 8, full = chroma444(c.picture.format);
        const unsigned threads = codecs::threads(c.softwareThreads);
        settings_.g_w = c.width;
        settings_.g_h = c.height;
        settings_.g_timebase = {1, 1'000'000}; // Microseconds.
        settings_.g_threads = threads;
        settings_.g_lag_in_frames = 0;
        settings_.g_pass = AOM_RC_ONE_PASS;
        settings_.g_profile = full ? 1 : 0;
        settings_.g_bit_depth = deep ? AOM_BITS_10 : AOM_BITS_8;
        settings_.g_input_bit_depth = deep ? 10 : 8;
        settings_.rc_end_usage = AOM_CBR;
        settings_.rc_target_bitrate = std::max(1u, c.bitrate / 1000);
        settings_.rc_dropframe_thresh = 0;
        settings_.rc_resize_mode = 0; // RESIZE_NONE
        settings_.rc_superres_mode = AOM_SUPERRES_NONE;
        settings_.rc_min_quantizer = 2;
        settings_.rc_max_quantizer = 56;
        settings_.rc_undershoot_pct = 50;
        settings_.rc_overshoot_pct = 50;
        settings_.rc_buf_initial_sz = 500;
        settings_.rc_buf_optimal_sz = 600;
        settings_.rc_buf_sz = 1000;
        settings_.kf_mode = AOM_KF_DISABLED;
        settings_.kf_max_dist = ~0u;
        if (aom_codec_enc_init(&context_, iface, &settings_, deep ? AOM_CODEC_USE_HIGHBITDEPTH : 0) !=
            AOM_CODEC_OK) {
            failed(context_, "Opening libaom's AV1 encoder");
        }
        open_ = true;
        const bool screen = c.content == RCS_CONTENT_DETAIL;
        control(AOME_SET_CPUUSED, c.speed == RCS_ENCODER_BEST_QUALITY ? 8 : c.speed == RCS_ENCODER_BALANCED ? 9 : 10);
        // A keyframe may take this many times the average picture's bits
        // (WebRTC's rule: half the optimal buffer's worth, at most).
        control(AOME_SET_MAX_INTRA_BITRATE_PCT, std::max(300u, unsigned(600 * 0.5 * c.framesPerSecond / 10)));
        control(AV1E_SET_TUNE_CONTENT, int(screen ? AOM_CONTENT_SCREEN : AOM_CONTENT_DEFAULT));
        control(AV1E_SET_ENABLE_PALETTE, screen ? 1 : 0);
        control(AV1E_SET_ENABLE_INTRABC, screen ? 1 : 0);
        control(AV1E_SET_AQ_MODE, 3u); // Cyclic refresh, as real-time encoding uses.
        control(AV1E_SET_DELTAQ_MODE, 0u);
        control(AV1E_SET_ENABLE_CDEF, 1);
        control(AV1E_SET_ENABLE_ORDER_HINT, 0);
        control(AV1E_SET_ROW_MT, 1u);
        const int columns = int(std::min(std::floor(std::log2(threads)), std::floor(std::log2(c.width / 256.0))));
        control(AV1E_SET_TILE_COLUMNS, std::max(0, columns));
        control(AV1E_SET_NOISE_SENSITIVITY, 0u);
        const Cicp color = cicpOf(c.picture);
        control(AV1E_SET_COLOR_PRIMARIES, int(color.primaries));
        control(AV1E_SET_TRANSFER_CHARACTERISTICS, int(color.transfer));
        control(AV1E_SET_MATRIX_COEFFICIENTS, int(color.matrix));
        control(AV1E_SET_COLOR_RANGE, int(c.picture.range == RCS_RANGE_FULL ? AOM_CR_FULL_RANGE : AOM_CR_STUDIO_RANGE));
        format_ = deep ? (full ? AOM_IMG_FMT_I44416 : AOM_IMG_FMT_I42016) : (full ? AOM_IMG_FMT_I444 : AOM_IMG_FMT_NV12);
        frameUs_ = 1'000'000 / std::max(1u, c.framesPerSecond);
        output_.emplace(c.codec, c.picture, c.width, c.height, c.framesPerSecond);
        layers_ = std::clamp(c.temporalLayers, 1u, 3u);
        if (layers_ > 1) {
            layerRates(c.bitrate);
        }
    }

    // The layers' shares of `bitrate` and their frame rates (cumulative, as libaom takes them).
    void layerRates(uint32_t bitrate) {
        aom_svc_params_t svc{};
        svc.number_spatial_layers = 1;
        svc.number_temporal_layers = int(layers_);
        svc.scaling_factor_num[0] = svc.scaling_factor_den[0] = 1;
        static constexpr int shares[2][3] = {{60, 100, 0}, {40, 60, 100}}; // Percent, up to each layer.
        for (unsigned layer = 0; layer < layers_; ++layer) {
            svc.max_quantizers[layer] = int(settings_.rc_max_quantizer);
            svc.min_quantizers[layer] = int(settings_.rc_min_quantizer);
            svc.layer_target_bitrate[layer] =
                std::max(1, int(uint64_t(bitrate / 1000) * shares[layers_ - 2][layer] / 100));
            svc.framerate_factor[layer] = 1 << (layers_ - 1 - layer);
        }
        control(AV1E_SET_SVC_PARAMS, &svc);
    }

    template <class T>
    void control(int id, T value) {
        if (aom_codec_control(&context_, id, value) != AOM_CODEC_OK) {
            failed(context_, "libaom's encoder refused control " + std::to_string(id));
        }
    }

    std::optional<Output> encode(const YuvImage& image, const Request& request) override {
        if (request.bitrate) {
            settings_.rc_target_bitrate = std::max(1u, request.bitrate / 1000);
            if (aom_codec_enc_config_set(&context_, &settings_) != AOM_CODEC_OK) {
                log::warning("codec", "libaom refused the bitrate " + std::to_string(request.bitrate) + ": " +
                                          aom_codec_error(&context_));
            } else if (layers_ > 1) {
                layerRates(request.bitrate);
            }
        }
        aom_image_t picture;
        wrap(image, picture);
        aom_enc_frame_flags_t flags = 0;
        std::optional<uint64_t> recovers;
        bool keyframe = request.keyframe || !started_;
        int anchor = newestAnchor(~0ull);
        if (!keyframe && request.invalidateAfter) {
            const uint64_t after = *request.invalidateAfter;
            for (auto& slot : slots_) {
                slot.valid = slot.valid && slot.sequence <= after;
            }
            anchor = newestAnchor(after);
            if (!slots_[0].valid && anchor < 0) {
                keyframe = true; // Nothing from before the lost pictures is left.
            } else {
                flags |= AOM_EFLAG_ERROR_RESILIENT;
                recovers = after;
                index_ = 0; // A picture of layer 0: the pattern starts again.
            }
        }
        const unsigned place = layers_ == 3 ? index_ % 4 : layers_ == 2 ? index_ % 2 : 0;
        const uint32_t layer = keyframe ? 0 : layers_ == 3 ? (place == 0 ? 0 : place == 2 ? 1 : 2) : place;
        // The references (aom_svc_ref_frame_config's reference and ref_idx)
        // and the slots this picture refreshes.
        aom_svc_ref_frame_config_t references{};
        ++sinceAnchor_;
        if (keyframe) {
            flags = AOM_EFLAG_FORCE_KF;
            recovers.reset();
            sinceAnchor_ = 0;
            for (unsigned s = 0; s < slots; ++s) {
                references.refresh[s] = 1;
            }
        } else {
            // LAST: the latest of layer 0, or where it went the anchor alone; GOLDEN: the newest anchor.
            const int previous = slots_[0].valid ? 0 : anchor;
            std::fill(std::begin(references.ref_idx), std::end(references.ref_idx), previous);
            references.reference[last] = 1;
            if (anchor >= 0 && anchor != previous) {
                references.reference[golden] = 1;
                references.ref_idx[golden] = anchor;
            }
            if (layers_ == 3 && place == 3 && slots_[1].valid) {
                references.reference[last3] = 1;
                references.ref_idx[last3] = 1;
            }
            // libaom refreshes a slot through a reference mapped to it.
            if (layer == 0) {
                references.ref_idx[last2] = 0;
                references.refresh[0] = 1;
                if (sinceAnchor_ >= anchorInterval) {
                    sinceAnchor_ = 0;
                    const unsigned first = firstAnchor(), count = slots - first;
                    nextAnchor_ = nextAnchor_ >= first ? first + (nextAnchor_ - first + 1) % count : first;
                    references.ref_idx[altref] = int(nextAnchor_);
                    references.refresh[nextAnchor_] = 1;
                }
            } else if (layer == 1 && layers_ == 3) {
                references.ref_idx[last3] = 1;
                references.refresh[1] = 1;
            }
        }
        if (layers_ > 1) {
            aom_svc_layer_id_t id{0, int(layer)};
            control(AV1E_SET_SVC_LAYER_ID, &id);
        }
        control(AV1E_SET_SVC_REF_FRAME_CONFIG, &references);
        // Pictures at the stream's rate as far as the rate control goes: a
        // still screen sends fewer, never larger ones.
        const aom_codec_pts_t pts = aom_codec_pts_t(timestampUs_);
        timestampUs_ += frameUs_;
        if (aom_codec_encode(&context_, &picture, pts, frameUs_, flags) != AOM_CODEC_OK) {
            failed(context_, "libaom could not encode an AV1 picture");
        }
        stream_.clear();
        bool keyed = false;
        aom_codec_iter_t iter = nullptr;
        while (const aom_codec_cx_pkt_t* packet = aom_codec_get_cx_data(&context_, &iter)) {
            if (packet->kind != AOM_CODEC_CX_FRAME_PKT) {
                continue;
            }
            const auto* bytes = static_cast<const uint8_t*>(packet->data.frame.buf);
            stream_.insert(stream_.end(), bytes, bytes + packet->data.frame.sz);
            keyed |= (packet->data.frame.flags & AOM_FRAME_IS_KEY) != 0;
        }
        if (stream_.empty()) {
            return std::nullopt;
        }
        bool refreshed = false;
        for (unsigned s = 0; s < slots; ++s) {
            if (keyed || references.refresh[s]) {
                slots_[s] = {request.sequence, true};
                refreshed = true;
            }
        }
        started_ = true;
        index_ = keyed ? 1 : index_ + 1;
        EncodedPacket packet;
        output_->split(stream_, packet);
        Output out;
        out.data = std::move(packet.data);
        out.config = std::move(packet.config);
        out.keyframe = keyed;
        if (!keyed) {
            out.recoversAfter = recovers;
            out.temporalId = layer;
            out.discardable = !refreshed;
        }
        return out;
    }

    // Slots from this one on are anchors (with three layers slot 1 is layer 1's).
    unsigned firstAnchor() const noexcept { return layers_ == 3 ? 2 : 1; }

    // The anchor slot holding the newest picture no later than `after`; -1 where none does.
    int newestAnchor(uint64_t after) const noexcept {
        int newest = -1;
        for (unsigned s = firstAnchor(); s < slots; ++s) {
            if (slots_[s].valid && slots_[s].sequence <= after &&
                (newest < 0 || slots_[s].sequence > slots_[newest].sequence)) {
                newest = int(s);
            }
        }
        return newest;
    }

    // `image` as libaom reads it: NV12 as it is, the rest in planes of its own.
    void wrap(const YuvImage& image, aom_image_t& picture) {
        aom_img_wrap(&picture, format_, image.width, image.height, 1, const_cast<uint8_t*>(image.luma));
        if (format_ == AOM_IMG_FMT_NV12) {
            picture.planes[AOM_PLANE_Y] = const_cast<uint8_t*>(image.luma);
            picture.planes[AOM_PLANE_U] = const_cast<uint8_t*>(image.chroma);
            picture.planes[AOM_PLANE_V] = nullptr; // libaom's sign of NV12.
            picture.stride[AOM_PLANE_Y] = int(image.lumaStride);
            picture.stride[AOM_PLANE_U] = int(image.chromaStride);
            picture.stride[AOM_PLANE_V] = 0;
        } else {
            const Planes planes = planar_.from(image, 10);
            for (int i = 0; i < 3; ++i) {
                picture.planes[i] = const_cast<uint8_t*>(planes.plane[i]);
                picture.stride[i] = int(planes.stride[i]);
            }
            picture.bit_depth = planes.bits;
        }
    }

    aom_codec_ctx_t context_{};
    aom_codec_enc_cfg_t settings_{};
    bool open_ = false, started_ = false;
    aom_img_fmt_t format_ = AOM_IMG_FMT_NV12;
    PlanarBuffer planar_;
    std::array<Slot, slots> slots_{};
    unsigned sinceAnchor_ = 0, nextAnchor_ = 0;
    unsigned index_ = 0; // Its place in the layers' pattern: pictures since a keyframe or recovery.
    uint64_t timestampUs_ = 0, frameUs_ = 16'667;
    std::vector<uint8_t> stream_;
    std::optional<EncoderOutput> output_;
};

} // namespace

std::vector<CodecImplementation> implementations() {
    return {libraryImplementation(RCS_CODEC_AV1, RCS_CODEC_ENCODER, RCS_BACKEND_LIBAOM, formats, implementationName(),
                                  true, 3)};
}

std::unique_ptr<platform::VideoEncoder> encoder() {
    return std::make_unique<Encoder>();
}

} // namespace rcsdk::codecs::libaom
