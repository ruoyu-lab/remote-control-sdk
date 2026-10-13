// The stream a sharer sends: its own rcs_share_video_config, or what the
// viewer asked for (rcs_view_request_config) within that configuration's
// limits and within what the codec, the sharer's encoder and the viewer's
// decoder take (their offers). Pure logic; the share loop re-plans whenever
// either end's settings change and restarts the encoder when the plan does.
#pragma once

#include "platform/platform.hpp"
#include "protocol/messages.hpp"
#include "session/config.hpp"

#include <string>

namespace rcsdk::agent {
struct Machine;
}

namespace rcsdk::session {

struct StreamPlan {
    rcs_codec codec = 0; // 0: the offers have no codec with a picture format in common.
    Picture picture;     // The format: the viewer's request or the sharer's first; matrix and range the sharer's.
    uint32_t width = 0, height = 0, framesPerSecond = 0, bitrate = 0;
    // Not empty: the sharer's own size, sent as it is, is more than the codec
    // takes between these ends; why (the stream cannot be sent).
    std::string refusal;
    // Not 0: the frame rate asked for, more than the sharer's encoder takes at
    // this size in this format (encoderPixelRate): framesPerSecond is what it takes.
    uint32_t encoderLimited = 0;
    bool operator==(const StreamPlan&) const = default;
};

// With `machine` (the sharer's agent's): within what its encoder takes a
// second, the frame rate lowered, the size kept.
StreamPlan planStream(const SessionConfig& sharer, const protocol::CodecOffer& sharerOffer,
                      const protocol::CodecOffer& viewerOffer, const protocol::StreamRequest& request,
                      const platform::DisplayMode& display, const agent::Machine* machine = nullptr);

// The policy's limits with their defaults resolved against the display.
struct StreamLimits {
    uint32_t width = 0, height = 0, framesPerSecond = 0, bitrate = 0;
};
StreamLimits streamLimits(const rcs_share_video_config& video, const platform::DisplayMode& display) noexcept;

// Why `codec` cannot carry a picture of width x height from an encoder that
// takes up to `encoder` to a decoder that takes up to `decoder` (0x0: not
// known); empty when it can.
std::string sizeProblem(rcs_codec codec, uint32_t width, uint32_t height, PictureSize encoder, PictureSize decoder);

} // namespace rcsdk::session
