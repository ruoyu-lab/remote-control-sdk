// What the clipboard module's code shares. The module builds on the SDK's
// public headers alone (rcsdk/module.h), as anyone's module would.
#pragma once

#include <rcsdk/clipboard.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rcsdk::clipboard {

using Bytes = std::vector<uint8_t>;

// A failure, with the SDK's result for it.
class Error : public std::runtime_error {
public:
    Error(rcs_result result, const std::string& message) : std::runtime_error(message), result_(result) {}
    rcs_result result() const noexcept { return result_; }

private:
    rcs_result result_;
};

[[noreturn]] inline void fail(rcs_result result, const std::string& message) {
    throw Error(result, message);
}

inline constexpr uint32_t allFormats = RCS_CLIPBOARD_ALL_FORMATS;
inline constexpr uint32_t filesFormat = 1u << RCS_CLIPBOARD_FILES;
inline constexpr uint32_t knownFormats = allFormats | filesFormat; // What some version defines.
// rcs_clipboard_settings.max_bytes' range.
inline constexpr size_t minBytes = 1024, maxBytes = 256u << 20;
inline constexpr size_t maxEntries = 100'000; // Of a copy's files (file transfer's max_items at most).

// The characters (code points) of UTF-8 `text`; nothing when it is not UTF-8.
std::optional<size_t> utf8Characters(std::string_view text);

// UTF-8 text into a field of N bytes, NUL-terminated, cut short at a character's start.
template <size_t N>
void copyText(char (&field)[N], std::string_view text) noexcept {
    size_t size = std::min(text.size(), N - 1);
    while (size > 0 && size < text.size() && (uint8_t(text[size]) & 0xC0) == 0x80) {
        --size;
    }
    std::memcpy(field, text.data(), size);
    field[size] = '\0';
}

// "TEXT, IMAGE" for (1u << rcs_clipboard_format) bits; "nothing" for none.
std::string formatList(uint32_t formats);

// Whether `path` is a full path on this system (Windows: of a drive or a share).
bool fullPath(std::string_view path) noexcept;

// Big-endian numbers in the module's messages.
void putNumber(Bytes& out, uint64_t value, size_t width);
// Throws Error(PROTOCOL) when `bytes` has fewer than `width` from `offset` on.
uint64_t getNumber(std::span<const uint8_t> bytes, size_t& offset, size_t width);

} // namespace rcsdk::clipboard
