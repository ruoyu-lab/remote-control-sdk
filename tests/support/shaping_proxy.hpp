// A UDP proxy that shapes one direction like a bottleneck: limited bandwidth
// with a bounded queue (what overflows is dropped), extra delay, random loss,
// and what wireless adds: loss in bursts, a receiver whose radio sleeps to
// save power (Wi-Fi's power save: the access point holds what arrives for it
// and hands it all over when it wakes), and Wi-Fi's airtime: what waits goes
// out in aggregates, each after the medium is won. Tests put it between a
// sharer and a viewer.
//
// It maps each viewer's address to a port of its own towards the target, as a
// NAT does: rebind() forgets the mappings (a NAT that dropped or remapped
// them: the target sees the viewers come from new ports), block() drops
// what a viewer's address sends (its network went away), and a filtering NAT
// lets in only what comes from where the viewer sent to. With `upstream` it
// shapes the other way: what the viewers send to the target (an end's
// uplink, e.g. a sharer's to its forwarder).
#pragma once

#include "transport/socket.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace rcsdk::test {

class ShapingProxy {
public:
    // Viewers send to address(); datagrams go on to `target`. Traffic from the
    // target back to the viewer is shaped.
    explicit ShapingProxy(const transport::Endpoint& target, const std::string& front = "127.0.0.1",
                          uint16_t frontPort = 0, const std::string& back = "127.0.0.1")
        : target_(target), front_(transport::Endpoint::parse(front, frontPort)),
          backHost_(transport::Endpoint::parse(back, 0)) {
        mappings_.push_back({{}, std::make_shared<transport::UdpSocket>(backHost_)}); // For the first viewer.
        thread_ = std::thread([this] { run(); });
    }
    ~ShapingProxy() {
        stop_ = true;
        thread_.join();
    }
    transport::Endpoint address() const { return front_.local(); }
    // Where the target sees the latest viewer's datagrams come from (the first
    // viewer's before any came).
    transport::Endpoint backAddress() const {
        std::lock_guard lock(mutex_);
        return mappings_.back().back->local();
    }
    // The viewers' next datagrams come from new ports; what the target sends
    // to the old ones is lost.
    void rebind() {
        std::lock_guard lock(mutex_);
        mappings_.clear();
    }
    // What `viewer` sends is dropped from now on, and nothing reaches it, until unblock().
    void block(const transport::Endpoint& viewer) {
        std::lock_guard lock(mutex_);
        blocked_.push_back(viewer);
    }
    void unblock(const transport::Endpoint& viewer) {
        std::lock_guard lock(mutex_);
        std::erase(blocked_, viewer);
    }
    // The viewers' addresses as the proxy saw them, the latest last.
    std::vector<transport::Endpoint> viewers() const {
        std::lock_guard lock(mutex_);
        std::vector<transport::Endpoint> list;
        for (const auto& mapping : mappings_) {
            if (mapping.viewer.valid()) {
                list.push_back(mapping.viewer);
            }
        }
        return list;
    }

    std::atomic<uint64_t> bitsPerSecond{0}; // 0: unlimited.
    std::atomic<uint32_t> queueMs{150};
    std::atomic<uint32_t> delayMs{0};
    std::atomic<uint32_t> lossPermille{0};
    // Each datagram starts a run of burstLength lost ones with this chance.
    std::atomic<uint32_t> burstLossPermille{0}, burstLength{0};
    // Every sleepEveryMs the viewer's radio sleeps for sleepMs: what would
    // reach it meanwhile waits and comes when it wakes.
    std::atomic<uint32_t> sleepEveryMs{0}, sleepMs{0};
    // Wi-Fi's airtime, with bitsPerSecond (0: one datagram after another, as
    // a wire): what waits leaves in aggregates of up to aggregateUs of line
    // time, each arriving whole once its last datagram is through (a receiver
    // gets an aggregate at once). Before each, the medium is won after up to
    // accessUs (the backoff), and with a chance of busyPermille other stations
    // hold it first for up to busyUs: the airtime they take.
    std::atomic<uint32_t> aggregateUs{0}, accessUs{0}, busyPermille{0}, busyUs{0};
    std::atomic<bool> outage{false};   // Nothing passes in either direction: the path is down.
    std::atomic<bool> upstream{false}; // Shape what goes to the target, not what comes back.
    std::atomic<uint64_t> dropped{0}, forwarded{0};

    // How long each shaped datagram was held, from its arrival to its release,
    // while recording: what queued it at the bottleneck (and the path's delay).
    struct Hold {
        size_t bytes;
        std::chrono::nanoseconds held;
    };
    void record(bool on) { recording_ = on; }
    std::vector<Hold> takeHolds() {
        std::lock_guard lock(holdsMutex_);
        return std::exchange(holds_, {});
    }

private:
    struct Mapping {
        transport::Endpoint viewer; // None yet: the first viewer's.
        std::shared_ptr<transport::UdpSocket> back;
    };
    struct Pending {
        Clock::time_point arrived, release;
        transport::Endpoint to;
        std::vector<uint8_t> bytes;
        std::shared_ptr<transport::UdpSocket> via; // Towards the target through a mapping; none: to a viewer.
    };

    bool blocked(const transport::Endpoint& viewer) const {
        return std::find(blocked_.begin(), blocked_.end(), viewer) != blocked_.end();
    }

    void run() {
        std::array<uint8_t, 2048> buffer;
        transport::Endpoint from;
        Clock::time_point lineFree = Clock::now();
        std::deque<Pending> queue;   // On the way, in release order.
        std::deque<Pending> waiting; // Airtime: for the next aggregate.
        size_t waitingBytes = 0;
        std::optional<Clock::time_point> nextAggregate;
        uint32_t state = 7;
        const auto random = [&](uint32_t below) {
            state = state * 1103515245 + 12345;
            return below ? (state >> 16) % below : 0;
        };
        uint32_t burst = 0; // Datagrams still to lose in the current run.
        const auto start = Clock::now();
        // When a datagram due at `release` reaches the viewer: once its radio is awake.
        const auto awake = [&](Clock::time_point release) {
            const uint32_t every = sleepEveryMs, sleep = sleepMs;
            if (!every || !sleep) {
                return release;
            }
            const auto period = std::chrono::milliseconds(every);
            const auto into = (release - start) % period;
            return into < std::chrono::milliseconds(sleep) ? release - into + std::chrono::milliseconds(sleep) : release;
        };
        const auto lineTime = [&](size_t bytes) {
            return std::chrono::nanoseconds(uint64_t(bytes) * 8 * 1'000'000'000 / bitsPerSecond);
        };
        // The medium won after the aggregate before it, from `from`.
        const auto won = [&](Clock::time_point from) {
            auto at = from + std::chrono::microseconds(random(accessUs + 1));
            if (random(1000) < busyPermille) {
                at += std::chrono::microseconds(random(busyUs + 1));
            }
            return at;
        };
        const auto deliver = [&](Pending pending, Clock::time_point through) {
            pending.release = awake(through + std::chrono::milliseconds(delayMs.load()));
            queue.push_back(std::move(pending));
        };
        // The shaped direction: loss, then the bottleneck (its queue, its line or airtime).
        const auto shape = [&](Pending pending) {
            if (!burst && random(1000) < burstLossPermille) {
                burst = burstLength;
            }
            if (burst || random(1000) < lossPermille) {
                burst -= burst ? 1 : 0;
                ++dropped;
                return;
            }
            const auto now = pending.arrived;
            const size_t size = pending.bytes.size();
            const uint64_t rate = bitsPerSecond;
            if (rate && aggregateUs) {
                const auto ahead = std::max(lineFree, now) - now + lineTime(waitingBytes + size);
                if (ahead > std::chrono::milliseconds(queueMs.load())) {
                    ++dropped; // The bottleneck's queue is full.
                    return;
                }
                waitingBytes += size;
                waiting.push_back(std::move(pending));
                if (!nextAggregate) {
                    nextAggregate = won(std::max(lineFree, now));
                }
                return;
            }
            auto departure = std::max(now, lineFree);
            if (rate) {
                departure += lineTime(size);
                if (departure - now > std::chrono::milliseconds(queueMs.load())) {
                    ++dropped; // The bottleneck's queue is full.
                    return;
                }
                lineFree = departure;
            }
            deliver(std::move(pending), departure);
        };
        while (!stop_) {
            bool idle = true;
            {
                std::lock_guard lock(mutex_);
                while (const auto size = front_.receive(buffer, from)) { // Viewer -> sharer.
                    idle = false;
                    if (outage || blocked(from)) {
                        continue;
                    }
                    auto mapping = std::find_if(mappings_.begin(), mappings_.end(),
                                                [&](const Mapping& m) { return m.viewer == from; });
                    if (mapping == mappings_.end()) {
                        mapping = std::find_if(mappings_.begin(), mappings_.end(),
                                               [](const Mapping& m) { return !m.viewer.valid(); });
                    }
                    if (mapping == mappings_.end()) {
                        mappings_.push_back({from, std::make_shared<transport::UdpSocket>(backHost_)});
                        mapping = mappings_.end() - 1;
                    }
                    mapping->viewer = from;
                    if (upstream) {
                        shape(Pending{Clock::now(),
                                      {},
                                      target_,
                                      std::vector<uint8_t>(buffer.begin(), buffer.begin() + ptrdiff_t(*size)),
                                      mapping->back});
                    } else {
                        mapping->back->send(target_, std::span(buffer).first(*size));
                    }
                }
                for (const auto& mapping : mappings_) {
                    while (const auto size = mapping.back->receive(buffer, from)) { // Sharer -> viewer.
                        idle = false;
                        if (outage || !mapping.viewer.valid() || blocked(mapping.viewer)) {
                            ++dropped;
                        } else if (upstream) {
                            front_.send(mapping.viewer, std::span(buffer).first(*size));
                        } else {
                            shape(Pending{Clock::now(),
                                          {},
                                          mapping.viewer,
                                          std::vector<uint8_t>(buffer.begin(), buffer.begin() + ptrdiff_t(*size)),
                                          nullptr});
                        }
                    }
                }
            }
            const auto now = Clock::now();
            if (nextAggregate && now >= *nextAggregate) {
                // What arrived by now, up to the aggregate's airtime (at least one datagram).
                const auto most = std::chrono::microseconds(aggregateUs.load());
                std::chrono::nanoseconds air{0};
                std::vector<Pending> aggregate;
                while (!waiting.empty() && waiting.front().arrived <= now &&
                       (aggregate.empty() || air + lineTime(waiting.front().bytes.size()) <= most)) {
                    air += lineTime(waiting.front().bytes.size());
                    waitingBytes -= waiting.front().bytes.size();
                    aggregate.push_back(std::move(waiting.front()));
                    waiting.pop_front();
                }
                lineFree = now + air;
                for (auto& pending : aggregate) {
                    deliver(std::move(pending), lineFree);
                }
                nextAggregate = waiting.empty() ? std::nullopt : std::optional(won(lineFree));
            }
            while (!queue.empty() && queue.front().release <= Clock::now()) {
                if (recording_) {
                    std::lock_guard lock(holdsMutex_);
                    holds_.push_back({queue.front().bytes.size(), Clock::now() - queue.front().arrived});
                }
                if (queue.front().via) {
                    queue.front().via->send(queue.front().to, queue.front().bytes);
                } else {
                    front_.send(queue.front().to, queue.front().bytes);
                }
                queue.pop_front();
                ++forwarded;
            }
            if (idle) {
                // Not a sleep: on Windows one lasts a whole timer tick (15.6 ms), which would
                // release packets in bursts and look like queueing on the path.
                std::this_thread::yield();
            }
        }
    }

    transport::Endpoint target_;
    transport::UdpSocket front_;
    const transport::Endpoint backHost_;
    mutable std::mutex mutex_; // The mappings and blocks, which the test changes.
    std::vector<Mapping> mappings_;
    std::vector<transport::Endpoint> blocked_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> recording_{false};
    std::mutex holdsMutex_;
    std::vector<Hold> holds_;
    std::thread thread_;
};

} // namespace rcsdk::test
