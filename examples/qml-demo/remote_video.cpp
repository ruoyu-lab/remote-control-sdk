#include "remote_video.hpp"

#include "qt_input.hpp"
#include "video_node.hpp"

#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QStyleHints>

#include <QCursor>
#include <QLineF>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QPainter>
#include <QQuickPaintedItem>
#include <QQuickWindow>

#include <algorithm>

namespace {

// iOS and Android have no pointer of their own to give the remote one's shape.
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
constexpr bool localPointer = false;
#else
constexpr bool localPointer = true;
#endif

constexpr double touchSlop = 10;          // Item points a finger may wander and still tap.
constexpr int longPressMs = 500;
constexpr double scrollPerPoint = 4;      // SCROLL steps (1/120 line) per item point.
constexpr double trackpadSpeed = 1.6;     // Display widths per picture width a finger travels.

// Draws one cursor image, scaled to the item's size.
class CursorOverlay final : public QQuickPaintedItem {
public:
    using QQuickPaintedItem::QQuickPaintedItem;
    void show(const QImage& image) {
        if (image.cacheKey() != image_.cacheKey()) {
            image_ = image;
            update();
        }
    }
    void paint(QPainter* painter) override {
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(boundingRect(), image_);
    }

private:
    QImage image_;
};

} // namespace

RemoteVideo::RemoteVideo(QQuickItem* parent) : QQuickItem(parent) {
    overlay_ = new CursorOverlay(this);
    overlay_->setVisible(false);
    overlay_->setZ(1);
    connect(this, &RemoteVideo::hasFrameChanged, this, &RemoteVideo::updateCursor);
    connect(this, &RemoteVideo::fillModeChanged, this, &RemoteVideo::updateCursor);
    connect(this, &QQuickItem::widthChanged, this, &RemoteVideo::updateCursor);
    connect(this, &QQuickItem::heightChanged, this, &RemoteVideo::updateCursor);
    setFlag(ItemHasContents);
    setFlag(ItemIsFocusScope);
    setActiveFocusOnTab(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptTouchEvents(true);
    longPress_.setSingleShot(true);
    longPress_.setInterval(longPressMs);
    connect(&longPress_, &QTimer::timeout, this, &RemoteVideo::longPressed);
    connect(QGuiApplication::inputMethod(), &QInputMethod::visibleChanged, this, [this] {
        if (softKeyboard_ && !QGuiApplication::inputMethod()->isVisible()) { // Closed by its own button.
            setSoftKeyboard(false);
        }
    });
    fpsTimer_.setInterval(1000);
    connect(&fpsTimer_, &QTimer::timeout, this, [this] {
        const double seconds = fpsClock_.isValid() ? fpsClock_.restart() / 1000.0 : 0;
        renderedFps_ = seconds > 0 ? presented_.exchange(0) / seconds : 0;
        emit renderedFpsChanged();
    });
    fpsClock_.start();
    fpsTimer_.start();
}

RemoteVideo::~RemoteVideo() {
    if (session_) {
        session_->setRenderWindow(nullptr);
    }
}

void RemoteVideo::setSession(RemoteSession* session) {
    if (session_ == session) {
        return;
    }
    if (session_) {
        session_->setRenderWindow(nullptr);
        disconnect(session_, nullptr, this, nullptr);
    }
    session_ = session;
    if (session_) {
        connect(session_, &RemoteSession::frameReady, this, &QQuickItem::update);
        connect(session_, &RemoteSession::cursorChanged, this, &RemoteVideo::updateCursor);
        connect(session_, &RemoteSession::statisticsChanged, this, &RemoteVideo::updateCursor); // Stream changes.
    }
    emit sessionChanged();
    update();
}

void RemoteVideo::setFillMode(FillMode mode) {
    fillMode_ = mode;
    panX_ = panY_ = 0;
    emit fillModeChanged();
    update();
}

void RemoteVideo::setSmoothScaling(bool smooth) {
    smooth_ = smooth;
    emit fillModeChanged();
    update();
}

void RemoteVideo::setInputEnabled(bool enabled) {
    if (inputEnabled_ == enabled) {
        return;
    }
    if (inputEnabled_) {
        rcs_input_event release = RCS_INPUT_EVENT_INIT;
        release.type = RCS_INPUT_RELEASE_ALL;
        send(release);
    }
    inputEnabled_ = enabled;
    setAcceptHoverEvents(enabled);
    longPress_.stop();
    gesture_ = Gesture::None;
    if (!enabled) {
        setSoftKeyboard(false);
    }
    emit inputEnabledChanged();
    updateCursor();
}

void RemoteVideo::setTouchMode(TouchMode mode) {
    if (mode != touchMode_) {
        touchMode_ = mode;
        trackKnown_ = false; // Picked up from where the remote pointer is.
        emit touchModeChanged();
    }
}

void RemoteVideo::setSoftKeyboard(bool shown) {
    if (shown == softKeyboard_ || (shown && !inputEnabled_)) {
        return;
    }
    softKeyboard_ = shown;
    composing_.clear();
    setFlag(ItemAcceptsInputMethod, shown); // Otherwise keys go to the remote machine as keys.
    QInputMethod* method = QGuiApplication::inputMethod();
    if (shown) {
        forceActiveFocus(Qt::OtherFocusReason);
        method->update(Qt::ImEnabled | Qt::ImHints);
        method->show();
    } else {
        method->hide();
    }
    emit softKeyboardChanged();
}

void RemoteVideo::updateCursor() {
    const auto hide = [&] {
        overlay_->setVisible(false);
        if (pointerShape_) {
            unsetCursor();
            pointerShape_ = 0;
        }
    };
    const auto* shape = session_ ? session_->cursorShape(session_->cursor().shapeId) : nullptr;
    if (!shape || !frameWidth_ || !window()) {
        hide();
        return;
    }
    const auto cursor = session_->cursor();
    const auto info = session_->streamInfo();
    const qreal ratio = window()->effectiveDevicePixelRatio();
    const auto geometry = geometryFor(frameWidth_, frameHeight_);
    const double scale = rcsdk::displayScale(geometry, info) / ratio; // Item points per display pixel.
    const QSizeF size(shape->image.width() * scale, shape->image.height() * scale);
    if (inputEnabled_ && localPointer) { // The local pointer, in the remote pointer's shape.
        overlay_->setVisible(false);
        if (!cursor.visible) {
            setCursor(Qt::BlankCursor);
            pointerShape_ = UINT32_MAX;
        } else if (cursor.shapeId != pointerShape_ || scale != pointerScale_) {
            QPixmap pixmap = QPixmap::fromImage(shape->image.scaled((size * ratio).toSize(), Qt::IgnoreAspectRatio,
                                                                    Qt::SmoothTransformation));
            pixmap.setDevicePixelRatio(ratio);
            setCursor(QCursor(pixmap, int(shape->hotspot.x() * scale), int(shape->hotspot.y() * scale)));
            pointerShape_ = cursor.shapeId;
            pointerScale_ = scale;
        }
        return;
    }
    if (pointerShape_) {
        unsetCursor();
        pointerShape_ = 0;
    }
    uint32_t x = cursor.x, y = cursor.y;
    rcsdk::framePosition(x, y, info);
    double px = 0, py = 0;
    if (!cursor.visible || !geometry.point(x, y, frameWidth_, frameHeight_, px, py)) {
        overlay_->setVisible(false);
        return;
    }
    static_cast<CursorOverlay*>(overlay_)->show(shape->image);
    overlay_->setSize(size);
    overlay_->setPosition(QPointF(px / ratio - shape->hotspot.x() * scale, py / ratio - shape->hotspot.y() * scale));
    overlay_->setVisible(true);
}

void RemoteVideo::reportImport(const QString& error) {
    std::lock_guard lock(importMutex_);
    importError_ = error;
}

rcsdk::GpuFrame newerFrame(RemoteSession* session, const rcsdk::GpuFrame& drawn) {
    if (!session || !drawn) {
        return {};
    }
    auto newest = session->latestFrame();
    if (!newest || newest.handle() == drawn.handle()) {
        return {};
    }
    const auto a = newest.info(), b = drawn.info();
    const bool same = a.width == b.width && a.height == b.height && a.surface_width == b.surface_width &&
                      a.surface_height == b.surface_height && a.format == b.format && a.sequence > b.sequence;
    return same ? newest : rcsdk::GpuFrame();
}

rcsdk::ViewGeometry RemoteVideo::geometryFor(uint32_t frameWidth, uint32_t frameHeight) const {
    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    return rcsdk::viewGeometry(int(width() * ratio), int(height() * ratio), frameWidth, frameHeight,
                               fillMode_ == NativePixels ? rcsdk::ViewScale::nativePixels : rcsdk::ViewScale::fit,
                               panX_, panY_);
}

QSGNode* RemoteVideo::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    const rcsdk::GpuFrame frame = session_ ? session_->latestFrame() : rcsdk::GpuFrame();
    const bool had = hasFrame_;
    const QString previousError = frameError_;
    {
        std::lock_guard lock(importMutex_);
        frameError_ = importError_;
    }
    hasFrame_ = bool(frame);
    if (!frame) {
        if (session_) {
            session_->setRenderWindow(nullptr);
        }
        delete old;
        frameWidth_ = frameHeight_ = 0;
        if (had) {
            QMetaObject::invokeMethod(this, &RemoteVideo::hasFrameChanged, Qt::QueuedConnection);
        }
        return nullptr;
    }
    auto* node = old ? static_cast<FrameNode*>(old) : createFrameNode(window(), this);
    const auto info = frame.info();
    frameWidth_ = info.width;
    frameHeight_ = info.height;
    const qreal ratio = window()->effectiveDevicePixelRatio();
    const auto g = geometryFor(info.width, info.height);
    const QRectF target(g.x / ratio, g.y / ratio, g.width / ratio, g.height / ratio);
    // Normalized source rectangle; the surface may be padded beyond the image.
    const QRectF source(double(g.sourceX) / info.surface_width, double(g.sourceY) / info.surface_height,
                        double(g.sourceWidth) / info.surface_width, double(g.sourceHeight) / info.surface_height);
    node->update(frame, target, source, smooth_, session_);
    session_->setRenderWindow(window()); // Newer frames of this shape: drawn without a synchronization.
    if (!had || frameError_ != previousError) {
        QMetaObject::invokeMethod(this, &RemoteVideo::hasFrameChanged, Qt::QueuedConnection);
    }
    return node;
}

bool RemoteVideo::position(QPointF point, rcs_input_event& event, bool clamp) const {
    if (!frameWidth_ || !session_) {
        return false;
    }
    const qreal ratio = window()->effectiveDevicePixelRatio();
    // Item points -> encoded frame -> shared display (removing letterboxing).
    return geometryFor(frameWidth_, frameHeight_)
               .position(point.x() * ratio, point.y() * ratio, frameWidth_, frameHeight_, event.x, event.y, clamp) &&
           rcsdk::displayPosition(event.x, event.y, session_->streamInfo(), clamp);
}

void RemoteVideo::send(const rcs_input_event& event) {
    if (session_) {
        session_->sendInput(event);
    }
}

// A pointer that hovers. Touches make none (their gestures move the remote
// pointer), and Qt Quick hovers again each frame where nothing moved.
void RemoteVideo::hoverMoveEvent(QHoverEvent* hover) {
    if (hover->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen || gesture_ != Gesture::None ||
        hover->position() == lastHover_) {
        return;
    }
    lastHover_ = hover->position();
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_MOVE;
    if (inputEnabled_ && position(hover->position(), event, false)) {
        send(event);
    }
}

void RemoteVideo::mouseMoveEvent(QMouseEvent* mouse) {
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_MOVE;
    if (inputEnabled_ && position(mouse->position(), event, true)) { // Drags clamp to the edge.
        send(event);
    }
}

void RemoteVideo::button(QMouseEvent* mouse, bool down) {
    forceActiveFocus(Qt::MouseFocusReason);
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_BUTTON;
    event.down = down;
    event.code = demo::mouseButton(mouse->button());
    if (inputEnabled_ && event.code && position(mouse->position(), event, !down)) {
        send(event);
    }
    mouse->accept(); // Keep the grab so the release arrives here.
}

void RemoteVideo::mousePressEvent(QMouseEvent* mouse) {
    button(mouse, true);
}

void RemoteVideo::mouseReleaseEvent(QMouseEvent* mouse) {
    button(mouse, false);
}

void RemoteVideo::wheelEvent(QWheelEvent* wheel) {
    if (!inputEnabled_) { // Without control, the wheel pans a 1:1 view.
        if (fillMode_ == NativePixels && frameWidth_) {
            const qreal ratio = window()->effectiveDevicePixelRatio();
            panX_ =
                std::clamp(panX_ - wheel->angleDelta().x(), 0, std::max(0, int(frameWidth_) - int(width() * ratio)));
            panY_ =
                std::clamp(panY_ - wheel->angleDelta().y(), 0, std::max(0, int(frameHeight_) - int(height() * ratio)));
            update();
        }
        return;
    }
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_WHEEL;
    const int lines = QGuiApplication::styleHints()->wheelScrollLines();
    event.wheel_x = wheelX_.take(wheel->angleDelta().x(), lines);
    event.wheel_y = wheelY_.take(wheel->angleDelta().y(), lines);
    if ((event.wheel_x || event.wheel_y) && position(wheel->position(), event, false)) {
        send(event);
    }
}

void RemoteVideo::key(QKeyEvent* key, bool down) {
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_KEY;
    event.code = demo::hidUsage(*key, session_ && session_->peerPlatform() == RCS_PLATFORM_MACOS);
    event.down = down;
    event.repeat = key->isAutoRepeat();
    if (inputEnabled_ && event.code) {
        send(event);
        key->accept();
    } else {
        key->ignore();
    }
}

void RemoteVideo::keyPressEvent(QKeyEvent* event) {
    key(event, true);
}

void RemoteVideo::keyReleaseEvent(QKeyEvent* event) {
    if (!event->isAutoRepeat()) {
        key(event, false);
    }
}

void RemoteVideo::focusOutEvent(QFocusEvent* event) {
    if (inputEnabled_) { // No key may stay pressed on the remote machine.
        rcs_input_event release = RCS_INPUT_EVENT_INIT;
        release.type = RCS_INPUT_RELEASE_ALL;
        send(release);
    }
    QQuickItem::focusOutEvent(event);
}

// ---- Touch, pen and the on-screen keyboard ----------------------------------------

void RemoteVideo::touchEvent(QTouchEvent* touch) {
    if (!inputEnabled_ || !frameWidth_) {
        touch->ignore();
        return;
    }
    touch->accept();
    QPointF centre;
    int down = 0;
    for (const QEventPoint& point : touch->points()) {
        if (point.state() != QEventPoint::Released) {
            centre += point.position();
            ++down;
        }
    }
    centre = down ? centre / down : touchLast_;
    const bool moved = QLineF(centre, touchStart_).length() > touchSlop;
    switch (touch->type()) {
    case QEvent::TouchBegin:
        gesture_ = Gesture::Pending;
        fingers_ = down;
        touchStart_ = touchLast_ = centre;
        scrollRest_ = {};
        if (down == 1) {
            longPress_.start();
        }
        return;
    case QEvent::TouchUpdate:
        if (down >= 2 && fingers_ < 2) { // A second finger: scrolling or a right click, not a drag.
            longPress_.stop();
            if (gesture_ == Gesture::Dragging || gesture_ == Gesture::Holding) {
                click(RCS_BUTTON_LEFT, false, gesture_ == Gesture::Dragging ? std::optional(touchLast_) : std::nullopt);
            }
            fingers_ = down;
            gesture_ = Gesture::Pending;
            touchStart_ = touchLast_ = centre;
            return;
        }
        if (fingers_ >= 2) {
            if (down >= 2 && (gesture_ == Gesture::Scrolling || (gesture_ == Gesture::Pending && moved))) {
                gesture_ = Gesture::Scrolling;
                scroll(centre - touchLast_, centre);
                touchLast_ = centre;
            }
            return;
        }
        if (gesture_ == Gesture::Pending && moved) {
            longPress_.stop();
            if (touchMode_ == Direct) {
                click(RCS_BUTTON_LEFT, true, touchStart_);
                gesture_ = Gesture::Dragging;
            } else {
                gesture_ = Gesture::Moving;
            }
        }
        if (gesture_ == Gesture::Dragging) {
            rcs_input_event event = RCS_INPUT_EVENT_INIT;
            event.type = RCS_INPUT_MOVE;
            if (centre != touchLast_ && position(centre, event, true)) {
                send(event);
            }
        } else if (gesture_ == Gesture::Moving || gesture_ == Gesture::Holding) {
            moveTrackpad(centre - touchLast_);
        }
        touchLast_ = centre;
        return;
    case QEvent::TouchEnd:
        longPress_.stop();
        if (fingers_ >= 2) {
            if (gesture_ == Gesture::Pending) { // A two-finger tap.
                click(RCS_BUTTON_RIGHT, true, touchMode_ == Direct ? std::optional(touchStart_) : std::nullopt);
                click(RCS_BUTTON_RIGHT, false, touchMode_ == Direct ? std::optional(touchStart_) : std::nullopt);
            }
        } else if (gesture_ == Gesture::Pending) { // A tap.
            const auto at = touchMode_ == Direct ? std::optional(touchStart_) : std::nullopt;
            click(RCS_BUTTON_LEFT, true, at);
            click(RCS_BUTTON_LEFT, false, at);
        } else if (gesture_ == Gesture::Dragging) {
            click(RCS_BUTTON_LEFT, false, touchLast_);
        } else if (gesture_ == Gesture::Holding) {
            click(RCS_BUTTON_LEFT, false);
        }
        gesture_ = Gesture::None;
        return;
    case QEvent::TouchCancel: // The system took the touches: nothing stays pressed.
        longPress_.stop();
        if (gesture_ == Gesture::Dragging || gesture_ == Gesture::Holding) {
            click(RCS_BUTTON_LEFT, false, gesture_ == Gesture::Dragging ? std::optional(touchLast_) : std::nullopt);
        }
        gesture_ = Gesture::None;
        return;
    default:
        return;
    }
}

void RemoteVideo::longPressed() {
    if (gesture_ != Gesture::Pending || fingers_ != 1) {
        return;
    }
    if (touchMode_ == Direct) { // A right click where the finger is.
        click(RCS_BUTTON_RIGHT, true, touchStart_);
        click(RCS_BUTTON_RIGHT, false, touchStart_);
        gesture_ = Gesture::Done;
    } else { // The button stays down while the finger moves: a drag.
        click(RCS_BUTTON_LEFT, true);
        gesture_ = Gesture::Holding;
    }
}

void RemoteVideo::click(uint32_t button, bool down, std::optional<QPointF> at) {
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_MOVE;
    if (at) {
        if (!position(*at, event, true)) {
            return;
        }
    } else {
        moveTrackpad({}); // Where the remote pointer is, the first time.
        event.x = trackX_;
        event.y = trackY_;
    }
    if (down) { // The pointer gets there first, as a mouse's would.
        send(event);
    }
    event.type = RCS_INPUT_BUTTON;
    event.code = button;
    event.down = down;
    send(event);
}

void RemoteVideo::scroll(QPointF delta, QPointF at) {
    // A finger moving down shows what is above: scrolling up, which the SDK counts positive.
    scrollRest_ += QPointF(-delta.x(), delta.y()) * scrollPerPoint;
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_SCROLL;
    event.wheel_x = int32_t(scrollRest_.x());
    event.wheel_y = int32_t(scrollRest_.y());
    if (!event.wheel_x && !event.wheel_y) {
        return;
    }
    scrollRest_ -= QPointF(event.wheel_x, event.wheel_y);
    if (touchMode_ == Direct) {
        if (!position(at, event, true)) {
            return;
        }
    } else {
        moveTrackpad({});
        event.x = trackX_;
        event.y = trackY_;
    }
    send(event);
}

void RemoteVideo::moveTrackpad(QPointF delta) {
    if (!trackKnown_) { // From where the remote pointer is, or the middle.
        const auto cursor = session_ ? session_->cursor() : RemoteSession::Cursor{};
        trackX_ = cursor.visible ? cursor.x : 32768;
        trackY_ = cursor.visible ? cursor.y : 32768;
        trackKnown_ = true;
    }
    if (delta.isNull() || !frameWidth_) {
        return;
    }
    const qreal ratio = window()->effectiveDevicePixelRatio();
    const auto g = geometryFor(frameWidth_, frameHeight_);
    const double unitsX = 65535.0 * trackpadSpeed * ratio / std::max(1, g.width);
    const double unitsY = 65535.0 * trackpadSpeed * ratio / std::max(1, g.height);
    trackX_ = uint32_t(std::clamp(trackX_ + delta.x() * unitsX, 0.0, 65535.0));
    trackY_ = uint32_t(std::clamp(trackY_ + delta.y() * unitsY, 0.0, 65535.0));
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_MOVE;
    event.x = trackX_;
    event.y = trackY_;
    send(event);
}

void RemoteVideo::itemChange(ItemChange change, const ItemChangeData& value) {
    if (change == ItemSceneChange && value.window) {
        value.window->installEventFilter(this);
        // Before the window hides or goes, its render thread takes no more
        // frames from the session's thread.
        connect(value.window, &QQuickWindow::sceneGraphInvalidated, this, [this] {
            if (session_) {
                session_->setRenderWindow(nullptr);
            }
        }, Qt::DirectConnection);
    }
    QQuickItem::itemChange(change, value);
}

bool RemoteVideo::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        return pen(static_cast<QTabletEvent*>(event));
    default:
        return QQuickItem::eventFilter(watched, event);
    }
}

// Apple Pencil over the picture: a pen where the other end takes one (its
// pressure and tilt reach drawing programs); otherwise Qt makes it a mouse.
bool RemoteVideo::pen(QTabletEvent* tablet) {
    const QPointF point = mapFromScene(tablet->scenePosition());
    if (!inputEnabled_ || !isVisible() || !contains(point) || !session_ ||
        !(session_->peerInputTypes() & (1u << RCS_INPUT_PEN))) {
        return false;
    }
    tablet->accept(); // No mouse made of it.
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_PEN;
    if (!position(point, event, true)) {
        return true;
    }
    const bool touching = tablet->type() != QEvent::TabletRelease && (tablet->buttons() & Qt::LeftButton);
    event.down = touching;
    event.pressure = touching ? uint32_t(std::clamp(tablet->pressure() * 1024, 1.0, 1024.0)) : 0;
    event.tilt_x = int32_t(std::clamp(tablet->xTilt(), -90.0, 90.0));
    event.tilt_y = int32_t(std::clamp(tablet->yTilt(), -90.0, 90.0));
    event.rotation = uint32_t((int(tablet->rotation()) % 360 + 360) % 360);
    if (tablet->pointerType() == QPointingDevice::PointerType::Eraser) {
        event.pen |= RCS_PEN_ERASER;
    }
    send(event);
    if (tablet->type() == QEvent::TabletRelease) { // Lifted: out of range for the other end too.
        event.pen |= RCS_PEN_LEAVE;
        send(event);
    }
    return true;
}

void RemoteVideo::tapKey(uint32_t usage) {
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = RCS_INPUT_KEY;
    event.code = usage;
    event.down = 1;
    send(event);
    event.down = 0;
    send(event);
}

// What the on-screen keyboard commits: typed as text on the remote machine,
// line breaks and tabs as their keys.
void RemoteVideo::inputMethodEvent(QInputMethodEvent* method) {
    for (const char32_t code : method->commitString().toUcs4()) {
        if (code == U'\n' || code == U'\r') {
            tapKey(40);
        } else if (code == U'\t') {
            tapKey(43);
        } else if (code >= 0x20) {
            rcs_input_event event = RCS_INPUT_EVENT_INIT;
            event.type = RCS_INPUT_TEXT;
            event.code = code;
            send(event);
        }
    }
    if (method->preeditString() != composing_) {
        composing_ = method->preeditString();
        emit softKeyboardChanged();
    }
    method->accept();
}

QVariant RemoteVideo::inputMethodQuery(Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImEnabled:
        return softKeyboard_;
    case Qt::ImHints: // Each key as typed: no corrections of text the remote machine already has.
        return int(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
    case Qt::ImCursorRectangle: // At the top: iOS moves the window when the keyboard would cover it.
        return QRectF(width() / 2, 0, 1, 1);
    case Qt::ImSurroundingText:
        return QString();
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
        return 0;
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

// While controlling, application shortcuts belong to the remote machine.
bool RemoteVideo::event(QEvent* event) {
    if (inputEnabled_ && event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}
