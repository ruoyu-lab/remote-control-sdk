#include "session/stream_plan.hpp"

#include "media/levels.hpp"
#include "session/negotiation.hpp"

#include <algorithm>
#include <utility>

namespace rcsdk::session {
namespace {

constexpr uint32_t maxFrameRate = 240;
constexpr uint32_t defaultFrameRateLimit = 60, defaultBitrateLimit = 200'000'000;

// `limit` within what an encoder or decoder takes; 0x0 leaves it as it is.
PictureLimit within(const PictureLimit& limit, PictureSize largest) noexcept {
    return largest.width ? narrowed(limit, largest.width, largest.height) : limit;
}

} // namespace

StreamLimits streamLimits(const rcs_share_video_config& policy, const platform::DisplayMode& display) noexcept {
    StreamLimits limits;
    const uint32_t width = display.width ? std::min(display.width, maxPicture.width) : maxPicture.width;
    const uint32_t height = display.height ? std::min(display.height, maxPicture.height) : maxPicture.height;
    limits.width = (policy.max_width ? policy.max_width : width) & ~1u;
    limits.height = (policy.max_height ? policy.max_height : height) & ~1u;
    limits.framesPerSecond = policy.max_frames_per_second
                                 ? policy.max_frames_per_second
                                 : std::min(display.refreshHz ? display.refreshHz : defaultFrameRateLimit, maxFrameRate);
    limits.bitrate = policy.max_bitrate_bps ? policy.max_bitrate_bps : defaultBitrateLimit;
    return limits;
}

std::string sizeProblem(rcs_codec codec, uint32_t width, uint32_t height, PictureSize encoder, PictureSize decoder) {
    const std::string size = std::to_string(width) + "x" + std::to_string(height) + " is more than ";
    const auto codecs = codecLimit(codec);
    if (!takes(codecs, width, height)) {
        return size + codecName(codec) + " takes: " + limitText(codecs);
    }
    if (const auto limit = within(codecs, encoder); !takes(limit, width, height)) {
        return size + "the sharer's " + codecName(codec) + " encoder takes: " + limitText(limit);
    }
    if (const auto limit = within(codecs, decoder); !takes(limit, width, height)) {
        return size + "the viewer's " + codecName(codec) + " decoder takes: " + limitText(limit);
    }
    return {};
}

StreamPlan planStream(const SessionConfig& sharer, const protocol::CodecOffer& sharerOffer,
                      const protocol::CodecOffer& viewerOffer, const protocol::StreamRequest& request,
                      const platform::DisplayMode& display, const agent::Machine* machine) {
    const auto& video = sharer.shareVideo;
    StreamPlan plan{negotiatedCodec(sharerOffer, viewerOffer),
                    {},
                    video.width,
                    video.height,
                    video.frames_per_second,
                    video.bitrate_bps,
                    {}};
    if (!plan.codec) {
        return plan;
    }
    const bool follow = video.follow_requests != 0;
    // The picture formats both take in a codec; none where either does not offer it.
    const auto common = [&](rcs_codec codec) {
        return formatsOffered(sharerOffer, codec) & formatsOffered(viewerOffer, codec);
    };
    if (follow && request.codec && common(request.codec)) {
        plan.codec = request.codec;
    } else if (!(follow && request.width)) {
        // The sharer's own size, never made smaller: the first codec both
        // offer that takes it (the host listed every codec it accepts); with
        // none, the first common one, refused below with its reason.
        for (const rcs_codec codec : sharerOffer.codecs) {
            if (common(codec) &&
                sizeProblem(codec, plan.width, plan.height, largestOffered(sharerOffer, codec),
                            largestOffered(viewerOffer, codec))
                    .empty()) {
                plan.codec = codec;
                break;
            }
        }
    }
    // The picture format: the one the viewer asks for, else the sharer's first both take.
    const uint32_t formats = common(plan.codec);
    rcs_picture_format format = 0;
    if (follow && request.format && (formats & (1u << request.format))) {
        format = request.format;
    } else {
        const auto& listed = sharer.encoder.formats;
        const auto first =
            std::find_if(std::begin(listed), std::end(listed), [&](uint32_t f) { return f && (formats & (1u << f)); });
        format = first != std::end(listed) ? *first : 0;
    }
    plan.picture = picture(format, sharer.encoder.matrix, sharer.encoder.range);
    const PictureSize encoder = largestOffered(sharerOffer, plan.codec);
    const PictureSize decoder = largestOffered(viewerOffer, plan.codec);
    const auto limits = streamLimits(video, display);
    if (follow && request.width) { // Scaled down to fit, aspect kept.
        const auto limit =
            within(within(narrowed(codecLimit(plan.codec), limits.width, limits.height), encoder), decoder);
        const auto size = fitPicture(request.width, request.height, limit);
        plan.width = size.width;
        plan.height = size.height;
    } else if (auto problem = sizeProblem(plan.codec, plan.width, plan.height, encoder, decoder); !problem.empty()) {
        plan.refusal = "The sharer's stream (rcs_share_video_config): " + problem + "; the size is never made smaller";
    }
    if (follow && request.framesPerSecond) {
        plan.framesPerSecond = std::min(request.framesPerSecond, limits.framesPerSecond);
    }
    if (follow && request.bitrate) {
        plan.bitrate = std::min(request.bitrate, limits.bitrate);
    }
    if (const uint64_t rate = machine ? encoderPixelRate(sharer, *machine, plan.codec, plan.picture.format) : 0) {
        const auto fits = uint32_t(std::clamp<uint64_t>(rate / levelPixels(plan.codec, plan.width, plan.height), 1,
                                                        plan.framesPerSecond));
        if (fits < plan.framesPerSecond) {
            plan.encoderLimited = std::exchange(plan.framesPerSecond, fits);
        }
    }
    return plan;
}

} // namespace rcsdk::session
