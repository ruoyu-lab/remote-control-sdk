// The module's own messages (version 2 of "rcsdk.power"): the sharer's hello,
// a viewer's request and its answers, and what the two halves tell each
// other. Whatever comes is checked: the other end, and a sharer's agent of
// another process, may be anyone's. Decoding throws Refused(PROTOCOL).
#pragma once

#include "power_request.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace rcsdk::power {

inline constexpr uint32_t moduleVersion = 1;
inline constexpr uint32_t maxMessageBytes = 1024;
inline constexpr size_t maxText = 127; // Bytes of a reason on the wire.
// How long the machine waits before it carries out an action (rcs_power_settings.action_delay_ms).
inline constexpr uint32_t minActionDelayMs = 1000, maxActionDelayMs = 60000;

using Bytes = std::vector<uint8_t>;

// An interface on the wire: its MAC address, kind, wake setting, IPv4
// address and prefix, and name (at most 63 bytes); the broadcast address
// follows from the address and prefix.
rcs_power_interface makeInterface(const uint8_t (&mac)[6], rcs_power_link link, rcs_power_wake_setting wake,
                                  uint32_t address, uint32_t prefix, const std::string& name);
// The address in a dotted IPv4 text ("192.168.1.20": four numbers 0..255,
// no leading zeros, nothing else), in host order; false for anything else.
bool parseAddress(const char* text, uint32_t& address);

// ---- To the other end -------------------------------------------------------------

// The sharer's hello: the actions it offers, whether it tells its wake
// information, and its interfaces (at most RCS_POWER_INTERFACES_MAX). A
// viewer's hello is empty.
struct Hello {
    uint32_t actions = 0;
    bool wakeInfo = false;
    std::vector<rcs_power_interface> interfaces;
};
Bytes encode(const Hello& hello);
Hello decodeHello(std::span<const uint8_t> bytes);

// A viewer's request, and the sharer's answers to it.
struct Ask {
    uint64_t request = 0;
    rcs_power_action action = 0;
    uint32_t flags = 0;
};
struct Answer {
    uint64_t request = 0;
    rcs_power_action action = 0;
    uint32_t flags = 0;
    rcs_power_action_state state = 0;
    rcs_result result = RCS_OK;
    uint32_t delayMs = 0;
    std::string message;
};
using PeerMessage = std::variant<Ask, Answer>;
Bytes encode(const PeerMessage& message);
PeerMessage decodePeer(std::span<const uint8_t> bytes);

// ---- Between the halves -------------------------------------------------------------

// The session's half to the machine's: what to keep awake; carry out an
// action after a delay (serial: the session's half's number for it); list
// the interfaces.
struct Hold {
    rcs_keep_awake mode = RCS_KEEP_AWAKE_OFF;
};
struct Act {
    uint64_t serial = 0;
    rcs_power_action action = 0;
    uint32_t flags = 0;
    uint32_t delayMs = 0;
};
struct List {};
using ToMachine = std::variant<Hold, Act, List>;
Bytes encode(const ToMachine& message);
ToMachine decodeToMachine(std::span<const uint8_t> bytes);

// The machine's half to the session's: what it holds; the actions it can
// carry out (and why not the others), at its start; a step of an action; the
// interfaces, or why none.
struct Held {
    rcs_keep_awake held = RCS_KEEP_AWAKE_OFF;
    rcs_result result = RCS_OK;
    std::string message;
};
struct Can {
    uint32_t actions = 0;
    std::string message;
};
struct Acted {
    uint64_t serial = 0;
    rcs_power_action_state state = 0;
    rcs_result result = RCS_OK;
    uint32_t delayMs = 0;
    std::string message;
};
struct Listed {
    rcs_result result = RCS_OK;
    std::vector<rcs_power_interface> interfaces;
    std::string message;
};
using ToSession = std::variant<Held, Can, Acted, Listed>;
Bytes encode(const ToSession& message);
ToSession decodeToSession(std::span<const uint8_t> bytes);

} // namespace rcsdk::power
