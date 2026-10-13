// The module's messages: each one there and back, and what is refused of
// what comes (the other end, and an agent of another process, may be anyone's).
#include "messages.hpp"
#include "support/check.hpp"

#include <cstring>
#include <string>

using namespace rcsdk::power;

namespace {

bool refused(void (*decode)(const Bytes&), const Bytes& bytes) {
    try {
        decode(bytes);
        return false;
    } catch (const Refused& error) {
        return error.result() == RCS_ERROR_PROTOCOL;
    }
}

bool helloRefused(const Bytes& bytes) {
    return refused([](const Bytes& b) { decodeHello(b); }, bytes);
}
bool peerRefused(const Bytes& bytes) {
    return refused([](const Bytes& b) { decodePeer(b); }, bytes);
}
bool toMachineRefused(const Bytes& bytes) {
    return refused([](const Bytes& b) { decodeToMachine(b); }, bytes);
}
bool toSessionRefused(const Bytes& bytes) {
    return refused([](const Bytes& b) { decodeToSession(b); }, bytes);
}

bool same(const rcs_power_interface& a, const rcs_power_interface& b) {
    return std::memcmp(a.mac, b.mac, 6) == 0 && a.link == b.link && a.wake == b.wake &&
           std::string(a.address) == b.address && a.prefix_length == b.prefix_length &&
           std::string(a.broadcast) == b.broadcast && std::string(a.name) == b.name;
}

rcs_power_interface ethernet() {
    const uint8_t mac[6] = {0x00, 0x1b, 0x21, 0x3a, 0x4b, 0x5c};
    return makeInterface(mac, RCS_POWER_LINK_ETHERNET, RCS_POWER_WAKE_ENABLED, 0xC0A80114, 24, "Ethernet");
}

// Interfaces: the broadcast address follows from the address and prefix; a
// long name is cut at a character's start.
void interfaces() {
    const auto e = ethernet();
    CHECK(std::string(e.address) == "192.168.1.20" && std::string(e.broadcast) == "192.168.1.255");
    const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 1};
    CHECK(std::string(makeInterface(mac, RCS_POWER_LINK_WIFI, RCS_POWER_WAKE_UNKNOWN, 0x0A000005, 8, "w").broadcast) ==
          "10.255.255.255");
    CHECK(std::string(makeInterface(mac, RCS_POWER_LINK_WIFI, RCS_POWER_WAKE_UNKNOWN, 0x0A000005, 32, "w").broadcast) ==
          "10.0.0.5");
    CHECK(std::string(makeInterface(mac, RCS_POWER_LINK_WIFI, RCS_POWER_WAKE_UNKNOWN, 0x0A000005, 0, "w").broadcast) ==
          "255.255.255.255");
    std::string longName(62, 'a');
    longName += "\xC3\xA9\xC3\xA9"; // Two characters of two bytes: the first is cut in half at 63.
    const auto cut = makeInterface(mac, RCS_POWER_LINK_WIFI, RCS_POWER_WAKE_UNKNOWN, 1, 24, longName);
    CHECK(std::string(cut.name) == std::string(62, 'a'));

    uint32_t address = 0;
    CHECK(parseAddress("192.168.1.255", address) && address == 0xC0A801FF);
    CHECK(parseAddress("0.0.0.0", address) && address == 0);
    for (const char* bad : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "01.2.3.4", "1.2.3.4 ", " 1.2.3.4", "+1.2.3.4",
                            "1.2.3.-4", "1..3.4", "1.2.3.4x", "1234.1.1.1", "localhost", "::1"}) {
        CHECK(!parseAddress(bad, address));
    }
}

void hello() {
    Hello h;
    h.actions = 1u << RCS_POWER_RESTART | 1u << RCS_POWER_SLEEP;
    h.wakeInfo = true;
    h.interfaces = {ethernet()};
    const Hello back = decodeHello(encode(h));
    CHECK(back.actions == h.actions && back.wakeInfo && back.interfaces.size() == 1 &&
          same(back.interfaces[0], h.interfaces[0]));
    const Hello none = decodeHello(encode(Hello{}));
    CHECK(!none.actions && !none.wakeInfo && none.interfaces.empty());
    // At most RCS_POWER_INTERFACES_MAX go.
    h.interfaces.assign(RCS_POWER_INTERFACES_MAX + 3, ethernet());
    CHECK(decodeHello(encode(h)).interfaces.size() == RCS_POWER_INTERFACES_MAX);

    Bytes bytes = encode(Hello{});
    CHECK(helloRefused({}) && helloRefused({1, 0, 0}));
    bytes[0] = 1; // An action no version defines (bit 0).
    CHECK(helloRefused(bytes));
    bytes = encode(Hello{});
    bytes[4] = 2; // wakeInfo
    CHECK(helloRefused(bytes));
    bytes = encode(Hello{});
    bytes.push_back(0);
    CHECK(helloRefused(bytes));
    Hello told;
    told.wakeInfo = true;
    told.interfaces = {ethernet()};
    bytes = encode(told);
    bytes[4] = 0; // Interfaces in a hello that tells none.
    CHECK(helloRefused(bytes));
    bytes = encode(told);
    bytes[5] = RCS_POWER_INTERFACES_MAX + 1;
    CHECK(helloRefused(bytes));
    bytes = encode(told);
    bytes[6 + 6] = 3; // An unknown link.
    CHECK(helloRefused(bytes));
    bytes = encode(told);
    bytes[6 + 7] = 3; // An unknown wake setting.
    CHECK(helloRefused(bytes));
    bytes = encode(told);
    bytes[6 + 12] = 33; // A prefix past 32.
    CHECK(helloRefused(bytes));
    bytes = encode(told);
    bytes[6 + 14] = 0; // A NUL in its name.
    CHECK(helloRefused(bytes));
    bytes = encode(told);
    bytes[6 + 13] = 64; // A name longer than it is.
    CHECK(helloRefused(bytes));
}

void peerMessages() {
    const Ask ask{0x0102030405060708ull, RCS_POWER_SHUT_DOWN, RCS_POWER_FORCE};
    const auto a = std::get<Ask>(decodePeer(encode(PeerMessage{ask})));
    CHECK(a.request == ask.request && a.action == ask.action && a.flags == ask.flags);
    const Answer answer{7, RCS_POWER_RESTART, 0, RCS_POWER_ACTION_ACCEPTED, RCS_OK, 3000, {}};
    const auto b = std::get<Answer>(decodePeer(encode(PeerMessage{answer})));
    CHECK(b.request == 7 && b.action == RCS_POWER_RESTART && b.state == RCS_POWER_ACTION_ACCEPTED &&
          b.result == RCS_OK && b.delayMs == 3000 && b.message.empty());
    const Answer no{8, RCS_POWER_SIGN_OUT, 0, RCS_POWER_ACTION_REFUSED, RCS_ERROR_REFUSED, 0,
                    std::string(300, 'x')};
    const auto c = std::get<Answer>(decodePeer(encode(PeerMessage{no})));
    CHECK(c.result == RCS_ERROR_REFUSED && c.message == std::string(maxText, 'x'));

    CHECK(peerRefused({}) && peerRefused({9}));
    Bytes bytes = encode(PeerMessage{ask});
    bytes[9] = 0; // No action.
    CHECK(peerRefused(bytes));
    bytes[9] = 5;
    CHECK(peerRefused(bytes));
    bytes = encode(PeerMessage{ask});
    bytes[10] = 2; // A flag no version defines.
    CHECK(peerRefused(bytes));
    bytes = encode(PeerMessage{ask});
    bytes.pop_back();
    CHECK(peerRefused(bytes));
    bytes = encode(PeerMessage{answer});
    bytes[11] = 5; // No state.
    CHECK(peerRefused(bytes));
    bytes = encode(PeerMessage{answer});
    bytes[12] = 0xFD; // ACCEPTED with an error...
    bytes[13] = bytes[14] = bytes[15] = 0xFF;
    CHECK(peerRefused(bytes));
    bytes = encode(PeerMessage{no});
    bytes[12] = 0; // ...REFUSED with RCS_OK...
    bytes[13] = bytes[14] = bytes[15] = 0;
    CHECK(peerRefused(bytes));
    bytes = encode(PeerMessage{no});
    bytes[12] = 0x80; // ...and a result no version defines.
    bytes[13] = bytes[14] = bytes[15] = 0xFF;
    CHECK(peerRefused(bytes));
}

void machineMessages() {
    CHECK(std::get<Hold>(decodeToMachine(encode(ToMachine{Hold{RCS_KEEP_AWAKE_DISPLAY}}))).mode ==
          RCS_KEEP_AWAKE_DISPLAY);
    const Act act{42, RCS_POWER_SLEEP, 0, 1000};
    const auto a = std::get<Act>(decodeToMachine(encode(ToMachine{act})));
    CHECK(a.serial == 42 && a.action == RCS_POWER_SLEEP && a.delayMs == 1000);
    CHECK(std::holds_alternative<List>(decodeToMachine(encode(ToMachine{List{}}))));
    CHECK(toMachineRefused({}) && toMachineRefused({4}) && toMachineRefused({1, 3}) && toMachineRefused({3, 0}));
    for (const uint32_t delay : {999u, 60001u}) {
        Bytes bytes = encode(ToMachine{Act{1, RCS_POWER_RESTART, 0, delay}});
        CHECK(toMachineRefused(bytes));
    }

    const auto held = std::get<Held>(decodeToSession(encode(ToSession{Held{RCS_KEEP_AWAKE_OFF, RCS_ERROR_INTERNAL,
                                                                           "No power requests here"}})));
    CHECK(held.held == RCS_KEEP_AWAKE_OFF && held.result == RCS_ERROR_INTERNAL &&
          held.message == "No power requests here");
    const auto can = std::get<Can>(decodeToSession(encode(ToSession{Can{RCS_POWER_ALL_ACTIONS, {}}})));
    CHECK(can.actions == RCS_POWER_ALL_ACTIONS);
    const auto acted = std::get<Acted>(
        decodeToSession(encode(ToSession{Acted{9, RCS_POWER_ACTION_FAILED, RCS_ERROR_INTERNAL, 0, "No"}})));
    CHECK(acted.serial == 9 && acted.state == RCS_POWER_ACTION_FAILED && acted.message == "No");
    Listed list{RCS_OK, {ethernet(), ethernet()}, {}};
    const auto listed = std::get<Listed>(decodeToSession(encode(ToSession{list})));
    CHECK(listed.result == RCS_OK && listed.interfaces.size() == 2 && same(listed.interfaces[1], ethernet()));

    CHECK(toSessionRefused({}) && toSessionRefused({5}) && toSessionRefused({1, 3, 0, 0, 0, 0, 0}));
    CHECK(toSessionRefused({2, 1, 0, 0, 0, 0})); // An action bit no version defines.
    Bytes bytes = encode(ToSession{Acted{9, RCS_POWER_ACTION_STARTED, RCS_OK, 0, {}}});
    bytes[10] = 0xFF; // STARTED with an error.
    CHECK(toSessionRefused(bytes));
    bytes = encode(ToSession{list});
    bytes.push_back(0);
    CHECK(toSessionRefused(bytes));
}

} // namespace

int main() {
    return runTest("power.messages", [] {
        interfaces();
        hello();
        peerMessages();
        machineMessages();
    });
}
