// Qt key and mouse values to SDK input codes, for the QML demo.
#pragma once

#include "../common/keyboard.hpp"

#include <rcsdk/rcsdk.h>

#include <QKeyEvent>
#include <Qt>

#include <cstdint>
#include <utility>

namespace demo {

// Qt key to USB HID keyboard usage, for keys Qt has no native code for; 0
// when unmapped. By what the key types (US layout), and on macOS, where Qt
// calls Command Key_Control, by the key's place: Command is the GUI key.
inline uint32_t hidUsage(int key) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return 4 + (key - Qt::Key_A);
    }
    if (key >= Qt::Key_1 && key <= Qt::Key_9) {
        return 30 + (key - Qt::Key_1);
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        return 58 + (key - Qt::Key_F1);
    }
    static constexpr std::pair<int, uint32_t> others[] = {
        {Qt::Key_0, 39},         {Qt::Key_Return, 40},      {Qt::Key_Enter, 40},        {Qt::Key_Escape, 41},
        {Qt::Key_Backspace, 42}, {Qt::Key_Tab, 43},         {Qt::Key_Space, 44},        {Qt::Key_Minus, 45},
        {Qt::Key_Equal, 46},     {Qt::Key_BracketLeft, 47}, {Qt::Key_BracketRight, 48}, {Qt::Key_Backslash, 49},
        {Qt::Key_Semicolon, 51}, {Qt::Key_Apostrophe, 52},  {Qt::Key_QuoteLeft, 53},    {Qt::Key_Comma, 54},
        {Qt::Key_Period, 55},    {Qt::Key_Slash, 56},       {Qt::Key_Home, 74},         {Qt::Key_PageUp, 75},
        {Qt::Key_Delete, 76},    {Qt::Key_End, 77},         {Qt::Key_PageDown, 78},     {Qt::Key_Right, 79},
        {Qt::Key_Left, 80},      {Qt::Key_Down, 81},        {Qt::Key_Up, 82},
#ifdef Q_OS_MACOS
        {Qt::Key_Control, 227},  {Qt::Key_Meta, 224},
#else
        {Qt::Key_Control, 224},  {Qt::Key_Meta, 227},
#endif
        {Qt::Key_Shift, 225},    {Qt::Key_Alt, 226}};
    for (const auto& [qt, usage] : others) {
        if (qt == key) {
            return usage;
        }
    }
    return 0;
}

// The key's place, from the native code Qt reports (examples/common/keyboard.hpp),
// so the shared machine's layout decides what it types; Qt's key otherwise. A
// Mac viewer's Command acts as Control on a machine that is not a Mac (and its
// Control as the Windows key), so Command+C copies there as on the Mac.
inline uint32_t hidUsage(const QKeyEvent& key, bool remoteIsMac) {
    uint32_t usage = 0;
#if defined(Q_OS_WIN)
    if (const quint32 scan = key.nativeScanCode()) { // An extended key: 0xE0xx from Qt 6.9 on, bit 8 before.
        usage = hidFromWindowsScanCode((scan & 0x100) ? 0xE000u | (scan & 0xff) : scan);
    }
#elif defined(Q_OS_MACOS)
    if (key.nativeVirtualKey() || key.key() == Qt::Key_A) { // kVK_ANSI_A is 0.
        usage = hidFromMacKeyCode(key.nativeVirtualKey());
    }
#endif
    if (!usage) {
        usage = hidUsage(key.key());
    }
#ifdef Q_OS_MACOS
    if (!remoteIsMac) {
        static constexpr std::pair<uint32_t, uint32_t> swapped[] = {{227, 224}, {224, 227}, {231, 228}, {228, 231}};
        for (const auto& [from, to] : swapped) {
            if (usage == from) {
                return to;
            }
        }
    }
#else
    (void)remoteIsMac;
#endif
    return usage;
}

// Wheel turns in lines, as the SDK takes them (rcs_input_event.wheel_x/y):
// Qt's eighths of a degree (120 a notch) times the lines a notch scrolls on
// this machine, kept across events so small trackpad steps add up.
class WheelLines {
public:
    // The whole lines `angleDelta` adds, the rest kept for the next event.
    int take(int angleDelta, int linesPerNotch) {
        rest_ += angleDelta * linesPerNotch;
        const int lines = rest_ / 120;
        rest_ -= lines * 120;
        return lines;
    }

private:
    int rest_ = 0;
};

// The other way, for a host executing a viewer's keys itself: a HID usage as
// a Qt key and what it types on a US layout, plain and shifted (0: nothing).
struct UsKey {
    int qt = 0;
    char text = 0, shifted = 0;
};
inline UsKey usKey(uint32_t usage) {
    if (usage >= 4 && usage <= 29) { // A..Z
        return {Qt::Key_A + int(usage - 4), char('a' + usage - 4), char('A' + usage - 4)};
    }
    if (usage >= 30 && usage <= 39) { // 1..9, 0
        return {usage == 39 ? Qt::Key_0 : Qt::Key_1 + int(usage - 30), "1234567890"[usage - 30],
                "!@#$%^&*()"[usage - 30]};
    }
    static constexpr std::pair<uint32_t, UsKey> others[] = {
        {40, {Qt::Key_Return}},          {41, {Qt::Key_Escape}},         {42, {Qt::Key_Backspace}},
        {43, {Qt::Key_Tab}},             {44, {Qt::Key_Space, ' ', ' '}}, {45, {Qt::Key_Minus, '-', '_'}},
        {46, {Qt::Key_Equal, '=', '+'}}, {47, {Qt::Key_BracketLeft, '[', '{'}},
        {48, {Qt::Key_BracketRight, ']', '}'}}, {49, {Qt::Key_Backslash, '\\', '|'}},
        {51, {Qt::Key_Semicolon, ';', ':'}},   {52, {Qt::Key_Apostrophe, '\'', '"'}},
        {53, {Qt::Key_QuoteLeft, '`', '~'}},   {54, {Qt::Key_Comma, ',', '<'}},
        {55, {Qt::Key_Period, '.', '>'}},      {56, {Qt::Key_Slash, '/', '?'}},
        {74, {Qt::Key_Home}},            {75, {Qt::Key_PageUp}},          {76, {Qt::Key_Delete}},
        {77, {Qt::Key_End}},             {78, {Qt::Key_PageDown}},        {79, {Qt::Key_Right}},
        {80, {Qt::Key_Left}},            {81, {Qt::Key_Down}},            {82, {Qt::Key_Up}},
        {225, {Qt::Key_Shift}},          {229, {Qt::Key_Shift}}};
    for (const auto& [hid, key] : others) {
        if (hid == usage) {
            return key;
        }
    }
    return {};
}

// Qt mouse button to RCS_BUTTON_*; 0 for buttons the SDK does not forward.
inline uint32_t mouseButton(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton:
        return RCS_BUTTON_LEFT;
    case Qt::RightButton:
        return RCS_BUTTON_RIGHT;
    case Qt::MiddleButton:
        return RCS_BUTTON_MIDDLE;
    case Qt::BackButton:
        return RCS_BUTTON_BACK;
    case Qt::ForwardButton:
        return RCS_BUTTON_FORWARD;
    default:
        return 0;
    }
}

} // namespace demo
