// LAN remote-control demo: finds other instances on the network, shares this
// screen with a pairing code, and views and controls another machine.
// RemoteVideo draws decoded frames on the GPU inside the Qt Quick UI. On
// Windows the application creates the D3D11 device, hands it to Qt Quick and
// to the SDK, so decoded textures are used where they are made.
#include "demo_app.hpp"

#include <rcsdk/clipboard.h>
#include <rcsdk/power.h>

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSurfaceFormat>

#include <cstdio>

#ifdef Q_OS_ANDROID
#include <rcsdk/android.h>
#include <sys/system_properties.h>

#include <QJniEnvironment>
#include <QJniObject>
#endif

#ifdef _WIN32
#include <QQuickGraphicsDevice>
#include <d3d10_1.h> // ID3D10Multithread; before d3d10.h, as newer Windows SDKs require
#include <d3d11.h>
#include <wrl/client.h>
#endif

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
#ifdef Q_OS_ANDROID
    // The SDK reaches Java (the microphone permission, the clipboard, wake
    // locks) through the application's VM; LAN discovery needs Wi-Fi's multicast.
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    rcs_android_initialize(QJniEnvironment::javaVM(), context.object());
    QJniObject::callStaticMethod<void>("local/rcsdk/qmldemo/Platform", "holdMulticastLock",
                                       "(Landroid/content/Context;)V", context.object());
#endif
    rcsdk::registerModule(*rcs_clipboard_module()); // The clipboard the demo's sessions use...
    rcsdk::registerModule(*rcs_power_module());     // ...and keeping the machine awake.
    QGuiApplication::setApplicationName(QStringLiteral("RemoteControlDemo"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("远程控制 Demo"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

#ifdef _WIN32
    // Video support lets the SDK decode on this device; multithread protection
    // lets its decoder thread share it with the render thread.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) {
        qFatal("D3D11 device creation failed");
    }
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    context.As(&multithread);
    multithread->SetMultithreadProtected(TRUE);
    RemoteSession::setHostDevice(device.Get());
#endif

    // Scripted tests: --accept [--code C] hosts; --connect IP --code C views;
    // --port P is the port hosted on or connected to; --tcp uses TCP instead of
    // UDP; --video-codec / --audio-codec pick the host's preferred format (e.g.
    // av1, aac); --resolution WxH, --fps N and --bitrate MBPS set what the host
    // sends; --allow-control lets the viewer control this machine; --log
    // prints statistics; --vsync presents at the display's refresh (Qt's
    // default) instead of handing each frame to the system's compositor at once.
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption accept(QStringLiteral("accept"), QStringLiteral("Accept connections at once"));
    const QCommandLineOption code(QStringLiteral("code"), QStringLiteral("Pairing code"), QStringLiteral("code"));
    const QCommandLineOption connectTo(QStringLiteral("connect"), QStringLiteral("Connect to this host"),
                                       QStringLiteral("ip"));
    const QCommandLineOption port(QStringLiteral("port"), QStringLiteral("Port to host on or connect to"),
                                  QStringLiteral("port"));
    const QCommandLineOption videoCodec(QStringLiteral("video-codec"), QStringLiteral("Preferred video codec"),
                                        QStringLiteral("name"));
    const QCommandLineOption audioCodec(QStringLiteral("audio-codec"), QStringLiteral("Preferred audio codec"),
                                        QStringLiteral("name"));
    const QCommandLineOption resolution(QStringLiteral("resolution"), QStringLiteral("What the host sends, e.g. 3840x2160"),
                                        QStringLiteral("size"));
    const QCommandLineOption fps(QStringLiteral("fps"), QStringLiteral("The host's frame rate"), QStringLiteral("n"));
    const QCommandLineOption bitrate(QStringLiteral("bitrate"), QStringLiteral("The host's bitrate cap, Mbit/s"),
                                     QStringLiteral("mbps"));
    const QCommandLineOption tcp(QStringLiteral("tcp"), QStringLiteral("TCP instead of UDP"));
    const QCommandLineOption allowControl(QStringLiteral("allow-control"), QStringLiteral("Let viewers control"));
    const QCommandLineOption log(QStringLiteral("log"), QStringLiteral("Print statistics every second"));
    const QCommandLineOption vsync(QStringLiteral("vsync"), QStringLiteral("Present at the refresh, not at once"));
    parser.addOptions(
        {accept, code, connectTo, port, videoCodec, audioCodec, resolution, fps, bitrate, tcp, allowControl, log, vsync});
    QStringList arguments = app.arguments();
#ifdef Q_OS_ANDROID
    // Android starts an application without arguments: scripts set them in a
    // property first (adb shell setprop debug.rcsdk.demo.args "--connect ...").
    char property[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.rcsdk.demo.args", property) > 0) {
        arguments += QString::fromUtf8(property).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }
#endif
    parser.process(arguments);
    // Video paces the drawing: each frame goes to the system's compositor as
    // soon as it is drawn (it syncs to the display itself; windows do not
    // tear). Waiting for the refresh queues frames when they come at the
    // display's rate: at 120 FPS on a 120 Hz Mac, 51-55 ms glass to glass
    // instead of 27-28 (docs/PERFORMANCE.md). Before the window exists.
    if (!parser.isSet(vsync)) {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(format);
    }

    DemoApp demo;
    const int hostPort = parser.isSet(port) ? parser.value(port).toInt() : DemoApp::defaultHostPort;
    demo.setHostPort(hostPort);
    if (parser.isSet(code) && parser.isSet(accept)) {
        demo.setPairingCode(parser.value(code));
    }
    demo.setTcp(parser.isSet(tcp));
    // By the start of the label ("av1" matches "AV1", "h.264" matches "H.264").
    const auto pick = [&](const QCommandLineOption& option, const QStringList& labels, auto set) {
        if (parser.isSet(option)) {
            for (int i = 0; i < labels.size(); ++i) {
                if (labels[i].startsWith(parser.value(option), Qt::CaseInsensitive)) {
                    (demo.*set)(i);
                }
            }
        }
    };
    pick(videoCodec, demo.videoCodecs(), &DemoApp::setVideoCodec);
    if (parser.isSet(resolution)) { // "3840x2160" picks "3840 × 2160".
        const auto size = parser.value(resolution).split(QLatin1Char('x'));
        const auto labels = demo.resolutions();
        for (int i = 0; size.size() == 2 && i < labels.size(); ++i) {
            if (labels[i] == size[0] + QStringLiteral(" × ") + size[1]) {
                demo.setResolution(i);
            }
        }
    }
    if (parser.isSet(fps)) {
        demo.setFramesPerSecond(parser.value(fps).toInt());
    }
    if (parser.isSet(bitrate)) {
        demo.setBitrateMbps(parser.value(bitrate).toInt());
    }
    pick(audioCodec, demo.audioCodecs(), &DemoApp::setAudioCodec);
    if (parser.isSet(allowControl)) {
        demo.setAllowControl(true);
    }
    if (parser.isSet(accept)) {
        demo.setAccepting(true);
    }
    if (parser.isSet(connectTo)) {
        demo.connectTo(parser.value(connectTo), hostPort, parser.value(connectTo), parser.value(code),
                       parser.isSet(tcp));
    }
    if (parser.isSet(log)) { // The SDK's log, and one line per second from each running session.
        rcsdk::setLogCallback(RCS_LOG_DEBUG, [](const rcs_log_record& record) {
            // The host and the viewer session each have their own handle.
            static const char* sources[] = {"process", "session", "agent", "supervisor"};
            const void* handle = record.session ? static_cast<const void*>(record.session)
                                                : static_cast<const void*>(record.agent);
            std::printf("sdk %s %p %s: %s\n", sources[record.source], handle, record.module, record.message);
            std::fflush(stdout);
        });
        for (RemoteSession* session : {demo.host(), demo.viewer()}) {
            QObject::connect(session, &RemoteSession::statisticsChanged, &demo, [session, &demo] {
                std::printf("%s: %d FPS, %.1f Mbit/s, RTT %.1f ms, %s, %s\n",
                            session == demo.host() ? "host" : "viewer", session->framesPerSecondNow(),
                            session->bitrateMbpsNow(), session->roundTripMs(), qPrintable(session->stateText()),
                            qPrintable(session->statistics()));
                std::fflush(stdout);
            });
        }
    }
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("app"), QVariant::fromValue(&demo)}});
    engine.loadFromModule("RemoteControlDemo", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    demo.setWindow(window);
#ifdef _WIN32
    window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndContext(device.Get(), context.Get()));
#endif
    window->show();
    return app.exec();
}
