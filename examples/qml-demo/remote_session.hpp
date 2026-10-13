// A remote-control session for QML: configuration, lifecycle, statistics and
// the latest GPU frame. Frames arrive on an SDK thread and wait in a one-slot
// mailbox that RemoteVideo reads on the render thread. A viewer asks for a
// stream (requestStream) and may take the pointer apart from the picture
// (cursorSeparate); either end applies its settings while running (apply).
// A sharer may share pictures of the application's own (appContent: pushed
// with pushGpuFrame or, on Android, the screen MediaProjection draws, pushed
// with pushHardwareBuffer; its viewers' input handed to onHostInput) instead
// of the screen it captures. Either end may send its microphone, a sharer
// the sound it pushes (hostSound), and its machine stays awake while
// connected (rcsdk.power).
#pragma once

#include <rcsdk/rcsdk.hpp>

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class QQuickWindow;

class RemoteSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")

    Q_PROPERTY(bool sharing MEMBER sharing_ NOTIFY settingsChanged)
    Q_PROPERTY(QString address MEMBER address_ NOTIFY settingsChanged)
    Q_PROPERTY(int port MEMBER port_ NOTIFY settingsChanged)
    Q_PROPERTY(QString key MEMBER key_ NOTIFY settingsChanged)
    Q_PROPERTY(int width MEMBER width_ NOTIFY settingsChanged)
    Q_PROPERTY(int height MEMBER height_ NOTIFY settingsChanged)
    Q_PROPERTY(int framesPerSecond MEMBER framesPerSecond_ NOTIFY settingsChanged)
    Q_PROPERTY(int bitrateMbps MEMBER bitrateMbps_ NOTIFY settingsChanged)
    Q_PROPERTY(QString codecs MEMBER codecs_ NOTIFY settingsChanged) // e.g. "h264", "hevc" or "hevc,h264"
    Q_PROPERTY(QString audioCodecs MEMBER audioCodecs_ NOTIFY settingsChanged) // e.g. "opus,aac,pcm"
    Q_PROPERTY(bool software MEMBER software_ NOTIFY settingsChanged)
    Q_PROPERTY(bool udp MEMBER udp_ NOTIFY settingsChanged)     // UDP with adaptive bitrate, else TCP.
    Q_PROPERTY(bool audio MEMBER audio_ NOTIFY settingsChanged) // Share or receive sound.
    // Send this end's microphone, its echo cancelled (a sharer: instead of what the machine plays).
    Q_PROPERTY(bool microphone MEMBER microphone_ NOTIFY settingsChanged)
    // Share: send the sound pushed with pushAudio (Android: what the device plays).
    Q_PROPERTY(bool hostSound MEMBER hostSound_ NOTIFY settingsChanged)
    // Share: play the viewers' sound (their microphones), leaving this application's out of what it sends.
    Q_PROPERTY(bool playViewers MEMBER playViewers_ NOTIFY settingsChanged)
    // Share: the application's own pictures (RCS_CAPTURE_HOST) and input (RCS_INPUT_HOST).
    Q_PROPERTY(bool appContent MEMBER appContent_ NOTIFY settingsChanged)
    Q_PROPERTY(bool allowControl READ allowControl WRITE setAllowControl NOTIFY settingsChanged)
    // Copies go both ways between the two machines' clipboards (rcsdk.clipboard), at once while running.
    Q_PROPERTY(bool clipboard READ clipboard WRITE setClipboard NOTIFY settingsChanged)
    Q_PROPERTY(bool followRequests MEMBER followRequests_ NOTIFY settingsChanged) // Share: apply the viewer's requests.
    Q_PROPERTY(bool cursorSeparate MEMBER cursorSeparate_ NOTIFY settingsChanged) // View: RCS_CURSOR_SEPARATE.

    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(bool reconnecting READ reconnecting NOTIFY stateChanged) // View: the path broke; trying again.
    Q_PROPERTY(bool peerPaused READ peerPaused NOTIFY stateChanged)     // View: the sharer paused its picture.
    Q_PROPERTY(QString stateText READ stateText NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(bool controlGranted READ controlGranted NOTIFY statisticsChanged)
    Q_PROPERTY(QString statistics READ statistics NOTIFY statisticsChanged)
    // One value each, refreshed every second while running.
    Q_PROPERTY(QString codecText READ codecText NOTIFY statisticsChanged)
    Q_PROPERTY(QString resolutionText READ resolutionText NOTIFY statisticsChanged)
    Q_PROPERTY(int framesPerSecondNow READ framesPerSecondNow NOTIFY statisticsChanged)
    Q_PROPERTY(double roundTripMs READ roundTripMs NOTIFY statisticsChanged)
    Q_PROPERTY(double bitrateMbpsNow READ bitrateMbpsNow NOTIFY statisticsChanged)
    Q_PROPERTY(bool audioActive READ audioActive NOTIFY statisticsChanged)
    Q_PROPERTY(int audioBufferMs READ audioBufferMs NOTIFY statisticsChanged)
    Q_PROPERTY(QString audioCodecText READ audioCodecText NOTIFY statisticsChanged)
    Q_PROPERTY(QString pathText READ pathText NOTIFY statisticsChanged)
    Q_PROPERTY(int displayWidth READ displayWidth NOTIFY statisticsChanged) // The sharer's display, in pixels.
    Q_PROPERTY(int displayHeight READ displayHeight NOTIFY statisticsChanged)

public:
    explicit RemoteSession(QObject* parent = nullptr);
    ~RemoteSession() override;

    // Windows: the ID3D11Device Qt Quick renders with; frames are decoded on it.
    static void setHostDevice(void* device) { hostDevice_ = device; }

    Q_INVOKABLE bool start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE QString generateKey();
    // View: the stream to ask for (0: the sharer's choice; audioCodec "" too), at start or at once while running.
    Q_INVOKABLE void requestStream(int width, int height, int framesPerSecond, int bitrateMbps,
                                   const QString& audioCodec = {});
    // Every setting at once while running (rcs_session_update with all the
    // extensions; what did not change stays as it is).
    Q_INVOKABLE void apply();

    // View with cursorSeparate: the sharer's pointer, and the images it has shown.
    struct Cursor {
        bool visible = false;
        uint32_t x = 0, y = 0; // Hotspot, 0..65535 across the shared display.
        uint32_t shapeId = 0;
    };
    struct CursorShape {
        QImage image; // In the shared display's pixels.
        QPoint hotspot;
        rcs_cursor_type type = RCS_CURSOR_CUSTOM;
    };
    Cursor cursor() const { return cursor_; }
    const CursorShape* cursorShape(uint32_t id) const;

    bool allowControl() const { return allowControl_; }
    void setAllowControl(bool allowed);
    bool clipboard() const { return clipboard_; }
    void setClipboard(bool on);
    bool running() const;
    bool reconnecting() const { return state_ == RCS_STATE_RECONNECTING; }
    bool peerPaused() const { return peerPaused_; }
    QString stateText() const;
    QString error() const { return error_; }
    // The other end's system, once connected (RCS_PLATFORM_UNKNOWN before).
    rcs_platform peerPlatform() const { return session_ ? session_->peerInfo().platform : RCS_PLATFORM_UNKNOWN; }
    // The input the other end takes, as (1u << rcs_input_type) bits (0 before it is connected).
    uint32_t peerInputTypes() const { return session_ ? session_->peerInfo().input_types : 0; }
    rcs_result failureResult() const { return failureResult_; } // Why the run failed (FAILED).
    bool controlGranted() const { return controlGranted_; }
    QString statistics() const { return statistics_; }
    QString codecText() const { return codecText_; }
    QString resolutionText() const { return resolutionText_; }
    int framesPerSecondNow() const { return framesPerSecondNow_; }
    double roundTripMs() const { return roundTripMs_; }
    double bitrateMbpsNow() const { return bitrateMbpsNow_; }
    bool audioActive() const { return audioActive_; }
    int audioBufferMs() const { return audioBufferMs_; }
    QString audioCodecText() const { return audioCodecText_; }
    QString pathText() const { return pathText_; }
    int displayWidth() const { return displayWidth_; }
    int displayHeight() const { return displayHeight_; }
    rcs_state state() const { return state_; }

    // Render thread: the newest frame, or an empty one.
    rcsdk::GpuFrame latestFrame() const;
    // The window whose render thread draws this session's frames
    // (RemoteVideo, while it has a picture; null: none). A new frame of the
    // shape drawn before goes straight to that thread to be drawn again, no
    // GUI thread or synchronization in between (QQuickWindow::update from
    // the render thread); other frames, as the first, through frameReady.
    void setRenderWindow(QQuickWindow* window);
    rcs_stream_info streamInfo() const; // Any thread.
    void sendInput(const rcs_input_event& event);
    // Share with appContent, any thread: the next picture (see rcs_session_push_gpu_frame,
    // rcs_session_push_hardware_buffer).
    rcs_result pushGpuFrame(const rcs_gpu_frame_info& frame);
    rcs_result pushHardwareBuffer(const void* buffer);
    // Share with hostSound, any thread: 48 kHz stereo float32 (see rcs_session_push_audio).
    rcs_result pushAudio(const float* samples, uint32_t frames);
    // Share with appContent: each input event of a viewer, with the stream it
    // points into, on the session's thread (post to the GUI thread to use the
    // GUI: Qt for Android holds the GUI thread's events while the application
    // is in the background, where input on the whole device must go on).
    std::function<void(const rcs_input_event&, const rcs_stream_info&)> onHostInput;

signals:
    void settingsChanged();
    void stateChanged();
    void statisticsChanged();
    void frameReady(); // Queued to the GUI thread; at most one pending.
    void cursorChanged();

private:
    std::optional<rcsdk::SessionConfig> settings(); // Sets error_ when a property is invalid.
    void refresh();

    static inline void* hostDevice_ = nullptr;

    bool sharing_ = false;
    QString address_ = QStringLiteral("127.0.0.1");
    int port_ = 47990;
    QString key_;
    int width_ = 1920, height_ = 1080, framesPerSecond_ = 60, bitrateMbps_ = 20;
    QString codecs_ = QStringLiteral("h264");
    bool followRequests_ = true, cursorSeparate_ = false;
    int requestWidth_ = 0, requestHeight_ = 0, requestFramesPerSecond_ = 0, requestBitrateMbps_ = 0;
    rcs_audio_codec requestAudioCodec_ = 0;
    Cursor cursor_;
    QHash<uint32_t, CursorShape> cursorShapes_;
    QList<uint32_t> cursorOrder_; // The shapes held, oldest given first: the last RCS_MAX_CURSOR_SHAPES.
    int displayWidth_ = 0, displayHeight_ = 0;
    QString audioCodecs_ = QStringLiteral("opus,aac,pcm");
    bool software_ = false;
    bool udp_ = false;
    bool audio_ = true;
    bool microphone_ = false, playViewers_ = false, appContent_ = false, hostSound_ = false;
    bool allowControl_ = false;
    bool clipboard_ = true;

    mutable std::mutex pushMutex_; // Pushes and streamInfo on other threads against session_ changing.
    std::mutex streamMutex_;
    rcs_stream_info stream_ = RCS_STREAM_INFO_INIT; // As the last stream event said, for onHostInput.
    std::unique_ptr<rcsdk::Session> session_;
    rcs_state state_ = RCS_STATE_IDLE;
    bool peerPaused_ = false;
    QString error_;
    rcs_result failureResult_ = RCS_OK;
    bool controlGranted_ = false;
    QString statistics_;
    QString codecText_, resolutionText_, pathText_;
    int framesPerSecondNow_ = 0, audioBufferMs_ = 0;
    QString audioCodecText_;
    double roundTripMs_ = 0, bitrateMbpsNow_ = 0;
    bool audioActive_ = false;
    uint64_t lastFrames_ = 0, lastBytes_ = 0;
    QTimer timer_;

    mutable std::mutex mailboxMutex_;
    rcsdk::GpuFrame mailbox_;
    std::atomic<bool> frameQueued_{false};
    std::mutex renderMutex_; // The callback's use of renderWindow_ against its change.
    QQuickWindow* renderWindow_ = nullptr;
    rcs_gpu_frame_info shape_{}; // The last frame's, under mailboxMutex_.
};
