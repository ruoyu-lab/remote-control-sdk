// The system's mouse pointer over what is shared, for capture with the cursor
// sent apart from the picture: where it is (polled at every call) and its
// image (looked at every 50 ms, compared by its pixels).
#pragma once

#include "media/cursor.hpp"

#include <CoreGraphics/CoreGraphics.h>

#include <chrono>
#include <cstdint>

namespace rcsdk::platform::mac {

class CursorTracker {
public:
    // Reads the pointer now across `area` (global points: the display, the
    // region or the window shared), its image at `display`'s scale; call from
    // one thread.
    void poll(CGRect area, CGDirectDisplayID display);
    const CursorState& state() const noexcept { return state_; }

private:
    void readImage(CGDirectDisplayID display);

    CursorState state_;
    uint64_t imageHash_ = 0;
    std::chrono::steady_clock::time_point nextImage_{};
};

} // namespace rcsdk::platform::mac
