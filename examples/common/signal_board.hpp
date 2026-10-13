// A host's signaling channel for P2P and RELAY through a signal room of
// rcsdk_host_relay (examples/host-relay): every line a member sends reaches
// the others. How a host whose server already carries its users' messages
// would carry the SDK's signals: the text is opaque, and the host adds whom
// it is from or for. rcsdk_cli writes "view <id> <signal>" for a viewer's and
// "share <id> <signal>" for the sharer's answer to viewer <id>; the sharer
// passes <id> as the route (rcs_session_signal) and gets it back with its
// answer (on_signal). Test software, not part of the SDK.
#pragma once

#include "relay_socket.hpp"

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace relay {

class SignalBoard {
public:
    using Line = std::function<void(const std::string& line)>;

    // Connects to the relay and joins `room`; `line` gets each line the
    // other members send, on the board's thread. Throws when the relay
    // cannot be reached.
    SignalBoard(const std::string& relayAddress, const std::string& room, Line line) : line_(std::move(line)) {
        const Address address = parseAddress(relayAddress);
        socket_ = openSocket(address.storage.ss_family, SOCK_STREAM);
        if (::connect(socket_, address.get(), address.length) != 0) {
            closeSocket(socket_);
            throw std::runtime_error("cannot reach the signal relay at " + relayAddress);
        }
        send("RCSSIGNAL " + room);
        thread_ = std::thread([this] { run(); });
    }
    ~SignalBoard() {
        running_ = false;
        shutdownSocket(socket_);
        thread_.join();
        closeSocket(socket_);
    }
    SignalBoard(const SignalBoard&) = delete;
    SignalBoard& operator=(const SignalBoard&) = delete;

    // One line to every other member (any thread).
    void send(const std::string& text) {
        const std::string line = text + "\n";
        std::lock_guard lock(mutex_);
        for (size_t sent = 0; sent < line.size();) {
            const auto wrote = ::send(socket_, line.data() + sent, int(line.size() - sent), 0);
            if (wrote <= 0) {
                std::fprintf(stderr, "signal relay: the connection broke\n");
                return;
            }
            sent += size_t(wrote);
        }
    }

private:
    void run() {
        std::string line;
        char buffer[4096];
        while (running_) {
            const auto got = ::recv(socket_, buffer, int(sizeof(buffer)), 0);
            if (got <= 0) {
                if (running_) {
                    std::fprintf(stderr, "signal relay: the connection closed\n");
                }
                return;
            }
            for (int i = 0; i < int(got); ++i) {
                if (buffer[i] != '\n') {
                    line.push_back(buffer[i]);
                    continue;
                }
                line_(line);
                line.clear();
            }
        }
    }

    Line line_;
    Handle socket_ = noSocket;
    std::mutex mutex_;
    std::atomic<bool> running_{true};
    std::thread thread_; // Last.
};

} // namespace relay
