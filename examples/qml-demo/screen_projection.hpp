// Android: sharing the whole screen through MediaProjection, in the demo's
// Java (android/src/local/rcsdk/qmldemo): the user allows it in the system's
// dialog; then each picture goes to the host session as a hardware buffer
// (rcs_session_push_hardware_buffer, which fits it into the stream) and, with
// sound, what the device plays as the host's sound. A viewer's input goes the
// other way, through the demo's accessibility service (ScreenInput).
#pragma once

#include "remote_session.hpp"

#include <rcsdk/rcsdk.h>

#include <QObject>
#include <QString>

class ScreenProjection : public QObject {
    Q_OBJECT

public:
    explicit ScreenProjection(RemoteSession& session, QObject* parent = nullptr);
    ~ScreenProjection() override;

    // Asks the user, then shares the screen, its longer side at most `longSide` pixels.
    void start(int longSide, bool sound);
    void stop();
    bool running() const { return running_; }

signals:
    void runningChanged();
    // Sharing ended without stop(): the user said no, or stopped it in the status bar.
    void ended(const QString& reason);

private:
    friend struct ProjectionNatives;
    RemoteSession& session_;
    bool running_ = false;
};

// A viewer's input on this device, through the demo's accessibility service
// (which the user turns on in Settings → Accessibility).
class ScreenInput {
public:
    static bool enabled();
    static void openSettings();
    // `event` across the stream `stream`, whose content is the screen; on
    // the session's thread (the service acts on Android's main thread).
    void execute(const rcs_input_event& event, const rcs_stream_info& stream);

private:
    bool pressed_ = false, shift_ = false;
    uint32_t contact_ = 0; // The touch contact pressing.
    float lastU_ = 0.5f, lastV_ = 0.5f;
};
