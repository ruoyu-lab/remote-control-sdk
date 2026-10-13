// The helper's side of an rcs_supervisor (RemoteControl::Supervisor, which
// lives beside the core): what the supervisor handed the process it started,
// for rcs_helper_* and for Ctrl+Alt+Del (desktop.cpp). The supervisor puts
// handles in environment variables that also name its process, so that a
// process only takes them from its parent; they are removed at once.
#include "platform/platform.hpp"

#include "platform/windows/desktop.hpp"
#include "platform/windows/win.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <mutex>
#include <optional>

namespace rcsdk::platform {
namespace {

// The supervisor's names for them (components/supervisor).
constexpr const wchar_t* stopVariable = L"RCSDK_SUPERVISOR_STOP";
constexpr const wchar_t* sasVariable = L"RCSDK_SUPERVISOR_SAS";
constexpr const wchar_t* keyVariable = L"RCSDK_SUPERVISOR_KEY";

// The process that created this one.
DWORD parentProcessId() {
    const win::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
        if (entry.th32ProcessID == self) {
            return entry.th32ParentProcessID;
        }
    }
    return 0;
}

std::optional<uint64_t> number(const std::wstring& text) {
    const bool digits = std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (text.empty() || text.size() > 19 || !digits) {
        return std::nullopt;
    }
    return std::wcstoull(text.c_str(), nullptr, 10);
}

// A variable a supervisor set, "<its process id>:<value>": the value, when
// that process is this one's parent. The variable is removed either way, so
// that the processes this one starts do not see it.
std::optional<std::wstring> fromSupervisor(const wchar_t* name, DWORD parent) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) {
        return std::nullopt;
    }
    std::wstring text(size, L'\0');
    text.resize(GetEnvironmentVariableW(name, text.data(), size));
    SetEnvironmentVariableW(name, nullptr);
    const size_t colon = text.find(L':');
    const auto process = number(text.substr(0, colon));
    if (colon == std::wstring::npos || !process || !parent || *process != parent) {
        return std::nullopt;
    }
    return text.substr(colon + 1);
}

// A handle the supervisor gave: not passed on to processes this one starts.
HANDLE handleOf(const std::optional<std::wstring>& value) {
    const auto handle = value ? number(*value) : std::nullopt;
    if (!handle || !*handle) {
        return nullptr;
    }
    const auto result = reinterpret_cast<HANDLE>(uintptr_t(*handle));
    SetHandleInformation(result, HANDLE_FLAG_INHERIT, 0);
    return result;
}

} // namespace

namespace win {

const SupervisorLink* supervisorLink() {
    static const std::optional<SupervisorLink> link = []() -> std::optional<SupervisorLink> {
        const DWORD parent = parentProcessId();
        const auto stop = fromSupervisor(stopVariable, parent);
        const auto sas = fromSupervisor(sasVariable, parent);
        const auto key = fromSupervisor(keyVariable, parent);
        SupervisorLink found;
        found.stop = handleOf(stop);
        if (!found.stop) {
            return std::nullopt;
        }
        found.sas = handleOf(sas);
        found.key = handleOf(key);
        return found;
    }();
    return link ? &*link : nullptr;
}

} // namespace win

rcs_result waitForSupervisorStop(WaitLimit limit) noexcept {
    const auto* link = win::supervisorLink();
    if (!link) {
        return RCS_ERROR_UNSUPPORTED;
    }
    const DWORD result = WaitForSingleObject(link->stop, windowsTimeout(limit));
    return result == WAIT_OBJECT_0 ? RCS_OK : result == WAIT_TIMEOUT ? RCS_ERROR_TIMEOUT : RCS_ERROR_INTERNAL;
}

std::array<uint8_t, 32> helperKey() {
    static std::mutex mutex;
    static std::optional<std::array<uint8_t, 32>> key;
    static bool taken = false; // The pipe was read (and closed), whatever came of it.
    std::lock_guard lock(mutex);
    if (key) {
        return *key;
    }
    const auto* link = win::supervisorLink();
    if (!link) {
        fail(RCS_ERROR_UNSUPPORTED, "This process was not started by an rcs_supervisor");
    }
    if (!link->key) {
        fail(RCS_ERROR_UNSUPPORTED,
             "The rcs_supervisor that started this process handed it no key (rcs_supervisor_config.helper_key)");
    }
    if (taken) {
        fail(RCS_ERROR_INTERNAL, "The key the rcs_supervisor handed this process could not be read");
    }
    taken = true;
    std::array<uint8_t, 32> value{};
    DWORD total = 0;
    for (DWORD read = 0; total < value.size(); total += read) {
        if (!ReadFile(link->key, value.data() + total, DWORD(value.size()) - total, &read, nullptr) || !read) {
            break;
        }
    }
    CloseHandle(link->key);
    if (total != value.size()) {
        fail(RCS_ERROR_INTERNAL, "The key the rcs_supervisor handed this process could not be read");
    }
    key = value;
    return value;
}

} // namespace rcsdk::platform
