#include "app_input.hpp"

#include "qt_input.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QScreen>
#include <QStyleHints>
#include <qpa/qwindowsysteminterface.h>

#include <utility>

namespace {

Qt::MouseButton qtButton(uint32_t code) {
    switch (code) {
    case RCS_BUTTON_LEFT:
        return Qt::LeftButton;
    case RCS_BUTTON_RIGHT:
        return Qt::RightButton;
    case RCS_BUTTON_MIDDLE:
        return Qt::MiddleButton;
    case RCS_BUTTON_BACK:
        return Qt::BackButton;
    case RCS_BUTTON_FORWARD:
        return Qt::ForwardButton;
    default:
        return Qt::NoButton;
    }
}

} // namespace

void AppInput::execute(const rcs_input_event& event) {
    if (!window_ || !window_->screen()) {
        return;
    }
    // 0..65535 across the shared picture: the screen.
    const QRectF screen = window_->screen()->geometry();
    const QPointF global(screen.x() + event.x * screen.width() / 65535.0,
                         screen.y() + event.y * screen.height() / 65535.0);
    const QPointF local = window_->mapFromGlobal(global);
    const int linesPerNotch = std::max(1, QGuiApplication::styleHints()->wheelScrollLines());
    switch (event.type) {
    case RCS_INPUT_MOVE:
        position_ = local;
        QWindowSystemInterface::handleMouseEvent(window_, local, global, buttons_, Qt::NoButton,
                                                           QEvent::MouseMove);
        break;
    case RCS_INPUT_BUTTON:
        if (const Qt::MouseButton button = qtButton(event.code)) {
            position_ = local;
            buttons_.setFlag(button, event.down != 0);
            QWindowSystemInterface::handleMouseEvent(
                window_, local, global, buttons_, button,
                event.down ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease);
        }
        break;
    case RCS_INPUT_WHEEL:
    case RCS_INPUT_SCROLL: { // Qt's eighths of a degree, 120 a notch; positive x is left, the SDK's right.
        const double perUnit = event.type == RCS_INPUT_WHEEL ? 120.0 / linesPerNotch : 1.0 / linesPerNotch;
        const QPoint angle(int(-event.wheel_x * perUnit), int(event.wheel_y * perUnit));
        QWindowSystemInterface::handleWheelEvent(window_, local, global, QPoint(), angle);
        break;
    }
    case RCS_INPUT_KEY:
        key(event.code, event.down != 0, event.repeat != 0);
        break;
    case RCS_INPUT_TEXT:
        if (QObject* focus = QGuiApplication::focusObject()) {
            QInputMethodEvent typed;
            const char32_t code = event.code;
            typed.setCommitString(QString::fromUcs4(&code, 1));
            QCoreApplication::sendEvent(focus, &typed);
        }
        break;
    case RCS_INPUT_RELEASE_ALL:
        releaseAll();
        break;
    default: // Touch and pen: not in this demo.
        break;
    }
}

void AppInput::key(uint32_t usage, bool down, bool repeat) {
    const demo::UsKey key = demo::usKey(usage);
    if (!key.qt) {
        return;
    }
    if (key.qt == Qt::Key_Shift) {
        shift_ = down;
    }
    const Qt::KeyboardModifiers modifiers = shift_ ? Qt::ShiftModifier : Qt::NoModifier;
    const char typed = shift_ ? key.shifted : key.text;
    QWindowSystemInterface::handleKeyEvent(window_, down ? QEvent::KeyPress : QEvent::KeyRelease, key.qt,
                                                     modifiers, typed ? QString(QChar(typed)) : QString(), repeat);
}

void AppInput::releaseAll() {
    for (const Qt::MouseButton button : {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton, Qt::BackButton,
                                         Qt::ForwardButton}) {
        if (buttons_ & button) {
            buttons_.setFlag(button, false);
            QWindowSystemInterface::handleMouseEvent(window_, position_, window_->mapToGlobal(position_),
                                                               buttons_, button, QEvent::MouseButtonRelease);
        }
    }
    shift_ = false;
}
