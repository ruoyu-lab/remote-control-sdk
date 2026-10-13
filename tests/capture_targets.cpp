// Capture of single windows, applications and regions on the real desktop,
// with windows of the test's own: frames come and the picture follows the
// window's size; minimizing pauses capture (HIDDEN) until it is restored, and
// closing it pauses capture for good (GONE). Thumbnails of the same: their
// size, the test window's colour where it is, refusals where capture would
// pause, while a capture of the display runs, and through an agent attached
// over the link. On Windows a second capture of a display this process
// captures fails, saying why. Skipped (77) where the screen cannot be
// captured (no permission, no display, no such source).
//
// With RCSDK_TEST_INPUT=1 (Windows), remote input goes to a test window too:
// it lands where the picture says, a press on a covered part brings the shared
// window to the front first, and a minimized window takes none; an absolute
// position lands on the pixel it names, across the main display. That moves
// the real pointer: run it where nobody works.
#include "platform/platform.hpp"
#include "support/check.hpp"
#include "support/crash_report.hpp"
#include "support/test_window.hpp"

#include <rcsdk/agent.hpp>
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/thumbnail.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "platform/windows/d3d.hpp"
#include "platform/windows/display.hpp"
#include "platform/windows/window.hpp"

#include <windows.h>
#else
#include "platform/macos/window_server.hpp"

#include <unistd.h>
#endif

using namespace rcsdk;
using namespace std::chrono_literals;

namespace {

uint32_t ownProcess() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return uint32_t(getpid());
#endif
}

// What a capture says at a moment.
struct Look {
    bool started = false;
    uint64_t frames = 0;
    rcs_capture_pause paused = RCS_CAPTURE_PAUSE_NONE;
    rcsdk::Rect content;
    platform::DisplayMode display;
    std::string failure;
};

// Captures on a thread of its own while the main thread works the window system.
class Capturer {
public:
    explicit Capturer(const platform::CaptureTarget& target) {
        thread_ = std::thread([this, target] {
            try {
                auto capture = platform::createScreenCapture(target);
                capture->start({640, 360, 30, target, false, true, false}, cancelled_);
                {
                    std::lock_guard lock(mutex_);
                    look_.started = true;
                }
                while (!cancelled_) {
                    VideoFrame frame;
                    const bool fresh = capture->next(frame, 20ms, cancelled_);
                    std::lock_guard lock(mutex_);
                    look_.frames += fresh;
                    look_.paused = capture->paused();
                    look_.content = capture->content();
                    look_.display = capture->display();
                }
                capture->stop();
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex_);
                look_.failure = error.what();
            }
        });
    }
    ~Capturer() {
        cancelled_ = true;
        thread_.join();
    }

    Look look() {
        std::lock_guard lock(mutex_);
        return look_;
    }

    // Works the window system until `ready` holds (or the capture failed).
    bool waitFor(const std::function<bool(const Look&)>& ready, std::chrono::milliseconds limit = 6s) {
        for (const auto deadline = std::chrono::steady_clock::now() + limit;
             std::chrono::steady_clock::now() < deadline;) {
            const auto now = look();
            if (!now.failure.empty()) {
                std::fprintf(stderr, "capture failed: %s\n", now.failure.c_str());
                return false;
            }
            if (ready(now)) {
                return true;
            }
            test::TestWindow::pump(20ms);
        }
        const auto last = look();
        if (!ready(last)) {
            std::fprintf(stderr, "capture says: started %d, %llu frames, paused %u, %ux%u, content %u,%u %ux%u\n",
                         last.started, (unsigned long long)last.frames, last.paused, last.display.width,
                         last.display.height, last.content.x, last.content.y, last.content.width, last.content.height);
            return false;
        }
        return true;
    }

private:
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    Look look_;
    std::thread thread_;
};

// Where the test is, for a log that ends early.
void step(const char* what) {
    std::fprintf(stderr, "capture_targets: %s\n", what);
    std::fflush(stderr);
}

bool similar(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance * std::max(std::fabs(a), std::fabs(b));
}

// What is shared keeps its aspect ratio inside the 640x360 frame, and fills
// it one way.
bool fitted(const Look& look) {
    const auto& c = look.content;
    return look.display.width && look.display.height && c.width && c.height && c.x + c.width <= 640 &&
           c.y + c.height <= 360 && (c.width >= 636 || c.height >= 356) &&
           similar(double(c.width) / c.height, double(look.display.width) / look.display.height, 0.03);
}

void window() {
    step("window");
    test::TestWindow window(120, 140, 400, 300);
    const auto listed = platform::windows();
    CHECK(std::any_of(listed.begin(), listed.end(), [&](const rcs_window_info& w) {
        return w.window_id == window.id() && w.process_id == ownProcess() &&
               std::string(w.title) == "rcsdk test window" && w.width > 0 && w.display_id;
    }));

    Capturer capture({RCS_CAPTURE_WINDOW, 0, {}, window.id()});
    CHECK(capture.waitFor([](const Look& l) { return l.frames >= 5; }));
    const auto first = capture.look();
    CHECK(first.paused == RCS_CAPTURE_PAUSE_NONE && fitted(first));

    // Wider: the picture follows the window.
    step("window: resize");
    window.resize(800, 300);
    CHECK(capture.waitFor([&](const Look& l) { return l.display.width > first.display.width * 17 / 10 && fitted(l); }));
    const auto wide = capture.look();
    CHECK(std::abs(int(wide.display.height) - int(first.display.height)) <= 2);

    // Minimized: paused until it is restored, then at its own size again.
    step("window: minimize");
    window.minimize();
    CHECK(capture.waitFor([](const Look& l) { return l.paused == RCS_CAPTURE_PAUSE_HIDDEN; }));
    const uint64_t frames = capture.look().frames;
    test::TestWindow::pump(500ms);
    CHECK(capture.look().paused == RCS_CAPTURE_PAUSE_HIDDEN && capture.look().frames <= frames + 1);
    step("window: restore");
    window.restore();
    CHECK(capture.waitFor([&](const Look& l) { return l.paused == RCS_CAPTURE_PAUSE_NONE && l.frames >= frames + 5; }));
    const auto restored = capture.look();
    CHECK(std::abs(int(restored.display.width) - int(wide.display.width)) <= 2 && fitted(restored));
}

// Another process's window, closed: capture pauses for good. (A window of the
// capturing process itself would stay: macOS keeps one it captures.)
void closedWindow() {
    step("closedWindow");
    test::ChildWindow child;
    CHECK(child.id());
    Capturer capture({RCS_CAPTURE_WINDOW, 0, {}, child.id()});
    CHECK(capture.waitFor([](const Look& l) { return l.frames >= 5; }));
    child.close();
    CHECK(capture.waitFor([](const Look& l) { return l.paused == RCS_CAPTURE_PAUSE_GONE; }));
    test::TestWindow::pump(600ms);
    CHECK(capture.look().paused == RCS_CAPTURE_PAUSE_GONE);
}

// A window that is gone before capture starts: capture starts paused, for good.
void goneWindow() {
    step("goneWindow");
    uint64_t id = 0;
    {
        test::ChildWindow child;
        id = child.id();
    }
    Capturer capture({RCS_CAPTURE_WINDOW, 0, {}, id});
    CHECK(capture.waitFor([](const Look& l) { return l.started && l.paused == RCS_CAPTURE_PAUSE_GONE; }));
    // The stream still says where a picture would be: the whole frame.
    const auto content = capture.look().content;
    CHECK(content.x == 0 && content.y == 0 && content.width == 640 && content.height == 360);
}

void application() {
    step("application");
    test::TestWindow window(160, 160, 480, 320);
    Capturer capture({RCS_CAPTURE_APPLICATION, 0, {}, 0, ownProcess()});
    CHECK(capture.waitFor([](const Look& l) { return l.frames >= 5; }));
    const auto first = capture.look();
    CHECK(first.paused == RCS_CAPTURE_PAUSE_NONE && fitted(first));
    const auto displays = platform::displays();
    const auto main = std::find_if(displays.begin(), displays.end(), [](const auto& d) { return d.is_main; });
    CHECK(main != displays.end());
    if (first.display.width != main->pixel_width || first.display.height != main->pixel_height) {
        std::fprintf(stderr, "application on %ux%u, the main display %ux%u\n", first.display.width,
                     first.display.height, main->pixel_width, main->pixel_height);
    }
    CHECK(first.display.width == main->pixel_width && first.display.height == main->pixel_height);

    // Its only window minimized: nothing of it shows.
    window.minimize();
    CHECK(capture.waitFor([](const Look& l) { return l.paused == RCS_CAPTURE_PAUSE_HIDDEN; }));
    const uint64_t frames = capture.look().frames;
    window.restore();
    CHECK(capture.waitFor([&](const Look& l) { return l.paused == RCS_CAPTURE_PAUSE_NONE && l.frames >= frames + 5; }));
}

// Another application: shared while it runs, gone once it quits.
void quitApplication() {
    step("quitApplication");
    test::ChildWindow child;
    CHECK(child.process());
    Capturer capture({RCS_CAPTURE_APPLICATION, 0, {}, 0, child.process()});
    CHECK(capture.waitFor([](const Look& l) { return l.frames >= 5 && l.paused == RCS_CAPTURE_PAUSE_NONE; }));
    child.close(); // No window left: nothing of it shows.
    CHECK(capture.waitFor([](const Look& l) { return l.paused == RCS_CAPTURE_PAUSE_HIDDEN; }));
    child.quit();
    CHECK(capture.waitFor([](const Look& l) { return l.paused == RCS_CAPTURE_PAUSE_GONE; }));
}

void display() {
    step("display");
    test::TestWindow window(60, 60, 400, 300); // Something that changes on it.
    Capturer capture({RCS_CAPTURE_DISPLAY});
    CHECK(capture.waitFor([](const Look& l) { return l.frames >= 5; }));
    CHECK(capture.look().paused == RCS_CAPTURE_PAUSE_NONE && fitted(capture.look()));
    // A display that is captured shows the user's own desktop, someone logged in.
    CHECK((platform::desktopState() == platform::DesktopState{RCS_DESKTOP_USER, true}));
#ifdef _WIN32
    // Windows duplicates a display once per process: a second capture of it
    // fails saying so, and the first goes on.
    Capturer second({RCS_CAPTURE_DISPLAY});
    for (int i = 0; i < 250 && second.look().failure.empty(); ++i) {
        test::TestWindow::pump(20ms);
    }
    const std::string failure = second.look().failure;
    if (failure.find("another part of this process duplicates the display") == std::string::npos) {
        std::fprintf(stderr, "second capture: \"%s\"\n", failure.c_str());
        CHECK(false);
    }
    const uint64_t frames = capture.look().frames;
    window.setColor(40, 200, 80);
    CHECK(capture.waitFor([&](const Look& l) { return l.frames > frames; }));
#endif
}

void region() {
    step("region");
    test::TestWindow window(60, 60, 400, 300); // Something that changes inside the region.
    Capturer capture({RCS_CAPTURE_REGION, 0, {100, 80, 320, 200}});
    CHECK(capture.waitFor([](const Look& l) { return l.frames >= 5; }));
    const auto look = capture.look();
    CHECK(look.paused == RCS_CAPTURE_PAUSE_NONE && look.display.width == 320 && look.display.height == 200);
    CHECK(look.content.x == 32 && look.content.y == 0 && look.content.width == 576 && look.content.height == 360);
}

// ---- Thumbnails ----------------------------------------------------------------

// The test window's colour while thumbnails are taken of it.
constexpr int shownRed = 200, shownGreen = 40, shownBlue = 160;

// A picture as a host takes one (rcs_capture_picture), into a buffer of
// `capacity` bytes (what the limits need, unless given), filled with 0x5A
// beforehand.
struct Shot {
    rcs_result result = RCS_ERROR_INTERNAL;
    std::string reason;
    rcs_picture out = RCS_PICTURE_INIT;
    std::vector<uint8_t> pixels;

    // Red, green and blue at x, y.
    std::array<int, 3> at(uint32_t x, uint32_t y) const {
        const uint8_t* p = pixels.data() + size_t(y) * out.stride_bytes + size_t(x) * 4;
        return {p[2], p[1], p[0]};
    }
    // The test window's colour at x, y, as far as colour management leaves it.
    bool shown(uint32_t x, uint32_t y) const {
        const auto c = at(x, y);
        const bool matches =
            std::abs(c[0] - shownRed) < 40 && std::abs(c[1] - shownGreen) < 40 && std::abs(c[2] - shownBlue) < 40;
        if (!matches) {
            std::fprintf(stderr, "thumbnail %ux%u at %u,%u: %d,%d,%d\n", out.width, out.height, x, y, c[0], c[1], c[2]);
        }
        return matches;
    }
};

Shot shoot(const rcs_capture_config& what, uint32_t maxWidth, uint32_t maxHeight,
           std::optional<size_t> capacity = std::nullopt) {
    Shot shot;
    shot.pixels.assign(capacity.value_or(size_t(maxWidth) * maxHeight * 4), 0x5A);
    shot.out.data = shot.pixels.data();
    shot.out.capacity = shot.pixels.size();
    shot.result = rcs_capture_picture(&what, maxWidth, maxHeight, &shot.out);
    char reason[512] = {};
    rcs_get_last_call_error(reason, sizeof(reason));
    shot.reason = reason;
    if (shot.result != RCS_OK) {
        std::fprintf(stderr, "thumbnail: %s (%s)\n", rcs_result_message(shot.result), reason);
    }
    return shot;
}

rcs_capture_config windowCapture(uint64_t id) {
    rcs_capture_config what = RCS_CAPTURE_CONFIG_INIT;
    what.source = RCS_CAPTURE_WINDOW;
    what.window_id = id;
    return what;
}

// Takes thumbnails while the window system works (a window minimizes or comes
// back) until one's result is `expected`.
Shot shootUntil(const rcs_capture_config& what, rcs_result expected) {
    Shot shot;
    for (int i = 0; i < 100; ++i) {
        shot = shoot(what, 64, 64);
        if (shot.result == expected) {
            break;
        }
        test::TestWindow::pump(50ms);
    }
    return shot;
}

// The middle of the test window's content (one made at 120, 140, 400 x 300),
// in the main display's pixels.
std::pair<uint32_t, uint32_t> middleOfTestWindow() {
#ifdef __APPLE__
    const double scale = platform::mac::pixelsPerPoint(CGMainDisplayID());
#else
    const double scale = 1; // Its place and size are pixels.
#endif
    return {uint32_t((120 + 200) * scale), uint32_t((140 + 150) * scale)};
}

rcs_display_info mainDisplay() {
    const auto displays = platform::displays();
    const auto main = std::find_if(displays.begin(), displays.end(), [](const auto& d) { return d.is_main; });
    CHECK(main != displays.end());
    return *main;
}

void thumbnails() {
    step("thumbnails");
    test::TestWindow window(120, 140, 400, 300);
    window.setColor(shownRed, shownGreen, shownBlue);
    test::TestWindow::pump(500ms);
    const auto main = mainDisplay();
    const auto [middleX, middleY] = middleOfTestWindow();

    // The window alone, fitted into 200 x 200 keeping its aspect ratio.
    const auto listed = platform::windows();
    const auto info = std::find_if(listed.begin(), listed.end(),
                                   [&](const rcs_window_info& w) { return w.window_id == window.id(); });
    CHECK(info != listed.end());
    const auto shot = shoot(windowCapture(window.id()), 200, 200);
    CHECK(shot.result == RCS_OK);
    const auto& out = shot.out;
    CHECK(out.format == RCS_PIXEL_BGRA8 && out.stride_bytes == out.width * 4 &&
          out.data_size == uint64_t(out.stride_bytes) * out.height);
    CHECK(out.width <= 200 && out.height <= 200 && (out.width == 200 || out.height == 200));
    CHECK(similar(double(out.width) / out.height, double(info->width) / info->height, 0.03));
    CHECK(shot.shown(out.width / 2, out.height * 3 / 5)); // Below the title bar.

    // The main display, fitted the same way, the window where it is.
    rcs_capture_config what = RCS_CAPTURE_CONFIG_INIT;
    const auto display = shoot(what, 320, 320);
    const auto [fittedWidth, fittedHeight] = platform::thumbnailSize(main.pixel_width, main.pixel_height, 320, 320);
    CHECK(display.result == RCS_OK && display.out.width == fittedWidth && display.out.height == fittedHeight);
    const uint32_t x = middleX * fittedWidth / main.pixel_width, y = middleY * fittedHeight / main.pixel_height;
    CHECK(display.shown(x, y));

    // A region inside the window: never enlarged.
    what.source = RCS_CAPTURE_REGION;
    what.region_x = middleX - 80;
    what.region_y = middleY - 50;
    what.region_width = 160;
    what.region_height = 100;
    const auto region = shoot(what, 1000, 1000);
    CHECK(region.result == RCS_OK && region.out.width == 160 && region.out.height == 100);
    CHECK(region.shown(4, 4) && region.shown(80, 50) && region.shown(155, 95));

    // The application: its window where it is, black where it has none.
    what = RCS_CAPTURE_CONFIG_INIT;
    what.source = RCS_CAPTURE_APPLICATION;
    what.process_id = ownProcess();
    const auto application = shoot(what, 320, 320);
    CHECK(application.result == RCS_OK && application.out.width == fittedWidth &&
          application.out.height == fittedHeight);
    CHECK(application.shown(x, y));
    const auto corner = application.at(fittedWidth - 2, fittedHeight - 2);
    CHECK(corner[0] < 24 && corner[1] < 24 && corner[2] < 24);

    // A buffer too small: the size it takes, and nothing written.
    const auto cramped = shoot(windowCapture(window.id()), 200, 200, size_t(out.data_size - 1));
    CHECK(cramped.result == RCS_ERROR_BUFFER_TOO_SMALL && cramped.out.data_size == out.data_size &&
          cramped.out.width == out.width && cramped.out.height == out.height);
    CHECK(std::all_of(cramped.pixels.begin(), cramped.pixels.end(), [](uint8_t b) { return b == 0x5A; }));

    // Minimized: where capture would pause, there is no picture, and the reason says why.
    step("thumbnails: minimized");
    window.minimize();
    const auto minimized = shootUntil(windowCapture(window.id()), RCS_ERROR_INVALID_STATE);
    CHECK(minimized.result == RCS_ERROR_INVALID_STATE && !minimized.reason.empty());
    const auto hidden = shootUntil(what, RCS_ERROR_INVALID_STATE); // The application has no window showing.
    CHECK(hidden.result == RCS_ERROR_INVALID_STATE && !hidden.reason.empty());
    window.restore();
    CHECK(shootUntil(windowCapture(window.id()), RCS_OK).result == RCS_OK);

    // Gone: another process's window, closed.
    step("thumbnails: gone");
    uint64_t gone = 0;
    {
        test::ChildWindow child;
        gone = child.id();
        CHECK(shoot(windowCapture(gone), 64, 64).result == RCS_OK);
    }
    const auto closed = shoot(windowCapture(gone), 64, 64);
    CHECK(closed.result == RCS_ERROR_INVALID_STATE && closed.reason.find("gone") != std::string::npos);
}

// A thumbnail of a display that a capture of this process shares (Windows
// lets a process duplicate a display once at a time: that capture takes it,
// also from a still desktop); the capture goes on.
void thumbnailWhileCapturing() {
    step("thumbnailWhileCapturing");
    test::TestWindow window(120, 140, 400, 300);
    window.setColor(shownRed, shownGreen, shownBlue);
    Capturer capture({RCS_CAPTURE_DISPLAY});
    CHECK(capture.waitFor([](const Look& l) { return l.frames >= 1; }));
    test::TestWindow::pump(500ms); // Nothing changes now.
    const auto main = mainDisplay();
    const auto [middleX, middleY] = middleOfTestWindow();
    const auto [fittedWidth, fittedHeight] = platform::thumbnailSize(main.pixel_width, main.pixel_height, 320, 320);
    const rcs_capture_config display = RCS_CAPTURE_CONFIG_INIT;
    for (int i = 0; i < 3; ++i) {
        const auto shot = shoot(display, 320, 320);
        CHECK(shot.result == RCS_OK && shot.out.width == fittedWidth);
        CHECK(shot.shown(middleX * fittedWidth / main.pixel_width, middleY * fittedHeight / main.pixel_height));
    }
    const uint64_t frames = capture.look().frames;
    window.setColor(40, 200, 80);
    CHECK(capture.waitFor([&](const Look& l) { return l.frames > frames && l.paused == RCS_CAPTURE_PAUSE_NONE; }));
}

// A thumbnail through the session's module (rcs_thumbnail_capture), once its
// machine's half runs where the agent is.
rcsdk::CapturedPicture moduleThumbnail(rcsdk::Session& session, const rcs_capture_config& what, uint32_t maxWidth,
                               uint32_t maxHeight) {
    const auto take = [&](const rcs_capture_config& alone, rcs_picture& out) {
        return rcs_thumbnail_capture(session.handle(), &alone, maxWidth, maxHeight, &out);
    };
    for (int i = 0; i < 100; ++i) {
        try {
            return rcsdk::detail::picture(what, maxWidth, maxHeight, take);
        } catch (const rcsdk::Error& error) {
            if (std::string(error.what()).find("does not run on the machine") == std::string::npos) {
                throw;
            }
            test::TestWindow::pump(20ms);
        }
    }
    return rcsdk::detail::picture(what, maxWidth, maxHeight, take);
}

// Through a session whose agent attaches over the link: the agent takes it.
void agentThumbnail() {
    step("agentThumbnail");
    test::TestWindow window(120, 140, 400, 300);
    window.setColor(shownRed, shownGreen, shownBlue);
    test::TestWindow::pump(300ms);
#ifdef _WIN32
    const std::string endpoint = "rcsdk-test-thumbnail-" + std::to_string(GetCurrentProcessId());
#else
    const std::string endpoint = "/tmp/rcsdk-test-thumbnail-" + std::to_string(getpid()) + ".sock";
#endif
    const auto linkKey = rcsdk::generateConnectionKey();
    rcsdk::SessionConfig config(RCS_ROLE_SHARE);
    config.setAddress("127.0.0.1", 47970).setKey(rcsdk::generateConnectionKey()).use(rcsdk::agentLink(endpoint, linkKey));
    config.useModule(RCS_THUMBNAIL_MODULE, nullptr, 0);
    rcsdk::Session share(config);
    CHECK(share.start() == RCS_OK);
    rcsdk::Agent agent(rcsdk::AgentConfig(endpoint, linkKey));
    CHECK(agent.start() == RCS_OK);
    for (int i = 0; i < 500 && share.state() != RCS_STATE_LISTENING; ++i) {
        test::TestWindow::pump(20ms);
    }
    CHECK(share.state() == RCS_STATE_LISTENING);
    const auto what = windowCapture(window.id());
    const auto picture = moduleThumbnail(share, what, 200, 200);
    CHECK(picture.width <= 200 && picture.height <= 200 && (picture.width == 200 || picture.height == 200));
    const uint8_t* p = picture.pixels.data() + (size_t(picture.height) * 3 / 5 * picture.width + picture.width / 2) * 4;
    CHECK(std::abs(p[2] - shownRed) < 40 && std::abs(p[1] - shownGreen) < 40 && std::abs(p[0] - shownBlue) < 40);
    // What the agent cannot take comes back with why.
    window.minimize();
    std::string why;
    for (int i = 0; i < 100 && why.empty(); ++i) {
        try {
            moduleThumbnail(share, what, 64, 64);
            test::TestWindow::pump(50ms);
        } catch (const rcsdk::Error& error) {
            CHECK(error.result() == RCS_ERROR_INVALID_STATE);
            why = error.what();
        }
    }
    CHECK(!why.empty());
    window.restore();
    CHECK(agent.stop() == RCS_OK && share.stop() == RCS_OK);
}

#ifdef __APPLE__
// Remote input into a window of the sharing process itself, covered by another
// of its windows: input arrives on a thread of its own, and the shared window
// comes to the front on the main thread, the only one AppKit works on (raised
// through Accessibility it was raised on the input thread, and AppKit ended the
// process). Posting the press itself needs Accessibility; reaching the window
// does not.
void ownWindow() {
    step("ownWindow");
    test::TestWindow shared(100, 140, 420, 320);
    test::TestWindow cover(170, 200, 420, 320); // Made later: in front, over the shared one's middle.
    auto injector = platform::createInputInjector({RCS_CAPTURE_WINDOW, 0, {}, shared.id()});
    rcs_input_event press = RCS_INPUT_EVENT_INIT;
    press.type = RCS_INPUT_BUTTON;
    press.code = RCS_BUTTON_LEFT;
    press.x = press.y = 32768;
    std::atomic<bool> done{false};
    rcs_result result = RCS_ERROR_INTERNAL;
    std::thread input([&] {
        press.down = 1;
        result = injector->apply(press);
        press.down = 0;
        injector->apply(press);
        done = true;
    });
    while (!done) {
        test::TestWindow::pump(10ms); // The main thread runs its loop, as a host's does.
    }
    input.join();
    CHECK(result == RCS_OK);
    const auto state = platform::mac::windowState(CGWindowID(shared.id()));
    CHECK(state.has_value());
    const CGPoint middle{CGRectGetMidX(state->bounds), CGRectGetMidY(state->bounds)};
    const auto front = platform::mac::windowAt(middle, getpid());
    CHECK(front && front->window == CGWindowID(shared.id()));
}
#endif

#ifdef _WIN32
// A press at the middle of the shared window, as a viewer sends it.
rcs_result press(platform::InputInjector& injector, bool down) {
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_BUTTON;
    event.code = RCS_BUTTON_LEFT;
    event.down = down;
    event.x = event.y = 32768;
    return injector.apply(event);
}

void input() {
    step("input");
    // Where the child's window (another process) will cover its middle.
    test::TestWindow shared(650, 100, 500, 400);
    auto injector = platform::createInputInjector({RCS_CAPTURE_WINDOW, 0, {}, shared.id()});
    CHECK(press(*injector, true) == RCS_OK && press(*injector, false) == RCS_OK);
    CHECK([&] {
        for (int i = 0; i < 50 && shared.clicks() < 1; ++i) {
            test::TestWindow::pump(20ms);
        }
        return shared.clicks() == 1;
    }());
    int x = 0, y = 0;
    shared.lastClick(x, y);
    RECT client{};
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GetClientRect(HWND(uintptr_t(shared.id())), &client); // Physical pixels, as the window has its clicks.
    SetThreadDpiAwarenessContext(previous);
    if (std::abs(x - client.right / 2) >= 30) {
        std::fprintf(stderr, "click at %d,%d of a client area %ldx%ld\n", x, y, client.right, client.bottom);
    }
    CHECK(std::abs(x - client.right / 2) < 30); // The frame's middle: the client area's, across.

    // Covered there by another application's window: the shared one comes
    // to the front first, and takes the press.
    test::ChildWindow cover;
    test::TestWindow::pump(300ms);
    POINT middle{900, 300};
    {
        const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        RECT frame{};
        GetWindowRect(HWND(uintptr_t(shared.id())), &frame);
        middle = {(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        CHECK(GetAncestor(WindowFromPoint(middle), GA_ROOT) == HWND(uintptr_t(cover.id()))); // It is covered.
        SetThreadDpiAwarenessContext(previous);
    }
    CHECK(press(*injector, true) == RCS_OK && press(*injector, false) == RCS_OK);
    CHECK([&] {
        for (int i = 0; i < 50 && shared.clicks() < 2; ++i) {
            test::TestWindow::pump(20ms);
        }
        return shared.clicks() == 2;
    }());

    // Text, media keys, relative moves, touch, pen and smooth scrolling, into the shared window (in front now).
    const auto send = [&](rcs_input_type type, auto fill) {
        rcs_input_event event = RCS_INPUT_EVENT_INIT;
        event.type = type;
        event.x = event.y = 32768;
        fill(event);
        return injector->apply(event);
    };
    const auto received = [&](const std::string& line) {
        for (int i = 0; i < 50; ++i) {
            const auto lines = shared.received();
            if (std::find(lines.begin(), lines.end(), line) != lines.end()) {
                return true;
            }
            test::TestWindow::pump(20ms);
        }
        const auto lines = shared.received();
        std::fprintf(stderr, "no \"%s\" among %zu lines:", line.c_str(), lines.size());
        for (const auto& l : lines) {
            std::fprintf(stderr, " [%s]", l.c_str());
        }
        std::fprintf(stderr, "\n");
        return false;
    };
    // Text, whatever the layout: a Latin letter and an emoji (a surrogate pair).
    CHECK(send(RCS_INPUT_TEXT, [](auto& e) { e.code = 0xE9; }) == RCS_OK && received("char 00E9"));
    CHECK(send(RCS_INPUT_TEXT, [](auto& e) { e.code = 0x1F600; }) == RCS_OK && received("char D83D") &&
          received("char DE00"));
    // The back button, and the media key Back.
    CHECK(send(RCS_INPUT_BUTTON, [](auto& e) { e.code = RCS_BUTTON_BACK, e.down = 1; }) == RCS_OK);
    CHECK(send(RCS_INPUT_BUTTON, [](auto& e) { e.code = RCS_BUTTON_BACK; }) == RCS_OK && received("xbutton 1"));
    CHECK(send(RCS_INPUT_KEY, [](auto& e) { e.code = RCS_KEY_CONSUMER | 0x224, e.down = 1; }) == RCS_OK);
    CHECK(send(RCS_INPUT_KEY, [](auto& e) { e.code = RCS_KEY_CONSUMER | 0x224; }) == RCS_OK && received("key A6"));
    // A third of a line: a third of the wheel units a line takes.
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    CHECK(send(RCS_INPUT_SCROLL, [](auto& e) { e.wheel_y = 40; }) == RCS_OK);
    if (lines && lines != WHEEL_PAGESCROLL) {
        CHECK(received("wheel " + std::to_string(40 / int(lines))));
    }
    // Relative motion moves the pointer by as much, inside the window.
    POINT before{}, after{};
    {
        const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        GetCursorPos(&before);
        CHECK(send(RCS_INPUT_MOVE_RELATIVE, [](auto& e) { e.x = e.y = 0, e.dx = 10, e.dy = -5; }) == RCS_OK);
        GetCursorPos(&after);
        SetThreadDpiAwarenessContext(previous);
    }
    CHECK(after.x == before.x + 10 && after.y == before.y - 5);
    // Touch and pen, where this Windows has synthetic pointers.
    if (platform::capabilities().input_types & (1u << RCS_INPUT_TOUCH)) {
        CHECK(send(RCS_INPUT_TOUCH, [](auto& e) { e.down = 1, e.pressure = 300; }) == RCS_OK && received("touch down"));
        CHECK(send(RCS_INPUT_TOUCH, [](auto& e) { e.down = 1, e.x = 33000; }) == RCS_OK);
        CHECK(send(RCS_INPUT_TOUCH, [](auto& e) { e.down = 0; }) == RCS_OK && received("touch up"));
        CHECK(send(RCS_INPUT_PEN, [](auto& e) { e.down = 0; }) == RCS_OK); // Hovering.
        CHECK(send(RCS_INPUT_PEN, [](auto& e) { e.down = 1, e.pressure = 512; }) == RCS_OK && received("pen down 512"));
        CHECK(send(RCS_INPUT_PEN, [](auto& e) { e.down = 0, e.pen = RCS_PEN_LEAVE; }) == RCS_OK && received("pen up"));
    }

    // Minimized: the window takes no input.
    shared.minimize();
    test::TestWindow::pump(500ms);
    CHECK(press(*injector, true) == RCS_ERROR_INVALID_STATE);
    injector->releaseAll();
}

// An absolute position lands on the pixel it names, as a viewer that shows
// the main display one to one names it: every column and every row.
void pixels() {
    step("pixels");
    auto injector = platform::createInputInjector({RCS_CAPTURE_DISPLAY});
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    MONITORINFO primary{};
    primary.cbSize = sizeof(primary);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &primary);
    const RECT& r = primary.rcMonitor;
    const long width = r.right - r.left, height = r.bottom - r.top;
    int missed = 0;
    const auto lands = [&](long x, long y) {
        rcs_input_event move = RCS_INPUT_EVENT_INIT;
        move.type = RCS_INPUT_MOVE;
        move.x = uint32_t(std::lround(x * 65535.0 / (width - 1)));
        move.y = uint32_t(std::lround(y * 65535.0 / (height - 1)));
        POINT at{};
        const auto there = [&] {
            return GetCursorPos(&at) && at.x == r.left + x && at.y == r.top + y;
        };
        bool landed = injector->apply(move) == RCS_OK && there();
        if (!landed) {
            Sleep(2); // Input may take a moment.
            landed = there();
        }
        if (!landed && ++missed <= 5) {
            std::fprintf(stderr, "pixels: %ld,%ld landed at %ld,%ld\n", x, y, at.x - r.left, at.y - r.top);
        }
    };
    for (long x = 0; x < width; ++x) {
        lands(x, height / 2);
    }
    for (long y = 0; y < height; ++y) {
        lands(width / 2, y);
    }
    SetThreadDpiAwarenessContext(previous);
    std::fprintf(stderr, "pixels: %ldx%ld, %d of %ld missed\n", width, height, missed, width + height);
    CHECK(missed == 0);
}

#endif

} // namespace

int main(int argc, char** argv) {
    test::reportCrashes();
    if (argc > 1 && std::string(argv[1]) == "--child-window") {
        return test::runChildWindow();
    }
    const auto capabilities = platform::capabilities();
    if (platform::screenPermission() != RCS_PERMISSION_GRANTED || platform::displays().empty() ||
        !(capabilities.capture_sources & (1u << RCS_CAPTURE_WINDOW))) {
        std::printf("capture_targets skipped: the screen cannot be captured here\n");
        return 77;
    }
    CHECK(rcs_register_module(rcs_thumbnail_module()) == RCS_OK); // For the session and the agent: one process.
    // Named scenarios only, in the order given; all of them without names.
    const std::vector<std::pair<std::string, void (*)()>> scenarios = {
        {"window", window},
        {"closedWindow", closedWindow},
        {"goneWindow", goneWindow},
        {"application", application},
        {"quitApplication", quitApplication},
        {"display", display},
#ifdef __APPLE__
        {"ownWindow", ownWindow},
#endif
        {"region", region},
        {"thumbnails", thumbnails},
        {"thumbnailWhileCapturing", thumbnailWhileCapturing},
        {"agentThumbnail", agentThumbnail}};
    std::vector<std::string> names(argv + 1, argv + argc);
    return runTest("capture_targets", [&] {
        for (const auto& name : names.empty() ? std::vector<std::string>{} : names) {
            const auto found =
                std::find_if(scenarios.begin(), scenarios.end(), [&](const auto& s) { return s.first == name; });
            CHECK(found != scenarios.end());
            found->second();
        }
        if (names.empty()) {
            for (const auto& scenario : scenarios) {
                scenario.second();
            }
        }
#ifdef _WIN32
        if (const char* value = std::getenv("RCSDK_TEST_INPUT"); value && std::string(value) == "1") {
            input();
            pixels();
        }
#endif
    });
}
