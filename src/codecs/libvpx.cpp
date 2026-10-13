// VP8 and VP9 in software: libvpx (third_party/libvpx), real-time mode.
//
// Encoding: CBR without lookahead, the encoder never drops a picture (the
// pipeline's capture budget paces the stream), keyframes only when asked
// for, error resilience on (a lost picture's probability and context
// updates do not spoil the next), the screen content tools for DETAIL.
// References, set for each picture: LAST holds the latest picture of layer
// 0; GOLDEN and ALTREF are two anchors, pictures of layer 0 refreshed in turn
// about every eight pictures, so that one of them is from 8 to 16 pictures
// back.
// Temporal layers (EncoderConfig::temporalLayers), in a pattern that starts
// again at each keyframe: L1T2 alternates layers 0 and 1; L1T3 goes 0, 2,
// 1, 2, with ALTREF holding the latest picture of layer 1 instead of an
// anchor. A picture refers to LAST, the anchors and (layer 2 after layer 1)
// ALTREF: nothing from before the latest picture of layer 0 but the anchors,
// so that a viewer may take more layers from any picture of layer 0 on.
// Pictures of the top layer refresh nothing and are discardable; libvpx's
// bitstream carries no layer, so the encoder says it (EncodedPacket). VP8's
// rate control is told the layers (ts_number_layers, each picture's layer),
// as WebRTC tells it, with WebRTC's shares of the bitrate (60 and 40
// percent; 40, 20 and 40 percent): without, it overshoots its target several
// times over; VP9's keeps to it as it is.
// Reference invalidation: a viewer that lost pictures after `after` gets a
// picture of layer 0 that refers only to what LAST and the anchors held from
// before them (and says so: recoversAfter), from which the pattern starts
// again; a reference from after them is not referred to again until it is
// refreshed; with none from before them, a keyframe.
#include "codecs/software_codec.hpp"
#include "media/vpx.hpp"

#include "rcsdk_libvpx_names.h"
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>

#include <array>
#include <cmath>

namespace rcsdk::codecs::libvpx {
namespace {

using platform::CodecImplementation;

constexpr uint32_t vp9Formats = (1u << RCS_PICTURE_420_8) | (1u << RCS_PICTURE_420_10) | (1u << RCS_PICTURE_444_8) |
                                (1u << RCS_PICTURE_444_10) | (1u << RCS_PICTURE_HDR10_420) |
                                (1u << RCS_PICTURE_HDR10_444);
constexpr uint32_t vp8Formats = 1u << RCS_PICTURE_420_8;

std::string version() {
    std::string text = vpx_codec_version_str();
    return !text.empty() && text[0] == 'v' ? text.substr(1) : text;
}

std::string implementationName(rcs_codec codec) {
    return std::string("libvpx ") + codecName(codec) + " " + version();
}

[[noreturn]] void failed(vpx_codec_ctx_t& context, const std::string& what) {
    const char* detail = vpx_codec_error_detail(&context);
    fail(RCS_ERROR_CODEC, what + ": " + vpx_codec_error(&context) + (detail ? std::string(" (") + detail + ")" : ""));
}

class Encoder final : public SoftwareEncoder {
public:
    ~Encoder() override {
        if (open_) {
            vpx_codec_destroy(&context_);
        }
    }

    std::string name() const override { return implementationName(config_.codec); }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_LIBVPX; }

private:
    // Which picture a reference buffer holds, and whether it may be referred to.
    struct Buffer {
        uint64_t sequence = 0;
        bool valid = false;
    };
    enum { last, golden, altref };

    void start(const platform::EncoderConfig& c) override {
        const bool vp9 = c.codec == RCS_CODEC_VP9;
        if (!vp9 && c.codec != RCS_CODEC_VP8) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("libvpx does not encode ") + codecName(c.codec));
        }
        if (!((vp9 ? vp9Formats : vp8Formats) & (1u << c.picture.format))) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("libvpx does not encode ") + codecName(c.codec) + " in " +
                                            pictureFormatName(c.picture.format));
        }
        vpx_codec_iface_t* iface = vp9 ? vpx_codec_vp9_cx() : vpx_codec_vp8_cx();
        if (vpx_codec_enc_config_default(iface, &settings_, 0) != VPX_CODEC_OK) {
            fail(RCS_ERROR_CODEC, "libvpx has no default encoder settings");
        }
        const bool deep = bitDepth(c.picture.format) > 8, full = chroma444(c.picture.format);
        const unsigned threads = codecs::threads(c.softwareThreads);
        settings_.g_w = c.width;
        settings_.g_h = c.height;
        settings_.g_timebase = {1, 1'000'000}; // Microseconds.
        settings_.g_threads = threads;
        settings_.g_lag_in_frames = 0;
        settings_.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
        settings_.g_pass = VPX_RC_ONE_PASS;
        settings_.g_profile = vp9 ? (deep ? 2 : 0) + (full ? 1 : 0) : 0;
        settings_.g_bit_depth = deep ? VPX_BITS_10 : VPX_BITS_8;
        settings_.g_input_bit_depth = deep ? 10 : 8;
        settings_.rc_end_usage = VPX_CBR;
        settings_.rc_target_bitrate = std::max(1u, c.bitrate / 1000);
        settings_.rc_dropframe_thresh = 0;
        settings_.rc_resize_allowed = 0;
        // WebRTC's settings: VP8 needs its whole quantizer range and a tight
        // overshoot to come down to a low bitrate at all.
        settings_.rc_min_quantizer = 2;
        settings_.rc_max_quantizer = vp9 ? 56 : 63;
        settings_.rc_undershoot_pct = vp9 ? 50 : 100;
        settings_.rc_overshoot_pct = vp9 ? 50 : 15;
        settings_.rc_buf_initial_sz = 500;
        settings_.rc_buf_optimal_sz = 600;
        settings_.rc_buf_sz = 1000;
        settings_.kf_mode = VPX_KF_DISABLED;
        settings_.kf_max_dist = ~0u;
        layers_ = std::clamp(c.temporalLayers, 1u, 3u);
        if (!vp9 && layers_ > 1) {
            static constexpr unsigned pattern[2][4] = {{0, 1, 0, 1}, {0, 2, 1, 2}};
            settings_.ts_number_layers = layers_;
            settings_.ts_periodicity = layers_ == 2 ? 2 : 4;
            for (unsigned i = 0; i < settings_.ts_periodicity; ++i) {
                settings_.ts_layer_id[i] = pattern[layers_ - 2][i];
            }
            for (unsigned layer = 0; layer < layers_; ++layer) {
                settings_.ts_rate_decimator[layer] = 1u << (layers_ - 1 - layer);
            }
            layerRates(c.bitrate);
        }
        if (vpx_codec_enc_init(&context_, iface, &settings_, deep ? VPX_CODEC_USE_HIGHBITDEPTH : 0) !=
            VPX_CODEC_OK) {
            failed(context_, std::string("Opening libvpx's ") + codecName(c.codec) + " encoder");
        }
        open_ = true;
        const bool screen = c.content == RCS_CONTENT_DETAIL;
        const int speed = c.speed == RCS_ENCODER_BEST_QUALITY ? 7 : c.speed == RCS_ENCODER_BALANCED ? 8 : 9;
        // A keyframe may take this many times the average picture's bits
        // (WebRTC's rule: half the optimal buffer's worth, at most).
        const unsigned intra = std::max(300u, unsigned(600 * 0.5 * c.framesPerSecond / 10));
        control(VP8E_SET_MAX_INTRA_BITRATE_PCT, intra);
        control(VP8E_SET_STATIC_THRESHOLD, 1u);
        if (vp9) {
            control(VP8E_SET_CPUUSED, speed);
            control(VP9E_SET_TUNE_CONTENT, int(screen ? VP9E_CONTENT_SCREEN : VP9E_CONTENT_DEFAULT));
            control(VP9E_SET_AQ_MODE, 3u); // Cyclic refresh, as real-time encoding uses.
            control(VP9E_SET_ROW_MT, 1u);
            const int columns = int(std::min(std::floor(std::log2(threads)), std::floor(std::log2(c.width / 256.0))));
            control(VP9E_SET_TILE_COLUMNS, std::max(0, columns));
            control(VP9E_SET_NOISE_SENSITIVITY, 0u);
            control(VP9E_SET_COLOR_SPACE, int(vpx::colorSpace(c.picture) == vpx::csBt2020 ? VPX_CS_BT_2020
                                              : vpx::colorSpace(c.picture) == vpx::csBt601 ? VPX_CS_BT_601
                                                                                           : VPX_CS_BT_709));
            control(VP9E_SET_COLOR_RANGE, int(c.picture.range == RCS_RANGE_FULL ? VPX_CR_FULL_RANGE
                                                                                  : VPX_CR_STUDIO_RANGE));
        } else {
            control(VP8E_SET_CPUUSED, speed == 9 ? -10 : speed == 8 ? -8 : -6);
            control(VP8E_SET_SCREEN_CONTENT_MODE, screen ? 1u : 0u);
            control(VP8E_SET_NOISE_SENSITIVITY, 0u);
            control(VP8E_SET_TOKEN_PARTITIONS,
                    int(threads >= 8 ? VP8_EIGHT_TOKENPARTITION : threads >= 4 ? VP8_FOUR_TOKENPARTITION
                                                         : threads >= 2 ? VP8_TWO_TOKENPARTITION
                                                                        : VP8_ONE_TOKENPARTITION));
        }
        format_ = deep ? (full ? VPX_IMG_FMT_I44416 : VPX_IMG_FMT_I42016) : (full ? VPX_IMG_FMT_I444 : VPX_IMG_FMT_NV12);
        record_ = vpx::writeConfig(vpx::streamConfig(c.codec, c.picture, c.width, c.height, c.framesPerSecond));
        frameUs_ = 1'000'000 / std::max(1u, c.framesPerSecond);
    }

    // VP8's layers: their shares of `bitrate`, cumulative, in kbit/s.
    void layerRates(uint32_t bitrate) {
        static constexpr unsigned shares[2][3] = {{60, 100, 0}, {40, 60, 100}}; // Percent, up to each layer.
        for (unsigned layer = 0; layer < layers_; ++layer) {
            settings_.ts_target_bitrate[layer] = std::max(1u, bitrate / 1000 * shares[layers_ - 2][layer] / 100);
        }
    }

    template <class T>
    void control(int id, T value) {
        if (vpx_codec_control_(&context_, id, value) != VPX_CODEC_OK) {
            failed(context_, "Setting up libvpx's encoder (control " + std::to_string(id) + ")");
        }
    }

    std::optional<Output> encode(const YuvImage& image, const Request& request) override {
        if (request.bitrate) {
            settings_.rc_target_bitrate = std::max(1u, request.bitrate / 1000);
            if (settings_.ts_number_layers > 1) {
                layerRates(request.bitrate);
            }
            if (vpx_codec_enc_config_set(&context_, &settings_) != VPX_CODEC_OK) {
                log::warning("codec", "libvpx refused the bitrate " + std::to_string(request.bitrate) + ": " +
                                          vpx_codec_error(&context_));
            }
        }
        vpx_image_t picture;
        wrap(image, picture);
        std::optional<uint64_t> recovers;
        bool keyframe = request.keyframe || !started_;
        if (!keyframe && request.invalidateAfter) {
            const uint64_t after = *request.invalidateAfter;
            for (Buffer& buffer : buffers_) {
                buffer.valid = buffer.valid && buffer.sequence <= after;
            }
            if (buffers_[last].valid || buffers_[golden].valid || (layers_ < 3 && buffers_[altref].valid)) {
                recovers = after;
                index_ = 0; // A picture of layer 0: the pattern starts again.
            } else {
                keyframe = true; // Nothing from before the lost pictures is left.
            }
        }
        const unsigned place = layers_ == 3 ? index_ % 4 : layers_ == 2 ? index_ % 2 : 0;
        const uint32_t layer = keyframe ? 0 : layers_ == 3 ? (place == 0 ? 0 : place == 2 ? 1 : 2) : place;
        // The buffers it refers to and refreshes, as (1 << buffer) bits.
        unsigned refers = 0, refreshes = 0;
        ++sinceAnchor_;
        if (keyframe) {
            refreshes = 7;
            sinceAnchor_ = 0;
        } else {
            const bool ownAltref = layers_ == 3; // ALTREF holds layer 1, no anchor.
            refers = (1u << last) | (1u << golden) | (!ownAltref || place == 3 ? 1u << altref : 0);
            for (unsigned buffer = 0; buffer < 3; ++buffer) {
                refers &= buffers_[buffer].valid ? ~0u : ~(1u << buffer);
            }
            if (layer == 0) {
                refreshes = 1u << last;
                // About every eight pictures an anchor takes one of layer 0, in turn.
                if (sinceAnchor_ >= anchorInterval) {
                    sinceAnchor_ = 0;
                    nextIsGolden_ = ownAltref || !nextIsGolden_;
                    refreshes |= 1u << (nextIsGolden_ ? golden : altref);
                }
            } else if (layer == 1 && ownAltref) {
                refreshes = 1u << altref;
            }
        }
        vpx_enc_frame_flags_t flags = 0;
        if (keyframe) {
            flags = VPX_EFLAG_FORCE_KF;
        } else {
            flags |= (refers & (1u << last)) ? 0 : VP8_EFLAG_NO_REF_LAST;
            flags |= (refers & (1u << golden)) ? 0 : VP8_EFLAG_NO_REF_GF;
            flags |= (refers & (1u << altref)) ? 0 : VP8_EFLAG_NO_REF_ARF;
            flags |= (refreshes & (1u << last)) ? 0 : VP8_EFLAG_NO_UPD_LAST;
            flags |= (refreshes & (1u << golden)) ? 0 : VP8_EFLAG_NO_UPD_GF;
            flags |= (refreshes & (1u << altref)) ? 0 : VP8_EFLAG_NO_UPD_ARF;
            flags |= refreshes ? 0 : VP8_EFLAG_NO_UPD_ENTROPY;
        }
        if (settings_.ts_number_layers > 1) {
            control(VP8E_SET_TEMPORAL_LAYER_ID, int(layer));
        }
        // Pictures at the stream's rate as far as the rate control goes: a
        // still screen sends fewer, never larger ones.
        const vpx_codec_pts_t pts = vpx_codec_pts_t(timestampUs_);
        timestampUs_ += frameUs_;
        if (vpx_codec_encode(&context_, &picture, pts, frameUs_, flags, VPX_DL_REALTIME) != VPX_CODEC_OK) {
            failed(context_, std::string("libvpx could not encode a ") + codecName(config_.codec) + " picture");
        }
        Output out;
        vpx_codec_iter_t iter = nullptr;
        while (const vpx_codec_cx_pkt_t* packet = vpx_codec_get_cx_data(&context_, &iter)) {
            if (packet->kind != VPX_CODEC_CX_FRAME_PKT) {
                continue;
            }
            const auto* bytes = static_cast<const uint8_t*>(packet->data.frame.buf);
            out.data.insert(out.data.end(), bytes, bytes + packet->data.frame.sz);
            out.keyframe |= (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
        }
        if (out.data.empty()) {
            return std::nullopt;
        }
        if (out.keyframe) {
            refreshes = 7;
            started_ = true;
            index_ = 0;
        } else {
            out.recoversAfter = recovers;
            out.temporalId = layer;
            out.discardable = !refreshes;
        }
        for (unsigned buffer = 0; buffer < 3; ++buffer) {
            if (refreshes & (1u << buffer)) {
                buffers_[buffer] = {request.sequence, true};
            }
        }
        ++index_;
        out.config = record_;
        return out;
    }

    // `image` as libvpx reads it: NV12 as it is, the rest in planes of its own.
    void wrap(const YuvImage& image, vpx_image_t& picture) {
        vpx_img_wrap(&picture, format_, image.width, image.height, 1, const_cast<uint8_t*>(image.luma));
        if (format_ == VPX_IMG_FMT_NV12) {
            picture.planes[VPX_PLANE_Y] = const_cast<uint8_t*>(image.luma);
            picture.planes[VPX_PLANE_U] = const_cast<uint8_t*>(image.chroma);
            picture.planes[VPX_PLANE_V] = const_cast<uint8_t*>(image.chroma) + 1;
            picture.stride[VPX_PLANE_Y] = int(image.lumaStride);
            picture.stride[VPX_PLANE_U] = picture.stride[VPX_PLANE_V] = int(image.chromaStride);
            return;
        }
        const Planes planes = planar_.from(image, 10);
        for (int i = 0; i < 3; ++i) {
            picture.planes[i] = const_cast<uint8_t*>(planes.plane[i]);
            picture.stride[i] = int(planes.stride[i]);
        }
        picture.bit_depth = planes.bits;
    }

    static constexpr unsigned anchorInterval = 8;
    vpx_codec_ctx_t context_{};
    vpx_codec_enc_cfg_t settings_{};
    bool open_ = false, started_ = false, nextIsGolden_ = false;
    vpx_img_fmt_t format_ = VPX_IMG_FMT_NV12;
    std::vector<uint8_t> record_;
    PlanarBuffer planar_;
    std::array<Buffer, 3> buffers_{}; // LAST, GOLDEN, ALTREF.
    unsigned index_ = 0;              // Its place in the layers' pattern: pictures since a keyframe or recovery.
    unsigned sinceAnchor_ = 0;
    uint64_t timestampUs_ = 0, frameUs_ = 16'667;
};

class Decoder final : public SoftwareDecoder {
public:
    ~Decoder() override { close(); }

    void reset() override {
        close();
        start(config_);
    }
    std::string name() const override { return implementationName(config_.codec); }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_LIBVPX; }

private:
    void start(const platform::DecoderConfig& c) override {
        if (c.codec != RCS_CODEC_VP8 && c.codec != RCS_CODEC_VP9) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("libvpx does not decode ") + codecName(c.codec));
        }
        if (!((c.codec == RCS_CODEC_VP9 ? vp9Formats : vp8Formats) & (1u << c.picture.format))) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("libvpx does not decode ") + codecName(c.codec) + " in " +
                                            pictureFormatName(c.picture.format));
        }
        vpx_codec_dec_cfg_t settings{};
        settings.threads = codecs::threads(c.softwareThreads);
        if (vpx_codec_dec_init(&context_, c.codec == RCS_CODEC_VP9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(),
                               &settings, 0) != VPX_CODEC_OK) {
            failed(context_, std::string("Opening libvpx's ") + codecName(c.codec) + " decoder");
        }
        open_ = true;
    }

    void close() noexcept {
        if (open_) {
            vpx_codec_destroy(&context_);
            open_ = false;
        }
    }

    const YuvImage* decodePicture(const EncodedPacket& packet) override {
        if (vpx_codec_decode(&context_, packet.data.data(), unsigned(packet.data.size()), nullptr, 0) != VPX_CODEC_OK) {
            failed(context_, std::string("libvpx could not decode a ") + codecName(config_.codec) + " picture");
        }
        const vpx_image_t* shown = nullptr;
        vpx_codec_iter_t iter = nullptr;
        while (const vpx_image_t* picture = vpx_codec_get_frame(&context_, &iter)) {
            shown = picture;
        }
        if (!shown) {
            return nullptr;
        }
        const bool deep = shown->fmt & VPX_IMG_FMT_HIGHBITDEPTH;
        const vpx_img_fmt_t planar = vpx_img_fmt_t(shown->fmt & ~VPX_IMG_FMT_HIGHBITDEPTH);
        const rcs_pixel_format layout = yuvLayoutOf(packet.picture.format);
        const bool full = layout == RCS_PIXEL_NV24 || layout == RCS_PIXEL_P410;
        if (deep != (bitDepth(packet.picture.format) > 8) || (full ? planar != VPX_IMG_FMT_I444 : planar != VPX_IMG_FMT_I420)) {
            fail(RCS_ERROR_CODEC, std::string("libvpx decoded another picture format than the stream's (") +
                                      pictureFormatName(packet.picture.format) + ")");
        }
        Planes planes;
        for (int i = 0; i < 3; ++i) {
            planes.plane[i] = shown->planes[i];
            planes.stride[i] = size_t(shown->stride[i]);
        }
        planes.bits = deep ? shown->bit_depth : 8;
        fromPlanar(planes, layout, shown->d_w, shown->d_h, picture_);
        image_ = picture_.view();
        return &image_;
    }

    vpx_codec_ctx_t context_{};
    bool open_ = false;
    YuvBuffer picture_;
    YuvImage image_;
};

} // namespace

std::vector<CodecImplementation> implementations() {
    return {libraryImplementation(RCS_CODEC_VP8, RCS_CODEC_ENCODER, RCS_BACKEND_LIBVPX, vp8Formats,
                                  implementationName(RCS_CODEC_VP8), true, 3),
            libraryImplementation(RCS_CODEC_VP8, RCS_CODEC_DECODER, RCS_BACKEND_LIBVPX, vp8Formats,
                                  implementationName(RCS_CODEC_VP8), false),
            libraryImplementation(RCS_CODEC_VP9, RCS_CODEC_ENCODER, RCS_BACKEND_LIBVPX, vp9Formats,
                                  implementationName(RCS_CODEC_VP9), true, 3),
            libraryImplementation(RCS_CODEC_VP9, RCS_CODEC_DECODER, RCS_BACKEND_LIBVPX, vp9Formats,
                                  implementationName(RCS_CODEC_VP9), false)};
}

std::unique_ptr<platform::VideoEncoder> encoder() {
    return std::make_unique<Encoder>();
}

std::unique_ptr<platform::VideoDecoder> decoder() {
    return std::make_unique<Decoder>();
}

} // namespace rcsdk::codecs::libvpx
