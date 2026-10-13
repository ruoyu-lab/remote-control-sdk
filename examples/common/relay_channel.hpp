// A host's side of RCS_TRANSPORT_HOST through rcsdk_host_relay
// (examples/host-relay): how a host that already has a server would carry the
// session over it. STREAM: the session's bytes over a TCP connection to the
// relay; DATAGRAM: its datagrams over UDP. A viewer's channel leads to the
// sharer of its room; a sharer's to each viewer of its room, under the route
// the relay gives each (its connection carries them all, each piece behind
// its route: relay_socket.hpp). When the relay closes the connection, says
// the other end changed or that a viewer left, the session hears that the
// channel (to that route) closed; a TCP channel then connects again.
// Test software, not part of the SDK.
#pragma once

#include "relay_socket.hpp"

#include <rcsdk/host_transport.hpp>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace relay {

class RelayChannel final : public rcsdk::HostTransport {
public:
    // What may wait for the relay connection: beyond it the channel has no room.
    static constexpr size_t maxWaiting = 4u << 20;

    // `share`: the room's sharer, with a route to each viewer.
    RelayChannel(const std::string& relayAddress, std::string room, rcs_host_channel kind, bool share = false)
        : address_(parseAddress(relayAddress)), room_(std::move(room)), kind_(kind), share_(share) {}
    ~RelayChannel() override { stop(); }
    RelayChannel(const RelayChannel&) = delete;
    RelayChannel& operator=(const RelayChannel&) = delete;

    // The session's on_transport_send (rcsdk::hostTransport). False while the channel is down or full.
    bool send(uint64_t route, const uint8_t* data, uint32_t size) override {
        std::lock_guard lock(mutex_);
        if (socket_ == noSocket) {
            return false;
        }
        std::vector<uint8_t> routed;
        if (share_) { // Behind its route (and, over TCP, its length).
            putNumber(routed, route, 8);
            if (kind_ == RCS_HOST_CHANNEL_STREAM) {
                putNumber(routed, size, 4);
            }
            routed.insert(routed.end(), data, data + size);
            data = routed.data();
            size = uint32_t(routed.size());
        }
        if (kind_ == RCS_HOST_CHANNEL_DATAGRAM) {
            return ::send(socket_, reinterpret_cast<const char*>(data), int(size), 0) == int(size);
        }
        if (waiting_.size() + size > maxWaiting) {
            return false;
        }
        waiting_.insert(waiting_.end(), data, data + size);
        flush();
        return true;
    }

    // Deliveries go to `session` until stop(); stop the session first, so
    // that it calls send() no more.
    void start(rcsdk::Session& session) {
        session_ = &session;
        running_ = true;
        thread_ = std::thread([this] {
            try {
                kind_ == RCS_HOST_CHANNEL_STREAM ? runStream() : runDatagrams();
            } catch (const std::exception& error) {
                log(std::string("stopped: ") + error.what());
            }
        });
    }
    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        thread_.join();
        std::lock_guard lock(mutex_);
        if (socket_ != noSocket) {
            if (kind_ == RCS_HOST_CHANNEL_DATAGRAM) {
                const std::string leave = std::string(tag) + "leave";
                ::send(socket_, leave.data(), int(leave.size()), 0);
            }
            closeSocket(socket_);
            socket_ = noSocket;
        }
    }

private:
    void log(const std::string& text) {
        std::printf("relay channel: %s\n", text.c_str());
        std::fflush(stdout);
    }

    std::string hello() const { return std::string(share_ ? shareTag : tag) + room_; }

    // Writes what waits, as far as the connection takes it (holding mutex_).
    void flush() {
        while (!waiting_.empty()) {
            const int sent = int(::send(socket_, reinterpret_cast<const char*>(waiting_.data()), int(waiting_.size()), 0));
            if (sent <= 0) {
                return; // Full (the I/O thread goes on when it has room), or broken (the reader notices).
            }
            waiting_.erase(waiting_.begin(), waiting_.begin() + sent);
        }
    }

    // A sharer: the routes it heard from, each told when its channel closed.
    void heard(uint64_t route) { routes_.insert(route); }
    void closeRoute(uint64_t route) {
        routes_.erase(route);
        rcsdk::transportClosed(*session_, route);
    }
    void closeAll() {
        if (!share_) {
            rcsdk::transportClosed(*session_, 0);
            return;
        }
        while (!routes_.empty()) {
            closeRoute(*routes_.begin());
        }
    }

    // A sharer's stream: route(8) length(4) bytes, again and again; length 0:
    // that viewer left.
    void routedStream(std::vector<uint8_t>& pending, const uint8_t* data, size_t size) {
        pending.insert(pending.end(), data, data + size);
        size_t at = 0;
        while (pending.size() - at >= routeHeader) {
            const uint64_t route = getNumber(pending.data() + at, 8);
            const size_t length = size_t(getNumber(pending.data() + at + 8, 4));
            if (pending.size() - at - routeHeader < length) {
                break;
            }
            if (length) {
                heard(route);
                rcsdk::transportReceive(*session_, route, pending.data() + at + routeHeader, uint32_t(length));
            } else {
                closeRoute(route);
            }
            at += routeHeader + length;
        }
        pending.erase(pending.begin(), pending.begin() + ptrdiff_t(at));
    }

    void runStream() {
        std::vector<uint8_t> buffer(65536), pending;
        while (running_) {
            const Handle socket = openSocket(address_.storage.ss_family, SOCK_STREAM);
            if (::connect(socket, address_.get(), address_.length) != 0) {
                closeSocket(socket);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            const std::string line = hello() + "\n";
            ::send(socket, line.data(), int(line.size()), 0);
            makeNonBlocking(socket);
            {
                std::lock_guard lock(mutex_);
                socket_ = socket;
                waiting_.clear();
            }
            pending.clear();
            log("connected to the relay, room \"" + room_ + "\" (TCP" + (share_ ? ", sharer)" : ")"));
            bool open = true;
            while (running_ && open) {
                bool writing;
                {
                    std::lock_guard lock(mutex_);
                    flush();
                    writing = !waiting_.empty();
                }
                PollItem item{socket, short(readEvent | (writing ? writeEvent : 0)), 0};
                pollItems(&item, 1, writing ? 5 : 100);
                if (!(item.revents & ~writeEvent)) {
                    continue;
                }
                for (;;) {
                    const int got = int(::recv(socket, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0));
                    if (got < 0 && wouldBlock()) {
                        break;
                    }
                    if (got <= 0) {
                        open = false;
                        break;
                    }
                    if (share_) {
                        routedStream(pending, buffer.data(), size_t(got));
                    } else {
                        rcsdk::transportReceive(*session_, 0, buffer.data(), uint32_t(got));
                    }
                }
            }
            {
                std::lock_guard lock(mutex_);
                socket_ = noSocket;
                waiting_.clear();
            }
            closeSocket(socket);
            if (running_) {
                log("the relay closed the channel; connecting again");
                closeAll();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }

    void runDatagrams() {
        const Handle socket = openSocket(address_.storage.ss_family, SOCK_DGRAM);
        ::connect(socket, address_.get(), address_.length); // The relay is the only peer.
        makeNonBlocking(socket);
        {
            std::lock_guard lock(mutex_);
            socket_ = socket;
        }
        log("sending to the relay, room \"" + room_ + "\" (UDP" + (share_ ? ", sharer)" : ")"));
        const std::string greeting = hello();
        const std::string changed = std::string(tag) + "changed";
        const std::string closed = std::string(tag) + "closed";
        std::vector<uint8_t> buffer(65536);
        auto nextHello = std::chrono::steady_clock::now();
        while (running_) {
            if (std::chrono::steady_clock::now() >= nextHello) {
                ::send(socket, greeting.data(), int(greeting.size()), 0);
                nextHello = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            if (!readable(socket, 100)) {
                continue;
            }
            for (;;) {
                const int got = int(::recv(socket, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0));
                if (got <= 0) {
                    break; // Nothing more now (or an ICMP report: the relay is not there yet).
                }
                const auto is = [&](const std::string& text, size_t at) {
                    return size_t(got) == at + text.size() && std::memcmp(buffer.data() + at, text.data(), text.size()) == 0;
                };
                if (!share_) {
                    if (is(changed, 0)) {
                        log("the other end changed");
                        rcsdk::transportClosed(*session_, 0);
                        continue;
                    }
                    rcsdk::transportReceive(*session_, 0, buffer.data(), uint32_t(got));
                    continue;
                }
                if (size_t(got) <= 8) {
                    continue;
                }
                const uint64_t route = getNumber(buffer.data(), 8);
                if (is(closed, 8)) {
                    closeRoute(route);
                    continue;
                }
                heard(route);
                rcsdk::transportReceive(*session_, route, buffer.data() + 8, uint32_t(got - 8));
            }
        }
    }

    const Address address_;
    const std::string room_;
    const rcs_host_channel kind_;
    const bool share_;
    rcsdk::Session* session_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_; // socket_ and waiting_: the session's thread sends, the I/O thread reads.
    Handle socket_ = noSocket;
    std::vector<uint8_t> waiting_; // STREAM: bytes the connection has not taken yet.
    std::set<uint64_t> routes_;    // A sharer's, as heard (the I/O thread's).
};

} // namespace relay
