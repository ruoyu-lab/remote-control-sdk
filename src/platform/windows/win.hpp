// Small Windows helpers shared by the SDK's Windows code: an owned kernel
// handle, UTF-8 and UTF-16 text, error text, and token queries.
#pragma once

#include <windows.h>

#include <sddl.h>

#include "util/util.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace rcsdk::platform::win {

// Owns a kernel object's handle; null and INVALID_HANDLE_VALUE are none.
class Handle {
public:
    explicit Handle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~Handle() { reset(); }
    Handle(Handle&& other) noexcept : handle_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }
    void reset(HANDLE handle = nullptr) noexcept {
        if (*this) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_;
};

// "Windows error 5 (access denied)": the code, and what the common ones mean.
inline std::string errorText(DWORD code) {
    const char* meaning = nullptr;
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
        meaning = "not found";
        break;
    case ERROR_PATH_NOT_FOUND:
        meaning = "path not found";
        break;
    case ERROR_ACCESS_DENIED:
        meaning = "access denied";
        break;
    case ERROR_PIPE_BUSY:
        meaning = "all instances are busy";
        break;
    case ERROR_PRIVILEGE_NOT_HELD:
        meaning = "a privilege is not held";
        break;
    case ERROR_NO_TOKEN:
        meaning = "no token";
        break;
    case ERROR_BAD_EXE_FORMAT:
        meaning = "not a program";
        break;
    default:
        break;
    }
    return "Windows error " + std::to_string(code) + (meaning ? std::string(" (") + meaning + ")" : std::string());
}

// UTF-8 to UTF-16. Throws Failure(INVALID_ARGUMENT) naming `what` for text
// that is not UTF-8.
inline std::wstring wide(std::string_view text, const char* what = "text") {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (size <= 0) {
        fail(RCS_ERROR_INVALID_ARGUMENT, std::string(what) + " is not UTF-8");
    }
    std::wstring result(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), size);
    return result;
}

// UTF-16 to UTF-8 (what is not UTF-16 becomes U+FFFD).
inline std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size_t(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
    }
    return result;
}

// A token's information of one class (TokenUser, TokenGroups...); empty when
// it cannot be read, GetLastError() says why.
inline std::vector<uint8_t> tokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS type) {
    DWORD size = 0;
    if (GetTokenInformation(token, type, nullptr, 0, &size) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }
    std::vector<uint8_t> buffer(size);
    if (!GetTokenInformation(token, type, buffer.data(), size, &size)) {
        return {};
    }
    return buffer;
}

// A SID as SDDL text ("S-1-5-18"); empty when it cannot be converted.
inline std::wstring sidText(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) {
        return {};
    }
    std::wstring result = text;
    LocalFree(text);
    return result;
}

} // namespace rcsdk::platform::win
