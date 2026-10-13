// Share role: the picture on its way to the viewers. The agent captures and
// encodes it (agent/agent.hpp); the session announces each stream to every
// viewer before any of its packets.
#pragma once

#include "session/session.hpp"
#include "session/stream_plan.hpp"

namespace rcsdk::session {

// What the viewers are about to receive, as the stream announcement and
// rcs_stream_info carry it. `hdr`: the shared display's metadata, carried by
// HDR10 streams only; `layers`: the temporal layers its encoder makes (each
// viewer is told how many of them it gets).
rcs_stream_info streamInfo(const StreamPlan& plan, const rcs_hdr_metadata& hdr, rcs_acceleration acceleration,
                           const Rect& content, const platform::DisplayMode& display, bool cursorInVideo,
                           uint32_t layers);

} // namespace rcsdk::session
