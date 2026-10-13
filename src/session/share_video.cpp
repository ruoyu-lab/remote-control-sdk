#include "session/share_video.hpp"

namespace rcsdk::session {

rcs_stream_info streamInfo(const StreamPlan& plan, const rcs_hdr_metadata& hdr, rcs_acceleration acceleration,
                           const Rect& content, const platform::DisplayMode& display, bool cursorInVideo,
                           uint32_t layers) {
    rcs_stream_info info = RCS_STREAM_INFO_INIT;
    info.encodings = 1; // The main one; a stream with more says so.
    info.layers = info.received_layers = layers;
    info.codec = plan.codec;
    info.format = plan.picture.format;
    info.color = colorOf(plan.picture);
    if (rcsdk::hdr(plan.picture.format)) {
        info.hdr = hdr;
    }
    info.width = plan.width;
    info.height = plan.height;
    info.frames_per_second = plan.framesPerSecond;
    info.bitrate_bps = plan.bitrate;
    info.encoder_acceleration = acceleration;
    info.content_x = content.x;
    info.content_y = content.y;
    info.content_width = content.width;
    info.content_height = content.height;
    info.display_width = display.width;
    info.display_height = display.height;
    info.display_refresh_hz = display.refreshHz;
    info.cursor = cursorInVideo ? RCS_CURSOR_IN_VIDEO : RCS_CURSOR_SEPARATE;
    return info;
}

} // namespace rcsdk::session
