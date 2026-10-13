// P2P and RELAY signal routing without the network: a sharer paces its
// attempts and skips repeats (SignalQueue); a viewer's connection takes the
// answer to its own signal and nothing else (Answers); a sharer's answer
// names the signal it answers.
#include "session/signals.hpp"
#include "support/check.hpp"

#include <cstdio>
#include <string>

using namespace rcsdk;
using namespace rcsdk::session;

namespace {

transport::ice::Description signalOf(rcs_role role, const std::string& ufrag, const std::string& answers = {}) {
    transport::ice::Description d;
    d.role = role;
    d.ufrag = ufrag;
    d.password = "00112233445566778899aabbccddeeff";
    d.answers = answers;
    d.candidates.push_back({transport::ice::Type::host, transport::Endpoint::parse("192.0.2.1", 5000), 2130706431});
    return d;
}

ViewerSignal viewer(size_t index, uint64_t route) {
    char ufrag[16];
    std::snprintf(ufrag, sizeof(ufrag), "%08zx", index);
    return {signalOf(RCS_ROLE_VIEW, ufrag), route};
}

// Up to maxRunning attempts at once; the rest wait in order and start as
// attempts end; a repeat starts nothing, also while it waits or runs; too
// many waiting: the one that waited longest makes room.
void queue() {
    SignalQueue queue;
    for (size_t i = 0; i < SignalQueue::maxRunning; ++i) {
        const auto added = queue.add(viewer(i, 100 + i));
        CHECK(added.start && !added.repeat && !added.dropped);
    }
    CHECK(queue.running() == SignalQueue::maxRunning && queue.waiting() == 0);
    const auto waits = queue.add(viewer(1000, 7));
    CHECK(!waits.start && !waits.repeat && queue.waiting() == 1);
    CHECK(queue.add(viewer(1000, 8)).repeat); // The same signal again, whatever the route.
    CHECK(queue.add(viewer(3, 103)).repeat);  // One that runs.
    CHECK(queue.waiting() == 1);
    // An attempt ends: the one that waited starts, with its route.
    const auto next = queue.finished();
    CHECK(next && next->route == 7 && next->description.ufrag == viewer(1000, 0).description.ufrag);
    CHECK(queue.running() == SignalQueue::maxRunning && queue.waiting() == 0);
    // With none waiting, an ending attempt makes room.
    CHECK(!queue.finished() && queue.running() == SignalQueue::maxRunning - 1);
    CHECK(queue.add(viewer(2000, 9)).start && queue.running() == SignalQueue::maxRunning);
    // The waiting room is full: the oldest goes.
    for (size_t i = 0; i < SignalQueue::maxWaiting; ++i) {
        CHECK(!queue.add(viewer(3000 + i, 3000 + i)).dropped);
    }
    const auto overflow = queue.add(viewer(9000, 9000));
    CHECK(overflow.dropped && overflow.dropped->route == 3000 && queue.waiting() == SignalQueue::maxWaiting);
    // What waits when the run ends goes to the next one.
    const auto waiting = queue.takeWaiting();
    CHECK(waiting.size() == SignalQueue::maxWaiting && waiting.front().route == 3001 && waiting.back().route == 9000);
    CHECK(queue.waiting() == 0);
}

// The signals known as seen are the latest `remembered`: a repeat of an older
// one is answered again (its attempt then times out), and memory stays bounded.
void remembered() {
    SignalQueue queue;
    for (size_t i = 0; i < SignalQueue::remembered + 1; ++i) {
        queue.add(viewer(i, i));
        queue.finished();
    }
    CHECK(queue.add(viewer(SignalQueue::remembered, 1)).repeat);
    CHECK(!queue.add(viewer(0, 1)).repeat);
}

// A connection takes the answer to its own signal: answers to an earlier
// connection's, before or after it, are skipped; a later answer to the same
// signal (the sharer started again meanwhile) is the next one taken.
void answers() {
    Answers answers;
    CHECK(!answers.take("aaaaaaaa"));
    answers.add(signalOf(RCS_ROLE_SHARE, "11111111", "00000001")); // To an earlier connection.
    answers.add(signalOf(RCS_ROLE_SHARE, "22222222", "aaaaaaaa"));
    answers.add(signalOf(RCS_ROLE_SHARE, "33333333", "00000001"));
    answers.add(signalOf(RCS_ROLE_SHARE, "44444444", "aaaaaaaa")); // From the sharer's next run.
    const auto first = answers.take("aaaaaaaa");
    CHECK(first && first->ufrag == "22222222");
    const auto later = answers.take("aaaaaaaa");
    CHECK(later && later->ufrag == "44444444");
    CHECK(!answers.take("aaaaaaaa"));
    // A reconnecting viewer's new signal: what answered the old one is no use.
    answers.add(signalOf(RCS_ROLE_SHARE, "55555555", "aaaaaaaa"));
    CHECK(!answers.take("bbbbbbbb"));
    answers.add(signalOf(RCS_ROLE_SHARE, "66666666", "bbbbbbbb"));
    CHECK(answers.take("bbbbbbbb")->ufrag == "66666666");
    // Memory stays bounded.
    for (size_t i = 0; i < Answers::kept + 5; ++i) {
        answers.add(signalOf(RCS_ROLE_SHARE, "77777777", "cccccccc"));
    }
    size_t taken = 0;
    while (answers.take("cccccccc")) {
        ++taken;
    }
    CHECK(taken == Answers::kept);
}

// The answer travels with its viewer's ufrag through the text.
void encoded() {
    const auto answer = signalOf(RCS_ROLE_SHARE, "0a1b2c3d", "f0e1d2c3");
    const auto back = transport::ice::decode(transport::ice::encode(answer));
    CHECK(back.role == RCS_ROLE_SHARE && back.answers == "f0e1d2c3" && back.ufrag == "0a1b2c3d");
    CHECK(transport::ice::decode(transport::ice::encode(signalOf(RCS_ROLE_VIEW, "0a1b2c3d"))).answers.empty());
}

} // namespace

int main() {
    return runTest("signals", [] {
        queue();
        remembered();
        answers();
        encoded();
    });
}
