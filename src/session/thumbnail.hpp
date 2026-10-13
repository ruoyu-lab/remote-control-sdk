// Pictures (rcs_capture_picture; rcsdk.thumbnail takes them where a session
// captures): one picture of what a capture would share, its settings checked
// as a session's capture is, on the machine that captures it.
#pragma once

#include "agent/agent.hpp"
#include "util/api.hpp"

#include <functional>

namespace rcsdk::session {

using ThumbnailTaker = std::function<platform::Thumbnail(const platform::ThumbnailRequest& request)>;

// Validates the arguments (`what`: parseThumbnailCapture; the limits
// 1..8192), checks them against `machine()` as a session's capture is checked
// there (checkCapture), and has `take` take the picture. Throws Failure: the
// refusal, or what `machine` or `take` threw.
platform::Thumbnail takeThumbnail(const rcs_capture_config* what, uint32_t maxWidth, uint32_t maxHeight,
                                  const std::function<agent::Machine()>& machine, const ThumbnailTaker& take);

// This process's machine as far as capture goes: its capture sources, secure
// desktops, displays and screen permission.
agent::Machine captureMachine();

} // namespace rcsdk::session
