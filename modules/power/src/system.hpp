// What the machine's system does for actions and waking: the system's, unless
// a test put its own in place. Tests never restart, shut down, sleep or sign
// out the machine they run on: they replace it (setMachine) first.
#pragma once

#include "power_request.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rcsdk::power {

// The actions this process can have its machine carry out (bits), and why
// not the others.
struct Abilities {
    uint32_t actions = 0;
    std::string why;
};

class Machine {
public:
    virtual ~Machine() = default;
    virtual Abilities abilities() = 0;
    // Throws Refused (UNSUPPORTED: the reason) where this process cannot
    // carry out `action` with `flags`: checked before it is accepted.
    virtual void check(rcs_power_action action, uint32_t flags) = 0;
    // Carries it out now; throws Refused where the system does not take it.
    virtual void act(rcs_power_action action, uint32_t flags) = 0;
    // The interfaces that can wake the machine, Ethernet first; throws
    // Refused (UNSUPPORTED) where the system tells none...
    virtual std::vector<rcs_power_interface> interfaces() = 0;
    // ...and why it tells none there; empty where it does.
    virtual std::string noInterfaces() = 0;
};

// The system's.
std::shared_ptr<Machine> systemMachine();
// What the module uses: the system's, unless a test set its own (nullptr
// restores the system's).
void setMachine(std::shared_ptr<Machine> machine);
std::shared_ptr<Machine> machine();

// Sends a magic packet (rcs_power_send_wake, checked); throws Refused.
void sendWake(const rcs_power_wake& wake);
bool sendsWake();

// An action's name, as reasons say it ("restart").
const char* actionName(rcs_power_action action);

} // namespace rcsdk::power
