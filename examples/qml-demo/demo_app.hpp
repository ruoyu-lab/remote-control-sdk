// The LAN demo's application logic: this machine as a host others connect to
// (listening while "accepting", restarting after each viewer), connecting to
// a discovered host, and pairing. The viewer chooses the stream it wants
// (applied at once while viewing); the host's own settings apply at once too,
// and it follows the viewer's choice unless told not to. A host shows a 6-digit code; both ends
// derive the session key from it (SHA-256), so a wrong code fails the SDK's
// handshake. That is demo-grade pairing for a trusted LAN, not a product's.
// On iOS the host shares this application's own picture (appSharing): its
// viewers see and click the demo itself, and hear its microphone; or the whole
// screen, other apps too, through the demo's broadcast extension, which then
// takes the host's place with the same pairing code (viewers only watch). On
// Android the host shares the whole screen through MediaProjection (the
// system asks the user each time) and what the device plays; viewers control
// it through the demo's accessibility service.
#pragma once

#include "app_input.hpp"
#include "lan_discovery.hpp"
#include "remote_session.hpp"
#ifdef Q_OS_ANDROID
#include "screen_projection.hpp"
#endif

#include <QObject>
#include <QSize>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <memory>

class AppCapture;
class QWindow;
class ScreenBroadcast;
class ScreenProjection;

#ifdef Q_OS_IOS
#include "display_rate.hpp"
struct BroadcastSettings;
#endif

class DemoApp : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")

    Q_PROPERTY(DeviceList* devices READ devices CONSTANT)
    Q_PROPERTY(RemoteSession* host READ host CONSTANT)
    Q_PROPERTY(RemoteSession* viewer READ viewer CONSTANT)
    Q_PROPERTY(QString deviceName READ deviceName CONSTANT)
    Q_PROPERTY(QString platform READ platform CONSTANT)
    Q_PROPERTY(QString addresses READ addresses NOTIFY addressesChanged)
    Q_PROPERTY(QString sdkVersion READ sdkVersion CONSTANT)
    // iOS: the host shares this application's picture, not the screen, and sends the microphone.
    Q_PROPERTY(bool appSharing READ appSharing CONSTANT)

    // Host side.
    Q_PROPERTY(bool accepting READ accepting WRITE setAccepting NOTIFY hostChanged)
    Q_PROPERTY(QString pairingCode READ pairingCode NOTIFY hostChanged)
    Q_PROPERTY(QString hostStatus READ hostStatus NOTIFY hostChanged)
    Q_PROPERTY(bool hostConnected READ hostConnected NOTIFY hostChanged)
    Q_PROPERTY(bool allowControl READ allowControl WRITE setAllowControl NOTIFY hostChanged)
    Q_PROPERTY(int resolution READ resolution WRITE setResolution NOTIFY qualityChanged) // Index of resolutions().
    Q_PROPERTY(int framesPerSecond READ framesPerSecond WRITE setFramesPerSecond NOTIFY qualityChanged)
    Q_PROPERTY(int bitrateMbps READ bitrateMbps WRITE setBitrateMbps NOTIFY qualityChanged)
    Q_PROPERTY(int videoCodec READ videoCodec WRITE setVideoCodec NOTIFY qualityChanged) // Index of videoCodecs().
    Q_PROPERTY(int audioCodec READ audioCodec WRITE setAudioCodec NOTIFY qualityChanged) // Index of audioCodecs().
    Q_PROPERTY(bool tcp READ tcp WRITE setTcp NOTIFY qualityChanged) // Else UDP with adaptive bitrate.
    Q_PROPERTY(bool followViewer READ followViewer WRITE setFollowViewer NOTIFY qualityChanged)
    Q_PROPERTY(bool shareSound READ shareSound WRITE setShareSound NOTIFY qualityChanged)
    // Copies go both ways between the two machines' clipboards while connected (rcsdk.clipboard).
    Q_PROPERTY(bool syncClipboard READ syncClipboard WRITE setSyncClipboard NOTIFY qualityChanged)
    // Play what the viewers' microphones send.
    Q_PROPERTY(bool hearViewers READ hearViewers WRITE setHearViewers NOTIFY qualityChanged)
    // iOS: the broadcast extension shares the whole screen now. Android: MediaProjection does.
    Q_PROPERTY(bool sharingScreen READ sharingScreen NOTIFY hostChanged)
    Q_PROPERTY(bool screenPermission READ screenPermission NOTIFY permissionsChanged)
    Q_PROPERTY(bool inputPermission READ inputPermission NOTIFY permissionsChanged)

    // Viewer side.
    Q_PROPERTY(bool viewing READ viewing NOTIFY viewerChanged)
    Q_PROPERTY(QString peerName READ peerName NOTIFY viewerChanged)
    Q_PROPERTY(QString viewerError READ viewerError NOTIFY viewerChanged)
    Q_PROPERTY(bool receiveSound READ receiveSound WRITE setReceiveSound NOTIFY viewerChanged)
    // Send this machine's microphone to the host, its echo cancelled.
    Q_PROPERTY(bool sendMicrophone READ sendMicrophone WRITE setSendMicrophone NOTIFY viewerChanged)
    // The stream asked for: an index of viewResolutions(); 0 frame rate or bitrate: the host's choice.
    Q_PROPERTY(int viewResolution READ viewResolution WRITE setViewResolution NOTIFY viewQualityChanged)
    Q_PROPERTY(int viewFramesPerSecond READ viewFramesPerSecond WRITE setViewFramesPerSecond NOTIFY viewQualityChanged)
    Q_PROPERTY(int viewBitrateMbps READ viewBitrateMbps WRITE setViewBitrateMbps NOTIFY viewQualityChanged)
    // An index of viewAudioCodecs(); 0: the host's choice.
    Q_PROPERTY(int viewAudioCodec READ viewAudioCodec WRITE setViewAudioCodec NOTIFY viewQualityChanged)

public:
    explicit DemoApp(QObject* parent = nullptr);
    ~DemoApp() override;

    Q_INVOKABLE QStringList resolutions() const;
    // What this machine's hardware encodes, and the sound formats it encodes.
    Q_INVOKABLE QStringList videoCodecs() const;
    Q_INVOKABLE QStringList audioCodecs() const;
    Q_INVOKABLE QStringList viewResolutions() const;
    Q_INVOKABLE QStringList viewAudioCodecs() const;
    Q_INVOKABLE void newPairingCode();
    Q_INVOKABLE void requestPermissions();
    // iOS: hands the host's settings to the broadcast extension and opens the
    // system's picker (where the user starts or stops the broadcast).
    // Android: asks the user again to share the screen.
    Q_INVOKABLE void shareWholeScreen();
    // Connects to a host by address with the code it shows, over the transport it uses.
    Q_INVOKABLE void connectTo(const QString& address, int port, const QString& name, const QString& code,
                               bool tcp);
    Q_INVOKABLE void disconnectViewer();
    // For scripted tests: host with a fixed code instead of a random one, on another port.
    void setPairingCode(const QString& code);
    void setHostPort(int port);
    // The window a viewer's input reaches when this application is what is shared.
    void setWindow(QWindow* window) { input_.setWindow(window); }

    DeviceList* devices() { return discovery_.devices(); }
    RemoteSession* host() { return &host_; }
    RemoteSession* viewer() { return &viewer_; }
    QString deviceName() const;
    QString platform() const;
    QString addresses() const { return LanDiscovery::localAddresses().join(QStringLiteral(" · ")); }
    QString sdkVersion() const;
    bool appSharing() const;

    bool accepting() const { return accepting_; }
    void setAccepting(bool accepting);
    QString pairingCode() const { return accepting_ ? code_ : QString(); }
    QString hostStatus() const;
    bool hostConnected() const { return host_.state() == RCS_STATE_RUNNING; }
    bool allowControl() const { return host_.allowControl(); }
    void setAllowControl(bool allowed);
    int resolution() const { return resolution_; }
    void setResolution(int index);
    int framesPerSecond() const { return framesPerSecond_; }
    void setFramesPerSecond(int fps);
    int bitrateMbps() const { return bitrateMbps_; }
    void setBitrateMbps(int mbps);
    int videoCodec() const { return videoCodec_; }
    void setVideoCodec(int index);
    int audioCodec() const { return audioCodec_; }
    void setAudioCodec(int index);
    bool tcp() const { return tcp_; }
    void setTcp(bool tcp);
    bool followViewer() const { return followViewer_; }
    void setFollowViewer(bool follow);
    bool shareSound() const { return shareSound_; }
    void setShareSound(bool share);
    bool syncClipboard() const { return host_.clipboard(); }
    void setSyncClipboard(bool sync);
    bool hearViewers() const { return hearViewers_; }
    bool sharingScreen() const { return screenShared_; }
    void setHearViewers(bool hear);
    bool screenPermission() const;
    bool inputPermission() const;

    bool viewing() const { return viewing_; }
    QString peerName() const { return peerName_; }
    QString viewerError() const { return viewerError_; }
    bool receiveSound() const { return receiveSound_; }
    void setReceiveSound(bool receive);
    bool sendMicrophone() const { return sendMicrophone_; }
    void setSendMicrophone(bool send);
    int viewResolution() const { return viewResolution_; }
    void setViewResolution(int index);
    int viewFramesPerSecond() const { return viewFramesPerSecond_; }
    void setViewFramesPerSecond(int fps);
    int viewBitrateMbps() const { return viewBitrateMbps_; }
    void setViewBitrateMbps(int mbps);
    int viewAudioCodec() const { return viewAudioCodec_; }
    void setViewAudioCodec(int index);

    static constexpr int defaultHostPort = 47990;

signals:
    void addressesChanged();
    void hostChanged();
    void qualityChanged();
    void permissionsChanged();
    void viewerChanged();
    void viewQualityChanged();

private:
    static QString keyFor(const QString& code);
    // The host pushes pictures that follow the screen's shape (iOS, Android).
    static bool pushesPictures();
    QSize hostSize() const;
    void hostSettings(); // The host's properties from this class's settings.
    void startHost();
    void hostStateChanged();
    void viewerStateChanged();
    void announce();
    void qualityEdited();   // Applies at once, while connected too.
    void transportEdited(); // Applies from the next viewer.
    void audioEdited();     // Applies at once.
    void requestViewQuality();

    LanDiscovery discovery_;
    RemoteSession host_, viewer_;
#ifdef Q_OS_IOS
    DisplayRate displayRate_; // The display's highest while viewing.
    std::unique_ptr<AppCapture> capture_; // The picture while a viewer is connected.
    std::unique_ptr<ScreenBroadcast> broadcast_;
    QTimer broadcastWait_; // The user has a while to start the broadcast.
    void screenBroadcastChanged();
    BroadcastSettings broadcastSettings() const; // What the extension shares the screen with.
#endif
#ifdef Q_OS_ANDROID
    std::unique_ptr<ScreenProjection> projection_;
    ScreenInput screenInput_;
    void startProjection();
#endif
    bool broadcastWaiting_ = false, screenShared_ = false;
    AppInput input_;
    QTimer restart_;

    bool accepting_ = false;
    int hostPort_ = defaultHostPort;
    QString code_;
    QString hostError_;
    int resolution_ = 1, framesPerSecond_ = 60, bitrateMbps_ = 20;
    int videoCodec_ = 0, audioCodec_ = 0; // H.264 and Opus where available: listed first.
    bool tcp_ = false, shareSound_ = true, followViewer_ = true, hearViewers_ = false;

    bool viewing_ = false, receiveSound_ = true, sendMicrophone_ = false;
    int viewResolution_ = 0, viewFramesPerSecond_ = 0, viewBitrateMbps_ = 0, viewAudioCodec_ = 0;
    int requestedNativeWidth_ = 0; // Native resolution asked for once the host's display is known.
    QString peerName_, viewerError_;
};
