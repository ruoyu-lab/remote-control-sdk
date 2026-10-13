// The keys each system injects for a HID usage: every key either system can
// type is mapped, by its place on the keyboard; keys that share a target are
// the known aliases; Windows extended keys are told apart from their twins.
#include "platform/keys.hpp"
#include "support/check.hpp"

#include <map>
#include <set>
#include <vector>

using namespace rcsdk::platform;

namespace {

// Usages of keys found on Windows (PC) and Mac keyboards beyond the basic set.
const std::vector<uint32_t> added = [] {
    std::vector<uint32_t> usages{57, 70, 71, 72, 73, 83, 100, 101, 103, 133, 135, 136, 137, 138, 139, 144, 145};
    for (uint32_t usage = 84; usage <= 99; ++usage) { // The keypad.
        usages.push_back(usage);
    }
    for (uint32_t usage = 104; usage <= 115; ++usage) { // F13..F24.
        usages.push_back(usage);
    }
    return usages;
}();

// Usages no key code exists for (see keys.hpp).
const std::set<uint32_t> macMissing{112, 113, 114, 115, 127, 128, 129, 138, 139};

// Input confined to a window or an application (SA-06): the presses each
// system acts on whichever window has the focus are refused; the shared
// application's own shortcuts are not.
void confinement() {
    const keys::Modifiers none{}, alt{.alt = true}, control{.control = true}, gui{.gui = true};
    const keys::Modifiers guiShift{.shift = true, .gui = true}, guiAlt{.alt = true, .gui = true},
        guiControl{.control = true, .gui = true}, controlShift{.control = true, .shift = true};
    for (const uint32_t media : {127u, 128u, 129u}) {
        CHECK(keys::windowsBeyondConfinement(media, none) && keys::macBeyondConfinement(media, none));
    }
    // Windows: the Windows key, the task switcher, Start, Task Manager, Print Screen.
    CHECK(keys::windowsBeyondConfinement(227, none) && keys::windowsBeyondConfinement(231, control));
    CHECK(keys::windowsBeyondConfinement(43, alt) && !keys::windowsBeyondConfinement(43, none));
    CHECK(keys::windowsBeyondConfinement(41, alt) && keys::windowsBeyondConfinement(41, control) &&
          keys::windowsBeyondConfinement(41, controlShift) && !keys::windowsBeyondConfinement(41, none));
    CHECK(keys::windowsBeyondConfinement(70, none));
    // The application's own: Ctrl+C, Alt+F4 (its window), Ctrl+Tab, Alt and Shift alone.
    CHECK(!keys::windowsBeyondConfinement(6, control) && !keys::windowsBeyondConfinement(61, alt) &&
          !keys::windowsBeyondConfinement(43, control) && !keys::windowsBeyondConfinement(226, none) &&
          !keys::windowsBeyondConfinement(225, none));
    // macOS: the application switcher, Spotlight, Force Quit, Lock Screen, Log Out, screenshots, Mission Control.
    CHECK(keys::macBeyondConfinement(43, gui) && keys::macBeyondConfinement(44, gui) &&
          keys::macBeyondConfinement(44, guiAlt) && keys::macBeyondConfinement(41, guiAlt));
    CHECK(keys::macBeyondConfinement(20, guiControl) && keys::macBeyondConfinement(20, guiShift));
    for (const uint32_t digit : {32u, 33u, 34u, 35u}) {
        CHECK(keys::macBeyondConfinement(digit, guiShift) && !keys::macBeyondConfinement(digit, gui));
    }
    for (uint32_t arrow = 79; arrow <= 82; ++arrow) {
        CHECK(keys::macBeyondConfinement(arrow, control) && !keys::macBeyondConfinement(arrow, gui));
    }
    // The application's own: Command-C, Command-Q (quits it), Command-W, Escape, Command itself.
    CHECK(!keys::macBeyondConfinement(6, gui) && !keys::macBeyondConfinement(20, gui) &&
          !keys::macBeyondConfinement(26, gui) && !keys::macBeyondConfinement(41, gui) &&
          !keys::macBeyondConfinement(227, none) && !keys::macBeyondConfinement(43, none));
}

void windows() {
    std::map<uint16_t, std::vector<uint32_t>> targets;
    for (uint32_t usage = keys::firstUsage; usage <= keys::lastUsage; ++usage) {
        if (const uint16_t scan = keys::windowsScanCode(usage)) {
            CHECK((scan & 0xff00) == 0 || (scan & 0xff00) == 0xe000);
            CHECK((scan & 0xff) && (scan & 0xff) < 0x80); // Make codes only.
            targets[scan].push_back(usage);
        }
    }
    for (const uint32_t usage : added) {
        CHECK(keys::windowsScanCode(usage) != 0);
    }
    for (const auto& [scan, usages] : targets) { // Backslash and the ISO key beside Enter share their place.
        CHECK(usages.size() == 1 || (scan == 0x2b && usages == std::vector<uint32_t>{49, 50}));
    }
    // Twins: the extended key is the other one.
    CHECK(keys::windowsScanCode(40) == 0x1c && keys::windowsScanCode(88) == 0xe01c);  // Enter, keypad Enter.
    CHECK(keys::windowsScanCode(56) == 0x35 && keys::windowsScanCode(84) == 0xe035);  // Slash, keypad Divide.
    CHECK(keys::windowsScanCode(85) == 0x37 && keys::windowsScanCode(70) == 0xe037);  // Keypad *, Print Screen.
    CHECK(keys::windowsScanCode(72) == 0x45 && keys::windowsScanCode(83) == 0xe045);  // Pause, Num Lock.
    CHECK(keys::windowsScanCode(226) == 0x38 && keys::windowsScanCode(230) == 0xe038); // Left, right Alt.
    CHECK(keys::windowsScanCode(98) == 0x52 && keys::windowsScanCode(73) == 0xe052);  // Keypad 0, Insert.
    CHECK(keys::windowsScanCode(102) == 0 && keys::windowsScanCode(3) == 0 && keys::windowsScanCode(232) == 0);
}

void mac() {
    std::map<int, std::vector<uint32_t>> targets;
    for (uint32_t usage = keys::firstUsage; usage <= keys::lastUsage; ++usage) {
        if (const int code = keys::macKeyCode(usage); code >= 0) {
            CHECK(code < 0x80);
            targets[code].push_back(usage);
        }
    }
    for (const uint32_t usage : added) {
        CHECK(macMissing.count(usage) || keys::macKeyCode(usage) >= 0);
    }
    for (const uint32_t usage : macMissing) {
        CHECK(keys::macKeyCode(usage) == -1);
    }
    // macOS's own aliases: Print Screen, Scroll Lock and Pause are F13..F15,
    // Insert is Help, the ISO key beside Enter is Backslash, and the JIS Kana
    // key answers to both Katakana/Hiragana and LANG1.
    const std::map<int, std::vector<uint32_t>> aliases{
        {0x69, {70, 104}}, {0x6b, {71, 105}}, {0x71, {72, 106}}, {0x72, {73, 117}}, {0x2a, {49, 50}}, {0x68, {136, 144}}};
    for (const auto& [code, usages] : targets) {
        const auto alias = aliases.find(code);
        CHECK(usages.size() == 1 || (alias != aliases.end() && alias->second == usages));
    }
    CHECK(keys::macKeyCode(40) == 0x24 && keys::macKeyCode(88) == 0x4c); // Return, keypad Enter.
    CHECK(keys::macKeyCode(4) == 0x00 && keys::macKeyCode(29) == 0x06);  // A, Z.
    CHECK(keys::macKeyCode(83) == 0x47 && keys::macKeyCode(57) == 0x39); // Num Lock is Clear; Caps Lock.
    CHECK(keys::macKeyCode(102) == -1 && keys::macKeyCode(232) == -1);
}

// Media keys: the transport and volume keys on both systems, each to its own
// key; the keyboard page's volume keys as the consumer page's.
void consumer() {
    const std::vector<uint32_t> both{0xB5, 0xB6, 0xCD, 0xE2, 0xE9, 0xEA};
    std::set<uint8_t> windowsKeys;
    std::set<int> macKeys;
    for (const uint32_t usage : both) {
        CHECK(keys::windowsConsumerKey(usage) != 0 && keys::macConsumerKey(usage) >= 0);
        windowsKeys.insert(keys::windowsConsumerKey(usage));
        macKeys.insert(keys::macConsumerKey(usage));
    }
    CHECK(windowsKeys.size() == both.size() && macKeys.size() == both.size());
    CHECK(keys::windowsConsumerKey(0x224) == 0xA6 && keys::macConsumerKey(0x224) == -1); // Back: Windows only.
    CHECK(keys::macConsumerKey(0x6F) == 2 && keys::windowsConsumerKey(0x6F) == 0);       // Brightness: macOS only.
    CHECK(keys::windowsConsumerKey(0x1) == 0 && keys::macConsumerKey(0x1) == -1);
    CHECK(keys::consumerOfKeyboard(127) == 0xE2 && keys::consumerOfKeyboard(128) == 0xE9 &&
          keys::consumerOfKeyboard(129) == 0xEA && keys::consumerOfKeyboard(4) == 0);
}

} // namespace

int main() {
    return runTest("keys", [] {
        windows();
        mac();
        confinement();
        consumer();
    });
}
