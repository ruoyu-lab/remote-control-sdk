#include "session/thumbnail.hpp"

#include "seams/seams.hpp"
#include "session/checks.hpp"
#include "session/config.hpp"

namespace rcsdk::session {

platform::Thumbnail takeThumbnail(const rcs_capture_config* what, uint32_t maxWidth, uint32_t maxHeight,
                                  const std::function<agent::Machine()>& machine, const ThumbnailTaker& take) {
    const rcs_capture_config capture = parseThumbnailCapture(what);
    constexpr uint32_t most = platform::maxThumbnailSide;
    if (!maxWidth || !maxHeight || maxWidth > most || maxHeight > most) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "max_width and max_height must be 1..8192");
    }
    if (const auto refusal = checkCapture(capture, machine())) {
        fail(refusal.result, refusal.reason);
    }
    return take({captureTarget(capture), capture.exclude_current_process != 0, capture.secure_desktop != 0, maxWidth,
                 maxHeight});
}

agent::Machine captureMachine() {
    agent::Machine machine;
    machine.capabilities = platform::capabilities();
    seams::applyCapabilities(machine.capabilities);
    machine.displays = platform::displays();
    machine.screenPermission = platform::screenPermission();
    return machine;
}

} // namespace rcsdk::session
