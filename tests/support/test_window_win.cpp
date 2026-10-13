#include "support/test_window.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace rcsdk::test {

struct TestWindow::State {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable ready;
    HWND window = nullptr;
    bool created = false;
    std::atomic<int> hue{0};
    std::atomic<COLORREF> color{CLR_INVALID}; // A fixed colour (setColor), or the changing one.
    std::atomic<int> clicks{0};
    std::atomic<int> clickX{0}, clickY{0};
    mutable std::mutex inputMutex;
    std::vector<std::string> received;

    void note(const char* format, int value) {
        char line[64];
        std::snprintf(line, sizeof(line), format, value);
        std::lock_guard lock(inputMutex);
        received.push_back(line);
    }
};

namespace {

// Makes a system caret on the window's thread: wparam its place, lparam its size.
constexpr UINT showCaretMessage = WM_APP + 1;

// What every test window does: repaints at each tick (in a new colour), and
// ends its thread's loop when destroyed.
LRESULT procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_TIMER:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

TestWindow::TestWindow(int x, int y, int width, int height) : state_(std::make_unique<State>()) {
    State* state = state_.get();
    state->thread = std::thread([state, x, y, width, height] {
        if (!std::getenv("RCSDK_TEST_UNAWARE")) {
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        static const WNDCLASSW type = [] {
            WNDCLASSW c{};
            c.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
                auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                if (s && message == WM_TIMER && s->color != CLR_INVALID) {
                    return 0; // Its colour stays: the desktop does not change.
                }
                if (s && message == WM_PAINT) {
                    PAINTSTRUCT paint;
                    HDC dc = BeginPaint(window, &paint);
                    const int hue = ++s->hue;
                    const COLORREF fixed = s->color;
                    HBRUSH brush = CreateSolidBrush(fixed != CLR_INVALID ? fixed : RGB(hue * 7 % 256, 120, 200));
                    RECT client;
                    GetClientRect(window, &client);
                    FillRect(dc, &client, brush);
                    DeleteObject(brush);
                    EndPaint(window, &paint);
                    return 0;
                }
                if (s && message == WM_LBUTTONDOWN) {
                    s->clickX = short(LOWORD(lparam));
                    s->clickY = short(HIWORD(lparam));
                    ++s->clicks;
                }
                if (message == showCaretMessage) {
                    CreateCaret(window, nullptr, LOWORD(lparam), HIWORD(lparam));
                    SetCaretPos(short(LOWORD(wparam)), short(HIWORD(wparam)));
                    ShowCaret(window);
                    return 0;
                }
                if (s) {
                    switch (message) {
                    case WM_CHAR:
                        s->note("char %04X", int(wparam));
                        break;
                    case WM_KEYDOWN:
                        s->note("key %02X", int(wparam));
                        break;
                    case WM_XBUTTONDOWN:
                        s->note("xbutton %d", int(GET_XBUTTON_WPARAM(wparam)));
                        break;
                    case WM_MOUSEWHEEL:
                        s->note("wheel %d", int(GET_WHEEL_DELTA_WPARAM(wparam)));
                        break;
                    case WM_MOUSEHWHEEL:
                        s->note("hwheel %d", int(GET_WHEEL_DELTA_WPARAM(wparam)));
                        break;
                    case WM_POINTERDOWN:
                    case WM_POINTERUP: {
                        const UINT32 id = GET_POINTERID_WPARAM(wparam);
                        POINTER_INPUT_TYPE type = PT_POINTER;
                        GetPointerType(id, &type);
                        const bool down = message == WM_POINTERDOWN;
                        if (type == PT_TOUCH) {
                            s->note(down ? "touch down" : "touch up", 0);
                        } else if (type == PT_PEN) {
                            POINTER_PEN_INFO pen{};
                            GetPointerPenInfo(id, &pen);
                            s->note(down ? "pen down %d" : "pen up", int(pen.pressure));
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }
                return procedure(window, message, wparam, lparam);
            };
            c.hInstance = GetModuleHandleW(nullptr);
            c.lpszClassName = L"rcsdk test window";
            c.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&c);
            return c;
        }();
        // Topmost: no window of the desktop covers it (a console, the
        // terminal the test runs in), so the desktop shows what it draws.
        HWND window = CreateWindowExW(WS_EX_TOPMOST, type.lpszClassName, L"rcsdk test window", WS_OVERLAPPEDWINDOW, x,
                                      y, width, height, nullptr, nullptr, type.hInstance, nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(state));
        ShowWindow(window, SW_SHOWNORMAL);
        SetForegroundWindow(window);
        SetTimer(window, 1, 30, nullptr);
        {
            std::lock_guard lock(state->mutex);
            state->window = window;
            state->created = true;
        }
        state->ready.notify_all();
        for (MSG message; GetMessageW(&message, nullptr, 0, 0) > 0;) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });
    std::unique_lock lock(state->mutex);
    state->ready.wait(lock, [&] { return state->created; });
    lock.unlock();
    pump(std::chrono::milliseconds(300));
}

TestWindow::~TestWindow() {
    close();
}

uint64_t TestWindow::id() const {
    return uint64_t(reinterpret_cast<uintptr_t>(state_->window));
}

void TestWindow::setColor(uint8_t red, uint8_t green, uint8_t blue) {
    state_->color = RGB(red, green, blue);
    InvalidateRect(state_->window, nullptr, FALSE);
}

void TestWindow::resize(int width, int height) {
    // In physical pixels, as it was made: Windows scales the sizes of a
    // thread that is not DPI aware.
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetWindowPos(state_->window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
    SetThreadDpiAwarenessContext(previous);
}

void TestWindow::minimize() {
    ShowWindow(state_->window, SW_MINIMIZE);
}

void TestWindow::restore() {
    ShowWindow(state_->window, SW_RESTORE);
}

void TestWindow::close() {
    if (!state_->thread.joinable()) {
        return;
    }
    PostMessageW(state_->window, WM_CLOSE, 0, 0);
    state_->thread.join();
}

int TestWindow::clicks() const {
    return state_->clicks;
}

void TestWindow::lastClick(int& x, int& y) const {
    x = state_->clickX;
    y = state_->clickY;
}

std::vector<std::string> TestWindow::received() const {
    std::lock_guard lock(state_->inputMutex);
    return state_->received;
}

void TestWindow::showCaret(int x, int y, int width, int height) {
    SendMessageW(state_->window, showCaretMessage, MAKEWPARAM(x, y), MAKELPARAM(width, height));
}

void TestWindow::pump(std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
}

struct ChildWindow::Pipes {
    PROCESS_INFORMATION process{};
    HANDLE to = nullptr;
};

ChildWindow::ChildWindow() : pipes_(std::make_unique<Pipes>()) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    CreatePipe(&inRead, &inWrite, &inherit, 0);
    CreatePipe(&outRead, &outWrite, &inherit, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inRead;
    startup.hStdOutput = outWrite;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring line = L"\"" + std::wstring(path) + L"\" --child-window";
    CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &pipes_->process);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    pipes_->to = inWrite;
    std::string first; // Its window's id and its process.
    char c = 0;
    DWORD read = 0;
    while (ReadFile(outRead, &c, 1, &read, nullptr) && read == 1 && c != '\n') {
        first += c;
    }
    CloseHandle(outRead);
    unsigned long long id = 0;
    unsigned process = 0;
    if (std::sscanf(first.c_str(), "%llu %u", &id, &process) == 2) {
        id_ = id;
        process_ = process;
    }
}

ChildWindow::~ChildWindow() {
    quit();
}

void ChildWindow::send(const char* command) {
    if (pipes_->to) {
        const std::string line = std::string(command) + "\n";
        DWORD written = 0;
        WriteFile(pipes_->to, line.data(), DWORD(line.size()), &written, nullptr);
    }
}

void ChildWindow::close() {
    send("close");
}

void ChildWindow::quit() {
    if (!pipes_->to) {
        return;
    }
    CloseHandle(pipes_->to); // The end of its input ends it.
    pipes_->to = nullptr;
    WaitForSingleObject(pipes_->process.hProcess, 10'000);
    CloseHandle(pipes_->process.hProcess);
    CloseHandle(pipes_->process.hThread);
}

int runChildWindow() {
    auto window = std::make_unique<TestWindow>(700, 160, 360, 240);
    std::printf("%llu %lu\n", (unsigned long long)window->id(), GetCurrentProcessId());
    std::fflush(stdout);
    for (std::string line; std::getline(std::cin, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
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
    }
    return 0; // The parent closed its end: quit.
}

} // namespace rcsdk::test
