// iOS: this application's own picture, shared. ReplayKit's in-app capture
// (the system asks the user first) hands over the screen as pixel buffers;
// they are turned upright and scaled to the stream's size on the GPU
// (letterboxed where the shapes differ) and pushed to the sharing session
// without a copy to the CPU (rcs_session_push_gpu_frame).
#pragma once

#include <QObject>
#include <QString>

#include <memory>

class RemoteSession;

class AppCapture : public QObject {
    Q_OBJECT

public:
    explicit AppCapture(RemoteSession& session, QObject* parent = nullptr);
    ~AppCapture() override;

    void start();
    void stop();

signals:
    void failed(const QString& reason); // The system refused, or the user did not allow it.

private:
    struct Pipeline;
    std::shared_ptr<Pipeline> pipeline_; // Shared with ReplayKit's handler, which may outlive a stop.
};
