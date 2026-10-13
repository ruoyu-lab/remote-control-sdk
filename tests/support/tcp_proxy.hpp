// A TCP proxy for the tests: each connection to address() gets one of its
// own to `target`, and what either side sends goes on to the other, as a
// network between them carries it. A test breaks the connections there are
// (cut(): both sides see them closed, as a reset or a NAT that dropped them),
// lets them carry nothing more while they stay open (freeze(): a network that
// went away under them, which TCP does not notice for long) and again
// (thaw()), and refuses new ones (refuse(): a target that cannot be reached).
// Each of those for one client's connection too (a viewer's of several), by
// the address it came from. What the target sends may go on no faster than a
// slow link carries it (downBps): the rest waits in the connections' buffers,
// as it would in the sender's.
#pragma once

#include "transport/socket.hpp"
#include "transport/tcp_stream.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace rcsdk::test {

class TcpProxy {
public:
    explicit TcpProxy(const transport::Endpoint& target)
        : target_(target), listener_(std::make_unique<transport::TcpListenSocket>("127.0.0.1", 0)),
          address_(transport::localAddress(listener_->handle())) {
        thread_ = std::thread([this] { run(); });
    }
    ~TcpProxy() {
        stop_ = true;
        thread_.join();
    }
    TcpProxy(const TcpProxy&) = delete;
    TcpProxy& operator=(const TcpProxy&) = delete;

    transport::Endpoint address() const { return address_; }
    // The clients of the connections open now, the oldest first.
    std::vector<transport::Endpoint> clients() const {
        std::lock_guard lock(mutex_);
        std::vector<transport::Endpoint> list;
        for (const auto& pair : pairs_) {
            list.push_back(pair->client);
        }
        return list;
    }
    // Connections taken so far.
    size_t accepted() const { return accepted_; }
    // What the target sends goes on at most this fast, in bits per second
    // (0: as fast as it comes).
    std::atomic<uint64_t> downBps{0};
    // Each connection open now (or `client`'s) is closed on both sides.
    void cut(std::optional<transport::Endpoint> client = std::nullopt) {
        std::lock_guard lock(mutex_);
        std::erase_if(pairs_, [&](const auto& pair) { return !client || pair->client == *client; });
    }
    // Each connection open now (or `client`'s) carries nothing more, or again.
    void freeze(std::optional<transport::Endpoint> client = std::nullopt) { setFrozen(client, true); }
    void thaw(std::optional<transport::Endpoint> client = std::nullopt) { setFrozen(client, false); }
    // New connections are refused (the port closes), or taken again.
    void refuse(bool refused) {
        std::lock_guard lock(mutex_);
        if (refused) {
            listener_.reset();
        } else if (!listener_) {
            listener_ = std::make_unique<transport::TcpListenSocket>("127.0.0.1", address_.port());
        }
    }

private:
    struct Pair {
        std::unique_ptr<transport::TcpStream> front, back; // The client's, and the one to the target.
        transport::Endpoint client;
        std::vector<uint8_t> toBack, toFront; // Read and not yet written.
        bool frozen = false;
        double budget = 0; // Bytes of the target's it may still pass on now (downBps).
    };

    void setFrozen(const std::optional<transport::Endpoint>& client, bool frozen) {
        std::lock_guard lock(mutex_);
        for (auto& pair : pairs_) {
            if (!client || pair->client == *client) {
                pair->frozen = frozen;
            }
        }
    }

    // What `from` sent, as far as `to` takes it, `most` bytes of it at most;
    // little is held here, so that a slow reader holds the sender back as the
    // network would. What it read.
    static size_t pump(transport::TcpStream& from, transport::TcpStream& to, std::vector<uint8_t>& held,
                       std::vector<uint8_t>& chunk, size_t most = SIZE_MAX) {
        size_t taken = 0;
        if (held.size() < (1u << 20) && most) {
            taken = from.readSome(std::span(chunk).first(std::min(chunk.size(), most)));
            held.insert(held.end(), chunk.begin(), chunk.begin() + ptrdiff_t(taken));
        }
        if (!held.empty()) {
            held.erase(held.begin(), held.begin() + ptrdiff_t(to.writeSome(held)));
        }
        return taken;
    }

    void run() {
        static const std::atomic<bool> never{false};
        std::vector<uint8_t> chunk(64 * 1024);
        auto refilled = std::chrono::steady_clock::now();
        while (!stop_) {
            std::vector<transport::Readiness> waiting;
            const uint64_t limit = downBps;
            {
                std::lock_guard lock(mutex_);
                if (listener_) {
                    waiting.push_back({listener_->handle(), false});
                }
                for (const auto& pair : pairs_) {
                    if (!pair->frozen) {
                        waiting.push_back({pair->front->handle(), !pair->toFront.empty()});
                        if (!limit || pair->budget >= 1) { // Without budget its data waits: no spinning on it.
                            waiting.push_back({pair->back->handle(), !pair->toBack.empty()});
                        }
                    }
                }
            }
            transport::waitAny(waiting, std::chrono::milliseconds(limit ? 1 : 10));
            const auto now = std::chrono::steady_clock::now();
            const double refill = std::chrono::duration<double>(now - refilled).count() * double(limit) / 8;
            refilled = now;
            std::lock_guard lock(mutex_);
            while (listener_) {
                std::unique_ptr<transport::TcpStream> front;
                try {
                    front = listener_->tryAccept(never);
                } catch (const Failure&) {
                    break;
                }
                if (!front) {
                    break;
                }
                auto pair = std::make_unique<Pair>();
                try {
                    pair->client = front->peer();
                    pair->back = transport::TcpStream::connect(target_.address(), target_.port(), never,
                                                               std::chrono::seconds(1));
                } catch (const Failure&) {
                    continue; // The target refused: so is the client (its connection closes).
                }
                pair->front = std::move(front);
                pairs_.push_back(std::move(pair));
                ++accepted_;
            }
            std::erase_if(pairs_, [&](const auto& pair) {
                if (pair->frozen) {
                    return false;
                }
                try {
                    pump(*pair->front, *pair->back, pair->toBack, chunk);
                    if (!limit) {
                        pump(*pair->back, *pair->front, pair->toFront, chunk);
                        return false;
                    }
                    pair->budget = std::min(pair->budget + refill, 16.0 * 1024); // A burst of 16 KiB at most.
                    const auto most = size_t(pair->budget);
                    pair->budget -= double(pump(*pair->back, *pair->front, pair->toFront, chunk, most));
                    return false;
                } catch (const Failure&) { // One side closed: so does the other.
                    return true;
                }
            });
        }
    }

    const transport::Endpoint target_;
    mutable std::mutex mutex_; // Below.
    std::unique_ptr<transport::TcpListenSocket> listener_;
    const transport::Endpoint address_;
    std::vector<std::unique_ptr<Pair>> pairs_;
    std::atomic<size_t> accepted_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_; // Last.
};

} // namespace rcsdk::test
