// Systems that tell no one when their networks change (Android to native
// code, and the others without a layer of their own): a look every half
// second at the interfaces, their addresses and what networks() says of them.
#include "platform/platform.hpp"
#include "transport/interfaces.hpp"
#include "transport/network_changes.hpp"
#include "util/log.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace rcsdk::platform {
namespace {

// The networks as one text: what changes when they do.
std::string snapshot() {
    std::string text;
    try {
        for (const auto& i : transport::interfaces()) {
            text += i.name + ":";
            for (const auto& address : i.addresses) {
                text += address.toString() + ",";
            }
            text += ";";
        }
        for (const auto& network : networks()) {
            text += network.name + "=" + std::to_string(network.kind) + "/" + std::to_string(network.flags) + ";";
        }
    } catch (const Failure& failure) {
        text = failure.what();
    }
    return text;
}

class Poller {
public:
    Poller() : thread_(log::thread([this] { run(); })) {}
    ~Poller() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

private:
    void run() {
        std::string seen = snapshot();
        std::unique_lock lock(mutex_);
        while (!wake_.wait_for(lock, std::chrono::milliseconds(500), [&] { return stopping_; })) {
            lock.unlock();
            if (auto now = snapshot(); now != seen) {
                seen = std::move(now);
                transport::networkChanged();
            }
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread thread_; // Last.
};

} // namespace

std::shared_ptr<void> watchNetworks() {
    static std::mutex mutex;
    static std::weak_ptr<Poller> running; // One for every session of the process.
    std::lock_guard lock(mutex);
    if (auto shared = running.lock()) {
        return shared;
    }
    auto made = std::make_shared<Poller>();
    running = made;
    return made;
}

} // namespace rcsdk::platform
