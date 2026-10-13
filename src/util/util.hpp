// Small internal helpers shared by every module.
#pragma once

#include "util/api.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rcsdk {

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

inline uint64_t monotonicNs() noexcept {
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
#ifdef __EMSCRIPTEN__
    // A browser's threads are workers, each counting from its own start
    // (performance.timeOrigin, rounded in each): their clocks differ by tens
    // of microseconds. No reading goes below one any thread made before, so
    // a duration from one thread to another is never negative.
    static std::atomic<uint64_t> latest{0};
    uint64_t seen = latest.load(std::memory_order_relaxed);
    while (seen < now && !latest.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
    return std::max(now, seen);
#else
    return now;
#endif
}

// Set by stop(); every blocking operation polls it.
using CancelToken = const std::atomic<bool>&;

// Internal error carrying the public result code. Workers throw it; the session
// turns it into RCS_STATE_FAILED plus the last-error text.
class Failure : public std::runtime_error {
public:
    Failure(rcs_result result, const std::string& message) : std::runtime_error(message), result_(result) {}
    rcs_result result() const noexcept { return result_; }

private:
    rcs_result result_;
};

[[noreturn]] inline void fail(rcs_result result, const std::string& message) {
    throw Failure(result, message);
}

// A visitor of lambdas: std::visit(Overloaded{[](A) {...}, [](B) {...}}, variant).
template <class... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};
template <class... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

// Sleeps for `duration`, also where a plain sleep rounds up to the system's
// timer tick (Windows: 15.6 ms unless some process asked for finer). Throws
// Failure(INTERNAL) where the system has no precise timer.
void sleepPrecise(std::chrono::nanoseconds duration);

inline void throwIfCancelled(CancelToken cancelled) {
    if (cancelled.load()) {
        fail(RCS_ERROR_CANCELLED, "Operation cancelled");
    }
}

// UTF-8 text into a field of `capacity` bytes (at least 1), NUL-terminated,
// cut short at a character's start.
inline void copyText(char* field, size_t capacity, std::string_view text) noexcept {
    size_t size = std::min(text.size(), capacity - 1);
    while (size > 0 && size < text.size() && (uint8_t(text[size]) & 0xC0) == 0x80) {
        --size;
    }
    std::memcpy(field, text.data(), size);
    field[size] = '\0';
}
template <size_t N>
void copyText(char (&field)[N], std::string_view text) noexcept {
    copyText(field, N, text);
}

// The characters (code points) of UTF-8 `text`; nothing when it is not UTF-8.
inline std::optional<size_t> utf8Characters(std::string_view text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++count) {
        const auto lead = uint8_t(text[i]);
        const size_t length = lead < 0x80             ? 1
                              : (lead & 0xe0) == 0xc0 ? 2
                              : (lead & 0xf0) == 0xe0 ? 3
                              : (lead & 0xf8) == 0xf0 ? 4
                                                      : 0;
        if (!length || i + length > text.size()) {
            return std::nullopt;
        }
        for (size_t k = 1; k < length; ++k) {
            if ((uint8_t(text[i + k]) & 0xc0) != 0x80) {
                return std::nullopt;
            }
        }
        i += length;
    }
    return count;
}

// A duration for messages: "5 s", "1.5 s", "200 ms".
inline std::string secondsText(std::chrono::milliseconds duration) {
    const auto ms = duration.count();
    if (ms < 1000) {
        return std::to_string(ms) + " ms";
    }
    std::string text = std::to_string(ms / 1000);
    if (const auto rest = ms % 1000) {
        std::string fraction = std::to_string(rest + 1000).substr(1); // Three digits.
        fraction.erase(fraction.find_last_not_of('0') + 1);
        text += "." + fraction;
    }
    return text + " s";
}

} // namespace rcsdk
