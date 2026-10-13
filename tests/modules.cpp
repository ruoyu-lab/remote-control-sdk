// The module machinery on its own: calls into a half never overlap, and what
// they leave for after waits until they returned; a MachineHost starts, feeds
// and stops halves in order; the data queue's turns, replacements and first
// items; and a session's modules with the lists another end could send
// (another version, numbers it never listed, a list twice).
#include "modules/machine_host.hpp"
#include "modules/module.hpp"
#include "protocol/messages.hpp"
#include "session/data_channel.hpp"
#include "session/session.hpp"
#include "support/check.hpp"
#include "support/echo_module.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace rcsdk;
using namespace std::chrono_literals;
using session::DataQueue;

namespace {

template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

crypto::Bytes bytesOf(const std::string& text) {
    return crypto::Bytes(text.begin(), text.end());
}

void calls() {
    test::echoModule("test.calls", 0);
    rcs_module_impl half(*modules::find("test.calls"), nullptr);
    // From several threads: one at a time.
    std::atomic<int> inside{0};
    std::atomic<bool> overlapped{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                modules::call(half, [&] {
                    overlapped = overlapped || ++inside > 1;
                    std::this_thread::yield();
                    --inside;
                });
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(!overlapped);
    // Nested on one thread; what calls leave for after runs once the
    // outermost returned, in order; outside any call, at once.
    std::vector<int> order;
    modules::call(half, [&] {
        modules::afterCall(half, [&] { order.push_back(2); });
        modules::call(half, [&] {
            modules::afterCall(half, [&] { order.push_back(3); });
            order.push_back(1);
        });
        CHECK(order == std::vector<int>({1}) && modules::inCall(half));
    });
    CHECK(order == std::vector<int>({1, 2, 3}) && !modules::inCall(half));
    modules::afterCall(half, [&] { order.push_back(4); });
    CHECK(order.back() == 4);
    // A module that calls another's functions: what the other leaves for
    // after waits for the outer call too, whose lock is still held.
    test::echoModule("test.outer", 0);
    rcs_module_impl outer(*modules::find("test.outer"), nullptr);
    order.clear();
    modules::call(outer, [&] {
        modules::call(half, [&] { modules::afterCall(half, [&] { order.push_back(2); }); });
        CHECK(order.empty() && modules::inCall(outer) && !modules::inCall(half));
        order.push_back(1);
    });
    CHECK(order == std::vector<int>({1, 2}));
    // What a call throws stays inside it.
    modules::call(half, [] { throw std::runtime_error("thrown by a module"); });
    // A half's state, released when replaced, and with the half.
    int released = 0;
    const auto release = [](void* count) { ++*static_cast<int*>(count); };
    rcs_module_set_state(&half, &released, release);
    CHECK(rcs_module_state(&half) == &released);
    rcs_module_set_state(&half, &released, release); // The same: kept.
    CHECK(released == 0);
    rcs_module_set_state(&half, nullptr, nullptr);
    CHECK(released == 1);
    {
        rcs_module_impl other(*modules::find("test.calls"), nullptr);
        rcs_module_set_state(&other, &released, release);
    }
    CHECK(released == 2);
}

void machineHost() {
    auto& echo = test::echoModule();
    test::echoModule("test.peer-only", RCS_MODULE_PEER);
    echo.clear();
    std::mutex mutex;
    std::vector<std::tuple<std::string, rcs_result, std::string>> statuses;
    std::vector<std::pair<std::string, crypto::Bytes>> messages;
    {
        modules::MachineHost host(RCS_ROLE_VIEW,
                                  {[&](const std::string& id, rcs_result result, const std::string& message) {
                                       std::lock_guard lock(mutex);
                                       statuses.emplace_back(id, result, message);
                                   },
                                   [&](const std::string& id, Bytes bytes) {
                                       std::lock_guard lock(mutex);
                                       messages.emplace_back(id, std::move(bytes));
                                   }});
        host.start("test.echo", 1);
        host.start("test.echo", 1); // It runs: no second start.
        host.start("test.none", 1);
        host.start("test.peer-only", 1);
        host.start("test.calls", 2);
        host.fromSession("test.echo", bytesOf("a"));
        host.fromSession("test.echo", bytesOf("b"));
        host.fromSession("test.none", bytesOf("x")); // Nothing runs it.
        CHECK(waitFor([&] {
            std::lock_guard lock(mutex);
            return messages.size() == 2 && statuses.size() == 4;
        }));
        {
            std::lock_guard lock(mutex);
            CHECK((messages[0] == std::pair<std::string, crypto::Bytes>("test.echo", bytesOf("machine:a"))));
            CHECK(messages[1].second == bytesOf("machine:b"));
            CHECK(std::get<0>(statuses[0]) == "test.echo" && std::get<1>(statuses[0]) == RCS_OK);
            for (size_t i = 1; i < 4; ++i) {
                CHECK(std::get<1>(statuses[i]) == RCS_ERROR_UNSUPPORTED);
            }
            CHECK(std::get<2>(statuses[1]).find("has not registered") != std::string::npos);
            CHECK(std::get<2>(statuses[2]).find("no machine's half") != std::string::npos);
            CHECK(std::get<2>(statuses[3]).find("has version 1") != std::string::npos); // The session asked for 2.
        }
        CHECK(echo.saw("machine_start", nullptr, "view") && echo.count("machine_start") == 1);
        host.stop("test.echo");
        CHECK(waitFor([&] { return echo.count("machine_stop") == 1; }));
        host.fromSession("test.echo", bytesOf("c")); // Stopped: dropped.
        host.start("test.echo", 1);                  // ...and started again.
        CHECK(waitFor([&] { return echo.count("machine_start") == 2; }));
    }
    CHECK(echo.count("machine_stop") == 2 && !echo.overlapped); // Its end stops what runs.
    std::lock_guard lock(mutex);
    CHECK(messages.size() == 2);
}

DataQueue::Item item(protocol::DataPart::Kind kind, uint8_t module, const std::string& text, bool replace = false) {
    DataQueue::Item i;
    i.kind = kind;
    i.module = module;
    i.replace = replace;
    i.bytes = bytesOf(text);
    return i;
}

std::string popped(DataQueue& queue) {
    const std::atomic<bool> cancelled{false};
    const auto next = queue.pop(10ms, cancelled);
    return next ? std::to_string(next->module) + ":" + std::string(next->bytes.begin(), next->bytes.end()) : "none";
}

void dataQueue() {
    using protocol::DataPart;
    DataQueue queue;
    CHECK(queue.push(item(DataPart::data, 0, "a"), 100) == DataQueue::Push::closed);
    CHECK(!queue.pushFirst(item(DataPart::moduleList, 0, "list")));
    queue.open();
    CHECK(queue.push(item(DataPart::data, 0, "a"), 100) == DataQueue::Push::queued);
    CHECK(queue.push(item(DataPart::data, 0, "b"), 100) == DataQueue::Push::queued);
    CHECK(queue.push(item(DataPart::moduleMessage, 1, "x", true), 4) == DataQueue::Push::queued);
    CHECK(queue.push(item(DataPart::moduleMessage, 1, "yy", true), 4) == DataQueue::Push::queued); // Replaces x.
    CHECK(queue.bytes(1) == 2);
    CHECK(queue.push(item(DataPart::moduleMessage, 1, "zz"), 4) == DataQueue::Push::queued);
    CHECK(queue.fits(item(DataPart::moduleMessage, 1, "w"), 4) == DataQueue::Push::full); // Beside yy and zz.
    CHECK(queue.fits(item(DataPart::moduleMessage, 1, "ww", true), 4) == DataQueue::Push::queued); // In yy's place.
    CHECK(queue.push(item(DataPart::moduleMessage, 2, "n"), 4) == DataQueue::Push::queued);
    CHECK(queue.pushFirst(item(DataPart::moduleList, 0, "list")));
    CHECK(queue.pushFirst(item(DataPart::moduleHello, 1, "h1")));
    CHECK(queue.pushFirst(item(DataPart::moduleHello, 1, "h2"))); // In h1's place.
    // The list and the hello first, then the queues in turn, each in order.
    CHECK(popped(queue) == "0:list" && popped(queue) == "1:h2");
    CHECK(popped(queue) == "0:a" && popped(queue) == "1:yy" && popped(queue) == "2:n");
    CHECK(popped(queue) == "0:b" && popped(queue) == "1:zz" && popped(queue) == "none");
    // Room again once below half of what a queue holds, after a refusal
    // (fits() said full above), once.
    CHECK(queue.roomAgain(1, 4) && !queue.roomAgain(1, 4));
    CHECK(queue.push(item(DataPart::moduleMessage, 1, "12345"), 4) == DataQueue::Push::full);
    CHECK(queue.roomAgain(1, 4) && !queue.roomAgain(1, 4));
    // A clear tells the hosts of their refusal only.
    CHECK(queue.push(item(DataPart::data, 0, std::string(200, 'd')), 100) == DataQueue::Push::full);
    CHECK(queue.clear());
    CHECK(queue.push(item(DataPart::data, 0, "after"), 100) == DataQueue::Push::closed);
}

// A session's modules, with lists another end could send.
void moduleSet() {
    auto& echo = test::echoModule();
    echo.clear();
    struct Events {
        std::mutex mutex;
        std::vector<std::tuple<rcs_module_event_kind, rcs_result, std::string>> seen;
    } events;
    rcs_session_callbacks callbacks = RCS_SESSION_CALLBACKS_INIT;
    callbacks.user_data = &events;
    callbacks.on_event = [](const rcs_event* event, void* data) {
        if (event->type == RCS_EVENT_MODULE) {
            auto& e = *static_cast<Events*>(data);
            std::lock_guard lock(e.mutex);
            e.seen.emplace_back(event->module.kind, event->module.result, event->module.message);
        }
    };
    session::SessionConfig config;
    config.role = RCS_ROLE_VIEW;
    config.modules.push_back({modules::find("test.echo"), bytesOf("set")});
    session::Session session(config, callbacks, log::makeSource(RCS_LOG_SOURCE_SESSION, nullptr, nullptr, 0));
    auto& set = session.modules();
    set.start();
    CHECK(echo.saw("start", nullptr, "set") && waitFor([&] { return echo.saw("machine_joined"); }));
    const auto lastEvent = [&] {
        std::lock_guard lock(events.mutex);
        return events.seen.empty() ? std::tuple<rcs_module_event_kind, rcs_result, std::string>{} : events.seen.back();
    };
    const auto listOf = [](DataQueue& queue) {
        const std::atomic<bool> cancelled{false};
        auto first = queue.pop(10ms, cancelled);
        CHECK(first && first->kind == protocol::DataPart::moduleList);
        return protocol::decodeModuleList(first->bytes);
    };

    // This end's list, and one with another version: not active, said why.
    DataQueue queue(true);
    set.listTo(0, queue);
    set.listTo(0, queue); // Once.
    const auto mine = listOf(queue);
    CHECK(mine.modules.size() == 1 && mine.modules[0].id == "test.echo" && mine.modules[0].version == 1 &&
          mine.modules[0].hello == bytesOf("hello from view"));
    set.peerList(0, {{{"test.echo", 2, {}}}}, queue);
    CHECK(std::get<0>(lastEvent()) == RCS_MODULE_INACTIVE && std::get<1>(lastEvent()) == RCS_ERROR_UNSUPPORTED &&
          std::get<2>(lastEvent()).find("version 2") != std::string::npos);
    CHECK(throws<Failure>([&] { set.received(0, 1, bytesOf("ping:1"), false); }));
    CHECK(throws<Failure>([&] { set.peerList(0, {{{"test.echo", 1, {}}}}, queue); })); // Twice.
    set.peerGone(0);
    CHECK(!echo.saw("peer_joined"));

    // Again, alike: active. A datagram before the list is dropped; numbers
    // the list does not have, or of modules not active, break the protocol.
    set.listTo(0, queue);
    listOf(queue);
    set.received(0, 1, bytesOf("early"), true);
    set.peerList(0, {{{"com.example.other", 1, {}}, {"test.echo", 1, bytesOf("h")}}}, queue);
    CHECK(std::get<0>(lastEvent()) == RCS_MODULE_ACTIVE && echo.saw("peer_joined", nullptr, "h"));
    set.received(0, 2, bytesOf("ping:z"), false);
    {
        const std::atomic<bool> cancelled{false};
        const auto answer = queue.pop(10ms, cancelled);
        CHECK(answer && answer->kind == protocol::DataPart::moduleMessage && answer->module == 1 &&
              answer->bytes == bytesOf("pong:z") && answer->tag == 7);
    }
    CHECK(throws<Failure>([&] { set.received(0, 1, bytesOf("x"), false); })); // Not active here.
    CHECK(throws<Failure>([&] { set.received(0, 3, bytesOf("x"), false); })); // Not listed.
    set.received(0, 1, bytesOf("x"), true);                                     // A datagram of one: dropped.
    CHECK(set.maxMessage(0, 2) == 32u << 20);
    CHECK(throws<Failure>([&] { set.maxMessage(0, 1); }));
    CHECK(throws<Failure>([&] { set.received(0, 2, crypto::Bytes((32u << 20) + 1), false); }));
    set.peerHello(0, 2, bytesOf("new"));
    set.peerHello(0, 1, bytesOf("ignored"));
    CHECK(throws<Failure>([&] { set.peerHello(0, 9, bytesOf("x")); }));
    CHECK(echo.saw("peer_hello", nullptr, "new") && !echo.saw("peer_hello", nullptr, "ignored"));
    // Its own hello goes to the peer, first.
    rcs_module half = set.find("test.echo");
    CHECK(rcs_module_set_hello(half, reinterpret_cast<const uint8_t*>("mine"), 4) == RCS_OK);
    {
        const std::atomic<bool> cancelled{false};
        const auto hello = queue.pop(10ms, cancelled);
        CHECK(hello && hello->kind == protocol::DataPart::moduleHello && hello->module == 1 &&
              hello->bytes == bytesOf("mine"));
    }
    set.peerGone(0);
    set.peerGone(0); // Once.
    CHECK(echo.count("peer_left") == 1);
    CHECK(rcs_module_send(half, 0, reinterpret_cast<const uint8_t*>("x"), 1, 0, 0) == RCS_ERROR_INVALID_STATE);
    set.stop();
    CHECK(echo.saw("machine_left") && echo.saw("stop") && echo.saw("machine_stop") && !echo.overlapped);
}

} // namespace

int main() {
    int result = runTest("calls", calls);
    result |= runTest("machine host", machineHost);
    result |= runTest("data queue", dataQueue);
    result |= runTest("module set", moduleSet);
    return result;
}
