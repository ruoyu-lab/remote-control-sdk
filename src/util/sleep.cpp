#include "util/util.hpp"

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <thread>
#endif

namespace rcsdk {

#ifdef _WIN32
// sleep_for(1ms) rounds up to the default ~15.6 ms timer tick. A high-resolution
// waitable timer (Windows 10 1803, the minimum this SDK supports) gives
// sub-millisecond waits without raising the host process's global timer resolution.
void sleepPrecise(std::chrono::nanoseconds duration) {
    if (duration <= std::chrono::nanoseconds::zero()) {
        return;
    }
    struct Timer {
        HANDLE handle =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~Timer() {
            if (handle) {
                CloseHandle(handle);
            }
        }
    };
    thread_local Timer timer;
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<LONGLONG>(1, LONGLONG(duration.count() / 100));
    if (!timer.handle || !SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) ||
        WaitForSingleObject(timer.handle, INFINITE) != WAIT_OBJECT_0) {
        fail(RCS_ERROR_INTERNAL, "High-resolution timer failed (requires Windows 10 1803 or later)");
    }
}
#else
void sleepPrecise(std::chrono::nanoseconds duration) {
    std::this_thread::sleep_for(duration);
}
#endif

} // namespace rcsdk
