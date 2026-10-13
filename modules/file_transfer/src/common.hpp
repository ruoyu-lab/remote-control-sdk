// What the file transfer module's code shares. The module builds on the
// SDK's public headers alone (rcsdk/module.h), as anyone's module would.
#pragma once

#include <rcsdk/file_transfer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rcsdk::file_transfer {

using Bytes = std::vector<uint8_t>;
using Token = std::array<uint8_t, RCS_FILE_TOKEN_SIZE>;

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

inline constexpr uint32_t moduleVersion = 1;
inline constexpr uint32_t chunkBytes = 256u << 10;          // File bytes in one message.
inline constexpr uint64_t windowBytes = 4u << 20;           // Of one transfer on their way at a time.
inline constexpr uint64_t readWindowBytes = RCS_FILE_READS_MAX; // Of a READ fetch's reads not answered yet.
inline constexpr uint32_t maxPublicationsLimit = 10'000;    // max_publications' range.
inline constexpr uint32_t maxFetchesLimit = 64;             // max_fetches'.
inline constexpr uint32_t maxItemsLimit = 100'000;          // rcs_file_transfer_settings.max_items' range.
inline constexpr uint32_t maxPendingOffersLimit = 10'000;   // max_pending_offers' range.
inline constexpr uint32_t maxPendingItemsLimit = 1'000'000; // max_pending_items' and max_pending_items_total's.
inline constexpr size_t maxText = 1024;                     // Bytes of a reason in a message.
// The most a message holds, either way and between the halves: an offer of
// many items, or a list of them with where they are on the machine.
inline constexpr uint32_t maxMessageBytes = (16u << 20) + 4096;

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

// The token as 32 lowercase hex digits.
std::string hex(const Token& token);

// Little-endian numbers and length-prefixed text in the module's messages.
class Writer {
public:
    Writer& u8(uint8_t value) {
        bytes_.push_back(value);
        return *this;
    }
    Writer& u16(uint16_t value) { return u8(uint8_t(value)).u8(uint8_t(value >> 8)); }
    Writer& u32(uint32_t value) { return u16(uint16_t(value)).u16(uint16_t(value >> 16)); }
    Writer& u64(uint64_t value) { return u32(uint32_t(value)).u32(uint32_t(value >> 32)); }
    Writer& i32(int32_t value) { return u32(uint32_t(value)); }
    Writer& i64(int64_t value) { return u64(uint64_t(value)); }
    // Text of at most `most` bytes (cut short at a character's start), its size first.
    Writer& text(std::string_view value, size_t most = maxText);
    Writer& raw(std::span<const uint8_t> data) {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
        return *this;
    }
    Writer& token(const Token& value) { return raw(value); }
    size_t size() const noexcept { return bytes_.size(); }
    Bytes take() { return std::move(bytes_); }

private:
    Bytes bytes_;
};

// Reads a message; throws Error(PROTOCOL) naming `what` for anything short or out of range.
class Reader {
public:
    Reader(std::span<const uint8_t> data, const char* what) : data_(data), what_(what) {}
    uint8_t u8() { return take(1)[0]; }
    uint16_t u16() { return uint16_t(u8() | (uint16_t(u8()) << 8)); }
    uint32_t u32() { return u16() | (uint32_t(u16()) << 16); }
    uint64_t u64() { return u32() | (uint64_t(u32()) << 32); }
    int32_t i32() { return int32_t(u32()); }
    int64_t i64() { return int64_t(u64()); }
    // A flag: 0 or 1.
    bool flag();
    // Text of at most `most` bytes, UTF-8.
    std::string text(size_t most = maxText);
    // An error some version of the SDK defines (below RCS_OK), or RCS_OK with `orOk`.
    rcs_result result(bool orOk = false);
    Token token();
    std::span<const uint8_t> rest() { return take(data_.size() - at_); }
    size_t remaining() const noexcept { return data_.size() - at_; }
    // Throws unless everything was read.
    void end();
    [[noreturn]] void invalid(const std::string& why) const;

private:
    std::span<const uint8_t> take(size_t size);

    std::span<const uint8_t> data_;
    const char* what_;
    size_t at_ = 0;
};

// Whether `text` is UTF-8 (no overlong forms or surrogates).
bool utf8(std::string_view text) noexcept;

} // namespace rcsdk::file_transfer
