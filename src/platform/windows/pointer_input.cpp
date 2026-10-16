// Touch and pen with InjectSyntheticPointerInput. Each call carries one frame:
// for touch, every contact that is down (the system tracks contacts by their
// ids and wants all of them each time); for the pen, its one pointer.
#include "platform/windows/pointer_input.hpp"

namespace rcsdk::platform::win {
namespace {

// Declared by the headers only for Windows 10 1809 and later: resolved at run time.
using Create = HANDLE(WINAPI*)(POINTER_INPUT_TYPE, ULONG, int);
using Inject = BOOL(WINAPI*)(HANDLE, const POINTER_TYPE_INFO*, UINT32);
using Destroy = void(WINAPI*)(HANDLE);
constexpr int feedbackDefault = 1; // POINTER_FEEDBACK_DEFAULT: the system's touch and pen feedback.

struct Functions {
    Create create = nullptr;
    Inject inject = nullptr;
    Destroy destroy = nullptr;
};

const Functions& functions() {
    static const Functions result = [] {
        Functions f;
        if (HMODULE user = GetModuleHandleW(L"user32.dll")) {
            f.create = reinterpret_cast<Create>(GetProcAddress(user, "CreateSyntheticPointerDevice"));
            f.inject = reinterpret_cast<Inject>(GetProcAddress(user, "InjectSyntheticPointerInput"));
            f.destroy = reinterpret_cast<Destroy>(GetProcAddress(user, "DestroySyntheticPointerDevice"));
        }
        return f;
    }();
    return result;
}

// `point` in screen pixels. InjectSyntheticPointerInput takes them from the
// virtual screen's top left instead (measured: with a display left of the
// main one, every point came out that far to the left).
POINTER_INFO pointerInfo(POINTER_INPUT_TYPE type, UINT32 id, POINT point, POINTER_FLAGS flags) {
    POINTER_INFO info{};
    info.pointerType = type;
    info.pointerId = id;
    info.ptPixelLocation = {point.x - GetSystemMetrics(SM_XVIRTUALSCREEN), point.y - GetSystemMetrics(SM_YVIRTUALSCREEN)};
    info.pointerFlags = flags;
    return info;
}

} // namespace

bool syntheticPointersAvailable() noexcept {
    const auto& f = functions();
    return f.create && f.inject && f.destroy;
}

PointerInjector::~PointerInjector() {
    releaseAll();
    for (HANDLE device : {touch_, pen_}) {
        if (device) {
            functions().destroy(device);
        }
    }
}

bool PointerInjector::inject(HANDLE device, const POINTER_TYPE_INFO* frame, UINT32 count) noexcept {
    return count == 0 || functions().inject(device, frame, count);
}

rcs_result PointerInjector::touch(const rcs_input_event& e, POINT point) {
    if (!syntheticPointersAvailable()) {
        return RCS_ERROR_UNSUPPORTED;
    }
    if (!touch_ && !(touch_ = functions().create(PT_TOUCH, ULONG(contacts_.size()), feedbackDefault))) {
        return RCS_ERROR_PERMISSION_REQUIRED;
    }
    Contact& changed = contacts_[e.code];
    if (!e.down && !changed.down) {
        return RCS_OK; // Lifted already.
    }
    const POINTER_FLAGS flags = !e.down        ? POINTER_FLAG_UP
                                : changed.down ? POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT
                                               : POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    std::array<POINTER_TYPE_INFO, 10> frame{};
    UINT32 count = 0;
    for (UINT32 id = 0; id < contacts_.size(); ++id) {
        const bool self = id == e.code;
        if (!self && !contacts_[id].down) {
            continue;
        }
        const POINT at = self ? point : contacts_[id].point;
        const uint32_t pressure = self ? e.pressure : contacts_[id].pressure;
        POINTER_TYPE_INFO& info = frame[count++];
        info.type = PT_TOUCH;
        info.touchInfo.pointerInfo = pointerInfo(
            PT_TOUCH, id, at, self ? flags : POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
        info.touchInfo.touchMask = TOUCH_MASK_CONTACTAREA | (pressure ? TOUCH_MASK_PRESSURE : 0);
        const POINT injected = info.touchInfo.pointerInfo.ptPixelLocation; // The contact area is in its units too.
        info.touchInfo.rcContact = {injected.x - 2, injected.y - 2, injected.x + 2, injected.y + 2};
        info.touchInfo.pressure = pressure;
    }
    if (!inject(touch_, frame.data(), count)) {
        return RCS_ERROR_PERMISSION_REQUIRED;
    }
    changed = {e.down != 0, point, e.pressure};
    return RCS_OK;
}

POINTER_TYPE_INFO PointerInjector::penFrame(POINTER_FLAGS flags) const {
    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    info.penInfo.pointerInfo = pointerInfo(PT_PEN, 0, penPoint_, flags);
    const bool eraser = penState_.pen & RCS_PEN_ERASER;
    info.penInfo.penFlags = ((penState_.pen & RCS_PEN_BARREL) ? PEN_FLAG_BARREL : 0) |
                            (eraser ? PEN_FLAG_INVERTED : 0) |
                            (eraser && (flags & POINTER_FLAG_INCONTACT) ? PEN_FLAG_ERASER : 0);
    info.penInfo.penMask =
        PEN_MASK_ROTATION | PEN_MASK_TILT_X | PEN_MASK_TILT_Y | (penState_.pressure ? PEN_MASK_PRESSURE : 0);
    info.penInfo.pressure = penState_.pressure;
    info.penInfo.rotation = penState_.rotation;
    info.penInfo.tiltX = penState_.tilt_x;
    info.penInfo.tiltY = penState_.tilt_y;
    return info;
}

rcs_result PointerInjector::pen(const rcs_input_event& e, POINT point) {
    if (!syntheticPointersAvailable()) {
        return RCS_ERROR_UNSUPPORTED;
    }
    if (!pen_ && !(pen_ = functions().create(PT_PEN, 1, feedbackDefault))) {
        return RCS_ERROR_PERMISSION_REQUIRED;
    }
    penState_ = e;
    penPoint_ = point;
    constexpr POINTER_FLAGS hover = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE;
    POINTER_TYPE_INFO frame{};
    // A pen comes into range hovering, touches, lifts and leaves, in that order.
    if (!penInRange_ && !(e.pen & RCS_PEN_LEAVE)) {
        frame = penFrame(hover);
        if (!inject(pen_, &frame, 1)) {
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
        penInRange_ = true;
    }
    const bool down = e.down && !(e.pen & RCS_PEN_LEAVE);
    if (down != penDown_) {
        frame = penFrame(down ? POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT
                              : POINTER_FLAG_UP | POINTER_FLAG_INRANGE);
    } else {
        frame = penFrame(down ? hover | POINTER_FLAG_INCONTACT : hover);
    }
    if (penInRange_ && !inject(pen_, &frame, 1)) {
        return RCS_ERROR_PERMISSION_REQUIRED;
    }
    penDown_ = down;
    if ((e.pen & RCS_PEN_LEAVE) && penInRange_) {
        frame = penFrame(POINTER_FLAG_UPDATE); // Out of range.
        penInRange_ = false;
        if (!inject(pen_, &frame, 1)) {
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
    }
    return RCS_OK;
}

void PointerInjector::releaseAll() noexcept {
    for (UINT32 id = 0; id < contacts_.size(); ++id) {
        if (contacts_[id].down) {
            rcs_input_event lift = RCS_INPUT_EVENT_INIT;
            lift.type = RCS_INPUT_TOUCH;
            lift.code = id;
            touch(lift, contacts_[id].point);
            contacts_[id].down = false;
        }
    }
    if (penInRange_) {
        rcs_input_event leave = RCS_INPUT_EVENT_INIT;
        leave.type = RCS_INPUT_PEN;
        leave.pen = RCS_PEN_LEAVE;
        pen(leave, penPoint_);
        penDown_ = penInRange_ = false;
    }
}

} // namespace rcsdk::platform::win
