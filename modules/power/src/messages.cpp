#include "messages.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rcsdk::power {
namespace {

enum : uint8_t { askType = 1, answerType = 2 };
enum : uint8_t { holdType = 1, actType = 2, listType = 3 };
enum : uint8_t { heldType = 1, canType = 2, actedType = 3, listedType = 4 };
constexpr size_t maxName = 63;

[[noreturn]] void invalid(const char* what) {
    throw Refused(RCS_ERROR_PROTOCOL, std::string("Invalid power message: ") + what);
}

void put(Bytes& out, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(uint8_t(value >> (8 * i)));
    }
}

uint64_t get(std::span<const uint8_t> bytes, size_t& offset, int size) {
    if (bytes.size() - offset < size_t(size)) {
        invalid("cut short");
    }
    uint64_t value = 0;
    for (int i = 0; i < size; ++i) {
        value |= uint64_t(bytes[offset + size_t(i)]) << (8 * i);
    }
    offset += size_t(size);
    return value;
}

// Text: its length (one byte, at most `most`) and its bytes, no NUL among them.
void putText(Bytes& out, const std::string& text, size_t most) {
    const size_t size = std::min(text.size(), most);
    out.push_back(uint8_t(size));
    out.insert(out.end(), text.begin(), text.begin() + ptrdiff_t(size));
}

std::string getText(std::span<const uint8_t> bytes, size_t& offset, size_t most) {
    const size_t size = size_t(get(bytes, offset, 1));
    if (size > most || size > bytes.size() - offset) {
        invalid("its text");
    }
    std::string text(bytes.begin() + ptrdiff_t(offset), bytes.begin() + ptrdiff_t(offset + size));
    if (text.find('\0') != std::string::npos) {
        invalid("a NUL in its text");
    }
    offset += size;
    return text;
}

// A result some version of the SDK defines.
rcs_result getResult(std::span<const uint8_t> bytes, size_t& offset) {
    const auto result = rcs_result(int32_t(uint32_t(get(bytes, offset, 4))));
    if (result > RCS_OK || result < RCS_ERROR_REFUSED) {
        invalid("its result");
    }
    return result;
}

void putResult(Bytes& out, rcs_result result) {
    put(out, uint32_t(result), 4);
}

rcs_power_action getAction(std::span<const uint8_t> bytes, size_t& offset) {
    const auto action = rcs_power_action(get(bytes, offset, 1));
    if (action < RCS_POWER_RESTART || action > RCS_POWER_SIGN_OUT) {
        invalid("its action");
    }
    return action;
}

uint32_t getFlags(std::span<const uint8_t> bytes, size_t& offset) {
    const auto flags = uint32_t(get(bytes, offset, 1));
    if (flags & ~uint32_t(RCS_POWER_FORCE)) {
        invalid("its flags");
    }
    return flags;
}

rcs_power_action_state getState(std::span<const uint8_t> bytes, size_t& offset) {
    const auto state = rcs_power_action_state(get(bytes, offset, 1));
    if (state < RCS_POWER_ACTION_REFUSED || state > RCS_POWER_ACTION_FAILED) {
        invalid("its state");
    }
    return state;
}

rcs_keep_awake getMode(std::span<const uint8_t> bytes, size_t& offset) {
    const auto mode = rcs_keep_awake(get(bytes, offset, 1));
    if (mode > RCS_KEEP_AWAKE_DISPLAY) {
        invalid("what to keep awake");
    }
    return mode;
}

void putInterfaces(Bytes& out, const std::vector<rcs_power_interface>& interfaces) {
    const size_t count = std::min<size_t>(interfaces.size(), RCS_POWER_INTERFACES_MAX);
    out.push_back(uint8_t(count));
    for (size_t i = 0; i < count; ++i) {
        const auto& it = interfaces[i];
        out.insert(out.end(), std::begin(it.mac), std::end(it.mac));
        out.push_back(uint8_t(it.link));
        out.push_back(uint8_t(it.wake));
        uint32_t address = 0;
        parseAddress(it.address, address);
        for (int b = 3; b >= 0; --b) { // Network order.
            out.push_back(uint8_t(address >> (8 * b)));
        }
        out.push_back(uint8_t(it.prefix_length));
        putText(out, it.name, maxName);
    }
}

std::vector<rcs_power_interface> getInterfaces(std::span<const uint8_t> bytes, size_t& offset) {
    const size_t count = size_t(get(bytes, offset, 1));
    if (count > RCS_POWER_INTERFACES_MAX) {
        invalid("more interfaces than RCS_POWER_INTERFACES_MAX");
    }
    std::vector<rcs_power_interface> interfaces;
    for (size_t i = 0; i < count; ++i) {
        uint8_t mac[6];
        for (auto& b : mac) {
            b = uint8_t(get(bytes, offset, 1));
        }
        const auto link = rcs_power_link(get(bytes, offset, 1));
        const auto wake = rcs_power_wake_setting(get(bytes, offset, 1));
        uint32_t address = 0;
        for (int b = 0; b < 4; ++b) {
            address = address << 8 | uint32_t(get(bytes, offset, 1));
        }
        const auto prefix = uint32_t(get(bytes, offset, 1));
        const std::string name = getText(bytes, offset, maxName);
        if (link < RCS_POWER_LINK_ETHERNET || link > RCS_POWER_LINK_WIFI || wake > RCS_POWER_WAKE_DISABLED ||
            prefix > 32) {
            invalid("an interface");
        }
        interfaces.push_back(makeInterface(mac, link, wake, address, prefix, name));
    }
    return interfaces;
}

void end(std::span<const uint8_t> bytes, size_t offset) {
    if (offset != bytes.size()) {
        invalid("bytes after its end");
    }
}

std::string dotted(uint32_t address) {
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", address >> 24, (address >> 16) & 255u, (address >> 8) & 255u,
                  address & 255u);
    return text;
}

} // namespace

rcs_power_interface makeInterface(const uint8_t (&mac)[6], rcs_power_link link, rcs_power_wake_setting wake,
                                  uint32_t address, uint32_t prefix, const std::string& name) {
    rcs_power_interface it{};
    std::memcpy(it.mac, mac, sizeof(it.mac));
    it.link = link;
    it.wake = wake;
    std::snprintf(it.address, sizeof(it.address), "%s", dotted(address).c_str());
    it.prefix_length = prefix;
    const uint32_t host = prefix >= 32 ? 0u : ~0u >> prefix;
    std::snprintf(it.broadcast, sizeof(it.broadcast), "%s", dotted(address | host).c_str());
    // Cut at a character's start, so that what is cut stays UTF-8.
    size_t size = std::min(name.size(), std::min(maxName, sizeof(it.name) - 1));
    while (size < name.size() && size && (uint8_t(name[size]) & 0xC0) == 0x80) {
        --size;
    }
    std::memcpy(it.name, name.data(), size);
    return it;
}

bool parseAddress(const char* text, uint32_t& address) {
    uint32_t value = 0;
    for (int part = 0; part < 4; ++part) {
        if (part && *text++ != '.') {
            return false;
        }
        uint32_t number = 0;
        int digits = 0;
        for (; *text >= '0' && *text <= '9' && digits < 4; ++text, ++digits) {
            number = number * 10 + uint32_t(*text - '0');
        }
        if (!digits || digits > 3 || number > 255 || (digits > 1 && text[-digits] == '0')) {
            return false;
        }
        value = value << 8 | number;
    }
    if (*text) {
        return false;
    }
    address = value;
    return true;
}

// ---- To the other end -------------------------------------------------------------

Bytes encode(const Hello& hello) {
    Bytes out;
    put(out, hello.actions, 4);
    out.push_back(hello.wakeInfo ? 1 : 0);
    putInterfaces(out, hello.interfaces);
    return out;
}

Hello decodeHello(std::span<const uint8_t> bytes) {
    size_t offset = 0;
    Hello hello;
    hello.actions = uint32_t(get(bytes, offset, 4));
    const auto wakeInfo = get(bytes, offset, 1);
    if ((hello.actions & ~uint32_t(RCS_POWER_ALL_ACTIONS)) || wakeInfo > 1) {
        invalid("its hello");
    }
    hello.wakeInfo = wakeInfo == 1;
    hello.interfaces = getInterfaces(bytes, offset);
    if (!hello.wakeInfo && !hello.interfaces.empty()) {
        invalid("interfaces in a hello that tells none");
    }
    end(bytes, offset);
    return hello;
}

Bytes encode(const PeerMessage& message) {
    Bytes out;
    if (const auto* ask = std::get_if<Ask>(&message)) {
        out.push_back(askType);
        put(out, ask->request, 8);
        out.push_back(uint8_t(ask->action));
        out.push_back(uint8_t(ask->flags));
        return out;
    }
    const auto& answer = std::get<Answer>(message);
    out.push_back(answerType);
    put(out, answer.request, 8);
    out.push_back(uint8_t(answer.action));
    out.push_back(uint8_t(answer.flags));
    out.push_back(uint8_t(answer.state));
    putResult(out, answer.result);
    put(out, answer.delayMs, 4);
    putText(out, answer.message, maxText);
    return out;
}

PeerMessage decodePeer(std::span<const uint8_t> bytes) {
    size_t offset = 0;
    const auto type = uint8_t(get(bytes, offset, 1));
    if (type == askType) {
        Ask ask;
        ask.request = get(bytes, offset, 8);
        ask.action = getAction(bytes, offset);
        ask.flags = getFlags(bytes, offset);
        end(bytes, offset);
        return ask;
    }
    if (type != answerType) {
        invalid("its kind");
    }
    Answer answer;
    answer.request = get(bytes, offset, 8);
    answer.action = getAction(bytes, offset);
    answer.flags = getFlags(bytes, offset);
    answer.state = getState(bytes, offset);
    answer.result = getResult(bytes, offset);
    answer.delayMs = uint32_t(get(bytes, offset, 4));
    answer.message = getText(bytes, offset, maxText);
    end(bytes, offset);
    const bool ok = answer.state == RCS_POWER_ACTION_ACCEPTED || answer.state == RCS_POWER_ACTION_STARTED;
    if (ok != (answer.result == RCS_OK)) {
        invalid("a result that does not fit its state");
    }
    return answer;
}

// ---- Between the halves -------------------------------------------------------------

Bytes encode(const ToMachine& message) {
    Bytes out;
    if (const auto* hold = std::get_if<Hold>(&message)) {
        out.push_back(holdType);
        out.push_back(uint8_t(hold->mode));
    } else if (const auto* act = std::get_if<Act>(&message)) {
        out.push_back(actType);
        put(out, act->serial, 8);
        out.push_back(uint8_t(act->action));
        out.push_back(uint8_t(act->flags));
        put(out, act->delayMs, 4);
    } else {
        out.push_back(listType);
    }
    return out;
}

ToMachine decodeToMachine(std::span<const uint8_t> bytes) {
    size_t offset = 0;
    const auto type = uint8_t(get(bytes, offset, 1));
    ToMachine message;
    if (type == holdType) {
        message = Hold{getMode(bytes, offset)};
    } else if (type == actType) {
        Act act;
        act.serial = get(bytes, offset, 8);
        act.action = getAction(bytes, offset);
        act.flags = getFlags(bytes, offset);
        act.delayMs = uint32_t(get(bytes, offset, 4));
        if (act.delayMs < minActionDelayMs || act.delayMs > maxActionDelayMs) {
            invalid("its delay");
        }
        message = act;
    } else if (type == listType) {
        message = List{};
    } else {
        invalid("its kind");
    }
    end(bytes, offset);
    return message;
}

Bytes encode(const ToSession& message) {
    Bytes out;
    if (const auto* held = std::get_if<Held>(&message)) {
        out.push_back(heldType);
        out.push_back(uint8_t(held->held));
        putResult(out, held->result);
        putText(out, held->message, maxText);
    } else if (const auto* can = std::get_if<Can>(&message)) {
        out.push_back(canType);
        put(out, can->actions, 4);
        putText(out, can->message, maxText);
    } else if (const auto* acted = std::get_if<Acted>(&message)) {
        out.push_back(actedType);
        put(out, acted->serial, 8);
        out.push_back(uint8_t(acted->state));
        putResult(out, acted->result);
        put(out, acted->delayMs, 4);
        putText(out, acted->message, maxText);
    } else {
        const auto& listed = std::get<Listed>(message);
        out.push_back(listedType);
        putResult(out, listed.result);
        putInterfaces(out, listed.interfaces);
        putText(out, listed.message, maxText);
    }
    return out;
}

ToSession decodeToSession(std::span<const uint8_t> bytes) {
    size_t offset = 0;
    const auto type = uint8_t(get(bytes, offset, 1));
    ToSession message;
    if (type == heldType) {
        Held held;
        held.held = getMode(bytes, offset);
        held.result = getResult(bytes, offset);
        held.message = getText(bytes, offset, maxText);
        message = held;
    } else if (type == canType) {
        Can can;
        can.actions = uint32_t(get(bytes, offset, 4));
        if (can.actions & ~uint32_t(RCS_POWER_ALL_ACTIONS)) {
            invalid("its actions");
        }
        can.message = getText(bytes, offset, maxText);
        message = can;
    } else if (type == actedType) {
        Acted acted;
        acted.serial = get(bytes, offset, 8);
        acted.state = getState(bytes, offset);
        acted.result = getResult(bytes, offset);
        acted.delayMs = uint32_t(get(bytes, offset, 4));
        acted.message = getText(bytes, offset, maxText);
        const bool ok = acted.state == RCS_POWER_ACTION_ACCEPTED || acted.state == RCS_POWER_ACTION_STARTED;
        if (ok != (acted.result == RCS_OK)) {
            invalid("a result that does not fit its state");
        }
        message = acted;
    } else if (type == listedType) {
        Listed listed;
        listed.result = getResult(bytes, offset);
        listed.interfaces = getInterfaces(bytes, offset);
        listed.message = getText(bytes, offset, maxText);
        message = listed;
    } else {
        invalid("its kind");
    }
    end(bytes, offset);
    return message;
}

} // namespace rcsdk::power
