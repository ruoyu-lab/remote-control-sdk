// A window of the test's own on the real desktop, for the capture of single
// windows and applications: it changes colour all the time (so captures see
// new pictures), and the test resizes, minimizes, restores and closes it.
// macOS: an NSWindow, worked by the main thread's run loop (pump());
// Windows: a topmost window on a thread of its own with its message loop.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rcsdk::test {

class TestWindow {
public:
    // Its size in logical units (points on macOS, physical pixels on
    // Windows), at `x`, `y` of the main display.
    TestWindow(int x, int y, int width, int height);
    ~TestWindow();
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;

    // rcs_window_info.window_id.
    uint64_t id() const;
    // From now on it shows this colour (sRGB) and no longer changes.
    void setColor(uint8_t red, uint8_t green, uint8_t blue);
    void resize(int width, int height);
    void minimize();
    void restore();
    // Destroys it.
    void close();
    // Windows: presses of the left button it received, and the last one's
    // place in its client area.
    int clicks() const;
    void lastClick(int& x, int& y) const;
    // Windows: other input it received, a line each, in order: "char 00E9"
    // (WM_CHAR, a UTF-16 unit), "key A6" (WM_KEYDOWN, the virtual key),
    // "xbutton 1", "wheel 13" / "hwheel -40" (the delta), "touch down" /
    // "touch up", "pen down 512" (with its pressure) / "pen up".
    std::vector<std::string> received() const;
    // Windows: a system caret of this size at this place of its client area
    // (in its thread's units: physical pixels, or 96-DPI units with
    // RCSDK_TEST_UNAWARE), made on its thread; returns once it is there.
    void showCaret(int x, int y, int width, int height);

    // Lets the window system work for this long (macOS: runs the main run loop).
    static void pump(std::chrono::milliseconds duration);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// A test window of another process (this program started again with
// --child-window), for what a window that closes and an application that
// quits do: a process's own window that it captures stays (macOS keeps it).
class ChildWindow {
public:
    ChildWindow();
    ~ChildWindow();
    ChildWindow(const ChildWindow&) = delete;
    ChildWindow& operator=(const ChildWindow&) = delete;

    uint64_t id() const { return id_; }
    uint32_t process() const { return process_; }
    // Its window closes; the process goes on.
    void close();
    // The process ends.
    void quit();

private:
    void send(const char* command);
    uint64_t id_ = 0;
    uint32_t process_ = 0;
    struct Pipes;
    std::unique_ptr<Pipes> pipes_;
};

// In the child: shows a test window and does what the parent (or a script)
// says on stdin, a line each: "minimize", "restore", "resize W H", "close"
// (its window goes, the process stays); the end of the input ends it.
int runChildWindow();

} // namespace rcsdk::test
