#include "demo_app.hpp"

#ifdef Q_OS_IOS
#include "app_capture.hpp"
#include "codec_list.hpp"
#include "screen_broadcast.hpp"
#endif

#include <rcsdk/rcsdk.hpp>

#include <QCryptographicHash>
#include <QGuiApplication>
#include <QPermissions>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif
#include <QRandomGenerator>
#include <QScreen>
#include <QSysInfo>

#include <algorithm>
#include <functional>

namespace {

struct Size {
    int width, height;
    const char* label;
};
constexpr Size sizes[] = {{1280, 720, "1280 × 720"},
                          {1920, 1080, "1920 × 1080"},
                          {2560, 1440, "2560 × 1440"},
                          {3840, 2160, "3840 × 2160"}};

struct Format {
    uint32_t value;
    const char* name;
    const char* label;
};
constexpr Format videoFormats[] = {{RCS_CODEC_H264, "h264", "H.264"}, {RCS_CODEC_HEVC, "hevc", "HEVC"},
                                   {RCS_CODEC_AV1, "av1", "AV1"}};
constexpr Format audioFormats[] = {{RCS_AUDIO_OPUS, "opus", "Opus"}, {RCS_AUDIO_AAC, "aac", "AAC"},
                                   {RCS_AUDIO_PCM, "pcm", "PCM（无压缩）"}};

// The formats `mask` has, in table order.
template <size_t N>
std::vector<const Format*> available(const Format (&formats)[N], uint32_t mask) {
    std::vector<const Format*> result;
    for (const Format& f : formats) {
        if (mask & (1u << f.value)) {
            result.push_back(&f);
        }
    }
    return result;
}

// The demo encodes in hardware; on a machine with no hardware encoder at all (the iOS simulator), in software.
bool softwareEncoding() {
    return rcsdk::capabilities().hardware_encoders == 0;
}

std::vector<const Format*> encodableVideo() {
    const auto caps = rcsdk::capabilities();
    return available(videoFormats, softwareEncoding() ? caps.software_encoders : caps.hardware_encoders);
}

std::vector<const Format*> encodableAudio() {
    return available(audioFormats, rcsdk::capabilities().audio_encoders);
}

// The system asks the first time; the session's sound says why it is silent
// until the user allowed it. `then` runs once the user answered. (Android:
// an activity asks, which the SDK has none of; Qt's is the application's.)
void requestMicrophone(std::function<void()> then = {}) {
#ifdef Q_OS_ANDROID
    const QMicrophonePermission microphone;
    if (qApp->checkPermission(microphone) != Qt::PermissionStatus::Granted) {
        qApp->requestPermission(microphone, [then](const QPermission&) {
            if (then) {
                then();
            }
        });
        return;
    }
#else
    if (rcs_microphone_permission() == RCS_PERMISSION_REQUIRED) {
        rcs_request_microphone_permission();
    }
#endif
    if (then) {
        then();
    }
}

// "chosen,others..." for the session's codec list.
QString preferring(const std::vector<const Format*>& formats, int chosen) {
    QStringList names;
    if (chosen >= 0 && chosen < int(formats.size())) {
        names << QString::fromLatin1(formats[size_t(chosen)]->name);
    }
    for (int i = 0; i < int(formats.size()); ++i) {
        if (i != chosen) {
            names << QString::fromLatin1(formats[size_t(i)]->name);
        }
    }
    return names.join(u',');
}

} // namespace

DemoApp::DemoApp(QObject* parent) : QObject(parent) {
    newPairingCode();
    host_.setProperty("sharing", true);
    host_.setProperty("address", QStringLiteral("0.0.0.0"));
    // The viewer offers everything; the SDK keeps what this machine decodes and the host's choice wins.
    viewer_.setProperty("codecs", QStringLiteral("h264,hevc,av1"));
    viewer_.setProperty("audioCodecs", QStringLiteral("opus,aac,pcm"));
    viewer_.setProperty("cursorSeparate", true); // The local pointer follows the mouse at once.
    connect(&viewer_, &RemoteSession::statisticsChanged, this, [this] {
        if (viewResolution_ == 1 && viewer_.displayWidth() && viewer_.displayWidth() != requestedNativeWidth_) {
            requestViewQuality(); // Native: the host's display is known now.
        }
    });
    connect(&host_, &RemoteSession::stateChanged, this, &DemoApp::hostStateChanged);
#ifdef Q_OS_ANDROID
    // On the session's thread: the accessibility service acts while the demo is in the background too.
    host_.onHostInput = [this](const rcs_input_event& event, const rcs_stream_info& stream) {
        screenInput_.execute(event, stream);
    };
    projection_ = std::make_unique<ScreenProjection>(host_);
    connect(projection_.get(), &ScreenProjection::runningChanged, this, [this] {
        screenShared_ = projection_->running();
        emit hostChanged();
    });
    connect(projection_.get(), &ScreenProjection::ended, this, [this](const QString& reason) {
        hostError_ = reason;
        emit hostChanged();
    });
    // Back from Settings: the accessibility service may be on now.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, &DemoApp::permissionsChanged);
#else
    host_.onHostInput = [this](const rcs_input_event& event, const rcs_stream_info&) {
        QMetaObject::invokeMethod(this, [this, event] { input_.execute(event); }, Qt::QueuedConnection);
    };
#endif
    if (QScreen* screen = QGuiApplication::primaryScreen(); screen && pushesPictures()) {
        connect(screen, &QScreen::geometryChanged, this, &DemoApp::qualityEdited); // Turned: another shape.
    }
    // Played without echo where this machine's own sound can be left out of
    // what it shares (Android: the demo's own is not recorded).
    hearViewers_ = pushesPictures() || rcsdk::capabilities().audio_exclusion != 0;
#ifdef Q_OS_IOS
    connect(this, &DemoApp::viewerChanged, this, [this] { displayRate_.setHighest(viewing_); });
    capture_ = std::make_unique<AppCapture>(host_);
    connect(capture_.get(), &AppCapture::failed, this, [this](const QString& reason) {
        hostError_ = QStringLiteral("没能录制本 App 的画面：") + reason;
        emit hostChanged();
    });
    broadcast_ = std::make_unique<ScreenBroadcast>();
    connect(broadcast_.get(), &ScreenBroadcast::runningChanged, this, &DemoApp::screenBroadcastChanged);
    broadcastWait_.setSingleShot(true);
    broadcastWait_.setInterval(60'000);
    connect(&broadcastWait_, &QTimer::timeout, this, [this] { // Not started: the app shares again.
        broadcastWaiting_ = false;
        if (accepting_) {
            startHost();
        }
        emit hostChanged();
    });
#endif
    connect(&viewer_, &RemoteSession::stateChanged, this, &DemoApp::viewerStateChanged);
    // After a failed run, start again (a viewer leaving is not one: the SDK
    // listens for the next viewer by itself).
    restart_.setSingleShot(true);
    restart_.setInterval(300);
    connect(&restart_, &QTimer::timeout, this, [this] {
        if (accepting_) {
            startHost();
        }
    });
    announce();
}

DemoApp::~DemoApp() {
    accepting_ = false;
#ifdef Q_OS_IOS
    capture_.reset();
    broadcast_.reset();
#endif
#ifdef Q_OS_ANDROID
    projection_.reset();
#endif
    host_.stop();
    viewer_.stop();
}

bool DemoApp::appSharing() const {
#ifdef Q_OS_IOS
    return true;
#else
    return false;
#endif
}

bool DemoApp::pushesPictures() {
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    return true;
#else
    return false;
#endif
}

QStringList DemoApp::resolutions() const {
    QStringList list;
    for (const Size& size : sizes) {
        list << QString::fromUtf8(size.label);
    }
    return list;
}

QStringList DemoApp::videoCodecs() const {
    QStringList list;
    for (const Format* f : encodableVideo()) {
        list << QString::fromUtf8(f->label);
    }
    return list;
}

QStringList DemoApp::audioCodecs() const {
    QStringList list;
    for (const Format* f : encodableAudio()) {
        list << QString::fromUtf8(f->label);
    }
    return list;
}

QStringList DemoApp::viewResolutions() const {
    return {QStringLiteral("对方的设置"), QStringLiteral("原生（对方屏幕）"), QString::fromUtf8(sizes[3].label),
            QString::fromUtf8(sizes[2].label), QString::fromUtf8(sizes[1].label), QString::fromUtf8(sizes[0].label)};
}

QStringList DemoApp::viewAudioCodecs() const {
    QStringList list{QStringLiteral("声音：对方的设置")};
    for (const Format* f : available(audioFormats, rcsdk::capabilities().audio_decoders)) {
        list << QStringLiteral("声音：") + QString::fromUtf8(f->label);
    }
    return list;
}

void DemoApp::setViewAudioCodec(int index) {
    if (index != viewAudioCodec_) {
        viewAudioCodec_ = index;
        requestViewQuality();
        emit viewQualityChanged();
    }
}

void DemoApp::requestViewQuality() {
    int width = 0, height = 0;
    if (viewResolution_ == 1) {
        width = requestedNativeWidth_ = viewer_.displayWidth();
        height = viewer_.displayHeight();
    } else if (viewResolution_ >= 2) {
        const Size& size = sizes[5 - viewResolution_];
        width = size.width;
        height = size.height;
    }
    const auto decodable = available(audioFormats, rcsdk::capabilities().audio_decoders);
    const QString audio = viewAudioCodec_ > 0 && viewAudioCodec_ <= int(decodable.size())
                              ? QString::fromLatin1(decodable[size_t(viewAudioCodec_ - 1)]->name)
                              : QString();
    viewer_.requestStream(width, height, viewFramesPerSecond_, viewBitrateMbps_, audio);
}

void DemoApp::setViewResolution(int index) {
    if (index >= 0 && index < int(viewResolutions().size()) && index != viewResolution_) {
        viewResolution_ = index;
        requestedNativeWidth_ = 0;
        requestViewQuality();
        emit viewQualityChanged();
    }
}

void DemoApp::setViewFramesPerSecond(int fps) {
    if (fps != viewFramesPerSecond_) {
        viewFramesPerSecond_ = fps;
        requestViewQuality();
        emit viewQualityChanged();
    }
}

void DemoApp::setViewBitrateMbps(int mbps) {
    if (mbps != viewBitrateMbps_) {
        viewBitrateMbps_ = mbps;
        requestViewQuality();
        emit viewQualityChanged();
    }
}

QString DemoApp::deviceName() const {
#ifdef Q_OS_ANDROID
    // The name the user gave the device (Settings, About), else its model: its host name is "localhost".
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject resolver = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
    const QString given = QJniObject::callStaticObjectMethod(
                              "android/provider/Settings$Global", "getString",
                              "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;",
                              resolver.object(), QJniObject::fromString(QStringLiteral("device_name")).object())
                              .toString();
    return given.isEmpty() ? QJniObject::getStaticObjectField<jstring>("android/os/Build", "MODEL").toString()
                           : given;
#else
    const QString name = QSysInfo::machineHostName();
    return name.isEmpty() ? QSysInfo::prettyProductName() : name;
#endif
}

QString DemoApp::platform() const {
    return QSysInfo::prettyProductName();
}

QString DemoApp::sdkVersion() const {
    return QString::fromUtf8(rcs_version());
}

QString DemoApp::keyFor(const QString& code) {
    return QString::fromLatin1(
        QCryptographicHash::hash(("rcsdk-demo-pairing:" + code.trimmed()).toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

void DemoApp::newPairingCode() {
    code_ = QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(1'000'000), 6, 10, QLatin1Char('0'));
    if (accepting_ && !hostConnected()) {
        startHost(); // The new code takes effect for the next viewer.
    }
    emit hostChanged();
}

void DemoApp::setPairingCode(const QString& code) {
    code_ = code;
    emit hostChanged();
}

void DemoApp::setHostPort(int port) {
    hostPort_ = port;
    announce();
}

void DemoApp::announce() {
    discovery_.setAnnouncement(deviceName(), hostPort_, accepting_, tcp_);
}

// ---- Host -----------------------------------------------------------------------

void DemoApp::setAccepting(bool accepting) {
    if (accepting == accepting_) {
        return;
    }
    accepting_ = accepting;
    hostError_.clear();
    if (accepting_) {
        if (!screenPermission()) {
            requestPermissions();
        }
        startHost();
#ifdef Q_OS_ANDROID
        startProjection();
#endif
    } else {
        restart_.stop();
        host_.stop();
#ifdef Q_OS_ANDROID
        projection_->stop();
#endif
    }
    announce();
    emit hostChanged();
}

// The chosen resolution; pushing pictures of this application or of the
// whole screen (iOS, Android), the screen's shape as it is turned now, its
// longer side at most the chosen width.
QSize DemoApp::hostSize() const {
    const Size& size = sizes[resolution_];
    const QScreen* screen = QGuiApplication::primaryScreen();
    if (!pushesPictures() || !screen) {
        return {size.width, size.height};
    }
    const QSizeF pixels = QSizeF(screen->size()) * screen->devicePixelRatio();
    const double scale = std::min(1.0, size.width / std::max(pixels.width(), pixels.height()));
    return {int(pixels.width() * scale) & ~1, int(pixels.height() * scale) & ~1};
}

void DemoApp::hostSettings() {
    const QSize size = hostSize();
    host_.setProperty("width", size.width());
    host_.setProperty("height", size.height());
    host_.setProperty("framesPerSecond", framesPerSecond_);
    host_.setProperty("bitrateMbps", bitrateMbps_);
    if (const QString codecs = preferring(encodableVideo(), videoCodec_); !codecs.isEmpty()) {
        host_.setProperty("codecs", codecs);
    }
    host_.setProperty("audioCodecs", preferring(encodableAudio(), audioCodec_));
    host_.setProperty("followRequests", followViewer_);
    host_.setProperty("software", softwareEncoding());
    host_.setProperty("appContent", pushesPictures());
    host_.setProperty("audio", shareSound_);
    host_.setProperty("microphone", appSharing() && shareSound_); // iOS shares no other application's sound.
#ifdef Q_OS_ANDROID
    host_.setProperty("hostSound", shareSound_); // What the device plays, through MediaProjection.
#endif
    host_.setProperty("playViewers", hearViewers_);
    if (appSharing() && shareSound_) {
        requestMicrophone();
    }
}

void DemoApp::startHost() {
    hostSettings();
    host_.setProperty("udp", !tcp_);
    host_.setProperty("port", hostPort_);
    host_.setProperty("key", keyFor(code_));
    hostError_.clear();
    if (!host_.start()) {
        hostError_ = host_.error();
    }
    emit hostChanged();
}

void DemoApp::hostStateChanged() {
    const rcs_state state = host_.state();
#ifdef Q_OS_IOS
    if (state == RCS_STATE_RUNNING) { // Recording only while someone watches.
        capture_->start();
    } else {
        capture_->stop();
    }
#endif
    if (accepting_ && state == RCS_STATE_FAILED) { // STOPPED only follows this class's own stop().
        // Listen again, unless this machine cannot share at all.
        hostError_ = host_.error();
        if (host_.failureResult() != RCS_ERROR_PERMISSION_REQUIRED && !restart_.isActive()) {
            restart_.start();
        }
    }
    emit hostChanged();
    emit permissionsChanged();
}

QString DemoApp::hostStatus() const {
#ifdef Q_OS_IOS
    if (screenShared_) {
        return QStringLiteral("正在共享整个屏幕：对方用这个配对码连接，只能看");
    }
#endif
    if (broadcastWaiting_) {
        return QStringLiteral("请在系统的窗口里点“开始直播”");
    }
    if (!accepting_) {
        return QStringLiteral("未开放远程连接");
    }
    if (!hostError_.isEmpty()) {
        return QStringLiteral("出错：") + hostError_;
    }
#ifdef Q_OS_ANDROID
    if (!screenShared_) {
        return QStringLiteral("请在系统的窗口里允许共享屏幕");
    }
#endif
    switch (host_.state()) {
    case RCS_STATE_LISTENING:
        return QStringLiteral("等待连接，对方需要输入配对码");
    case RCS_STATE_RUNNING:
        return QStringLiteral("已被远程连接");
    default:
        return QStringLiteral("正在准备…");
    }
}

void DemoApp::shareWholeScreen() {
#ifdef Q_OS_ANDROID
    if (accepting_ && !screenShared_) {
        hostError_.clear();
        startProjection();
        emit hostChanged();
    }
#endif
#ifdef Q_OS_IOS
    if (screenShared_ || broadcastWaiting_) { // The picker also stops it.
        broadcast_->showPicker();
        return;
    }
    if (!accepting_) {
        return;
    }
    host_.stop(); // The extension listens on the port instead.
    if (!broadcast_->start(broadcastSettings())) {
        startHost();
        hostError_ = QStringLiteral("没能把设置交给共享整个屏幕的扩展（钥匙串）");
        emit hostChanged();
        return;
    }
    broadcastWaiting_ = true;
    broadcastWait_.start();
    emit hostChanged();
#endif
}

#ifdef Q_OS_IOS
BroadcastSettings DemoApp::broadcastSettings() const {
    BroadcastSettings settings;
    settings.port = uint16_t(hostPort_);
    settings.key = keyFor(code_).toStdString();
    settings.tcp = tcp_;
    settings.longSide = uint32_t(sizes[resolution_].width);
    settings.framesPerSecond = uint32_t(framesPerSecond_);
    settings.bitrateMbps = uint32_t(bitrateMbps_);
    settings.codecs.clear();
    for (const QString& name : preferring(encodableVideo(), videoCodec_).split(u',', Qt::SkipEmptyParts)) {
        settings.codecs.push_back(demo::videoCodec(name));
    }
    settings.audioCodecs.clear();
    for (const QString& name : preferring(encodableAudio(), audioCodec_).split(u',', Qt::SkipEmptyParts)) {
        settings.audioCodecs.push_back(demo::audioCodec(name));
    }
    settings.sound = shareSound_;
    return settings;
}
#endif

#ifdef Q_OS_ANDROID
// With sound, the user is asked to let the demo record first: the device's
// sound comes through the microphone's permission.
void DemoApp::startProjection() {
    const auto start = [this] {
        if (accepting_) {
            projection_->start(sizes[resolution_].width, shareSound_);
        }
    };
    if (shareSound_) {
        requestMicrophone(start);
    } else {
        start();
    }
}
#endif

#ifdef Q_OS_IOS
void DemoApp::screenBroadcastChanged() {
    const bool running = broadcast_->running();
    if (running && broadcastWaiting_) {
        broadcastWaiting_ = false;
        broadcastWait_.stop();
        screenShared_ = true;
    } else if (!running && screenShared_) { // Stopped: the app shares itself again.
        screenShared_ = false;
        if (accepting_) {
            startHost();
        }
    }
    emit hostChanged();
}
#endif

void DemoApp::setAllowControl(bool allowed) {
    if (allowed && !inputPermission()) {
        requestPermissions();
    }
    host_.setAllowControl(allowed);
    emit hostChanged();
}

void DemoApp::qualityEdited() {
    emit qualityChanged();
    if (!accepting_) {
        return;
    }
#ifdef Q_OS_IOS
    if (screenShared_ || broadcastWaiting_) { // The extension follows at once.
        saveBroadcastSettings(broadcastSettings());
        return;
    }
#endif
    hostSettings();
    host_.apply(); // Listening or connected: the stream changes without reconnecting.
}

void DemoApp::transportEdited() {
    emit qualityChanged();
    if (accepting_ && !hostConnected()) {
        startHost(); // The next viewer connects with it.
    }
}

void DemoApp::setFollowViewer(bool follow) {
    if (follow != followViewer_) {
        followViewer_ = follow;
        qualityEdited();
    }
}

void DemoApp::setResolution(int index) {
    if (index >= 0 && index < int(std::size(sizes)) && index != resolution_) {
        resolution_ = index;
        qualityEdited();
    }
}

void DemoApp::setFramesPerSecond(int fps) {
    if (fps != framesPerSecond_) {
        framesPerSecond_ = fps;
        qualityEdited();
    }
}

void DemoApp::setBitrateMbps(int mbps) {
    if (mbps != bitrateMbps_) {
        bitrateMbps_ = mbps;
        qualityEdited();
    }
}

void DemoApp::setVideoCodec(int index) {
    if (index != videoCodec_) {
        videoCodec_ = index;
        qualityEdited();
    }
}

void DemoApp::setAudioCodec(int index) {
    if (index != audioCodec_) {
        audioCodec_ = index;
        audioEdited();
    }
}

void DemoApp::audioEdited() {
    emit qualityChanged();
#ifdef Q_OS_IOS
    if (screenShared_ || broadcastWaiting_) { // The extension follows at once.
        saveBroadcastSettings(broadcastSettings());
        return;
    }
#endif
    hostSettings();
    host_.apply(); // Listening or connected: at once.
}

void DemoApp::setTcp(bool tcp) {
    if (tcp != tcp_) {
        tcp_ = tcp;
        announce(); // Viewers connect with what the host announces.
        transportEdited();
    }
}

void DemoApp::setShareSound(bool share) {
    if (share != shareSound_) {
        shareSound_ = share;
        audioEdited();
    }
}

// ---- The clipboard ---------------------------------------------------------------

void DemoApp::setSyncClipboard(bool sync) {
    host_.setClipboard(sync); // Both ends' sessions: whichever is connected.
    viewer_.setClipboard(sync);
    emit qualityChanged();
}

void DemoApp::setHearViewers(bool hear) {
    if (hear != hearViewers_) {
        hearViewers_ = hear;
        audioEdited();
    }
}

// Sharing its own picture and executing its viewers' input itself, the
// application needs neither. Android asks for the screen each time; its
// viewers' input needs the demo's accessibility service on.
bool DemoApp::screenPermission() const {
    return pushesPictures() || rcs_screen_capture_permission() != RCS_PERMISSION_REQUIRED;
}

bool DemoApp::inputPermission() const {
#ifdef Q_OS_ANDROID
    return ScreenInput::enabled();
#else
    return appSharing() || rcs_input_permission() != RCS_PERMISSION_REQUIRED;
#endif
}

void DemoApp::requestPermissions() {
    if (!screenPermission()) {
        rcs_request_screen_capture_permission();
    }
    if (host_.allowControl() && !inputPermission()) {
#ifdef Q_OS_ANDROID
        ScreenInput::openSettings();
#else
        rcs_request_input_permission();
#endif
    }
    emit permissionsChanged();
}

// ---- Viewer -----------------------------------------------------------------------

void DemoApp::connectTo(const QString& address, int port, const QString& name, const QString& code, bool tcp) {
    viewer_.stop();
    viewerError_.clear();
    peerName_ = name.isEmpty() ? address : name;
    viewer_.setProperty("address", address);
    viewer_.setProperty("port", port);
    viewer_.setProperty("key", keyFor(code));
    viewer_.setProperty("udp", !tcp);
    viewer_.setProperty("audio", receiveSound_);
    viewer_.setProperty("microphone", sendMicrophone_);
    requestedNativeWidth_ = 0;
    requestViewQuality(); // Native resolution follows once the host's display is known.
    viewing_ = viewer_.start();
    if (!viewing_) {
        viewerError_ = viewer_.error();
    }
    emit viewerChanged();
}

void DemoApp::disconnectViewer() {
    viewing_ = false;
    viewer_.stop();
    emit viewerChanged();
}

void DemoApp::setReceiveSound(bool receive) {
    receiveSound_ = receive;
    viewer_.setProperty("audio", receive);
    viewer_.apply(); // While viewing: at once.
    emit viewerChanged();
}

void DemoApp::setSendMicrophone(bool send) {
    sendMicrophone_ = send;
    if (send) {
        requestMicrophone();
    }
    viewer_.setProperty("microphone", send);
    viewer_.apply(); // While viewing: at once.
    emit viewerChanged();
}

void DemoApp::viewerStateChanged() {
    if (viewing_ && viewer_.state() == RCS_STATE_FAILED) {
        const rcs_result result = viewer_.failureResult();
        viewerError_ = result == RCS_ERROR_AUTHENTICATION ? QStringLiteral("配对码不正确，或对方已换了新的配对码")
                       : result == RCS_ERROR_DISCONNECTED ? QStringLiteral("对方已断开连接")
                       : result == RCS_ERROR_NETWORK && viewer_.error().contains(QStringLiteral("reconnect"))
                           ? QStringLiteral("连接断开后没能重新连上：") + viewer_.error()
                           : QStringLiteral("连接失败：") + viewer_.error();
        viewing_ = false;
    }
    emit viewerChanged();
}
