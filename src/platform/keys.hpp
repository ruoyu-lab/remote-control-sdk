// USB HID keyboard usages (rcs_input_event.code, 4..231) to the keys each
// system injects. A usage names a key by its place on the keyboard, not by
// what it types, and so do both targets: macOS virtual key codes and Windows
// scan codes. The keyboard layout of the shared machine decides the character.
//
// Plain numbers, so tests check both tables on any system.
//
// Not injected (0 / -1 below, RCS_ERROR_UNSUPPORTED):
// - Power (102): it acts on the machine, not on input; neither system has a
//   key event for it.
// - Execute through Find (116..126; but Help, 117, on macOS), keypad = for
//   AS/400 (134), International6..9 (140..143), LANG3..9 (146..152), and
//   153..221 (system request, keypad extras such as 00, hexadecimal and memory
//   keys): no key of either system's keyboards.
// - macOS: F21..F24 (112..115), Henkan and Muhenkan (138, 139) have no
//   virtual key code. Mute, Volume Up, Volume Down (127..129) have none
//   either; they go in as the consumer page's (below).
//
// Media keys (RCS_KEY_CONSUMER | consumer page usage): Windows virtual keys,
// sent as extended keys, and macOS system-defined auxiliary keys (NX_KEYTYPE).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rcsdk::platform::keys {

inline constexpr uint32_t firstUsage = 4, lastUsage = 231;

namespace detail {
template <class T>
using Table = std::array<T, lastUsage + 1>;

template <class T>
struct Entry {
    uint32_t usage;
    T key;
};

// Fills every usage not listed with `none`.
template <class T, size_t N>
constexpr Table<T> table(T none, const Entry<T> (&entries)[N]) {
    Table<T> result{};
    result.fill(none);
    for (const auto& [usage, key] : entries) {
        result[usage] = key;
    }
    return result;
}

// macOS (HIToolbox Events.h kVK_*). Keys of PC keyboards take the codes macOS
// gives them itself: Print Screen, Scroll Lock and Pause are F13..F15, Insert
// is Help, Num Lock is the keypad's Clear.
inline constexpr Entry<int> macEntries[] = {
    {4, 0x00},   {5, 0x0B},   {6, 0x08},   {7, 0x02},   {8, 0x0E},   {9, 0x03},   {10, 0x05},  {11, 0x04},
    {12, 0x22},  {13, 0x26},  {14, 0x28},  {15, 0x25},  {16, 0x2E},  {17, 0x2D},  {18, 0x1F},  {19, 0x23},
    {20, 0x0C},  {21, 0x0F},  {22, 0x01},  {23, 0x11},  {24, 0x20},  {25, 0x09},  {26, 0x0D},  {27, 0x07},
    {28, 0x10},  {29, 0x06},  {30, 0x12},  {31, 0x13},  {32, 0x14},  {33, 0x15},  {34, 0x17},  {35, 0x16},
    {36, 0x1A},  {37, 0x1C},  {38, 0x19},  {39, 0x1D},  {40, 0x24},  {41, 0x35},  {42, 0x33},  {43, 0x30},
    {44, 0x31},  {45, 0x1B},  {46, 0x18},  {47, 0x21},  {48, 0x1E},  {49, 0x2A},  {50, 0x2A},  {51, 0x29},
    {52, 0x27},  {53, 0x32},  {54, 0x2B},  {55, 0x2F},  {56, 0x2C},  {57, 0x39},  {58, 0x7A},  {59, 0x78},
    {60, 0x63},  {61, 0x76},  {62, 0x60},  {63, 0x61},  {64, 0x62},  {65, 0x64},  {66, 0x65},  {67, 0x6D},
    {68, 0x67},  {69, 0x6F},  {70, 0x69},  {71, 0x6B},  {72, 0x71},  {73, 0x72},  {74, 0x73},  {75, 0x74},
    {76, 0x75},  {77, 0x77},  {78, 0x79},  {79, 0x7C},  {80, 0x7B},  {81, 0x7D},  {82, 0x7E},  {83, 0x47},
    {84, 0x4B},  {85, 0x43},  {86, 0x4E},  {87, 0x45},  {88, 0x4C},  {89, 0x53},  {90, 0x54},  {91, 0x55},
    {92, 0x56},  {93, 0x57},  {94, 0x58},  {95, 0x59},  {96, 0x5B},  {97, 0x5C},  {98, 0x52},  {99, 0x41},
    {100, 0x0A}, {101, 0x6E}, {103, 0x51}, {104, 0x69}, {105, 0x6B}, {106, 0x71}, {107, 0x6A}, {108, 0x40},
    {109, 0x4F}, {110, 0x50}, {111, 0x5A}, {117, 0x72}, {133, 0x5F}, {135, 0x5E}, {136, 0x68}, {137, 0x5D},
    {144, 0x68}, {145, 0x66}, {224, 0x3B}, {225, 0x38}, {226, 0x3A}, {227, 0x37}, {228, 0x3E}, {229, 0x3C},
    {230, 0x3D}, {231, 0x36},
};

// Windows: scan code set 1 as Windows reports it, 0xE0xx for extended keys.
// Num Lock is extended and Pause is not (Windows' own convention for both,
// whose keyboards send 45 and E1 1D 45).
inline constexpr Entry<uint16_t> windowsEntries[] = {
    {4, 0x1E},     {5, 0x30},     {6, 0x2E},     {7, 0x20},     {8, 0x12},     {9, 0x21},     {10, 0x22},
    {11, 0x23},    {12, 0x17},    {13, 0x24},    {14, 0x25},    {15, 0x26},    {16, 0x32},    {17, 0x31},
    {18, 0x18},    {19, 0x19},    {20, 0x10},    {21, 0x13},    {22, 0x1F},    {23, 0x14},    {24, 0x16},
    {25, 0x2F},    {26, 0x11},    {27, 0x2D},    {28, 0x15},    {29, 0x2C},    {30, 0x02},    {31, 0x03},
    {32, 0x04},    {33, 0x05},    {34, 0x06},    {35, 0x07},    {36, 0x08},    {37, 0x09},    {38, 0x0A},
    {39, 0x0B},    {40, 0x1C},    {41, 0x01},    {42, 0x0E},    {43, 0x0F},    {44, 0x39},    {45, 0x0C},
    {46, 0x0D},    {47, 0x1A},    {48, 0x1B},    {49, 0x2B},    {50, 0x2B},    {51, 0x27},    {52, 0x28},
    {53, 0x29},    {54, 0x33},    {55, 0x34},    {56, 0x35},    {57, 0x3A},    {58, 0x3B},    {59, 0x3C},
    {60, 0x3D},    {61, 0x3E},    {62, 0x3F},    {63, 0x40},    {64, 0x41},    {65, 0x42},    {66, 0x43},
    {67, 0x44},    {68, 0x57},    {69, 0x58},    {70, 0xE037},  {71, 0x46},    {72, 0x45},    {73, 0xE052},
    {74, 0xE047},  {75, 0xE049},  {76, 0xE053},  {77, 0xE04F},  {78, 0xE051},  {79, 0xE04D},  {80, 0xE04B},
    {81, 0xE050},  {82, 0xE048},  {83, 0xE045},  {84, 0xE035},  {85, 0x37},    {86, 0x4A},    {87, 0x4E},
    {88, 0xE01C},  {89, 0x4F},    {90, 0x50},    {91, 0x51},    {92, 0x4B},    {93, 0x4C},    {94, 0x4D},
    {95, 0x47},    {96, 0x48},    {97, 0x49},    {98, 0x52},    {99, 0x53},    {100, 0x56},   {101, 0xE05D},
    {103, 0x59},   {104, 0x64},   {105, 0x65},   {106, 0x66},   {107, 0x67},   {108, 0x68},   {109, 0x69},
    {110, 0x6A},   {111, 0x6B},   {112, 0x6C},   {113, 0x6D},   {114, 0x6E},   {115, 0x76},   {127, 0xE020},
    {128, 0xE030}, {129, 0xE02E}, {133, 0x7E},   {135, 0x73},   {136, 0x70},   {137, 0x7D},
    {138, 0x79},   {139, 0x7B},   {144, 0x72},   {145, 0x71},   {224, 0x1D},   {225, 0x2A},   {226, 0x38},
    {227, 0xE05B}, {228, 0xE01D}, {229, 0x36},   {230, 0xE038}, {231, 0xE05C},
};

inline constexpr auto macTable = table(-1, macEntries);
inline constexpr auto windowsTable = table(uint16_t(0), windowsEntries);
} // namespace detail

// The macOS virtual key code of `usage`; -1 where macOS has none.
constexpr int macKeyCode(uint32_t usage) noexcept {
    return usage >= firstUsage && usage <= lastUsage ? detail::macTable[usage] : -1;
}

// The Windows scan code of `usage`, 0xE0xx for an extended key; 0 where
// Windows has none.
constexpr uint16_t windowsScanCode(uint32_t usage) noexcept {
    return usage >= firstUsage && usage <= lastUsage ? detail::windowsTable[usage] : 0;
}

// The consumer page usage of a keyboard page media key (Mute, Volume Up,
// Volume Down); 0 for any other.
constexpr uint16_t consumerOfKeyboard(uint32_t usage) noexcept {
    switch (usage) {
    case 127:
        return 0xE2;
    case 128:
        return 0xE9;
    case 129:
        return 0xEA;
    default:
        return 0;
    }
}

// The Windows virtual key of a consumer usage; 0 where Windows has none.
constexpr uint8_t windowsConsumerKey(uint32_t usage) noexcept {
    switch (usage) {
    case 0xB5:
        return 0xB0; // VK_MEDIA_NEXT_TRACK
    case 0xB6:
        return 0xB1; // VK_MEDIA_PREV_TRACK
    case 0xB7:
        return 0xB2; // VK_MEDIA_STOP
    case 0xCD:
        return 0xB3; // VK_MEDIA_PLAY_PAUSE
    case 0xE2:
        return 0xAD; // VK_VOLUME_MUTE
    case 0xE9:
        return 0xAF; // VK_VOLUME_UP
    case 0xEA:
        return 0xAE; // VK_VOLUME_DOWN
    case 0x18A:
        return 0xB4; // VK_LAUNCH_MAIL (AL Email Reader)
    case 0x221:
        return 0xAA; // VK_BROWSER_SEARCH (AC Search)
    case 0x223:
        return 0xAC; // VK_BROWSER_HOME
    case 0x224:
        return 0xA6; // VK_BROWSER_BACK
    case 0x225:
        return 0xA7; // VK_BROWSER_FORWARD
    case 0x226:
        return 0xA9; // VK_BROWSER_STOP
    case 0x227:
        return 0xA8; // VK_BROWSER_REFRESH
    case 0x22A:
        return 0xAB; // VK_BROWSER_FAVORITES (AC Bookmarks)
    default:
        return 0;
    }
}

// The modifiers an injector holds (usages 224..231, either side).
struct Modifiers {
    bool control = false, shift = false, alt = false, gui = false; // gui: the Windows key, Command.
};

// While remote input is confined to a shared window or application
// (RCS_CAPTURE_WINDOW, APPLICATION): whether a press of `usage` with
// `held` acts beyond it whichever window has the focus, so it is refused.
// Media keys of the keyboard page (Mute, Volume Up, Volume Down) on both;
// Windows: the Windows key (Start and every Windows key shortcut), Alt+Tab,
// Alt+Esc, Ctrl+Esc, Ctrl+Shift+Esc and Print Screen (the whole screen to
// the clipboard).
constexpr bool windowsBeyondConfinement(uint32_t usage, Modifiers held) noexcept {
    switch (usage) {
    case 227: // Left GUI.
    case 231: // Right GUI.
    case 70:  // Print Screen.
    case 127:
    case 128:
    case 129:
        return true;
    case 43: // Tab.
        return held.alt;
    case 41: // Escape.
        return held.alt || held.control;
    default:
        return false;
    }
}

// macOS: Command-Tab, Command-Space (Spotlight) and Command-Option-Space,
// Command-Option-Esc (Force Quit), Control-Command-Q (Lock Screen),
// Shift-Command-Q (Log Out), Shift-Command-3, 4, 5 and 6 (screenshots, with
// Control to the clipboard) and Control with an arrow key (Mission Control,
// the application's windows, other Spaces).
constexpr bool macBeyondConfinement(uint32_t usage, Modifiers held) noexcept {
    switch (usage) {
    case 127:
    case 128:
    case 129:
        return true;
    case 43: // Tab.
    case 44: // Space.
        return held.gui;
    case 41: // Escape.
        return held.gui && held.alt;
    case 20: // Q.
        return held.gui && (held.control || held.shift);
    case 32: // 3.
    case 33: // 4.
    case 34: // 5.
    case 35: // 6.
        return held.gui && held.shift;
    case 79: // Right.
    case 80: // Left.
    case 81: // Down.
    case 82: // Up.
        return held.control;
    default:
        return false;
    }
}

// The macOS auxiliary key (NX_KEYTYPE_*) of a consumer usage; -1 where macOS
// has none.
constexpr int macConsumerKey(uint32_t usage) noexcept {
    switch (usage) {
    case 0xE9:
        return 0; // NX_KEYTYPE_SOUND_UP
    case 0xEA:
        return 1; // NX_KEYTYPE_SOUND_DOWN
    case 0x6F:
        return 2; // NX_KEYTYPE_BRIGHTNESS_UP
    case 0x70:
        return 3; // NX_KEYTYPE_BRIGHTNESS_DOWN
    case 0xE2:
        return 7; // NX_KEYTYPE_MUTE
    case 0xB8:
        return 14; // NX_KEYTYPE_EJECT
    case 0xCD:
        return 16; // NX_KEYTYPE_PLAY
    case 0xB5:
        return 17; // NX_KEYTYPE_NEXT
    case 0xB6:
        return 18; // NX_KEYTYPE_PREVIOUS
    case 0xB3:
        return 19; // NX_KEYTYPE_FAST
    case 0xB4:
        return 20; // NX_KEYTYPE_REWIND
    default:
        return -1;
    }
}

} // namespace rcsdk::platform::keys
