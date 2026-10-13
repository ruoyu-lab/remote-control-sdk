// Short stalls of a path, as the end that receives over it sees them
// (rcs_network_stats: stalls, RCS_EVENT_STALLS).
//
// A stall: nothing arrived for minimumGap or more while the other end kept
// sending. What ends the gap tells: a datagram it sent during the gap that
// waited on the path for half the gap or more (its one-way delay above the
// lowest of the last 10 s, from the send time video, parity and sound carry),
// or datagrams it sent during the gap that never came (two or more numbers
// skipped). A gap because the sender had nothing to send ends with a datagram
// that did not wait and nothing missing: no stall. A Wi-Fi radio that leaves
// its channel (Apple's AWDL, every 1024 TU = 1.05 s when the network is not
// on one of AWDL's channels; a scan) holds everything for tens of
// milliseconds, then delivers it at once. This end's network thread held up
// looks the same.
//
// The pace: stalls that come at a steady interval, or at whole multiples of
// it (a beat a radio skipped), most of those of the last 10 s on that beat.
// Pure logic, fed by the connection's network thread.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace rcsdk::protocol {

class StallWatch {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto minimumGap = std::chrono::milliseconds(30);
    static constexpr auto window = std::chrono::seconds(10);

    // A datagram that opened: its number, its send time where it carries one
    // (the sender's 32-bit microseconds), and when it was read (`arrivedNs`
    // on the clock the send times are compared with, and `now`).
    void arrived(uint64_t number, std::optional<uint32_t> sentUs, uint64_t arrivedNs, Clock::time_point now);
    // Another path: its delays and gaps start over; the stalls seen stay.
    void restart() noexcept;

    struct Summary {
        uint32_t count = 0;     // Stalls in the last 10 s...
        uint32_t medianMs = 0;  // ...the median one's length...
        uint32_t longestMs = 0; // ...the longest...
        uint32_t periodMs = 0;  // ...and their pace when they come steadily; 0 otherwise.
        uint64_t total = 0;     // Since this watch began.
        bool operator==(const Summary&) const = default;
    };
    Summary summary(Clock::time_point now) const;

private:
    // How long a datagram sent at `sentUs` and read at `arrivedNs` queued on
    // the path: its one-way delay above the lowest of each second of the last 10 s.
    std::chrono::microseconds queued(uint32_t sentUs, uint64_t arrivedNs, Clock::time_point now);
    void stalled(Clock::time_point start, Clock::duration length);

    // One-way delay (µs, arbitrary origin) and its lowest per second.
    std::optional<uint32_t> previousRaw_;
    int64_t delay_ = 0;
    std::deque<std::pair<Clock::time_point, int64_t>> lowest_;
    // The last datagram, and a gap that ended without telling yet (what
    // ended it carried no send time): the next that does, soon, decides.
    std::optional<Clock::time_point> last_;
    uint64_t highest_ = 0;
    struct Gap {
        Clock::time_point start;
        Clock::duration length{};
        Clock::time_point until;
    };
    std::optional<Gap> open_;
    std::deque<std::pair<Clock::time_point, uint32_t>> stalls_; // Start, length in ms.
    uint64_t total_ = 0;
};

} // namespace rcsdk::protocol
