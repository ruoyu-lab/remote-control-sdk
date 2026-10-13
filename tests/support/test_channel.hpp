// A host's own channel between sessions in one process (RCS_TRANSPORT_HOST),
// doing what real channels do: a datagram channel loses, reorders, repeats
// and delays datagrams; a stream carries bytes in order, cut into pieces of
// any size; either may have no room now and then, or close. End 0 is the
// sharer's, ends 1 and up viewers' (the sharer's route to end n is n), each
// with a channel of its own to the sharer. Deliveries come from the channel's
// own thread, as from a host's network code. Attach each session for as long
// as it lives (attach() returns a guard that detaches it); declare the
// channel before the sessions.
#pragma once

#include <rcsdk/host_transport.hpp>
#include <rcsdk/rcsdk.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace rcsdk::test {

// A session's end of a host channel that calls a function, e.g. TestChannel::sender.
class FunctionTransport final : public rcsdk::HostTransport {
public:
    explicit FunctionTransport(std::function<bool(uint64_t, const uint8_t*, uint32_t)> send) : send_(std::move(send)) {}
    bool send(uint64_t route, const uint8_t* data, uint32_t size) override { return send_(route, data, size); }

private:
    std::function<bool(uint64_t, const uint8_t*, uint32_t)> send_;
};

class TestChannel {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int maxEnds = 16;

    explicit TestChannel(rcs_host_channel kind) : kind_(kind), thread_([this] { run(); }) {}
    ~TestChannel() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        changed_.notify_all();
        thread_.join();
    }
    TestChannel(const TestChannel&) = delete;
    TestChannel& operator=(const TestChannel&) = delete;

    // Impairments (datagram: loss, reorder, repeat; both: refuse), each a
    // probability per datagram; a stream's pieces are 1..maxPiece bytes.
    std::atomic<double> loss{0}, reorder{0}, repeat{0}, refuse{0};
    std::atomic<size_t> maxPiece{4096};
    std::atomic<uint64_t> refused{0};

    // What `end` sends goes to the other end of its channel: the session's
    // on_transport_send (FunctionTransport). The sharer's (end 0) goes to the
    // end its route names; a viewer's to the sharer.
    std::function<bool(uint64_t, const uint8_t*, uint32_t)> sender(int end) {
        return [this, end](uint64_t route, const uint8_t* data, uint32_t size) {
            return send(end, end ? 0 : int(route), data, size);
        };
    }

    class Attachment {
    public:
        Attachment(TestChannel& channel, int end) : channel_(channel), end_(end) {}
        ~Attachment() { channel_.attach(end_, nullptr); }
        Attachment(const Attachment&) = delete;
        Attachment& operator=(const Attachment&) = delete;

    private:
        TestChannel& channel_;
        int end_;
    };
    // Deliveries to `end` go to `session` until the guard goes.
    [[nodiscard]] std::unique_ptr<Attachment> attachment(int end, rcsdk::Session& session) {
        attach(end, &session);
        return std::make_unique<Attachment>(*this, end);
    }

    // The channel between the sharer and viewer `end` closes and opens again:
    // what is on the way is lost, its streams start over, and both ends hear
    // it (rcs_session_transport_closed).
    void close(int end = 1) {
        std::lock_guard lock(mutex_);
        forget(end);
        if (auto* sharer = sessions_[0]) {
            rcsdk::transportClosed(*sharer, uint64_t(end));
        }
        if (auto* viewer = sessions_[end]) {
            rcsdk::transportClosed(*viewer, 0);
        }
    }

    // The channel between the sharer and viewer `end` is replaced by a new
    // one to the same ends (a WebSocket that reconnected): what is on the way
    // is lost, a stream's after a few of its bytes went (a datagram cut
    // short), and both ends hear it (rcs_session_transport_restarted).
    void restart(int end = 1) {
        std::lock_guard lock(mutex_);
        for (const auto& [from, to] : {std::pair(0, end), std::pair(end, 0)}) {
            auto& stream = streams_[{from, to}];
            if (!stream.empty()) {
                deliver(from, to, stream.data(), std::min<size_t>(stream.size(), 3));
            }
        }
        forget(end);
        if (auto* sharer = sessions_[0]) {
            rcsdk::transportRestarted(*sharer, uint64_t(end));
        }
        if (auto* viewer = sessions_[end]) {
            rcsdk::transportRestarted(*viewer, 0);
        }
    }

    // Results of deliveries other than RCS_OK and INVALID_STATE (a stopped session).
    std::atomic<int> unexpected{0};

private:
    struct Datagram {
        int from, to;
        std::vector<uint8_t> bytes;
        Clock::time_point due;
    };

    void attach(int end, rcsdk::Session* session) {
        std::lock_guard lock(mutex_);
        sessions_[end] = session;
    }

    double chance() { return std::uniform_real_distribution<double>(0, 1)(random_); }

    // What is on its way between the sharer and `end`, either way (holding mutex_).
    void forget(int end) {
        std::erase_if(inFlight_, [&](const Datagram& d) { return d.from == end || d.to == end; });
        streams_[{0, end}].clear();
        streams_[{end, 0}].clear();
    }

    bool send(int from, int to, const uint8_t* data, uint32_t size) {
        std::lock_guard lock(mutex_);
        if (to < 0 || to >= maxEnds || (from && to) || (!from && !to)) {
            ++unexpected; // A route the sharer never heard of, or a viewer's other than 0.
            return false;
        }
        if (chance() < refuse.load()) {
            ++refused;
            return false;
        }
        const auto now = Clock::now();
        if (kind_ == RCS_HOST_CHANNEL_STREAM) {
            auto& stream = streams_[{from, to}];
            stream.insert(stream.end(), data, data + size);
        } else if (chance() >= loss.load()) {
            // Reordered: held back a few milliseconds, so later ones overtake it.
            const auto delay = chance() < reorder.load() ? std::chrono::milliseconds(3 + int(chance() * 5))
                                                         : std::chrono::milliseconds(0);
            inFlight_.push_back({from, to, std::vector<uint8_t>(data, data + size), now + delay});
            if (chance() < repeat.load()) {
                inFlight_.push_back(
                    {from, to, std::vector<uint8_t>(data, data + size), now + delay + std::chrono::milliseconds(1)});
            }
        }
        changed_.notify_all();
        return true;
    }

    // To `to` from `from`: the sharer hears the viewer's route.
    void deliver(int from, int to, const uint8_t* data, size_t size) {
        auto* session = sessions_[to];
        if (!session) {
            return; // Nobody at that end.
        }
        const rcs_result result = rcsdk::transportReceive(*session, to ? 0 : uint64_t(from), data, uint32_t(size));
        if (result != RCS_OK && result != RCS_ERROR_INVALID_STATE) {
            ++unexpected;
        }
    }

    void run() {
        std::unique_lock lock(mutex_);
        while (!stop_) {
            const auto now = Clock::now();
            // Datagrams that are due, in the order they fall due.
            std::stable_sort(inFlight_.begin(), inFlight_.end(),
                             [](const Datagram& a, const Datagram& b) { return a.due < b.due; });
            while (!inFlight_.empty() && inFlight_.front().due <= now) {
                const Datagram d = std::move(inFlight_.front());
                inFlight_.pop_front();
                deliver(d.from, d.to, d.bytes.data(), d.bytes.size());
            }
            // Each stream's bytes, in pieces.
            for (auto& [ends, stream] : streams_) {
                while (!stream.empty()) {
                    const size_t piece = std::min(
                        stream.size(), std::uniform_int_distribution<size_t>(1, std::max<size_t>(1, maxPiece))(random_));
                    const std::vector<uint8_t> bytes(stream.begin(), stream.begin() + ptrdiff_t(piece));
                    stream.erase(stream.begin(), stream.begin() + ptrdiff_t(piece));
                    deliver(ends.first, ends.second, bytes.data(), bytes.size());
                }
            }
            if (inFlight_.empty()) {
                changed_.wait_for(lock, std::chrono::milliseconds(50));
            } else {
                changed_.wait_until(lock, inFlight_.front().due);
            }
        }
    }

    const rcs_host_channel kind_;
    std::mutex mutex_; // Everything below; deliveries happen holding it, as attach() waits for them.
    std::condition_variable changed_;
    std::deque<Datagram> inFlight_;
    std::map<std::pair<int, int>, std::vector<uint8_t>> streams_; // By (from, to).
    rcsdk::Session* sessions_[maxEnds] = {};
    std::mt19937 random_{12345};
    bool stop_ = false;
    std::thread thread_;
};

} // namespace rcsdk::test
