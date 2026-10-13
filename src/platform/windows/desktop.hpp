// The desktop that receives input, and the processes that may follow it.
// Windows shows the sign-in screen, the lock screen, UAC prompts and the
// Ctrl+Alt+Del screen on a desktop of its own ("Winlogon"); only LocalSystem
// can open it. Windows may run a screen saver on one apart ("Screen-saver").
// A thread captures or injects input on the one desktop it is attached to, so
// following means attaching the thread to whichever desktop receives input now.
#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace rcsdk::platform::win {

// Whether this process runs as LocalSystem (what a service runs as), and the
// Windows session it is in (0: services; the user's own start at 1).
bool runningAsSystem() noexcept;
DWORD sessionId() noexcept;
// The Remote Desktop session this process runs in; none at the console.
std::optional<DWORD> remoteSession();

// The user's own desktop, the one a process of the user's captures and
// controls without following, and the name of a desktop.
inline constexpr wchar_t userDesktop[] = L"Default";
std::wstring nameOf(HDESK desktop);
// Whether the user's own desktop receives input now (no secure desktop or
// screen saver's is up): all a process that does not follow them injects into.
bool userDesktopHasInput();

// What the rcs_supervisor that started this process gave it (helper.cpp; the
// supervisor is RemoteControl::Supervisor's):
// an event set when it should end, its end of the pipe through which it asks
// for Ctrl+Alt+Del (connected: nothing else reaches it), and a pipe holding
// its key (until helperKey() read it).
struct SupervisorLink {
    HANDLE stop = nullptr;
    HANDLE sas = nullptr;
    HANDLE key = nullptr;
};
// Read once, and only from this process's parent: null when no supervisor
// started it. The variables that carry it are removed from this process's
// environment, and its handles are not inherited further.
const SupervisorLink* supervisorLink();

// A thread following the input desktop: attach() puts the calling thread on
// the desktop that receives input, restore() or the destructor puts it back on
// its own. Use from one thread; the thread must own no window or hook
// (SetThreadDesktop refuses otherwise).
class InputDesktop {
public:
    InputDesktop() = default;
    ~InputDesktop() { restore(); }
    InputDesktop(const InputDesktop&) = delete;
    InputDesktop& operator=(const InputDesktop&) = delete;

    // True when the thread is on the desktop that receives input now (it
    // already was, or this moved it). False when the system refuses, see
    // error(): a secure desktop is closed to processes other than LocalSystem.
    bool attach();
    void restore() noexcept;
    DWORD error() const noexcept { return error_; }
    const std::wstring& name() const noexcept { return name_; }

private:
    HDESK own_ = nullptr;     // The thread's desktop before the first attach (not owned).
    HDESK current_ = nullptr; // The attached one, when it is not the thread's own (owned).
    DWORD thread_ = 0;
    DWORD error_ = 0;
    std::wstring name_;
};

} // namespace rcsdk::platform::win
