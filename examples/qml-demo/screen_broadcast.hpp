// iOS: sharing the whole screen through the demo's broadcast upload
// extension (broadcast/SampleHandler.mm), which runs a sharing session of its
// own while other apps are in front. The app leaves it the settings, opens the
// system's broadcast picker for it (the user starts the broadcast there), and
// hears whether the screen is being captured.
#pragma once

#include "broadcast/broadcast_settings.hpp"

#include <QObject>

class ScreenBroadcast : public QObject {
    Q_OBJECT

public:
    explicit ScreenBroadcast(QObject* parent = nullptr);
    ~ScreenBroadcast() override;

    // Leaves `settings` for the extension and shows the picker; false when
    // they could not be left.
    bool start(const BroadcastSettings& settings);
    // The system's picker alone: it also stops a broadcast that runs.
    void showPicker();
    // The screen is being captured: the broadcast (or the system's recording or mirroring).
    bool running() const;

signals:
    void runningChanged();

private:
    void* observer_ = nullptr; // UIScreenCapturedDidChangeNotification's.
};
