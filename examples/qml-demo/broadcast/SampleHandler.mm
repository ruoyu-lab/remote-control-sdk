// The demo's broadcast upload extension: shares the whole iPhone or iPad
// screen, other apps in front too. ReplayKit runs it in a process of its own
// with little memory (about 50 MB) and hands it the screen, the device's
// sound and the microphone (when the user turned it on in the system's
// panel) as sample buffers; it runs a sharing session of its own with the
// settings the app left (broadcast_settings.hpp) and pushes them as they come
// (rcs_session_push_sample_buffer). iOS lets no app control others: viewers
// watch. The stream takes the screen's shape, again when the device turns,
// and follows the settings when the app changes them; pausing the broadcast
// pauses the session (its viewers hear it).
#include "broadcast_settings.hpp"

#include <rcsdk/apple.hpp>
#include <rcsdk/rcsdk.hpp>

#import <ReplayKit/ReplayKit.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>

@interface SampleHandler : RPBroadcastSampleHandler
@end

namespace {

// The screen's size upright: ReplayKit's buffers lie as the device does.
std::pair<uint32_t, uint32_t> uprightSize(CMSampleBufferRef sample) {
    CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
    const auto width = uint32_t(CVPixelBufferGetWidth(image)), height = uint32_t(CVPixelBufferGetHeight(image));
    NSNumber* orientation = (__bridge NSNumber*)CMGetAttachment(
        sample, (__bridge CFStringRef)RPVideoSampleOrientationKey, nullptr);
    const uint32_t lies = orientation ? orientation.unsignedIntValue : kCGImagePropertyOrientationUp;
    const bool sideways = lies == kCGImagePropertyOrientationLeft || lies == kCGImagePropertyOrientationRight;
    return sideways ? std::pair(height, width) : std::pair(width, height);
}

// That shape with its longer side at most `longSide`, even.
std::pair<uint32_t, uint32_t> streamSize(uint32_t width, uint32_t height, uint32_t longSide) {
    const double scale = std::min(1.0, double(longSide) / std::max(width, height));
    return {uint32_t(width * scale) & ~1u, uint32_t(height * scale) & ~1u};
}

template <class T, size_t N>
void setList(T (&list)[N], const std::vector<T>& values) {
    std::fill(std::begin(list), std::end(list), T{});
    std::copy_n(values.begin(), std::min(values.size(), N), list);
}

bool decodeKey(const std::string& hex, uint8_t (&key)[32]) {
    if (hex.size() != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        unsigned value = 0;
        if (std::sscanf(hex.c_str() + i * 2, "%2x", &value) != 1) {
            return false;
        }
        key[i] = uint8_t(value);
    }
    return true;
}

} // namespace

@implementation SampleHandler {
    std::mutex _mutex; // ReplayKit calls from its own queues.
    std::optional<BroadcastSettings> _settings;
    std::unique_ptr<rcsdk::Session> _session;
    std::pair<uint32_t, uint32_t> _screen, _size; // The screen upright, and the stream.
}

- (void)broadcastStartedWithSetupInfo:(NSDictionary<NSString*, NSObject*>*)setupInfo {
    rcsdk::setLogCallback(RCS_LOG_INFO, [](const rcs_log_record& record) {
        NSLog(@"rcsdk %s: %s", record.module, record.message);
    });
    CFNotificationCenterAddObserver(
        CFNotificationCenterGetDarwinNotifyCenter(), (__bridge const void*)self,
        [](CFNotificationCenterRef, void* observer, CFNotificationName, const void*, CFDictionaryRef) {
            [(__bridge SampleHandler*)observer settingsChanged];
        },
        (__bridge CFStringRef)@(broadcastSettingsChanged), nullptr, CFNotificationSuspensionBehaviorDeliverImmediately);
    std::lock_guard lock(_mutex);
    _settings = loadBroadcastSettings();
    if (!_settings) {
        [self stop:@"请先在“远程控制 Demo”里打开“允许远程连接”，再点“共享整个屏幕”"];
    }
}

- (void)processSampleBuffer:(CMSampleBufferRef)sample withType:(RPSampleBufferType)type {
    std::lock_guard lock(_mutex);
    if (!_settings) {
        return;
    }
    if (type == RPSampleBufferTypeVideo) {
        _screen = uprightSize(sample);
        const auto size = streamSize(_screen.first, _screen.second, _settings->longSide);
        if (!_session || (size.first > size.second) != (_size.first > _size.second)) {
            if (![self shareAt:size]) {
                return;
            }
        }
        rcsdk::pushSampleBuffer(*_session, sample, RCS_SAMPLE_BUFFER_VIDEO); // Refused while no viewer watches.
    } else if (_session && _settings->sound) {
        // The device's sound and, when the user turned it on in the system's panel, the microphone: the SDK
        // mixes the two (rcs_send_audio_config's sample_buffer_*_percent).
        rcsdk::pushSampleBuffer(*_session, sample,
                                type == RPSampleBufferTypeAudioMic ? RCS_SAMPLE_BUFFER_MICROPHONE
                                                                   : RCS_SAMPLE_BUFFER_AUDIO);
    }
}

// The app changed the settings: the stream follows them, the connection stays.
- (void)settingsChanged {
    std::lock_guard lock(_mutex);
    auto changed = loadBroadcastSettings();
    if (!_settings || !changed) {
        return;
    }
    changed->port = _settings->port; // Only a broadcast of its own listens anew.
    changed->key = _settings->key;
    changed->tcp = _settings->tcp;
    _settings = std::move(changed);
    if (_session) {
        [self shareAt:streamSize(_screen.first, _screen.second, _settings->longSide)];
    }
}

// Starts the session at that stream size, or turns its stream to it and to the settings (holding _mutex).
- (bool)shareAt:(std::pair<uint32_t, uint32_t>)size {
    _size = size;
    const auto& s = *_settings;
    rcsdk::SessionConfig config(RCS_ROLE_SHARE);
    config.setVideo(size.first, size.second, s.framesPerSecond, s.bitrateMbps * 1'000'000)
        .setAudioEnabled(s.sound)
        .setAudioSource(RCS_AUDIO_HOST);
    setList(config.encoder().codecs, s.codecs);
    setList(config.sendAudio().codecs, s.audioCodecs);
    setList(config.receiveAudio().codecs, s.audioCodecs);
    if (_session) {
        _session->update(config);
        return true;
    }
    config.setAddress("0.0.0.0", s.port)
        .setTransport(s.tcp ? RCS_TRANSPORT_TCP : RCS_TRANSPORT_UDP)
        .setCaptureSource(RCS_CAPTURE_HOST);
    if (!decodeKey(s.key, config.core().identity.key)) {
        [self stop:@"共享的设置不完整，请在 Demo 里重新点“共享整个屏幕”"];
        _settings.reset();
        return false;
    }
    rcsdk::Callbacks callbacks;
    __weak SampleHandler* weakSelf = self;
    callbacks.onEvent = [weakSelf](const rcs_event& event) {
        if (event.type == RCS_EVENT_STATE && event.state.value == RCS_STATE_FAILED) {
            [weakSelf stop:[NSString stringWithFormat:@"共享整个屏幕失败：%s", rcs_result_message(event.state.result)]];
        }
    };
    try {
        _session = std::make_unique<rcsdk::Session>(config, std::move(callbacks));
    } catch (const rcsdk::Error& error) {
        [self stop:[NSString stringWithFormat:@"共享整个屏幕失败：%s", error.what()]];
        _settings.reset();
        return false;
    }
    if (_session->start() != RCS_OK) {
        [self stop:[NSString stringWithFormat:@"共享整个屏幕失败：%s", _session->failure().message.c_str()]];
        _session.reset();
        _settings.reset();
        return false;
    }
    return true;
}

- (void)broadcastPaused {
    std::lock_guard lock(_mutex);
    if (_session) {
        _session->pause(RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO);
    }
}

- (void)broadcastResumed {
    std::lock_guard lock(_mutex);
    if (_session) {
        _session->resume(RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO);
    }
}

- (void)dealloc {
    CFNotificationCenterRemoveEveryObserver(CFNotificationCenterGetDarwinNotifyCenter(), (__bridge const void*)self);
}

- (void)broadcastFinished {
    std::lock_guard lock(_mutex);
    _session.reset();
    _settings.reset();
}

- (void)stop:(NSString*)reason {
    [self finishBroadcastWithError:[NSError errorWithDomain:@"local.rcsdk.qmldemo"
                                                       code:1
                                                   userInfo:@{NSLocalizedDescriptionKey : reason}]];
}

@end
