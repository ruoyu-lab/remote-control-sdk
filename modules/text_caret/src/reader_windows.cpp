// The text caret on Windows: the system caret of the thread that has the
// focus (GetGUIThreadInfo), which classic controls and many programs make;
// else UI Automation's text pattern, which browsers, Office and most newer
// programs answer (not all: Windows Terminal has no TextPattern2, some
// programs no text pattern at all, and those report no caret).
//
// UI Automation lives on the reader's thread, in the multithreaded apartment,
// and waits at most half a second for a program to answer (a program that
// hangs would make it wait 20 s). A degenerate range, which a caret is, has no
// bounding rectangle: the character after it gives its place (its left edge),
// at the end of a line the one before it (its right edge), on an empty line
// the line.
#include <windows.h>

#include "reader.hpp"

#include <oleauto.h>
#include <uiautomation.h>

#include <optional>
#include <utility>
#include <vector>

namespace rcsdk::text_caret {
namespace {

// Owns one COM reference.
template <class T>
class Com {
public:
    T* value = nullptr;
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T** out() noexcept {
        reset();
        return &value;
    }
    T* operator->() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
    void reset() noexcept {
        if (value) {
            value->Release();
            value = nullptr;
        }
    }
};

// Per-monitor DPI awareness for the calling thread while it lives: physical
// pixels, as rcs_module_shared has them.
struct DpiScope {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope() {
        if (previous) {
            SetThreadDpiAwarenessContext(previous);
        }
    }
};

DWORD processOf(HWND window) noexcept {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process;
}

constexpr DWORD automationTimeoutMs = 500;

// The rectangles of a text range, in physical screen pixels (the reading
// thread is per-monitor aware): left, top, width, height each.
std::vector<double> rectangles(IUIAutomationTextRange* range) {
    SAFEARRAY* array = nullptr;
    std::vector<double> values;
    if (FAILED(range->GetBoundingRectangles(&array)) || !array) {
        return values;
    }
    LONG first = 0, last = -1;
    double* data = nullptr;
    if (SUCCEEDED(SafeArrayGetLBound(array, 1, &first)) && SUCCEEDED(SafeArrayGetUBound(array, 1, &last)) &&
        last >= first && SUCCEEDED(SafeArrayAccessData(array, reinterpret_cast<void**>(&data)))) {
        values.assign(data, data + (last - first + 1) / 4 * 4);
        SafeArrayUnaccessData(array);
    }
    SafeArrayDestroy(array);
    return values;
}

// A range's text holds something on a line, not a line's end (or nothing).
bool printable(IUIAutomationTextRange* range) {
    BSTR text = nullptr;
    if (FAILED(range->GetText(2, &text)) || !text) {
        return false;
    }
    const bool result =
        SysStringLen(text) && text[0] != L'\r' && text[0] != L'\n' && text[0] != 0x2028 && text[0] != 0x2029;
    SysFreeString(text);
    return result;
}

class WindowsReader final : public Reader {
public:
    explicit WindowsReader(rcs_module module)
        : module_(module), apartment_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~WindowsReader() override {
        automation_.reset();
        if (apartment_) {
            CoUninitialize();
        }
    }

    rcs_text_caret read(const rcs_module_shared& shared) override {
        DpiScope dpi;
        const HWND focus = GetForegroundWindow();
        if (!focus) {
            return {};
        }
        const DWORD owner = processOf(focus);
        if ((shared.source == RCS_CAPTURE_WINDOW || shared.source == RCS_CAPTURE_APPLICATION) &&
            owner != shared.process_id) {
            return {}; // Typing goes to another application, which is not shared.
        }
        auto caret = systemCaret(focus);
        if (!caret) {
            caret = automationCaret(owner);
        }
        return caret ? caretWithin(*caret, areaOf(shared)) : rcs_text_caret{};
    }

private:
    // The focused thread's system caret: in client coordinates of its window,
    // and in that window's own DPI scale (an application that is not DPI aware
    // has it in 96-DPI units on a scaled display), as the client area shows.
    static std::optional<ScreenRect> systemCaret(HWND focus) {
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(GetWindowThreadProcessId(focus, nullptr), &info) || !info.hwndCaret ||
            IsRectEmpty(&info.rcCaret)) {
            return std::nullopt;
        }
        RECT own{}; // The client area in the window's own scale...
        const DPI_AWARENESS_CONTEXT previous =
            SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(info.hwndCaret));
        GetClientRect(info.hwndCaret, &own);
        SetThreadDpiAwarenessContext(previous);
        RECT client{}; // ...and in physical pixels, where it is on the screen.
        POINT origin{0, 0};
        if (!GetClientRect(info.hwndCaret, &client) || !ClientToScreen(info.hwndCaret, &origin) || own.right <= 0 ||
            own.bottom <= 0) {
            return std::nullopt;
        }
        const double sx = double(client.right) / own.right, sy = double(client.bottom) / own.bottom;
        const RECT& c = info.rcCaret;
        return ScreenRect{origin.x + c.left * sx, origin.y + c.top * sy, origin.x + c.right * sx,
                          origin.y + c.bottom * sy};
    }

    std::optional<ScreenRect> automationCaret(DWORD owner) {
        if (!automationTried_) {
            automationTried_ = true;
            makeAutomation();
        }
        if (!automation_) {
            return std::nullopt;
        }
        Com<IUIAutomationElement> focused;
        int process = 0;
        if (FAILED(automation_->GetFocusedElement(focused.out())) || !focused ||
            FAILED(focused->get_CurrentProcessId(&process)) || DWORD(process) != owner) {
            return std::nullopt; // The focus moved on meanwhile.
        }
        // Where the caret is: TextPattern2 says it; TextPattern's selection
        // ends there (its end taken as the caret).
        Com<IUIAutomationTextRange> caret;
        Com<IUIAutomationTextPattern2> pattern2;
        if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPattern2Id, __uuidof(IUIAutomationTextPattern2),
                                                   reinterpret_cast<void**>(pattern2.out()))) &&
            pattern2) {
            BOOL active = FALSE;
            if (FAILED(pattern2->GetCaretRange(&active, caret.out()))) {
                caret.reset(); // Not implemented: the selection says.
            } else if (caret && !active) {
                return std::nullopt; // It has a caret, without the keyboard focus.
            }
        }
        if (!caret) {
            Com<IUIAutomationTextPattern> pattern;
            Com<IUIAutomationTextRangeArray> selection;
            int ranges = 0;
            if (FAILED(focused->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                                                    reinterpret_cast<void**>(pattern.out()))) ||
                !pattern || FAILED(pattern->GetSelection(selection.out())) || !selection ||
                FAILED(selection->get_Length(&ranges)) || ranges < 1 || FAILED(selection->GetElement(0, caret.out())) ||
                !caret ||
                FAILED(caret->MoveEndpointByRange(TextPatternRangeEndpoint_Start, caret.value,
                                                  TextPatternRangeEndpoint_End))) {
                return std::nullopt;
            }
        }
        return place(caret.value);
    }

    // A degenerate range's place: before the character after it, after the
    // one before it, or at the start of its line; one pixel wide.
    static std::optional<ScreenRect> place(IUIAutomationTextRange* caret) {
        const auto edge = [](IUIAutomationTextRange* range, bool right) -> std::optional<ScreenRect> {
            const auto r = rectangles(range);
            if (r.size() < 4) {
                return std::nullopt;
            }
            const size_t at = right ? r.size() - 4 : 0;
            const double x = right ? r[at] + r[at + 2] : r[at];
            return ScreenRect{x, r[at + 1], x + 1, r[at + 1] + r[at + 3]};
        };
        Com<IUIAutomationTextRange> next, previous, line;
        if (SUCCEEDED(caret->Clone(next.out())) && next && SUCCEEDED(next->ExpandToEnclosingUnit(TextUnit_Character)) &&
            printable(next.value)) {
            if (auto found = edge(next.value, false)) {
                return found;
            }
        }
        int moved = 0;
        if (SUCCEEDED(caret->Clone(previous.out())) && previous &&
            SUCCEEDED(previous->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, -1, &moved)) &&
            moved && printable(previous.value)) {
            if (auto found = edge(previous.value, true)) {
                return found;
            }
        }
        if (SUCCEEDED(caret->Clone(line.out())) && line && SUCCEEDED(line->ExpandToEnclosingUnit(TextUnit_Line))) {
            return edge(line.value, false);
        }
        return std::nullopt;
    }

    void makeAutomation() {
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation2),
                                    reinterpret_cast<void**>(automation_.out())))) {
            rcs_module_log(module_, RCS_LOG_WARNING,
                           "UI Automation is not available: the text caret is known only where programs make a "
                           "system caret");
            return;
        }
        if (FAILED(automation_->put_ConnectionTimeout(automationTimeoutMs)) ||
            FAILED(automation_->put_TransactionTimeout(automationTimeoutMs))) {
            rcs_module_log(module_, RCS_LOG_WARNING,
                           "UI Automation's timeouts could not be set: looking for the text caret waits as long as "
                           "a program that hangs (up to 20 s)");
        }
    }

    const rcs_module module_;
    const bool apartment_;
    bool automationTried_ = false;
    Com<IUIAutomation2> automation_; // Made at the first look that needs it.
};

} // namespace

std::unique_ptr<Reader> createPlatformReader(rcs_module module) {
    return std::make_unique<WindowsReader>(module);
}

bool platformAvailable() noexcept {
    return true;
}

} // namespace rcsdk::text_caret
