// Keeping a Windows machine awake with a power request, shown by
// `powercfg /requests` under the session's reason.
#include "power_request.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace rcsdk::power {
namespace {

std::string errorText(DWORD error) {
    char* text = nullptr;
    const DWORD length =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = length ? std::string(text, length) : "error " + std::to_string(error);
    LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
}

std::wstring wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (size <= 0 && !text.empty()) {
        throw Refused(RCS_ERROR_INVALID_ARGUMENT, "The reason for a power request is not UTF-8");
    }
    std::wstring out(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), size);
    return out;
}

class PowerRequest final : public Request {
public:
    PowerRequest(rcs_keep_awake mode, const std::string& reason) {
        std::wstring text = wide(reason);
        REASON_CONTEXT context{};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = text.data();
        handle_ = PowerCreateRequest(&context);
        if (handle_ == INVALID_HANDLE_VALUE || !handle_) {
            handle_ = nullptr;
            throw Refused(RCS_ERROR_INTERNAL, "The system refused a power request (" + errorText(GetLastError()) + ")");
        }
        const std::vector<POWER_REQUEST_TYPE> wanted =
            mode == RCS_KEEP_AWAKE_DISPLAY
                ? std::vector<POWER_REQUEST_TYPE>{PowerRequestSystemRequired, PowerRequestDisplayRequired}
                : std::vector<POWER_REQUEST_TYPE>{PowerRequestSystemRequired};
        for (const auto type : wanted) {
            if (!PowerSetRequest(handle_, type)) {
                const DWORD error = GetLastError();
                release();
                throw Refused(RCS_ERROR_INTERNAL, "The system refused a power request (" + errorText(error) + ")");
            }
            types_.push_back(type);
        }
    }
    ~PowerRequest() override { release(); }

private:
    void release() noexcept {
        for (const auto type : types_) {
            PowerClearRequest(handle_, type);
        }
        types_.clear();
        if (handle_) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    HANDLE handle_ = nullptr;
    std::vector<POWER_REQUEST_TYPE> types_; // The ones set.
};

} // namespace

std::unique_ptr<Request> systemRequest(rcs_keep_awake mode, const std::string& reason) {
    return std::make_unique<PowerRequest>(mode, reason);
}

bool systemAvailable() {
    return true;
}

} // namespace rcsdk::power
