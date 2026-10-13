// ICE: signals, checks between two agents on this machine, STUN gathering
// against a minimal server, and a clear failure when no path exists.
#include "support/check.hpp"
#include "transport/ice.hpp"
#include "transport/interfaces.hpp"
#include "transport/stun.hpp"

#include <algorithm>
#include <cctype>
#include <future>
#include <thread>

using namespace rcsdk;
using namespace rcsdk::transport;

namespace {

std::atomic<bool> never{false};

void signals() {
    ice::Description d{RCS_ROLE_SHARE, "0a1b2c3d", "00112233445566778899aabbccddeeff", "f0e1d2c3", {}};
    d.candidates.push_back({ice::Type::host, Endpoint::parse("192.168.1.20", 5000), 2130706431});
    d.candidates.push_back({ice::Type::serverReflexive, Endpoint::parse("2001:db8::9", 6000), 1694498815});
    const auto text = ice::encode(d);
    CHECK(text.rfind("rcs1.", 0) == 0);
    CHECK(std::all_of(text.begin(), text.end(),
                      [](char c) { return std::isalnum(uint8_t(c)) || c == '.' || c == '-' || c == '_'; }));
    const auto back = ice::decode(text);
    CHECK(back.role == RCS_ROLE_SHARE && back.ufrag == d.ufrag && back.password == d.password);
    CHECK(back.answers == "f0e1d2c3"); // The viewer signal it answers.
    CHECK(ice::decode(" " + text + " \r\n").ufrag == d.ufrag); // As text channels may deliver it.
    CHECK(back.candidates.size() == 2 && back.candidates[1].address.toString() == "[2001:db8::9]:6000");
    CHECK(back.candidates[1].type == ice::Type::serverReflexive && back.candidates[0].priority == 2130706431u);
    for (const std::string& bad : std::vector<std::string>{"", "rcs1.", "rcs2.AAAA", "rcs1.!!!!",
                                                           text.substr(0, text.size() - 3), text + "AA"}) {
        CHECK(throws<Failure>([&] { ice::decode(bad); }));
    }
    // A sharer's signal answers a viewer's; a viewer's answers none.
    auto unanswered = d;
    unanswered.answers.clear();
    CHECK(throws<Failure>([&] { ice::decode(ice::encode(unanswered)); }));
    auto viewer = d;
    viewer.role = RCS_ROLE_VIEW;
    CHECK(throws<Failure>([&] { ice::decode(ice::encode(viewer)); }));
    viewer.answers.clear();
    CHECK(ice::decode(ice::encode(viewer)).answers.empty());
    // A signal of a version no version of this SDK makes is refused by its version.
    try {
        ice::decode("rcs1.CQEIMGExYjJjM2QgMDAxMTIyMzM0NDU1NjY3Nzg4OTlhYWJiY2NkZGVlZmYA");
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(std::string(failure.what()).find("version") != std::string::npos);
    }
    // More candidates than a signal carries is a fault of this end, never cut silently.
    auto crowded = d;
    crowded.candidates.resize(33, d.candidates[0]);
    CHECK(throws<Failure>([&] { ice::encode(crowded); }));
    crowded.candidates.resize(32);
    CHECK(ice::decode(ice::encode(crowded)).candidates.size() == 32);
    CHECK(ice::resolve("127.0.0.1:3478").at(0).toString() == "127.0.0.1:3478");

    // A restart of a running connection's path: sealed with the connection's
    // key, which the host carries but cannot change or make; either role may
    // start one, and an answer names the restart it answers.
    const std::array<uint8_t, 32> key{1, 2, 3}, other{3, 2, 1};
    ice::Restart restart{{9, 8, 7, 6, 5, 4, 3, 2}, 7, false, d};
    restart.description.answers.clear();
    const auto sealed = ice::encodeRestart(restart, key);
    CHECK(ice::signalKind(sealed) == ice::SignalKind::restart && ice::signalKind(text) == ice::SignalKind::connect);
    CHECK(ice::signalTag(sealed) == restart.tag && throws<Failure>([&] { ice::signalTag(text); }));
    const auto opened = ice::decodeRestart(sealed, key);
    CHECK(opened.tag == restart.tag && opened.generation == 7 && !opened.standby &&
          opened.description.role == RCS_ROLE_SHARE && opened.description.answers.empty() &&
          opened.description.candidates.size() == 2);
    restart.standby = true; // A path kept ready beside the one in use.
    CHECK(ice::decodeRestart(ice::encodeRestart(restart, key), key).standby);
    restart.standby = false;
    try {
        ice::decodeRestart(sealed, other);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_AUTHENTICATION);
    }
    auto changed = sealed; // A candidate changed on the way.
    changed[changed.size() - 30] = changed[changed.size() - 30] == 'A' ? 'B' : 'A';
    CHECK(throws<Failure>([&] { ice::decodeRestart(changed, key); }));
    CHECK(throws<Failure>([&] { ice::decode(sealed); })); // Not a connection's first signal.
    restart.description.answers = "0a0b0c0d";
    CHECK(ice::decodeRestart(ice::encodeRestart(restart, key), key).description.answers == "0a0b0c0d");
    const auto unknown = ice::encodeUnknown(restart.tag);
    CHECK(ice::signalKind(unknown) == ice::SignalKind::unknown && ice::signalTag(unknown) == restart.tag);
    CHECK(throws<Failure>([&] { ice::decodeRestart(unknown, key); }));
    // The longest signal there can be decodes; anything longer is refused
    // before it is decoded, however long.
    auto longest = restart;
    longest.description.ufrag = std::string(64, 'a');
    longest.description.password = std::string(128, 'b');
    longest.description.answers = std::string(64, 'c');
    longest.description.candidates.assign(32, d.candidates[1]);
    const auto longText = ice::encodeRestart(longest, key);
    CHECK(ice::decodeRestart(" " + longText + "\n", key).description.candidates.size() == 32);
    for (const auto& tooLong : {longText + "A", "rcs1." + std::string(50u << 20, 'A')}) {
        const auto started = Clock::now();
        try {
            ice::signalKind(tooLong);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_INVALID_ARGUMENT &&
                  std::string(failure.what()).find("longer than any signal") != std::string::npos);
        }
        CHECK(Clock::now() - started < 500ms);
    }
    CHECK(ice::resolve("[::1]:19302").at(0).toString() == "[::1]:19302");
    CHECK(ice::resolve("localhost:1").size() >= 1);
    CHECK(ice::resolve("nonsense:0").empty() && ice::resolve("host.invalid:3478").empty());
}

// Two agents on this machine find each other and agree on the path.
void pair() {
    ice::Agent sharer(false, {}, {}, never), viewer(true, {}, {}, never);
    const auto sharerSignal = sharer.gather(RCS_ROLE_SHARE), viewerSignal = viewer.gather(RCS_ROLE_VIEW);
    CHECK(!sharerSignal.candidates.empty() && sharerSignal.ufrag != viewerSignal.ufrag);
    auto controlled = std::async(std::launch::async, [&] { return sharer.connect(viewerSignal, 5s); });
    auto viewerPath = viewer.connect(sharerSignal, 5s);
    auto sharerPath = controlled.get();
    // Each end's selected remote is the other's socket.
    CHECK(viewerPath.remote.port() == sharerPath.socket->local().port());
    CHECK(sharerPath.remote.port() == viewerPath.socket->local().port());
    // Same role on both sides is refused.
    ice::Agent other(true, {}, {}, never);
    other.gather(RCS_ROLE_VIEW);
    CHECK(throws<Failure>([&] { other.connect(viewerSignal, 1s); }));
}

// The host may deliver a signal from the peer's earlier run before, after or
// along with its current one. Each end uses the signal the peer's checks name.
void staleSignals() {
    ice::Agent earlierSharer(false, {}, {}, never), earlierViewer(true, {}, {}, never);
    const auto staleSharer = earlierSharer.gather(RCS_ROLE_SHARE), staleViewer = earlierViewer.gather(RCS_ROLE_VIEW);
    // Hands out `signals` one per poll, the first after `delay`.
    const auto later = [](std::vector<ice::Description> signals, std::chrono::milliseconds delay) {
        const auto from = Clock::now() + delay;
        return [signals, from]() mutable -> std::optional<ice::Description> {
            if (signals.empty() || Clock::now() < from) {
                return std::nullopt;
            }
            auto next = signals.front();
            signals.erase(signals.begin());
            return next;
        };
    };
    const auto connects = [](ice::Agent& sharer, ice::Agent& viewer, auto sharerConnect, auto viewerConnect) {
        auto controlled = std::async(std::launch::async, [&] { return sharerConnect(sharer); });
        const auto viewerPath = viewerConnect(viewer);
        const auto sharerPath = controlled.get();
        return viewerPath.remote.port() == sharerPath.socket->local().port() &&
               sharerPath.remote.port() == viewerPath.socket->local().port();
    };
    {
        // The viewer gets the sharer's stale signal first, the current one later.
        ice::Agent sharer(false, {}, {}, never), viewer(true, {}, {}, never);
        const auto offer = sharer.gather(RCS_ROLE_SHARE), answer = viewer.gather(RCS_ROLE_VIEW);
        CHECK(connects(
            sharer, viewer, [&](ice::Agent& a) { return a.connect(answer, 5s); },
            [&](ice::Agent& a) { return a.connect(staleSharer, 5s, later({offer}, 300ms)); }));
    }
    {
        // The viewer has the current one; the stale one and a repeat follow at once.
        ice::Agent sharer(false, {}, {}, never), viewer(true, {}, {}, never);
        const auto offer = sharer.gather(RCS_ROLE_SHARE), answer = viewer.gather(RCS_ROLE_VIEW);
        CHECK(connects(
            sharer, viewer, [&](ice::Agent& a) { return a.connect(answer, 5s); },
            [&](ice::Agent& a) { return a.connect(offer, 5s, later({staleSharer, offer}, 0ms)); }));
    }
    {
        // The sharer gets the viewer's stale signal first.
        ice::Agent sharer(false, {}, {}, never), viewer(true, {}, {}, never);
        const auto offer = sharer.gather(RCS_ROLE_SHARE), answer = viewer.gather(RCS_ROLE_VIEW);
        CHECK(connects(
            sharer, viewer, [&](ice::Agent& a) { return a.connect(staleViewer, 5s, later({answer}, 300ms)); },
            [&](ice::Agent& a) { return a.connect(offer, 5s); }));
    }
    {
        // Only the stale signal ever arrives: the failure says what is missing.
        ice::Agent sharer(false, {}, {}, never), viewer(true, {}, {}, never);
        const auto offer = sharer.gather(RCS_ROLE_SHARE), answer = viewer.gather(RCS_ROLE_VIEW);
        auto controlled = std::async(std::launch::async, [&] {
            try {
                sharer.connect(answer, 1500ms);
            } catch (const Failure&) {
            }
        });
        try {
            viewer.connect(staleSharer, 1500ms);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(std::string(failure.what()).find("latest") != std::string::npos);
        }
        controlled.get();
    }
}

// A minimal STUN server: answers Binding requests with the source address.
void stunServer(UdpSocket& socket, std::atomic<bool>& stop) {
    std::array<uint8_t, 1500> buffer;
    while (!stop) {
        Endpoint from;
        if (const auto size = socket.receive(buffer, from)) {
            const auto request = stun::parse(std::span(buffer).first(*size));
            if (request && request->method == stun::binding && request->klass == stun::request) {
                stun::Message response;
                response.method = stun::binding;
                response.klass = stun::success;
                response.transaction = request->transaction;
                response.addAddress(stun::xorMappedAddress, from);
                socket.send(from, stun::encode(response));
            }
        } else {
            std::this_thread::sleep_for(2ms);
        }
    }
}

// A sharer's paths take a port each: the first free of a range.
void ports() {
    // Three free ports in a row, found by binding them.
    std::vector<std::unique_ptr<UdpSocket>> taken;
    uint16_t first = 0;
    for (uint16_t candidate = 47600; candidate < 47700 && !first; candidate += 3) {
        try {
            taken.clear();
            for (uint16_t port = candidate; port < candidate + 3; ++port) {
                taken.push_back(std::make_unique<UdpSocket>(Endpoint::parse("0.0.0.0", port)));
            }
            first = candidate;
        } catch (const Failure&) {
        }
    }
    CHECK(first != 0);
    taken.resize(1); // The first stays taken, the other two are free.
    ice::Agent a(false, {}, {first, 3}, never), b(false, {}, {first, 3}, never);
    const auto portOf = [](const ice::Description& d) {
        return std::find_if(d.candidates.begin(), d.candidates.end(),
                            [](const ice::Candidate& c) { return !c.address.ipv6(); })
            ->address.port();
    };
    CHECK(portOf(a.gather(RCS_ROLE_SHARE)) == first + 1);
    CHECK(portOf(b.gather(RCS_ROLE_SHARE)) == first + 2);
    ice::Agent full(false, {}, {first, 3}, never);
    try {
        full.gather(RCS_ROLE_SHARE);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_NETWORK);
        CHECK(std::string(failure.what()).find("No UDP port of " + std::to_string(first) + ".." +
                                               std::to_string(first + 2)) != std::string::npos);
    }
}

void gathering() {
    UdpSocket server(Endpoint::parse("127.0.0.1", 0));
    std::atomic<bool> stop{false};
    std::thread thread([&] { stunServer(server, stop); });
    ice::Agent agent(true, {server.local(), Endpoint::parse("127.0.0.1", 9)}, {}, never);
    const auto started = Clock::now();
    const auto description = agent.gather(RCS_ROLE_VIEW);
    stop = true;
    thread.join();
    // The server sees the agent at 127.0.0.1 (not an interface candidate): server-reflexive.
    CHECK(std::any_of(description.candidates.begin(), description.candidates.end(), [](const ice::Candidate& c) {
        return c.type == ice::Type::serverReflexive && c.address.address() == "127.0.0.1";
    }));
    // The silent second server costs at most the gathering timeout.
    CHECK(Clock::now() - started < 2s);
}

// A host restricts the session to one interface: only its addresses are offered.
void restricted() {
    try {
        ice::Agent agent(true, {}, {}, never, std::nullopt, "no-such-interface");
        agent.gather(RCS_ROLE_VIEW);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_NETWORK &&
              std::string(failure.what()).find("available") != std::string::npos);
    }
    const auto all = interfaces();
    if (all.empty()) {
        return;
    }
    const auto& chosen = all.front();
    ice::Agent sharer(false, {}, {}, never, std::nullopt, chosen.name), viewer(true, {}, {}, never);
    const auto offer = sharer.gather(RCS_ROLE_SHARE);
    for (const auto& candidate : offer.candidates) {
        Endpoint plain = candidate.address;
        plain.setPort(0);
        CHECK(std::find(chosen.addresses.begin(), chosen.addresses.end(), plain) != chosen.addresses.end());
    }
    const auto answer = viewer.gather(RCS_ROLE_VIEW);
    auto controlled = std::async(std::launch::async, [&] { return sharer.connect(answer, 5s); });
    const auto path = viewer.connect(offer, 5s);
    const auto sharerPath = controlled.get();
    CHECK(sharerPath.socket != nullptr);
    CHECK(std::any_of(offer.candidates.begin(), offer.candidates.end(),
                      [&](const ice::Candidate& c) { return c.address.port() == path.remote.port(); }));
}

void noPath() {
    ice::Agent viewer(true, {}, {}, never, std::nullopt, {}, 1500ms, RCS_ICE_ADDRESSES_LOOPBACK);
    viewer.gather(RCS_ROLE_VIEW);
    ice::Description unreachable{RCS_ROLE_SHARE, "0a1b2c3d", "00112233445566778899aabbccddeeff", {}};
    unreachable.candidates.push_back({ice::Type::host, Endpoint::parse("127.0.0.1", 9), 2130706431});
    const auto started = Clock::now();
    try {
        viewer.connect(unreachable, 1500ms);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_NETWORK);
        CHECK(std::string(failure.what()).find("No direct path") != std::string::npos);
        CHECK(std::string(failure.what()).find("127.0.0.1:9") != std::string::npos);
    }
    CHECK(Clock::now() - started < 3s);
}

// What a peer's signal names is probed, so it must not point this end's
// checks at addresses no peer is at, nor at this machine or link unless this
// end is there too, nor at many ports of one address (SA-12). Only the
// loopback candidates could ever be sent to: they are listeners here, which
// count what reaches them.
void peerAddresses() {
    std::vector<std::unique_ptr<UdpSocket>> listeners;
    ice::Description peer{RCS_ROLE_SHARE, "0a1b2c3d", "00112233445566778899aabbccddeeff", {}, {}};
    for (int i = 0; i < 6; ++i) {
        listeners.push_back(std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)));
        peer.candidates.push_back({ice::Type::host, listeners.back()->local(), 2130706431});
    }
    for (const char* address : {"0.0.0.0", "224.0.0.251", "255.255.255.255", "240.0.0.1", "169.254.169.254",
                                "100.100.100.200", "168.63.129.16", "169.254.7.7", "::", "ff02::fb", "fe80::1",
                                "fd00:ec2::254", "64:ff9b::a9fe:a9fe", "::ffff:224.0.0.1"}) {
        peer.candidates.push_back({ice::Type::host, Endpoint::parse(address, 9), 1});
    }
    // The listeners checks reached, and the reason the attempt failed.
    const auto attempt = [&](uint32_t addresses) {
        ice::Agent viewer(true, {}, {}, never, std::nullopt, {}, 1500ms, addresses);
        viewer.gather(RCS_ROLE_VIEW);
        std::string reason;
        try {
            viewer.connect(peer, 600ms);
            CHECK(false);
        } catch (const Failure& failure) {
            reason = failure.what();
        }
        int reached = 0;
        for (const auto& listener : listeners) {
            std::array<uint8_t, 1500> buffer;
            Endpoint from;
            bool checked = false;
            while (const auto size = listener->receive(buffer, from)) {
                checked = checked || stun::looksLikeStun(std::span(buffer).first(*size));
            }
            reached += checked ? 1 : 0;
        }
        return std::pair{reached, reason};
    };
    const auto says = [](const std::string& reason, const std::string& what) {
        return reason.find(what) != std::string::npos;
    };
    const auto [alone, why] = attempt(0);
    CHECK(alone == 0 && says(why, "checked 0 address pairs"));
    CHECK(says(why, listeners[0]->local().toString() + " (loopback)"));
    for (const char* left : {"0.0.0.0:9 (unspecified)", "224.0.0.251:9 (multicast)",
                             "255.255.255.255:9 (broadcast or reserved)", "240.0.0.1:9 (broadcast or reserved)",
                             "169.254.169.254:9 (a cloud metadata service)", "100.100.100.200:9 (a cloud metadata service)",
                             "168.63.129.16:9 (a cloud metadata service)", "169.254.7.7:9 (link-local)",
                             "[::]:9 (unspecified)", "[ff02::fb]:9 (multicast)", "[fe80::1]:9 (link-local)",
                             "[fd00:ec2::254]:9 (a cloud metadata service)",
                             "[64:ff9b::a9fe:a9fe]:9 (a cloud metadata service)", "[::ffff:224.0.0.1]:9 (multicast)"}) {
        CHECK(says(why, left));
    }
    // On loopback itself: the first four of the address, the rest left out.
    const auto [onLoopback, because] = attempt(RCS_ICE_ADDRESSES_LOOPBACK);
    CHECK(onLoopback == 4 && says(because, "checked 4 address pairs"));
    CHECK(says(because, listeners[4]->local().toString() + " (more of one address than a peer has)"));
    CHECK(says(because, "224.0.0.251:9 (multicast)") && says(because, "169.254.7.7:9 (link-local)"));
}

} // namespace

int main() {
    return runTest("ice", [] {
        signals();
        pair();
        staleSignals();
        ports();
        gathering();
        restricted();
        noPath();
        peerAddresses();
    });
}
