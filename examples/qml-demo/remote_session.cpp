#include "remote_session.hpp"

#include "codec_list.hpp"

#include <rcsdk/android.hpp>
#include <rcsdk/clipboard.h>
#include <rcsdk/gpu.hpp>
#include <rcsdk/power.h>

#include <QDebug>
#include <QMetaObject>
#include <QQuickWindow>
#include <QRunnable>
#include <algorithm>
#include <QStringList>

#include <cstring>
#include <optional>

namespace {

QString stateName(rcs_state state) {
    switch (state) {
    case RCS_STATE_STARTING:
        return QStringLiteral("正在启动");
    case RCS_STATE_LISTENING:
        return QStringLiteral("等待对端连接");
    case RCS_STATE_CONNECTING:
        return QStringLiteral("正在连接");
    case RCS_STATE_RUNNING:
        return QStringLiteral("运行中");
    case RCS_STATE_RECONNECTING:
        return QStringLiteral("正在重新连接");
    case RCS_STATE_STOPPED:
        return QStringLiteral("已停止");
    case RCS_STATE_FAILED:
        return QStringLiteral("失败");
    default:
        return QStringLiteral("未启动");
    }
}

QString codecName(rcs_codec codec) {
    switch (codec) {
    case RCS_CODEC_H264:
        return QStringLiteral("H.264");
    case RCS_CODEC_HEVC:
        return QStringLiteral("HEVC");
    case RCS_CODEC_AV1:
        return QStringLiteral("AV1");
    default:
        return QStringLiteral("—");
    }
}

} // namespace

RemoteSession::RemoteSession(QObject* parent) : QObject(parent) {
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &RemoteSession::refresh);
    timer_.start();
}

RemoteSession::~RemoteSession() {
    session_.reset(); // Joins the SDK threads before members go away.
}

// Every setting of this end, from the properties: for start(), and while
// running for apply(), which hands the SDK all of them at once.
std::optional<rcsdk::SessionConfig> RemoteSession::settings() {
    rcsdk::SessionConfig config(sharing_ ? RCS_ROLE_SHARE : RCS_ROLE_VIEW);
    const QByteArray address = address_.trimmed().toLatin1();
    const QByteArray key = QByteArray::fromHex(key_.trimmed().toLatin1());
    if (address.size() >= int(sizeof(rcs_connect::address)) || key.size() != 32) {
        error_ = QStringLiteral("请填写对端地址和两端一致的 64 位十六进制密钥");
        return std::nullopt;
    }
    config.setAddress(address.toStdString(), uint32_t(port_));
    std::memcpy(config.core().identity.key, key.constData(), 32);
    config.setTransport(udp_ ? RCS_TRANSPORT_UDP : RCS_TRANSPORT_TCP)
        // A viewer decodes in software what its hardware cannot (the iOS simulator has no hardware).
        .setAcceleration(software_  ? RCS_ACCELERATION_SOFTWARE
                         : sharing_ ? RCS_ACCELERATION_HARDWARE
                                    : RCS_ACCELERATION_HARDWARE_FIRST)
        .setAudioEnabled(audio_);
    if (microphone_) {
        config.setMicrophone(); // The default input, voice processing on.
    } else if (sharing_ && hostSound_) {
        config.setAudioSource(RCS_AUDIO_HOST);
    }
    if (sharing_) {
        config.setVideo(uint32_t(width_), uint32_t(height_), uint32_t(framesPerSecond_),
                        uint32_t(bitrateMbps_) * 1'000'000)
            .setRequestPolicy(followRequests_)
            .setControl(allowControl_, appContent_ ? RCS_INPUT_HOST : RCS_INPUT_SYSTEM);
        if (appContent_) {
            config.setCaptureSource(RCS_CAPTURE_HOST);
        }
        if (playViewers_) { // Not sent back to them in what this machine plays.
            config.setReceiveAudio(true).setExcludeOwnSound(!microphone_);
        }
    } else {
        config.setCursorMode(cursorSeparate_ ? RCS_CURSOR_SEPARATE : RCS_CURSOR_IN_VIDEO)
            .setVideoRequest(uint32_t(requestWidth_), uint32_t(requestHeight_), uint32_t(requestFramesPerSecond_),
                             uint32_t(requestBitrateMbps_) * 1'000'000)
            .setAudioRequest(requestAudioCodec_);
        if (hostDevice_) {
            config.use(rcsdk::gpuDevice(hostDevice_));
        }
    }
    // Always used, so that turning it on and off while running is a change of its settings.
    rcs_clipboard_settings clipboard = RCS_CLIPBOARD_SETTINGS_INIT;
    clipboard.flow = clipboard_ ? RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE : 0;
    config.useModule(RCS_CLIPBOARD_MODULE, clipboard);
    // Awake while connected; a viewer's display stays on too, and iOS keeps
    // an application running only while its display is on.
    rcs_power_settings power = RCS_POWER_SETTINGS_INIT;
#ifndef Q_OS_IOS
    if (sharing_) {
        power.keep_awake = RCS_KEEP_AWAKE_SYSTEM;
    } else
#endif
    {
        power.keep_awake = RCS_KEEP_AWAKE_DISPLAY;
    }
    config.useModule(RCS_POWER_MODULE, power);
    if (!demo::setCodecList(sharing_ ? config.encoder().codecs : config.decoder().codecs, codecs_)) {
        error_ = QStringLiteral("编码格式应为 h264、hevc、av1 中的一到三个，用逗号分隔");
        return std::nullopt;
    }
    if (!demo::setAudioCodecList(sharing_ ? config.sendAudio().codecs : config.receiveAudio().codecs, audioCodecs_)) {
        error_ = QStringLiteral("声音格式应为 opus、aac、pcm 中的一到三个，用逗号分隔");
        return std::nullopt;
    }
    return config;
}

bool RemoteSession::start() {
    stop();
    error_.clear();
    const auto config = settings();
    if (!config) {
        emit stateChanged();
        return false;
    }

    {
        std::lock_guard lock(streamMutex_);
        stream_ = RCS_STREAM_INFO_INIT;
    }
    rcsdk::Callbacks callbacks;
    callbacks.onEvent = [this](const rcs_event& event) {
        if (event.type == RCS_EVENT_STREAM) {
            std::lock_guard lock(streamMutex_);
            stream_ = event.stream.info;
        }
        if (event.type == RCS_EVENT_CURSOR) {
            const auto& c = event.cursor;
            std::optional<CursorShape> shape;
            if (c.shape) { // Borrowed: copied before the event returns.
                const QImage view(c.shape->pixels, int(c.shape->width), int(c.shape->height),
                                  int(c.shape->stride_bytes), QImage::Format_ARGB32); // BGRA bytes, straight alpha.
                shape = CursorShape{view.copy(), QPoint(int(c.shape->hotspot_x), int(c.shape->hotspot_y)),
                                    c.shape->type};
            }
            QMetaObject::invokeMethod(
                this,
                [this, cursor = Cursor{c.visible != 0, c.x, c.y, c.shape_id}, shape = std::move(shape)] {
                    if (shape) { // The session gives one again when it returns after it was dropped.
                        cursorShapes_.insert(cursor.shapeId, *shape);
                        cursorOrder_.append(cursor.shapeId);
                        if (cursorOrder_.size() > RCS_MAX_CURSOR_SHAPES) {
                            cursorShapes_.remove(cursorOrder_.takeFirst());
                        }
                    }
                    cursor_ = cursor;
                    emit cursorChanged();
                },
                Qt::QueuedConnection);
            return;
        }
        if (event.type == RCS_EVENT_PAUSE) {
            QMetaObject::invokeMethod(
                this,
                [this, paused = (event.pause.by_peer & RCS_PAUSE_VIDEO) != 0] {
                    peerPaused_ = paused;
                    emit stateChanged();
                },
                Qt::QueuedConnection);
            return;
        }
        if (event.type != RCS_EVENT_STATE) {
            return;
        }
        const rcs_state state = event.state.value;
        QMetaObject::invokeMethod(
            this,
            [this, state] {
                state_ = state;
                if (state == RCS_STATE_FAILED && session_) {
                    const auto failure = session_->failure();
                    failureResult_ = failure.result;
                    error_ = QString::fromStdString(failure.message);
                }
                emit stateChanged();
            },
            Qt::QueuedConnection);
    };
    if (sharing_ && appContent_) {
        callbacks.onInput = [this](const rcs_input_event& event) {
            if (onHostInput) {
                rcs_stream_info stream;
                {
                    std::lock_guard lock(streamMutex_);
                    stream = stream_;
                }
                onHostInput(event, stream);
            }
            return true;
        };
    }
    if (!sharing_) { // A sharer shows no preview, so its capture loop never waits for one.
        // GPU frames only: decoded frames stay on the GPU until the scene graph draws them.
        callbacks.onGpuFrame = [this](const rcsdk::GpuFrame& frame) {
            bool sameShape = false;
            {
                std::lock_guard lock(mailboxMutex_);
                mailbox_ = frame;
                const auto info = frame.info();
                sameShape = info.width == shape_.width && info.height == shape_.height &&
                            info.surface_width == shape_.surface_width &&
                            info.surface_height == shape_.surface_height && info.format == shape_.format;
                shape_ = info;
            }
            if (sameShape) { // Drawn again on the render thread at once.
                std::lock_guard lock(renderMutex_);
                if (QQuickWindow* window = renderWindow_) {
                    window->scheduleRenderJob(QRunnable::create([window] { window->update(); }),
                                              QQuickWindow::NoStage);
                    return;
                }
            }
            if (!frameQueued_.exchange(true)) {
                QMetaObject::invokeMethod(
                    this,
                    [this] {
                        frameQueued_ = false;
                        emit frameReady();
                    },
                    Qt::QueuedConnection);
            }
        };
    }
    try {
        auto session = std::make_unique<rcsdk::Session>(*config, std::move(callbacks));
        std::lock_guard lock(pushMutex_);
        session_ = std::move(session);
    } catch (const rcsdk::Error& failure) {
        error_ = QString::fromUtf8(failure.what());
        emit stateChanged();
        return false;
    }
    if (const auto result = session_->start(); result != RCS_OK) {
        error_ = QString::fromStdString(session_->failure().message);
        if (error_.isEmpty()) {
            error_ = QString::fromUtf8(rcs_result_message(result));
        }
        emit stateChanged();
        return false;
    }
    return true;
}

void RemoteSession::stop() {
    {
        std::lock_guard lock(pushMutex_);
        session_.reset();
    }
    cursor_ = {};
    cursorShapes_.clear(); // Shape ids belong to a run.
    cursorOrder_.clear();
    emit cursorChanged();
    lastFrames_ = lastBytes_ = 0;
    {
        std::lock_guard lock(mailboxMutex_);
        mailbox_ = {};
        shape_ = {};
    }
    state_ = RCS_STATE_STOPPED;
    peerPaused_ = false;
    controlGranted_ = false;
    emit stateChanged();
    emit frameReady(); // RemoteVideo clears its picture.
}

QString RemoteSession::generateKey() {
    const auto key = rcsdk::generateConnectionKey();
    key_ = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(key.data()), int(key.size())).toHex());
    emit settingsChanged();
    return key_;
}

void RemoteSession::requestStream(int width, int height, int framesPerSecond, int bitrateMbps,
                                  const QString& audioCodec) {
    requestWidth_ = width & ~1;
    requestHeight_ = height & ~1;
    requestFramesPerSecond_ = framesPerSecond;
    requestBitrateMbps_ = bitrateMbps;
    requestAudioCodec_ = demo::audioCodec(audioCodec);
    apply();
}

void RemoteSession::apply() {
    if (!session_) {
        return;
    }
    error_.clear();
    if (const auto config = settings(); config && session_->update(*config) != RCS_OK) {
        error_ = QString::fromStdString(rcsdk::lastCallError());
    }
    emit stateChanged();
}

const RemoteSession::CursorShape* RemoteSession::cursorShape(uint32_t id) const {
    const auto found = cursorShapes_.find(id);
    return found == cursorShapes_.end() ? nullptr : &*found;
}

void RemoteSession::setAllowControl(bool allowed) {
    allowControl_ = allowed;
    apply();
    emit settingsChanged();
}

void RemoteSession::setClipboard(bool on) {
    clipboard_ = on;
    apply();
    emit settingsChanged();
}

bool RemoteSession::running() const {
    return session_ && state_ != RCS_STATE_STOPPED && state_ != RCS_STATE_FAILED;
}

QString RemoteSession::stateText() const {
    return stateName(state_);
}

rcsdk::GpuFrame RemoteSession::latestFrame() const {
    std::lock_guard lock(mailboxMutex_);
    return mailbox_;
}

void RemoteSession::setRenderWindow(QQuickWindow* window) {
    std::lock_guard lock(renderMutex_);
    renderWindow_ = window;
}

rcs_stream_info RemoteSession::streamInfo() const {
    std::lock_guard lock(pushMutex_);
    return session_ ? session_->streamInfo() : rcs_stream_info RCS_STREAM_INFO_INIT;
}


rcs_result RemoteSession::pushGpuFrame(const rcs_gpu_frame_info& frame) {
    std::lock_guard lock(pushMutex_);
    return session_ ? rcsdk::pushGpuFrame(*session_, frame) : RCS_ERROR_INVALID_STATE;
}

rcs_result RemoteSession::pushHardwareBuffer(const void* buffer) {
    std::lock_guard lock(pushMutex_);
    return session_ ? rcsdk::pushHardwareBuffer(*session_, buffer) : RCS_ERROR_INVALID_STATE;
}

rcs_result RemoteSession::pushAudio(const float* samples, uint32_t frames) {
    std::lock_guard lock(pushMutex_);
    return session_ ? session_->pushAudio(samples, frames) : RCS_ERROR_INVALID_STATE;
}

void RemoteSession::sendInput(const rcs_input_event& event) {
    if (session_ && controlGranted_) {
        session_->sendInput(event);
    }
}

void RemoteSession::refresh() {
    if (!session_) {
        return;
    }
    const auto sent = session_->stats<rcs_video_send_stats>();
    const auto received = session_->stats<rcs_video_receive_stats>();
    const auto stream = session_->streamInfo();
    controlGranted_ = session_->controlInfo().granted != 0;
    const auto network = session_->stats<rcs_network_stats>();
    const uint64_t frames = sharing_ ? sent.encoded_frames : received.decoded_frames;
    framesPerSecondNow_ = int(frames - std::min(frames, lastFrames_));
    lastFrames_ = frames;
    bitrateMbpsNow_ = double(network.bytes - std::min(network.bytes, lastBytes_)) * 8 / 1e6;
    lastBytes_ = network.bytes;
    roundTripMs_ = double(network.round_trip_ns) / 1e6;
    codecText_ = codecName(stream.codec);
    resolutionText_ = stream.width ? QStringLiteral("%1×%2").arg(stream.width).arg(stream.height) : QString();
    pathText_ = QString::fromUtf8(network.path);
    displayWidth_ = int(stream.display_width);
    displayHeight_ = int(stream.display_height);
    audioCodecText_ = stream.audio_codec == RCS_AUDIO_OPUS  ? QStringLiteral("Opus")
                      : stream.audio_codec == RCS_AUDIO_AAC ? QStringLiteral("AAC")
                      : stream.audio_codec == RCS_AUDIO_PCM ? QStringLiteral("PCM")
                                                            : QString();
    if (sharing_) {
        audioActive_ = session_->stats<rcs_audio_send_stats>().active != 0;
        audioBufferMs_ = 0;
    } else {
        const auto audio = session_->stats<rcs_audio_receive_stats>();
        audioActive_ = audio.active != 0;
        audioBufferMs_ = int(audio.buffer_ms);
    }
    const auto capture = session_->stats<rcs_capture_stats>();
    statistics_ = QStringLiteral("%1 %2×%3 · 目标 %4 FPS · %5 · 采集 %6 · 编码 %7 · 解码 %8 · 丢弃 %9 · 恢复 %10")
                      .arg(codecName(stream.codec))
                      .arg(stream.width)
                      .arg(stream.height)
                      .arg(stream.frames_per_second)
                      .arg(stream.encoder_acceleration == RCS_ACCELERATION_SOFTWARE ? QStringLiteral("软件编码")
                                                                                    : QStringLiteral("硬件编码"))
                      .arg(capture.captured_frames)
                      .arg(sent.encoded_frames)
                      .arg(received.decoded_frames)
                      .arg(received.dropped_packets)
                      .arg(received.recovery_requests);
    if (sharing_) { // What adaptive bitrate sees on the path.
        statistics_ += QStringLiteral(" · 码率目标 %1 Mbit/s · 到达 %2 Mbit/s · 丢包 %3‰ · 排队 %4 ms")
                           .arg(sent.bitrate_bps / 1e6, 0, 'f', 1)
                           .arg(network.received_bps / 1e6, 0, 'f', 1)
                           .arg(network.loss_permille)
                           .arg(network.queue_delay_us / 1000.0, 0, 'f', 1);
    }
    emit statisticsChanged();
    // Each second in the system's log (logcat, Console), for looking into a run afterwards.
    QString line = QStringLiteral("rcsdk-demo %1: %2 %3x%4 %5 fps, %6 Mbit/s, rtt %7 ms")
                       .arg(sharing_ ? QStringLiteral("share") : QStringLiteral("view"), codecText_)
                       .arg(stream.width)
                       .arg(stream.height)
                       .arg(framesPerSecondNow_)
                       .arg(bitrateMbpsNow_, 0, 'f', 1)
                       .arg(roundTripMs_, 0, 'f', 1);
    if (sharing_) {
        line += QStringLiteral(", target %1 Mbit/s, arrives %2, loss %3‰, queue %4 ms")
                    .arg(sent.bitrate_bps / 1e6, 0, 'f', 1)
                    .arg(network.received_bps / 1e6, 0, 'f', 1)
                    .arg(network.loss_permille)
                    .arg(network.queue_delay_us / 1000.0, 0, 'f', 1);
    } else {
        const auto audio = session_->stats<rcs_audio_receive_stats>();
        line += QStringLiteral(", dropped %1, recoveries %2; sound %3: buffer %4/%5 ms, lost %6, recovered %7, "
                               "concealed %8 ms, dropped %9 ms, underruns %10 (device %11), device period %12")
                    .arg(received.dropped_packets)
                    .arg(received.recovery_requests)
                    .arg(audioCodecText_)
                    .arg(audio.buffer_ms)
                    .arg(audio.target_ms)
                    .arg(audio.lost_packets)
                    .arg(audio.recovered_packets)
                    .arg(audio.concealed_ms)
                    .arg(audio.dropped_ms)
                    .arg(audio.underruns)
                    .arg(audio.device_underruns)
                    .arg(audio.playback_period_frames);
    }
    line += QStringLiteral(", wifi low latency %1, stalls %2 (median %3 ms, every %4 ms; the other end's every %5 ms)")
                .arg(network.wifi_low_latency)
                .arg(network.stalls)
                .arg(network.stall_ms)
                .arg(network.stall_period_ms)
                .arg(network.peer_stall_period_ms);
    qInfo().noquote() << line;
}
