// The system's pointer as GDI has it, for captures that get no pointer from
// Desktop Duplication (a window's): where it is, whether it shows, and its
// image as straight-alpha BGRA.
#pragma once

#include "media/cursor.hpp"
#include "util/api.hpp"

#include <windows.h>

namespace rcsdk::platform::win {

// The standard cursor `cursor` is, by the system's shared cursor handles.
rcs_cursor_type standardType(HCURSOR cursor);

class CursorReader {
public:
    // Reads the pointer now, across `area` (physical pixels on the virtual
    // desktop: what is shared); its image when it changed.
    void poll(const RECT& area);
    const CursorState& state() const noexcept { return state_; }

private:
    CursorState state_;
    HCURSOR shown_ = nullptr;
};

} // namespace rcsdk::platform::win
