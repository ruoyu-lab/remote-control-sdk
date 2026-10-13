// QML element showing a RemoteSession's frames. Decoded NV12 frames are drawn
// by a render node straight into Qt Quick's render target: no CPU copy, no
// intermediate texture. With inputEnabled it forwards mouse and keyboard
// input to the remote machine, touch as gestures (touchMode), an Apple
// Pencil as a pen where the other end takes one, and with softKeyboard what
// the on-screen keyboard types. With the session's cursor apart from the
// picture, the local pointer takes the remote pointer's shape while input is
// enabled (it then follows the mouse at once); otherwise, and where there is
// no local pointer (iOS), an overlay draws the remote pointer where it is.
#pragma once

#include "qt_input.hpp"

#include "remote_session.hpp"

#include <rcsdk/view_geometry.hpp>

#include <QElapsedTimer>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <mutex>
#include <optional>

class QQuickPaintedItem;
class QTabletEvent;

class RemoteVideo : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(RemoteSession* session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(FillMode fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(bool smooth READ smoothScaling WRITE setSmoothScaling NOTIFY fillModeChanged)
    Q_PROPERTY(bool inputEnabled READ inputEnabled WRITE setInputEnabled NOTIFY inputEnabledChanged)
    Q_PROPERTY(TouchMode touchMode READ touchMode WRITE setTouchMode NOTIFY touchModeChanged)
    // The system's on-screen keyboard, typing on the remote machine.
    Q_PROPERTY(bool softKeyboard READ softKeyboard WRITE setSoftKeyboard NOTIFY softKeyboardChanged)
    // What an input method is composing, before it types it (e.g. pinyin).
    Q_PROPERTY(QString composing READ composing NOTIFY softKeyboardChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged)
    Q_PROPERTY(QString frameError READ frameError NOTIFY hasFrameChanged)
    Q_PROPERTY(double renderedFps READ renderedFps NOTIFY renderedFpsChanged)

public:
    enum FillMode { Fit, NativePixels };
    Q_ENUM(FillMode)
    // Direct: the pointer goes where the finger is; a tap clicks, a long press
    // right-clicks, a drag drags. Trackpad: a finger moves the pointer as on a
    // trackpad; a tap clicks where it is, a long press holds the button for a
    // drag. In both, two fingers scroll and a two-finger tap right-clicks.
    enum TouchMode { Direct, Trackpad };
    Q_ENUM(TouchMode)

    explicit RemoteVideo(QQuickItem* parent = nullptr);
    ~RemoteVideo() override;

    RemoteSession* session() const { return session_; }
    void setSession(RemoteSession* session);
    FillMode fillMode() const { return fillMode_; }
    void setFillMode(FillMode mode);
    bool smoothScaling() const { return smooth_; }
    void setSmoothScaling(bool smooth);
    bool inputEnabled() const { return inputEnabled_; }
    // Disabling sends a release-all so no remote key stays pressed.
    void setInputEnabled(bool enabled);
    TouchMode touchMode() const { return touchMode_; }
    void setTouchMode(TouchMode mode);
    bool softKeyboard() const { return softKeyboard_; }
    QString composing() const { return composing_; }
    void setSoftKeyboard(bool shown);
    bool hasFrame() const { return hasFrame_; }
    QString frameError() const { return frameError_; }
    double renderedFps() const { return renderedFps_; }

    // Render thread: the last frame import's error, empty on success.
    void reportImport(const QString& error);
    // Render thread: a new frame went to the screen (renderedFps).
    void framePresented() { ++presented_; }

signals:
    void sessionChanged();
    void fillModeChanged();
    void inputEnabledChanged();
    void touchModeChanged();
    void softKeyboardChanged();
    void hasFrameChanged();
    void renderedFpsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void touchEvent(QTouchEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    bool event(QEvent* event) override;
    // The window's tablet events: Qt Quick gives items none.
    bool eventFilter(QObject* watched, QEvent* event) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    rcsdk::ViewGeometry geometryFor(uint32_t frameWidth, uint32_t frameHeight) const;
    bool position(QPointF point, rcs_input_event& event, bool clamp) const;
    void send(const rcs_input_event& event);
    void button(QMouseEvent* mouse, bool down);
    void key(QKeyEvent* key, bool down);
    void updateCursor();
    // Touch gestures: a button at a point of the item or, in trackpad mode, where the pointer is.
    void click(uint32_t button, bool down, std::optional<QPointF> at = {});
    void longPressed();
    void scroll(QPointF delta, QPointF at);
    void moveTrackpad(QPointF delta);
    void tapKey(uint32_t usage);
    bool pen(QTabletEvent* tablet);

    QPointer<RemoteSession> session_;
    demo::WheelLines wheelX_, wheelY_;
    FillMode fillMode_ = Fit;
    bool smooth_ = true;
    bool inputEnabled_ = false;
    TouchMode touchMode_ = Direct;
    bool softKeyboard_ = false;
    QString composing_;
    int panX_ = 0, panY_ = 0;
    QPointF lastHover_; // Where the last hover move was sent from.

    // The touch gesture in progress: waiting to see what it is, a drag holding
    // the left button (direct), moving the pointer (trackpad), the button held
    // after a long press (trackpad), done until the fingers lift, scrolling.
    enum class Gesture { None, Pending, Dragging, Moving, Holding, Done, Scrolling };
    Gesture gesture_ = Gesture::None;
    int fingers_ = 0;                  // The most fingers down during it.
    QPointF touchStart_, touchLast_;   // Item points: where it started, the last centre.
    QTimer longPress_;
    QPointF scrollRest_;               // Scroll steps not sent yet.
    uint32_t trackX_ = 32768, trackY_ = 32768; // The trackpad's pointer, 0..65535 across the display.
    bool trackKnown_ = false;

    // Written during synchronization, read on the GUI thread.
    uint32_t frameWidth_ = 0, frameHeight_ = 0;
    bool hasFrame_ = false;
    QString frameError_; // Copied from importError_ during synchronization.
    std::mutex importMutex_;
    QString importError_;

    QQuickPaintedItem* overlay_ = nullptr; // The remote pointer while input is off.
    uint32_t pointerShape_ = 0;            // Shape and scale of the local pointer set last.
    double pointerScale_ = 0;

    std::atomic<int> presented_{0};
    double renderedFps_ = 0;
    QTimer fpsTimer_;
    QElapsedTimer fpsClock_;
};
