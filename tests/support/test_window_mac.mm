#include "support/test_window.hpp"

#import <AppKit/AppKit.h>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>

extern char** environ;

namespace rcsdk::test {

struct TestWindow::State {
    NSWindow* window = nil;
    NSTimer* timer = nil;
};

TestWindow::TestWindow(int x, int y, int width, int height) : state_(std::make_unique<State>()) {
    @autoreleasepool { // What AppKit autoreleases goes with it: closed, the window is gone.
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        // AppKit's origin is the bottom left of the main display.
        const CGFloat top = NSScreen.screens.firstObject.frame.size.height;
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(x, top - y - height, width, height)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        window.title = @"rcsdk test window";
        window.releasedWhenClosed = NO;
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        __block int step = 0;
        __weak NSWindow* weak = window; // The timer must not keep it: closed, it is gone.
        state_->timer = [NSTimer scheduledTimerWithTimeInterval:0.03
                                                        repeats:YES
                                                          block:^(NSTimer*) {
                                                              weak.backgroundColor =
                                                                  [NSColor colorWithHue:(++step % 60) / 60.0
                                                                             saturation:0.6
                                                                             brightness:0.8
                                                                                  alpha:1];
                                                          }];
        state_->window = window;
    }
    pump(std::chrono::milliseconds(300));
}

TestWindow::~TestWindow() { close(); }

uint64_t TestWindow::id() const { return uint64_t(state_->window.windowNumber); }

void TestWindow::setColor(uint8_t red, uint8_t green, uint8_t blue) {
    @autoreleasepool {
        [state_->timer invalidate];
        state_->timer = nil;
        state_->window.backgroundColor = [NSColor colorWithSRGBRed:red / 255.0
                                                             green:green / 255.0
                                                              blue:blue / 255.0
                                                             alpha:1];
    }
}

void TestWindow::resize(int width, int height) {
    @autoreleasepool {
        NSRect frame = state_->window.frame;
        const NSRect content = [state_->window contentRectForFrameRect:frame];
        frame.size.width += width - content.size.width;
        frame.origin.y += frame.size.height - (frame.size.height + height - content.size.height);
        frame.size.height += height - content.size.height;
        [state_->window setFrame:frame display:YES];
    }
}

void TestWindow::minimize() {
    @autoreleasepool {
        [state_->window miniaturize:nil];
    }
}

void TestWindow::restore() {
    @autoreleasepool {
        [state_->window deminiaturize:nil];
    }
}

void TestWindow::close() {
    if (!state_->window) {
        return;
    }
    [state_->timer invalidate];
    state_->timer = nil;
    @autoreleasepool {
        [state_->window close];
        state_->window = nil; // Released: the window server lets the window go.
    }
}

int TestWindow::clicks() const { return 0; }

void TestWindow::lastClick(int& x, int& y) const { x = y = 0; }

std::vector<std::string> TestWindow::received() const { return {}; }

void TestWindow::showCaret(int, int, int, int) {}

void TestWindow::pump(std::chrono::milliseconds duration) {
    @autoreleasepool {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:duration.count() / 1000.0]];
    }
}

struct ChildWindow::Pipes {
    pid_t pid = 0;
    FILE* to = nullptr;
};

ChildWindow::ChildWindow() : pipes_(std::make_unique<Pipes>()) {
    char path[4096];
    uint32_t size = sizeof(path);
    _NSGetExecutablePath(path, &size);
    int in[2], out[2];
    pipe(in);
    pipe(out);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addclose(&actions, in[1]);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    char* arguments[] = {path, const_cast<char*>("--child-window"), nullptr};
    posix_spawn(&pipes_->pid, path, &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(in[0]);
    ::close(out[1]);
    pipes_->to = fdopen(in[1], "w");
    FILE* from = fdopen(out[0], "r");
    unsigned long long id = 0;
    unsigned process = 0;
    if (std::fscanf(from, "%llu %u", &id, &process) == 2) {
        id_ = id;
        process_ = process;
    }
    std::fclose(from);
}

ChildWindow::~ChildWindow() { quit(); }

void ChildWindow::send(const char* command) {
    if (pipes_->to) {
        std::fprintf(pipes_->to, "%s\n", command);
        std::fflush(pipes_->to);
    }
}

void ChildWindow::close() { send("close"); }

void ChildWindow::quit() {
    if (!pipes_->to) {
        return;
    }
    std::fclose(pipes_->to); // The end of its input ends it.
    pipes_->to = nullptr;
    int status = 0;
    waitpid(pipes_->pid, &status, 0);
}

int runChildWindow() {
    auto window = std::make_unique<TestWindow>(700, 160, 360, 240);
    std::printf("%llu %u\n", (unsigned long long)window->id(), unsigned(getpid()));
    std::fflush(stdout);
    std::string line;
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK); // poll() does not say when a pipe's writer is gone.
    for (;;) {
        char c = 0;
        const ssize_t got = read(0, &c, 1);
        if (got == 0) {
            return 0; // The parent closed its end: quit.
        }
        if (got < 0) {
            TestWindow::pump(std::chrono::milliseconds(30));
            continue;
        }
        if (c != '\n') {
            line += c;
            continue;
        }
        int width = 0, height = 0;
        if (line == "close") {
            window.reset();
        } else if (window && line == "minimize") {
            window->minimize();
        } else if (window && line == "restore") {
            window->restore();
        } else if (window && std::sscanf(line.c_str(), "resize %d %d", &width, &height) == 2) {
            window->resize(width, height);
        }
        line.clear();
    }
}

} // namespace rcsdk::test
