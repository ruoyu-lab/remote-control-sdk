// The text caret module's pieces: a caret on the screen mapped across what is
// shared, and what a report may hold. With RCSDK_TEST_INPUT=1 (Windows) the
// system caret of a test window with the keyboard focus is found where it is,
// across each kind of what is shared; that moves the real pointer (a press
// gives the window the focus): run it where nobody works.
#include "reader.hpp"
#include "support/check.hpp"

#ifdef _WIN32
#include "platform/platform.hpp"
#include "platform/windows/display.hpp"
#include "platform/windows/window.hpp"
#include "support/test_window.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#endif

using namespace rcsdk::text_caret;

namespace {

// Its corners map as a press there would; clipped to it; none outside it.
void within() {
    const ScreenRect area{100, 100, 1100, 600}; // 1000x500 pixels.
    const auto inside = caretWithin({600, 350, 601, 370}, area);
    CHECK(inside.visible && inside.x == 32800 && inside.y == 32833 && inside.width == 66 && inside.height == 2627);
    const auto first = caretWithin({100, 100, 101, 120}, area); // The first pixel is 0.
    CHECK(first.visible && first.x == 0 && first.y == 0);
    const auto edge = caretWithin({1099, 590, 1110, 640}, area); // Clipped at the last pixel.
    CHECK(edge.visible && edge.x == 65535 && edge.width == 0 && edge.y + edge.height == 65535);
    CHECK(!caretWithin({1100, 300, 1101, 320}, area).visible); // Right of it.
    CHECK(!caretWithin({50, 300, 99, 320}, area).visible);
    for (const auto& caret : {inside, first, edge}) {
        CHECK(valid(caret));
    }
    CHECK(!valid({1, 65000, 0, 536, 10})); // Past what is shared.
    CHECK(!valid({1, 0, 65536, 0, 0}));
    CHECK(!valid({0, 0, 0, 1, 0})); // Not visible, yet of a size.
    CHECK(!valid({2, 1, 1, 1, 1}));
    CHECK(valid({}));
}

#ifdef _WIN32
using namespace std::chrono_literals;
namespace platform = rcsdk::platform;

// One look, by a reader of a thread of its own (it keeps UI Automation there),
// across where `target` shows as the module's machine's half is told it.
rcs_text_caret lookForCaret(const platform::CaptureTarget& target) {
    rcs_text_caret caret{};
    const auto area = platform::sharedArea(target);
    if (!area) {
        return caret;
    }
    rcs_module_shared shared = RCS_MODULE_SHARED_INIT;
    shared.source = target.source;
    shared.window_id = target.windowId;
    shared.process_id = area->processId;
    shared.left = area->left;
    shared.top = area->top;
    shared.right = area->right;
    shared.bottom = area->bottom;
    std::thread([&] { caret = createPlatformReader(nullptr)->read(shared); }).join();
    return caret;
}

bool sameCaret(const rcs_text_caret& found, const rcs_text_caret& wanted) {
    const auto close = [](uint32_t a, uint32_t b) {
        return std::abs(int(a) - int(b)) <= 2;
    };
    const bool same = found.visible && wanted.visible && close(found.x, wanted.x) && close(found.y, wanted.y) &&
                      close(found.width, wanted.width) && close(found.height, wanted.height);
    if (!same) {
        std::fprintf(stderr, "caret: found %u %u,%u %ux%u, wanted %u %u,%u %ux%u\n", found.visible, found.x, found.y,
                     found.width, found.height, wanted.visible, wanted.x, wanted.y, wanted.width, wanted.height);
    }
    return same;
}

rcs_result press(platform::InputInjector& injector, bool down) {
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_BUTTON;
    event.code = RCS_BUTTON_LEFT;
    event.down = down;
    event.x = event.y = 32768;
    return injector.apply(event);
}

// The system caret of the window with the keyboard focus, found where it is
// across each kind of what is shared; none where typing goes elsewhere.
void systemCaret() {
    rcsdk::test::TestWindow window(300, 200, 500, 300);
    const HWND hwnd = HWND(uintptr_t(window.id()));
    if (GetForegroundWindow() != hwnd) { // Windows may refuse it the focus: a press gives it.
        auto injector = platform::createInputInjector({RCS_CAPTURE_WINDOW, 0, {}, window.id()});
        CHECK(press(*injector, true) == RCS_OK && press(*injector, false) == RCS_OK);
        rcsdk::test::TestWindow::pump(200ms);
    }
    CHECK(GetForegroundWindow() == hwnd);
    window.showCaret(120, 40, 2, 22);
    rcsdk::test::TestWindow::pump(300ms);
    // Where it is in physical pixels: the client area's origin, and its place
    // in the window's own units scaled as the client area is.
    ScreenRect expected, frame, display;
    {
        const auto previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd));
        RECT own{};
        GetClientRect(hwnd, &own);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        RECT client{};
        POINT origin{0, 0};
        GetClientRect(hwnd, &client);
        ClientToScreen(hwnd, &origin);
        SetThreadDpiAwarenessContext(previous);
        const double scale = double(client.right) / own.right;
        expected = {origin.x + 120 * scale, origin.y + 40 * scale, origin.x + 122 * scale, origin.y + 62 * scale};
    }
    const auto state = platform::win::windowState(hwnd, GetCurrentProcessId());
    const auto primary = platform::win::findMonitor(0);
    CHECK(state && primary);
    frame = {double(state->frame.left), double(state->frame.top), double(state->frame.right),
             double(state->frame.bottom)};
    display = {double(primary->rect.left), double(primary->rect.top), double(primary->rect.right),
               double(primary->rect.bottom)};
    const uint32_t self = GetCurrentProcessId();
    CHECK(sameCaret(lookForCaret({RCS_CAPTURE_WINDOW, 0, {}, window.id()}), caretWithin(expected, frame)));
    CHECK(sameCaret(lookForCaret({RCS_CAPTURE_DISPLAY}), caretWithin(expected, display)));
    CHECK(sameCaret(lookForCaret({RCS_CAPTURE_APPLICATION, 0, {}, 0, self}), caretWithin(expected, display)));
    CHECK(!lookForCaret({RCS_CAPTURE_APPLICATION, 0, {}, 0, self + 4}).visible); // Typing goes to another.
    CHECK(!lookForCaret({RCS_CAPTURE_REGION, 0, {0, 0, 16, 16}}).visible);       // Not in what is shared.
    // It moves: found where it is now.
    window.showCaret(200, 80, 2, 22);
    rcsdk::test::TestWindow::pump(100ms);
    const auto moved = lookForCaret({RCS_CAPTURE_WINDOW, 0, {}, window.id()});
    CHECK(moved.visible && moved.x > caretWithin(expected, frame).x);
}
#endif

} // namespace

int main() {
    return runTest("text_caret.reader", [] {
        within();
#ifdef _WIN32
        if (const char* value = std::getenv("RCSDK_TEST_INPUT"); value && std::string(value) == "1") {
            systemCaret();
        }
#endif
    });
}
