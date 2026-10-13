#include "platform/windows/window.hpp"

#include "platform/windows/d3d.hpp"
#include "platform/windows/display.hpp"
#include "platform/windows/win.hpp"

#include "platform/platform.hpp"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <thread>

namespace rcsdk::platform {
namespace win {
namespace {

bool cloaked(HWND window) {
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &value, sizeof(value))) && value;
}

RECT frameOf(HWND window) {
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        GetWindowRect(window, &frame);
    }
    return frame;
}

bool shown(HWND window) {
    return IsWindowVisible(window) && !IsIconic(window) && !cloaked(window);
}

// What can be seen of a window: shown, and not a click-through overlay
// (transparent to the pointer, usually to the eye as well).
bool opaque(HWND window) {
    return shown(window) && !(GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TRANSPARENT);
}

// Top-level windows, front to back.
template <class Visit>
void eachWindow(Visit&& visit) {
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL { return (*reinterpret_cast<Visit*>(data))(window) ? TRUE : FALSE; },
        reinterpret_cast<LPARAM>(&visit));
}

// At most `limit` bytes of UTF-8, cut at a character.
std::string utf8(const std::wstring& text, size_t limit) {
    std::string result = narrow(text);
    size_t length = std::min(result.size(), limit);
    while (length && length < result.size() && (uint8_t(result[length]) & 0xC0) == 0x80) {
        --length; // Cut at a character, never inside one.
    }
    result.resize(length);
    return result;
}

// An application's name: its program's description ("Google Chrome"), or
// the program's file name without .exe.
std::wstring applicationName(DWORD process) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) {
        return {};
    }
    wchar_t path[MAX_PATH * 2];
    DWORD size = DWORD(std::size(path));
    const bool named = QueryFullProcessImageNameW(handle, 0, path, &size);
    CloseHandle(handle);
    if (!named) {
        return {};
    }
    std::wstring name(path, size);
    name = name.substr(name.find_last_of(L'\\') + 1);
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) {
        name.resize(name.size() - 4);
    }
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
    std::vector<uint8_t> version(bytes);
    struct Translation {
        WORD language, codePage;
    }* translations = nullptr;
    UINT length = 0;
    if (bytes && GetFileVersionInfoW(path, 0, bytes, version.data()) &&
        VerQueryValueW(version.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations),
                       &length) &&
        length >= sizeof(Translation)) {
        wchar_t key[64];
        swprintf(key, std::size(key), L"\\StringFileInfo\\%04x%04x\\FileDescription", translations->language,
                 translations->codePage);
        wchar_t* description = nullptr;
        if (VerQueryValueW(version.data(), key, reinterpret_cast<void**>(&description), &length) && length > 1 &&
            description[0]) {
            return description;
        }
    }
    return name;
}

// The windows Alt+Tab shows: visible, not cloaked, titled, and not a tool
// window or an owned one unless it asks to be shown as an application.
bool listed(HWND window) {
    if (!IsWindowVisible(window) || cloaked(window) || !GetWindowTextLengthW(window)) {
        return false;
    }
    const LONG style = GetWindowLongW(window, GWL_EXSTYLE);
    if (style & WS_EX_APPWINDOW) {
        return true;
    }
    if ((style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || GetWindow(window, GW_OWNER)) {
        return false;
    }
    wchar_t type[32]{};
    GetClassNameW(window, type, int(std::size(type)));
    return wcscmp(type, L"Progman") != 0 && wcscmp(type, L"WorkerW") != 0; // The desktop itself.
}

} // namespace

DWORD processOf(HWND window) noexcept {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process;
}

std::optional<WindowState> windowState(HWND window, DWORD process) {
    if (!IsWindow(window) || processOf(window) != process) {
        return std::nullopt;
    }
    DpiScope dpi;
    return WindowState{frameOf(window), shown(window)};
}

std::vector<RECT> visibleParts(DWORD process, const RECT& display) {
    DpiScope dpi;
    HRGN visible = CreateRectRgn(0, 0, 0, 0), covered = CreateRectRgn(0, 0, 0, 0), part = CreateRectRgn(0, 0, 0, 0);
    eachWindow([&](HWND window) {
        const RECT bounds = frameOf(window);
        RECT frame{};
        if (!opaque(window) || !IntersectRect(&frame, &display, &bounds)) {
            return true;
        }
        HRGN area = CreateRectRgnIndirect(&frame);
        if (processOf(window) == process) {
            CombineRgn(part, area, covered, RGN_DIFF);
            CombineRgn(visible, visible, part, RGN_OR);
        }
        CombineRgn(covered, covered, area, RGN_OR);
        DeleteObject(area);
        return true;
    });
    std::vector<RECT> parts;
    if (const DWORD size = GetRegionData(visible, 0, nullptr)) {
        std::vector<uint8_t> bytes(size);
        auto* data = reinterpret_cast<RGNDATA*>(bytes.data());
        if (GetRegionData(visible, size, data)) {
            const auto* rects = reinterpret_cast<const RECT*>(data->Buffer);
            for (DWORD i = 0; i < data->rdh.nCount; ++i) {
                RECT r = rects[i];
                OffsetRect(&r, -display.left, -display.top);
                parts.push_back(r);
            }
        }
    }
    DeleteObject(visible);
    DeleteObject(covered);
    DeleteObject(part);
    return parts;
}

bool showsOn(DWORD process, const RECT& display) {
    DpiScope dpi;
    bool found = false;
    eachWindow([&](HWND window) {
        const RECT bounds = frameOf(window);
        RECT frame{};
        found = processOf(window) == process && shown(window) && IntersectRect(&frame, &display, &bounds);
        return !found;
    });
    return found;
}

HWND windowAt(POINT point) {
    DpiScope dpi;
    const HWND window = WindowFromPoint(point);
    return window ? GetAncestor(window, GA_ROOT) : nullptr;
}

HWND windowOfAt(DWORD process, POINT point) {
    DpiScope dpi;
    HWND found = nullptr;
    eachWindow([&](HWND window) {
        const RECT bounds = frameOf(window);
        if (processOf(window) == process && shown(window) && PtInRect(&bounds, point)) {
            found = window;
        }
        return !found;
    });
    return found;
}

bool bringToFront(HWND window, POINT point, const std::function<bool(HWND)>& reached) {
    const auto inFront = [&] {
        for (int wait = 0; wait < 15; ++wait) {
            if (reached(windowAt(point))) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    // Above the other windows without taking the focus (the press that
    // follows gives it); asynchronous, so a window that hangs cannot stall input.
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    if (inFront()) {
        return true;
    }
    SetForegroundWindow(window);
    return inFront();
}

DWORD foregroundProcess() {
    const HWND window = GetForegroundWindow();
    return window ? processOf(window) : 0;
}

rcs_result sharedArea(const CaptureTarget& target, uint32_t displayId, HWND window, DWORD process, RECT& area) {
    if (target.source == RCS_CAPTURE_WINDOW) {
        const auto state = windowState(window, process);
        if (!state || !state->shown) {
            return RCS_ERROR_INVALID_STATE; // A window that does not show takes no input.
        }
        area = state->frame;
        return RCS_OK;
    }
    const auto monitor = findMonitor(displayId);
    if (!monitor) {
        return RCS_ERROR_UNSUPPORTED; // The display is gone.
    }
    area = monitor->rect;
    if (target.source == RCS_CAPTURE_REGION) {
        const auto& r = target.region;
        const RECT region{area.left + LONG(r.x), area.top + LONG(r.y), area.left + LONG(r.x + r.width),
                          area.top + LONG(r.y + r.height)};
        const RECT display = area;
        if (!IntersectRect(&area, &region, &display)) {
            return RCS_ERROR_INVALID_STATE; // The region is off the display.
        }
    }
    return RCS_OK;
}

} // namespace win

std::optional<SharedArea> sharedArea(const CaptureTarget& target) {
    win::DpiScope dpi; // Physical pixels, as displays and windows are named.
    HWND shared = nullptr;
    DWORD process = 0;
    if (target.source == RCS_CAPTURE_WINDOW) {
        shared = win::windowOf(target.windowId);
        process = win::processOf(shared);
    } else if (target.source == RCS_CAPTURE_APPLICATION) {
        process = target.processId;
    }
    RECT area{};
    if (win::sharedArea(target, target.displayId, shared, process, area) != RCS_OK) {
        return std::nullopt;
    }
    return SharedArea{double(area.left), double(area.top), double(area.right), double(area.bottom), process};
}

std::vector<rcs_window_info> windows() {
    win::DpiScope dpi;
    const HWND foreground = GetForegroundWindow();
    const auto displays = win::monitors();
    std::map<DWORD, std::string> names; // Per process, once.
    std::vector<rcs_window_info> result;
    win::eachWindow([&](HWND window) {
        if (!win::listed(window)) {
            return true;
        }
        rcs_window_info& item = result.emplace_back();
        item.window_id = uint64_t(reinterpret_cast<uintptr_t>(window));
        item.process_id = win::processOf(window);
        item.minimized = IsIconic(window) ? 1 : 0;
        item.focused = window == foreground;
        RECT frame = win::frameOf(window);
        if (item.minimized) { // Its size when restored; it shows on no display.
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            GetWindowPlacement(window, &placement);
            frame = placement.rcNormalPosition;
            OffsetRect(&frame, -frame.left, -frame.top);
        } else {
            LONG most = 0;
            for (const auto& display : displays) {
                RECT shown{};
                if (IntersectRect(&shown, &frame, &display.rect)) {
                    if (const LONG area = (shown.right - shown.left) * (shown.bottom - shown.top); area > most) {
                        most = area;
                        item.display_id = display.id;
                        item.x = frame.left - display.rect.left;
                        item.y = frame.top - display.rect.top;
                    }
                }
            }
        }
        item.width = uint32_t(std::max<LONG>(0, frame.right - frame.left));
        item.height = uint32_t(std::max<LONG>(0, frame.bottom - frame.top));
        std::wstring title(size_t(GetWindowTextLengthW(window)) + 1, L'\0');
        title.resize(size_t(GetWindowTextW(window, title.data(), int(title.size()))));
        const std::string text = win::utf8(title, sizeof(item.title) - 1);
        std::memcpy(item.title, text.data(), text.size());
        auto name = names.find(item.process_id);
        if (name == names.end()) {
            name = names
                       .emplace(item.process_id,
                                win::utf8(win::applicationName(item.process_id), sizeof(item.application) - 1))
                       .first;
        }
        std::memcpy(item.application, name->second.data(), name->second.size());
        return true;
    });
    return result;
}

} // namespace rcsdk::platform
