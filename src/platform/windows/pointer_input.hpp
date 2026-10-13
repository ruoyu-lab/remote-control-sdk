// Touch and pen injection through synthetic pointer devices (Windows 10 1809
// or later), loaded at run time so the SDK still starts where they are missing.
#pragma once

#include "util/api.hpp"

#include <windows.h>

#include <array>

namespace rcsdk::platform::win {

// Whether this system injects touch and pen (rcs_capabilities.input_types).
bool syntheticPointersAvailable() noexcept;

// One touch screen of ten contacts and one pen, created when first used.
// Positions are physical screen pixels.
class PointerInjector {
public:
    PointerInjector() = default;
    ~PointerInjector();
    PointerInjector(const PointerInjector&) = delete;
    PointerInjector& operator=(const PointerInjector&) = delete;

    // RCS_INPUT_TOUCH: puts down, moves or lifts contact `event.code`; every
    // other contact that is down goes into the same frame, as the system wants.
    rcs_result touch(const rcs_input_event& event, POINT point);
    // RCS_INPUT_PEN: hovers, touches, lifts or (RCS_PEN_LEAVE) leaves.
    rcs_result pen(const rcs_input_event& event, POINT point);
    // Lifts every contact and takes the pen away.
    void releaseAll() noexcept;
    // Whether contact `code` or the pen is down now (a press that must reach
    // what is shared already did).
    bool touching(uint32_t code) const noexcept { return code < contacts_.size() && contacts_[code].down; }
    bool penTouching() const noexcept { return penDown_; }

private:
    struct Contact {
        bool down = false;
        POINT point{};
        uint32_t pressure = 0;
    };

    bool inject(HANDLE device, const POINTER_TYPE_INFO* frame, UINT32 count) noexcept;
    POINTER_TYPE_INFO penFrame(POINTER_FLAGS flags) const;

    HANDLE touch_ = nullptr, pen_ = nullptr;
    std::array<Contact, 10> contacts_{};
    bool penInRange_ = false, penDown_ = false;
    rcs_input_event penState_{};
    POINT penPoint_{};
};

} // namespace rcsdk::platform::win
