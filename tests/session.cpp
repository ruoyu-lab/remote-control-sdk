// End-to-end share and view sessions over loopback, through the public API.
// The sharer captures a test pattern (installed through the testing hook).
#include "support/check.hpp"
#include "support/test_pattern.hpp"
#include "support/test_tone.hpp"
#include "support/shaping_proxy.hpp"
#include "support/tcp_proxy.hpp"
#include "support/surface_pattern.hpp"
#include "support/echo_module.hpp"
#include "support/test_channel.hpp"
#include "support/turn_server.hpp"

#include "codecs/codecs.hpp"
#include "media/gpu_frame.hpp"
#include "media/vpx.hpp"
#include "platform/decode_probes.hpp"
#include "platform/platform.hpp"
#include "transport/framed_socket.hpp"
#include "transport/ice.hpp"
#include "transport/tcp_stream.hpp"
#include "transport/websocket.hpp"
#ifdef RCSDK_TEST_FORWARDER
#include "../server/forwarder/src/forwarder.hpp"
#endif

#include "fake_clipboard.hpp"
#include "files.hpp"
#include "../modules/file_transfer/src/messages.hpp" // Not the clipboard's, nor the protocol's.
#include "../modules/power/src/messages.hpp" // The power module's: its interfaces...
#include "../modules/power/src/system.hpp"   // ...and its machine in the system's place.
#include "power_request.hpp"
#include "reader.hpp"

#include <rcsdk/agent.hpp>
#include <rcsdk/apple.hpp>
#include <rcsdk/clipboard.h>
#include <rcsdk/file_transfer.h>
#include <rcsdk/gpu.hpp>
#include <rcsdk/host_codec.hpp>
#include <rcsdk/host_transport.hpp>
#include <rcsdk/power.h>
#include <rcsdk/rate_control.hpp>
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/text_caret.h>
#include <rcsdk/tuning.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <d3d11_4.h>
#include <wrl/client.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#endif
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#endif
#include <rcsdk/android.hpp>

namespace {

using namespace std::chrono_literals;

// Collects the SDK's log for the whole test run.
struct LogCollector {
    std::mutex mutex;
    std::vector<std::string> lines;
    std::atomic<rcs_result> nested{RCS_OK};

    bool contains(const std::string& text) {
        std::lock_guard lock(mutex);
        return std::any_of(lines.begin(), lines.end(),
                           [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }
};

LogCollector& sessionLog() {
    static LogCollector log;
    return log;
}

// Sends the SDK's log to sessionLog() (again, after a scenario that took the callback).
void collectLog() {
    CHECK(rcsdk::setLogCallback(RCS_LOG_DEBUG, [](const rcs_log_record& record) {
              CHECK(record.level >= RCS_LOG_ERROR && record.level <= RCS_LOG_DEBUG && record.module && record.message);
              auto& log = sessionLog();
              if (log.nested == RCS_OK) { // Changing the callback from inside it is refused.
                  log.nested = rcs_set_log_callback(RCS_LOG_DEBUG, nullptr, nullptr);
              }
              std::lock_guard lock(log.mutex);
              log.lines.push_back(std::string(record.module) + ": " + record.message);
          }) == RCS_OK);
}

constexpr uint32_t h264Bit = 1u << RCS_CODEC_H264, hevcBit = 1u << RCS_CODEC_HEVC;

// H.264 in hardware where this system encodes and decodes it there, else in
// software: an explicit choice, as a host makes it.
// The test's injector in place of the system's (seams::setInputOverride). A
// system that executes no input (iOS) counts as executing what macOS does, so
// that sharing with control runs there too.
void overrideInput(rcsdk::seams::InputOverride input) {
    if (!input.types && !rcsdk::platform::capabilities().input_types) {
        for (const rcs_input_type type : {RCS_INPUT_MOVE, RCS_INPUT_MOVE_RELATIVE, RCS_INPUT_BUTTON, RCS_INPUT_WHEEL,
                                          RCS_INPUT_SCROLL, RCS_INPUT_KEY, RCS_INPUT_TEXT, RCS_INPUT_PEN,
                                          RCS_INPUT_RELEASE_ALL}) {
            input.types = input.types.value_or(0) | 1u << type;
        }
    }
    rcsdk::seams::setInputOverride(std::move(input));
}

rcs_acceleration testAcceleration() {
    const auto caps = rcsdk::capabilities();
    return (caps.hardware_encoders & caps.hardware_decoders & h264Bit) ? RCS_ACCELERATION_HARDWARE
                                                                       : RCS_ACCELERATION_SOFTWARE;
}

rcsdk::SessionConfig config(rcs_role role, uint16_t port, const uint8_t (&key)[32]) {
    rcsdk::SessionConfig c(role);
    c.setAddress("127.0.0.1", port).setKey(key).setCodecs({RCS_CODEC_H264}).setAcceleration(testAcceleration());
    // One viewer per run, and a viewer that leaves ends it, as these tests
    // expect; keepListening() and reconnect() cover the defaults.
    c.setKeepListening(false).setReconnect(false);
    if (role == RCS_ROLE_SHARE) {
        c.setVideo(640, 360, 30, 2'000'000);
    } else {
        c.setAudioPlayback(false); // Sound reaches on_audio, never the speakers.
    }
    return c;
}

// The tuning a test asks for, on the configuration's preset (rcsdk/tuning.hpp).
template <class Config>
rcsdk::SessionConfig& decodeQueue(Config&& c, uint32_t frames, uint32_t maxAgeMs) {
    auto& video = c.template extension<rcs_video_tuning_config>();
    video.queue_frames = frames;
    video.max_queue_age_ms = maxAgeMs;
    return c;
}
template <class Config>
rcsdk::SessionConfig& reconnecting(Config&& c, uint32_t windowMs, uint32_t intervalMs) {
    auto& network = c.setReconnect(true).template extension<rcs_network_tuning_config>();
    network.reconnect_window_ms = windowMs;
    network.reconnect_interval_ms = intervalMs;
    return c;
}
template <class Config>
rcsdk::SessionConfig& startBitrate(Config&& c, uint32_t bps) {
    c.template extension<rcs_video_tuning_config>().start_bitrate_bps = bps;
    return c;
}
template <class Config>
rcsdk::SessionConfig& soundMarked(Config&& c, uint32_t dscp) {
    c.template extension<rcs_network_tuning_config>().audio_dscp = dscp;
    return c;
}

// The replacing layer's callbacks, as functions.
struct EncodedCallback final : rcsdk::EncodedOutput {
    explicit EncodedCallback(std::function<void(const rcs_encoded_packet&)> f) : function(std::move(f)) {}
    void video(const rcs_encoded_packet& packet) override { function(packet); }
    std::function<void(const rcs_encoded_packet&)> function;
};
struct RateCallback final : rcsdk::RateDecider {
    explicit RateCallback(std::function<void(const rcs_rate_report&, rcs_rate_decision&)> f) : function(std::move(f)) {}
    void decide(const rcs_rate_report& report, rcs_rate_decision& decision) override { function(report, decision); }
    std::function<void(const rcs_rate_report&, rcs_rate_decision&)> function;
};

template <class Config>
rcsdk::SessionConfig& dataQueue(Config&& c, uint32_t queueBytes, uint32_t inFlightBytes = 65536) {
    auto& network = c.template extension<rcs_network_tuning_config>();
    network.send_queue_bytes = queueBytes;
    network.in_flight_bytes = inFlightBytes;
    return c;
}

// Records a session's events; a module's with what they point to.
struct EventLog {
    struct ModuleEvent {
        rcs_viewer_id viewer = 0;
        std::string module;
        rcs_module_event_kind kind = 0;
        rcs_result result = RCS_OK;
        std::string message;
        std::vector<uint8_t> payload;
    };
    std::mutex mutex;
    std::vector<rcs_event> events;
    std::vector<ModuleEvent> moduleEvents;
    std::atomic<bool> overlapping{false};
    std::atomic<int> inside{0};

    std::function<void(const rcs_event&)> callback() {
        return [this](const rcs_event& event) {
            overlapping = overlapping || ++inside > 1;
            {
                std::lock_guard lock(mutex);
                events.push_back(event);
                if (event.type == RCS_EVENT_MODULE) {
                    const auto& m = event.module;
                    const auto* payload = static_cast<const uint8_t*>(m.payload);
                    moduleEvents.push_back({event.viewer_id, m.module, m.kind, m.result, m.message,
                                            std::vector<uint8_t>(payload, payload + (payload ? m.size : 0))});
                    events.back().module.module = nullptr; // Borrowed for the callback only.
                    events.back().module.payload = nullptr;
                }
            }
            --inside;
        };
    }
    std::vector<ModuleEvent> ofModule(const std::string& module, rcs_module_event_kind kind) {
        std::lock_guard lock(mutex);
        std::vector<ModuleEvent> result;
        for (const auto& event : moduleEvents) {
            if (event.module == module && event.kind == kind) {
                result.push_back(event);
            }
        }
        return result;
    }
    std::vector<rcs_event> of(rcs_event_type type) {
        std::lock_guard lock(mutex);
        std::vector<rcs_event> result;
        for (const auto& event : events) {
            if (event.type == type) {
                result.push_back(event);
            }
        }
        return result;
    }
};

template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// Streams to a viewer that takes GPU frames or CPU frames, never both.
void streaming(const uint8_t (&key)[32], uint16_t port, bool gpu) {
    std::atomic<int> frames{0}, encoded{0};
    std::atomic<bool> callbackLifecycleRejected{false};
    rcsdk::GpuFrame kept;
    std::mutex keptMutex;

    EventLog shareEvents, viewEvents;
    rcsdk::Callbacks shareCallbacks;
    EncodedCallback output([&](const rcs_encoded_packet& p) {
        CHECK(p.codec == RCS_CODEC_H264 && p.data_size > 0 && p.config_size > 0);
        CHECK(p.format == RCS_PICTURE_420_8 && p.color.matrix == RCS_MATRIX_BT709 &&
              p.color.range == RCS_RANGE_LIMITED && p.color.transfer == RCS_TRANSFER_BT709);
        ++encoded;
    });
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(config(RCS_ROLE_SHARE, port, key).use(rcsdk::encodedOutput(output)), shareCallbacks);

    rcsdk::Session* viewPointer = nullptr;
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onEvent = viewEvents.callback();
    if (gpu) {
        viewCallbacks.onGpuFrame = [&](const rcsdk::GpuFrame& frame) {
            const auto info = frame.info();
            CHECK(info.width == 640 && info.height == 360 && info.resource);
            // The default picture: NV12, BT.709 primaries, transfer and matrix, limited range.
            CHECK(info.format == RCS_PIXEL_NV12 && info.color.primaries == RCS_PRIMARIES_BT709 &&
                  info.color.transfer == RCS_TRANSFER_BT709 && info.color.matrix == RCS_MATRIX_BT709 &&
                  info.color.range == RCS_RANGE_LIMITED);
            if (++frames == 1 && viewPointer) {
                callbackLifecycleRejected = viewPointer->stop() == RCS_ERROR_CALLBACK_THREAD;
            }
            std::lock_guard lock(keptMutex);
            kept = frame; // Retained beyond the callback and the session.
        };
    } else {
        viewCallbacks.onFrame = [&](const rcs_frame& f) {
            CHECK(f.width == 640 && f.height == 360 && f.format == RCS_PIXEL_BGRA8 && f.data);
            ++frames;
        };
    }
    rcsdk::Session view(config(RCS_ROLE_VIEW, port, key), viewCallbacks);
    viewPointer = &view;

    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 20; }));
    if (gpu) {
        CHECK(callbackLifecycleRejected);
    }

    const auto stream = view.streamInfo();
    CHECK(stream.codec == RCS_CODEC_H264 && stream.width == 640 && stream.height == 360);
    CHECK(stream.format == RCS_PICTURE_420_8 && stream.color.matrix == RCS_MATRIX_BT709 &&
          stream.color.range == RCS_RANGE_LIMITED && stream.color.primaries == RCS_PRIMARIES_BT709 &&
          !stream.hdr.max_luminance);
    CHECK(stream.frames_per_second == 30 && stream.content_width == 640);
    CHECK(stream.encoder_acceleration == testAcceleration());
    // Each end knows the other: its platform and what it offers.
    const auto platform = rcsdk::capabilities().platform;
    for (const auto& peer : {view.peerInfo(), share.peerInfo()}) {
        CHECK(peer.platform == platform && peer.codecs == h264Bit && (peer.sends_audio || peer.receives_audio));
        CHECK(peer.codec_limits[0].codec == RCS_CODEC_H264 &&
              peer.codec_limits[0].picture_formats == (1u << RCS_PICTURE_420_8));
        CHECK((peer.send_audio_codecs & peer.receive_audio_codecs & (1u << RCS_AUDIO_OPUS)) != 0);
    }
    // The viewer learns which input the sharer takes: what its machine executes.
    CHECK(view.peerInfo().input_types == rcsdk::capabilities().input_types && share.peerInfo().input_types == 0);
    CHECK(platform == RCS_PLATFORM_MACOS || platform == RCS_PLATFORM_WINDOWS
              ? (rcsdk::capabilities().input_types & (1u << RCS_INPUT_TEXT)) != 0
              : !rcsdk::capabilities().input_types); // iOS, Android: none; Linux: no platform layer yet.
    const auto received = view.stats<rcs_video_receive_stats>();
    CHECK(received.decoded_frames >= 20 && received.delivered_frames >= 20 && received.decoder[0]);
    CHECK(view.stats<rcs_network_stats>().bytes > 0 && share.stats<rcs_network_stats>().bytes > 0);
    // The sharer counts a packet once it is sent, which may be after the viewer decoded it.
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().encoded_frames >= 20; }));
    const auto sent = share.stats<rcs_video_send_stats>();
    CHECK(sent.encoded_frames >= 20 && encoded >= 20 && sent.encoder[0] && sent.preview_frames == 0);
    CHECK(share.stats<rcs_capture_stats>().captured_frames >= 20);
    // Sections of the other role do not exist.
    rcs_capture_stats capture = RCS_CAPTURE_STATS_INIT;
    CHECK(rcs_session_get_stats(view.handle(), RCS_STATS_CAPTURE, &capture) == RCS_ERROR_UNSUPPORTED);
    rcs_video_receive_stats receive = RCS_VIDEO_RECEIVE_STATS_INIT;
    CHECK(rcs_session_get_stats(share.handle(), RCS_STATS_VIDEO_RECEIVE, &receive) == RCS_ERROR_UNSUPPORTED);
    // A section no version defines is a mistake, not a missing capability.
    CHECK(rcs_session_get_stats(share.handle(), 99, &receive) == RCS_ERROR_INVALID_ARGUMENT);

    // Events: the states in order, the stream, and the codec each end opened.
    const auto states = [](EventLog& log) {
        std::vector<rcs_state> values;
        for (const auto& event : log.of(RCS_EVENT_STATE)) {
            values.push_back(event.state.value);
        }
        return values;
    };
    CHECK((states(shareEvents) == std::vector<rcs_state>{RCS_STATE_STARTING, RCS_STATE_LISTENING, RCS_STATE_RUNNING}));
    CHECK((states(viewEvents) == std::vector<rcs_state>{RCS_STATE_STARTING, RCS_STATE_CONNECTING, RCS_STATE_RUNNING}));
    for (EventLog* log : {&shareEvents, &viewEvents}) {
        const auto streams = log->of(RCS_EVENT_STREAM);
        CHECK(!streams.empty() && streams[0].stream.info.codec == RCS_CODEC_H264 && streams[0].stream.info.width == 640);
        // The first announcement names everything about the picture.
        constexpr uint32_t picture = RCS_STREAM_CODEC | RCS_STREAM_FORMAT | RCS_STREAM_SIZE | RCS_STREAM_FRAME_RATE |
                                     RCS_STREAM_BITRATE | RCS_STREAM_CONTENT | RCS_STREAM_DISPLAY;
        CHECK((streams[0].stream.changes & picture) == picture);
        for (size_t i = 1; i < streams.size(); ++i) { // Then only the sound starts.
            CHECK(streams[i].stream.changes == RCS_STREAM_AUDIO);
        }
        const auto peers = log->of(RCS_EVENT_PEER);
        CHECK(peers.size() == 1 && peers[0].peer.platform == platform && peers[0].peer.codecs == h264Bit);
        const auto codecs = log->of(RCS_EVENT_CODEC);
        CHECK(codecs.size() == 1 && codecs[0].codec.codec == RCS_CODEC_H264 && codecs[0].codec.name[0]);
        CHECK(codecs[0].codec.acceleration == testAcceleration() && codecs[0].codec.backend != RCS_BACKEND_AUTO);
        CHECK(codecs[0].timestamp_ns > 0 && codecs[0].struct_size == sizeof(rcs_event));
        CHECK(!log->overlapping);
    }
    CHECK(shareEvents.of(RCS_EVENT_CODEC)[0].codec.direction == RCS_CODEC_ENCODER);
    CHECK(viewEvents.of(RCS_EVENT_CODEC)[0].codec.direction == RCS_CODEC_DECODER);
    CHECK(std::string(viewEvents.of(RCS_EVENT_CODEC)[0].codec.name) == received.decoder);
    CHECK(std::string(shareEvents.of(RCS_EVENT_CODEC)[0].codec.name) == sent.encoder);

    // Control is off by default: the viewer is not granted it.
    CHECK(!view.controlInfo().granted);
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    CHECK(view.sendInput(move) == RCS_ERROR_INVALID_STATE);
    CHECK(share.sendInput(move) == RCS_ERROR_UNSUPPORTED); // A sharer does not send input.

    CHECK(view.stop() == RCS_OK);
    CHECK(view.state() == RCS_STATE_STOPPED && view.failure().result == RCS_OK);
    // The sharer notices the viewer leaving and fails its run.
    CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }));
    CHECK(share.failure().result == RCS_ERROR_DISCONNECTED && !share.failure().message.empty());
    CHECK(states(viewEvents).back() == RCS_STATE_STOPPED);
    const auto failed = shareEvents.of(RCS_EVENT_STATE).back();
    CHECK(failed.state.value == RCS_STATE_FAILED && failed.state.result == RCS_ERROR_DISCONNECTED);

    std::lock_guard lock(keptMutex);
    if (gpu) {
        CHECK(kept && kept.info().width == 640 && kept.wait(1000) == RCS_OK);
    }
}

void recovery(const uint8_t (&key)[32]) {
    // A viewer that cannot keep up recovers via keyframes instead of building latency.
    std::atomic<bool> slow{true};
    std::atomic<int> frames{0};
    EventLog events;
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47822, key));
    rcsdk::Callbacks callbacks;
    callbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
        if (slow) {
            std::this_thread::sleep_for(140ms);
        }
    };
    callbacks.onEvent = events.callback();
    rcsdk::Session view(decodeQueue(config(RCS_ROLE_VIEW, 47822, key), 2, 40), callbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    std::this_thread::sleep_for(1500ms);
    slow = false;
    const int before = frames;
    std::this_thread::sleep_for(1200ms);
    const auto stats = view.stats<rcs_video_receive_stats>();
    CHECK(stats.dropped_packets > 0 && stats.recovery_requests > 0 && stats.decoder_resets > 0);
    CHECK(frames - before >= 10);
    CHECK(view.state() == RCS_STATE_RUNNING);
    const auto recoveries = events.of(RCS_EVENT_RECOVERY);
    CHECK(!recoveries.empty() && recoveries.size() <= stats.recovery_requests);
    CHECK(recoveries[0].recovery.reason == RCS_RECOVERY_BACKLOG);

    // The queue limits change while running; with generous ones a slow viewer
    // falls behind for a while (32 frames: ~1.4 s here) instead of recovering.
    auto generous = rcsdk::defaults<rcs_video_tuning_config>();
    generous.queue_frames = 32;
    generous.max_queue_age_ms = 0;
    CHECK(view.update(generous) == RCS_OK);
    slow = true;
    std::this_thread::sleep_for(200ms);
    const uint64_t requests = view.stats<rcs_video_receive_stats>().recovery_requests;
    std::this_thread::sleep_for(600ms);
    CHECK(view.stats<rcs_video_receive_stats>().recovery_requests == requests);
    slow = false;
}

// The same session over UDP: pictures, statistics and remote control, and a
// viewer leaving is noticed at once.
void udpStreaming(const uint8_t (&key)[32]) {
    std::atomic<int> frames{0};
    rcsdk::Callbacks callbacks;
    callbacks.onFrame = [&](const rcs_frame& frame) {
        CHECK(frame.width == 640 && frame.height == 360);
        ++frames;
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47836, key).setTransport(RCS_TRANSPORT_UDP));
    // A queue that never drops for age: a packet dropped is one the transport lost, not one a viewer slowed
    // down (a sanitizer build, a loaded machine) let go to stay current.
    rcsdk::Session view(decodeQueue(config(RCS_ROLE_VIEW, 47836, key).setTransport(RCS_TRANSPORT_UDP), 32, 0),
                        callbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 30; }));
    CHECK(view.streamInfo().codec == RCS_CODEC_H264 && view.streamInfo().width == 640);
    CHECK(view.stats<rcs_network_stats>().bytes > 0 && share.stats<rcs_video_send_stats>().encoded_frames >= 30);
    CHECK(view.stats<rcs_video_receive_stats>().dropped_packets == 0);
    // A TCP viewer cannot reach a UDP sharer: both ends must agree.
    rcsdk::Session tcpView(config(RCS_ROLE_VIEW, 47836, key).setTransport(RCS_TRANSPORT_TCP));
    CHECK(tcpView.start() == RCS_OK);
    CHECK(waitFor([&] { return tcpView.state() == RCS_STATE_FAILED; }));
    const auto stopped = std::chrono::steady_clock::now();
    CHECK(view.stop() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }, 2s));
    CHECK(std::chrono::steady_clock::now() - stopped < 1s && share.failure().result == RCS_ERROR_DISCONNECTED);
}

// The sharer serves viewers one after another (the default): a viewer leaving
// takes the run back to LISTENING, with the reason; the next viewer starts afresh.
void keepListening(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47868, key).setKeepListening(true), shareCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    const auto serve = [&](uint64_t connection) {
        std::atomic<int> frames{0};
        rcsdk::Callbacks callbacks;
        callbacks.onFrame = [&](const rcs_frame&) { ++frames; };
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47868, key), callbacks);
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 10; }));
        CHECK(share.state() == RCS_STATE_RUNNING && share.peerInfo().codecs == h264Bit);
        CHECK(share.stats<rcs_network_stats>().connections == connection && view.stats<rcs_network_stats>().connections == 1);
        CHECK(view.stop() == RCS_OK);
    };
    serve(1);
    // The viewer stopped: listening again, saying so; what was known of the viewer is gone.
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 2s));
    {
        const auto last = shareEvents.of(RCS_EVENT_STATE).back();
        CHECK(last.state.value == RCS_STATE_LISTENING && last.state.result == RCS_ERROR_DISCONNECTED);
    }
    CHECK(share.peerInfo().platform == RCS_PLATFORM_UNKNOWN && share.streamInfo().codec == 0);
    CHECK(share.failure().result == RCS_OK);
    serve(2);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 2s));
    // Each viewer got its own announcements.
    CHECK(shareEvents.of(RCS_EVENT_PEER).size() == 2);
    const auto streams = shareEvents.of(RCS_EVENT_STREAM);
    CHECK(std::count_if(streams.begin(), streams.end(),
                        [](const rcs_event& e) { return (e.stream.changes & RCS_STREAM_CODEC) != 0; }) == 2);
    // Turned off while running: the next viewer leaving ends the run, as before.
    auto once = rcsdk::defaults<rcs_reconnect_config>();
    once.keep_listening = 0;
    CHECK(share.update(once) == RCS_OK);
    serve(3);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }, 2s));
    CHECK(share.failure().result == RCS_ERROR_DISCONNECTED);
}

// The viewer survives a broken path (the default): it reconnects within its
// window and the sharer, listening again, takes it as a new viewer. A sharer
// that stopped is not waited for; a path that stays broken ends the run.
void reconnect(const uint8_t (&key)[32]) {
    EventLog viewEvents;
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onEvent = viewEvents.callback();
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    // Without migration (rcs_paths_config; the viewer's 0 makes it so for
    // both): a lost path is a lost connection, as before (path_hold keeps it).
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47869, key).setKeepListening(true));
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47869));
    rcsdk::Session view(reconnecting(config(RCS_ROLE_VIEW, proxy.address().port(), key), 30000, 500).setMigrate(false),
                        viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10; }));

    // The path breaks: both ends notice within 5 s. The viewer keeps trying.
    proxy.outage = true;
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RECONNECTING; }, 8s));
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 8s));
    {
        const auto last = viewEvents.of(RCS_EVENT_STATE).back();
        CHECK(last.state.value == RCS_STATE_RECONNECTING && last.state.result == RCS_ERROR_NETWORK);
    }
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    CHECK(view.sendInput(move) == RCS_ERROR_INVALID_STATE);
    std::this_thread::sleep_for(3s); // Attempts fail meanwhile.
    CHECK(view.state() == RCS_STATE_RECONNECTING && view.failure().result == RCS_OK);
    // The path is back: the viewer gets in again; its attempts were never CONNECTING.
    proxy.outage = false;
    const int before = frames;
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING; }, 15s));
    CHECK(waitFor([&] { return frames >= before + 10; }));
    CHECK(view.stats<rcs_network_stats>().connections == 2 && share.stats<rcs_network_stats>().connections == 2);
    const auto states = viewEvents.of(RCS_EVENT_STATE);
    CHECK(std::count_if(states.begin(), states.end(),
                        [](const rcs_event& e) { return e.state.value == RCS_STATE_CONNECTING; }) == 1);
    const auto streams = viewEvents.of(RCS_EVENT_STREAM);
    CHECK(std::count_if(streams.begin(), streams.end(),
                        [](const rcs_event& e) { return (e.stream.changes & RCS_STREAM_CODEC) != 0; }) == 2);
    // The sharer stops: the viewer learns it closed and does not try again.
    CHECK(share.stop() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }, 3s));
    CHECK(view.failure().result == RCS_ERROR_DISCONNECTED);

    // The window runs out: a path that stays broken ends the run with the reason.
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    auto brief = rcsdk::defaults<rcs_network_tuning_config>();
    brief.reconnect_window_ms = 8000;
    brief.reconnect_interval_ms = 500;
    CHECK(view.update(brief) == RCS_OK);
    frames = 0;
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10; }));
    proxy.outage = true;
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RECONNECTING; }, 8s));
    CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }, 20s));
    CHECK(view.failure().result == RCS_ERROR_NETWORK);
    proxy.outage = false;
    CHECK(share.stop() == RCS_OK);
}

// The hosts' own messages: reliable ones whole and in order both ways, big
// ones in parts, unreliable ones as datagrams; the queue refuses what it
// cannot hold and says when it has room again. Over UDP and TCP.
void dataChannels(const uint8_t (&key)[32]) {
    uint16_t port = 47870;
    for (const rcs_transport transport : {RCS_TRANSPORT_UDP, RCS_TRANSPORT_TCP}) {
        ++port;
        struct Received {
            std::mutex mutex;
            std::vector<std::pair<uint32_t, std::string>> messages;
            std::vector<uint8_t> big;
            int bigMessages = 0, datagrams = 0;
            size_t count() {
                std::lock_guard lock(mutex);
                return messages.size();
            }
            int bigCount() {
                std::lock_guard lock(mutex);
                return bigMessages;
            }
        } atShare, atView;
        const auto collect = [](Received& r) {
            return [&r](const rcs_data_message& m) {
                std::lock_guard lock(r.mutex);
                CHECK(m.struct_size == sizeof(rcs_data_message) && m.size > 0 && m.data);
                if (m.flags & RCS_DATA_UNRELIABLE) {
                    ++r.datagrams;
                } else if (m.channel == 3) {
                    r.big.assign(m.data, m.data + m.size);
                    ++r.bigMessages;
                } else {
                    r.messages.emplace_back(m.channel, std::string(reinterpret_cast<const char*>(m.data), m.size));
                }
            };
        };
        EventLog shareEvents;
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onData = collect(atShare);
        shareCallbacks.onEvent = shareEvents.callback();
        viewCallbacks.onData = collect(atView);
        viewCallbacks.onFrame = [](const rcs_frame&) {};
        rcsdk::Session share(dataQueue(config(RCS_ROLE_SHARE, port, key).setTransport(transport), 1u << 20),
                             shareCallbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, port, key).setTransport(transport), viewCallbacks);
        CHECK(share.sendData(1, "early") == RCS_ERROR_INVALID_STATE); // Not running.
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING && share.state() == RCS_STATE_RUNNING; }));

        // Both ways, in order across channels.
        for (int i = 0; i < 200; ++i) {
            CHECK(view.sendData(uint32_t(i % 2), std::to_string(i)) == RCS_OK);
            CHECK(share.sendData(5, std::to_string(i)) == RCS_OK);
        }
        CHECK(waitFor([&] { return atShare.count() == 200 && atView.count() == 200; }));
        for (size_t i = 0; i < 200; ++i) {
            CHECK(atShare.messages[i].first == i % 2 && atShare.messages[i].second == std::to_string(i));
            CHECK(atView.messages[i].first == 5 && atView.messages[i].second == std::to_string(i));
        }
        // A big message arrives whole.
        std::vector<uint8_t> big(5u << 20);
        for (size_t i = 0; i < big.size(); ++i) {
            big[i] = uint8_t(i * 7 + (i >> 12));
        }
        CHECK(view.sendData(3, big.data(), uint32_t(big.size())) == RCS_OK);
        CHECK(waitFor(
            [&] {
                std::lock_guard lock(atShare.mutex);
                return atShare.big.size() == big.size();
            },
            20s));
        CHECK(atShare.big == big);
        // Unreliable: datagrams; on loopback they all arrive.
        const std::vector<uint8_t> datagram(RCS_DATA_MAX_DATAGRAM, 7);
        for (int i = 0; i < 50; ++i) {
            CHECK(share.sendData(9, datagram.data(), uint32_t(datagram.size()), RCS_DATA_UNRELIABLE) == RCS_OK);
        }
        CHECK(waitFor([&] {
            std::lock_guard lock(atView.mutex);
            return atView.datagrams >= 45;
        }));
        // What the message cannot be.
        CHECK(view.sendData(1, nullptr, 0) == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(view.sendData(1, big.data(), RCS_DATA_MAX_DATAGRAM + 1, RCS_DATA_UNRELIABLE) ==
              RCS_ERROR_INVALID_ARGUMENT);
        CHECK(view.sendData(1, big.data(), 8, 4) == RCS_ERROR_INVALID_ARGUMENT); // An unknown flag.
        // The sharer's queue holds 1 MiB: pushing faster than the path takes it
        // is refused, and RCS_EVENT_DATA says when to go on.
        const std::vector<uint8_t> chunk(1u << 20, 1);
        constexpr int total = 24;
        int refused = 0;
        for (int sent = 0; sent < total;) {
            const auto result = share.sendData(3, chunk.data(), uint32_t(chunk.size()));
            if (result == RCS_OK) {
                ++sent;
                continue;
            }
            CHECK(result == RCS_ERROR_QUEUE_FULL);
            ++refused;
            const size_t events = shareEvents.of(RCS_EVENT_DATA).size();
            CHECK(waitFor([&] { return shareEvents.of(RCS_EVENT_DATA).size() > events; }, 10s));
        }
        CHECK(refused > 0);
        CHECK(waitFor(
            [&] {
                return atView.bigCount() == total && share.stats<rcs_data_stats>().sent_messages == 200 + 50 + total;
            },
            30s));
        const auto sent = share.stats<rcs_data_stats>();
        CHECK(sent.sent_messages == 200 + 50 + total && sent.sent_bytes > uint64_t(total) << 20 && sent.queued_bytes == 0);
        // Datagrams may be lost (a few are, on Windows loopback); the rest matches.
        const auto received = view.stats<rcs_data_stats>();
        CHECK(received.received_messages >= 200 + 45 + total && received.received_messages <= sent.sent_messages);
        CHECK(received.received_bytes <= sent.sent_bytes &&
              received.received_bytes >= sent.sent_bytes - 5 * RCS_DATA_MAX_DATAGRAM);
        // The picture went on throughout (all of the above takes well under a second on loopback).
        const uint64_t decoded = view.stats<rcs_video_receive_stats>().decoded_frames;
        CHECK(view.state() == RCS_STATE_RUNNING);
        CHECK(waitFor([&] { return view.stats<rcs_video_receive_stats>().decoded_frames >= decoded + 15; }));
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
}

// How the ends prove who they are: a password; device keys alone, each end
// trusting the other's; an impostor refused at the sharer (which keeps
// listening) and a sharer the viewer does not trust (its key reported).
void identities(const uint8_t (&key)[32]) {
    const auto watch = [](rcsdk::Session& share, rcsdk::Session& view, std::atomic<int>& frames) {
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 8s));
        CHECK(view.start() == RCS_OK);
        return waitFor([&] { return frames >= 10 || view.state() == RCS_STATE_FAILED; }, 8s) && frames >= 10;
    };
    std::atomic<int> frames{0};
    rcsdk::Callbacks counting;
    counting.onFrame = [&](const rcs_frame&) { ++frames; };
    {
        uint8_t none[32] = {}; // No connection key: the password replaces it.
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47872, none).setPassword("Vq7-pX2m-Lr9k"));
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47872, none).setPassword("Vq7-pX2m-Lr9k"), counting);
        CHECK(watch(share, view, frames));
        CHECK(view.peerInfo().has_public_key == 0);
        CHECK(view.stop() == RCS_OK && share.stop() == RCS_OK);
        rcsdk::Session guess(config(RCS_ROLE_VIEW, 47872, none).setPassword("Vq7-pX2m-Lr9K"), counting);
        rcsdk::Session keyed(config(RCS_ROLE_VIEW, 47872, key));
        frames = 0;
        CHECK(!watch(share, guess, frames));
        CHECK(guess.failure().result == RCS_ERROR_AUTHENTICATION &&
              guess.failure().message.find("Wrong password") != std::string::npos);
        CHECK(keyed.start() == RCS_OK);
        CHECK(waitFor([&] { return keyed.state() == RCS_STATE_FAILED; }, 8s));
        CHECK(keyed.failure().message.find("authenticates with a password") != std::string::npos);
        CHECK(share.state() == RCS_STATE_LISTENING); // Neither ended the sharer's run.
        CHECK(share.stop() == RCS_OK);
    }
    const auto sharerKey = rcsdk::generateDeviceKey(), viewerKey = rcsdk::generateDeviceKey();
    const auto sharerPublic = rcsdk::publicKeyOf(sharerKey), viewerPublic = rcsdk::publicKeyOf(viewerKey);
    uint8_t none[32] = {};
    auto sharing = config(RCS_ROLE_SHARE, 47873, none);
    sharing.setSecret(RCS_SECRET_NONE).setDeviceKey(sharerKey).setTrustedKeys({viewerPublic});
    rcsdk::Session share(sharing);
    {
        frames = 0;
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47873, none)
                                .setSecret(RCS_SECRET_NONE)
                                .setDeviceKey(viewerKey)
                                .setTrustedKeys({sharerPublic}),
                            counting);
        CHECK(watch(share, view, frames));
        const auto peer = view.peerInfo();
        CHECK(peer.has_public_key && std::equal(sharerPublic.begin(), sharerPublic.end(), peer.public_key));
        CHECK(share.peerInfo().has_public_key &&
              std::equal(viewerPublic.begin(), viewerPublic.end(), share.peerInfo().public_key));
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    {
        // A device the sharer does not trust: dropped, counted, the sharer listens
        // on; the viewer learns that it was refused (no reconnecting).
        const auto stranger = rcsdk::generateDeviceKey();
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47873, none)
                                .setSecret(RCS_SECRET_NONE)
                                .setDeviceKey(stranger)
                                .setTrustedKeys({sharerPublic}),
                            counting);
        frames = 0;
        CHECK(!watch(share, view, frames));
        CHECK(view.state() == RCS_STATE_FAILED && view.failure().result == RCS_ERROR_AUTHENTICATION &&
              view.failure().message.find("refused") != std::string::npos);
        CHECK(share.stats<rcs_network_stats>().rejected_connections == 1);
        CHECK(share.state() == RCS_STATE_LISTENING);
        // Over TCP the same.
        auto tcpSharing = sharing;
        tcpSharing.setTransport(RCS_TRANSPORT_TCP).setAddress("127.0.0.1", 47874);
        rcsdk::Session tcpShare(tcpSharing);
        rcsdk::Session tcpView(config(RCS_ROLE_VIEW, 47874, none)
                                   .setTransport(RCS_TRANSPORT_TCP)
                                   .setSecret(RCS_SECRET_NONE)
                                   .setDeviceKey(stranger)
                                   .setTrustedKeys({sharerPublic}),
                               counting);
        CHECK(!watch(tcpShare, tcpView, frames));
        CHECK(tcpView.failure().result == RCS_ERROR_AUTHENTICATION &&
              tcpView.failure().message.find("refused") != std::string::npos);
        CHECK(tcpShare.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    for (const auto transport : {RCS_TRANSPORT_UDP, RCS_TRANSPORT_TCP}) {
        // A viewer that proves no device key, to a sharer that trusts some:
        // told it was refused as well, not left to a silent path.
        rcsdk::Session guarded(config(RCS_ROLE_SHARE, 47875, key)
                                   .setTransport(transport)
                                   .setDeviceKey(sharerKey)
                                   .setTrustedKeys({viewerPublic}));
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47875, key).setTransport(transport), counting);
        frames = 0;
        const auto started = std::chrono::steady_clock::now();
        CHECK(!watch(guarded, view, frames));
        CHECK(view.failure().result == RCS_ERROR_AUTHENTICATION &&
              view.failure().message.find("refused") != std::string::npos &&
              view.failure().message.find("proves no device key") != std::string::npos);
        CHECK(std::chrono::steady_clock::now() - started < 4s);
        CHECK(view.peerInfo().has_public_key == 0);
        CHECK(guarded.stop() == RCS_OK);
    }
    {
        // A sharer the viewer does not trust: the run fails, and the viewer's
        // host learns which key the sharer proved.
        const auto expected = rcsdk::publicKeyOf(rcsdk::generateDeviceKey());
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47873, none)
                                .setSecret(RCS_SECRET_NONE)
                                .setDeviceKey(viewerKey)
                                .setTrustedKeys({expected}),
                            counting);
        frames = 0;
        CHECK(!watch(share, view, frames));
        CHECK(view.failure().result == RCS_ERROR_AUTHENTICATION);
        const auto peer = view.peerInfo();
        CHECK(peer.has_public_key && std::equal(sharerPublic.begin(), sharerPublic.end(), peer.public_key));
        CHECK(share.stop() == RCS_OK);
    }
}

// Short PINs (CPace) over UDP and TCP: the right PIN connects; a wrong one
// fails with AUTHENTICATION and is reported (RCS_EVENT_PIN, pin_failures);
// after pin_attempts failures the sharer refuses PIN handshakes, saying so,
// until the host sets the identity again (the same PIN too).
void pins(const uint8_t (&)[32]) {
    uint8_t none[32] = {};
    for (const auto& [transport, port] :
         {std::pair{RCS_TRANSPORT_UDP, uint16_t(47876)}, std::pair{RCS_TRANSPORT_TCP, uint16_t(47877)}}) {
        const auto pin = rcsdk::generatePin(6);
        auto wrong = pin;
        wrong.back() = pin.back() == '0' ? '1' : '0';
        EventLog shareEvents;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        auto sharing = config(RCS_ROLE_SHARE, port, none).setTransport(transport).setKeepListening(true).setPin(pin, 2);
        rcsdk::Session share(sharing, shareCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 8s));
        // RCS_OK once frames arrive, else the run's failure.
        const auto viewWith = [&](const std::string& tried) {
            std::atomic<int> frames{0};
            rcsdk::Callbacks counting;
            counting.onFrame = [&](const rcs_frame&) {
                ++frames;
            };
            rcsdk::Session view(config(RCS_ROLE_VIEW, port, none).setTransport(transport).setPin(tried), counting);
            CHECK(view.start() == RCS_OK);
            CHECK(waitFor([&] { return frames >= 5 || view.state() == RCS_STATE_FAILED; }, 10s));
            const auto failure = frames >= 5 ? rcsdk::SessionFailure{} : view.failure();
            CHECK(view.stop() == RCS_OK);
            return failure;
        };
        const auto pinEvents = [&] {
            return shareEvents.of(RCS_EVENT_PIN);
        };
        CHECK(viewWith(pin).result == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 8s));
        CHECK(pinEvents().empty());
        for (const uint32_t failures : {1u, 2u}) {
            const auto failed = viewWith(wrong);
            CHECK(failed.result == RCS_ERROR_AUTHENTICATION && failed.message.find("Wrong PIN") != std::string::npos);
            // UDP: the sharer learns at the attempt's end, 5 s after the answer.
            CHECK(waitFor([&] { return pinEvents().size() == failures; }, 8s));
            const auto event = pinEvents().back().pin;
            CHECK(event.failures == failures && event.attempts_left == 2 - failures);
        }
        CHECK(share.stats<rcs_network_stats>().pin_failures == 2);
        // Locked: the right PIN is refused too, and the viewer says why.
        const auto locked = viewWith(pin);
        CHECK(locked.result == RCS_ERROR_AUTHENTICATION &&
              locked.message.find("no more PIN attempts") != std::string::npos);
        CHECK(share.state() == RCS_STATE_LISTENING && pinEvents().size() == 2);
        // Setting the PIN again (the same one) counts afresh.
        CHECK(share.setIdentity(sharing.core().identity) == RCS_OK);
        CHECK(viewWith(pin).result == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
}

// P2P: a sharer that keeps listening answers each viewer's signal, the
// answer coming back with the route the host passed; the host delivers it to
// that viewer. A viewer signals at each connection, the sharer never first.
void p2pKeepListening(const uint8_t (&key)[32]) {
    std::mutex mutex;
    std::vector<std::pair<uint64_t, std::string>> answers; // What the sharer's host got, and for whom.
    rcsdk::Session* viewPointer = nullptr;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onSignal = [&](const std::string& text, uint64_t route) {
        std::lock_guard lock(mutex);
        answers.emplace_back(route, text);
        CHECK(viewPointer->signal(text, 77) == RCS_OK); // A viewer does not read the route.
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 0, key).setTransport(RCS_TRANSPORT_P2P).setKeepListening(true),
                         shareCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    std::this_thread::sleep_for(200ms);
    CHECK(answers.empty()); // Nothing until a viewer signals.
    for (const uint64_t route : {11u, 12u}) {
        std::atomic<int> frames{0};
        rcsdk::Callbacks viewCallbacks;
        viewCallbacks.onSignal = [&, route](const std::string& text, uint64_t own) {
            CHECK(own == 0);
            CHECK(share.signal(text, route) == RCS_OK);
        };
        viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
        rcsdk::Session view(config(RCS_ROLE_VIEW, 0, key).setTransport(RCS_TRANSPORT_P2P), viewCallbacks);
        {
            std::lock_guard lock(mutex);
            viewPointer = &view;
        }
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 10; }, 10s));
        CHECK(share.stats<rcs_network_stats>().connections == route - 10);
        CHECK(share.viewers().at(0).route == route);
        {
            std::lock_guard lock(mutex);
            CHECK(answers.size() == route - 10 && answers.back().first == route);
        }
        CHECK(view.stop() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 8s));
    }
    CHECK(share.stop() == RCS_OK);
}

// P2P: the hosts relay each end's signal to the other; ICE finds the path.
// Answers the host delivers again, also from an earlier run, are skipped.
void p2pStreaming(const uint8_t (&key)[32]) {
    std::atomic<int> frames{0};
    rcsdk::Session* sharePointer = nullptr;
    rcsdk::Session* viewPointer = nullptr;
    std::atomic<int> signals{0};
    std::string firstAnswer;
    std::atomic<bool> leftoverFirst{true};
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    // The "host's channel": hand the text to the other session. In later runs
    // it also delivers the sharer's first answer again, a leftover.
    shareCallbacks.onSignal = [&](const std::string& text, uint64_t route) {
        ++signals;
        CHECK(route == 5);
        if (firstAnswer.empty()) {
            firstAnswer = text;
        } else if (leftoverFirst) {
            CHECK(viewPointer->signal(firstAnswer) == RCS_OK);
        }
        CHECK(viewPointer->signal(text) == RCS_OK);
        if (text != firstAnswer && !leftoverFirst) {
            CHECK(viewPointer->signal(firstAnswer) == RCS_OK);
        }
    };
    viewCallbacks.onSignal = [&](const std::string& text, uint64_t) {
        ++signals;
        CHECK(sharePointer->signal(text, 5) == RCS_OK);
        CHECK(sharePointer->signal(text, 5) == RCS_OK);                  // A repeat: answered once.
        CHECK(viewPointer->signal(text) == RCS_ERROR_INVALID_ARGUMENT); // Its own role.
    };
    viewCallbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 0, key).setTransport(RCS_TRANSPORT_P2P), shareCallbacks);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 0, key).setTransport(RCS_TRANSPORT_P2P), viewCallbacks);
    sharePointer = &share;
    viewPointer = &view;
    CHECK(share.signal("not a signal") == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 20; }, 10s));
    CHECK(signals == 2 && view.streamInfo().codec == RCS_CODEC_H264);
    CHECK(view.stop() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }, 2s));

    // Later runs: the first run's answer reaches the viewer again, before and
    // after the current one. The viewer connects with the current one.
    for (const bool before : {true, false}) {
        leftoverFirst = before;
        frames = 0;
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 20; }, 10s));
        CHECK(view.stop() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }, 2s));
    }
    CHECK(signals == 6);

    // P2P without on_signal cannot work: refused at start.
    rcsdk::Session silent(config(RCS_ROLE_VIEW, 0, key).setTransport(RCS_TRANSPORT_P2P));
    CHECK(silent.start() == RCS_ERROR_INVALID_ARGUMENT && silent.failure().message.find("on_signal") != std::string::npos);
    // TCP and UDP need a port.
    CHECK(throws<rcsdk::Error>([&] { rcsdk::Session invalid(config(RCS_ROLE_VIEW, 0, key)); }));
}

// RELAY: as P2P, but every datagram passes a TURN server, reached over UDP or,
// where only TCP passes, over one TCP connection.
void relayStreaming(const uint8_t (&key)[32]) {
    rcsdk::test::TurnServer server("host", "s3cret");
    rcsdk::Session* sharePointer = nullptr;
    rcsdk::Session* viewPointer = nullptr;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onSignal = [&](const std::string& text, uint64_t) {
        viewPointer->signal(text);
    };
    viewCallbacks.onSignal = [&](const std::string& text, uint64_t) {
        sharePointer->signal(text, 1);
    };
    for (const rcs_turn_transport turnTransport : {RCS_TURN_UDP, RCS_TURN_TCP}) {
        std::atomic<int> frames{0};
        viewCallbacks.onFrame = [&](const rcs_frame&) {
            ++frames;
        };
        const uint64_t relayedBefore = server.relayed;
        const int tcpBefore = server.tcpClients;
        const std::string address = server.address(turnTransport).toString();
        rcsdk::Session share(config(RCS_ROLE_SHARE, 0, key).setRelay(address, "host", "s3cret", turnTransport),
                             shareCallbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 0, key).setRelay(address, "host", "s3cret", turnTransport),
                            viewCallbacks);
        sharePointer = &share;
        viewPointer = &view;
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 20; }, 10s));
        CHECK(server.relayed - relayedBefore > 40);
        CHECK(server.tcpClients - tcpBefore == (turnTransport == RCS_TURN_TCP ? 2 : 0));
        const std::string path = view.stats<rcs_network_stats>().path;
        CHECK(path.find("relay") != std::string::npos &&
              (path.find("over TCP") != std::string::npos) == (turnTransport == RCS_TURN_TCP));
        CHECK(view.stop() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }, 2s));
    }
    CHECK(server.allocations == 4);

    // Wrong credentials: the run fails and says why.
    rcsdk::Session wrong(config(RCS_ROLE_VIEW, 0, key).setRelay(server.addressText(), "host", "nope"), viewCallbacks);
    CHECK(wrong.start() == RCS_OK);
    CHECK(waitFor([&] { return wrong.state() == RCS_STATE_FAILED; }, 8s));
    CHECK(wrong.failure().result == RCS_ERROR_AUTHENTICATION && wrong.failure().message.find("TURN") != std::string::npos);
    // RELAY without a server is refused at start.
    rcsdk::Session none(config(RCS_ROLE_VIEW, 0, key).setTransport(RCS_TRANSPORT_RELAY), viewCallbacks);
    CHECK(none.start() == RCS_ERROR_INVALID_ARGUMENT && none.failure().message.find("turn_server") != std::string::npos);
}

// The host sees each report adaptive bitrate decides on and may decide
// instead: watching changes nothing, a ceiling holds, a decision beyond the
// limits is kept within them, and the rules change while running.
void rateDecisions(const uint8_t (&key)[32]) {
    std::mutex mutex;
    std::vector<rcs_rate_report> reports;
    std::vector<uint32_t> events;
    std::atomic<uint32_t> cap{0};
    std::atomic<bool> overshoot{false};
    rcsdk::Callbacks shareCallbacks;
    RateCallback decider([&](const rcs_rate_report& report, rcs_rate_decision& decision) {
        CHECK(report.struct_size == sizeof(report) && decision.struct_size == sizeof(decision));
        CHECK(decision.bitrate_bps >= report.minimum_bps && decision.bitrate_bps <= report.maximum_bps);
        {
            std::lock_guard lock(mutex);
            reports.push_back(report);
        }
        if (const uint32_t ceiling = cap) {
            decision.bitrate_bps = std::min(decision.bitrate_bps, ceiling);
        }
        if (overshoot) {
            decision.bitrate_bps = 4'000'000'000u;
        }
    });
    shareCallbacks.onEvent = [&](const rcs_event& event) {
        if (event.type == RCS_EVENT_RATE) {
            std::lock_guard lock(mutex);
            events.push_back(event.rate.bitrate_bps);
        }
    };
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    rcsdk::Session share(startBitrate(config(RCS_ROLE_SHARE, 47892, key)
                                          .setTransport(RCS_TRANSPORT_UDP)
                                          .setVideo(640, 360, 30, 6'000'000)
                                          .setRateMode(RCS_RATE_ADAPTIVE, 300'000)
                                          .use(rcsdk::rateDecider(decider)),
                                      2'000'000),
                         shareCallbacks);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47892, key).setTransport(RCS_TRANSPORT_UDP), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 30; }));
    const auto bitrate = [&] { return share.stats<rcs_video_send_stats>().bitrate_bps; };
    const auto latest = [&] {
        std::lock_guard lock(mutex);
        return reports.back();
    };

    // Watching: about 20 reports a second, with what was measured; the open
    // path lets the rules raise the bitrate from its start.
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return reports.size() >= 40;
    }));
    CHECK(waitFor([&] { return bitrate() > 2'500'000; }));
    {
        std::lock_guard lock(mutex);
        uint64_t previous = 0;
        for (const auto& r : reports) {
            CHECK(r.timestamp_ns > previous && r.window_us >= 10'000 && r.window_us <= 1'000'000);
            CHECK(r.minimum_bps == 300'000 && r.maximum_bps <= 6'000'000 && r.maximum_bps > 5'000'000);
            CHECK(r.current_bps >= r.minimum_bps && r.current_bps <= r.maximum_bps);
            previous = r.timestamp_ns;
        }
        const auto& last = reports.back();
        CHECK(last.received_bytes > 0 && last.received_bps > 0 && last.round_trip_ns > 0 && last.encoder_bps > 0);
        CHECK(last.queue_delay_us < 50'000 && last.loss_permille < 20);
    }
    {
        const auto network = share.stats<rcs_network_stats>();
        CHECK(network.rate_reports >= 40 && network.rate_overrides == 0);
    }

    // A ceiling: the target comes down to it and stays there, reported as the
    // rules' changes are.
    cap = 1'000'000;
    CHECK(waitFor([&] { return bitrate() <= 1'000'000; }));
    std::this_thread::sleep_for(1s);
    CHECK(bitrate() <= 1'000'000 && share.stats<rcs_network_stats>().rate_overrides > 0);
    {
        std::lock_guard lock(mutex);
        CHECK(!events.empty() && events.back() <= 1'000'000);
    }

    // Beyond the limits: kept within them.
    cap = 0;
    overshoot = true;
    CHECK(waitFor([&] { return bitrate() == latest().maximum_bps; }));
    overshoot = false;

    // The rules change while running: without increases the target holds
    // where the ceiling left it; with the defaults back it climbs again.
    cap = 1'000'000;
    CHECK(waitFor([&] { return bitrate() <= 1'000'000; }));
    auto rules = rcsdk::defaults<rcs_video_tuning_config>();
    rules.probe_increase_percent = rules.increase_percent = rules.recovery_increase_percent = 0;
    CHECK(share.update(rules) == RCS_OK);
    std::this_thread::sleep_for(300ms); // The share loop hands the rules over within 100 ms.
    cap = 0;
    std::this_thread::sleep_for(2s);
    CHECK(bitrate() <= 1'000'000);
    CHECK(share.update(rcsdk::defaults<rcs_video_tuning_config>()) == RCS_OK);
    CHECK(waitFor([&] { return bitrate() > 1'500'000; }));
    rules = rcsdk::defaults<rcs_video_tuning_config>();
    rules.clear_queue_ms = rules.overuse_queue_ms;
    CHECK(share.update(rules) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcsdk::lastCallError().find("clear_queue_ms") != std::string::npos);
    view.stop();
}

// Adaptive bitrate: a 6 Mbit/s sharer behind a 2 Mbit/s bottleneck settles
// below it and the picture keeps flowing; when the path opens, it climbs. (The
// grain pattern needs about 0.7 Mbit/s at this size; a desktop needs less.)
// Parity for lost video (rcs_video_tuning_config.fec_mode) through a path
// that loses 5% of what the sharer sends: FIXED 20% rebuilds most of it
// (rcs_network_stats on both ends), OFF sends none, AUTO starts its own
// once the reports show the loss; all while running.
void parity(const uint8_t (&key)[32]) {
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    auto sharing = config(RCS_ROLE_SHARE, 47846, key).setTransport(RCS_TRANSPORT_UDP).setVideo(640, 360, 30, 4'000'000);
    auto& tuning = sharing.extension<rcs_video_tuning_config>();
    CHECK(tuning.fec_mode == RCS_FEC_AUTO && tuning.fec_percent == 50); // The default.
    tuning.fec_mode = RCS_FEC_FIXED;
    tuning.fec_percent = 20;
    rcsdk::Session share(sharing);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47846));
    proxy.lossPermille = 50;
    rcsdk::Session view(config(RCS_ROLE_VIEW, proxy.address().port(), key).setTransport(RCS_TRANSPORT_UDP),
                        viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 60; }, 10s));
    CHECK(waitFor([&] { return view.stats<rcs_network_stats>().fec_recovered > 0; }, 10s));
    auto shared = share.stats<rcs_network_stats>();
    auto viewed = view.stats<rcs_network_stats>();
    std::printf("parity: FIXED 20%% against 5%% loss: %llu parity sent, %llu fragments rebuilt, %llu resent, "
                "%llu pictures lost, %d frames\n",
                (unsigned long long)shared.fec_sent, (unsigned long long)viewed.fec_recovered,
                (unsigned long long)viewed.resend_recovered, (unsigned long long)viewed.frames_lost, frames.load());
    CHECK(shared.fec_percent == 20 && shared.fec_sent > 0 && viewed.fec_percent == 0 && viewed.fec_sent == 0);
    CHECK(viewed.fec_recovered > viewed.resend_recovered);
    // OFF: no more parity.
    tuning.fec_mode = RCS_FEC_OFF;
    CHECK(share.update(tuning) == RCS_OK);
    CHECK(waitFor([&] { return share.stats<rcs_network_stats>().fec_percent == 0; }));
    const uint64_t sentBefore = share.stats<rcs_network_stats>().fec_sent;
    const uint64_t resentBefore = view.stats<rcs_network_stats>().resend_recovered;
    CHECK(waitFor([&] { return view.stats<rcs_network_stats>().resend_recovered > resentBefore; }, 10s));
    CHECK(share.stats<rcs_network_stats>().fec_sent == sentBefore);
    // AUTO: parity of its own, as the loss goes on.
    tuning.fec_mode = RCS_FEC_AUTO;
    tuning.fec_percent = 30;
    CHECK(share.update(tuning) == RCS_OK);
    CHECK(waitFor([&] {
        const uint32_t now = share.stats<rcs_network_stats>().fec_percent;
        return now >= 10 && now <= 30;
    }));
    tuning.fec_percent = 0;
    CHECK(share.update(tuning) == RCS_ERROR_INVALID_ARGUMENT);
    tuning.fec_percent = 101;
    CHECK(share.update(tuning) == RCS_ERROR_INVALID_ARGUMENT);
    tuning.fec_percent = 30;
    tuning.fec_mode = 3;
    CHECK(share.update(tuning) == RCS_ERROR_INVALID_ARGUMENT);
    const int before = frames;
    CHECK(waitFor([&] { return frames >= before + 30; }));
}

// Pictures lost on the way (rcs_video_tuning_config.ref_invalidation),
// without parity. Loss the transport asks for again: an encoder that can
// (rcs_codec_implementation.ref_invalidation) drops each missing picture from
// its references at once (send statistics: invalidations), one that cannot or
// is told not to does nothing until a picture is given up. A picture given up
// on (an outage longer than the sharer keeps video): both ends report it,
// the viewer once decoding goes on, with how and how long it stood still:
// a keyframe where the encoder no longer holds what came before, a picture
// where it still holds a long-term reference from before (VideoToolbox).
void lostPictures(const uint8_t (&key)[32]) {
    uint16_t port = 47847;
    for (const uint32_t enabled : {1u, 0u}) {
        EventLog shareEvents, viewEvents;
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        viewCallbacks.onEvent = viewEvents.callback();
        std::atomic<int> frames{0};
        viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
        auto sharing =
            config(RCS_ROLE_SHARE, port, key).setTransport(RCS_TRANSPORT_UDP).setVideo(640, 360, 30, 4'000'000);
        auto& tuning = sharing.extension<rcs_video_tuning_config>();
        tuning.fec_mode = RCS_FEC_OFF;
        tuning.ref_invalidation = enabled;
        rcsdk::Session share(sharing, shareCallbacks);
        rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", port));
        rcsdk::Session view(config(RCS_ROLE_VIEW, proxy.address().port(), key).setTransport(RCS_TRANSPORT_UDP),
                            viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 15; }, 10s));
        // Whether the stream's encoder drops pictures from its references, as its codec event says.
        const auto encoders = shareEvents.of(RCS_EVENT_CODEC);
        const bool invalidates = !encoders.empty() && encoders.back().codec.ref_invalidation;
        std::printf("lost_pictures: %s %s references\n", encoders.empty() ? "?" : encoders.back().codec.name,
                    invalidates ? "drops" : "cannot drop");
        proxy.lossPermille = 100;
        std::this_thread::sleep_for(3s);
        proxy.lossPermille = 0;
        const auto sent = share.stats<rcs_video_send_stats>();
        const auto repaired = view.stats<rcs_network_stats>();
        std::printf("lost_pictures: ref_invalidation %u, 10%% loss for 3 s: %llu invalidations, %llu resent\n",
                    enabled, (unsigned long long)sent.invalidations, (unsigned long long)repaired.resend_recovered);
        CHECK(repaired.resend_recovered > 0);
        CHECK(enabled && invalidates ? sent.invalidations > 0 : sent.invalidations == 0);
        // Longer than the sharer keeps video: given up on.
        const int before = frames;
        proxy.outage = true;
        std::this_thread::sleep_for(1500ms);
        proxy.outage = false;
        const auto lossOf = [](EventLog& log) {
            auto events = log.of(RCS_EVENT_RECOVERY);
            std::erase_if(events, [](const rcs_event& e) { return e.recovery.reason != RCS_RECOVERY_LOSS; });
            return events;
        };
        CHECK(waitFor([&] { return !lossOf(viewEvents).empty() && !lossOf(shareEvents).empty(); }, 10s));
        const auto viewed = lossOf(viewEvents).back();
        std::printf("lost_pictures: the viewer went on from a %s after %.0f ms\n",
                    viewed.recovery.method == RCS_RECOVERY_KEYFRAME ? "keyframe" : "picture",
                    viewed.recovery.frozen_ns / 1e6);
        const auto shared = lossOf(shareEvents).back();
        CHECK(viewed.recovery.frozen_ns >= 1'000'000'000ull && viewed.viewer_id == 0);
        CHECK(shared.recovery.method == viewed.recovery.method && shared.viewer_id == 1 && !shared.recovery.frozen_ns);
        CHECK(viewed.recovery.method == RCS_RECOVERY_KEYFRAME || (enabled && invalidates));
        CHECK(waitFor([&] { return frames >= before + 15; }, 10s));
        ++port;
    }
}

void adaptiveBitrate(const uint8_t (&key)[32]) {
    auto& pattern = rcsdk::test::patternControl();
    pattern.grain = true;
    std::mutex mutex;
    std::vector<rcs_rate_event> rates;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = [&](const rcs_event& event) {
        if (event.type == RCS_EVENT_RATE) {
            std::lock_guard lock(mutex);
            rates.push_back(event.rate);
        }
    };
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47837, key)
                             .setTransport(RCS_TRANSPORT_UDP)
                             .setVideo(640, 360, 30, 6'000'000)
                             .setRateMode(RCS_RATE_DEFAULT, 200'000),
                         shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47837));
    proxy.bitsPerSecond = 2'000'000;
    proxy.delayMs = 10;
    rcsdk::Session view(config(RCS_ROLE_VIEW, proxy.address().port(), key).setTransport(RCS_TRANSPORT_UDP), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    std::this_thread::sleep_for(9s);
    const int settled = frames;
    std::this_thread::sleep_for(3s);
    const auto sent = share.stats<rcs_video_send_stats>();
    const auto network = share.stats<rcs_network_stats>();
    const size_t changes = [&] {
        std::lock_guard lock(mutex);
        return rates.size();
    }();
    std::printf("2 Mbit/s bottleneck: target %u kbit/s, delivered %llu kbit/s, loss %u per mille, queue %u ms, "
                "%d frames in 3 s, %zu rate changes\n",
                sent.bitrate_bps / 1000, (unsigned long long)network.received_bps / 1000, network.loss_permille,
                network.queue_delay_us / 1000, frames - settled, changes);
    CHECK(sent.bitrate_bps < 2'400'000 && sent.bitrate_bps >= 800'000);
    CHECK(frames - settled >= 45); // The picture keeps coming at the lower rate.
    CHECK(sent.size_percent == 100 && sent.limited_ms > 0); // FRAME_RATE: fewer pictures, never smaller ones.
    CHECK(network.round_trip_ns > 0 && std::string(network.path).find("UDP") != std::string::npos);
    CHECK(view.stats<rcs_network_stats>().round_trip_ns > 0);
    {
        // The statistics' bitrate was reported as an event (a newer one may
        // have followed since they were read).
        std::lock_guard lock(mutex);
        CHECK(std::any_of(rates.begin(), rates.end(),
                          [&](const rcs_rate_event& r) { return r.bitrate_bps == sent.bitrate_bps; }));
        CHECK(!rates.empty() && rates.back().round_trip_ms < 1000);
    }
    // The path opens: the bitrate follows (8% per second, faster far below
    // the last overflow).
    proxy.bitsPerSecond = 0;
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().bitrate_bps > 3'500'000; }, 15s));
    pattern.grain = false;
    view.stop();
}

// Degradation SIZE: grain the bitrate cannot carry makes the stream smaller in
// steps (each a new stream the viewer hears of), never below
// min_size_percent; once the path opens and the bitrate grew, it steps back up.
void degradation(const uint8_t (&key)[32]) {
    auto& pattern = rcsdk::test::patternControl();
    pattern.grain = true;
    EventLog viewEvents;
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onEvent = viewEvents.callback();
    viewCallbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
    };
    auto sharing = config(RCS_ROLE_SHARE, 47930, key)
                       .setTransport(RCS_TRANSPORT_UDP)
                       .setVideo(1280, 720, 30, 6'000'000)
                       .setRateMode(RCS_RATE_DEFAULT, 200'000);
    auto& tuning = startBitrate(sharing, 1'000'000).extension<rcs_video_tuning_config>();
    tuning.degradation = RCS_DEGRADE_SIZE;
    tuning.min_size_percent = 50;
    rcsdk::Session share(sharing);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47930));
    proxy.bitsPerSecond = 1'200'000;
    proxy.delayMs = 10;
    rcsdk::Session view(config(RCS_ROLE_VIEW, proxy.address().port(), key).setTransport(RCS_TRANSPORT_UDP),
                        viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    // Two steps down to the floor (75%, then 50%), and no further.
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().size_percent == 50; }, 25s));
    std::this_thread::sleep_for(5s);
    auto sent = share.stats<rcs_video_send_stats>();
    std::printf("Grain at 700 kbit/s: %u%% of the size, %u kbit/s, held back %llu ms\n", sent.size_percent,
                sent.bitrate_bps / 1000, (unsigned long long)sent.limited_ms);
    CHECK(sent.size_percent == 50 && sent.limited_ms > 0);
    const auto sizes = [&] {
        std::vector<uint32_t> widths;
        for (const auto& e : viewEvents.of(RCS_EVENT_STREAM)) {
            if (e.stream.changes & RCS_STREAM_SIZE) {
                widths.push_back(e.stream.info.width);
            }
        }
        return widths;
    };
    CHECK(waitFor([&] { return !sizes().empty() && sizes().back() == 640; }));
    // Pictures come at the smaller size: within 5 s and the time the sharer
    // held them back meanwhile (20 s of it at most). An encoder whose first
    // pictures overshoot the bitrate spends the budget of seconds: Android's
    // software H.264 encoder (the emulator's) makes some 200 KB of each of the
    // first grainy 1280x720 pictures (VideoToolbox: 47 KB of the keyframe).
    const int before = frames;
    const auto start = std::chrono::steady_clock::now();
    uint64_t held = 0, heldSeen = share.stats<rcs_video_send_stats>().limited_ms;
    while (frames < before + 10) {
        const uint64_t now = share.stats<rcs_video_send_stats>().limited_ms; // From 0 again with a new encoder.
        held += now >= heldSeen ? now - heldSeen : now;
        heldSeen = now;
        if (std::chrono::steady_clock::now() - start >
            5s + std::chrono::milliseconds(std::min<uint64_t>(held, 20'000))) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::printf("At the smaller size: %d pictures in %lld ms, held back %llu ms meanwhile\n", frames - before,
                (long long)waited.count(), (unsigned long long)held);
    CHECK(frames >= before + 10);
    // The path opens: the bitrate grows past 1.8 times what it was, and a step back up follows.
    proxy.bitsPerSecond = 0;
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().size_percent == 75; }, 40s));
    CHECK(waitFor([&] { return sizes().back() == 960; }));
    pattern.grain = false;
    // FRAME_RATE again while running: the plan's own size.
    tuning.degradation = RCS_DEGRADE_FRAME_RATE;
    CHECK(share.update(tuning) == RCS_OK);
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().size_percent == 100 && sizes().back() == 1280; }));
}

// A wireless viewer: its radio naps 200 ms every 2 s to save power (what
// came meanwhile arrives at once) and 2% of datagrams are lost on their own.
// Neither is congestion: adaptive bitrate climbs to its maximum all the same
// (it backed off at every nap before), the pictures keep coming, and the
// sound's lost packets come back with the next ones (redundancy).
void wirelessPath(const uint8_t (&key)[32]) {
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
    };
    rcsdk::Session share(soundMarked(startBitrate(config(RCS_ROLE_SHARE, 47839, key)
                                                      .setTransport(RCS_TRANSPORT_UDP)
                                                      .setVideo(640, 360, 30, 6'000'000)
                                                      .setRateMode(RCS_RATE_ADAPTIVE, 500'000),
                                                  1'000'000),
                                     46));
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47839));
    proxy.delayMs = 5;
    proxy.sleepEveryMs = 2000;
    proxy.sleepMs = 200;
    proxy.lossPermille = 20;
    rcsdk::Session view(config(RCS_ROLE_VIEW, proxy.address().port(), key).setTransport(RCS_TRANSPORT_UDP),
                        viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    std::this_thread::sleep_for(12s);
    const int settled = frames;
    std::this_thread::sleep_for(3s);
    const auto sent = share.stats<rcs_video_send_stats>();
    const auto sound = view.stats<rcs_audio_receive_stats>();
    std::printf("wireless (200 ms naps every 2 s, 2%% loss): target %u kbit/s, %d frames in 3 s; sound: %llu "
                "received, %llu lost, %llu recovered\n",
                sent.bitrate_bps / 1000, frames - settled, (unsigned long long)sound.received_packets,
                (unsigned long long)sound.lost_packets, (unsigned long long)sound.recovered_packets);
    CHECK(sent.bitrate_bps >= 5'000'000);
    CHECK(frames - settled >= 60);
    CHECK(sound.recovered_packets > 0 && sound.lost_packets < sound.recovered_packets);
    // Sound marked EF as asked (rcs_network_tuning_config.audio_dscp), every packet of it.
    CHECK(share.stats<rcs_network_stats>().marked_packets >= sound.received_packets);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// The 50th, 95th and 99th percentiles and the most of `values`, as text.
std::string percentiles(std::vector<double> values) {
    if (values.empty()) {
        return "none";
    }
    std::sort(values.begin(), values.end());
    const auto at = [&](double p) { return values[std::min(values.size() - 1, size_t(p * double(values.size())))]; };
    char text[96];
    std::snprintf(text, sizeof(text), "%.1f / %.1f / %.1f / %.1f", at(0.5), at(0.95), at(0.99), values.back());
    return text;
}

// What a sharer's sound and pictures met on a Wi-Fi-like path.
struct AirtimeRun {
    std::vector<double> soundLate, picturesLate; // ms
    uint64_t underruns = 0, concealedMs = 0, lost = 0, pictures = 0;
    uint64_t pathBps = 0;                                      // The median the statistics showed...
    uint32_t targetMs = 0, soundQueueUs = 0, soundWaitUs = 0; // ...and the most.
};

// 1080p60 at 20 Mbit/s (grain the encoder fills it with), sound in 10 ms
// packets, and a keyframe every 9 s (a viewer that lost a picture, a new
// window), further apart than the playback buffer remembers jitter; through
// `proxy` for `measured` after a warm-up and a fresh start of the sound: how
// late each sound packet reached the viewer after the sharer handed it over,
// how late each picture arrived after its capture, what the playback buffer
// did (the viewer plays into a silent device), and what the statistics showed.
AirtimeRun airtimeRun(const uint8_t (&key)[32], uint16_t port, rcsdk::test::ShapingProxy& proxy,
                      uint32_t pathPacingPercent, std::chrono::seconds measured) {
    struct Times final : rcsdk::EncodedOutput {
        std::mutex mutex;
        std::map<uint64_t, uint64_t> sounds; // Sequence -> when.
        std::vector<double> late;            // Pictures: capture to now, ms.
        bool recording = false;
        void video(const rcs_encoded_packet& p) override {
            std::lock_guard lock(mutex);
            if (recording) {
                late.push_back(double(rcsdk::monotonicNs() - p.timestamp_ns) / 1e6);
            }
        }
        void audio(const rcs_encoded_audio& p) override {
            std::lock_guard lock(mutex);
            sounds[p.sequence] = rcsdk::monotonicNs();
        }
    } shared, received;
    auto& pattern = rcsdk::test::patternControl();
    pattern.grain = true;
    auto sharing = config(RCS_ROLE_SHARE, port, key)
                       .setTransport(RCS_TRANSPORT_UDP)
                       .setVideo(1920, 1080, 60, 20'000'000)
                       .setRateMode(RCS_RATE_ADAPTIVE, 2'000'000)
                       .use(rcsdk::encodedOutput(shared));
    startBitrate(sharing, 20'000'000).extension<rcs_video_tuning_config>().path_pacing_percent = pathPacingPercent;
    rcsdk::Session share(sharing);
    rcsdk::Session view(config(RCS_ROLE_VIEW, proxy.address().port(), key)
                            .setTransport(RCS_TRANSPORT_UDP)
                            .setAudioPlayback(true)
                            .use(rcsdk::encodedOutput(received)));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    std::this_thread::sleep_for(6s);
    // The sound again, with a playback buffer that has not seen the connection's start.
    auto audio = rcsdk::defaults<rcs_receive_audio_config>();
    audio.enabled = 0;
    CHECK(view.update(audio) == RCS_OK);
    CHECK(waitFor([&] { return !view.stats<rcs_audio_receive_stats>().active; }));
    audio.enabled = 1;
    CHECK(view.update(audio) == RCS_OK);
    std::this_thread::sleep_for(4s);
    const auto before = view.stats<rcs_audio_receive_stats>();
    const uint64_t picturesBefore = view.stats<rcs_video_receive_stats>().received_packets;
    {
        std::scoped_lock lock(shared.mutex, received.mutex);
        shared.sounds.clear();
        received.sounds.clear();
        received.recording = true;
    }
    AirtimeRun run;
    std::vector<uint64_t> paths;
    for (auto waited = 0ms; waited < measured; waited += 250ms) {
        if (waited % 9s == 1s) {
            CHECK(view.requestKeyframe() == RCS_OK);
        }
        std::this_thread::sleep_for(250ms);
        run.soundQueueUs = std::max(run.soundQueueUs, view.stats<rcs_network_stats>().sound_queue_us);
        run.soundWaitUs = std::max(run.soundWaitUs, share.stats<rcs_network_stats>().sound_wait_us);
        paths.push_back(share.stats<rcs_network_stats>().path_bps);
    }
    std::nth_element(paths.begin(), paths.begin() + ptrdiff_t(paths.size() / 2), paths.end());
    run.pathBps = paths[paths.size() / 2];
    const auto after = view.stats<rcs_audio_receive_stats>();
    run.pictures = view.stats<rcs_video_receive_stats>().received_packets - picturesBefore;
    run.underruns = after.underruns - before.underruns;
    run.concealedMs = after.concealed_ms - before.concealed_ms;
    run.lost = after.lost_packets - before.lost_packets;
    run.targetMs = after.target_ms;
    {
        std::scoped_lock lock(shared.mutex, received.mutex);
        for (const auto& [sequence, arrived] : received.sounds) {
            if (const auto sent = shared.sounds.find(sequence); sent != shared.sounds.end()) {
                run.soundLate.push_back(double(arrived - sent->second) / 1e6);
            }
        }
        run.picturesLate = received.late;
    }
    CHECK(view.stop() == RCS_OK && share.stop() == RCS_OK);
    pattern.grain = false;
    std::printf("  sound handed over to arrival (p50 / p95 / p99 / most, ms): %s\n"
                "  picture captured to arrival: %s\n"
                "  %llu pictures in %lld s; playback: %llu underruns, target %u ms at the end, %llu ms concealed, "
                "%llu lost\n"
                "  statistics: path %llu kbit/s, sound queued on the path up to %.1f ms, waited at the sharer up to "
                "%.1f ms\n",
                percentiles(run.soundLate).c_str(), percentiles(run.picturesLate).c_str(),
                (unsigned long long)run.pictures, (long long)measured.count(), (unsigned long long)run.underruns,
                run.targetMs, (unsigned long long)run.concealedMs, (unsigned long long)run.lost,
                (unsigned long long)run.pathBps / 1000, run.soundQueueUs / 1e3, run.soundWaitUs / 1e3);
    return run;
}

// The share of `values` above `limit`.
double above(const std::vector<double>& values, double limit) {
    return values.empty() ? 0
                          : double(std::count_if(values.begin(), values.end(), [&](double v) { return v > limit; })) /
                                double(values.size());
}

// A Wi-Fi-like path between sharer and viewer: 60 Mbit/s of airtime in
// aggregates of up to 4 ms, each after a backoff of up to 0.4 ms, and before
// one in seven other stations take up to 4 ms first. Each keyframe (700 KB of
// grain) takes about 100 ms of it. Sent at once (path_pacing_percent 0) it
// stands in the bottleneck's queue ahead of the sound, which arrives up to
// 200 ms late; the playback buffer runs dry and keeps 200 ms after. With the
// defaults the sharer holds what the path cannot take at once and the sound
// overtakes it there. The statistics show both: the path's rate, the sound's
// queueing on the path.
void wifiSound(const uint8_t (&key)[32]) {
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47990));
    proxy.bitsPerSecond = 60'000'000;
    proxy.queueMs = 200;
    proxy.delayMs = 2;
    proxy.aggregateUs = 4000;
    proxy.accessUs = 400;
    proxy.busyPermille = 150;
    proxy.busyUs = 4000;
    std::printf("Wi-Fi-like path, path_pacing_percent 0:\n");
    const auto burst = airtimeRun(key, 47990, proxy, 0, 20s);
    std::printf("Wi-Fi-like path, defaults:\n");
    const auto paced = airtimeRun(key, 47990, proxy, rcsdk::defaults<rcs_video_tuning_config>().path_pacing_percent, 20s);
    CHECK(burst.pictures >= 600 && paced.pictures >= 600); // 1200 at 60 FPS; a busy machine encodes fewer.
    if (burst.soundQueueUs > 60'000) { // The keyframes queued sound on the path (an encoder's large enough).
        CHECK(above(burst.soundLate, 80) > 0.003);
        CHECK(above(paced.soundLate, 80) < above(burst.soundLate, 80) / 3);
        CHECK(paced.soundQueueUs < burst.soundQueueUs);
    } else {
        std::printf("this encoder's keyframes queued no sound on the path: nothing to compare\n");
    }
    CHECK(paced.pathBps > 30'000'000 && paced.pathBps < 90'000'000);
}

// rcs_session_update: any extension of the role while stopped, all but what
// is fixed for a run while running; all or nothing.
void updates(const uint8_t (&key)[32]) {
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47835, key));
    auto video = config(RCS_ROLE_SHARE, 47835, key).shareVideo();
    video.width = 320;
    video.height = 180;
    CHECK(share.update(video) == RCS_OK); // Idle: applies to the next start.
    rcs_decoder_config decoder = RCS_DECODER_CONFIG_INIT;
    CHECK(share.update(decoder) == RCS_ERROR_INVALID_ARGUMENT); // Not a sharer's extension.

    std::atomic<uint32_t> width{0};
    std::atomic<int> frames{0};
    std::atomic<rcs_session> viewHandle{nullptr};
    std::atomic<rcs_result> updatedWhileStopping{RCS_ERROR_INTERNAL};
    rcsdk::Callbacks callbacks;
    callbacks.onFrame = [&](const rcs_frame& frame) {
        width = frame.width;
        ++frames;
    };
    // Updates from a callback while stop() waits for that callback.
    callbacks.onEvent = [&](const rcs_event& event) {
        if (event.type == RCS_EVENT_STATE && event.state.value == RCS_STATE_STOPPED) {
            rcs_video_tuning_config queue = RCS_VIDEO_TUNING_CONFIG_INIT;
            updatedWhileStopping = rcs_session_update(viewHandle, &queue.header);
        }
    };
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47835, key), callbacks);
    viewHandle = view.handle();
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 5; }));
    CHECK(width == 320 && view.streamInfo().width == 320);

    // Running: the stream changes without reconnecting.
    video.width = 640;
    video.height = 360;
    CHECK(share.update(video) == RCS_OK);
    CHECK(waitFor([&] { return width == 640 && view.streamInfo().width == 640; }));
    CHECK(share.streamInfo().height == 360 && view.state() == RCS_STATE_RUNNING);

    // A chain with another way to the TURN server, which cannot change while running, changes nothing.
    rcs_ice_servers_config servers = RCS_ICE_SERVERS_CONFIG_INIT;
    servers.turn_transport = RCS_TURN_TCP;
    video.width = 320;
    video.height = 180;
    CHECK(share.update(video, servers) == RCS_ERROR_INVALID_STATE);
    char reason[256] = {};
    CHECK(rcs_get_last_call_error(reason, sizeof(reason)) == RCS_OK && std::strstr(reason, "turn_transport"));
    std::this_thread::sleep_for(200ms);
    CHECK(view.streamInfo().width == 640);
    video.width = 640;
    video.height = 360;
    auto tuning = rcsdk::defaults<rcs_video_tuning_config>();
    tuning.input_boost_ms = 5000;
    CHECK(share.update(video, tuning) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_get_last_call_error(reason, sizeof(reason)) == RCS_OK && std::strstr(reason, "input_boost_ms"));
    CHECK(view.update(video) == RCS_ERROR_INVALID_ARGUMENT); // A sharer's extension.
    tuning.input_boost_ms = 0;

    // The bitrate while running: fixed at once, adaptive again from a new start.
    video.rate_mode = RCS_RATE_FIXED;
    video.bitrate_bps = 1'500'000;
    CHECK(share.update(video) == RCS_OK);
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().bitrate_bps == 1'500'000; }));
    CHECK(waitFor([&] { return share.streamInfo().bitrate_bps == 1'500'000; })); // The stream's restart says so.
    video.rate_mode = RCS_RATE_ADAPTIVE;
    video.bitrate_bps = 3'000'000;
    tuning.start_bitrate_bps = 700'000;
    CHECK(share.update(video, tuning) == RCS_OK);
    CHECK(waitFor([&] {
        const uint32_t bitrate = share.stats<rcs_video_send_stats>().bitrate_bps;
        return bitrate >= 700'000 && bitrate < 1'500'000;
    }));
    CHECK(view.stop() == RCS_OK);
    CHECK(updatedWhileStopping == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; })); // The viewer left.
    CHECK(share.update(servers) == RCS_OK); // Between runs everything changes.
}

// Keyframes on request (the host's, either end) and by interval.
void keyframes(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    std::atomic<int> frames{0};
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47865, key), shareCallbacks);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47865, key), viewCallbacks);
    CHECK(share.requestKeyframe() == RCS_ERROR_INVALID_STATE); // Not running.
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10; }));
    const auto count = [&] { return share.stats<rcs_video_send_stats>().keyframes; };
    std::this_thread::sleep_for(500ms);
    const uint64_t first = count();
    CHECK(first >= 1);
    CHECK(share.requestKeyframe() == RCS_OK);
    CHECK(waitFor([&] { return count() > first; }));
    const uint64_t second = count();
    CHECK(view.requestKeyframe() == RCS_OK); // The viewer's host asks the sharer.
    CHECK(waitFor([&] { return count() > second; }));
    CHECK(shareEvents.of(RCS_EVENT_RECOVERY).size() >= 2);
    CHECK(shareEvents.of(RCS_EVENT_RECOVERY).back().recovery.reason == RCS_RECOVERY_REQUESTED);
    // With an interval: about twice a second, without anyone asking.
    auto encoder = rcsdk::defaults<rcs_video_tuning_config>();
    encoder.keyframe_interval_ms = 500;
    CHECK(share.update(encoder) == RCS_OK);
    std::this_thread::sleep_for(300ms);
    const uint64_t before = count();
    std::this_thread::sleep_for(2s);
    const uint64_t interval = count() - before;
    CHECK(interval >= 3 && interval <= 6);
    encoder.keyframe_interval_ms = 50;
    CHECK(share.update(encoder) == RCS_ERROR_INVALID_ARGUMENT); // 100..60 000.
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// The viewer asks for a stream; the sharer follows within its policy, and both
// change it while running.
void streamChanges(const uint8_t (&key)[32]) {
    std::mutex mutex;
    rcs_stream_info announced = RCS_STREAM_INFO_INIT;
    uint32_t changes = 0; // Since the last check.
    std::atomic<int> streamEvents{0};
    std::atomic<uint32_t> width{0}, height{0};
    rcsdk::Callbacks callbacks;
    callbacks.onFrame = [&](const rcs_frame& frame) {
        width = frame.width;
        height = frame.height;
    };
    callbacks.onEvent = [&](const rcs_event& event) {
        if (event.type == RCS_EVENT_STREAM) {
            std::lock_guard lock(mutex);
            announced = event.stream.info;
            changes |= event.stream.changes;
            ++streamEvents;
        }
    };
    const auto showing = [&](uint32_t w, uint32_t h) {
        return waitFor([&] { return width == w && height == h; }, 8s);
    };
    const auto changed = [&] {
        std::lock_guard lock(mutex);
        return std::exchange(changes, 0u);
    };
    const auto caps = rcsdk::capabilities();
    const uint32_t both = testAcceleration() == RCS_ACCELERATION_HARDWARE
                              ? caps.hardware_encoders & caps.hardware_decoders
                              : caps.software_encoders & caps.software_decoders;
    const bool hevc = both & hevcBit;
    const auto withCodecs = [hevc](rcsdk::SessionConfig c) {
        return hevc ? c.setCodecs({RCS_CODEC_H264, RCS_CODEC_HEVC}) : c.setCodecs({RCS_CODEC_H264});
    };
    rcsdk::test::patternControl().displayWidth = 1280;
    rcsdk::test::patternControl().displayHeight = 720;

    // Asked for at start (and the codec, where both have HEVC); then changed while running.
    const auto sharing = withCodecs(config(RCS_ROLE_SHARE, 47862, key));
    rcsdk::Session share(sharing);
    rcsdk::Session view(
        withCodecs(config(RCS_ROLE_VIEW, 47862, key)).setVideoRequest(800, 450, 0, 0, hevc ? RCS_CODEC_HEVC : 0),
        callbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(showing(800, 450));
    {
        std::lock_guard lock(mutex);
        CHECK(announced.codec == (hevc ? RCS_CODEC_HEVC : RCS_CODEC_H264));
        CHECK(announced.display_width == 1280 && announced.display_height == 720 && announced.display_refresh_hz == 60);
    }
    changed();
    rcs_view_request_config request = RCS_VIEW_REQUEST_CONFIG_INIT;
    request.width = 480;
    request.height = 270;
    request.frames_per_second = 15;
    CHECK(view.update(request) == RCS_OK);
    CHECK(showing(480, 270));
    CHECK(waitFor([&] { return share.streamInfo().frames_per_second == 15; }));
    // The events say what changed: the size and frame rate, and (with HEVC
    // asked for before, none now) the codec, back to the sharer's first.
    uint32_t seen = 0;
    CHECK(waitFor([&] {
        seen |= changed();
        return (seen & RCS_STREAM_SIZE) && (seen & RCS_STREAM_FRAME_RATE);
    }));
    CHECK(bool(seen & RCS_STREAM_CODEC) == hevc && !(seen & RCS_STREAM_DISPLAY));
    // Beyond the display: scaled down to fit it, keeping the aspect ratio.
    request.width = 3200;
    request.height = 1200;
    CHECK(view.update(request) == RCS_OK);
    CHECK(showing(1280, 480));
    // A request the sharer does not follow leaves its own settings.
    auto policy = rcsdk::SessionConfig(sharing).shareVideo();
    policy.follow_requests = 0;
    CHECK(share.update(policy) == RCS_OK);
    CHECK(showing(640, 360));
    // The sharer changes codecs while running: the viewer reopens its decoder.
    if (hevc) {
        rcs_encoder_config codec = RCS_ENCODER_CONFIG_INIT;
        codec.acceleration = testAcceleration();
        codec.codecs[0] = RCS_CODEC_H264;
        CHECK(share.update(codec) == RCS_OK);
        CHECK(waitFor([&] { return view.streamInfo().codec == RCS_CODEC_H264; }, 8s));
        // The viewer learns the sharer's new offer.
        CHECK(waitFor([&] { return view.peerInfo().codecs == h264Bit; }));
        const auto decoded = view.stats<rcs_video_receive_stats>().decoded_frames;
        CHECK(waitFor([&] { return view.stats<rcs_video_receive_stats>().decoded_frames > decoded + 10; }));
    }
    // A codec list the viewer has nothing in common with is refused, and nothing changes.
    rcs_encoder_config av1Only = RCS_ENCODER_CONFIG_INIT;
    av1Only.acceleration = testAcceleration();
    av1Only.codecs[0] = RCS_CODEC_AV1;
    CHECK(share.update(av1Only) == RCS_ERROR_UNSUPPORTED);
    CHECK(view.state() == RCS_STATE_RUNNING && streamEvents >= 4);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// Pictures beyond 4096x2160: a 5120x2880 HEVC stream (this Mac's main
// display's size) is planned and decoded at that size. H.264 takes no such
// picture: a sharer's own size beyond it is refused at start and by an update,
// and on both ends of a connection that negotiates H.264; a viewer's request
// beyond it is fitted.
void largePictures(const uint8_t (&key)[32]) {
    auto& pattern = rcsdk::test::patternControl();
    pattern.displayWidth = 5120;
    pattern.displayHeight = 2880;
    const auto fiveK = [](rcsdk::SessionConfig c) {
        return c.setVideo(5120, 2880, 10, 20'000'000);
    };
    {
        rcsdk::Session share(fiveK(config(RCS_ROLE_SHARE, 47914, key)));
        CHECK(share.start() == RCS_ERROR_UNSUPPORTED);
        CHECK(rcsdk::lastCallError().find("5120x2880 is more than H.264 takes: 4096 a side and 36 864 macroblocks") !=
              std::string::npos);
    }
    {
        EventLog shareEvents;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        rcsdk::Session share(fiveK(config(RCS_ROLE_SHARE, 47915, key))
                                 .setCodecs({RCS_CODEC_H264, RCS_CODEC_HEVC})
                                 .setKeepListening(true),
                             shareCallbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47915, key).setCodecs({RCS_CODEC_H264}));
        CHECK(share.start() == RCS_OK); // HEVC takes it...
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK); // ...but the viewer decodes H.264 only.
        CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }));
        CHECK(view.failure().result == RCS_ERROR_UNSUPPORTED &&
              view.failure().message.find("5120x2880 is more than H.264 takes") != std::string::npos);
        CHECK(waitFor([&] {
            const auto states = shareEvents.of(RCS_EVENT_STATE);
            return states.size() >= 2 && states.back().state.value == RCS_STATE_LISTENING &&
                   states.back().state.result == RCS_ERROR_UNSUPPORTED;
        }));
        // Never RUNNING: the refusal is part of the negotiation.
        for (const auto& state : shareEvents.of(RCS_EVENT_STATE)) {
            CHECK(state.state.value != RCS_STATE_RUNNING);
        }
    }
    {
        // H.264 comes first in both lists but cannot take 5120x2880: the first codec both offer that does.
        std::atomic<int> pictures{0};
        rcsdk::Callbacks callbacks;
        callbacks.onGpuFrame = [&](const rcsdk::GpuFrame&) { ++pictures; };
        rcsdk::Session share(fiveK(config(RCS_ROLE_SHARE, 47917, key)).setCodecs({RCS_CODEC_H264, RCS_CODEC_HEVC}));
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47917, key).setCodecs({RCS_CODEC_H264, RCS_CODEC_HEVC}),
                            callbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return pictures >= 2; }, 15s));
        const auto stream = view.streamInfo();
        CHECK(stream.codec == RCS_CODEC_HEVC && stream.width == 5120 && stream.height == 2880);
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    std::atomic<int> frames{0};
    std::atomic<uint32_t> width{0}, height{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onGpuFrame = [&](const rcsdk::GpuFrame& frame) {
        const auto info = frame.info();
        width = info.width;
        height = info.height;
        ++frames;
    };
    rcsdk::Session share(fiveK(config(RCS_ROLE_SHARE, 47916, key)).setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264}));
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47916, key).setCodecs({RCS_CODEC_H264, RCS_CODEC_HEVC}), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 5; }, 15s));
    CHECK(width == 5120 && height == 2880);
    const auto stream = view.streamInfo();
    CHECK(stream.codec == RCS_CODEC_HEVC && stream.width == 5120 && stream.height == 2880 &&
          stream.display_width == 5120 && stream.display_height == 2880);
    // Each end knows how large a picture the other takes in each codec it offers.
    const auto sharer = view.peerInfo(), viewer = share.peerInfo();
    CHECK(sharer.codec_limits[0].codec == RCS_CODEC_HEVC && sharer.codec_limits[0].max_width >= 5120 &&
          sharer.codec_limits[1].codec == RCS_CODEC_H264 && sharer.codec_limits[1].max_width <= 4096);
    CHECK(viewer.codec_limits[0].codec == RCS_CODEC_H264 && viewer.codec_limits[1].codec == RCS_CODEC_HEVC &&
          viewer.codec_limits[1].max_width >= 5120 && viewer.codec_limits[2].codec == 0);
    // A codec list none of whose codecs takes the stream's size is refused; the stream goes on.
    rcs_encoder_config h264Only = RCS_ENCODER_CONFIG_INIT;
    h264Only.acceleration = testAcceleration();
    CHECK(share.update(h264Only) == RCS_ERROR_UNSUPPORTED);
    CHECK(rcsdk::lastCallError().find("more than H.264 takes") != std::string::npos);
    // A request beyond what its codec takes is fitted, keeping the aspect ratio.
    rcs_view_request_config request = RCS_VIEW_REQUEST_CONFIG_INIT;
    request.width = 5120;
    request.height = 2880;
    request.codec = RCS_CODEC_H264;
    CHECK(view.update(request) == RCS_OK);
    CHECK(waitFor([&] { return width == 4096 && height == 2304; }, 15s));
    CHECK(view.streamInfo().codec == RCS_CODEC_H264 && view.state() == RCS_STATE_RUNNING);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// The pointer apart from the picture: shapes once each, positions as it moves.
void cursorStream(const uint8_t (&key)[32]) {
    auto& pattern = rcsdk::test::patternControl();
    std::mutex mutex;
    struct Seen {
        uint32_t x = 0, shapeId = 0, shapes = 0, events = 0;
        rcs_cursor_type type = 0;
        uint32_t hotspotX = 99, width = 0;
        uint8_t blue = 0;
    } seen;
    rcsdk::Callbacks callbacks;
    callbacks.onFrame = [](const rcs_frame&) {};
    callbacks.onEvent = [&](const rcs_event& event) {
        if (event.type != RCS_EVENT_CURSOR) {
            return;
        }
        std::lock_guard lock(mutex);
        CHECK(event.cursor.visible);
        seen.x = event.cursor.x;
        seen.shapeId = event.cursor.shape_id;
        ++seen.events;
        if (const rcs_cursor_shape* shape = event.cursor.shape) {
            CHECK(shape->id == event.cursor.shape_id && shape->height == 16 && shape->stride_bytes == 64);
            ++seen.shapes;
            seen.type = shape->type;
            seen.hotspotX = shape->hotspot_x;
            seen.width = shape->width;
            seen.blue = shape->pixels[15 * 64]; // Bottom left: inside the triangle.
        }
    };
    const auto observe = [&](auto check) {
        return waitFor([&] {
            std::lock_guard lock(mutex);
            return check(seen);
        });
    };
    pattern.cursorX = 1000;
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47863, key));
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47863, key).setCursorMode(RCS_CURSOR_SEPARATE), callbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(observe([](const Seen& s) {
        return s.shapes == 1 && s.type == RCS_CURSOR_ARROW && s.width == 16 && s.hotspotX == 0 && s.blue == 255 &&
               s.x == 1000;
    }));
    CHECK(!pattern.cursorInVideo);
    const uint32_t arrow = seen.shapeId;
    pattern.cursorX = 30000; // Moves: positions only.
    CHECK(observe([](const Seen& s) { return s.x == 30000 && s.shapes == 1; }));
    pattern.cursorImage = 2; // A new image: reported with its first position.
    CHECK(observe([&](const Seen& s) {
        return s.shapes == 2 && s.shapeId != arrow && s.type == RCS_CURSOR_HAND && s.hotspotX == 6 && s.blue == 0;
    }));
    pattern.cursorImage = 1; // Back: the host has that image already.
    CHECK(observe([&](const Seen& s) { return s.shapeId == arrow; }));
    CHECK(seen.shapes == 2);
    // Into the picture again while running, and out.
    rcs_view_request_config cursor = RCS_VIEW_REQUEST_CONFIG_INIT; // RCS_CURSOR_IN_VIDEO
    CHECK(view.update(cursor) == RCS_OK);
    CHECK(waitFor([&] { return pattern.cursorInVideo.load(); }));
    cursor.cursor = RCS_CURSOR_SEPARATE;
    CHECK(view.update(cursor) == RCS_OK);
    CHECK(waitFor([&] { return !pattern.cursorInVideo; }));
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// Streams with the given offers; returns the codec both ends report, or 0
// when both sessions failed for want of a common codec.
rcs_codec negotiate(const uint8_t (&key)[32], uint16_t port, std::initializer_list<rcs_codec> shareCodecs,
                    std::initializer_list<rcs_codec> viewCodecs) {
    std::atomic<int> frames{0};
    rcsdk::Session share(config(RCS_ROLE_SHARE, port, key).setCodecs(shareCodecs));
    rcsdk::Callbacks callbacks;
    callbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
    };
    rcsdk::Session view(config(RCS_ROLE_VIEW, port, key).setCodecs(viewCodecs), callbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10 || view.state() == RCS_STATE_FAILED; }));
    // Each end learns what the other offered, whether or not they agree.
    uint32_t shareMask = 0, viewMask = 0;
    for (const rcs_codec codec : shareCodecs) {
        shareMask |= 1u << codec;
    }
    for (const rcs_codec codec : viewCodecs) {
        viewMask |= 1u << codec;
    }
    CHECK(view.peerInfo().codecs == shareMask);
    if (view.state() == RCS_STATE_FAILED) {
        CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }));
        CHECK(share.peerInfo().codecs == viewMask);
        CHECK(view.failure().result == RCS_ERROR_UNSUPPORTED && share.failure().result == RCS_ERROR_UNSUPPORTED);
        CHECK(view.failure().message.find("HEVC") != std::string::npos);
        CHECK(share.failure().message.find("H.264") != std::string::npos);
        return 0;
    }
    CHECK(share.peerInfo().codecs == viewMask);
    const rcs_codec codec = view.streamInfo().codec;
    CHECK(share.streamInfo().codec == codec && view.stats<rcs_video_receive_stats>().decoded_frames >= 10);
    return codec;
}

void negotiation(const uint8_t (&key)[32]) {
    CHECK(negotiate(key, 47826, {RCS_CODEC_H264}, {RCS_CODEC_H264}) == RCS_CODEC_H264);
    const auto caps = rcsdk::capabilities();
    const bool hardware = testAcceleration() == RCS_ACCELERATION_HARDWARE;
    const uint32_t both =
        hardware ? caps.hardware_encoders & caps.hardware_decoders : caps.software_encoders & caps.software_decoders;
    if (!(both & hevcBit)) {
        std::printf("HEVC is not available with this acceleration; HEVC negotiation skipped\n");
        return;
    }
    // The sharer's preference wins when the viewer accepts it...
    CHECK(negotiate(key, 47827, {RCS_CODEC_HEVC, RCS_CODEC_H264}, {RCS_CODEC_H264, RCS_CODEC_HEVC}) == RCS_CODEC_HEVC);
    // ...otherwise its next codec the viewer accepts, because the host listed it.
    CHECK(negotiate(key, 47828, {RCS_CODEC_HEVC, RCS_CODEC_H264}, {RCS_CODEC_H264}) == RCS_CODEC_H264);
    // Without a common codec both ends fail and say why; nothing is substituted.
    CHECK(negotiate(key, 47829, {RCS_CODEC_HEVC}, {RCS_CODEC_H264}) == 0);
}

// The sharer's tone (support/test_tone.hpp) reaches the viewer with each codec
// over TCP and UDP (a codec this system lacks, AAC on Linux, is refused at
// start); audio negotiates like video; a viewer with audio off is no failure.
void audioStreaming(const uint8_t (&key)[32]) {
    uint16_t port = 47850;
    for (const rcs_transport transport : {RCS_TRANSPORT_TCP, RCS_TRANSPORT_UDP}) {
        for (const rcs_audio_codec codec : {RCS_AUDIO_OPUS, RCS_AUDIO_AAC, RCS_AUDIO_PCM}) {
            if (!(rcsdk::capabilities().audio_encoders & (1u << codec))) {
                rcsdk::Session refused(config(RCS_ROLE_SHARE, port, key).setAudioCodecs({codec}));
                CHECK(refused.start() == RCS_ERROR_UNSUPPORTED &&
                      refused.failure().message.find("RCS_EXT_SEND_AUDIO: this system encodes none") !=
                          std::string::npos);
                continue;
            }
            std::mutex mutex;
            std::vector<float> received; // Left channel.
            EventLog viewEvents, shareEvents;
            rcsdk::Callbacks viewCallbacks, shareCallbacks;
            viewCallbacks.onEvent = viewEvents.callback();
            shareCallbacks.onEvent = shareEvents.callback();
            viewCallbacks.onAudio = [&](const rcs_audio_frame& frame) {
                CHECK(frame.sample_rate == 48000 && frame.channels == 2 && frame.frames > 0);
                std::lock_guard lock(mutex);
                for (uint32_t i = 0; i < frame.frames; ++i) {
                    received.push_back(frame.samples[i * 2]);
                }
            };
            ++port;
            rcsdk::Session share(config(RCS_ROLE_SHARE, port, key).setTransport(transport).setAudioCodecs({codec}),
                                 shareCallbacks);
            rcsdk::Session view(config(RCS_ROLE_VIEW, port, key).setTransport(transport), viewCallbacks);
            CHECK(share.start() == RCS_OK);
            CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
            CHECK(view.start() == RCS_OK);
            CHECK(waitFor([&] {
                std::lock_guard lock(mutex);
                return received.size() >= 48000;
            }));
            std::vector<float> tail;
            {
                std::lock_guard lock(mutex);
                tail.assign(received.end() - 24000, received.end());
            }
            const auto measured = rcsdk::test::measureTone(tail);
            std::printf("audio %s over %s: %.1f Hz, RMS %.3f\n", codec == RCS_AUDIO_OPUS  ? "Opus"
                                                                   : codec == RCS_AUDIO_AAC ? "AAC"
                                                                                            : "PCM",
                        transport == RCS_TRANSPORT_TCP ? "TCP" : "UDP", measured.hz, measured.rms);
            CHECK(std::abs(measured.hz - rcsdk::test::toneHz) < 10 && std::abs(measured.rms - 0.354) < 0.05);
            CHECK(view.streamInfo().audio_codec == codec && share.streamInfo().audio_codec == codec);
            CHECK(view.peerInfo().send_audio_codecs == (1u << codec));
            const auto sent = share.stats<rcs_audio_send_stats>();
            const auto got = view.stats<rcs_audio_receive_stats>();
            CHECK(sent.active && sent.codec == codec && sent.encoded_packets > 0 && sent.encoder[0]);
            CHECK(got.active && got.codec == codec && got.received_packets > 0 && got.decoder[0] && !got.device[0]);
            CHECK(got.lost_packets == 0);
            const auto active = viewEvents.of(RCS_EVENT_AUDIO);
            CHECK(!active.empty() && active.back().audio.active && active.back().audio.codec == codec);
            CHECK(view.stop() == RCS_OK && share.stop() == RCS_OK);
        }
    }

    // No common audio codec: both ends fail and say what the other offered.
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47860, key).setAudioCodecs({RCS_AUDIO_OPUS}));
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47860, key).setAudioCodecs({RCS_AUDIO_PCM}));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED && share.state() == RCS_STATE_FAILED; }));
    CHECK(view.failure().result == RCS_ERROR_UNSUPPORTED && share.failure().result == RCS_ERROR_UNSUPPORTED);
    CHECK(view.peerInfo().send_audio_codecs == (1u << RCS_AUDIO_OPUS));
    CHECK(view.failure().message.find("Opus") != std::string::npos);

    // A viewer with audio off: video runs; the sharer reports why there is no sound.
    std::atomic<int> frames{0};
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    rcsdk::Session quietShare(config(RCS_ROLE_SHARE, 47861, key), shareCallbacks);
    rcsdk::Session quietView(config(RCS_ROLE_VIEW, 47861, key).setAudioEnabled(false), viewCallbacks);
    CHECK(quietShare.start() == RCS_OK);
    CHECK(waitFor([&] { return quietShare.state() == RCS_STATE_LISTENING; }));
    CHECK(quietView.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10; }));
    const auto events = shareEvents.of(RCS_EVENT_AUDIO);
    CHECK(events.size() == 1 && !events[0].audio.active && events[0].audio.result == RCS_OK);
    CHECK(std::string(events[0].audio.message) == "The viewer receives no sound");
    // Offered all the same, so the viewer can turn sound on later.
    CHECK(quietShare.streamInfo().audio_codec == 0 && quietShare.peerInfo().receive_audio_codecs != 0);
    CHECK(!quietShare.peerInfo().receives_audio);
}

// Sound changes while running: an end turns it off and on, the viewer asks
// for another codec; video goes on throughout.
void audioChanges(const uint8_t (&key)[32]) {
    std::atomic<uint64_t> samples{0};
    EventLog viewEvents, shareEvents;
    rcsdk::Callbacks viewCallbacks, shareCallbacks;
    viewCallbacks.onEvent = viewEvents.callback();
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onAudio = [&](const rcs_audio_frame& frame) { samples += frame.frames; };
    const auto flowing = [&] {
        const uint64_t before = samples;
        return waitFor([&] { return samples > before + 24000; });
    };
    const auto silent = [&] {
        std::this_thread::sleep_for(300ms); // Packets already on their way.
        const uint64_t before = samples;
        std::this_thread::sleep_for(500ms);
        return samples == before;
    };
    const auto lastAudio = [](EventLog& log) {
        const auto events = log.of(RCS_EVENT_AUDIO);
        return events.empty() ? rcs_event{} : events.back();
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47864, key), shareCallbacks);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47864, key), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(flowing() && view.streamInfo().audio_codec == RCS_AUDIO_OPUS);

    // The sharer turns sound off, then on.
    rcs_send_audio_config audio = RCS_SEND_AUDIO_CONFIG_INIT;
    audio.enabled = 0;
    CHECK(share.update(audio) == RCS_OK);
    CHECK(waitFor([&] { return !view.peerInfo().sends_audio; })); // Its offer says so.
    CHECK(waitFor([&] { return !lastAudio(viewEvents).audio.active; }));
    CHECK(std::string(lastAudio(viewEvents).audio.message) == "The sharer sends no sound");
    CHECK(silent() && view.streamInfo().audio_codec == 0);
    audio.enabled = 1;
    CHECK(share.update(audio) == RCS_OK);
    CHECK(flowing());

    // The viewer asks for PCM: the sound restarts with it.
    rcs_view_request_config request = RCS_VIEW_REQUEST_CONFIG_INIT;
    request.audio_codec = RCS_AUDIO_PCM;
    CHECK(view.update(request) == RCS_OK);
    CHECK(waitFor([&] { return view.streamInfo().audio_codec == RCS_AUDIO_PCM; }));
    CHECK(flowing() && share.stats<rcs_audio_send_stats>().codec == RCS_AUDIO_PCM);

    // The viewer turns sound off: the sharer stops sending, and says why.
    rcs_receive_audio_config receiveAudio = RCS_RECEIVE_AUDIO_CONFIG_INIT;
    receiveAudio.playback = 0;
    receiveAudio.enabled = 0;
    CHECK(view.update(receiveAudio) == RCS_OK);
    CHECK(waitFor([&] { return std::string(lastAudio(shareEvents).audio.message) == "The viewer receives no sound"; }));
    const auto sent = share.stats<rcs_audio_send_stats>().encoded_packets;
    CHECK(silent() && share.stats<rcs_audio_send_stats>().encoded_packets == sent);
    receiveAudio.enabled = 1;
    CHECK(view.update(receiveAudio) == RCS_OK);
    CHECK(flowing());

    // The viewer offers other codecs: the sharer follows. (The request names
    // PCM still, so it must stay listed.)
    request.audio_codec = 0;
    CHECK(view.update(request) == RCS_OK);
    receiveAudio.codecs[0] = RCS_AUDIO_OPUS;
    receiveAudio.codecs[1] = receiveAudio.codecs[2] = 0;
    CHECK(view.update(receiveAudio) == RCS_OK);
    CHECK(waitFor([&] { return share.peerInfo().receive_audio_codecs == (1u << RCS_AUDIO_OPUS); }));
    CHECK(waitFor([&] { return view.streamInfo().audio_codec == RCS_AUDIO_OPUS; }));
    CHECK(flowing());
    // A sharer's must keep one the viewer has.
    rcs_send_audio_config onlyAac = RCS_SEND_AUDIO_CONFIG_INIT;
    onlyAac.codecs[0] = RCS_AUDIO_AAC;
    onlyAac.codecs[1] = onlyAac.codecs[2] = 0;
    CHECK(share.update(onlyAac) == RCS_ERROR_UNSUPPORTED); // The viewer offers Opus only now.
    CHECK(view.state() == RCS_STATE_RUNNING && view.stats<rcs_video_receive_stats>().decoded_frames > 30);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

#ifdef __APPLE__
// An IOSurface-backed pixel buffer of one colour: BGRA8 (b, g, r) or NV12 (y, cb, cr).
CVPixelBufferRef solidPixelBuffer(OSType format, uint32_t width, uint32_t height, uint8_t a, uint8_t b, uint8_t c) {
    CFDictionaryRef surface = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void* values[] = {surface};
    CFDictionaryRef attributes =
        CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef buffer = nullptr;
    CHECK(CVPixelBufferCreate(nullptr, width, height, format, attributes, &buffer) == kCVReturnSuccess);
    CFRelease(attributes);
    CFRelease(surface);
    CVPixelBufferLockBaseAddress(buffer, 0);
    if (format == kCVPixelFormatType_32BGRA) {
        auto* row = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        for (uint32_t y = 0; y < height; ++y, row += CVPixelBufferGetBytesPerRow(buffer)) {
            for (uint32_t x = 0; x < width; ++x) {
                row[x * 4] = a;
                row[x * 4 + 1] = b;
                row[x * 4 + 2] = c;
                row[x * 4 + 3] = 255;
            }
        }
    } else {
        for (size_t plane = 0; plane < 2; ++plane) {
            auto* row = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, plane));
            const size_t rows = CVPixelBufferGetHeightOfPlane(buffer, plane);
            for (size_t y = 0; y < rows; ++y, row += CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)) {
                for (size_t x = 0; x < CVPixelBufferGetWidthOfPlane(buffer, plane); ++x) {
                    if (plane == 0) {
                        row[x] = a;
                    } else {
                        row[x * 2] = b;
                        row[x * 2 + 1] = c;
                    }
                }
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    return buffer;
}

// A BGRA8 picture whose top half is one colour and the bottom another.
CVPixelBufferRef halvesPixelBuffer(uint32_t width, uint32_t height, const uint8_t (&top)[3],
                                   const uint8_t (&bottom)[3]) {
    CVPixelBufferRef buffer = solidPixelBuffer(kCVPixelFormatType_32BGRA, width, height, 0, 0, 0);
    CVPixelBufferLockBaseAddress(buffer, 0);
    auto* row = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    for (uint32_t y = 0; y < height; ++y, row += CVPixelBufferGetBytesPerRow(buffer)) {
        const auto& colour = y < height / 2 ? top : bottom;
        for (uint32_t x = 0; x < width; ++x) {
            std::memcpy(row + x * 4, colour, 3);
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    return buffer;
}

// A sample buffer around a picture, lying as ReplayKit's attachment says (0: none).
CMSampleBufferRef pictureSample(CVPixelBufferRef image, int32_t orientation) {
    CMVideoFormatDescriptionRef format = nullptr;
    CHECK(CMVideoFormatDescriptionCreateForImageBuffer(nullptr, image, &format) == noErr);
    const CMSampleTimingInfo timing{CMTimeMake(1, 30), CMTimeMake(0, 30), kCMTimeInvalid};
    CMSampleBufferRef sample = nullptr;
    CHECK(CMSampleBufferCreateReadyWithImageBuffer(nullptr, image, format, &timing, &sample) == noErr);
    CFRelease(format);
    if (orientation) {
        CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &orientation);
        CMSetAttachment(sample, CFSTR("RPVideoSampleOrientationKey"), number, kCMAttachmentMode_ShouldPropagate);
        CFRelease(number);
    }
    return sample;
}

// 10 ms of a 1 kHz tone as ReplayKit gives an application's sound: 44.1 kHz,
// 16-bit big-endian stereo; or, `microphone`, of 1.5 kHz as it gives the
// microphone: 48 kHz, 16-bit mono. `frame` counts on across buffers.
CMSampleBufferRef toneSample(uint64_t& frame, bool microphone = false) {
    const int rate = microphone ? 48000 : 44100, frames = rate / 100, channels = microphone ? 1 : 2;
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked |
                          (microphone ? 0 : kAudioFormatFlagIsBigEndian);
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 16;
    format.mBytesPerFrame = 2 * channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = 2 * channels;
    CMAudioFormatDescriptionRef description = nullptr;
    CHECK(CMAudioFormatDescriptionCreate(nullptr, &format, 0, nullptr, 0, nullptr, nullptr, &description) == noErr);
    std::vector<uint16_t> samples(size_t(frames) * channels);
    for (int i = 0; i < frames; ++i) {
        const double hz = microphone ? 1500 : 1000;
        const auto value = uint16_t(int16_t(8000 * std::sin(2 * 3.14159265 * hz * double(frame + i) / rate)));
        for (int c = 0; c < channels; ++c) {
            samples[size_t(i) * channels + c] = microphone ? value : OSSwapHostToBigInt16(value);
        }
    }
    const size_t bytes = samples.size() * sizeof(uint16_t);
    CMBlockBufferRef block = nullptr;
    CHECK(CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, bytes, kCFAllocatorDefault, nullptr, 0, bytes, 0,
                                             &block) == noErr);
    CHECK(CMBlockBufferReplaceDataBytes(samples.data(), block, 0, bytes) == noErr);
    CMSampleBufferRef sample = nullptr;
    CHECK(CMAudioSampleBufferCreateReadyWithPacketDescriptions(nullptr, block, description, frames,
                                                               CMTimeMake(int64_t(frame), rate), nullptr,
                                                               &sample) == noErr);
    CFRelease(block);
    CFRelease(description);
    frame += frames;
    return sample;
}

// The amplitude of `hz` in `mono` (Goertzel).
double toneLevel(const std::vector<float>& mono, double hz) {
    const double coefficient = 2 * std::cos(2 * 3.14159265358979 * hz / 48000);
    double s1 = 0, s2 = 0;
    for (const float sample : mono) {
        const double s = sample + coefficient * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coefficient * s1 * s2)) / double(mono.size());
}
#endif

// Pictures and sound as Core Media hands them over (rcs_session_push_sample_buffer,
// e.g. from ReplayKit): a picture lying sideways arrives upright; one of
// another shape is fitted into the stream, the rest black, its place and size
// announced; the applications' sound at 44.1 kHz in big-endian 16-bit and the
// microphone at 48 kHz in 16-bit mono arrive converted and mixed, and the
// microphone at volume 0 is left out while the run goes on. Mirrored and
// undefined orientations and buffers of the wrong kind are refused; systems
// without sample buffers say so.
void sampleBuffers(const uint8_t (&key)[32]) {
    auto sharing = config(RCS_ROLE_SHARE, 47982, key);
    sharing.setCaptureSource(RCS_CAPTURE_HOST).setAudioSource(RCS_AUDIO_HOST).setAudioCodecs({RCS_AUDIO_PCM});
    rcsdk::Session share(sharing);
    rcs_sample_buffer_info info = RCS_SAMPLE_BUFFER_INFO_INIT;
#ifndef __APPLE__
    int notABuffer = 0;
    CHECK(rcs_session_push_sample_buffer(share.handle(), &notABuffer, &info) == RCS_ERROR_UNSUPPORTED);
#else
    struct Seen {
        uint8_t left[4], right[4], top[4], bottom[4], edge[4];
    };
    std::mutex mutex;
    Seen seen{};
    int frames = 0;
    double energy = 0;
    uint64_t sound = 0;
    std::vector<float> heard; // The latest half second, left channel.
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame& frame) {
        const auto at = [&](uint32_t x, uint32_t y, uint8_t(&out)[4]) {
            std::memcpy(out, frame.data + size_t(y) * frame.stride_bytes + size_t(x) * 4, 4);
        };
        std::lock_guard lock(mutex);
        at(frame.width / 4, frame.height / 2, seen.left);
        at(frame.width * 3 / 4, frame.height / 2, seen.right);
        at(frame.width / 2, frame.height / 4, seen.top);
        at(frame.width / 2, frame.height * 3 / 4, seen.bottom);
        at(8, frame.height / 2, seen.edge);
        ++frames;
    };
    viewCallbacks.onAudio = [&](const rcs_audio_frame& frame) {
        std::lock_guard lock(mutex);
        for (uint32_t i = 0; i < frame.frames * frame.channels; ++i) {
            energy += double(frame.samples[i]) * frame.samples[i];
        }
        sound += frame.frames;
        for (uint32_t i = 0; i < frame.frames; ++i) {
            heard.push_back(frame.samples[i * frame.channels]);
        }
        if (heard.size() > 24'000) {
            heard.erase(heard.begin(), heard.end() - 24'000);
        }
    };
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47982, key), viewCallbacks);
    const auto is = [](const uint8_t(&pixel)[4], int b, int g, int r) {
        return std::abs(pixel[0] - b) < 32 && std::abs(pixel[1] - g) < 32 && std::abs(pixel[2] - r) < 32;
    };
    constexpr uint8_t red[3] = {0, 0, 255}, blue[3] = {255, 0, 0};
    // Portrait 180x320, red above and blue below. Lying as 6 (it stands once
    // turned a quarter clockwise) it stands as 320x180 with red on the right.
    CVPixelBufferRef portrait = halvesPixelBuffer(180, 320, red, blue);
    CMSampleBufferRef sideways = pictureSample(portrait, 6);
    CMSampleBufferRef standing = pictureSample(portrait, 0); // No attachment: as it is.
    uint64_t toneFrame = 0;
    CMSampleBufferRef tone = toneSample(toneFrame);
    CHECK(rcsdk::pushSampleBuffer(share, sideways, RCS_SAMPLE_BUFFER_VIDEO) == RCS_ERROR_INVALID_STATE); // Not running.
    CHECK(rcsdk::pushSampleBuffer(view, sideways, RCS_SAMPLE_BUFFER_VIDEO) == RCS_ERROR_UNSUPPORTED);    // Not a sharer.
    info.type = 4;
    CHECK(rcs_session_push_sample_buffer(share.handle(), sideways, &info) == RCS_ERROR_INVALID_ARGUMENT);
    info.type = RCS_SAMPLE_BUFFER_AUDIO;
    info.orientation = 6;
    CHECK(rcs_session_push_sample_buffer(share.handle(), tone, &info) == RCS_ERROR_INVALID_ARGUMENT);
    info.type = RCS_SAMPLE_BUFFER_MICROPHONE;
    CHECK(rcs_session_push_sample_buffer(share.handle(), tone, &info) == RCS_ERROR_INVALID_ARGUMENT);
    std::atomic<CMSampleBufferRef> pushing{sideways};
    std::atomic<bool> running{true};
    std::atomic<int> refused{0};
    std::thread host([&] { // A picture every 33 ms, 10 ms of each sound every 10 ms.
        auto nextPicture = std::chrono::steady_clock::now();
        uint64_t next = 0, nextMicrophone = 0;
        while (running) {
            for (const bool microphone : {false, true}) {
                CMSampleBufferRef piece = toneSample(microphone ? nextMicrophone : next, microphone);
                const rcs_result pushed = rcsdk::pushSampleBuffer(
                    share, piece, microphone ? RCS_SAMPLE_BUFFER_MICROPHONE : RCS_SAMPLE_BUFFER_AUDIO);
                CFRelease(piece);
                refused += pushed != RCS_OK && pushed != RCS_ERROR_INVALID_STATE;
            }
            if (std::chrono::steady_clock::now() >= nextPicture) {
                const rcs_result shown = rcsdk::pushSampleBuffer(share, pushing.load(), RCS_SAMPLE_BUFFER_VIDEO);
                refused += shown != RCS_OK && shown != RCS_ERROR_INVALID_STATE; // Before the stream starts.
                nextPicture += 33ms;
            }
            std::this_thread::sleep_for(10ms);
        }
    });
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    // Upright, 320x180 filling the 640x360 stream.
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return frames > 5 && is(seen.right, 0, 0, 255) && is(seen.left, 255, 0, 0);
    }));
    auto stream = view.streamInfo();
    CHECK(stream.display_width == 320 && stream.display_height == 180);
    CHECK(stream.content_x == 0 && stream.content_width == 640 && stream.content_height == 360);
    // Standing as it is, 180x320: fitted into 202x360 in the middle, the rest black.
    pushing = standing;
    CHECK(waitFor([&] {
        stream = view.streamInfo();
        return stream.display_width == 180 && stream.display_height == 320;
    }));
    CHECK(stream.content_x == 218 && stream.content_y == 0 && stream.content_width == 202 &&
          stream.content_height == 360);
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return is(seen.top, 0, 0, 255) && is(seen.bottom, 255, 0, 0) && is(seen.edge, 0, 0, 0);
    }));
    // Both sounds, converted to 48 kHz stereo and mixed (8000 / 32767 each).
    const auto hears = [&](double applications, double microphone) {
        std::lock_guard lock(mutex);
        return heard.size() == 24'000 && std::abs(toneLevel(heard, 1000) - applications) < 0.02 &&
               std::abs(toneLevel(heard, 1500) - microphone) < 0.02;
    };
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return sound > 24'000 && energy > 1;
    }));
    CHECK(waitFor([&] { return hears(0.244, 0.244); }));
    // The microphone left out, at once.
    sharing.setSampleBufferVolumes(100, 0);
    CHECK(share.update(sharing) == RCS_OK);
    {
        std::lock_guard lock(mutex);
        heard.clear();
    }
    CHECK(waitFor([&] { return hears(0.244, 0); }));
    // Mirrored, undefined, and a sound for a picture.
    info.type = RCS_SAMPLE_BUFFER_VIDEO;
    info.orientation = 2;
    CHECK(rcs_session_push_sample_buffer(share.handle(), sideways, &info) == RCS_ERROR_UNSUPPORTED);
    CHECK(rcsdk::lastCallError().find("never mirrored") != std::string::npos);
    info.orientation = 9;
    CHECK(rcs_session_push_sample_buffer(share.handle(), sideways, &info) == RCS_ERROR_INVALID_ARGUMENT);
    info.orientation = 0;
    CHECK(rcs_session_push_sample_buffer(share.handle(), tone, &info) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcsdk::lastCallError().find("no picture") != std::string::npos);
    running = false;
    host.join();
    CHECK(refused == 0);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    CFRelease(tone);
    CFRelease(standing);
    CFRelease(sideways);
    CVPixelBufferRelease(portrait);
#endif
}

#ifdef __ANDROID__
// An RGBA8 hardware buffer, `top` above and `bottom` below (blue, green,
// red), with `usage` besides the CPU's writing.
AHardwareBuffer* halvesHardwareBuffer(uint32_t width, uint32_t height, const uint8_t (&top)[3],
                                      const uint8_t (&bottom)[3],
                                      uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) {
    AHardwareBuffer_Desc desc{};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = usage | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer* buffer = nullptr;
    CHECK(AHardwareBuffer_allocate(&desc, &buffer) == 0);
    AHardwareBuffer_describe(buffer, &desc);
    void* data = nullptr;
    CHECK(AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &data) == 0);
    for (uint32_t y = 0; y < height; ++y) {
        auto* row = static_cast<uint8_t*>(data) + size_t(y) * desc.stride * 4;
        const auto& colour = y < height / 2 ? top : bottom;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 4] = colour[2];
            row[x * 4 + 1] = colour[1];
            row[x * 4 + 2] = colour[0];
            row[x * 4 + 3] = 255;
        }
    }
    AHardwareBuffer_unlock(buffer, nullptr);
    return buffer;
}

// Android's hardware buffers as the host's GPU images (hostGpuFrames).
void hardwareBufferFrames(rcsdk::Session& share, const uint8_t (&key)[32]) {
    CHECK(rcsdk::capabilities().pushed_gpu_frames == (1u << RCS_GPU_AHARDWAREBUFFER));
    std::mutex mutex;
    uint8_t centre[4] = {};
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame& frame) {
        std::lock_guard lock(mutex);
        std::memcpy(centre, frame.data + size_t(frame.height / 2) * frame.stride_bytes + frame.width / 2 * 4, 4);
        ++frames;
    };
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47988, key).setAudioEnabled(false), viewCallbacks);
    constexpr uint8_t red[3] = {0, 0, 255}, green[3] = {0, 255, 0};
    AHardwareBuffer* redBuffer = halvesHardwareBuffer(640, 360, red, red);
    AHardwareBuffer* greenBuffer = halvesHardwareBuffer(640, 360, green, green);
    AHardwareBuffer* small = halvesHardwareBuffer(320, 180, red, red);
    AHardwareBuffer* cpuOnly = halvesHardwareBuffer(640, 360, red, red, 0);
    rcs_gpu_frame_info info = RCS_GPU_FRAME_INFO_INIT;
    info.resource_type = RCS_GPU_AHARDWAREBUFFER;
    info.format = RCS_PIXEL_BGRA8;
    info.width = 640;
    info.height = 360;
    info.resource = redBuffer;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_STATE); // Not running.
    CHECK(rcs_session_push_gpu_frame(view.handle(), &info) == RCS_ERROR_UNSUPPORTED);    // Not a sharer.
    std::atomic<AHardwareBuffer*> pushing{redBuffer};
    std::atomic<bool> running{true};
    std::atomic<int> refused{0};
    std::thread host([&, valid = info] { // 30 pictures a second (`info` turns invalid below).
        while (running) {
            rcs_gpu_frame_info frame = valid;
            frame.resource = pushing.load();
            const rcs_result pushed = rcs_session_push_gpu_frame(share.handle(), &frame);
            refused += pushed != RCS_OK && pushed != RCS_ERROR_INVALID_STATE; // Before the stream starts.
            std::this_thread::sleep_for(33ms);
        }
    });
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    const auto showing = [&](int b, int g, int r) {
        return waitFor([&] {
            std::lock_guard lock(mutex);
            return frames > 5 && std::abs(centre[0] - b) < 24 && std::abs(centre[1] - g) < 24 &&
                   std::abs(centre[2] - r) < 24;
        });
    };
    CHECK(showing(0, 0, 255));
    pushing = greenBuffer;
    CHECK(showing(0, 255, 0));
    // Pictures that are not what their description says.
    info.format = RCS_PIXEL_NV12;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_ARGUMENT); // An RGB buffer.
    info.format = RCS_PIXEL_BGRA8;
    info.resource = small;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_ARGUMENT); // Its size is not 640x360.
    info.resource = cpuOnly;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_ARGUMENT); // The GPU cannot sample it.
    CHECK(rcsdk::lastCallError().find("GPU_SAMPLED_IMAGE") != std::string::npos);
    info.resource_type = RCS_GPU_CVPIXELBUFFER;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_UNSUPPORTED);
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().encoded_frames > 10; }));
    running = false;
    host.join();
    CHECK(refused == 0);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    for (AHardwareBuffer* buffer : {redBuffer, greenBuffer, small, cpuOnly}) {
        AHardwareBuffer_release(buffer);
    }
}
#endif

// Pictures as Android hands them over (rcs_session_push_hardware_buffer,
// e.g. what MediaProjection drew), copied before the call returns: one lying
// sideways arrives upright; one of another shape is fitted into the stream,
// the rest black, its place and size announced. Other rotations and buffers
// the GPU cannot sample are refused; other systems take none.
void hardwareBuffers(const uint8_t (&key)[32]) {
    auto sharing = config(RCS_ROLE_SHARE, 47984, key);
    sharing.setCaptureSource(RCS_CAPTURE_HOST).setAudioEnabled(false);
    rcsdk::Session share(sharing);
#ifndef __ANDROID__
    int notABuffer = 0;
    CHECK(rcsdk::pushHardwareBuffer(share, &notABuffer) == RCS_ERROR_UNSUPPORTED);
#else
    struct Seen {
        uint8_t left[4], right[4], top[4], bottom[4], edge[4];
    };
    std::mutex mutex;
    Seen seen{};
    int frames = 0;
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame& frame) {
        const auto at = [&](uint32_t x, uint32_t y, uint8_t(&out)[4]) {
            std::memcpy(out, frame.data + size_t(y) * frame.stride_bytes + size_t(x) * 4, 4);
        };
        std::lock_guard lock(mutex);
        at(frame.width / 4, frame.height / 2, seen.left);
        at(frame.width * 3 / 4, frame.height / 2, seen.right);
        at(frame.width / 2, frame.height / 4, seen.top);
        at(frame.width / 2, frame.height * 3 / 4, seen.bottom);
        at(8, frame.height / 2, seen.edge);
        ++frames;
    };
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47984, key).setAudioEnabled(false), viewCallbacks);
    const auto is = [](const uint8_t(&pixel)[4], int b, int g, int r) {
        return std::abs(pixel[0] - b) < 32 && std::abs(pixel[1] - g) < 32 && std::abs(pixel[2] - r) < 32;
    };
    constexpr uint8_t red[3] = {0, 0, 255}, blue[3] = {255, 0, 0};
    // Portrait 180x320, red above and blue below. Turned a quarter clockwise
    // to stand, it stands as 320x180 with red on the right.
    AHardwareBuffer* portrait = halvesHardwareBuffer(180, 320, red, blue);
    AHardwareBuffer* cpuOnly = halvesHardwareBuffer(180, 320, red, blue, 0);
    CHECK(rcsdk::pushHardwareBuffer(share, portrait, 90) == RCS_ERROR_INVALID_STATE); // Not running.
    CHECK(rcsdk::pushHardwareBuffer(view, portrait, 90) == RCS_ERROR_UNSUPPORTED);    // Not a sharer.
    CHECK(rcsdk::pushHardwareBuffer(share, portrait, 45) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcsdk::pushSampleBuffer(share, portrait, RCS_SAMPLE_BUFFER_VIDEO) == RCS_ERROR_UNSUPPORTED);
    std::atomic<uint32_t> rotation{90};
    std::atomic<bool> running{true};
    std::atomic<int> refused{0};
    std::thread host([&] { // A picture every 33 ms.
        while (running) {
            const rcs_result shown = rcsdk::pushHardwareBuffer(share, portrait, rotation.load());
            refused += shown != RCS_OK && shown != RCS_ERROR_INVALID_STATE; // Before the stream starts.
            std::this_thread::sleep_for(33ms);
        }
    });
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    // Upright, 320x180 filling the 640x360 stream.
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return frames > 5 && is(seen.right, 0, 0, 255) && is(seen.left, 255, 0, 0);
    }));
    auto stream = view.streamInfo();
    CHECK(stream.display_width == 320 && stream.display_height == 180);
    CHECK(stream.content_x == 0 && stream.content_width == 640 && stream.content_height == 360);
    // Standing as it is, 180x320: fitted into 202x360 in the middle, the rest black.
    rotation = 0;
    CHECK(waitFor([&] {
        stream = view.streamInfo();
        return stream.display_width == 180 && stream.display_height == 320;
    }));
    CHECK(stream.content_x == 218 && stream.content_y == 0 && stream.content_width == 202 &&
          stream.content_height == 360);
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return is(seen.top, 0, 0, 255) && is(seen.bottom, 255, 0, 0) && is(seen.edge, 0, 0, 0);
    }));
    // Upside down: blue above.
    rotation = 180;
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return is(seen.top, 255, 0, 0) && is(seen.bottom, 0, 0, 255);
    }));
    CHECK(rcsdk::pushHardwareBuffer(share, cpuOnly) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcsdk::lastCallError().find("GPU_SAMPLED_IMAGE") != std::string::npos);
    running = false;
    host.join();
    CHECK(refused == 0);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    AHardwareBuffer_release(portrait);
    AHardwareBuffer_release(cpuOnly);
#endif
}

// The host's own GPU images as the pictures (rcs_session_push_gpu_frame):
// macOS and iOS take CVPixelBuffers (BGRA8 and YCbCr), encoded without a
// copy, Android AHardwareBuffers, copied when pushed; their colours arrive;
// an image that is not what its description says is refused; systems that
// take none say so (pushed_gpu_frames 0).
void hostGpuFrames(const uint8_t (&key)[32]) {
    auto sharing = config(RCS_ROLE_SHARE, 47988, key);
    sharing.setCaptureSource(RCS_CAPTURE_HOST).setAudioEnabled(false);
    rcsdk::Session share(sharing);
    rcs_gpu_frame_info info = RCS_GPU_FRAME_INFO_INIT;
    info.width = 640;
    info.height = 360;
#if defined(__ANDROID__)
    hardwareBufferFrames(share, key);
#elif !defined(__APPLE__)
    CHECK(rcsdk::capabilities().pushed_gpu_frames == 0);
    info.resource_type = RCS_GPU_D3D11_TEXTURE2D;
    info.resource = &info; // Never read: the system takes none.
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_UNSUPPORTED);
#else
    CHECK(rcsdk::capabilities().pushed_gpu_frames == (1u << RCS_GPU_CVPIXELBUFFER));
    std::mutex mutex;
    uint8_t centre[4] = {};
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame& frame) {
        std::lock_guard lock(mutex);
        std::memcpy(centre, frame.data + size_t(frame.height / 2) * frame.stride_bytes + frame.width / 2 * 4, 4);
        ++frames;
    };
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47988, key).setAudioEnabled(false), viewCallbacks);
    CVPixelBufferRef red = solidPixelBuffer(kCVPixelFormatType_32BGRA, 640, 360, 0, 0, 255);
    // BT.709 limited green: Y 173, Cb 42, Cr 26.
    CVPixelBufferRef green = solidPixelBuffer(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 640, 360, 173, 42, 26);
    CVPixelBufferRef small = solidPixelBuffer(kCVPixelFormatType_32BGRA, 320, 180, 0, 0, 255);
    info.resource_type = RCS_GPU_CVPIXELBUFFER;
    info.format = RCS_PIXEL_BGRA8;
    info.resource = red;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_STATE); // Not running.
    CHECK(rcs_session_push_gpu_frame(view.handle(), &info) == RCS_ERROR_UNSUPPORTED);    // Not a sharer.
    std::atomic<CVPixelBufferRef> pushing{red};
    std::atomic<rcs_pixel_format> pushingFormat{RCS_PIXEL_BGRA8};
    std::atomic<bool> running{true};
    std::atomic<int> refused{0};
    std::thread host([&] { // 30 pictures a second.
        while (running) {
            rcs_gpu_frame_info frame = RCS_GPU_FRAME_INFO_INIT;
            frame.resource_type = RCS_GPU_CVPIXELBUFFER;
            frame.resource = pushing.load();
            frame.format = pushingFormat.load();
            frame.width = 640;
            frame.height = 360;
            if (frame.format == RCS_PIXEL_NV12) {
                frame.color = {RCS_PRIMARIES_BT709, RCS_TRANSFER_BT709, RCS_MATRIX_BT709, RCS_RANGE_LIMITED};
            }
            // INVALID_STATE before the stream starts, while the session may already run.
            const rcs_result pushed = rcs_session_push_gpu_frame(share.handle(), &frame);
            refused += pushed != RCS_OK && pushed != RCS_ERROR_INVALID_STATE;
            std::this_thread::sleep_for(33ms);
        }
    });
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    const auto showing = [&](int b, int g, int r) {
        return waitFor([&] {
            std::lock_guard lock(mutex);
            return frames > 5 && std::abs(centre[0] - b) < 24 && std::abs(centre[1] - g) < 24 &&
                   std::abs(centre[2] - r) < 24;
        });
    };
    CHECK(showing(0, 0, 255));
    pushingFormat = RCS_PIXEL_NV12; // YCbCr from now on, as ReplayKit gives it.
    pushing = green;
    CHECK(showing(0, 255, 0));
    // Pictures that are not what their description says.
    info.resource = red;
    info.format = RCS_PIXEL_NV12;
    info.color = {RCS_PRIMARIES_BT709, RCS_TRANSFER_BT709, RCS_MATRIX_BT709, RCS_RANGE_LIMITED};
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_ARGUMENT); // A BGRA buffer.
    info.resource = green;
    info.color = {};
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_ARGUMENT); // YCbCr without its colour.
    info.resource = small;
    info.format = RCS_PIXEL_BGRA8;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_ARGUMENT); // Its size is not 640x360.
    info.width = 320;
    info.height = 180;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_ARGUMENT); // Not the stream's size.
    CHECK(rcsdk::lastCallError().find("the stream 640x360") != std::string::npos);
    info.resource_type = RCS_GPU_D3D11_TEXTURE2D;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_UNSUPPORTED);
    info.struct_size = 8;
    CHECK(rcs_session_push_gpu_frame(share.handle(), &info) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().encoded_frames > 10; }));
    running = false;
    host.join();
    CHECK(refused == 0);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    CVPixelBufferRelease(red);
    CVPixelBufferRelease(green);
    CVPixelBufferRelease(small);
#endif
}

// The host's own picture and sound (capture source HOST, audio source HOST)
// instead of the display and the system's sound.
void hostSources(const uint8_t (&key)[32]) {
    std::mutex mutex;
    uint8_t centre[4] = {};
    std::vector<float> received;
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame& frame) {
        std::lock_guard lock(mutex);
        std::memcpy(centre, frame.data + size_t(frame.height / 2) * frame.stride_bytes + frame.width / 2 * 4, 4);
        ++frames;
    };
    viewCallbacks.onAudio = [&](const rcs_audio_frame& frame) {
        std::lock_guard lock(mutex);
        for (uint32_t i = 0; i < frame.frames; ++i) {
            received.push_back(frame.samples[i * 2]);
        }
    };
    auto sharing = config(RCS_ROLE_SHARE, 47867, key);
    // PCM (1.5 Mbit/s) beside 2 Mbit/s of video: the pacer makes room for both.
    sharing.setCaptureSource(RCS_CAPTURE_HOST).setAudioSource(RCS_AUDIO_HOST).setAudioCodecs({RCS_AUDIO_PCM});
    rcsdk::Session share(sharing);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47867, key), viewCallbacks);
    std::vector<uint8_t> picture(640 * 360 * 4);
    const auto paint = [&](uint8_t b, uint8_t g, uint8_t r) {
        for (size_t i = 0; i < picture.size(); i += 4) {
            picture[i] = b;
            picture[i + 1] = g;
            picture[i + 2] = r;
            picture[i + 3] = 255;
        }
    };
    paint(0, 0, 255); // Red.
    CHECK(share.pushVideoFrame(picture.data(), 640, 360, 640 * 4) == RCS_ERROR_INVALID_STATE); // Not running.
    CHECK(view.pushVideoFrame(picture.data(), 640, 360, 640 * 4) == RCS_ERROR_UNSUPPORTED);    // Not a sharer.

    // The host's thread: 30 pictures and 100 blocks of 440 Hz a second.
    std::atomic<bool> pushing{true};
    std::atomic<int> refusedSize{0};
    std::thread host([&] {
        std::vector<float> block(480 * 2);
        uint64_t produced = 0;
        auto next = std::chrono::steady_clock::now();
        for (int tick = 0; pushing; ++tick) {
            for (uint32_t i = 0; i < 480; ++i) {
                block[i * 2] = block[i * 2 + 1] =
                    float(0.5 * std::sin(2 * 3.14159265358979 * 440 * double(produced + i) / 48000));
            }
            produced += 480;
            share.pushAudio(block.data(), 480);
            if (tick % 3 == 0) {
                std::lock_guard lock(mutex);
                share.pushVideoFrame(picture.data(), 640, 360, 640 * 4);
                refusedSize += share.pushVideoFrame(picture.data(), 320, 180, 640 * 4) == RCS_ERROR_INVALID_ARGUMENT;
            }
            next += 10ms;
            std::this_thread::sleep_until(next);
        }
    });
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    const auto showing = [&](uint8_t b, uint8_t g, uint8_t r) {
        return waitFor([&] {
            std::lock_guard lock(mutex);
            const auto similar = [](uint8_t value, uint8_t expected) { return std::abs(int(value) - expected) < 40; };
            return frames > 5 && similar(centre[0], b) && similar(centre[1], g) && similar(centre[2], r);
        });
    };
    CHECK(showing(0, 0, 255));
    {
        std::lock_guard lock(mutex);
        paint(255, 0, 0); // Blue.
    }
    CHECK(showing(255, 0, 0));
    CHECK(waitFor([&] { return refusedSize > 0 && share.stats<rcs_capture_stats>().captured_frames >= 10; }));
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return received.size() >= 48000;
    }));
    std::vector<float> tail;
    {
        std::lock_guard lock(mutex);
        tail.assign(received.end() - 24000, received.end());
    }
    const auto measured = rcsdk::test::measureTone(tail);
    CHECK(std::abs(measured.hz - 440) < 10 && std::abs(measured.rms - 0.354) < 0.05);
    CHECK(share.stats<rcs_audio_send_stats>().captured_frames >= 48000);
    pushing = false;
    host.join();
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);

    // A machine with no display to inject input into (a server sharing its
    // own pictures): the picture goes on; control, though allowed, is not granted.
    rcsdk::seams::InputOverride noDisplay;
    noDisplay.factory = [](const rcsdk::platform::CaptureTarget&,
                           const rcsdk::platform::InputOptions&) -> std::unique_ptr<rcsdk::platform::InputInjector> {
        rcsdk::fail(RCS_ERROR_UNSUPPORTED, "The input display is not available");
    };
    noDisplay.permission = [] { return RCS_PERMISSION_GRANTED; };
    overrideInput(std::move(noDisplay));
    {
        rcsdk::Session server(config(RCS_ROLE_SHARE, 47867, key).setCaptureSource(RCS_CAPTURE_HOST).setControl(true));
        frames = 0;
        rcsdk::Session viewer(config(RCS_ROLE_VIEW, 47867, key), viewCallbacks);
        CHECK(server.start() == RCS_OK);
        CHECK(waitFor([&] { return server.state() == RCS_STATE_LISTENING; }));
        CHECK(viewer.start() == RCS_OK);
        std::atomic<bool> drawing{true};
        std::thread painter([&] {
            while (drawing) {
                {
                    std::lock_guard lock(mutex);
                    server.pushVideoFrame(picture.data(), 640, 360, 640 * 4);
                }
                std::this_thread::sleep_for(33ms);
            }
        });
        CHECK(waitFor([&] { return frames >= 30; }));
        CHECK(server.state() == RCS_STATE_RUNNING && server.controlInfo().allowed && !server.controlInfo().granted);
        CHECK(sessionLog().contains("Remote input is unavailable: The input display is not available"));
        drawing = false;
        painter.join();
    }
    rcsdk::seams::setInputOverride({});
}

// Remote input without the OS: a recording injector and a switchable input permission.
struct InputRecorder {
    std::mutex mutex;
    std::vector<rcs_input_event> applied;
    std::atomic<int> secureFlag{-1}; // Whether the injector last made followed secure desktops.
    std::atomic<int> releases{0};
    std::atomic<bool> available{true}; // false: like a lock screen or a UAC prompt.
    std::atomic<bool> refuse{false};   // true: like an elevated window refusing SendInput.

    size_t count() {
        std::lock_guard lock(mutex);
        return applied.size();
    }
};

class RecordingInjector final : public rcsdk::platform::InputInjector {
public:
    explicit RecordingInjector(InputRecorder& recorder) : recorder_(recorder) {}
    rcs_result apply(const rcs_input_event& event) override {
        if (recorder_.refuse) {
            return RCS_ERROR_PERMISSION_REQUIRED;
        }
        std::lock_guard lock(recorder_.mutex);
        recorder_.applied.push_back(event);
        return RCS_OK;
    }
    void releaseAll() noexcept override { ++recorder_.releases; }

private:
    InputRecorder& recorder_;
};

// The grant pauses while the OS refuses input and resumes afterwards; the
// host's setting is never revoked by the SDK.
void control(const uint8_t (&key)[32]) {
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [&] {
        return recorder.available ? RCS_PERMISSION_GRANTED : RCS_PERMISSION_REQUIRED;
    };
    overrideInput(std::move(input));
    {
        EventLog shareEvents, viewEvents;
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        viewCallbacks.onEvent = viewEvents.callback();
        // The host sees each event first and may drop it.
        std::atomic<bool> dropKeys{false};
        shareCallbacks.onInput = [&](const rcs_input_event& event) {
            return !(dropKeys && event.type == RCS_INPUT_KEY);
        };
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47832, key).setControl(true), shareCallbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47832, key), viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        CHECK(share.controlInfo().allowed && share.controlInfo().granted);
        const auto granted = [](EventLog& log) {
            const auto events = log.of(RCS_EVENT_CONTROL);
            return !events.empty() && events.back().control.granted;
        };
        CHECK(waitFor([&] { return granted(shareEvents) && granted(viewEvents); }));
        // The grant is announced once the agent has said the system takes
        // input, not first as "not granted" while that is unknown.
        CHECK(shareEvents.of(RCS_EVENT_CONTROL).size() == 1 && viewEvents.of(RCS_EVENT_CONTROL).size() == 1);

        rcs_input_event key4 = RCS_INPUT_EVENT_INIT;
        key4.type = RCS_INPUT_KEY;
        key4.code = 4;
        key4.down = 1;
        const auto press = [&] {
            const size_t before = recorder.count();
            return view.sendInput(key4) == RCS_OK && waitFor([&] { return recorder.count() > before; });
        };
        CHECK(press());

        // Lock screen: the grant pauses, held keys are released, input is refused.
        recorder.available = false;
        CHECK(waitFor([&] { return !view.controlInfo().granted; }));
        CHECK(share.controlInfo().allowed && !share.controlInfo().granted && recorder.releases >= 1);
        CHECK(view.sendInput(key4) == RCS_ERROR_INVALID_STATE);
        CHECK(waitFor([&] { return !granted(shareEvents) && !granted(viewEvents); }));
        // Unlocked: control resumes without the host doing anything.
        recorder.available = true;
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        CHECK(press());

        // A single refused event (an elevated window) keeps the grant.
        recorder.refuse = true;
        CHECK(view.sendInput(key4) == RCS_OK);
        CHECK(waitFor([&] { return share.stats<rcs_input_stats>().last_result == RCS_ERROR_PERMISSION_REQUIRED; }));
        std::this_thread::sleep_for(1500ms);
        CHECK(view.controlInfo().granted && share.controlInfo().granted);
        recorder.refuse = false;
        CHECK(press());
        const auto input = share.stats<rcs_input_stats>();
        CHECK(input.received >= 4 && input.applied >= 3 && view.stats<rcs_input_stats>().sent >= 4);

        // Dropped by the host: never executed.
        dropKeys = true;
        const size_t executed = recorder.count();
        CHECK(view.sendInput(key4) == RCS_OK);
        CHECK(waitFor([&] { return share.stats<rcs_input_stats>().received >= input.received + 1; }));
        std::this_thread::sleep_for(100ms);
        CHECK(recorder.count() == executed);
        dropKeys = false;

        // The host revokes control while running: held input is released.
        rcs_control_config revoked = RCS_CONTROL_CONFIG_INIT;
        const int releases = recorder.releases;
        CHECK(share.update(revoked) == RCS_OK);
        CHECK(waitFor([&] { return !view.controlInfo().granted; }));
        CHECK(!share.controlInfo().allowed && recorder.releases > releases);
    }
    rcsdk::seams::setInputOverride({});
}

// Input the host executes itself (RCS_INPUT_HOST): nothing reaches the system.
void hostInput(const uint8_t (&key)[32]) {
    InputRecorder recorder; // The system's injector: must stay unused.
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] { return RCS_PERMISSION_REQUIRED; }; // Not needed for the host.
    overrideInput(std::move(input));
    {
        std::mutex mutex;
        std::vector<rcs_input_event> handled;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onInput = [&](const rcs_input_event& event) {
            std::lock_guard lock(mutex);
            handled.push_back(event);
            return false; // Ignored for the host target.
        };
        const auto handledOf = [&](uint32_t type) {
            std::lock_guard lock(mutex);
            return std::count_if(handled.begin(), handled.end(), [&](const auto& e) { return e.type == type; });
        };
        // Without on_input there is nobody to execute it.
        CHECK(throws<rcsdk::Error>([&] {
            rcsdk::Session invalid(config(RCS_ROLE_SHARE, 47866, key).setControl(true, RCS_INPUT_HOST));
            rcsdk::check(invalid.start());
        }));
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47866, key).setControl(true, RCS_INPUT_HOST), shareCallbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47866, key));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        rcs_input_event move = RCS_INPUT_EVENT_INIT;
        move.type = RCS_INPUT_MOVE;
        move.x = 1234;
        CHECK(view.sendInput(move) == RCS_OK);
        CHECK(waitFor([&] { return handledOf(RCS_INPUT_MOVE) == 1; }));
        CHECK(share.stats<rcs_input_stats>().applied >= 1 && recorder.count() == 0);
        // Back to the system while running: the host's input is released first.
        rcs_control_config system = RCS_CONTROL_CONFIG_INIT;
        system.allowed = 0;
        CHECK(share.update(system) == RCS_OK);
        CHECK(waitFor([&] { return handledOf(RCS_INPUT_RELEASE_ALL) >= 1; }));
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::seams::setInputOverride({});
}

// What the viewer's Caps Lock does reaches the system's injector (a hold of 0:
// 500 ms); a change while running makes a new one; values no version defines
// are refused.
void capsLock(const uint8_t (&key)[32]) {
    using rcsdk::platform::InputOptions;
    InputRecorder recorder;
    std::mutex mutex;
    std::optional<InputOptions> made; // The last injector's.
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const InputOptions& options) {
        std::lock_guard lock(mutex);
        made = options;
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    {
        for (const auto& [capsLock, holdMs] : {std::pair{2u, 0u}, {RCS_CAPS_LOCK_INPUT_SOURCE, 99u},
                                                {RCS_CAPS_LOCK_INPUT_SOURCE, 5001u}}) {
            CHECK(throws<rcsdk::Error>([&] {
                rcsdk::Session invalid(config(RCS_ROLE_SHARE, 47851, key).setControl(true).setCapsLock(capsLock, holdMs));
            }));
        }
        const auto last = [&] {
            std::lock_guard lock(mutex);
            return made;
        };
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47851, key).setControl(true).setCapsLock(RCS_CAPS_LOCK_CAPS_LOCK, 800));
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47851, key));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        CHECK((last() == InputOptions{false, RCS_CAPS_LOCK_CAPS_LOCK, 800}));
        auto control = rcsdk::defaults<rcs_control_config>();
        control.allowed = 1;
        CHECK(share.update(control) == RCS_OK);
        CHECK(waitFor([&] { return last() == InputOptions{false, RCS_CAPS_LOCK_INPUT_SOURCE, 500}; }));
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::seams::setInputOverride({});
}

// Types the host executes beside the system (host_input_types): touch goes to
// the host whatever on_input returns, keys to the system as on_input decides,
// release-all to both; the viewer learns the sharer takes touch; handing it
// back releases what both hold.
void hostInputTypes(const uint8_t (&key)[32]) {
    InputRecorder recorder; // The system's injector.
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    {
        constexpr uint32_t touch = 1u << RCS_INPUT_TOUCH;
        // The host executes them in on_input; target HOST has it execute every
        // type already; bits are input types.
        try {
            rcsdk::Session invalid(config(RCS_ROLE_SHARE, 47989, key).setControl(true).setHostInputTypes(touch));
            rcsdk::check(invalid.start());
            CHECK(false);
        } catch (const rcsdk::Error& error) {
            CHECK(error.result() == RCS_ERROR_INVALID_ARGUMENT &&
                  std::string(error.what()).find("host_input_types needs on_input") != std::string::npos);
        }
        CHECK(throws<rcsdk::Error>([&] {
            rcsdk::Session invalid(
                config(RCS_ROLE_SHARE, 47989, key).setControl(true, RCS_INPUT_HOST).setHostInputTypes(touch));
        }));
        CHECK(throws<rcsdk::Error>(
            [&] { rcsdk::Session invalid(config(RCS_ROLE_SHARE, 47989, key).setHostInputTypes(1u << 31)); }));

        std::mutex mutex;
        std::vector<rcs_input_event> handled;
        std::atomic<bool> drop{false};
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onInput = [&](const rcs_input_event& event) {
            std::lock_guard lock(mutex);
            handled.push_back(event);
            return !drop; // Counts for the system's types only.
        };
        const auto handledOf = [&](uint32_t type) {
            std::lock_guard lock(mutex);
            return std::count_if(handled.begin(), handled.end(), [&](const auto& e) { return e.type == type; });
        };
        const auto executedOf = [&](uint32_t type) {
            std::lock_guard lock(recorder.mutex);
            return std::count_if(recorder.applied.begin(), recorder.applied.end(),
                                 [&](const auto& e) { return e.type == type; });
        };
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47989, key).setControl(true).setHostInputTypes(touch),
                             shareCallbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47989, key));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        CHECK((view.peerInfo().input_types & touch) && (view.peerInfo().input_types & (1u << RCS_INPUT_KEY)));
        CHECK(rcsdk::agentInfo(share).input_types & touch);

        drop = true;
        rcs_input_event contact = RCS_INPUT_EVENT_INIT;
        contact.type = RCS_INPUT_TOUCH;
        contact.code = 3;
        contact.down = 1;
        contact.x = 20000;
        CHECK(view.sendInput(contact) == RCS_OK);
        CHECK(waitFor([&] { return handledOf(RCS_INPUT_TOUCH) == 1 && share.stats<rcs_input_stats>().applied == 1; }));
        rcs_input_event key4 = RCS_INPUT_EVENT_INIT;
        key4.type = RCS_INPUT_KEY;
        key4.code = 4;
        key4.down = 1;
        CHECK(view.sendInput(key4) == RCS_OK); // Dropped by the host: never executed.
        CHECK(waitFor([&] { return handledOf(RCS_INPUT_KEY) == 1; }));
        drop = false;
        CHECK(view.sendInput(key4) == RCS_OK);
        CHECK(waitFor([&] { return executedOf(RCS_INPUT_KEY) == 1; }));
        CHECK(executedOf(RCS_INPUT_TOUCH) == 0);
        rcs_input_event release = RCS_INPUT_EVENT_INIT;
        release.type = RCS_INPUT_RELEASE_ALL;
        CHECK(view.sendInput(release) == RCS_OK);
        CHECK(waitFor([&] { return handledOf(RCS_INPUT_RELEASE_ALL) == 1 && executedOf(RCS_INPUT_RELEASE_ALL) == 1; }));

        // Handed back to the system while running: what both held is released,
        // the system executes touch (here: the recording injector), and the
        // viewer learns whether this machine does.
        const int releases = recorder.releases;
        const auto hostReleases = handledOf(RCS_INPUT_RELEASE_ALL);
        auto control = rcsdk::defaults<rcs_control_config>();
        control.allowed = 1;
        CHECK(share.update(control) == RCS_OK);
        CHECK(waitFor([&] { return recorder.releases > releases && handledOf(RCS_INPUT_RELEASE_ALL) > hostReleases; }));
        const uint32_t systemTouch = rcsdk::capabilities().input_types & touch;
        CHECK(waitFor([&] { return (view.peerInfo().input_types & touch) == systemTouch; }));
        CHECK(waitFor([&] { // Input of the old grant is discarded: sent again until the new one took it.
            view.sendInput(contact);
            return executedOf(RCS_INPUT_TOUCH) >= 1;
        }));
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::seams::setInputOverride({});
}

// A text caret of the test's: where it is, whether it shows, and across what
// it was looked for.
struct CaretControl {
    std::atomic<int> looks{0};
    std::atomic<uint32_t> x{1000};
    std::atomic<bool> shown{true};
    std::mutex mutex;
    rcs_module_shared shared = RCS_MODULE_SHARED_INIT;
};

class TestCaret final : public rcsdk::text_caret::Reader {
public:
    explicit TestCaret(CaretControl& control) : control_(control) {}
    rcs_text_caret read(const rcs_module_shared& shared) override {
        ++control_.looks;
        {
            std::lock_guard lock(control_.mutex);
            control_.shared = shared;
        }
        if (!control_.shown) {
            return {};
        }
        return {1, control_.x.load(), 30000, 40, 1200};
    }

private:
    CaretControl& control_;
};

// Stands for the system's power requests (the power module's factory): what
// is held, and how many were made.
struct PowerRecorder {
    std::atomic<int> held{RCS_KEEP_AWAKE_OFF}; // The newest request's mode while any lives.
    std::atomic<int> created{0}, live{0};
    std::string reason;
    std::mutex mutex;

    struct Request final : rcsdk::power::Request {
        Request(PowerRecorder& recorder, rcs_keep_awake mode) : recorder_(recorder) {
            ++recorder_.live;
            recorder_.held = int(mode);
        }
        ~Request() override {
            if (--recorder_.live == 0) {
                recorder_.held = RCS_KEEP_AWAKE_OFF;
            }
        }
        PowerRecorder& recorder_;
    };
    std::unique_ptr<rcsdk::power::Request> make(rcs_keep_awake mode, const std::string& text) {
        {
            std::lock_guard lock(mutex);
            reason = text;
        }
        ++created;
        return std::make_unique<Request>(*this, mode);
    }
};

rcsdk::SessionConfig& usePower(rcsdk::SessionConfig& c, rcs_keep_awake mode, bool wholeRun = false) {
    static std::once_flag once;
    std::call_once(once, [] { rcsdk::registerModule(*rcs_power_module()); });
    rcs_power_settings settings = RCS_POWER_SETTINGS_INIT;
    settings.keep_awake = mode;
    settings.while_running = wholeRun ? 1 : 0;
    return c.useModule(RCS_POWER_MODULE, settings);
}

rcs_power_status powerStatus(rcsdk::Session& session) {
    rcs_power_status status = RCS_POWER_STATUS_INIT;
    CHECK(rcs_power_get_status(session.handle(), &status) == RCS_OK);
    return status;
}

// Keeping the machine awake (the power module): a request on the agent's
// machine while a viewer is connected, or for the whole run; settings change
// while running; a system that refuses is reported and the run goes on.
void power(const uint8_t (&key)[32]) {
    PowerRecorder power;
    rcsdk::power::setFactory([&](rcs_keep_awake mode, const std::string& reason) { return power.make(mode, reason); });
    {
        EventLog shareEvents;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        auto sharing = config(RCS_ROLE_SHARE, 47896, key).setKeepListening(true);
        rcsdk::Session share(usePower(sharing, RCS_KEEP_AWAKE_DISPLAY), shareCallbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47896, key), {});
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        std::this_thread::sleep_for(200ms);
        CHECK(power.held == RCS_KEEP_AWAKE_OFF && powerStatus(share).wanted == RCS_KEEP_AWAKE_OFF);
        CHECK(view.start() == RCS_OK); // The viewer does not use the module: it is connected all the same.
        CHECK(waitFor([&] { return power.held == RCS_KEEP_AWAKE_DISPLAY; }));
        CHECK(power.reason == "Remote session in progress");
        CHECK(waitFor([&] {
            const auto s = powerStatus(share);
            return s.wanted == RCS_KEEP_AWAKE_DISPLAY && s.held == RCS_KEEP_AWAKE_DISPLAY && s.result == RCS_OK;
        }));
        CHECK(waitFor([&] {
            for (const auto& e : shareEvents.ofModule(RCS_POWER_MODULE, RCS_POWER_CHANGED)) {
                if (e.payload.size() == sizeof(rcs_power_status)) {
                    rcs_power_status s;
                    std::memcpy(&s, e.payload.data(), sizeof(s));
                    if (s.held == RCS_KEEP_AWAKE_DISPLAY) {
                        return true;
                    }
                }
            }
            return false;
        }));
        // The viewer leaves: the machine may sleep again.
        CHECK(view.stop() == RCS_OK);
        CHECK(waitFor([&] { return power.held == RCS_KEEP_AWAKE_OFF && powerStatus(share).held == RCS_KEEP_AWAKE_OFF; }));
        // Settings change while running: the request follows.
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        auto changed = config(RCS_ROLE_SHARE, 47896, key).setKeepListening(true);
        CHECK(share.update(usePower(changed, RCS_KEEP_AWAKE_SYSTEM, true)) == RCS_OK);
        CHECK(waitFor([&] { return power.held == RCS_KEEP_AWAKE_SYSTEM; }));
        CHECK(share.stop() == RCS_OK);
        CHECK(waitFor([&] { return power.live == 0; }));
    }
    // The whole run, not just while a viewer is connected.
    {
        auto sharing = config(RCS_ROLE_SHARE, 47897, key);
        rcsdk::Session share(usePower(sharing, RCS_KEEP_AWAKE_SYSTEM, true));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return power.held == RCS_KEEP_AWAKE_SYSTEM; }));
        CHECK(share.stop() == RCS_OK);
        CHECK(waitFor([&] { return power.live == 0; }));
    }
    // A viewer keeps its own machine awake while it is connected to its sharer
    // (its display: an iPad that watches), whether the sharer uses the module or not.
    {
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47899, key));
        auto viewing = config(RCS_ROLE_VIEW, 47899, key);
        rcsdk::Session view(usePower(viewing, RCS_KEEP_AWAKE_DISPLAY));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(powerStatus(view).wanted == RCS_KEEP_AWAKE_OFF); // Not connected yet.
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] {
            const auto s = powerStatus(view);
            return power.held == RCS_KEEP_AWAKE_DISPLAY && s.held == RCS_KEEP_AWAKE_DISPLAY && s.result == RCS_OK;
        }));
        CHECK(view.stop() == RCS_OK);
        CHECK(waitFor([&] { return power.live == 0; }));
        CHECK(share.stop() == RCS_OK);
    }
    // A system that refuses the request: reported, and the run goes on.
    rcsdk::power::setFactory([](rcs_keep_awake, const std::string&) -> std::unique_ptr<rcsdk::power::Request> {
        throw rcsdk::power::Refused(RCS_ERROR_INTERNAL, "No power requests here");
    });
    {
        std::atomic<int> frames{0};
        rcsdk::Callbacks viewCallbacks;
        viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
        auto sharing = config(RCS_ROLE_SHARE, 47898, key);
        rcsdk::Session share(usePower(sharing, RCS_KEEP_AWAKE_SYSTEM));
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47898, key), viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 10; }));
        CHECK(waitFor([&] {
            const auto s = powerStatus(share);
            return s.wanted == RCS_KEEP_AWAKE_SYSTEM && s.held == RCS_KEEP_AWAKE_OFF && s.result == RCS_ERROR_INTERNAL &&
                   std::string(s.message) == "No power requests here";
        }));
        CHECK(sessionLog().contains("The machine is not kept awake: No power requests here"));
    }
    rcsdk::power::setFactory({});
    // A session without the module has no status.
    rcsdk::Session plain(config(RCS_ROLE_SHARE, 47898, key));
    rcs_power_status status = RCS_POWER_STATUS_INIT;
    CHECK(rcs_power_get_status(plain.handle(), &status) == RCS_ERROR_INVALID_STATE);
}

void useTextCaret() {
    static std::once_flag once;
    std::call_once(once, [] { rcsdk::registerModule(*rcs_text_caret_module()); });
}

// Where text goes (the text caret module): the sharer looks while the viewer
// asks and holds control; the viewer hears of changes only, and of the caret
// going when control, the ask or the sharer's telling ends.
void textCaret(const uint8_t (&key)[32]) {
    if (!(rcsdk::capabilities().capture_sources & (1u << RCS_CAPTURE_DISPLAY))) {
        std::printf("The caret lies on a screen this system cannot share (iOS): not tested\n");
        return;
    }
    useTextCaret();
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    CaretControl caret;
    rcsdk::text_caret::setFactory([&] { return std::make_unique<TestCaret>(caret); });
    {
        rcs_text_caret_settings looks = RCS_TEXT_CARET_SETTINGS_INIT;
        looks.interval_ms = 5; // Too often.
        CHECK(throws<rcsdk::Error>([&] {
            rcsdk::Session invalid(config(RCS_ROLE_SHARE, 47911, key).useModule(RCS_TEXT_CARET_MODULE, looks));
        }));
        looks.interval_ms = 20;
        EventLog viewEvents;
        rcsdk::Callbacks viewCallbacks;
        viewCallbacks.onEvent = viewEvents.callback();
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47911, key).setControl(true).useModule(RCS_TEXT_CARET_MODULE, looks));
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47911, key).useModule(RCS_TEXT_CARET_MODULE, nullptr, 0),
                            viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        const auto carets = [&] {
            std::vector<rcs_text_caret> list;
            for (const auto& e : viewEvents.ofModule(RCS_TEXT_CARET_MODULE, RCS_TEXT_CARET_CHANGED)) {
                CHECK(e.payload.size() == sizeof(rcs_text_caret));
                rcs_text_caret c{};
                std::memcpy(&c, e.payload.data(), sizeof(c));
                list.push_back(c);
            }
            return list;
        };
        const auto last = [&] {
            return carets().back();
        };
        const auto tells = [&] {
            const auto said = viewEvents.ofModule(RCS_TEXT_CARET_MODULE, RCS_TEXT_CARET_TELLS);
            uint32_t value = 0;
            if (!said.empty()) {
                std::memcpy(&value, said.back().payload.data(), sizeof(value));
            }
            return value;
        };
        const auto updateCaret = [](rcsdk::Session& session, const rcs_text_caret_settings& settings) {
            rcs_module_config update = RCS_MODULE_CONFIG_INIT;
            update.module = RCS_TEXT_CARET_MODULE;
            update.settings = &settings;
            update.size = sizeof(settings);
            return session.update(update);
        };
        const auto peerOf = [](rcsdk::Session& session, rcs_viewer_id viewer) {
            rcs_text_caret_peer peer = RCS_TEXT_CARET_PEER_INIT;
            CHECK(rcs_text_caret_get_peer(session.handle(), viewer, &peer) == RCS_OK);
            return peer;
        };
        CHECK(waitFor([&] { return tells() == 1 && peerOf(view, 0).tells == 1; }));
        CHECK(peerOf(share, share.viewers().at(0).viewer_id).asks == 1);
        CHECK(waitFor([&] { return !carets().empty() && last().visible && last().x == 1000; }));
        CHECK(last().y == 30000 && last().width == 40 && last().height == 1200);
        CHECK(peerOf(view, 0).caret.x == 1000);
        {
            // What is shared: the main display, named, and where it shows.
            const auto displays = rcsdk::platform::displays();
            const auto primary = std::find_if(displays.begin(), displays.end(),
                                              [](const rcs_display_info& d) { return d.is_main != 0; });
            std::lock_guard lock(caret.mutex);
            CHECK(caret.shared.source == RCS_CAPTURE_DISPLAY &&
                  caret.shared.display_id == (primary != displays.end() ? primary->display_id : 0));
            CHECK(caret.shared.right > caret.shared.left && caret.shared.bottom > caret.shared.top);
        }
        std::this_thread::sleep_for(300ms); // Looked at again and again: told once.
        CHECK(carets().size() == 1 && caret.looks > 5);
        caret.x = 2000;
        CHECK(waitFor([&] { return carets().size() == 2 && last().x == 2000; }));
        caret.shown = false;
        CHECK(waitFor([&] { return carets().size() == 3 && !last().visible && !last().x && !last().height; }));
        caret.shown = true;
        CHECK(waitFor([&] { return carets().size() == 4 && last().visible; }));

        // Control revoked: the caret is gone, and the sharer stops looking.
        CHECK(share.update(rcsdk::defaults<rcs_control_config>()) == RCS_OK);
        CHECK(waitFor([&] { return carets().size() == 5 && !last().visible; }));
        std::this_thread::sleep_for(100ms);
        const int looked = caret.looks;
        std::this_thread::sleep_for(200ms);
        CHECK(caret.looks <= looked + 1);
        auto control = rcsdk::defaults<rcs_control_config>();
        control.allowed = 1; // Granted again: told again.
        CHECK(share.update(control) == RCS_OK);
        CHECK(waitFor([&] { return carets().size() == 6 && last().visible; }));

        // The viewer stops asking: gone, once.
        rcs_text_caret_settings asks = RCS_TEXT_CARET_SETTINGS_INIT;
        asks.ask = 0;
        CHECK(updateCaret(view, asks) == RCS_OK);
        CHECK(waitFor([&] { return carets().size() == 7 && !last().visible; }));
        CHECK(waitFor([&] { return peerOf(share, share.viewers().at(0).viewer_id).asks == 0; }));
        std::this_thread::sleep_for(300ms);
        CHECK(carets().size() == 7);
        asks.ask = 1;
        CHECK(updateCaret(view, asks) == RCS_OK);
        CHECK(waitFor([&] { return carets().size() == 8 && last().visible; }));

        // The sharer does not look any more: the viewer learns it, and the caret goes.
        looks.interval_ms = 0;
        CHECK(updateCaret(share, looks) == RCS_OK);
        CHECK(waitFor([&] { return tells() == 0 && carets().size() == 9 && !last().visible; }));
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
        CHECK(carets().size() == 9);
    }
    // A sharer without the module tells nothing: the module is not active there.
    {
        EventLog viewEvents;
        rcsdk::Callbacks viewCallbacks;
        viewCallbacks.onEvent = viewEvents.callback();
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47911, key).setControl(true));
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47911, key).useModule(RCS_TEXT_CARET_MODULE, nullptr, 0),
                            viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return !viewEvents.ofModule(RCS_TEXT_CARET_MODULE, RCS_MODULE_INACTIVE).empty(); }));
        rcs_text_caret_peer peer = RCS_TEXT_CARET_PEER_INIT;
        CHECK(rcs_text_caret_get_peer(view.handle(), 0, &peer) == RCS_ERROR_INVALID_STATE);
    }
    rcsdk::text_caret::setFactory({});
    rcsdk::seams::setInputOverride({});
}

// A sharer that stops to restart says so: the viewer reports RESTARTING (not
// DISCONNECTED), reconnects by itself, and the next sharer on the same address
// gets it back. A sharer that stops for good still ends the viewer's run.
void restartNotice(const uint8_t (&key)[32]) {
    uint16_t port = 47890;
    for (const rcs_transport transport : {RCS_TRANSPORT_UDP, RCS_TRANSPORT_TCP}) {
        ++port;
        EventLog viewEvents;
        std::atomic<int> frames{0};
        rcsdk::Callbacks viewCallbacks;
        viewCallbacks.onEvent = viewEvents.callback();
        viewCallbacks.onFrame = [&](const rcs_frame&) {
            ++frames;
        };
        rcsdk::Session view(reconnecting(config(RCS_ROLE_VIEW, port, key).setTransport(transport), 20000, 200),
                            viewCallbacks);
        {
            rcsdk::Session share(config(RCS_ROLE_SHARE, port, key).setTransport(transport));
            CHECK(share.start() == RCS_OK);
            CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
            CHECK(view.start() == RCS_OK);
            CHECK(waitFor([&] { return frames >= 10; }));
            // An unknown reason is refused; the restart one is accepted.
            CHECK(rcs_session_stop_with_reason(share.handle(), 7) == RCS_ERROR_INVALID_ARGUMENT);
            const auto stopped = std::chrono::steady_clock::now();
            CHECK(share.stop(RCS_STOP_RESTART) == RCS_OK);
            CHECK(waitFor(
                [&] {
                    const auto states = viewEvents.of(RCS_EVENT_STATE);
                    return states.back().state.value == RCS_STATE_RECONNECTING &&
                           states.back().state.result == RCS_ERROR_RESTARTING;
                },
                2s));
            CHECK(std::chrono::steady_clock::now() - stopped < 2s); // Told at once, not after a timeout.
        }
        // The program is back: the viewer connects without the host doing anything.
        const int before = frames;
        rcsdk::Session again(config(RCS_ROLE_SHARE, port, key).setTransport(transport));
        CHECK(again.start() == RCS_OK);
        CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING; }, 15s));
        CHECK(waitFor([&] { return frames >= before + 10; }));
        CHECK(view.stats<rcs_network_stats>().connections == 2);
        // It stops for good: the viewer learns it is gone, and gives up.
        CHECK(again.stop() == RCS_OK);
        CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }, 3s));
        CHECK(view.failure().result == RCS_ERROR_DISCONNECTED);
    }

    // A viewer that restarts: the sharer listens for it again and says why.
    {
        EventLog shareEvents;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47893, key).setKeepListening(true), shareCallbacks);
        std::atomic<int> frames{0};
        rcsdk::Callbacks viewCallbacks;
        viewCallbacks.onFrame = [&](const rcs_frame&) {
            ++frames;
        };
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47893, key), viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 10; }));
        CHECK(view.stop(RCS_STOP_RESTART) == RCS_OK);
        CHECK(waitFor([&] {
            const auto states = shareEvents.of(RCS_EVENT_STATE);
            return states.back().state.value == RCS_STATE_LISTENING &&
                   states.back().state.result == RCS_ERROR_RESTARTING;
        }));
        CHECK(share.stop() == RCS_OK);
    }
}

// What the shared machine shows reaches the host and the viewer; capture and
// input follow secure desktops where the process can; Ctrl+Alt+Del is an input
// the machine executes.
void machineState(const uint8_t (&key)[32]) {
    // A process that cannot follow secure desktops does not pretend to.
    rcsdk::seams::setSecureCapabilities({false, false});
    {
        rcsdk::Session refused(config(RCS_ROLE_SHARE, 47895, key).setSecureDesktop(true));
        CHECK(refused.start() == RCS_ERROR_UNSUPPORTED);
        CHECK(refused.failure().message.find("secure_desktop") != std::string::npos);
    }
    // One that can: both settings reach the capture and the injector.
    rcsdk::seams::setSecureCapabilities({true, true});
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions& options) {
        recorder.secureFlag = options.secureDesktop ? 1 : 0;
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    std::atomic<int> desktop{0}; // 0: the user's desktop, 1: a lock screen, 2: the sign-in screen.
    rcsdk::seams::setDesktopState([&] {
        return rcsdk::platform::DesktopState{rcs_desktop_kind(desktop == 0 ? RCS_DESKTOP_USER : RCS_DESKTOP_SECURE),
                                             desktop != 2};
    });
    // Ctrl+Alt+Del goes to the process that runs the session, which sends it.
    std::atomic<int> attentions{0};
    std::atomic<rcs_result> attentionResult{RCS_OK};
    rcsdk::seams::setSecureAttention([&] {
        ++attentions;
        return attentionResult.load();
    });
    {
        EventLog shareEvents, viewEvents;
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        viewCallbacks.onEvent = viewEvents.callback();
        std::atomic<bool> dropAttention{false};
        shareCallbacks.onInput = [&](const rcs_input_event& event) {
            return !(dropAttention && event.type == RCS_INPUT_SECURE_ATTENTION);
        };
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47896, key)
                                 .setSecureDesktop(true)
                                 .setControl(true)
                                 .setKeepListening(true), // For the next viewer.
                             shareCallbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47896, key), viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        // The host hears what the machine shows before any viewer is there.
        CHECK(waitFor(
            [&] { return share.desktopInfo().kind == RCS_DESKTOP_USER && share.desktopInfo().user_logged_on; }));
        CHECK(waitFor([&] { return !shareEvents.of(RCS_EVENT_DESKTOP).empty(); })); // The event follows the info.
        CHECK(view.desktopInfo().kind == RCS_DESKTOP_UNKNOWN);

        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        CHECK(waitFor([&] { return view.desktopInfo().kind == RCS_DESKTOP_USER; }));
        CHECK(rcsdk::test::patternControl().secureDesktop && recorder.secureFlag == 1);
        CHECK(rcsdk::agentInfo(share).secure_desktop == 1);

        // The machine shows a lock screen, the sign-in screen, the user's desktop again: both ends follow.
        desktop = 1;
        CHECK(waitFor(
            [&] { return view.desktopInfo().kind == RCS_DESKTOP_SECURE && view.desktopInfo().user_logged_on; }));
        CHECK(waitFor([&] { return share.desktopInfo().kind == RCS_DESKTOP_SECURE; }));
        desktop = 2;
        CHECK(waitFor(
            [&] { return view.desktopInfo().kind == RCS_DESKTOP_SECURE && !view.desktopInfo().user_logged_on; }));
        desktop = 0;
        CHECK(waitFor([&] { return view.desktopInfo().kind == RCS_DESKTOP_USER; }));
        {
            // User, lock, sign-in, user: each change once, at both ends.
            CHECK(waitFor([&] {
                return viewEvents.of(RCS_EVENT_DESKTOP).size() == 4 && shareEvents.of(RCS_EVENT_DESKTOP).size() == 4;
            }));
            const auto events = viewEvents.of(RCS_EVENT_DESKTOP);
            CHECK(events[1].desktop.kind == RCS_DESKTOP_SECURE && events[1].desktop.user_logged_on);
            CHECK(events[2].desktop.kind == RCS_DESKTOP_SECURE && !events[2].desktop.user_logged_on);
            CHECK(!viewEvents.overlapping);
        }

        // Ctrl+Alt+Del: an input like the others; the host sees it first and may drop it.
        rcs_input_event attention = RCS_INPUT_EVENT_INIT;
        attention.type = RCS_INPUT_SECURE_ATTENTION;
        const size_t before = recorder.count();
        CHECK(view.sendInput(attention) == RCS_OK);
        CHECK(waitFor([&] { return attentions == 1 && share.stats<rcs_input_stats>().applied >= 1; }));
        CHECK(recorder.count() == before); // Not a key: the injector never sees it.
        dropAttention = true;
        CHECK(view.sendInput(attention) == RCS_OK);
        CHECK(waitFor([&] { return share.stats<rcs_input_stats>().received >= 2; }));
        std::this_thread::sleep_for(150ms);
        CHECK(attentions == 1);
        dropAttention = false;
        // What the system answers is what the host's statistics say; the grant stays.
        attentionResult = RCS_ERROR_UNSUPPORTED; // (Not the code the host's own drop above left behind.)
        CHECK(view.sendInput(attention) == RCS_OK);
        CHECK(waitFor([&] { return attentions == 2; }));
        CHECK(waitFor([&] { return share.stats<rcs_input_stats>().last_result == RCS_ERROR_UNSUPPORTED; }));
        CHECK(view.controlInfo().granted && share.controlInfo().granted);
        attentionResult = RCS_OK;

        // A viewer that leaves, and the one that returns hears the desktop at once.
        CHECK(view.stop() == RCS_OK);
        desktop = 1;
        CHECK(waitFor([&] { return share.desktopInfo().kind == RCS_DESKTOP_SECURE; }));
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.desktopInfo().kind == RCS_DESKTOP_SECURE; }));
        desktop = 0;
        CHECK(waitFor([&] { return view.desktopInfo().kind == RCS_DESKTOP_USER; }));

        // Settings change while running: capture restarts for the new setting.
        auto capture = rcsdk::defaults<rcs_capture_config>();
        CHECK(share.update(capture) == RCS_OK); // secure_desktop off.
        CHECK(waitFor([&] { return !rcsdk::test::patternControl().secureDesktop && recorder.secureFlag == 0; }));
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::seams::setDesktopState({});
    rcsdk::seams::setInputOverride({});
    rcsdk::seams::setSecureAttention({});
    rcsdk::seams::setSecureCapabilities({});
}

// ---- A sharer whose machine-bound work runs in an agent ------------------------------------------

std::string agentEndpoint(const char* name) {
#ifdef _WIN32
    return std::string("rcsdk-test-session-") + name + "-" + std::to_string(GetCurrentProcessId());
#elif defined(__ANDROID__) // No /tmp: where the test runner says (TMPDIR).
    return std::getenv("TMPDIR") + std::string("/rcsdk-test-session-") + name + "-" + std::to_string(getpid()) +
           ".sock";
#else
    return std::string("/tmp/rcsdk-test-session-") + name + "-" + std::to_string(getpid()) + ".sock";
#endif
}

unsigned long ownProcess() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return unsigned(getpid());
#endif
}

// The capture, the encoder, the sound and the input run in an agent (here in
// the same process: threads of its own behind the link); the session keeps the
// connection. The agent comes and goes; the viewer stays.
void agents(const uint8_t (&key)[32]) {
    const std::string endpoint = agentEndpoint("agents");
    const auto linkKey = rcsdk::generateConnectionKey();
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [&] { return recorder.available ? RCS_PERMISSION_GRANTED : RCS_PERMISSION_REQUIRED; };
    overrideInput(std::move(input));
    // The agent's machine can follow secure desktops and shows a desktop of its own; Ctrl+Alt+Del is the session's.
    rcsdk::seams::setSecureCapabilities({true, false});
    std::atomic<int> desktop{0};
    rcsdk::seams::setDesktopState(
        [&] {
            return rcsdk::platform::DesktopState{rcs_desktop_kind(desktop == 0 ? RCS_DESKTOP_USER : RCS_DESKTOP_SECURE),
                                                 true};
        });
    std::atomic<int> attentions{0};
    rcsdk::seams::setSecureAttention([&] {
        ++attentions;
        return RCS_OK;
    });
    {
        std::mutex mutex;
        std::vector<float> received;
        std::atomic<int> frames{0}, encoded{0};
        EventLog shareEvents, viewEvents;
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        EncodedCallback output([&](const rcs_encoded_packet&) { ++encoded; });
        viewCallbacks.onEvent = viewEvents.callback();
        viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
        viewCallbacks.onAudio = [&](const rcs_audio_frame& frame) {
            std::lock_guard lock(mutex);
            for (uint32_t i = 0; i < frame.frames; ++i) {
                received.push_back(frame.samples[i * 2]);
            }
        };
        // The viewer leaving does not end the run: the agent stays for the next one.
        auto shareConfig = config(RCS_ROLE_SHARE, 47880, key)
                               .use(rcsdk::agentLink(endpoint, linkKey))
                               .use(rcsdk::encodedOutput(output))
                               .setControl(true);
        shareConfig.setKeepListening(true);
        rcsdk::SessionConfig viewConfig = config(RCS_ROLE_VIEW, 47880, key);
        rcsdk::Session share(shareConfig, shareCallbacks);

        // Nothing is offered before an agent attached: the session waits.
        CHECK(share.start() == RCS_OK);
        std::this_thread::sleep_for(400ms);
        CHECK(share.state() == RCS_STATE_STARTING && !rcsdk::agentInfo(share).attached);
        CHECK(share.displays().empty());
        const auto failsWith = [](rcs_result expected, const std::function<void()>& call) {
            try {
                call();
            } catch (const rcsdk::Error& error) {
                return error.result() == expected;
            }
            return false;
        };
        CHECK(failsWith(RCS_ERROR_INVALID_STATE, [&] { share.windows(); })); // No agent to ask yet.
        CHECK(shareEvents.of(RCS_EVENT_AGENT).empty());

        // The agent attaches; the port opens.
        std::mutex agentMutex;
        std::vector<rcs_state> agentStates;
        auto agent =
            std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey), [&](const rcs_agent_event& event) {
                std::lock_guard lock(agentMutex);
                agentStates.push_back(event.state.value);
            });
        CHECK(agent->start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(waitFor([&] { return agent->state() == RCS_STATE_RUNNING; }));
        const auto info = rcsdk::agentInfo(share);
        CHECK(info.attached && info.process_id == ownProcess() && info.platform == rcsdk::capabilities().platform);
        CHECK(info.capture_sources == rcsdk::capabilities().capture_sources);
        CHECK(info.hardware_encoders == rcsdk::capabilities().hardware_encoders &&
              info.software_encoders == rcsdk::capabilities().software_encoders);
        CHECK(info.input_permission == RCS_PERMISSION_GRANTED && info.displays == rcsdk::displays().size());
        CHECK(share.displays().size() == rcsdk::displays().size());
        // Its windows: asked of it, as its machine lists them.
        rcsdk::seams::setWindowList([] {
            std::vector<rcs_window_info> list(2);
            list[0].window_id = 7;
            list[1].window_id = 8;
            list[1].focused = 1;
            std::snprintf(list[1].title, sizeof(list[1].title), "%s", "Report");
            return list;
        });
        const auto windows = share.windows();
        CHECK(windows.size() == 2 && windows[1].window_id == 8 && windows[1].focused &&
              std::string(windows[1].title) == "Report");
        rcsdk::seams::setWindowList({});
        {
            const auto events = shareEvents.of(RCS_EVENT_AGENT);
            CHECK(events.size() == 1 && events[0].agent.attached && events[0].agent.process_id == ownProcess());
        }

        // A viewer: the agent captures and encodes; the session sends.
        rcsdk::Session view(viewConfig, viewCallbacks);
        CHECK(view.start() == RCS_OK);
        CHECK(failsWith(RCS_ERROR_UNSUPPORTED, [&] { view.windows(); })); // A viewer has none to share.
        CHECK(waitFor([&] { return frames >= 20; }));
        const auto stream = view.streamInfo();
        CHECK(stream.codec == RCS_CODEC_H264 && stream.width == 640 && stream.height == 360 &&
              stream.frames_per_second == 30);
        CHECK(waitFor([&] { return share.stats<rcs_capture_stats>().captured_frames >= 20; }));
        const auto sent = share.stats<rcs_video_send_stats>();
        CHECK(sent.encoded_frames >= 20 && encoded >= 20 && sent.encoder[0] && sent.keyframes >= 1);
        CHECK(sent.preview_frames == 0);
        CHECK(share.streamInfo().codec == RCS_CODEC_H264 && share.streamInfo().content_width == 640);
        {
            const auto codecs = shareEvents.of(RCS_EVENT_CODEC);
            CHECK(codecs.size() == 1 && codecs[0].codec.direction == RCS_CODEC_ENCODER && codecs[0].codec.name[0]);
            const auto streams = shareEvents.of(RCS_EVENT_STREAM);
            CHECK(!streams.empty() && streams[0].stream.info.display_width == rcsdk::test::patternControl().displayWidth);
        }
        CHECK(!shareEvents.overlapping && !viewEvents.overlapping);

        // The sound: captured by the agent, encoded by the session.
        CHECK(waitFor([&] {
            std::lock_guard lock(mutex);
            return received.size() >= 48000;
        }));
        {
            std::vector<float> tail;
            {
                std::lock_guard lock(mutex);
                tail.assign(received.end() - 24000, received.end());
            }
            const auto measured = rcsdk::test::measureTone(tail);
            CHECK(std::abs(measured.hz - rcsdk::test::toneHz) < 10 && std::abs(measured.rms - 0.354) < 0.05);
            const auto audio = share.stats<rcs_audio_send_stats>();
            CHECK(audio.active && audio.capture_period_frames == 480 && audio.encoded_packets > 0);
        }

        // Input: granted, executed by the agent, results counted. The grant
        // waits for the agent to say the system takes input: no "not granted"
        // first while that is unknown.
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        CHECK(waitFor([&] { return !shareEvents.of(RCS_EVENT_CONTROL).empty(); }));
        CHECK(shareEvents.of(RCS_EVENT_CONTROL).front().control.granted &&
              viewEvents.of(RCS_EVENT_CONTROL).front().control.granted);
        rcs_input_event press = RCS_INPUT_EVENT_INIT;
        press.type = RCS_INPUT_KEY;
        press.code = 4;
        press.down = 1;
        CHECK(view.sendInput(press) == RCS_OK);
        CHECK(waitFor([&] { return recorder.count() == 1; }));
        CHECK(waitFor([&] { return share.stats<rcs_input_stats>().applied == 1; }));
        CHECK(share.stats<rcs_input_stats>().received == 1);
        // A lock screen on the agent's machine pauses the grant; unlocking resumes it.
        recorder.available = false;
        CHECK(waitFor([&] { return !view.controlInfo().granted; }, 4s));
        CHECK(share.controlInfo().allowed && !share.controlInfo().granted);
        recorder.available = true;
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }, 4s));
        recorder.refuse = true; // An elevated window refuses one event: the result is the system's.
        CHECK(view.sendInput(press) == RCS_OK);
        CHECK(waitFor([&] { return share.stats<rcs_input_stats>().last_result == RCS_ERROR_PERMISSION_REQUIRED; }));
        recorder.refuse = false;

        // Ctrl+Alt+Del is sent by the session's own process (a service can; the user's session cannot): the
        // agent's injector never sees it.
        rcs_input_event attention = RCS_INPUT_EVENT_INIT;
        attention.type = RCS_INPUT_SECURE_ATTENTION;
        const size_t executed = recorder.count();
        CHECK(view.sendInput(attention) == RCS_OK);
        CHECK(waitFor([&] { return attentions == 1; }));
        std::this_thread::sleep_for(100ms);
        CHECK(recorder.count() == executed);
        // What the agent's machine shows reaches the host and the viewer through the link.
        CHECK(waitFor([&] {
            return view.desktopInfo().kind == RCS_DESKTOP_USER && share.desktopInfo().kind == RCS_DESKTOP_USER;
        }));
        CHECK(rcsdk::agentInfo(share).secure_desktop == 1);
        desktop = 1;
        CHECK(waitFor([&] {
            return view.desktopInfo().kind == RCS_DESKTOP_SECURE && share.desktopInfo().kind == RCS_DESKTOP_SECURE;
        }));
        desktop = 0;
        CHECK(waitFor([&] { return view.desktopInfo().kind == RCS_DESKTOP_USER; }));

        // The agent leaves: the viewer stays connected; the picture waits, control and sound pause.
        const int before = frames;
        agent.reset();
        CHECK(waitFor([&] { return !rcsdk::agentInfo(share).attached; }));
        CHECK(waitFor([&] {
            const auto events = shareEvents.of(RCS_EVENT_CAPTURE);
            return !events.empty() && events.back().capture.paused;
        }));
        CHECK(waitFor([&] { return !view.controlInfo().granted; }));
        CHECK(waitFor([&] { return !share.stats<rcs_audio_send_stats>().active; }));
        std::this_thread::sleep_for(600ms);
        CHECK(view.state() == RCS_STATE_RUNNING && share.state() == RCS_STATE_RUNNING);
        const int stalled = frames;
        std::this_thread::sleep_for(500ms);
        CHECK(frames - stalled <= 2 && stalled >= before);
        CHECK(view.sendInput(press) == RCS_ERROR_INVALID_STATE);
        {
            const auto events = shareEvents.of(RCS_EVENT_AGENT);
            CHECK(events.size() == 2 && !events.back().agent.attached);
        }
        CHECK(waitFor([&] { return share.desktopInfo().kind == RCS_DESKTOP_UNKNOWN; })); // Nothing is known now.

        // Another agent: the stream starts again by itself, from a keyframe, and everything works.
        agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
        CHECK(agent->start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= stalled + 20; }));
        CHECK(waitFor([&] {
            const auto events = shareEvents.of(RCS_EVENT_CAPTURE);
            return !events.empty() && !events.back().capture.paused;
        }));
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        CHECK(waitFor([&] { return share.stats<rcs_audio_send_stats>().active; }));
        const size_t pressed = recorder.count();
        CHECK(view.sendInput(press) == RCS_OK);
        CHECK(waitFor([&] { return recorder.count() > pressed; }));
        CHECK(rcsdk::agentInfo(share).attached);
        CHECK(shareEvents.of(RCS_EVENT_AGENT).size() == 3);

        // A viewer that leaves and another that comes: the agent stays.
        CHECK(view.stop() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(agent->state() == RCS_STATE_RUNNING);
        std::atomic<int> second{0};
        rcsdk::Callbacks secondCallbacks;
        secondCallbacks.onFrame = [&](const rcs_frame&) { ++second; };
        rcsdk::Session another(viewConfig, secondCallbacks);
        CHECK(another.start() == RCS_OK);
        CHECK(waitFor([&] { return second >= 20; }));
        CHECK(another.stop() == RCS_OK);

        // A viewer that comes while no agent is attached connects all the same, and gets the
        // picture once one attaches.
        agent.reset();
        CHECK(waitFor([&] { return !rcsdk::agentInfo(share).attached; }));
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        std::atomic<int> waiting{0};
        rcsdk::Callbacks waitingCallbacks;
        waitingCallbacks.onFrame = [&](const rcs_frame&) { ++waiting; };
        rcsdk::Session early(viewConfig, waitingCallbacks);
        CHECK(early.start() == RCS_OK);
        CHECK(waitFor([&] { return early.state() == RCS_STATE_RUNNING && share.state() == RCS_STATE_RUNNING; }));
        std::this_thread::sleep_for(300ms);
        CHECK(waiting == 0);
        agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
        CHECK(agent->start() == RCS_OK);
        CHECK(waitFor([&] { return waiting >= 20; }));
        CHECK(early.stop() == RCS_OK);

        // Stopping the session sends the agent looking for the next one; starting it again attaches it.
        CHECK(share.stop() == RCS_OK);
        CHECK(waitFor([&] { return agent->state() == RCS_STATE_RECONNECTING; }));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING && agent->state() == RCS_STATE_RUNNING; }));
        rcsdk::Session third(viewConfig, secondCallbacks);
        const int beforeThird = second;
        CHECK(third.start() == RCS_OK);
        CHECK(waitFor([&] { return second >= beforeThird + 20; }));
        CHECK(third.stop() == RCS_OK && share.stop() == RCS_OK);
    }

    // What an agent cannot be given is refused where it is known.
    {
        // The host's own pictures are encoded in the session's process.
        CHECK(throws<rcsdk::Error>([&] {
            rcsdk::Session invalid(config(RCS_ROLE_SHARE, 47881, key)
                                       .use(rcsdk::agentLink(endpoint, linkKey))
                                       .setCaptureSource(RCS_CAPTURE_HOST));
        }));
        // A picture that does not leave the agent's process cannot be previewed.
        rcsdk::Callbacks preview;
        preview.onFrame = [](const rcs_frame&) {};
        rcsdk::Session previewing(config(RCS_ROLE_SHARE, 47881, key).use(rcsdk::agentLink(endpoint, linkKey)), preview);
        CHECK(previewing.start() == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(previewing.failure().message.find("RCS_EXT_AGENT_LINK") != std::string::npos);
        // The agent is a sharer's.
        CHECK(throws<rcsdk::Error>([&] {
            rcsdk::SessionConfig view(RCS_ROLE_VIEW);
            view.setAddress("127.0.0.1", 47881).setKey(key);
            view.use(rcsdk::defaults<rcs_agent_link_config>());
            rcsdk::Session invalid(view);
        }));
        // An endpoint needs its key; a key needs an endpoint the system accepts.
        CHECK(throws<rcsdk::Error>([&] {
            rcsdk::SessionConfig bad = config(RCS_ROLE_SHARE, 47881, key);
            std::strcpy(bad.extension<rcs_agent_link_config>().endpoint, endpoint.c_str());
            rcsdk::Session invalid(bad);
        }));
        CHECK(throws<rcsdk::Error>([&] {
            rcsdk::Session invalid(config(RCS_ROLE_SHARE, 47881, key).use(rcsdk::agentLink("relative/endpoint", linkKey)));
        }));
        // Fixed for a run, like the transport.
        rcsdk::Session running(config(RCS_ROLE_SHARE, 47881, key).use(rcsdk::agentLink(endpoint, linkKey)));
        CHECK(running.start() == RCS_OK);
        rcs_agent_link_config other = rcsdk::defaults<rcs_agent_link_config>();
        CHECK(running.update(other) == RCS_ERROR_INVALID_STATE);
        CHECK(running.stop() == RCS_OK);
    }

    // The agent's machine decides what the settings can do: an agent that cannot
    // carry them out ends the run with the reason, once it attached.
    {
        const auto caps = rcsdk::capabilities();
        if (!(caps.hardware_encoders & (1u << RCS_CODEC_AV1)) && !(caps.software_encoders & (1u << RCS_CODEC_AV1))) {
            EventLog events;
            rcsdk::Callbacks callbacks;
            callbacks.onEvent = events.callback();
            rcsdk::Session share(config(RCS_ROLE_SHARE, 47882, key).use(rcsdk::agentLink(endpoint, linkKey)).setCodecs({RCS_CODEC_AV1}),
                                 callbacks);
            CHECK(share.start() == RCS_OK); // The agent's machine is not known yet.
            rcsdk::Agent agent(rcsdk::AgentConfig(endpoint, linkKey).setReconnect(true, 100));
            CHECK(agent.start() == RCS_OK);
            CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }));
            CHECK(share.failure().result == RCS_ERROR_UNSUPPORTED);
            CHECK(share.failure().message.find("AV1") != std::string::npos);
            CHECK(waitFor([&] { return agent.state() == RCS_STATE_RECONNECTING; })); // The session is gone; it looks on.
        }
    }

    // An agent without the session's key never attaches: the session keeps waiting.
    {
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47883, key).use(rcsdk::agentLink(endpoint, linkKey)));
        CHECK(share.start() == RCS_OK);
        auto wrong = linkKey;
        wrong[0] ^= 1;
        rcsdk::Agent stranger(rcsdk::AgentConfig(endpoint, wrong).setReconnect(false));
        CHECK(stranger.start() == RCS_OK);
        CHECK(stranger.wait(8000) == RCS_OK);
        CHECK(stranger.state() == RCS_STATE_FAILED || stranger.state() == RCS_STATE_STOPPED);
        CHECK(stranger.failure().result != RCS_OK);
        CHECK(share.state() == RCS_STATE_STARTING && !rcsdk::agentInfo(share).attached);
        CHECK(share.stop() == RCS_OK);
        // Calls on an agent that never ran.
        rcsdk::Agent idle{rcsdk::AgentConfig(endpoint, linkKey)};
        CHECK(idle.wait(10) == RCS_ERROR_INVALID_STATE);
        CHECK(idle.stop() == RCS_OK);
        rcs_agent_config bad = RCS_AGENT_CONFIG_INIT;
        rcs_agent handle = nullptr;
        CHECK(rcs_agent_create(&bad, nullptr, &handle) == RCS_ERROR_INVALID_ARGUMENT); // No endpoint, no key.
        bad.version = RCSDK_VERSION + 1;
        CHECK(rcs_agent_create(&bad, nullptr, &handle) == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(rcs_agent_get_state(nullptr, nullptr) == RCS_ERROR_INVALID_ARGUMENT);
    }
    rcsdk::seams::setInputOverride({});
    rcsdk::seams::setDesktopState({});
    rcsdk::seams::setSecureAttention({});
    rcsdk::seams::setSecureCapabilities({});
    // A viewing session has no agent.
    {
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47884, key));
        rcs_agent_info info = RCS_AGENT_INFO_INIT;
        CHECK(rcs_session_get_agent_info(view.handle(), &info) == RCS_ERROR_UNSUPPORTED);
        uint32_t count = 0;
        CHECK(rcs_session_list_displays(view.handle(), nullptr, sizeof(rcs_display_info), 0, &count) ==
              RCS_ERROR_UNSUPPORTED);
        // A sharer without an agent link: its own process is the agent.
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47884, key));
        CHECK(rcsdk::agentInfo(share).attached && rcsdk::agentInfo(share).process_id == ownProcess());
    }
}

// Capture pauses (lock screen, secure desktop) keep the session connected;
// after a display mode change the new letterboxing is announced.
void captureInterruption(const uint8_t (&key)[32]) {
    auto& pattern = rcsdk::test::patternControl();
    std::atomic<int> frames{0};
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47833, key), shareCallbacks);
    rcsdk::Callbacks callbacks;
    callbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
    };
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47833, key), callbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10; }));
    CHECK(view.streamInfo().content_width == 640);

    pattern.interruptions = pattern.interruptions + 1;
    pattern.paused = RCS_CAPTURE_PAUSE_DESKTOP;
    CHECK(waitFor([&] { return share.stats<rcs_capture_stats>().paused == RCS_CAPTURE_PAUSE_DESKTOP; }));
    // Longer than the 5 s idle timeout: heartbeats keep both ends connected.
    std::this_thread::sleep_for(6s);
    CHECK(share.state() == RCS_STATE_RUNNING && view.state() == RCS_STATE_RUNNING);

    pattern.contentWidth = 480;
    pattern.paused = RCS_CAPTURE_PAUSE_NONE;
    const int before = frames;
    CHECK(waitFor([&] { return frames >= before + 10; }));
    CHECK(view.streamInfo().content_width == 480 && view.streamInfo().content_x == 80);
    const auto stats = share.stats<rcs_capture_stats>();
    CHECK(stats.paused == RCS_CAPTURE_PAUSE_NONE && stats.interruptions == 1);
    // What is shared closes: the reason is told, and the run stays.
    pattern.paused = RCS_CAPTURE_PAUSE_GONE;
    CHECK(waitFor([&] { return share.stats<rcs_capture_stats>().paused == RCS_CAPTURE_PAUSE_GONE; }));
    CHECK(share.state() == RCS_STATE_RUNNING && view.state() == RCS_STATE_RUNNING);
    pattern.paused = RCS_CAPTURE_PAUSE_NONE;
    CHECK(waitFor([&] { return share.stats<rcs_capture_stats>().paused == RCS_CAPTURE_PAUSE_NONE; }));
    const auto pauses = shareEvents.of(RCS_EVENT_CAPTURE);
    CHECK(pauses.size() == 4 && pauses[0].capture.paused == RCS_CAPTURE_PAUSE_DESKTOP &&
          pauses[1].capture.paused == RCS_CAPTURE_PAUSE_NONE && pauses[2].capture.paused == RCS_CAPTURE_PAUSE_GONE &&
          pauses[3].capture.paused == RCS_CAPTURE_PAUSE_NONE);
    const auto streams = shareEvents.of(RCS_EVENT_STREAM);
    const auto pictures = [&] {
        std::vector<rcs_stream_event> result;
        for (const auto& event : streams) {
            if (event.stream.changes & ~uint32_t(RCS_STREAM_AUDIO)) {
                result.push_back(event.stream);
            }
        }
        return result;
    }();
    CHECK(pictures.size() == 2 && pictures[1].info.content_width == 480 && pictures[1].changes == RCS_STREAM_CONTENT);
    pattern.contentWidth = 0;
}

// Configurations the system cannot honour are refused by start(), with a reason.
void refusals(const uint8_t (&key)[32]) {
    const auto caps = rcsdk::capabilities();
    // Images go to one callback.
    rcsdk::Callbacks both;
    both.onFrame = [](const rcs_frame&) {
    };
    both.onGpuFrame = [](const rcsdk::GpuFrame&) {
    };
    CHECK(throws<rcsdk::Error>([&] { rcsdk::Session invalid(config(RCS_ROLE_VIEW, 47830, key), both); }));

    // Invalid requests, policies and cursor modes are refused at creation, naming the field.
    const auto invalid = [&](rcsdk::SessionConfig c, const char* field) {
        try {
            rcsdk::Session session(c);
        } catch (const rcsdk::Error& error) {
            return error.result() == RCS_ERROR_INVALID_ARGUMENT && std::strstr(error.what(), field);
        }
        return false;
    };
    CHECK(invalid(config(RCS_ROLE_VIEW, 47830, key).setVideoRequest(1280, 0), "width and height"));
    CHECK(invalid(config(RCS_ROLE_VIEW, 47830, key).setVideoRequest(1280, 720, 241), "frames_per_second"));
    CHECK(invalid(config(RCS_ROLE_VIEW, 47830, key).setVideoRequest(1280, 720, 60, 1000), "bitrate_bps"));
    CHECK(invalid(config(RCS_ROLE_VIEW, 47830, key).setVideoRequest(0, 0, 0, 0, RCS_CODEC_HEVC), "codecs[]"));
    CHECK(invalid(config(RCS_ROLE_VIEW, 47830, key).setCursorMode(2), "mode"));
    auto audioRequest = config(RCS_ROLE_VIEW, 47830, key).setAudioCodecs({RCS_AUDIO_OPUS});
    audioRequest.viewRequest().audio_codec = RCS_AUDIO_PCM;
    CHECK(invalid(audioRequest, "audio_codec"));
    audioRequest.viewRequest().audio_codec = 0;
    audioRequest.viewRequest().audio_bitrate_bps = 1000;
    CHECK(invalid(audioRequest, "audio_bitrate_bps"));
    CHECK(invalid(config(RCS_ROLE_SHARE, 47830, key).setCursorMode(RCS_CURSOR_SEPARATE), "viewing sessions"));
    CHECK(invalid(config(RCS_ROLE_VIEW, 47830, key).setRequestPolicy(true), "sharing sessions"));
    CHECK(invalid(config(RCS_ROLE_SHARE, 47830, key).setRequestPolicy(true, 1281), "max_width"));
    CHECK(invalid(config(RCS_ROLE_SHARE, 47830, key).setRequestPolicy(true, 0, 0, 241), "max_frames_per_second"));
    // What is captured: a region needs a size, a window its id, an
    // application its process; following secure desktops is for displays
    // and regions (windows show on the user's desktop only).
    CHECK(invalid(config(RCS_ROLE_SHARE, 47830, key).setCaptureRegion(0, 0, 0, 8, 600), "region_width"));
    CHECK(invalid(config(RCS_ROLE_SHARE, 47830, key).setCaptureWindow(0), "window_id"));
    CHECK(invalid(config(RCS_ROLE_SHARE, 47830, key).setCaptureApplication(0), "process_id"));
    CHECK(invalid(config(RCS_ROLE_SHARE, 47830, key).setCaptureWindow(42).setSecureDesktop(true), "secure_desktop"));
    auto unknownSource = config(RCS_ROLE_SHARE, 47830, key);
    unknownSource.capture().source = RCS_CAPTURE_ENCODED + 1;
    CHECK(invalid(unknownSource, "source"));
    // A region must lie inside its display, as the display is now (checked
    // against the machine: without the test pattern).
    const auto screens = rcsdk::displays();
    const auto main = std::find_if(screens.begin(), screens.end(), [](const auto& d) { return d.is_main; });
    if (rcsdk::screenCapturePermission() == RCS_PERMISSION_GRANTED && main != screens.end()) {
        rcsdk::seams::setCaptureFactory({});
        rcsdk::Session outside(
            config(RCS_ROLE_SHARE, 47830, key).setCaptureRegion(0, main->pixel_width - 100, 0, 200, 200));
        CHECK(outside.start() == RCS_ERROR_INVALID_ARGUMENT &&
              outside.failure().message.find("not inside") != std::string::npos);
        rcsdk::seams::setCaptureFactory(
            [](const auto&) { return std::make_unique<rcsdk::test::TestPatternCapture>(); });
    }

    // No platform encodes HEVC in software: refused, not replaced by another
    // codec or by hardware encoding.
    rcsdk::Session share(
        config(RCS_ROLE_SHARE, 47830, key).setCodecs({RCS_CODEC_HEVC}).setAcceleration(RCS_ACCELERATION_SOFTWARE));
    CHECK(share.start() == RCS_ERROR_UNSUPPORTED && share.state() == RCS_STATE_IDLE);
    CHECK(share.failure().result == RCS_ERROR_UNSUPPORTED && share.failure().message.find("HEVC") != std::string::npos);

    // A backend of another platform is refused, not replaced by this platform's.
    const rcs_backend foreign =
        caps.platform == RCS_PLATFORM_WINDOWS ? RCS_BACKEND_VIDEOTOOLBOX : RCS_BACKEND_MEDIA_FOUNDATION;
    rcsdk::Session elsewhere(config(RCS_ROLE_VIEW, 47830, key).setBackend(foreign));
    CHECK(elsewhere.start() == RCS_ERROR_UNSUPPORTED && elsewhere.failure().message.find("backend") != std::string::npos);

    // The C++ builder refuses what the C structures cannot hold instead of cutting it short.
    const auto refused = [](const std::function<void(rcsdk::SessionConfig&)>& set, const char* field) {
        rcsdk::SessionConfig c(RCS_ROLE_VIEW);
        try {
            set(c);
        } catch (const rcsdk::Error& error) {
            return error.result() == RCS_ERROR_INVALID_ARGUMENT && std::strstr(error.what(), field);
        }
        return false;
    };
    const std::string long128(128, 'a');
    CHECK(refused([](auto& c) { c.setStunServers({"a:1", "b:1", "c:1", "d:1", "e:1"}); }, "stun_servers"));
    CHECK(refused([&](auto& c) { c.setStunServers({long128}); }, "stun_servers"));
    CHECK(refused([&](auto& c) { c.setRelay(long128, "user", "pass"); }, "turn_server"));
    CHECK(refused([&](auto& c) { c.setRelay("turn.example.com:3478", "user", long128); }, "turn_password"));
    CHECK(refused([&](auto& c) { c.setRelayServerName(long128); }, "turn_tls_server_name"));
    CHECK(refused([&](auto& c) { c.setAddress(long128, 1); }, "address"));
    CHECK(refused([](auto& c) { c.setCodecs({1, 2, 3, 16, 17, 18, 19, 20, 21}); }, "codecs")); // RCS_MAX_CODECS is 8.
    rcsdk::SessionConfig fits(RCS_ROLE_VIEW);
    fits.setStunServers({"a:1", "b:1", "c:1", "d:1"}).setRelay(std::string(127, 't'), "user", "pass");
    CHECK(std::strlen(fits.iceServers().turn_server) == 127 && fits.iceServers().stun_servers[3][0] == 'd');
}

// ---- Over the host's own channel (RCS_TRANSPORT_HOST) ------------------------------------------------

rcsdk::SessionConfig hostConfig(rcs_role role, const uint8_t (&key)[32], rcs_host_channel kind,
                                rcsdk::HostTransport& transport, uint32_t maxDatagram = 0) {
    auto c = config(role, 0, key);
    c.setTransport(RCS_TRANSPORT_HOST).use(rcsdk::hostTransport(transport, kind, maxDatagram));
    return c;
}

// What one end's on_data heard: reliable messages by channel, in order, and
// unreliable datagrams.
struct DataLog {
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> reliable, unreliable;
    std::function<void(const rcs_data_message&)> callback() {
        return [this](const rcs_data_message& m) {
            std::lock_guard lock(mutex);
            (m.flags & RCS_DATA_UNRELIABLE ? unreliable : reliable).emplace_back(m.data, m.data + m.size);
        };
    }
    size_t count(bool reliableOnes) {
        std::lock_guard lock(mutex);
        return (reliableOnes ? reliable : unreliable).size();
    }
};

std::vector<uint8_t> bytesOf(size_t size, uint8_t seed) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = uint8_t(seed * 31 + i * 7 + (i >> 9));
    }
    return bytes;
}

// A datagram channel that loses 5%, reorders 10% and repeats 2% of what it
// carries: the picture keeps coming, reliable data arrives whole and in order,
// unreliable data mostly arrives, and both ends name the path.
void hostDatagram(const uint8_t (&key)[32]) {
    rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_DATAGRAM);
    channel.loss = 0.05;
    channel.reorder = 0.10;
    channel.repeat = 0.02;
    std::atomic<int> frames{0};
    DataLog shareData;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    rcsdk::test::FunctionTransport shareSend(channel.sender(0));
    shareCallbacks.onData = shareData.callback();
    rcsdk::test::FunctionTransport viewSend(channel.sender(1));
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    rcsdk::Session share(hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_DATAGRAM, shareSend), shareCallbacks);
    rcsdk::Session view(hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_DATAGRAM, viewSend), viewCallbacks);
    const auto shareEnd = channel.attachment(0, share);
    const auto viewEnd = channel.attachment(1, view);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 30; }, 15s));
    for (uint8_t i = 0; i < 40; ++i) {
        const auto message = bytesOf(3000 + i * 100, i);
        CHECK(view.sendData(2, message.data(), uint32_t(message.size())) == RCS_OK);
        const auto brief = bytesOf(200, i);
        CHECK(view.sendData(3, brief.data(), uint32_t(brief.size()), RCS_DATA_UNRELIABLE) == RCS_OK);
    }
    CHECK(waitFor([&] { return shareData.count(true) == 40; }, 10s));
    {
        std::lock_guard lock(shareData.mutex);
        for (uint8_t i = 0; i < 40; ++i) {
            CHECK(shareData.reliable[i] == bytesOf(3000 + i * 100, i));
        }
        CHECK(shareData.unreliable.size() >= 20 && shareData.unreliable.size() <= 40);
    }
    for (auto* session : {&share, &view}) {
        const auto network = session->stats<rcs_network_stats>();
        CHECK(std::string(network.path) == "host datagram channel" && network.round_trip_ns > 0);
        CHECK(network.host_refused == 0 && network.connections == 1);
    }
    CHECK(view.stats<rcs_video_receive_stats>().decoded_frames >= 30);
    CHECK(channel.unexpected == 0);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// A stream cut into pieces of 1..3000 bytes; the sharer sends datagrams of up
// to 16 KiB, the viewer of 1200: a megabyte arrives whole, both ways.
void hostStream(const uint8_t (&key)[32]) {
    rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_STREAM);
    channel.maxPiece = 3000;
    std::atomic<int> frames{0};
    DataLog shareData, viewData;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    rcsdk::test::FunctionTransport shareSend(channel.sender(0));
    shareCallbacks.onData = shareData.callback();
    rcsdk::test::FunctionTransport viewSend(channel.sender(1));
    viewCallbacks.onData = viewData.callback();
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    rcsdk::Session share(hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_STREAM, shareSend, 16384), shareCallbacks);
    rcsdk::Session view(hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_STREAM, viewSend), viewCallbacks);
    const auto shareEnd = channel.attachment(0, share);
    const auto viewEnd = channel.attachment(1, view);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 30; }, 15s));
    const auto big = bytesOf(1u << 20, 9);
    CHECK(share.sendData(1, big.data(), uint32_t(big.size())) == RCS_OK);
    CHECK(view.sendData(1, big.data(), uint32_t(big.size())) == RCS_OK);
    const auto unreliable = bytesOf(RCS_DATA_MAX_DATAGRAM, 3);
    CHECK(view.sendData(4, unreliable.data(), uint32_t(unreliable.size()), RCS_DATA_UNRELIABLE) == RCS_OK);
    CHECK(waitFor([&] { return viewData.count(true) == 1 && shareData.count(true) == 1 && shareData.count(false) == 1; },
                  15s));
    CHECK(viewData.reliable[0] == big && shareData.reliable[0] == big && shareData.unreliable[0] == unreliable);
    CHECK(std::string(view.stats<rcs_network_stats>().path) == "host stream channel");
    CHECK(channel.unexpected == 0);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// The host's channel closes under a connection: the viewer (not reconnecting)
// fails with NETWORK, the sharer listens again and serves the next viewer.
// While it waits, a close changes nothing.
void hostKeepListening(const uint8_t (&key)[32]) {
    rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_DATAGRAM);
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    rcsdk::test::FunctionTransport shareSend(channel.sender(0));
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_DATAGRAM, shareSend).setKeepListening(true),
                         shareCallbacks);
    const auto shareEnd = channel.attachment(0, share);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    channel.close(); // No viewer yet: still waiting.
    std::this_thread::sleep_for(200ms);
    CHECK(share.state() == RCS_STATE_LISTENING);
    for (const int connection : {1, 2}) {
        std::atomic<int> frames{0};
        rcsdk::Callbacks viewCallbacks;
        rcsdk::test::FunctionTransport viewSend(channel.sender(1));
        viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
        rcsdk::Session view(hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_DATAGRAM, viewSend), viewCallbacks);
        const auto viewEnd = channel.attachment(1, view);
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 10; }, 10s));
        CHECK(share.stats<rcs_network_stats>().connections == uint64_t(connection));
        channel.close();
        CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }));
        CHECK(view.failure().result == RCS_ERROR_NETWORK &&
              view.failure().message.find("host's channel") != std::string::npos);
        CHECK(waitFor([&] {
            const auto states = shareEvents.of(RCS_EVENT_STATE);
            return !states.empty() && states.back().state.value == RCS_STATE_LISTENING &&
                   states.back().state.result == RCS_ERROR_NETWORK;
        }));
    }
    CHECK(share.stop() == RCS_OK);
}

// A viewer that reconnects: the channel closes, the viewer goes to
// RECONNECTING and comes back over the same channel at once; the picture
// resumes. Both ends count two connections.
void hostReconnect(const uint8_t (&key)[32]) {
    rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_STREAM);
    std::atomic<int> frames{0};
    EventLog viewEvents;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    rcsdk::test::FunctionTransport shareSend(channel.sender(0));
    rcsdk::test::FunctionTransport viewSend(channel.sender(1));
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    viewCallbacks.onEvent = viewEvents.callback();
    rcsdk::Session share(hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_STREAM, shareSend).setKeepListening(true),
                         shareCallbacks);
    rcsdk::Session view(reconnecting(hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_STREAM, viewSend), 10000, 100),
                        viewCallbacks);
    const auto shareEnd = channel.attachment(0, share);
    const auto viewEnd = channel.attachment(1, view);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10; }, 10s));
    channel.close();
    CHECK(waitFor([&] {
        const auto states = viewEvents.of(RCS_EVENT_STATE);
        return std::any_of(states.begin(), states.end(), [](const rcs_event& e) {
            return e.state.value == RCS_STATE_RECONNECTING && e.state.result == RCS_ERROR_NETWORK;
        });
    }));
    const int before = frames;
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING && frames >= before + 10; }, 10s));
    CHECK(view.stats<rcs_network_stats>().connections == 2 && share.stats<rcs_network_stats>().connections == 2);
    CHECK(channel.unexpected == 0);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// A channel that has no room for half of what is sent for a while: the
// connection takes it as loss, the adaptive bitrate comes down, the picture
// keeps coming, and the refusals are counted.
void hostBackpressure(const uint8_t (&key)[32]) {
    rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_DATAGRAM);
    rcsdk::test::patternControl().grain = true; // Content that needs the bitrate.
    std::atomic<int> frames{0};
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    rcsdk::test::FunctionTransport shareSend(channel.sender(0));
    rcsdk::test::FunctionTransport viewSend(channel.sender(1));
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    rcsdk::Session share(hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_DATAGRAM, shareSend).setVideo(640, 360, 30, 4'000'000),
                         shareCallbacks);
    rcsdk::Session view(hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_DATAGRAM, viewSend), viewCallbacks);
    const auto shareEnd = channel.attachment(0, share);
    const auto viewEnd = channel.attachment(1, view);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 20; }, 10s));
    const uint32_t clear = share.stats<rcs_video_send_stats>().bitrate_bps;
    channel.refuse = 0.3;
    const int before = frames;
    std::this_thread::sleep_for(4s);
    const auto network = share.stats<rcs_network_stats>();
    const uint32_t squeezed = share.stats<rcs_video_send_stats>().bitrate_bps;
    std::printf("host backpressure: %u -> %u kbit/s, %llu refused, %d frames in 4 s\n", clear / 1000, squeezed / 1000,
                (unsigned long long)network.host_refused, frames - before);
    // The picture keeps coming (at a rate the machine's load decides: the
    // whole suite shares the hardware encoder).
    CHECK(waitFor([&] { return frames - before >= 10; }, 4s));
    channel.refuse = 0;
    CHECK(network.host_refused > 100 && network.host_refused <= channel.refused);
    CHECK(squeezed < clear);
    CHECK(view.state() == RCS_STATE_RUNNING && share.state() == RCS_STATE_RUNNING);
    rcsdk::test::patternControl().grain = false;
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// Calls that do not fit, a wrong key, and a viewer whose channel reaches nobody.
void hostErrors(const uint8_t (&key)[32]) {
    const uint8_t byte = 1;
    {
        // Another transport; a session that does not run.
        rcsdk::Session udp(config(RCS_ROLE_VIEW, 47898, key), {});
        CHECK(rcsdk::transportReceive(udp, 0, &byte, 1) == RCS_ERROR_UNSUPPORTED &&
              rcsdk::transportClosed(udp, 0) == RCS_ERROR_UNSUPPORTED);
        rcsdk::test::FunctionTransport sink([](uint64_t, const uint8_t*, uint32_t) { return true; });
        rcsdk::Session idle(hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_DATAGRAM, sink), {});
        CHECK(rcsdk::transportReceive(idle, 7, &byte, 1) == RCS_ERROR_INVALID_STATE);
        CHECK(rcsdk::transportClosed(idle, 7) == RCS_ERROR_INVALID_STATE);
        CHECK(idle.start() == RCS_OK);
        CHECK(waitFor([&] { return idle.state() == RCS_STATE_LISTENING; }));
        CHECK(rcsdk::transportReceive(idle, 7, &byte, 0) == RCS_ERROR_INVALID_ARGUMENT);
        const std::vector<uint8_t> tooLarge(RCS_HOST_MAX_DATAGRAM + 1);
        CHECK(rcsdk::transportReceive(idle, 7, tooLarge.data(), uint32_t(tooLarge.size())) ==
              RCS_ERROR_INVALID_ARGUMENT);
        CHECK(rcsdk::transportReceive(idle, 7, &byte, 1) == RCS_OK && rcsdk::transportClosed(idle, 7) == RCS_OK);
        // A route the sharer never heard of closes or restarts as well: nothing is there.
        CHECK(rcsdk::transportClosed(idle, 8) == RCS_OK && rcsdk::transportRestarted(idle, 8) == RCS_OK);
        // The channel is fixed for a run.
        CHECK(idle.update(rcsdk::hostTransport(sink, RCS_HOST_CHANNEL_STREAM)) == RCS_ERROR_INVALID_STATE);
        CHECK(rcsdk::lastCallError().find("RCS_EXT_HOST_TRANSPORT") != std::string::npos);
        CHECK(idle.update(rcsdk::hostTransport(sink, RCS_HOST_CHANNEL_DATAGRAM, 4000)) == RCS_ERROR_INVALID_STATE);
        CHECK(idle.stop() == RCS_OK);
        CHECK(rcsdk::transportReceive(idle, 7, &byte, 1) == RCS_ERROR_INVALID_STATE);
    }
    {
        // A viewer's channel leads to the sharer alone: route 0.
        rcsdk::test::FunctionTransport sink([](uint64_t, const uint8_t*, uint32_t) { return true; });
        rcsdk::Session view(hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_DATAGRAM, sink), {});
        CHECK(view.start() == RCS_OK);
        CHECK(rcsdk::transportReceive(view, 1, &byte, 1) == RCS_ERROR_INVALID_ARGUMENT &&
              rcsdk::transportClosed(view, 1) == RCS_ERROR_INVALID_ARGUMENT &&
              rcsdk::transportRestarted(view, 1) == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(rcsdk::lastCallError().find("route 0") != std::string::npos);
        CHECK(rcsdk::transportReceive(view, 0, &byte, 1) == RCS_OK);
        CHECK(view.stop() == RCS_OK);
    }
    {
        // HOST needs the host's channel: rcs_session_create says so.
        auto missing = config(RCS_ROLE_VIEW, 0, key);
        missing.setTransport(RCS_TRANSPORT_HOST);
        try {
            rcsdk::Session session(missing, {});
            CHECK(false);
        } catch (const rcsdk::Error& error) {
            CHECK(error.result() == RCS_ERROR_INVALID_ARGUMENT &&
                  std::string(error.what()).find("RCS_EXT_HOST_TRANSPORT") != std::string::npos);
        }
    }
    {
        // Another key: the viewer learns why within a few seconds.
        rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_DATAGRAM);
        uint8_t other[32];
        std::memcpy(other, key, 32);
        other[0] ^= 1;
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        rcsdk::test::FunctionTransport shareSend(channel.sender(0));
        rcsdk::test::FunctionTransport viewSend(channel.sender(1));
        rcsdk::Session share(hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_DATAGRAM, shareSend), shareCallbacks);
        rcsdk::Session view(hostConfig(RCS_ROLE_VIEW, other, RCS_HOST_CHANNEL_DATAGRAM, viewSend), viewCallbacks);
        const auto shareEnd = channel.attachment(0, share);
        const auto viewEnd = channel.attachment(1, view);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }, 8s));
        CHECK(view.failure().result == RCS_ERROR_AUTHENTICATION);
        // The sharer counts the attempt once it expires (5 s after its hello), and keeps listening.
        CHECK(waitFor([&] { return share.stats<rcs_network_stats>().rejected_connections >= 1; }, 8s));
        CHECK(share.state() == RCS_STATE_LISTENING);
        CHECK(share.stop() == RCS_OK);
    }
    {
        // Nobody at the other end: no answer within 5 s, and the reason says where to look.
        rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_DATAGRAM);
        rcsdk::Callbacks viewCallbacks;
        rcsdk::test::FunctionTransport viewSend(channel.sender(1));
        rcsdk::Session view(hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_DATAGRAM, viewSend), viewCallbacks);
        const auto viewEnd = channel.attachment(1, view);
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }, 8s));
        CHECK(view.failure().result == RCS_ERROR_NETWORK &&
              view.failure().message.find("host's channel does not reach it") != std::string::npos);
    }
}

// The host delivers inside on_transport_send, straight into the other
// session (allowed); lifecycle calls from it are refused, as from every callback.
void hostInline(const uint8_t (&key)[32]) {
    rcsdk::Session* shareEnd = nullptr;
    rcsdk::Session* viewEnd = nullptr;
    std::atomic<int> frames{0};
    std::atomic<rcs_result> fromCallback{RCS_OK};
    std::atomic<bool> tryStop{false};
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    rcsdk::test::FunctionTransport shareSend([&](uint64_t route, const uint8_t* data, uint32_t size) {
        if (tryStop.exchange(false)) {
            fromCallback = shareEnd->stop();
        }
        return route == 5 && rcsdk::transportReceive(*viewEnd, 0, data, size) == RCS_OK;
    });
    rcsdk::test::FunctionTransport viewSend([&](uint64_t route, const uint8_t* data, uint32_t size) {
        return !route && rcsdk::transportReceive(*shareEnd, 5, data, size) == RCS_OK;
    });
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    rcsdk::Session share(hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_STREAM, shareSend), shareCallbacks);
    rcsdk::Session view(hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_STREAM, viewSend), viewCallbacks);
    shareEnd = &share;
    viewEnd = &view;
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 20; }, 10s));
    tryStop = true;
    CHECK(waitFor([&] { return !tryStop; }));
    CHECK(fromCallback == RCS_ERROR_CALLBACK_THREAD && share.state() == RCS_STATE_RUNNING);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

void failures(const uint8_t (&key)[32]) {
    // Nobody listening: the viewer fails with a network error and can restart.
    std::atomic<int> failedStates{0};
    rcsdk::Callbacks callbacks;
    callbacks.onEvent = [&](const rcs_event& e) {
        failedStates += e.type == RCS_EVENT_STATE && e.state.value == RCS_STATE_FAILED;
    };
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47823, key), callbacks);
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }, 8s)); // UDP gives up after 5 s unanswered.
    CHECK(failedStates == 1 && view.failure().result == RCS_ERROR_NETWORK && !view.failure().message.empty());
    CHECK(view.start() == RCS_OK);
    CHECK(view.start() == RCS_ERROR_INVALID_STATE);
    CHECK(view.stop() == RCS_OK);

    // A wrong key fails authentication for that peer only: the sharer drops
    // it, and peers that hang up or stay silent, and keeps listening. Over
    // TCP, with raw TCP peers (tests/udp.cpp covers the UDP side).
    uint8_t other[32] = {9};
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47824, key).setTransport(RCS_TRANSPORT_TCP));
    rcsdk::Session intruder(config(RCS_ROLE_VIEW, 47824, other).setTransport(RCS_TRANSPORT_TCP));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(intruder.start() == RCS_OK);
    CHECK(waitFor([&] { return intruder.state() == RCS_STATE_FAILED; }));
    CHECK(intruder.failure().result == RCS_ERROR_AUTHENTICATION);
    {
        std::atomic<bool> never{false};
        rcsdk::transport::TcpStream::connect("127.0.0.1", 47824, never, 1s); // Hangs up at once.
        const auto silent = rcsdk::transport::TcpStream::connect("127.0.0.1", 47824, never, 1s);
        // The real viewer queues behind the silent peer and still gets in.
        std::atomic<int> frames{0};
        rcsdk::Callbacks counting;
        counting.onFrame = [&](const rcs_frame&) {
            ++frames;
        };
        rcsdk::Session viewer(config(RCS_ROLE_VIEW, 47824, key).setTransport(RCS_TRANSPORT_TCP), counting);
        CHECK(viewer.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 5; }, 8s));
        // The port stays open while the viewer runs: the silent peer is
        // dropped when its 2 s are up, not when the viewer got in.
        CHECK(share.state() == RCS_STATE_RUNNING);
        CHECK(waitFor([&] { return share.stats<rcs_network_stats>().rejected_connections == 3; }, 4s));
    }
    CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; })); // The viewer left.

    // Stopping a sharer that is still listening is prompt.
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    const auto started = std::chrono::steady_clock::now();
    CHECK(share.stop() == RCS_OK && share.state() == RCS_STATE_STOPPED);
    CHECK(std::chrono::steady_clock::now() - started < 1s);

    // Invalid configuration is rejected when the session is created.
    auto bad = config(RCS_ROLE_SHARE, 47825, key);
    bad.shareVideo().width = 641;
    CHECK(throws<rcsdk::Error>([&] { rcsdk::Session invalid(bad); }));
    CHECK(throws<rcsdk::Error>([&] { rcsdk::Session invalid(config(RCS_ROLE_VIEW, 0, key)); }));
    CHECK(throws<rcsdk::Error>([&] { rcsdk::Session invalid(config(RCS_ROLE_VIEW, 47825, key).setCodecs({})); }));
    // Extensions of the other role are refused, with the reason in the error.
    try {
        rcsdk::Session invalid(config(RCS_ROLE_VIEW, 47825, key).setVideo(640, 360, 30, 2'000'000));
        CHECK(false);
    } catch (const rcsdk::Error& error) {
        CHECK(error.result() == RCS_ERROR_INVALID_ARGUMENT &&
              std::string(error.what()).find("RCS_EXT_SHARE_VIDEO") != std::string::npos);
    }
    rcs_control_config control = RCS_CONTROL_CONFIG_INIT;
    control.allowed = 1;
    CHECK(view.update(control) == RCS_ERROR_INVALID_ARGUMENT); // A sharer's extension.
}

// Start, stop and destroy from any callback are refused, whichever of the
// session's threads runs it (reader, sender, sound, data, encoder): a host
// that stops from a callback gets an error instead of a deadlock.
void callbackLifecycle(const uint8_t (&key)[32]) {
    rcsdk::Session* sharePointer = nullptr;
    rcsdk::Session* viewPointer = nullptr;
    std::atomic<int> refused{0}, accepted{0}, audioEvents{0}, frames{0}, data{0};
    const auto probe = [&](rcsdk::Session* session) {
        if (session->stop() == RCS_ERROR_CALLBACK_THREAD && session->start() == RCS_ERROR_CALLBACK_THREAD) {
            ++refused;
        } else {
            ++accepted;
        }
    };
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = [&](const rcs_event&) { probe(sharePointer); };
    EncodedCallback output([&](const rcs_encoded_packet&) { probe(sharePointer); });
    shareCallbacks.onData = [&](const rcs_data_message&) { probe(sharePointer); };
    viewCallbacks.onEvent = [&](const rcs_event& event) {
        audioEvents += event.type == RCS_EVENT_AUDIO;
        probe(viewPointer);
    };
    viewCallbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
        probe(viewPointer);
    };
    viewCallbacks.onAudio = [&](const rcs_audio_frame&) { probe(viewPointer); };
    viewCallbacks.onData = [&](const rcs_data_message&) {
        ++data;
        probe(viewPointer);
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47871, key).use(rcsdk::encodedOutput(output)), shareCallbacks);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47871, key), viewCallbacks);
    sharePointer = &share;
    viewPointer = &view;
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10 && audioEvents >= 1; }));
    // Sound off and on again: the viewer's sound thread reports it.
    auto audio = rcsdk::defaults<rcs_receive_audio_config>();
    audio.playback = 0;
    audio.enabled = 0;
    const int before = audioEvents;
    CHECK(view.update(audio) == RCS_OK);
    CHECK(waitFor([&] { return audioEvents > before; }));
    audio.enabled = 1;
    CHECK(view.update(audio) == RCS_OK);
    CHECK(share.sendData(1, "hello") == RCS_OK);
    CHECK(waitFor([&] { return data >= 1; }));
    CHECK(view.state() == RCS_STATE_RUNNING && share.state() == RCS_STATE_RUNNING);
    CHECK(accepted == 0 && refused > 20);
}

// A sharer that keeps listening is not ended by a viewer it cannot serve (no
// sound codec in common) or one its host refuses: it listens again, says why,
// and serves the next one. Over TCP, and over a host's stream channel, each
// viewer in turn on route 1 (closed by the host as each one goes).
void listenAfterRefusals(const uint8_t (&key)[32], rcs_transport transport) {
    const bool host = transport == RCS_TRANSPORT_HOST;
    rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_STREAM);
    rcsdk::test::FunctionTransport shareSend(channel.sender(0));
    rcsdk::test::FunctionTransport viewSend(channel.sender(1));
    const auto configured = [&](rcs_role role) {
        return host ? hostConfig(role, key, RCS_HOST_CHANNEL_STREAM, role == RCS_ROLE_SHARE ? shareSend : viewSend)
                    : config(role, 47872, key);
    };
    EventLog shareEvents;
    std::atomic<int> decided{0};
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    shareCallbacks.onViewerJoin = [&](const rcs_viewer_info&, rcs_viewer_decision& decision) {
        if (++decided == 1) {
            decision.admission = RCS_REFUSE;
            std::snprintf(decision.reason, sizeof(decision.reason), "%s", "Not now");
        }
    };
    rcsdk::Session share(configured(RCS_ROLE_SHARE).setKeepListening(true).setAudioCodecs({RCS_AUDIO_OPUS}),
                         shareCallbacks);
    const auto shareEnd = channel.attachment(0, share);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    const auto refused = [&](rcsdk::SessionConfig viewing, rcs_result result) {
        {
            rcsdk::Session view(std::move(viewing));
            const auto viewEnd = channel.attachment(1, view);
            CHECK(view.start() == RCS_OK);
            CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }));
            CHECK(view.failure().result == result);
        }
        channel.close();
    };
    refused(configured(RCS_ROLE_VIEW).setAudioCodecs({RCS_AUDIO_PCM}), RCS_ERROR_UNSUPPORTED);
    // Listening again, with the reason: the sharer's own finding, or the
    // viewer hanging up first after finding the same.
    CHECK(waitFor([&] {
        const auto states = shareEvents.of(RCS_EVENT_STATE);
        return !states.empty() && states.back().state.value == RCS_STATE_LISTENING &&
               (states.back().state.result == RCS_ERROR_UNSUPPORTED ||
                states.back().state.result == RCS_ERROR_DISCONNECTED);
    }));
    refused(configured(RCS_ROLE_VIEW), RCS_ERROR_REFUSED);
    CHECK(decided == 1 && share.state() == RCS_STATE_LISTENING && share.failure().result == RCS_OK);
    std::atomic<int> frames{0};
    rcsdk::Callbacks callbacks;
    callbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    rcsdk::Session view(configured(RCS_ROLE_VIEW), callbacks);
    const auto viewEnd = channel.attachment(1, view);
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 5; }));
    CHECK(decided == 2 && share.stats<rcs_network_stats>().connections == 3);
    CHECK(channel.unexpected == 0);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// Data belongs to one connection: what is queued when it ends is dropped, a
// host waiting for room is told, and nothing is taken until the next viewer.
void dataBetweenConnections(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(dataQueue(config(RCS_ROLE_SHARE, 47873, key).setKeepListening(true), 1u << 20, 4096),
                         shareCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    std::atomic<uint64_t> received{0};
    {
        rcsdk::Callbacks callbacks;
        callbacks.onData = [&](const rcs_data_message& message) { received += message.size; };
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47873, key), callbacks);
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_RUNNING; }));
        const std::vector<uint8_t> chunk(256 * 1024, 7);
        rcs_result result = RCS_OK;
        for (int i = 0; i < 64 && result == RCS_OK; ++i) {
            result = share.sendData(2, chunk.data(), uint32_t(chunk.size()));
        }
        CHECK(result == RCS_ERROR_QUEUE_FULL);
    }
    // The viewer left: what was queued is gone, and a host that was refused
    // has been told there is room (as the queue drained, or as it was dropped).
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(waitFor([&] { return !shareEvents.of(RCS_EVENT_DATA).empty(); }));
    CHECK(share.sendData(2, "between") == RCS_ERROR_INVALID_STATE);
    CHECK(share.stats<rcs_data_stats>().queued_bytes == 0);
    // The next viewer gets only what is sent to it.
    std::atomic<int> messages{0};
    rcsdk::Callbacks callbacks;
    callbacks.onData = [&](const rcs_data_message& message) {
        CHECK(message.size == 4 && std::memcmp(message.data, "next", 4) == 0);
        ++messages;
    };
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47873, key), callbacks);
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_RUNNING; }));
    CHECK(share.sendData(2, "next") == RCS_OK);
    CHECK(waitFor([&] { return messages == 1; }));
    std::this_thread::sleep_for(200ms);
    CHECK(messages == 1);
}

// P2P, listening: a repeat of a viewer's signal (an at-least-once signalling
// channel) starts nothing; a viewer signal left over from a connection that
// gave up costs an attempt whose checks time out. While another viewer
// watches that is logged and nothing else; with none there, the sharer says
// why it listens again, as it did for its one viewer.
void p2pStaleSignal(const uint8_t (&key)[32]) {
    std::mutex signalMutex;
    std::map<uint64_t, rcsdk::Session*> routes; // The host's: who each route leads to.
    std::atomic<int> answers{0};
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    shareCallbacks.onSignal = [&](const std::string& text, uint64_t route) {
        ++answers;
        std::lock_guard lock(signalMutex);
        if (const auto found = routes.find(route); found != routes.end()) {
            CHECK(found->second->signal(text) == RCS_OK);
        }
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 0, key).setTransport(RCS_TRANSPORT_P2P).setKeepListening(true),
                         shareCallbacks);
    std::string lastViewerSignal;
    // A viewer on `route` that hands its signals over.
    const auto viewer = [&](uint64_t route, std::atomic<int>& frames) {
        rcsdk::Callbacks callbacks;
        callbacks.onSignal = [&, route](const std::string& text, uint64_t) {
            {
                std::lock_guard lock(signalMutex);
                lastViewerSignal = text;
            }
            CHECK(share.signal(text, route) == RCS_OK);
        };
        callbacks.onFrame = [&](const rcs_frame&) { ++frames; };
        auto view =
            std::make_unique<rcsdk::Session>(config(RCS_ROLE_VIEW, 0, key).setTransport(RCS_TRANSPORT_P2P), callbacks);
        std::lock_guard lock(signalMutex);
        routes[route] = view.get();
        return view;
    };
    const auto leave = [&](std::unique_ptr<rcsdk::Session>& view, uint64_t route) {
        CHECK(view->stop() == RCS_OK);
        std::lock_guard lock(signalMutex);
        routes.erase(route);
        view.reset();
    };
    // A viewer's signal whose candidate nobody answers: what a viewer whose
    // connection gave up leaves behind.
    const auto leftover = [](const char* ufrag) {
        rcsdk::transport::ice::Description d{RCS_ROLE_VIEW, ufrag, "00112233445566778899aabbccddeeff", {}, {}};
        d.candidates.push_back(
            {rcsdk::transport::ice::Type::host, rcsdk::transport::Endpoint::parse("127.0.0.1", 9), 2130706431});
        return rcsdk::transport::ice::encode(d);
    };
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    std::atomic<int> frames{0};
    auto first = viewer(1, frames);
    CHECK(first->start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 5; }, 10s));
    std::string firstSignal;
    {
        std::lock_guard lock(signalMutex);
        firstSignal = lastViewerSignal;
    }
    // A repeat: answered once already.
    CHECK(share.signal(firstSignal, 1) == RCS_OK);
    // A leftover while the viewer watches: its attempt ends, logged, and the
    // viewer is not disturbed.
    CHECK(share.signal(leftover("0badc0de"), 2) == RCS_OK);
    CHECK(waitFor([&] { return answers == 2; }));
    const size_t states = shareEvents.of(RCS_EVENT_STATE).size();
    CHECK(waitFor(
        [&] { return sessionLog().contains("A viewer's connection attempt failed (Network connection failed)"); },
        20s));
    CHECK(share.state() == RCS_STATE_RUNNING && shareEvents.of(RCS_EVENT_STATE).size() == states);
    const int watched = frames;
    CHECK(waitFor([&] { return frames >= watched + 5; }));
    CHECK(answers == 2);
    leave(first, 1);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 3s));
    // A leftover with no viewer there: the sharer listens again and says why.
    CHECK(share.signal(leftover("1badc0de"), 3) == RCS_OK);
    CHECK(waitFor(
        [&] {
            const auto list = shareEvents.of(RCS_EVENT_STATE);
            return list.back().state.value == RCS_STATE_LISTENING && list.back().state.result == RCS_ERROR_NETWORK;
        },
        20s));
    CHECK(share.failure().result == RCS_OK);
    // The next viewer gets in.
    frames = 0;
    auto next = viewer(4, frames);
    CHECK(next->start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 5; }, 10s));
    leave(next, 4);
}

// What sessionLogs() saw.
struct LogRecords {
    struct Record {
        rcs_log_level level;
        rcs_log_source source;
        rcs_session session;
        void* userData;
        std::string message;
        std::thread::id thread;
    };
    std::mutex mutex;
    std::vector<Record> records;
    std::atomic<rcs_session> stopFrom{nullptr}; // Stopped from inside the callback: refused.
    std::atomic<rcs_result> stopResult{RCS_OK};

    template <class Match>
    std::vector<Record> where(Match&& match, size_t from = 0) {
        std::lock_guard lock(mutex);
        std::vector<Record> found;
        std::copy_if(records.begin() + ptrdiff_t(from), records.end(), std::back_inserter(found), match);
        return found;
    }
};

LogRecords& logRecords() {
    static LogRecords log;
    return log;
}

void collectRecords(const rcs_log_record* r, void*) {
    auto& log = logRecords();
    CHECK(r->struct_size == sizeof(*r) && r->module && r->message && r->timestamp_ns);
    if (const rcs_session target = log.stopFrom.exchange(nullptr)) {
        log.stopResult = rcs_session_stop(target); // It would hold up every thread that logs.
    }
    std::lock_guard lock(log.mutex);
    log.records.push_back({r->level, r->source, r->session, r->user_data, r->message, std::this_thread::get_id()});
}

// Two sessions in one process: each record names the session whose work wrote
// it, from whichever of its threads, with that session's user_data; each logs
// up to its own level, which changes while it runs.
void sessionLogs(const uint8_t (&key)[32]) {
    auto& log = logRecords();
    CHECK(rcs_set_log_callback(RCS_LOG_INFO, collectRecords, nullptr) == RCS_OK);
    static std::atomic<int> frames{0};
    int shareTag = 1, viewTag = 2;
    rcs_session_callbacks shareTable = RCS_SESSION_CALLBACKS_INIT, viewTable = RCS_SESSION_CALLBACKS_INIT;
    shareTable.user_data = &shareTag;
    viewTable.user_data = &viewTag;
    viewTable.on_frame = [](const rcs_frame*, void*) { ++frames; };
    // The sharer logs everything; the viewer as much as the process.
    auto shareConfig = config(RCS_ROLE_SHARE, 47891, key).setLogLevel(RCS_LOG_DEBUG);
    auto viewConfig = config(RCS_ROLE_VIEW, 47891, key);
    rcs_session share = nullptr, view = nullptr;
    CHECK(rcs_session_create(&shareConfig.get(), &shareTable, &share) == RCS_OK);
    CHECK(rcs_session_create(&viewConfig.get(), &viewTable, &view) == RCS_OK);
    const auto state = [](rcs_session session) {
        rcs_state value = RCS_STATE_IDLE;
        rcs_session_get_state(session, &value);
        return value;
    };
    CHECK(rcs_session_start(share) == RCS_OK);
    CHECK(waitFor([&] { return state(share) == RCS_STATE_LISTENING; }));
    CHECK(rcs_session_start(view) == RCS_OK);
    CHECK(waitFor([&] { return frames >= 30; }));
    const auto from = [](rcs_session session) {
        return [session](const LogRecords::Record& r) { return r.session == session; };
    };
    CHECK(waitFor([&] { // The viewer's sound thread.
        return !log.where([&](const LogRecords::Record& r) {
                       return r.session == view && r.message.find("decoder:") != std::string::npos;
                   }).empty();
    }));
    log.stopFrom = share;
    CHECK(rcs_session_request_keyframe(share) == RCS_OK); // A DEBUG record of the sharer.
    // The callback takes the session before it stores the result: wait for the result.
    CHECK(waitFor([&] { return log.stopResult.load() != RCS_OK; }));
    CHECK(log.stopResult == RCS_ERROR_CALLBACK_THREAD);
    CHECK(waitFor([&] {
        return !log.where([&](const LogRecords::Record& r) {
                       return r.session == share && r.message == "A keyframe was asked for";
                   }).empty();
    }));

    // Everything so far was written for one of the two, with its user_data.
    for (const auto& r : log.where([](const LogRecords::Record&) { return true; })) {
        if (r.source != RCS_LOG_SOURCE_SESSION) {
            std::printf("a record of no session: %s\n", r.message.c_str());
        }
        CHECK(r.source == RCS_LOG_SOURCE_SESSION);
        CHECK((r.session == share && r.userData == &shareTag) || (r.session == view && r.userData == &viewTag));
    }
    const auto threads = [](const std::vector<LogRecords::Record>& records) {
        std::set<std::thread::id> ids;
        for (const auto& r : records) {
            ids.insert(r.thread);
        }
        return ids.size();
    };
    const auto shared = log.where(from(share)), viewed = log.where(from(view));
    CHECK(threads(shared) >= 2 && threads(viewed) >= 2);
    CHECK(std::any_of(shared.begin(), shared.end(), [](const auto& r) { return r.level == RCS_LOG_DEBUG; }));
    CHECK(std::none_of(viewed.begin(), viewed.end(), [](const auto& r) { return r.level == RCS_LOG_DEBUG; }));

    // While running: the sharer quiet, the viewer in detail.
    rcs_log_config quiet = RCS_LOG_CONFIG_INIT, detailed = RCS_LOG_CONFIG_INIT, wrong = RCS_LOG_CONFIG_INIT;
    quiet.max_level = RCS_LOG_ERROR;
    detailed.max_level = RCS_LOG_DEBUG;
    wrong.max_level = RCS_LOG_DEBUG + 1;
    CHECK(rcs_session_update(share, &quiet.header) == RCS_OK);
    CHECK(rcs_session_update(view, &detailed.header) == RCS_OK);
    CHECK(rcs_session_update(view, &wrong.header) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_set_log_callback(RCS_LOG_INFO, collectRecords, nullptr) == RCS_OK); // No record on its way now.
    size_t mark;
    {
        std::lock_guard lock(log.mutex);
        mark = log.records.size();
    }
    CHECK(rcs_session_stop(view) == RCS_OK);
    CHECK(waitFor([&] { return state(share) == RCS_STATE_FAILED; }));
    CHECK(rcs_session_stop(share) == RCS_OK);
    const auto sharedAfter = log.where(from(share), mark), viewedAfter = log.where(from(view), mark);
    CHECK(std::all_of(sharedAfter.begin(), sharedAfter.end(), [](const auto& r) { return r.level == RCS_LOG_ERROR; }));
    CHECK(std::any_of(viewedAfter.begin(), viewedAfter.end(),
                      [](const auto& r) { return r.level == RCS_LOG_DEBUG && r.message == "View session stopped"; }));
    CHECK(rcs_session_destroy(view) == RCS_OK);
    CHECK(rcs_session_destroy(share) == RCS_OK);

    // A level no version defines.
    auto refused = config(RCS_ROLE_VIEW, 47891, key);
    refused.log().max_level = 9;
    rcs_session never = nullptr;
    CHECK(rcs_session_create(&refused.get(), nullptr, &never) == RCS_ERROR_INVALID_ARGUMENT && !never);
    collectLog();
}

// ---- The host's own codecs ---------------------------------------------------------------------

// A packet the public interface handed over, copied (rcsdk::publicPacket the other way).
rcsdk::EncodedPacket packetOf(const rcs_encoded_packet& view) {
    rcsdk::EncodedPacket packet;
    packet.codec = view.codec;
    packet.picture = {view.format, view.color.matrix, view.color.range};
    packet.width = view.width;
    packet.height = view.height;
    packet.keyframe = view.keyframe != 0;
    packet.sequence = view.sequence;
    packet.timestampNs = view.timestamp_ns;
    packet.data.assign(view.data, view.data + view.data_size);
    if (view.config) {
        packet.config.assign(view.config, view.config + view.config_size);
    }
    return packet;
}

// A CPU picture (BGRA8) as a frame of the SDK's.
rcsdk::VideoFrame frameOf(const rcs_frame& picture) {
    rcsdk::VideoFrame frame;
    frame.width = picture.width;
    frame.height = picture.height;
    frame.stride = picture.stride_bytes;
    frame.pixels = std::make_shared<const std::vector<uint8_t>>(picture.data, picture.data + picture.data_size);
    return frame;
}

// An encoder of the SDK's (the system's, or a library's) behind the host's
// interface, as an NVENC or x264 of the host's would be: real H.264 out, its
// packets pushed from an output thread of its own (an asynchronous encoder).
class WrappedEncoder final : public rcsdk::HostEncoder {
public:
    std::atomic<int> opens{0}, closes{0}, keyframes{0}, gpuFrames{0}, cpuFrames{0};
    std::mutex mutex;
    std::vector<uint32_t> bitrates;
    std::vector<rcs_encoder_speed> speeds; // Of each open.
    std::atomic<uint32_t> refusedAbove{0}; // Opens at more frames a second than this fail (0: none do)...
    std::atomic<int> refusals{0};          // ...counted.

    rcs_result open(const rcs_host_encoder_settings& s) override {
        if (refusedAbove && s.frames_per_second > refusedAbove) {
            ++refusals;
            return RCS_ERROR_UNSUPPORTED;
        }
        settings_ = s;
        {
            std::lock_guard lock(mutex);
            speeds.push_back(s.speed);
        }
        rcsdk::Picture picture;
        CHECK(rcsdk::pictureOf(s.format, s.color, picture));
        const rcsdk::platform::EncoderConfig config{
            s.codec,          s.width, s.height, s.frames_per_second, s.bitrate_bps, testAcceleration(),
            RCS_BACKEND_AUTO, {},      picture,  s.speed};
        inner_ = rcsdk::codecs::createEncoder(config, rcsdk::codecs::implementations());
        inner_->open(config);
        stop_ = false;
        output_ = std::thread([this] {
            for (rcsdk::EncodedPacket packet; !stop_;) {
                if (inner_->receive(packet, 20ms)) {
                    CHECK(rcsdk::pushEncoded(settings_.output, rcsdk::publicPacket(packet)) == RCS_OK);
                }
            }
        });
        ++opens;
        return RCS_OK;
    }
    rcs_result encode(const rcs_host_encoder_frame& picture) override {
        CHECK(picture.struct_size == sizeof(picture) && bool(picture.gpu_frame) != bool(picture.cpu_frame));
        rcsdk::VideoFrame frame = picture.gpu_frame ? picture.gpu_frame->frame : frameOf(*picture.cpu_frame);
        ++(picture.gpu_frame ? gpuFrames : cpuFrames);
        frame.sequence = picture.sequence;
        frame.timestampNs = picture.timestamp_ns;
        if (picture.keyframe) {
            ++keyframes;
            inner_->requestKeyframe();
        }
        inner_->submit(frame);
        return RCS_OK;
    }
    rcs_result setBitrate(uint32_t, uint32_t, uint32_t bitrate) override {
        inner_->setBitrate(bitrate);
        std::lock_guard lock(mutex);
        bitrates.push_back(bitrate);
        return RCS_OK;
    }
    void close(uint32_t, uint32_t) override {
        stop_ = true;
        output_.join();
        inner_.reset();
        ++closes;
    }

private:
    rcs_host_encoder_settings settings_{};
    std::unique_ptr<rcsdk::platform::VideoEncoder> inner_;
    std::atomic<bool> stop_{false};
    std::thread output_;
};

// A decoder of the SDK's behind the host's interface: it decodes and keeps the
// pictures to itself, as a host showing them in its own renderer would.
class WrappedDecoder final : public rcsdk::HostDecoder {
public:
    std::atomic<int> opens{0}, closes{0}, resets{0}, packets{0}, pictures{0}, failNext{0}, slowMs{0};
    std::atomic<uint32_t> width{0}, height{0};

    rcs_result open(const rcs_host_decoder_settings& s) override {
        CHECK(s.struct_size == sizeof(s) && s.width && s.height);
        rcsdk::platform::DecoderConfig config;
        config.codec = s.codec;
        config.acceleration = testAcceleration();
        config.cpuImages = true;
        inner_ = rcsdk::codecs::createDecoder(config, rcsdk::codecs::implementations());
        inner_->open(config);
        width = s.width;
        height = s.height;
        ++opens;
        return RCS_OK;
    }
    rcs_result decode(const rcs_encoded_packet& packet) override {
        if (failNext > 0) {
            --failNext;
            return RCS_ERROR_DEVICE; // A GPU reset.
        }
        ++packets;
        std::this_thread::sleep_for(std::chrono::milliseconds(slowMs.load()));
        rcsdk::VideoFrame frame;
        static const std::atomic<bool> never{false};
        pictures += inner_->decode(packetOf(packet), frame, never) ? 1 : 0;
        return RCS_OK;
    }
    void reset(uint32_t) override {
        inner_->reset();
        ++resets;
    }
    void close(uint32_t) override {
        inner_.reset();
        ++closes;
    }

private:
    std::unique_ptr<rcsdk::platform::VideoDecoder> inner_;
};

// A codec of the hosts' own (host codec 16): each packet holds the picture's
// colour at its centre, four bytes, and is a keyframe. What the codec means is
// the hosts' agreement; the SDK only carries it.
class ColourEncoder final : public rcsdk::HostEncoder {
public:
    std::atomic<int> gpuFrames{0}, cpuFrames{0}, retainedOk{0};
    std::atomic<bool> deviceMatches{true};

    rcs_result open(const rcs_host_encoder_settings& s) override {
        settings_ = s;
        return RCS_OK;
    }
    rcs_result encode(const rcs_host_encoder_frame& picture) override {
        uint32_t colour = 0;
        if (picture.gpu_frame) {
            ++gpuFrames;
            rcs_gpu_frame_info info = RCS_GPU_FRAME_INFO_INIT;
            CHECK(rcs_gpu_frame_get_info(picture.gpu_frame, &info) == RCS_OK);
            CHECK(info.format == RCS_PIXEL_BGRA8 && info.width == settings_.width && info.resource);
            deviceMatches = deviceMatches && info.device == settings_.d3d11_device;
            // An asynchronous encoder keeps the picture beyond the call.
            CHECK(rcs_gpu_frame_retain(picture.gpu_frame) == RCS_OK);
            if (held_) {
                rcs_gpu_frame_info earlier = RCS_GPU_FRAME_INFO_INIT;
                retainedOk += rcs_gpu_frame_get_info(held_, &earlier) == RCS_OK && earlier.resource;
                rcs_gpu_frame_release(held_);
            }
            held_ = picture.gpu_frame;
            rcsdk::platform::withCpuImage(picture.gpu_frame->frame, [&](const rcs_frame& view) {
                std::memcpy(&colour, view.data + size_t(view.height / 2) * view.stride_bytes + view.width / 2 * 4, 4);
            });
        } else {
            ++cpuFrames;
            const auto& view = *picture.cpu_frame;
            std::memcpy(&colour, view.data + size_t(view.height / 2) * view.stride_bytes + view.width / 2 * 4, 4);
        }
        rcs_encoded_packet packet = RCS_ENCODED_PACKET_INIT;
        packet.codec = settings_.codec;
        packet.format = settings_.format;
        packet.color = settings_.color;
        packet.width = settings_.width;
        packet.height = settings_.height;
        packet.keyframe = 1;
        packet.sequence = picture.sequence;
        packet.timestamp_ns = picture.timestamp_ns;
        packet.data = reinterpret_cast<const uint8_t*>(&colour);
        packet.data_size = 4;
        return rcsdk::pushEncoded(settings_.output, packet); // During encode: a synchronous encoder.
    }
    void close(uint32_t, uint32_t) override {
        if (held_) {
            rcs_gpu_frame_release(held_);
            held_ = nullptr;
        }
    }

private:
    rcs_host_encoder_settings settings_{};
    rcs_gpu_frame held_ = nullptr;
};

class ColourDecoder final : public rcsdk::HostDecoder {
public:
    std::atomic<uint32_t> colour{0};
    std::atomic<int> packets{0};
    rcs_result open(const rcs_host_decoder_settings& s) override {
        CHECK(s.codec == RCS_CODEC_HOST_FIRST);
        return RCS_OK;
    }
    rcs_result decode(const rcs_encoded_packet& packet) override {
        CHECK(packet.codec == RCS_CODEC_HOST_FIRST && packet.data_size == 4 && !packet.config_size);
        uint32_t value;
        std::memcpy(&value, packet.data, 4);
        colour = value;
        ++packets;
        return RCS_OK;
    }
    void reset(uint32_t) override {}
    void close(uint32_t) override {}
};

// The share's codec events with their backend, as the host sees them.
std::vector<rcs_codec_event> codecEvents(EventLog& log) {
    std::vector<rcs_codec_event> codecs;
    for (const auto& event : log.of(RCS_EVENT_CODEC)) {
        codecs.push_back(event.codec);
    }
    return codecs;
}

// A change of the stream its encoder does not take (here the host's encoder
// refuses more than 40 frames a second; NVENC's H.264 4:4:4 at 4K and 129)
// leaves the stream as it was: the host hears what was asked and why
// (RCS_EVENT_ENCODING of encoding 0, running), the viewer keeps its pictures
// and the run goes on; the same change is not tried again, another one is.
void refusedChange(const uint8_t (&key)[32]) {
    WrappedEncoder encoder;
    encoder.refusedAbove = 40;
    EventLog shareEvents;
    std::atomic<int> frames{0};
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    auto sharing = config(RCS_ROLE_SHARE, 47998, key).setTransport(RCS_TRANSPORT_UDP).setVideo(640, 360, 30, 2'000'000);
    sharing.use(rcsdk::hostEncoder(encoder, {RCS_CODEC_H264}, "picky H.264", false, RCS_ACCELERATION_SOFTWARE));
    rcsdk::Session share(sharing, shareCallbacks);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47998, key).setTransport(RCS_TRANSPORT_UDP), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 20 && view.streamInfo().frames_per_second == 30; }, 10s));
    auto video = sharing.extension<rcs_share_video_config>();
    video.frames_per_second = 60;
    CHECK(share.update(video) == RCS_OK);
    const auto refused = [&] {
        for (const auto& e : shareEvents.of(RCS_EVENT_ENCODING)) {
            if (e.encoding.encoding == 0 && e.encoding.result != RCS_OK) {
                return std::optional(e.encoding);
            }
        }
        return std::optional<rcs_encoding_event>();
    };
    CHECK(waitFor([&] { return refused().has_value(); }, 10s));
    const auto event = *refused();
    CHECK(event.running == 1 && event.frames_per_second == 60 && event.width == 640 &&
          event.result == RCS_ERROR_UNSUPPORTED && event.message[0]);
    // The stream as it was: pictures come, at 30 FPS, from an encoder open again at 30.
    const int before = frames;
    std::this_thread::sleep_for(3s);
    std::printf("refused change: %d frames in 3 s after it, sharer state %d, viewer state %d\n", frames - before,
                share.state(), view.state());
    CHECK(frames - before >= 30 && share.state() == RCS_STATE_RUNNING && view.state() == RCS_STATE_RUNNING);
    CHECK(view.streamInfo().frames_per_second == 30 && encoder.refusals == 1);
    {
        std::lock_guard lock(encoder.mutex);
        CHECK(encoder.speeds.size() == 2); // The first open, and the one again after the refusal.
    }
    // Another change is tried: 25 FPS opens.
    video.frames_per_second = 25;
    CHECK(share.update(video) == RCS_OK);
    CHECK(waitFor([&] { return view.streamInfo().frames_per_second == 25; }, 10s));
    CHECK(encoder.refusals == 1 && view.state() == RCS_STATE_RUNNING);
    std::printf("refused change: 60 FPS refused (%s), the stream stayed at 30, then went to 25\n", event.message);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// The host's encoder encodes the SDK's pictures into standard H.264 the
// viewer's system decoder plays: keyframe requests and adaptive bitrate reach
// it, events and statistics name it; an agent of another process takes one
// too, where the encoding happens.
void hostEncoder(const uint8_t (&key)[32]) {
    WrappedEncoder encoder;
    EventLog shareEvents;
    std::atomic<int> frames{0};
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    auto sharing = config(RCS_ROLE_SHARE, 47900, key).setTransport(RCS_TRANSPORT_UDP);
    startBitrate(sharing.setRateMode(RCS_RATE_ADAPTIVE, 300'000), 1'000'000); // From 1 Mbit/s up: the bitrate changes.
    sharing.use(rcsdk::hostEncoder(encoder, {RCS_CODEC_H264}, "wrapped H.264", false, RCS_ACCELERATION_SOFTWARE));
    {
        rcsdk::Session share(sharing, shareCallbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47900, key).setTransport(RCS_TRANSPORT_UDP), viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 30; }));
        CHECK(encoder.opens == 1 && encoder.cpuFrames > 0 && encoder.keyframes >= 1);
        CHECK(std::string(share.stats<rcs_video_send_stats>().encoder) == "wrapped H.264");
        const auto opened = codecEvents(shareEvents);
        CHECK(!opened.empty() && opened.back().backend == RCS_BACKEND_HOST &&
              opened.back().acceleration == RCS_ACCELERATION_SOFTWARE &&
              std::string(opened.back().name) == "wrapped H.264");
        // A keyframe the viewer asks for reaches encode; adaptive bitrate reaches set_bitrate.
        const int asked = encoder.keyframes;
        CHECK(view.requestKeyframe() == RCS_OK);
        CHECK(waitFor([&] { return encoder.keyframes > asked; }));
        CHECK(waitFor([&] {
            std::lock_guard lock(encoder.mutex);
            return !encoder.bitrates.empty();
        }));
        // The encoder's speed reaches the host's encoder, which opens again for another.
        {
            std::lock_guard lock(encoder.mutex);
            CHECK(encoder.speeds == std::vector<rcs_encoder_speed>({RCS_ENCODER_FASTEST}));
        }
        auto slower = sharing.extension<rcs_video_tuning_config>();
        slower.speed = RCS_ENCODER_BEST_QUALITY;
        CHECK(share.update(slower) == RCS_OK);
        CHECK(waitFor([&] {
            std::lock_guard lock(encoder.mutex);
            return encoder.speeds.size() == 2 && encoder.speeds.back() == RCS_ENCODER_BEST_QUALITY;
        }));
        slower.speed = 4;
        CHECK(share.update(slower) == RCS_ERROR_INVALID_ARGUMENT);
        // The host's codecs are fixed for a run.
        CHECK(share.update(sharing.extension<rcs_host_encoder_config>()) == RCS_ERROR_INVALID_STATE);
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    CHECK(encoder.closes == encoder.opens);

    // An agent of another process encodes with the host's encoder it was given;
    // the session learns of it when the agent attaches. (Not on Android: a
    // program run from adb can make no agent link there.)
    WrappedEncoder agentEncoder;
    const std::string endpoint = agentEndpoint("host-encoder");
    const auto linkKey = rcsdk::generateConnectionKey();
    frames = 0;
#ifndef __ANDROID__
    {
        EventLog events;
        rcsdk::Callbacks callbacks;
        callbacks.onEvent = events.callback();
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47906, key).use(rcsdk::agentLink(endpoint, linkKey)), callbacks);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47906, key), viewCallbacks);
        rcsdk::Agent agent(rcsdk::AgentConfig(endpoint, linkKey)
                               .setReconnect(true, 100)
                               .setHostEncoder(rcsdk::hostEncoder(agentEncoder, {RCS_CODEC_H264}, "agent's H.264")));
        CHECK(share.start() == RCS_OK && agent.start() == RCS_OK);
        if (!waitFor([&] { return share.state() == RCS_STATE_LISTENING; })) {
            std::printf("share %d (%s), agent %d (%s), attached %u\n", share.state(), share.failure().message.c_str(),
                        agent.state(), agent.failure().message.c_str(), rcsdk::agentInfo(share).attached);
            CHECK(false);
        }
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return frames >= 30; }));
        CHECK(agentEncoder.opens >= 1 && std::string(share.stats<rcs_video_send_stats>().encoder) == "agent's H.264");
        const auto opened = codecEvents(events);
        CHECK(!opened.empty() && opened.back().backend == RCS_BACKEND_HOST);
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
#endif
    // ...and not a session of this process with an agent of another.
    rcs_session never = nullptr;
    auto wrong = config(RCS_ROLE_SHARE, 47906, key).use(rcsdk::agentLink(endpoint, linkKey));
    wrong.use(rcsdk::hostEncoder(encoder, {RCS_CODEC_H264}, "here"));
    CHECK(rcs_session_create(&wrong.get(), nullptr, &never) == RCS_ERROR_INVALID_ARGUMENT && !never);
    CHECK(rcsdk::lastCallError().find("rcs_agent_config") != std::string::npos);
}

// GPU pictures reach the host's encoder as the capture made them (handles of
// on_gpu_frame's kind, retainable, on the capture's device), or read back as
// BGRA8 when it asks; a codec of the hosts' own carries them end to end.
void hostCodec(const uint8_t (&key)[32]) {
    if (!rcsdk::test::SurfacePatternCapture::available()) {
        std::printf("No GPU surfaces on this platform\n");
        return;
    }
    rcsdk::test::UseSurfacePattern surfaces;
    for (const bool cpu : {false, true}) {
        ColourEncoder encoder;
        ColourDecoder decoder;
        std::atomic<int> frames{0};
        rcsdk::Callbacks viewCallbacks;
        viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; }; // The host's decoder shows them instead.
        auto sharing = config(RCS_ROLE_SHARE, 47902, key);
        sharing.setCodecs({RCS_CODEC_HOST_FIRST, RCS_CODEC_H264})
            .use(rcsdk::hostEncoder(encoder, {RCS_CODEC_HOST_FIRST}, "colour", cpu));
        auto viewing = config(RCS_ROLE_VIEW, 47902, key);
        viewing.setCodecs({RCS_CODEC_HOST_FIRST, RCS_CODEC_H264})
            .use(rcsdk::hostDecoder(decoder, {RCS_CODEC_HOST_FIRST}, "colour"));
        rcsdk::Session share(sharing);
        rcsdk::Session view(viewing, viewCallbacks);
        rcsdk::test::SurfacePatternCapture::colour() = 0xFF2040C0u;
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return decoder.colour == 0xFF2040C0u; }));
        rcsdk::test::SurfacePatternCapture::colour() = 0xFF30A050u;
        CHECK(waitFor([&] { return decoder.colour == 0xFF30A050u; }));
        CHECK(view.streamInfo().codec == RCS_CODEC_HOST_FIRST && (view.peerInfo().codecs & (1u << 16)));
        CHECK(waitFor([&] { return view.stats<rcs_video_receive_stats>().decoded_frames > 10; }));
        CHECK(frames == 0);
        if (cpu) {
            CHECK(encoder.cpuFrames > 10 && encoder.gpuFrames == 0);
        } else {
            CHECK(encoder.gpuFrames > 10 && encoder.cpuFrames == 0 && encoder.retainedOk > 5 && encoder.deviceMatches);
        }
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    // A host codec needs the host's own codec at this end.
    rcs_session never = nullptr;
    auto alone = config(RCS_ROLE_VIEW, 47902, key);
    alone.setCodecs({RCS_CODEC_HOST_FIRST});
    CHECK(rcs_session_create(&alone.get(), nullptr, &never) == RCS_ERROR_INVALID_ARGUMENT && !never);
    CHECK(rcsdk::lastCallError().find("RCS_EXT_HOST_DECODER") != std::string::npos);
}

// The colour codec of the hosts' own, saying which picture formats it takes,
// and what each open of it was for.
class FormatEncoder final : public rcsdk::HostEncoder {
public:
    std::mutex mutex;
    std::vector<rcs_host_encoder_settings> opened;
    rcs_result open(const rcs_host_encoder_settings& s) override {
        std::lock_guard lock(mutex);
        opened.push_back(s);
        settings_ = s;
        return RCS_OK;
    }
    rcs_result encode(const rcs_host_encoder_frame& picture) override {
        const uint32_t colour = 0x01020304;
        rcs_encoded_packet packet = RCS_ENCODED_PACKET_INIT;
        packet.codec = settings_.codec;
        packet.format = settings_.format;
        packet.color = settings_.color;
        packet.width = settings_.width;
        packet.height = settings_.height;
        packet.keyframe = 1;
        packet.sequence = picture.sequence;
        packet.data = reinterpret_cast<const uint8_t*>(&colour);
        packet.data_size = 4;
        if (wrongFormat) { // A packet of another format than the one opened is refused.
            packet.format = settings_.format == RCS_PICTURE_444_8 ? RCS_PICTURE_420_8 : RCS_PICTURE_444_8;
            refusedWrong = rcsdk::pushEncoded(settings_.output, packet) == RCS_ERROR_INVALID_ARGUMENT;
            wrongFormat = false;
            packet.format = settings_.format;
        }
        return rcsdk::pushEncoded(settings_.output, packet);
    }
    void close(uint32_t, uint32_t) override {}
    std::atomic<bool> wrongFormat{false}, refusedWrong{false};

private:
    rcs_host_encoder_settings settings_{};
};

class FormatDecoder final : public rcsdk::HostDecoder {
public:
    std::mutex mutex;
    std::vector<rcs_host_decoder_settings> opened;
    std::atomic<int> packets{0};
    std::atomic<rcs_picture_format> lastFormat{0};
    rcs_result open(const rcs_host_decoder_settings& s) override {
        std::lock_guard lock(mutex);
        opened.push_back(s);
        return RCS_OK;
    }
    rcs_result decode(const rcs_encoded_packet& packet) override {
        lastFormat = packet.format;
        ++packets;
        return RCS_OK;
    }
    void reset(uint32_t) override {}
    void close(uint32_t) override {}
};

// Picture formats negotiate as codecs do: each end lists what it takes, the
// sharer's order decides, the viewer may ask for one both take; the stream,
// the packets and both ends' codecs carry the format, the matrix and the
// range; without a common format both ends fail and say why. Host codecs,
// which declare any format, stand in for platform codecs here, fed the host's
// own BGRA8 pictures (which any SDR format can be made of).
// RCS_ACCELERATION_HARDWARE_FIRST with the system's codecs: a 4:4:4 HEVC
// stream is decoded in hardware where a hardware decoder takes 4:4:4
// (macOS), else in software (Windows), and reaches on_gpu_frame either way
// (Windows' software decoder uploads BGRA8 textures); asked for 4:2:0, the
// viewer decodes in hardware again. RCS_EVENT_CODEC says which each time.
void hardwareFirst(const uint8_t (&key)[32]) {
    constexpr uint32_t full444 = 1u << RCS_PICTURE_444_8, sdr8 = 1u << RCS_PICTURE_420_8;
    uint32_t encodes = 0, hardware = 0, software = 0;
    for (const auto& i : rcsdk::codecImplementations()) {
        if (i.codec != RCS_CODEC_HEVC) {
            continue;
        }
        if (i.direction == RCS_CODEC_ENCODER) {
            encodes |= i.acceleration == RCS_ACCELERATION_HARDWARE ? i.picture_formats : 0;
        } else {
            (i.acceleration == RCS_ACCELERATION_HARDWARE ? hardware : software) |= i.picture_formats;
        }
    }
    if (!(encodes & rcsdk::capabilities().picture_formats & full444) || !((hardware | software) & full444) ||
        !(hardware & sdr8)) {
        std::printf("No system HEVC codecs here encode and decode 4:4:4 and decode 4:2:0 in hardware; not tested\n");
        return;
    }
    const rcs_acceleration for444 = (hardware & full444) ? RCS_ACCELERATION_HARDWARE : RCS_ACCELERATION_SOFTWARE;
    const bool windows = rcsdk::capabilities().platform == RCS_PLATFORM_WINDOWS;
    std::atomic<int> frames{0};
    std::atomic<rcs_pixel_format> layout{0};
    EventLog viewEvents;
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onEvent = viewEvents.callback();
    viewCallbacks.onGpuFrame = [&](const rcsdk::GpuFrame& frame) {
        layout = frame.info().format;
        ++frames;
    };
    auto sharing = config(RCS_ROLE_SHARE, 47908, key);
    sharing.setCodecs({RCS_CODEC_HEVC})
        .setAcceleration(RCS_ACCELERATION_HARDWARE)
        .setPictureFormats({RCS_PICTURE_444_8, RCS_PICTURE_420_8});
    auto viewing = config(RCS_ROLE_VIEW, 47908, key);
    viewing.setCodecs({RCS_CODEC_HEVC})
        .setAcceleration(RCS_ACCELERATION_HARDWARE_FIRST)
        .setPictureFormats({RCS_PICTURE_444_8, RCS_PICTURE_420_8});
    rcsdk::Session share(sharing);
    rcsdk::Session view(viewing, viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10; }));
    const auto decoder = [&] {
        const auto codecs = viewEvents.of(RCS_EVENT_CODEC);
        return codecs.empty() ? rcs_codec_event{} : codecs.back().codec;
    };
    CHECK(view.streamInfo().format == RCS_PICTURE_444_8 && decoder().acceleration == for444);
    CHECK(layout == (windows && for444 == RCS_ACCELERATION_SOFTWARE ? RCS_PIXEL_BGRA8 : RCS_PIXEL_NV24));
    std::printf("hardware first: 4:4:4 decoded in %s by %s\n",
                decoder().acceleration == RCS_ACCELERATION_HARDWARE ? "hardware" : "software", decoder().name);
    auto ask = viewing.viewRequest();
    ask.format = RCS_PICTURE_420_8;
    CHECK(view.update(ask) == RCS_OK);
    CHECK(waitFor([&] { return view.streamInfo().format == RCS_PICTURE_420_8; }));
    CHECK(waitFor([&] { return decoder().acceleration == RCS_ACCELERATION_HARDWARE && layout == RCS_PIXEL_NV12; }));
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

void pictureFormats(const uint8_t (&key)[32]) {
    constexpr uint32_t sdr8 = 1u << RCS_PICTURE_420_8, full444 = 1u << RCS_PICTURE_444_8;
    // Pushes 640x360 pictures while it lives.
    struct Pusher {
        rcsdk::Session& share;
        std::atomic<bool> pushing{true};
        std::thread thread{[this] {
            const std::vector<uint8_t> picture(640 * 360 * 4, 0x80);
            while (pushing) {
                share.pushVideoFrame(picture.data(), 640, 360, 640 * 4);
                std::this_thread::sleep_for(33ms);
            }
        }};
        ~Pusher() {
            pushing = false;
            thread.join();
        }
    };
    {
        FormatEncoder encoder;
        FormatDecoder decoder;
        EventLog shareEvents, viewEvents;
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        viewCallbacks.onEvent = viewEvents.callback();
        auto sharing = config(RCS_ROLE_SHARE, 47907, key);
        sharing.setCaptureSource(RCS_CAPTURE_HOST)
            .setCodecs({RCS_CODEC_HOST_FIRST})
            .setPictureFormats({RCS_PICTURE_444_8, RCS_PICTURE_420_8})
            .setColor(RCS_MATRIX_BT601, RCS_RANGE_FULL)
            .use(rcsdk::hostEncoder(encoder, {RCS_CODEC_HOST_FIRST}, "formats", false, RCS_ACCELERATION_HARDWARE, true,
                            sdr8 | full444));
        auto viewing = config(RCS_ROLE_VIEW, 47907, key);
        viewing.setCodecs({RCS_CODEC_HOST_FIRST})
            .setPictureFormats({RCS_PICTURE_420_8, RCS_PICTURE_444_8})
            .use(rcsdk::hostDecoder(decoder, {RCS_CODEC_HOST_FIRST}, "formats", RCS_ACCELERATION_HARDWARE, sdr8 | full444));
        rcsdk::Session share(sharing, shareCallbacks);
        rcsdk::Session view(viewing, viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        Pusher pusher{share};
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return decoder.packets >= 5; }));
        // The sharer's first that both take, its matrix and range.
        const auto stream = view.streamInfo();
        CHECK(stream.format == RCS_PICTURE_444_8 && stream.color.matrix == RCS_MATRIX_BT601 &&
              stream.color.range == RCS_RANGE_FULL && stream.color.primaries == RCS_PRIMARIES_BT709 &&
              stream.color.transfer == RCS_TRANSFER_BT709);
        CHECK(share.streamInfo().format == RCS_PICTURE_444_8);
        CHECK(view.peerInfo().codec_limits[0].picture_formats == (sdr8 | full444));
        {
            std::lock_guard lock(encoder.mutex);
            CHECK(encoder.opened.back().format == RCS_PICTURE_444_8 &&
                  encoder.opened.back().color.matrix == RCS_MATRIX_BT601 &&
                  encoder.opened.back().color.range == RCS_RANGE_FULL);
        }
        {
            std::lock_guard lock(decoder.mutex);
            CHECK(!decoder.opened.empty() && decoder.opened.back().format == RCS_PICTURE_444_8 &&
                  decoder.opened.back().color.range == RCS_RANGE_FULL);
        }
        CHECK(decoder.lastFormat == RCS_PICTURE_444_8);
        const auto first = viewEvents.of(RCS_EVENT_STREAM).front().stream;
        CHECK((first.changes & RCS_STREAM_FORMAT) && first.info.format == RCS_PICTURE_444_8);
        // A packet of another format than the encoder was opened for is refused.
        encoder.wrongFormat = true;
        CHECK(waitFor([&] { return encoder.refusedWrong.load(); }));

        // The viewer asks for 4:2:0: the stream restarts in it, both codecs open again.
        auto ask = viewing.viewRequest();
        ask.format = RCS_PICTURE_420_8;
        CHECK(view.update(ask) == RCS_OK);
        CHECK(waitFor([&] { return view.streamInfo().format == RCS_PICTURE_420_8; }));
        CHECK(waitFor([&] { return decoder.lastFormat == RCS_PICTURE_420_8; }));
        CHECK(waitFor([&] {
            const auto streams = viewEvents.of(RCS_EVENT_STREAM);
            return !streams.empty() && (streams.back().stream.changes & RCS_STREAM_FORMAT) &&
                   streams.back().stream.info.format == RCS_PICTURE_420_8;
        }));
        {
            std::lock_guard lock(decoder.mutex);
            CHECK(decoder.opened.back().format == RCS_PICTURE_420_8);
        }
        // A request it cannot follow (no common format) leaves the sharer's choice.
        ask.format = RCS_PICTURE_420_10;
        auto decoding = viewing.decoder();
        decoding.formats[2] = RCS_PICTURE_420_10;
        CHECK(view.update(decoding) == RCS_OK);
        CHECK(view.update(ask) == RCS_OK);
        CHECK(waitFor([&] { return view.streamInfo().format == RCS_PICTURE_444_8; }));
        // The sharer stops offering 4:4:4 while running: 4:2:0 it is.
        auto encoding = sharing.encoder();
        encoding.formats[0] = RCS_PICTURE_420_8;
        encoding.formats[1] = 0;
        CHECK(share.update(encoding) == RCS_OK);
        CHECK(waitFor([&] { return view.streamInfo().format == RCS_PICTURE_420_8; }));
        CHECK(waitFor([&] { return view.peerInfo().codec_limits[0].picture_formats == sdr8; }));
        // A list without anything the peer takes is refused, the stream stays.
        encoding.formats[0] = RCS_PICTURE_444_10;
        CHECK(share.update(encoding) == RCS_ERROR_UNSUPPORTED);
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    // The system's own codecs, where this machine encodes and decodes 4:4:4
    // (an M4 Pro: HEVC, and H.264 by its pinned encoder): the viewer's GPU
    // frames are NV24 of the stream's colour, whichever of the two it asks for.
    const auto both444 = [](rcs_codec codec) {
        uint32_t encodes = 0, decodes = 0;
        for (const auto& i : rcsdk::codecImplementations()) {
            if (i.codec == codec && i.acceleration == testAcceleration()) {
                (i.direction == RCS_CODEC_ENCODER ? encodes : decodes) |= i.picture_formats;
            }
        }
        return (encodes & decodes & rcsdk::capabilities().picture_formats & full444) != 0;
    };
    if (both444(RCS_CODEC_HEVC) && both444(RCS_CODEC_H264)) {
        std::atomic<int> nv24{0};
        std::atomic<bool> other{false};
        rcsdk::Callbacks viewCallbacks;
        viewCallbacks.onGpuFrame = [&](const rcsdk::GpuFrame& frame) {
            const auto info = frame.info();
            const bool expected = info.format == RCS_PIXEL_NV24 && info.color.matrix == RCS_MATRIX_BT709 &&
                                  info.color.range == RCS_RANGE_FULL && info.color.transfer == RCS_TRANSFER_BT709;
            (expected ? ++nv24 : (other = true, 0));
        };
        auto sharing = config(RCS_ROLE_SHARE, 47907, key);
        sharing.setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264})
            .setPictureFormats({RCS_PICTURE_444_8, RCS_PICTURE_420_8})
            .setColor(RCS_MATRIX_BT709, RCS_RANGE_FULL);
        auto viewing = config(RCS_ROLE_VIEW, 47907, key);
        viewing.setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264}).setPictureFormats({RCS_PICTURE_420_8, RCS_PICTURE_444_8});
        rcsdk::Session share(sharing);
        rcsdk::Session view(viewing, viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return nv24 >= 10; }));
        CHECK(view.streamInfo().codec == RCS_CODEC_HEVC && view.streamInfo().format == RCS_PICTURE_444_8);
        auto ask = viewing.viewRequest();
        ask.codec = RCS_CODEC_H264;
        CHECK(view.update(ask) == RCS_OK);
        CHECK(waitFor([&] { return view.streamInfo().codec == RCS_CODEC_H264; }));
        const int before = nv24;
        CHECK(waitFor([&] { return nv24 >= before + 10; }));
        CHECK(view.streamInfo().format == RCS_PICTURE_444_8 && !other);
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    } else {
        std::printf("No system codec encodes and decodes 4:4:4 here; the platform part is not tested\n");
    }
    // No format in common: both ends fail and say what each takes.
    {
        FormatEncoder encoder;
        FormatDecoder decoder;
        auto sharing = config(RCS_ROLE_SHARE, 47907, key);
        sharing.setCaptureSource(RCS_CAPTURE_HOST)
            .setCodecs({RCS_CODEC_HOST_FIRST})
            .setPictureFormats({RCS_PICTURE_444_8})
            .use(rcsdk::hostEncoder(encoder, {RCS_CODEC_HOST_FIRST}, "formats", false, RCS_ACCELERATION_HARDWARE, true,
                            full444));
        auto viewing = config(RCS_ROLE_VIEW, 47907, key);
        viewing.setCodecs({RCS_CODEC_HOST_FIRST})
            .use(rcsdk::hostDecoder(decoder, {RCS_CODEC_HOST_FIRST}, "formats", RCS_ACCELERATION_HARDWARE, sdr8));
        rcsdk::Session share(sharing);
        rcsdk::Session view(viewing);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED && share.state() == RCS_STATE_FAILED; }));
        for (const auto* end : {&view, &share}) {
            const auto failure = end->failure();
            CHECK(failure.result == RCS_ERROR_UNSUPPORTED);
            CHECK(failure.message.find("No picture format in common: in host codec 16 the sharer sends 4:4:4 8-bit "
                                       "and the viewer takes 4:2:0 8-bit") != std::string::npos);
        }
        CHECK(encoder.opened.empty() && decoder.opened.empty());
    }
    // What a system cannot do is refused at start, with the reason.
    {
        // CPU frames are BGRA8, SDR: no HDR10 with on_frame.
        rcsdk::Callbacks cpu;
        cpu.onFrame = [](const rcs_frame&) {};
        auto viewing = config(RCS_ROLE_VIEW, 47907, key);
        viewing.setPictureFormats({RCS_PICTURE_420_8, RCS_PICTURE_HDR10_420});
        rcsdk::Session view(viewing, cpu);
        CHECK(view.start() == RCS_ERROR_UNSUPPORTED &&
              view.failure().message.find("on_gpu_frame") != std::string::npos);
        // A sharer's preview through on_frame neither.
        auto sharing = config(RCS_ROLE_SHARE, 47907, key);
        sharing.setPictureFormats({RCS_PICTURE_HDR10_420, RCS_PICTURE_420_8});
        rcsdk::Session share(sharing, cpu);
        CHECK(share.start() == RCS_ERROR_UNSUPPORTED && share.failure().message.find("on_frame") != std::string::npos);
        // HDR does not reach the host's encoder.
        FormatEncoder encoder;
        auto hosted = config(RCS_ROLE_SHARE, 47907, key);
        hosted.setCodecs({RCS_CODEC_HOST_FIRST})
            .setPictureFormats({RCS_PICTURE_HDR10_420})
            .use(rcsdk::hostEncoder(encoder, {RCS_CODEC_HOST_FIRST}, "formats", false, RCS_ACCELERATION_HARDWARE, true,
                            1u << RCS_PICTURE_HDR10_420));
        rcsdk::Session hostShare(hosted);
        CHECK(hostShare.start() == RCS_ERROR_UNSUPPORTED &&
              hostShare.failure().message.find("RCS_EXT_HOST_ENCODER") != std::string::npos);
        // 4:4:4 in a codec this system decodes, but not in 4:4:4 (AV1's
        // hardware decoders, OpenH264), where there is one.
        for (const rcs_codec codec : {RCS_CODEC_AV1, RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_VP9}) {
            uint32_t decodes = 0;
            bool decoder = false;
            for (const auto& i : rcsdk::codecImplementations()) {
                if (i.codec == codec && i.direction == RCS_CODEC_DECODER && i.acceleration == testAcceleration()) {
                    decoder = true;
                    decodes |= i.picture_formats;
                }
            }
            if (!decoder || decodes & (1u << RCS_PICTURE_444_8)) {
                continue;
            }
            auto viewing444 = config(RCS_ROLE_VIEW, 47907, key);
            viewing444.setCodecs({codec}).setPictureFormats({RCS_PICTURE_444_8});
            rcsdk::Session high(viewing444);
            CHECK(high.start() == RCS_ERROR_UNSUPPORTED &&
                  high.failure().message.find("cannot decode 4:4:4 8-bit pictures (formats[])") != std::string::npos);
            break;
        }
    }
    // Values no version defines are mistakes: unknown formats, a list with
    // gaps or twice the same, a request outside the list, BT.2020's matrix for SDR.
    rcs_session never = nullptr;
    const auto refused = [&](rcsdk::SessionConfig c, const char* why) {
        CHECK(rcs_session_create(&c.get(), nullptr, &never) == RCS_ERROR_INVALID_ARGUMENT && !never);
        CHECK(rcsdk::lastCallError().find(why) != std::string::npos);
    };
    auto unknown = config(RCS_ROLE_SHARE, 47907, key);
    unknown.encoder().formats[0] = RCS_PICTURE_HDR10_444 + 1;
    refused(unknown, "unknown picture format");
    auto twice = config(RCS_ROLE_VIEW, 47907, key);
    twice.setPictureFormats({RCS_PICTURE_444_8, RCS_PICTURE_444_8});
    refused(twice, "lists a picture format twice");
    auto none = config(RCS_ROLE_VIEW, 47907, key);
    none.decoder().formats[0] = 0;
    refused(none, "lists no picture format");
    auto asked = config(RCS_ROLE_VIEW, 47907, key);
    asked.setVideoRequest(0, 0, 0, 0, 0, RCS_PICTURE_444_8);
    refused(asked, "format must be one of RCS_EXT_DECODER's formats[]");
    auto matrix = config(RCS_ROLE_SHARE, 47907, key);
    matrix.setColor(RCS_MATRIX_BT2020, RCS_RANGE_LIMITED);
    refused(matrix, "matrix BT2020");
    auto range = config(RCS_ROLE_SHARE, 47907, key);
    range.setColor(RCS_MATRIX_BT709, rcs_color_range(0));
    refused(range, "unknown range");
}

// The host's decoder decodes the system encoder's H.264 and keeps the
// pictures: packets in order, a reset after loss, a new open for a new size,
// one failure recovered from, two in a row the end of the run.
void hostDecoder(const uint8_t (&key)[32]) {
    WrappedDecoder decoder;
    EventLog viewEvents;
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onEvent = viewEvents.callback();
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47903, key).setTransport(RCS_TRANSPORT_UDP));
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47903));
    auto viewing = config(RCS_ROLE_VIEW, proxy.address().port(), key).setTransport(RCS_TRANSPORT_UDP);
    viewing.use(rcsdk::hostDecoder(decoder, {RCS_CODEC_H264}, "wrapped decoder", RCS_ACCELERATION_SOFTWARE));
    rcsdk::Session view(viewing, viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return decoder.pictures >= 30; }));
    CHECK(decoder.opens == 1 && decoder.width == 640 && decoder.height == 360 && frames == 0);
    CHECK(view.stats<rcs_video_receive_stats>().decoded_frames >= uint64_t(decoder.packets.load()) - 1);
    const auto opened = codecEvents(viewEvents);
    CHECK(!opened.empty() && opened.back().direction == RCS_CODEC_DECODER &&
          opened.back().backend == RCS_BACKEND_HOST && std::string(opened.back().name) == "wrapped decoder");
    // A decoder that falls behind: the queue drops its backlog and the decoder
    // starts over at a keyframe.
    decoder.slowMs = 150;
    CHECK(waitFor([&] { return decoder.resets > 0; }, 8s));
    decoder.slowMs = 0;
    // A new size: closed and opened again, at the new size.
    auto video = rcsdk::defaults<rcs_share_video_config>();
    video.width = 320;
    video.height = 180;
    video.frames_per_second = 30;
    video.bitrate_bps = 2'000'000;
    CHECK(share.update(video) == RCS_OK);
    CHECK(waitFor([&] { return decoder.width == 320 && decoder.pictures > 0; }));
    CHECK(decoder.closes >= 1);
    // One failure: a new decoder, from the next keyframe.
    const int pictures = decoder.pictures;
    decoder.failNext = 1;
    CHECK(waitFor([&] {
        const auto events = viewEvents.of(RCS_EVENT_RECOVERY);
        return std::any_of(events.begin(), events.end(),
                           [](const rcs_event& e) { return e.recovery.reason == RCS_RECOVERY_DECODER; });
    }));
    CHECK(waitFor([&] { return decoder.pictures > pictures + 10; }));
    // Two in a row: the run ends with the host's result.
    decoder.failNext = 1000;
    CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }));
    CHECK(view.failure().result == RCS_ERROR_DEVICE &&
          view.failure().message.find("wrapped decoder") != std::string::npos);
    share.stop();
}

// The host's own bitstream: the SDK neither captures nor encodes, says when a
// keyframe is due and whether the path has room, checks what it is given and
// sends every packet it took.
void encodedSource(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    std::atomic<int> frames{0};
    std::atomic<bool> keyframeDue{false};
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = [&](const rcs_event& event) {
        if (event.type == RCS_EVENT_KEYFRAME) {
            keyframeDue = true;
        }
        shareEvents.callback()(event);
    };
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    auto sharing = config(RCS_ROLE_SHARE, 47904, key).setTransport(RCS_TRANSPORT_UDP);
    sharing.setCaptureSource(RCS_CAPTURE_ENCODED).extension<rcs_video_tuning_config>().keyframe_interval_ms = 1000;
    rcsdk::Session share(sharing, shareCallbacks);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47904, key).setTransport(RCS_TRANSPORT_UDP), viewCallbacks);
    const uint8_t nal[] = {0, 0, 0, 1, 0x41};
    rcs_encoded_packet stray = RCS_ENCODED_PACKET_INIT;
    stray.codec = RCS_CODEC_H264;
    stray.width = 640;
    stray.height = 360;
    stray.data = nal;
    stray.data_size = sizeof(nal);
    CHECK(rcsdk::pushEncodedVideo(share, stray) == RCS_ERROR_INVALID_STATE); // No stream yet.
    CHECK(rcsdk::videoReady(share) == RCS_ERROR_INVALID_STATE);
    CHECK(rcsdk::pushEncodedVideo(view, stray) == RCS_ERROR_UNSUPPORTED);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return !shareEvents.of(RCS_EVENT_KEYFRAME).empty(); }));
    CHECK(shareEvents.of(RCS_EVENT_KEYFRAME).front().keyframe.reason == RCS_KEYFRAME_START);
    CHECK(codecEvents(shareEvents).empty()); // No encoder of the SDK's.
    // What is checked before anything goes out.
    CHECK(rcsdk::pushEncodedVideo(share, stray) == RCS_ERROR_INVALID_STATE); // A stream starts with a keyframe.
    stray.keyframe = 1;
    CHECK(rcsdk::pushEncodedVideo(share, stray) == RCS_ERROR_INVALID_ARGUMENT); // No parameter sets.
    CHECK(rcsdk::lastCallError().find("parameter sets") != std::string::npos);
    stray.width = 320;
    CHECK(rcsdk::pushEncodedVideo(share, stray) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcsdk::lastCallError().find("640x360") != std::string::npos);
    const uint8_t broken[] = {0, 0, 0, 9, 0x67};
    stray.width = 640;
    stray.config = broken;
    stray.config_size = sizeof(broken);
    CHECK(rcsdk::pushEncodedVideo(share, stray) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcsdk::lastCallError().find("NAL unit") != std::string::npos);

    // The host's encoder (one of the SDK's), fed pictures of its own, keyframes
    // as the SDK says, a picture only when the path has room.
    std::atomic<bool> running{true};
    std::atomic<int> pushed{0}, skipped{0};
    std::thread host([&] {
        const auto info = share.streamInfo();
        const rcsdk::platform::EncoderConfig config{RCS_CODEC_H264, info.width,         info.height,      30,
                                                    2'000'000,      testAcceleration(), RCS_BACKEND_AUTO, {}};
        auto encoder = rcsdk::codecs::createEncoder(config, rcsdk::codecs::implementations());
        encoder->open(config);
        std::vector<uint8_t> pixels(size_t(info.width) * info.height * 4);
        uint64_t sequence = 0;
        for (auto next = std::chrono::steady_clock::now(); running; next += 33ms) {
            std::this_thread::sleep_until(next);
            for (rcsdk::EncodedPacket packet; encoder->receive(packet, 0ms);) {
                CHECK(rcsdk::pushEncodedVideo(share, rcsdk::publicPacket(packet)) == RCS_OK);
                ++pushed;
            }
            if (rcsdk::videoReady(share) != RCS_OK) {
                ++skipped;
                continue;
            }
            std::fill(pixels.begin(), pixels.end(), uint8_t(sequence * 7));
            rcsdk::VideoFrame frame;
            frame.width = info.width;
            frame.height = info.height;
            frame.stride = info.width * 4;
            frame.pixels = std::make_shared<const std::vector<uint8_t>>(pixels);
            frame.sequence = ++sequence;
            frame.timestampNs = rcsdk::monotonicNs();
            if (keyframeDue.exchange(false)) {
                encoder->requestKeyframe();
            }
            encoder->submit(frame);
        }
    });
    CHECK(waitFor([&] { return frames >= 30; }));
    CHECK(view.streamInfo().codec == RCS_CODEC_H264 && share.stats<rcs_video_send_stats>().encoded_frames >= 30);
    // A keyframe the viewer asks for, and the interval's.
    CHECK(view.requestKeyframe() == RCS_OK);
    const auto reasons = [&](rcs_keyframe_reason reason) {
        const auto events = shareEvents.of(RCS_EVENT_KEYFRAME);
        return std::any_of(events.begin(), events.end(), [&](const auto& e) { return e.keyframe.reason == reason; });
    };
    CHECK(waitFor([&] { return reasons(RCS_KEYFRAME_REQUESTED); }));
    CHECK(waitFor([&] { return reasons(RCS_KEYFRAME_INTERVAL); }, 3s));
    running = false;
    host.join();
    CHECK(pushed >= 30); // As many as were decoded, at least.
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// ---- The encoded packets, to the host ----------------------------------------------------------

// Copies of the packets an end hands its host (RCS_EXT_ENCODED_OUTPUT), in
// the order they came; `onVideo` runs inside the call first (a slow host).
struct PacketLog final : rcsdk::EncodedOutput {
    struct Picture {
        rcs_codec codec = 0;
        uint32_t width = 0, height = 0;
        bool keyframe = false, afterGap = false;
        uint64_t sequence = 0, timestamp = 0;
        std::vector<uint8_t> data, config;
    };
    struct Sound {
        rcs_viewer_id viewer = 0;
        rcs_audio_codec codec = 0;
        uint32_t frames = 0;
        uint64_t sequence = 0, timestamp = 0;
        std::vector<uint8_t> data;
    };
    std::mutex mutex;
    std::vector<Picture> pictures;
    std::vector<Sound> sounds;
    std::function<void()> onVideo;

    void video(const rcs_encoded_packet& p) override {
        CHECK(p.struct_size == sizeof(p) && p.data && p.data_size && (p.config || !p.config_size));
        if (onVideo) {
            onVideo();
        }
        Picture copy{p.codec,       p.width,        p.height,
                     p.keyframe != 0, p.after_gap != 0, p.sequence,
                     p.timestamp_ns, {p.data, p.data + p.data_size}, {p.config, p.config + p.config_size}};
        std::lock_guard lock(mutex);
        pictures.push_back(std::move(copy));
    }
    void audio(const rcs_encoded_audio& p) override {
        CHECK(p.struct_size == sizeof(p) && p.sample_rate == 48000 && p.channels == 2 && p.frames > 0 && p.data &&
              p.data_size > 0 && !p.config && !p.config_size); // Opus: no configuration.
        std::lock_guard lock(mutex);
        sounds.push_back({p.viewer_id, p.codec, p.frames, p.sequence, p.timestamp_ns, {p.data, p.data + p.data_size}});
    }
    size_t pictureCount() {
        std::lock_guard lock(mutex);
        return pictures.size();
    }
    size_t soundCount() {
        std::lock_guard lock(mutex);
        return sounds.size();
    }
    size_t keyframes() {
        std::lock_guard lock(mutex);
        return size_t(std::count_if(pictures.begin(), pictures.end(), [](const Picture& p) { return p.keyframe; }));
    }
    bool resumed() {
        std::lock_guard lock(mutex);
        return std::any_of(pictures.begin(), pictures.end(), [](const Picture& p) { return p.afterGap; });
    }
};

// The viewer's pictures are the sharer's, byte for byte and in its order,
// each with the parameter sets: they start at a keyframe and, where some of
// the sharer's are missing, resume at a picture marked after_gap (and only
// there): a keyframe, or with `references` (the sharer's encoder drops lost
// pictures from its references) one that refers to none of them. Returns how
// many times they resumed.
size_t samePictures(PacketLog& sharer, PacketLog& viewer, bool references = false) {
    std::scoped_lock lock(sharer.mutex, viewer.mutex);
    CHECK(!viewer.pictures.empty() && viewer.pictures.front().keyframe && !viewer.pictures.front().afterGap);
    size_t at = 0, gaps = 0;
    for (size_t i = 0; i < viewer.pictures.size(); ++i) {
        const auto& p = viewer.pictures[i];
        const size_t from = at;
        while (at < sharer.pictures.size() && sharer.pictures[at].sequence != p.sequence) {
            ++at;
        }
        CHECK(at < sharer.pictures.size());
        if (at == sharer.pictures.size()) {
            break;
        }
        const auto& s = sharer.pictures[at++];
        CHECK(p.codec == RCS_CODEC_H264 && p.codec == s.codec && p.width == s.width && p.height == s.height &&
              p.keyframe == s.keyframe && p.timestamp == s.timestamp && p.data == s.data && p.config == s.config);
        CHECK(!p.config.empty() && !s.afterGap);
        if (i > 0) {
            CHECK((at - 1 != from) == p.afterGap);
        }
        CHECK(!p.afterGap || p.keyframe || references);
        gaps += p.afterGap;
    }
    return gaps;
}

// The viewer's sound packets are the ones the sharer encoded for it, byte for
// byte, in order (a gap is lost sound).
void sameSounds(PacketLog& sharer, PacketLog& viewer) {
    std::scoped_lock lock(sharer.mutex, viewer.mutex);
    CHECK(!sharer.sounds.empty() && !viewer.sounds.empty());
    const rcs_viewer_id id = sharer.sounds.front().viewer;
    std::map<uint64_t, const PacketLog::Sound*> sent;
    for (const auto& s : sharer.sounds) {
        CHECK(s.viewer == id && s.codec == RCS_AUDIO_OPUS);
        sent[s.sequence] = &s;
    }
    CHECK(id != 0);
    for (size_t i = 0; i < viewer.sounds.size(); ++i) {
        const auto& p = viewer.sounds[i];
        const auto found = sent.find(p.sequence);
        CHECK(found != sent.end() && p.viewer == 0 && (i == 0 || p.sequence > viewer.sounds[i - 1].sequence));
        if (found == sent.end()) {
            break;
        }
        const auto& s = *found->second;
        CHECK(p.codec == s.codec && p.frames == s.frames && p.timestamp == s.timestamp && p.data == s.data);
    }
}

// A viewer gets the encoded packets as they arrive and decodes them as
// usual: the sharer's pictures and sound byte for byte, on the sharer's
// clock, a keyframe when asked; after pictures were lost the packets resume
// at a picture marked after_gap.
void encodedPackets(const uint8_t (&key)[32]) {
    PacketLog shared, received;
    EventLog shareEvents, viewEvents;
    std::atomic<int> frames{0};
    std::atomic<uint64_t> sound{0};
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onEvent = viewEvents.callback();
    viewCallbacks.onFrame = [&](const rcs_frame&) { ++frames; };
    viewCallbacks.onAudio = [&](const rcs_audio_frame& f) { sound += f.frames; };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47985, key).use(rcsdk::encodedOutput(shared)), shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47985));
    // A queue that never drops for age: what is missing below is what the path lost.
    auto viewing = config(RCS_ROLE_VIEW, proxy.address().port(), key).use(rcsdk::encodedOutput(received));
    rcsdk::Session view(decodeQueue(viewing, 32, 0), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return received.pictureCount() >= 30 && received.soundCount() >= 50 && frames >= 20; }));
    CHECK(sound > 0); // Decoded as usual besides.
    const size_t keyframes = received.keyframes();
    CHECK(view.requestKeyframe() == RCS_OK);
    CHECK(waitFor([&] { return received.keyframes() > keyframes; }));
    const auto encoders = codecEvents(shareEvents);
    const bool references = !encoders.empty() && encoders.back().ref_invalidation;
    samePictures(shared, received, references); // Without gaps unless a loaded machine lost some.
    sameSounds(shared, received);
    {
        // Pictures and sound on one clock, the sharer's (this process's here):
        // the latest of each were captured within a second of each other, and of now.
        std::scoped_lock lock(received.mutex);
        const uint64_t now = rcsdk::monotonicNs();
        const uint64_t picture = received.pictures.back().timestamp, tone = received.sounds.back().timestamp;
        std::printf("latest picture %.1f ms ago, latest sound %.1f ms ago\n", (now - picture) / 1e6,
                    (now - tone) / 1e6);
        CHECK(picture <= now && tone <= now && now - picture < 2'000'000'000 && now - tone < 2'000'000'000);
        CHECK((picture > tone ? picture - tone : tone - picture) < 1'000'000'000);
    }

    // Nothing passes for longer than the sharer keeps what it sent (1 s): the
    // transport gives up on those pictures, and the packets resume at a
    // keyframe, or at a picture that refers to none of them where the encoder
    // drops them from its references (VideoToolbox's long-term references).
    proxy.outage = true;
    std::this_thread::sleep_for(1500ms);
    proxy.outage = false;
    CHECK(waitFor(
        [&] {
            const auto events = viewEvents.of(RCS_EVENT_RECOVERY);
            return std::any_of(events.begin(), events.end(),
                               [](const rcs_event& e) { return e.recovery.reason == RCS_RECOVERY_LOSS; });
        },
        8s));
    CHECK(waitFor([&] { return received.resumed(); }, 8s));
    const int before = frames;
    CHECK(waitFor([&] { return frames >= before + 10; }));
    const size_t gaps = samePictures(shared, received, references);
    CHECK(gaps >= 1);
    sameSounds(shared, received);
    std::printf("viewer's packets: %zu pictures (%zu of the sharer's), %zu sound packets, resumed %zu times\n",
                received.pictureCount(), shared.pictureCount(), received.soundCount(), gaps);
    CHECK(view.stop() == RCS_OK && share.stop() == RCS_OK);
}

// decode 0: the viewer takes the packets without decoding them (recording
// without watching). No decoder opens and nothing is decoded; the offer is
// what the host listed; a host that falls behind gets packets that resume at
// a keyframe marked after_gap.
void recordOnly(const uint8_t (&key)[32]) {
    {
        PacketLog unused;
        rcsdk::Callbacks pictures;
        pictures.onFrame = [](const rcs_frame&) {};
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47986, key).use(rcsdk::encodedOutput(unused, false)), pictures);
        CHECK(view.start() == RCS_ERROR_INVALID_ARGUMENT &&
              view.failure().message.find("decode 0") != std::string::npos);
    }
    PacketLog shared, received;
    std::atomic<bool> slow{false};
    received.onVideo = [&] {
        if (slow) {
            std::this_thread::sleep_for(150ms);
        }
    };
    EventLog viewEvents;
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onEvent = viewEvents.callback();
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47986, key).use(rcsdk::encodedOutput(shared)));
    // What it can store, whatever this system decodes: a host codec without a
    // host decoder, and every listed format in each codec (AV1 4:4:4 too).
    auto viewing = config(RCS_ROLE_VIEW, 47986, key)
                       .setCodecs({RCS_CODEC_AV1, RCS_CODEC_HOST_FIRST, RCS_CODEC_H264})
                       .setPictureFormats({RCS_PICTURE_444_8, RCS_PICTURE_420_8})
                       .use(rcsdk::encodedOutput(received, false));
    rcsdk::Session view(decodeQueue(viewing, 2, 40), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return received.pictureCount() >= 30 && received.soundCount() >= 50; }));
    // The offer is the list, each codec at its largest picture.
    const auto peer = share.peerInfo();
    CHECK(peer.codecs == ((1u << RCS_CODEC_AV1) | (1u << RCS_CODEC_HOST_FIRST) | h264Bit));
    constexpr uint32_t formats = (1u << RCS_PICTURE_444_8) | (1u << RCS_PICTURE_420_8);
    const rcs_codec_limit expected[] = {{RCS_CODEC_AV1, 8192, 8192, formats},
                                        {RCS_CODEC_HOST_FIRST, 8192, 8192, formats},
                                        {RCS_CODEC_H264, 4096, 4096, formats}};
    for (size_t i = 0; i < std::size(expected); ++i) {
        const auto& l = peer.codec_limits[i];
        CHECK(l.codec == expected[i].codec && l.max_width == expected[i].max_width &&
              l.max_height == expected[i].max_height && l.picture_formats == expected[i].picture_formats);
    }
    CHECK(view.streamInfo().codec == RCS_CODEC_H264 && view.streamInfo().format == RCS_PICTURE_420_8);
    // Nothing decodes, the sound included; the packets are the sharer's.
    const auto video = view.stats<rcs_video_receive_stats>();
    const auto audio = view.stats<rcs_audio_receive_stats>();
    CHECK(video.decoded_frames == 0 && video.delivered_frames == 0 && !video.decoder[0] && video.received_packets >= 30);
    CHECK(audio.active && audio.codec == RCS_AUDIO_OPUS && audio.received_packets > 0 && !audio.decoder[0] &&
          !audio.device[0] && audio.played_frames == 0);
    CHECK(codecEvents(viewEvents).empty());
    const auto sounds = viewEvents.of(RCS_EVENT_AUDIO);
    CHECK(!sounds.empty() && sounds.back().audio.active && sounds.back().audio.codec == RCS_AUDIO_OPUS);
    samePictures(shared, received);
    sameSounds(shared, received);

    // A host that falls behind: the queue drops what waited and the packets
    // resume at a keyframe, marked after_gap.
    slow = true;
    std::this_thread::sleep_for(1s);
    slow = false;
    CHECK(waitFor([&] { return received.resumed(); }));
    const size_t count = received.pictureCount();
    CHECK(waitFor([&] { return received.pictureCount() >= count + 10; }));
    const auto recoveries = viewEvents.of(RCS_EVENT_RECOVERY);
    CHECK(!recoveries.empty() && recoveries.front().recovery.reason == RCS_RECOVERY_BACKLOG);
    const size_t gaps = samePictures(shared, received);
    const uint64_t dropped = view.stats<rcs_video_receive_stats>().dropped_packets;
    CHECK(gaps >= 1 && dropped > 0);
    std::printf("record only: %zu pictures (%zu of the sharer's, %llu dropped behind a slow host), %zu sound "
                "packets, resumed %zu times\n",
                received.pictureCount(), shared.pictureCount(), (unsigned long long)dropped, received.soundCount(),
                gaps);
    CHECK(view.stop() == RCS_OK && share.stop() == RCS_OK);
}

// What cannot work is refused when the session is made; a host codec that
// fails ends the run with its result and its name.
// A whole session without a codec of the system's, as where no platform layer
// encodes or decodes (WebAssembly, a Linux without codecs): the sharer pushes
// a bitstream (RCS_CAPTURE_ENCODED), the viewer takes the packets
// (RCS_EXT_ENCODED_OUTPUT, decode 0), over the host's datagram and stream
// channels. Handshake, encryption, the UDP protocol, pacing and adaptive
// bitrate all run; every picture arrives whole and in order.
void hostEncoded(const uint8_t (&key)[32]) {
    namespace probes = rcsdk::platform::probes;
    // Each picture: the probe keyframe and a filler NAL unit (type 12) of 20 KB.
    std::vector<uint8_t> data(std::begin(probes::h264_444_8_data), std::end(probes::h264_444_8_data));
    constexpr uint32_t filler = 20'000;
    for (int shift = 24; shift >= 0; shift -= 8) {
        data.push_back(uint8_t((filler + 1) >> shift));
    }
    data.push_back(0x0c);
    data.insert(data.end(), filler - 1, 0xff);
    data.push_back(0x80);
    rcs_encoded_packet packet = RCS_ENCODED_PACKET_INIT;
    packet.codec = RCS_CODEC_H264;
    packet.format = RCS_PICTURE_444_8;
    packet.width = probes::width;
    packet.height = probes::height;
    packet.keyframe = 1;
    packet.data = data.data();
    packet.data_size = data.size();
    packet.config = probes::h264_444_8_config;
    packet.config_size = sizeof(probes::h264_444_8_config);
    for (const rcs_host_channel kind : {RCS_HOST_CHANNEL_DATAGRAM, RCS_HOST_CHANNEL_STREAM}) {
        rcsdk::test::TestChannel channel(kind);
        rcsdk::test::FunctionTransport shareSend(channel.sender(0)), viewSend(channel.sender(1));
        auto sharing = hostConfig(RCS_ROLE_SHARE, key, kind, shareSend)
                           .setCodecs({RCS_CODEC_H264})
                           .setVideo(probes::width, probes::height, 60, 20'000'000)
                           .setCaptureSource(RCS_CAPTURE_ENCODED)
                           .setSendAudio(false);
        sharing.extension<rcs_encoder_config>().formats[0] = RCS_PICTURE_444_8;
        // The host's encoder chose: HARDWARE_FIRST is announced as hardware, which viewers take.
        sharing.extension<rcs_encoder_config>().acceleration = RCS_ACCELERATION_HARDWARE_FIRST;
        PacketLog received;
        auto viewing = hostConfig(RCS_ROLE_VIEW, key, kind, viewSend)
                           .setCodecs({RCS_CODEC_H264})
                           .use(rcsdk::encodedOutput(received, false));
        viewing.extension<rcs_decoder_config>().formats[0] = RCS_PICTURE_444_8;
        rcsdk::Session share(sharing), view(viewing);
        const auto shareEnd = channel.attachment(0, share);
        const auto viewEnd = channel.attachment(1, view);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return rcsdk::videoReady(share) != RCS_ERROR_INVALID_STATE; }, 10s));
        // Two seconds at 60 pictures a second, each pushed as the path takes it.
        int pushed = 0;
        for (auto next = std::chrono::steady_clock::now(); pushed < 120; next += 16667us) {
            std::this_thread::sleep_until(next);
            packet.timestamp_ns = rcsdk::monotonicNs();
            if (rcsdk::videoReady(share) == RCS_OK && rcsdk::pushEncodedVideo(share, packet) == RCS_OK) {
                ++pushed;
            }
        }
        CHECK(waitFor([&] { return received.pictureCount() == size_t(pushed); }, 5s));
        CHECK(view.streamInfo().encoder_acceleration == RCS_ACCELERATION_HARDWARE);
        std::lock_guard lock(received.mutex);
        for (size_t i = 0; i < received.pictures.size(); ++i) {
            const auto& picture = received.pictures[i];
            CHECK(picture.data == data && picture.width == probes::width && picture.keyframe);
            CHECK(!i || picture.sequence > received.pictures[i - 1].sequence);
        }
        std::printf("%s channel: %zu of %d pictures\n", kind == RCS_HOST_CHANNEL_DATAGRAM ? "Datagram" : "Stream",
                    received.pictures.size(), pushed);
        CHECK(channel.unexpected == 0);
    }

    // VP9 packets without a configuration: the SDK writes 'vpcC' from the stream.
    rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_STREAM);
    rcsdk::test::FunctionTransport shareSend(channel.sender(0)), viewSend(channel.sender(1));
    auto sharing = hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_STREAM, shareSend)
                       .setCodecs({RCS_CODEC_VP9})
                       .setVideo(probes::width, probes::height, 60, 20'000'000)
                       .setCaptureSource(RCS_CAPTURE_ENCODED)
                       .setSendAudio(false);
    sharing.extension<rcs_encoder_config>().formats[0] = RCS_PICTURE_420_10;
    PacketLog received;
    auto viewing = hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_STREAM, viewSend)
                       .setCodecs({RCS_CODEC_VP9})
                       .use(rcsdk::encodedOutput(received, false));
    viewing.extension<rcs_decoder_config>().formats[0] = RCS_PICTURE_420_10;
    rcsdk::Session share(sharing), view(viewing);
    const auto shareEnd = channel.attachment(0, share);
    const auto viewEnd = channel.attachment(1, view);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return rcsdk::videoReady(share) != RCS_ERROR_INVALID_STATE; }, 10s));
    rcs_encoded_packet vp9 = RCS_ENCODED_PACKET_INIT;
    vp9.codec = RCS_CODEC_VP9;
    vp9.format = RCS_PICTURE_420_10;
    vp9.width = probes::width;
    vp9.height = probes::height;
    vp9.keyframe = 1;
    vp9.data = probes::vp9_420_10_data;
    vp9.data_size = sizeof(probes::vp9_420_10_data);
    int pushed = 0;
    for (auto next = std::chrono::steady_clock::now(); pushed < 10; next += 16667us) {
        std::this_thread::sleep_until(next);
        vp9.timestamp_ns = rcsdk::monotonicNs();
        if (rcsdk::videoReady(share) == RCS_OK && rcsdk::pushEncodedVideo(share, vp9) == RCS_OK) {
            ++pushed;
        }
    }
    CHECK(waitFor([&] { return received.pictureCount() == size_t(pushed); }, 5s));
    const auto record = rcsdk::vpx::writeConfig(rcsdk::vpx::streamConfig(
        RCS_CODEC_VP9, rcsdk::picture(RCS_PICTURE_420_10, RCS_MATRIX_BT709, RCS_RANGE_LIMITED), probes::width,
        probes::height, 60));
    std::lock_guard lock(received.mutex);
    for (const auto& picture : received.pictures) {
        CHECK(picture.codec == RCS_CODEC_VP9 && picture.config == record && picture.keyframe);
    }
    std::printf("VP9 without configuration: %zu of %d pictures, 'vpcC' written\n", received.pictures.size(), pushed);
}

void hostCodecErrors(const uint8_t (&key)[32]) {
    rcs_session never = nullptr;
    const auto refused = [&](const rcsdk::SessionConfig& c, const char* why) {
        const bool ok = rcs_session_create(&c.get(), nullptr, &never) == RCS_ERROR_INVALID_ARGUMENT && !never &&
                        rcsdk::lastCallError().find(why) != std::string::npos;
        if (!ok) {
            std::printf("not refused for \"%s\": %s\n", why, rcsdk::lastCallError().c_str());
        }
        return ok;
    };
    WrappedEncoder anyEncoder;
    auto both = config(RCS_ROLE_SHARE, 47905, key).setCaptureSource(RCS_CAPTURE_ENCODED);
    both.use(rcsdk::hostEncoder(anyEncoder, {RCS_CODEC_H264}, "unneeded"));
    CHECK(refused(both, "ENCODED"));
    auto unhandled = config(RCS_ROLE_SHARE, 47905, key);
    unhandled.setCodecs({RCS_CODEC_HOST_FIRST + 1});
    CHECK(refused(unhandled, "host codec 17"));
    auto chosen = config(RCS_ROLE_SHARE, 47905, key);
    chosen.encoder().backend = RCS_BACKEND_HOST;
    CHECK(refused(chosen, "RCS_BACKEND_HOST"));
    auto unnamed = config(RCS_ROLE_SHARE, 47905, key);
    unnamed.use(rcsdk::hostEncoder(anyEncoder, {RCS_CODEC_H264}, "x"));
    unnamed.extension<rcs_host_encoder_config>().name[0] = '\0';
    CHECK(refused(unnamed, "name"));
    auto twice = config(RCS_ROLE_SHARE, 47905, key);
    twice.use(rcsdk::hostEncoder(anyEncoder, {RCS_CODEC_H264, RCS_CODEC_H264}, "twice"));
    CHECK(refused(twice, "twice"));
    auto viewOnly = config(RCS_ROLE_SHARE, 47905, key);
    WrappedDecoder anyDecoder;
    viewOnly.use(rcsdk::hostDecoder(anyDecoder, {RCS_CODEC_H264}, "decoder"));
    CHECK(refused(viewOnly, "viewing sessions only"));

    // Open fails: the run ends with the host's result, naming the encoder.
    struct Failing final : rcsdk::HostEncoder {
        rcs_result result;
        bool atOpen;
        Failing(rcs_result r, bool open) : result(r), atOpen(open) {}
        rcs_result open(const rcs_host_encoder_settings&) override { return atOpen ? result : RCS_OK; }
        rcs_result encode(const rcs_host_encoder_frame&) override { return atOpen ? RCS_OK : result; }
        void close(uint32_t, uint32_t) override {}
    };
    for (const auto& [result, atOpen, expected] :
         {std::tuple{rcs_result(RCS_ERROR_DEVICE), true, rcs_result(RCS_ERROR_DEVICE)},
          std::tuple{rcs_result(-999), false, rcs_result(RCS_ERROR_CODEC)}}) {
        Failing failing(result, atOpen);
        auto sharing = config(RCS_ROLE_SHARE, 47905, key);
        sharing.use(rcsdk::hostEncoder(failing, {RCS_CODEC_H264}, "failing encoder"));
        rcsdk::Session share(sharing);
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47905, key));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }));
        const auto failure = share.failure();
        CHECK(failure.result == expected && failure.message.find("failing encoder") != std::string::npos);
        CHECK(atOpen || failure.message.find("-999") != std::string::npos);
    }

    // An encoder that takes frames and never gives a packet: the run ends
    // with CODEC and the reason, not a stream that stops unseen.
    struct Silent final : rcsdk::HostEncoder {
        rcs_result open(const rcs_host_encoder_settings&) override { return RCS_OK; }
        rcs_result encode(const rcs_host_encoder_frame&) override { return RCS_OK; }
        void close(uint32_t, uint32_t) override {}
    } silent;
    auto sharing = config(RCS_ROLE_SHARE, 47905, key);
    sharing.use(rcsdk::hostEncoder(silent, {RCS_CODEC_H264}, "silent encoder"));
    rcsdk::Session share(sharing);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47905, key));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }, 10s));
    const auto failure = share.failure();
    CHECK(failure.result == RCS_ERROR_CODEC && failure.message.find("silent encoder gave no picture") != std::string::npos);
}

} // namespace

#ifdef _WIN32
// Windows viewers decode on a host's D3D11 device when given one; start()
// refuses a device the decoder cannot use instead of substituting its own.
void hostDevice(const uint8_t (&key)[32]) {
    using Microsoft::WRL::ComPtr;
    const auto device = [](UINT flags, bool protect) {
        ComPtr<ID3D11Device> created;
        ComPtr<ID3D11DeviceContext> context;
        CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                          D3D11_SDK_VERSION, &created, nullptr, &context)));
        ComPtr<ID3D11Multithread> multithread;
        CHECK(SUCCEEDED(context.As(&multithread)));
        multithread->SetMultithreadProtected(protect);
        return created;
    };
    const auto refused = [&](ID3D11Device* candidate) {
        rcsdk::Session view(config(RCS_ROLE_VIEW, 47834, key).use(rcsdk::gpuDevice(candidate)));
        return view.start() == RCS_ERROR_INVALID_ARGUMENT && !view.failure().message.empty();
    };
    CHECK(refused(device(D3D11_CREATE_DEVICE_VIDEO_SUPPORT, false).Get())); // No multithread protection.
    // Whether a device without D3D11_CREATE_DEVICE_VIDEO_SUPPORT lacks the video
    // interfaces depends on the driver (here it does not), so only the
    // multithread requirement is tested as a refusal.
    const auto host = device(D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, true);
    std::atomic<int> onHost{0}, elsewhere{0};
    rcsdk::Callbacks callbacks;
    callbacks.onGpuFrame = [&](const rcsdk::GpuFrame& frame) {
        ++(frame.info().device == host.Get() ? onHost : elsewhere);
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47834, key));
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47834, key).use(rcsdk::gpuDevice(host.Get())), callbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return onHost >= 10; }));
    CHECK(elsewhere == 0);
}
#endif

namespace {

// One scenario: a ctest of its own (rcsdk.session.<name>, tests/CMakeLists.txt).
// ---- One sharer, many viewers (rcs_viewers_config) ---------------------------------------

// A viewer of these tests: its session, what it decoded, the data it got.
struct TestViewer {
    EventLog events;
    std::atomic<int> frames{0};
    std::mutex mutex;
    std::vector<std::string> data;
    std::unique_ptr<rcsdk::Session> session;

    explicit TestViewer(const rcsdk::SessionConfig& config) {
        rcsdk::Callbacks callbacks;
        callbacks.onEvent = events.callback();
        callbacks.onFrame = [this](const rcs_frame&) {
            ++frames;
        };
        callbacks.onData = [this](const rcs_data_message& message) {
            CHECK(message.viewer_id == 0); // A viewer's messages come from the sharer.
            std::lock_guard lock(mutex);
            data.emplace_back(reinterpret_cast<const char*>(message.data), message.size);
        };
        session = std::make_unique<rcsdk::Session>(config, callbacks);
    }
    // It decodes on: `count` more frames.
    bool watching(int count = 10) {
        const int before = frames;
        return waitFor([&] { return frames >= before + count; }, 10s);
    }
    std::vector<std::string> received() {
        std::lock_guard lock(mutex);
        return data;
    }
};

std::vector<rcs_event> eventsOf(EventLog& log, rcs_event_type type, rcs_viewer_id viewer) {
    auto events = log.of(type);
    std::erase_if(events, [&](const rcs_event& e) { return e.viewer_id != viewer; });
    return events;
}

// The host decides later (RCS_ADMIT_LATER). Over TCP and UDP two viewers wait
// longer than a silent connection lasts, connected, without pictures and
// holding their places; they are decided in the other order, one refused with
// the host's reason; one that leaves while it waits is gone and can no longer
// be decided. A host that does not decide in time refuses (TIMEOUT); one that
// refuses at once gives its reason without the admission's events; one may
// decide from inside on_viewer_join.
void admission(const uint8_t (&key)[32]) {
    const auto admissions = [](EventLog& log, rcs_viewer_id viewer, rcs_admission_state state) {
        auto events = eventsOf(log, RCS_EVENT_ADMISSION, viewer);
        std::erase_if(events, [&](const rcs_event& e) { return e.admission.state != state; });
        return events;
    };
    const auto reached = [&](EventLog& log, rcs_viewer_id viewer, rcs_admission_state state) {
        return waitFor([&] { return !admissions(log, viewer, state).empty(); }, 10s);
    };
    uint16_t port = 47980;
    for (const rcs_transport transport : {RCS_TRANSPORT_TCP, RCS_TRANSPORT_UDP}) {
        ++port;
        EventLog shareEvents;
        rcsdk::Callbacks callbacks;
        callbacks.onEvent = shareEvents.callback();
        callbacks.onViewerJoin = [](const rcs_viewer_info&, rcs_viewer_decision& decision) {
            decision.admission = RCS_ADMIT_LATER;
        };
        rcsdk::Session share(
            config(RCS_ROLE_SHARE, port, key).setTransport(transport).setKeepListening(true).setViewers(2), callbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        const auto viewing = config(RCS_ROLE_VIEW, port, key).setTransport(transport);
        TestViewer a(viewing), b(viewing);
        CHECK(a.session->start() == RCS_OK && reached(shareEvents, 1, RCS_ADMISSION_WAITING));
        CHECK(b.session->start() == RCS_OK && reached(shareEvents, 2, RCS_ADMISSION_WAITING));
        CHECK(reached(a.events, 0, RCS_ADMISSION_WAITING) && reached(b.events, 0, RCS_ADMISSION_WAITING));
        CHECK(admissions(shareEvents, 1, RCS_ADMISSION_WAITING)[0].admission.timeout_ms == 60000);
        const uint32_t left = admissions(a.events, 0, RCS_ADMISSION_WAITING)[0].admission.timeout_ms;
        CHECK(left > 50000 && left <= 60000);
        // Longer than a connection stays silent (5 s): connected, no pictures.
        std::this_thread::sleep_for(6s);
        CHECK(a.session->state() == RCS_STATE_CONNECTING && b.session->state() == RCS_STATE_CONNECTING);
        CHECK(a.frames == 0 && b.frames == 0 && share.viewers().empty());
        // They hold both places.
        TestViewer c(viewing);
        CHECK(c.session->start() == RCS_OK);
        CHECK(waitFor([&] { return c.session->state() == RCS_STATE_FAILED; }));
        CHECK(c.session->failure().result == RCS_ERROR_BUSY);
        // The second is let in first, the first is refused with a reason.
        rcs_viewer_decision decision = RCS_VIEWER_DECISION_INIT;
        CHECK(share.admitViewer(2, decision) == RCS_OK && b.watching());
        CHECK(share.admitViewer(2, decision) == RCS_ERROR_INVALID_STATE); // Decided.
        CHECK(reached(shareEvents, 2, RCS_ADMISSION_ADMITTED) && reached(b.events, 0, RCS_ADMISSION_ADMITTED));
        CHECK(a.session->state() == RCS_STATE_CONNECTING && a.frames == 0 && share.viewers().size() == 1);
        decision.admission = RCS_REFUSE;
        std::snprintf(decision.reason, sizeof(decision.reason), "%s", "Not now, thanks");
        CHECK(share.admitViewer(1, decision) == RCS_OK);
        CHECK(waitFor([&] { return a.session->state() == RCS_STATE_FAILED; }));
        CHECK(a.session->failure().result == RCS_ERROR_REFUSED &&
              a.session->failure().message.find("Not now, thanks") != std::string::npos);
        CHECK(reached(shareEvents, 1, RCS_ADMISSION_REFUSED) && reached(a.events, 0, RCS_ADMISSION_REFUSED));
        for (EventLog* log : {&shareEvents, &a.events}) {
            const auto refused = admissions(*log, log == &shareEvents ? 1 : 0, RCS_ADMISSION_REFUSED)[0].admission;
            CHECK(refused.result == RCS_ERROR_REFUSED && std::string(refused.message) == "Not now, thanks");
        }
        CHECK(admissions(a.events, 0, RCS_ADMISSION_ADMITTED).empty());
        CHECK(share.admitViewer(1, decision) == RCS_ERROR_INVALID_STATE);
        CHECK(share.admitViewer(9, decision) == RCS_ERROR_INVALID_ARGUMENT);
        // One that leaves while it waits (the third id: the busy one got none).
        TestViewer d(viewing);
        CHECK(d.session->start() == RCS_OK && reached(d.events, 0, RCS_ADMISSION_WAITING));
        CHECK(reached(shareEvents, 3, RCS_ADMISSION_WAITING));
        CHECK(d.session->stop() == RCS_OK);
        CHECK(reached(shareEvents, 3, RCS_ADMISSION_GONE));
        CHECK(admissions(shareEvents, 3, RCS_ADMISSION_GONE)[0].admission.result != RCS_OK);
        CHECK(share.admitViewer(3, decision) == RCS_ERROR_INVALID_STATE);
        CHECK(b.watching(5) && share.viewers().size() == 1);
    }

    // A host that does not decide in time; one that refuses at once; one that
    // decides inside on_viewer_join.
    EventLog shareEvents;
    rcsdk::Session* self = nullptr;
    rcsdk::Callbacks callbacks;
    callbacks.onEvent = shareEvents.callback();
    callbacks.onViewerJoin = [&](const rcs_viewer_info& viewer, rcs_viewer_decision& decision) {
        if (viewer.viewer_id == 2) {
            decision.admission = RCS_REFUSE;
            std::snprintf(decision.reason, sizeof(decision.reason), "%s", "Go away");
            return;
        }
        decision.admission = RCS_ADMIT_LATER;
        if (viewer.viewer_id == 3) {
            rcs_viewer_decision now = RCS_VIEWER_DECISION_INIT;
            CHECK(self->admitViewer(3, now) == RCS_OK);
        }
    };
    auto quick = config(RCS_ROLE_SHARE, 47983, key).setKeepListening(true);
    quick.viewers().admission_timeout_ms = 1000;
    rcsdk::Session share(quick, callbacks);
    self = &share;
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    const auto viewing = config(RCS_ROLE_VIEW, 47983, key);
    TestViewer late(viewing);
    CHECK(late.session->start() == RCS_OK);
    CHECK(waitFor([&] { return late.session->state() == RCS_STATE_FAILED; }));
    CHECK(late.session->failure().result == RCS_ERROR_TIMEOUT &&
          late.session->failure().message.find("did not decide within") != std::string::npos);
    const uint32_t left = admissions(late.events, 0, RCS_ADMISSION_WAITING)[0].admission.timeout_ms;
    CHECK(left > 0 && left <= 1000);
    CHECK(admissions(late.events, 0, RCS_ADMISSION_REFUSED)[0].admission.result == RCS_ERROR_TIMEOUT);
    CHECK(reached(shareEvents, 1, RCS_ADMISSION_REFUSED) &&
          admissions(shareEvents, 1, RCS_ADMISSION_REFUSED)[0].admission.result == RCS_ERROR_TIMEOUT);
    TestViewer refused(viewing);
    CHECK(refused.session->start() == RCS_OK);
    CHECK(waitFor([&] { return refused.session->state() == RCS_STATE_FAILED; }));
    CHECK(refused.session->failure().result == RCS_ERROR_REFUSED &&
          refused.session->failure().message.find("Go away") != std::string::npos);
    CHECK(refused.events.of(RCS_EVENT_ADMISSION).empty() && eventsOf(shareEvents, RCS_EVENT_ADMISSION, 2).empty());
    TestViewer inside(viewing);
    CHECK(inside.session->start() == RCS_OK && inside.watching());
    CHECK(reached(shareEvents, 3, RCS_ADMISSION_ADMITTED) && reached(inside.events, 0, RCS_ADMISSION_ADMITTED));
}

// Sound both ways: two viewers send theirs to the sharer, one its host's
// (pushed, 750 Hz), one its microphone (the test's, 500 Hz, with voice
// processing); the sharer receives each on its own (on_audio's viewer_id,
// RCS_EVENT_AUDIO receiving, per viewer statistics) while it sends its
// machine's (1000 Hz) to each. A viewer that stops sending, and a sharer that
// stops receiving, are reported on both ends.
// Each display a picture of its own (rcs_streams_config): the main display
// (id 0 asks for it) 1920x1080 in dark grey, display 7 1024x768 in light
// grey; the pointer shows on display 7 only.
class DisplayCapture final : public rcsdk::platform::ScreenCapture {
public:
    explicit DisplayCapture(uint32_t display) : display_(display) {}
    void start(const rcsdk::platform::CaptureConfig& config, rcsdk::CancelToken) override {
        config_ = config;
        next_ = rcsdk::Clock::now();
    }
    void setCursorInVideo(bool) override {}
    rcsdk::CursorState cursor() const override {
        rcsdk::CursorState state;
        state.visible = display_ == 7;
        state.x = 30000;
        state.y = 20000;
        state.imageSerial = 1;
        auto image = std::make_shared<rcsdk::CursorImage>();
        image->width = image->height = 8;
        image->pixels.assign(8 * 8 * 4, 255);
        state.image = std::move(image);
        return state;
    }
    rcsdk::platform::DisplayMode display() const noexcept override {
        return display_ == 7 ? rcsdk::platform::DisplayMode{1024, 768, 60} : rcsdk::platform::DisplayMode{1920, 1080, 60};
    }
    bool next(rcsdk::VideoFrame& frame, std::chrono::milliseconds timeout, rcsdk::CancelToken cancelled) override {
        const auto deadline = rcsdk::Clock::now() + timeout;
        while (rcsdk::Clock::now() < next_) {
            if (cancelled || rcsdk::Clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        next_ += std::chrono::nanoseconds(1'000'000'000 / config_.framesPerSecond);
        // The display fills the frame's middle, its aspect ratio kept, as captures do.
        const rcsdk::Rect area = content();
        auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(config_.width) * config_.height * 4, 0);
        for (uint32_t y = area.y; y < area.y + area.height; ++y) {
            std::fill_n(pixels->begin() + ptrdiff_t((size_t(y) * config_.width + area.x) * 4), size_t(area.width) * 4,
                        uint8_t(display_ == 7 ? 160 : 40));
        }
        const uint32_t column = uint32_t(frames_ * 7 % config_.width); // It moves: every frame is new.
        for (uint32_t y = 0; y < config_.height; ++y) {
            for (uint32_t x = column; x < std::min(config_.width, column + 4); ++x) {
                (*pixels)[(size_t(y) * config_.width + x) * 4 + 1] = 100;
            }
        }
        frame.width = config_.width;
        frame.height = config_.height;
        frame.stride = config_.width * 4;
        frame.pixels = std::move(pixels);
        frame.sequence = ++frames_;
        frame.timestampNs = rcsdk::monotonicNs();
        return true;
    }
    void stop() noexcept override {}
    uint64_t captured() const noexcept override { return frames_; }
    uint64_t overwritten() const noexcept override { return 0; }
    rcsdk::Rect content() const noexcept override {
        const auto d = display();
        const uint32_t width = std::min(config_.width, d.width * config_.height / d.height / 2 * 2);
        const uint32_t height = std::min(config_.height, d.height * config_.width / d.width / 2 * 2);
        return {(config_.width - width) / 2, (config_.height - height) / 2, width, height};
    }

private:
    const uint32_t display_;
    rcsdk::platform::CaptureConfig config_;
    rcsdk::Clock::time_point next_;
    uint64_t frames_ = 0;
};

// A viewer pauses its picture and its sound (rcs_session_pause), and the
// application's background pauses what rcs_background_config says: the sharer
// sends a paused viewer no stream (its stream ends there) or no sound, no
// longer captures what no viewer takes, and another viewer goes on; resuming
// starts from a keyframe. The host's pause and the background's are their
// own, and each change is reported (RCS_EVENT_PAUSE). A sharer pauses for
// every viewer, and they hear it (by_peer).
void pausing(const uint8_t (&key)[32]) {
    const uint16_t port = 47987;
    const auto lastPause = [](TestViewer& viewer) {
        const auto list = viewer.events.of(RCS_EVENT_PAUSE);
        return list.empty() ? rcs_pause_event{} : list.back().pause;
    };
    const auto pausedAs = [&](TestViewer& viewer, uint32_t paused, uint32_t host, uint32_t background,
                              uint32_t peer = 0) {
        const auto event = lastPause(viewer);
        return event.paused == paused && event.by_host == host && event.by_background == background &&
               event.by_peer == peer;
    };
    const auto ended = [](TestViewer& viewer) {
        size_t count = 0;
        for (const auto& event : viewer.events.of(RCS_EVENT_STREAM)) {
            count += (event.stream.changes & RCS_STREAM_ENDED) != 0;
        }
        return count;
    };
    const auto sounds = [](TestViewer& viewer) {
        return viewer.session->stats<rcs_audio_receive_stats>().received_packets;
    };
    const auto captured = [](rcsdk::Session& share) { return share.stats<rcs_capture_stats>().captured_frames; };
    const auto video = RCS_PAUSE_VIDEO, audio = RCS_PAUSE_AUDIO;

    // The sharer is not paused by the background here (its own part below): only the viewers are.
    auto sharing = config(RCS_ROLE_SHARE, port, key).setViewers(2);
    sharing.background().pause = 0;
    rcsdk::Session share(sharing);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    TestViewer viewer(config(RCS_ROLE_VIEW, port, key));
    CHECK(viewer.session->pause(1u << 2) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(viewer.session->start() == RCS_OK);
    CHECK(viewer.watching());
    CHECK(waitFor([&] { return sounds(viewer) > 50; }));
    CHECK(viewer.events.of(RCS_EVENT_PAUSE).empty()); // Nothing paused, nothing said.

    // Its picture: its stream ends, and with no other viewer the sharer captures no more.
    CHECK(viewer.session->pause(video) == RCS_OK);
    CHECK(pausedAs(viewer, video, video, 0));
    CHECK(waitFor([&] { return ended(viewer) == 1; }));
    std::this_thread::sleep_for(300ms);
    const int frozen = viewer.frames;
    const uint64_t capturedThen = captured(share), heard = sounds(viewer);
    std::this_thread::sleep_for(1s);
    CHECK(viewer.frames == frozen && captured(share) == capturedThen);
    CHECK(sounds(viewer) > heard + 20); // The sound goes on.
    const size_t said = viewer.events.of(RCS_EVENT_PAUSE).size();
    CHECK(viewer.session->pause(video) == RCS_OK && viewer.events.of(RCS_EVENT_PAUSE).size() == said); // No change.
    CHECK(viewer.session->resume(video) == RCS_OK);
    CHECK(pausedAs(viewer, 0, 0, 0));
    CHECK(viewer.watching());
    CHECK(viewer.session->stats<rcs_video_receive_stats>().decoder_recoveries == 0);

    // Its sound; the picture goes on.
    CHECK(viewer.session->pause(audio) == RCS_OK);
    std::this_thread::sleep_for(500ms);
    const uint64_t quiet = sounds(viewer);
    std::this_thread::sleep_for(1s);
    CHECK(sounds(viewer) == quiet);
    CHECK(viewer.watching());
    CHECK(viewer.session->resume(audio) == RCS_OK);
    CHECK(waitFor([&] { return sounds(viewer) > quiet + 50; }));

    // The background pauses the picture (the default), apart from the host's pause.
    rcsdk::seams::setAppBackground(true);
    CHECK(waitFor([&] { return pausedAs(viewer, video, 0, video); }));
    CHECK(waitFor([&] { return ended(viewer) == 2; }));
    CHECK(viewer.session->pause(video) == RCS_OK);
    CHECK(pausedAs(viewer, video, video, video));
    rcsdk::seams::setAppBackground(false); // The host's pause holds it.
    CHECK(pausedAs(viewer, video, video, 0));
    std::this_thread::sleep_for(500ms);
    const int held = viewer.frames;
    std::this_thread::sleep_for(500ms);
    CHECK(viewer.frames == held);
    CHECK(viewer.session->resume(video) == RCS_OK);
    CHECK(viewer.watching());
    // Pausing nothing in the background leaves it to the host; the setting may change meanwhile.
    auto background = rcsdk::defaults<rcs_background_config>();
    background.pause = 0;
    CHECK(viewer.session->update(background) == RCS_OK);
    rcsdk::seams::setAppBackground(true);
    CHECK(viewer.watching());
    CHECK(pausedAs(viewer, 0, 0, 0));
    background.pause = video | audio;
    CHECK(viewer.session->update(background) == RCS_OK);
    CHECK(pausedAs(viewer, video | audio, 0, video | audio));
    background.pause = 1u << 2;
    CHECK(viewer.session->update(background) == RCS_ERROR_INVALID_ARGUMENT);
    rcsdk::seams::setAppBackground(std::nullopt);
    CHECK(pausedAs(viewer, 0, 0, 0));
    CHECK(viewer.watching());
    CHECK(waitFor([&] { return sounds(viewer) > quiet + 200; }));

    // Two viewers: one that starts paused says so first and gets no stream;
    // the other goes on, and the sharer captures for whoever takes the stream.
    TestViewer other(config(RCS_ROLE_VIEW, port, key));
    CHECK(other.session->pause(video) == RCS_OK);
    CHECK(other.events.of(RCS_EVENT_PAUSE).empty()); // Not running: said when it starts.
    CHECK(other.session->start() == RCS_OK);
    CHECK(waitFor([&] { return other.session->state() == RCS_STATE_RUNNING; }));
    CHECK(pausedAs(other, video, video, 0));
    std::this_thread::sleep_for(1s);
    CHECK(other.frames == 0);
    CHECK(viewer.watching());
    CHECK(other.session->resume(video) == RCS_OK);
    CHECK(other.watching());
    CHECK(viewer.session->pause(video) == RCS_OK);
    const uint64_t capturing = captured(share);
    CHECK(other.watching());
    CHECK(captured(share) > capturing);

    // The sharer pauses its picture for every viewer: their streams end, they
    // hear it (by_peer), and it captures no more; its sound goes on until it
    // pauses that too.
    CHECK(viewer.session->resume(video) == RCS_OK);
    CHECK(viewer.watching());
    const size_t endedBefore = ended(other);
    CHECK(share.pause(video) == RCS_OK);
    CHECK(waitFor([&] { return pausedAs(viewer, video, 0, 0, video) && pausedAs(other, video, 0, 0, video); }));
    CHECK(waitFor([&] { return ended(other) > endedBefore; }));
    std::this_thread::sleep_for(300ms);
    const int still = other.frames;
    const uint64_t idle = captured(share), heardBefore = sounds(other);
    std::this_thread::sleep_for(1s);
    CHECK(other.frames == still && captured(share) == idle && sounds(other) > heardBefore + 20);
    CHECK(share.pause(audio) == RCS_OK);
    CHECK(waitFor([&] { return pausedAs(other, video | audio, 0, 0, video | audio); }));
    std::this_thread::sleep_for(500ms);
    const uint64_t silent = sounds(other);
    std::this_thread::sleep_for(1s);
    CHECK(sounds(other) == silent);
    CHECK(share.resume(video | audio) == RCS_OK);
    CHECK(waitFor([&] { return pausedAs(other, 0, 0, 0, 0) && pausedAs(viewer, 0, 0, 0, 0); }));
    CHECK(other.watching() && viewer.watching());
    CHECK(waitFor([&] { return sounds(other) > silent + 50; }));
    // The sharer's own background setting: the same, while the viewers are left alone.
    background.pause = 0;
    CHECK(viewer.session->update(background) == RCS_OK && other.session->update(background) == RCS_OK);
    CHECK(share.update(rcsdk::defaults<rcs_background_config>()) == RCS_OK); // The picture.
    rcsdk::seams::setAppBackground(true);
    CHECK(waitFor([&] { return pausedAs(other, video, 0, 0, video); }));
    rcsdk::seams::setAppBackground(std::nullopt);
    CHECK(waitFor([&] { return pausedAs(other, 0, 0, 0, 0); }));
    CHECK(other.watching());
    CHECK(viewer.session->stop() == RCS_OK);
    CHECK(other.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// Two displays, two streams: each its own picture, display, announcement,
// statistics and decoder; the bitrate shared by weight; the pointer and input
// on the stream they belong to; a viewer that takes one stream stops the
// other, and the sharer's host adding and removing displays while running.
void streams(const uint8_t (&key)[32]) {
    rcsdk::seams::setCaptureFactory(
        [](const rcsdk::platform::CaptureTarget& target) { return std::make_unique<DisplayCapture>(target.displayId); });
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] { return RCS_PERMISSION_GRANTED; };
    overrideInput(std::move(input));
    {
        struct Seen {
            uint64_t frames = 0;
            uint32_t width = 0, height = 0;
            double shade = 0; // The latest frame's mean blue.
        };
        std::mutex mutex;
        std::array<Seen, RCS_MAX_STREAMS> seen;
        EventLog shareEvents, viewEvents;
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        viewCallbacks.onEvent = viewEvents.callback();
        viewCallbacks.onFrame = [&](const rcs_frame& f) {
            uint64_t sum = 0;
            for (uint32_t y = 0; y < f.height; y += 8) {
                for (uint32_t x = 0; x < f.width; x += 8) {
                    sum += f.data[size_t(y) * f.stride_bytes + size_t(x) * 4];
                }
            }
            std::lock_guard lock(mutex);
            auto& s = seen.at(f.stream);
            ++s.frames;
            s.width = f.width;
            s.height = f.height;
            s.shade = double(sum) / ((f.height / 8 + (f.height % 8 != 0)) * (f.width / 8 + (f.width % 8 != 0)));
        };
        const auto framesOf = [&](uint32_t stream) {
            std::lock_guard lock(mutex);
            return seen[stream].frames;
        };
        auto sharing = config(RCS_ROLE_SHARE, 47857, key).setControl(true).setStreams({7}, {3, 1});
        auto viewing = config(RCS_ROLE_VIEW, 47857, key).setCursorMode(RCS_CURSOR_SEPARATE);
        rcsdk::Session share(sharing, shareCallbacks);
        rcsdk::Session view(viewing, viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return framesOf(0) >= 10 && framesOf(1) >= 10; }, 10s));
        {
            std::lock_guard lock(mutex);
            // 640x360 at most (config()): the 16:9 display fills it, the 4:3 one is as high.
            // 640x360 (config()): the 16:9 display fills it, the 4:3 one sits in its middle.
            CHECK(seen[0].width == 640 && seen[0].height == 360 && seen[1].width == 640 && seen[1].height == 360);
            CHECK(seen[0].shade < 80 && seen[1].shade > 90);
        }
        const auto first = view.streamInfo(0), second = view.streamInfo(1);
        CHECK(first.stream == 0 && first.content_width == 640 && first.display_width == 1920);
        CHECK(second.stream == 1 && second.content_width == 480 && second.content_x == 80 &&
              second.display_width == 1024 && second.display_height == 768);
        CHECK(share.streamInfo(1).content_width == 480 && !view.streamInfo(2).codec);
        const auto announced = [](EventLog& log, uint32_t stream) {
            for (const auto& e : log.of(RCS_EVENT_STREAM)) {
                if (e.stream_index == stream && e.stream.info.stream == stream && e.stream.info.codec) {
                    return true;
                }
            }
            return false;
        };
        CHECK(announced(viewEvents, 0) && announced(viewEvents, 1) && announced(shareEvents, 1));
        // Each stream's statistics; the bitrate by weight (3 against 1), together
        // no more than the bitrate (2 Mbit/s, config(), less the sound's): each
        // stream's floor comes out of it.
        CHECK(waitFor([&] {
            const auto zero = share.streamStats<rcs_video_send_stats>(0), one = share.streamStats<rcs_video_send_stats>(1);
            return zero.encoded_frames > 10 && one.encoded_frames > 10 && zero.bitrate_bps > 2 * one.bitrate_bps &&
                   one.bitrate_bps > 0;
        }));
        CHECK(share.streamStats<rcs_video_send_stats>(0).bitrate_bps +
                  share.streamStats<rcs_video_send_stats>(1).bitrate_bps <=
              2'000'000);
        CHECK(view.streamStats<rcs_video_receive_stats>(1).decoded_frames > 0 &&
              view.stats<rcs_video_receive_stats>().decoded_frames ==
                  view.streamStats<rcs_video_receive_stats>(0).decoded_frames);
        CHECK(share.streamStats<rcs_capture_stats>(1).captured_frames > 0);
        rcs_video_send_stats unused = RCS_VIDEO_SEND_STATS_INIT;
        CHECK(rcs_session_get_stream_stats(share.handle(), 4, RCS_STATS_VIDEO_SEND, &unused) ==
              RCS_ERROR_INVALID_ARGUMENT);
        // The pointer is on display 7: stream 1.
        CHECK(waitFor([&] {
            const auto cursors = viewEvents.of(RCS_EVENT_CURSOR);
            return !cursors.empty() && cursors.back().stream_index == 1 && cursors.back().cursor.stream == 1 &&
                   cursors.back().cursor.visible && cursors.back().cursor.x == 30000;
        }));
        // Input on stream 1 reaches the injector as stream 1's.
        CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
        rcs_input_event move = RCS_INPUT_EVENT_INIT;
        move.type = RCS_INPUT_MOVE;
        move.x = 100;
        move.y = 200;
        move.stream = 1;
        CHECK(view.sendInput(move) == RCS_OK);
        CHECK(waitFor([&] {
            std::lock_guard lock(recorder.mutex);
            return !recorder.applied.empty() && recorder.applied.back().stream == 1 &&
                   recorder.applied.back().x == 100;
        }));
        move.stream = 4;
        CHECK(view.sendInput(move) == RCS_ERROR_INVALID_ARGUMENT);

        // The viewer takes stream 1 only: stream 0 ends at both ends.
        auto request = viewing.viewRequest();
        request.streams = 1u << 1;
        CHECK(view.update(request) == RCS_OK);
        const auto ended = [](EventLog& log, uint32_t stream) {
            for (const auto& e : log.of(RCS_EVENT_STREAM)) {
                if (e.stream_index == stream && (e.stream.changes & RCS_STREAM_ENDED)) {
                    return true;
                }
            }
            return false;
        };
        CHECK(waitFor([&] { return ended(viewEvents, 0) && ended(shareEvents, 0); }));
        CHECK(!view.streamInfo(0).codec && !share.streamInfo(0).codec);
        const uint64_t stopped = framesOf(0), going = framesOf(1);
        std::this_thread::sleep_for(500ms);
        CHECK(framesOf(0) <= stopped + 2 && framesOf(1) > going + 5);
        // All again: stream 0 starts over at a keyframe.
        request.streams = 0;
        CHECK(view.update(request) == RCS_OK);
        const uint64_t again = framesOf(0);
        CHECK(waitFor([&] { return framesOf(0) > again + 10; }, 5s));

        // The sharer's host takes display 7 out: stream 1 ends, stream 0 goes on.
        rcs_streams_config none = RCS_STREAMS_CONFIG_INIT;
        CHECK(share.update(none) == RCS_OK);
        CHECK(waitFor([&] { return !view.streamInfo(1).codec; }));
        const uint64_t onlyFirst = framesOf(0);
        CHECK(waitFor([&] { return framesOf(0) > onlyFirst + 10; }));
        // And back: stream 1 starts again.
        rcs_streams_config seven = RCS_STREAMS_CONFIG_INIT;
        seven.displays[0] = 7;
        CHECK(share.update(seven) == RCS_OK);
        const uint64_t back = framesOf(1);
        CHECK(waitFor([&] { return framesOf(1) > back + 10; }, 5s));
        // A display stream 0 shares, named again, or twice: INVALID_ARGUMENT.
        rcs_streams_config twice = RCS_STREAMS_CONFIG_INIT;
        twice.displays[0] = twice.displays[1] = 7;
        CHECK(share.update(twice) == RCS_ERROR_INVALID_ARGUMENT);
        rcs_streams_config gap = RCS_STREAMS_CONFIG_INIT;
        gap.displays[1] = 7;
        CHECK(share.update(gap) == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(!shareEvents.overlapping && !viewEvents.overlapping);
    }
    rcsdk::seams::setInputOverride({});
    rcsdk::seams::setCaptureFactory({});
}

void viewerAudio(const uint8_t (&key)[32]) {
    rcsdk::test::UseTestTone tone;
    rcsdk::test::UseTestMicrophone microphone(500);
    std::mutex mutex;
    std::map<rcs_viewer_id, std::vector<float>> heard; // Left channel.
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    shareCallbacks.onAudio = [&](const rcs_audio_frame& frame) {
        std::lock_guard lock(mutex);
        auto& samples = heard[frame.viewer_id];
        for (uint32_t i = 0; i < frame.frames; ++i) {
            samples.push_back(frame.samples[i * frame.channels]);
        }
    };
    // The host plays them (and so its own machine's sound may go on as it is).
    auto sharing = config(RCS_ROLE_SHARE, 47849, key).setTransport(RCS_TRANSPORT_UDP).setViewers(2)
                       .setReceiveAudio(true).setAudioPlayback(false);
    auto& receive = sharing.receiveAudio();
    rcsdk::Session share(sharing, shareCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    auto pushing = config(RCS_ROLE_VIEW, 47849, key).setTransport(RCS_TRANSPORT_UDP)
                       .setSendAudio(true).setAudioSource(RCS_AUDIO_HOST);
    auto talking = config(RCS_ROLE_VIEW, 47849, key).setTransport(RCS_TRANSPORT_UDP).setMicrophone();
    auto& microphoneSettings = talking.sendAudio();
    TestViewer a(pushing);
    CHECK(a.session->start() == RCS_OK && a.watching());
    TestViewer b(talking);
    CHECK(b.session->start() == RCS_OK && b.watching());
    std::atomic<bool> going{true};
    std::atomic<uint64_t> taken{0}; // Frames A's session took.
    std::thread pusher([&] {
        std::vector<float> block(480 * 2);
        uint64_t produced = 0;
        for (auto next = std::chrono::steady_clock::now(); going; next += 10ms) {
            for (uint32_t i = 0; i < 480; ++i) {
                block[i * 2] = block[i * 2 + 1] =
                    float(0.5 * std::sin(2 * 3.14159265358979 * 750 * double(produced + i) / 48000));
            }
            produced += 480;
            if (a.session->pushAudio(block.data(), 480) == RCS_OK) {
                taken += 480;
            }
            std::this_thread::sleep_until(next + 10ms);
        }
    });
    const auto enough = [&](rcs_viewer_id viewer) {
        std::lock_guard lock(mutex);
        return heard[viewer].size() >= 48000;
    };
    CHECK(waitFor([&] { return enough(1) && enough(2); }, 10s));
    for (const auto& [viewer, hz] : {std::pair<rcs_viewer_id, double>{1, 750}, {2, 500}}) {
        std::vector<float> tail;
        {
            std::lock_guard lock(mutex);
            const auto& samples = heard[viewer];
            tail.assign(samples.end() - 12000, samples.end());
        }
        const auto measured = rcsdk::test::measureTone(tail);
        std::printf("viewer_audio: the sharer hears viewer %u at %.0f Hz, rms %.3f\n", viewer, measured.hz,
                    measured.rms);
        CHECK(std::abs(measured.hz - hz) < 10 && std::abs(measured.rms - 0.354) < 0.05);
        const auto got = share.viewerStats<rcs_audio_receive_stats>(viewer);
        CHECK(got.active && got.codec == RCS_AUDIO_OPUS && got.received_packets > 0);
    }
    const auto audioOf = [](EventLog& log, rcs_viewer_id viewer, rcs_audio_direction direction) {
        auto events = eventsOf(log, RCS_EVENT_AUDIO, viewer);
        std::erase_if(events, [&](const rcs_event& e) { return e.audio.direction != direction; });
        return events;
    };
    for (const rcs_viewer_id viewer : {1u, 2u}) {
        CHECK(!audioOf(shareEvents, viewer, RCS_AUDIO_RECEIVING).empty() &&
              audioOf(shareEvents, viewer, RCS_AUDIO_RECEIVING).back().audio.active);
        CHECK(audioOf(shareEvents, viewer, RCS_AUDIO_SENDING).back().audio.active); // Its own goes on.
    }
    const auto sentByB = audioOf(b.events, 0, RCS_AUDIO_SENDING);
    CHECK(!sentByB.empty() && sentByB.back().audio.active && sentByB.back().audio.voice_processing);
    CHECK(a.session->stats<rcs_audio_send_stats>().encoded_packets > 0);
    CHECK(waitFor([&] { return a.session->stats<rcs_audio_receive_stats>().received_packets > 0; }));
    CHECK(audioOf(a.events, 0, RCS_AUDIO_RECEIVING).back().audio.active);
    // B stops sending: the sharer hears so.
    microphoneSettings.enabled = 0;
    CHECK(b.session->update(microphoneSettings) == RCS_OK);
    CHECK(waitFor([&] {
        const auto events = audioOf(shareEvents, 2, RCS_AUDIO_RECEIVING);
        return !events.back().audio.active && std::string(events.back().audio.message) == "The viewer sends no sound";
    }));
    // The sharer stops receiving: A hears so, and stops.
    receive.enabled = 0;
    CHECK(share.update(receive) == RCS_OK);
    CHECK(waitFor([&] {
        const auto events = audioOf(a.events, 0, RCS_AUDIO_SENDING);
        return !events.back().audio.active && std::string(events.back().audio.message) == "The sharer receives no sound";
    }));
    going = false;
    pusher.join();
    // A viewer's captured sound is counted once, as it was taken.
    CHECK(taken > 0 && a.session->stats<rcs_audio_send_stats>().captured_frames == taken);
}

// Three viewers watch one stream: each joins at a keyframe; data goes to one
// viewer or to all; the host picks the main viewer, disconnects one from a
// callback; the main viewer leaves and the next takes over; stopping with
// several viewers takes one goodbye's time.
void viewers(const uint8_t (&key)[32], rcs_transport transport, uint16_t port) {
    EventLog shareEvents;
    std::mutex mutex;
    std::vector<std::pair<rcs_viewer_id, std::string>> fromViewers;
    rcsdk::Session* sharePointer = nullptr;
    std::atomic<rcs_result> disconnected{RCS_ERROR_INTERNAL};
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    shareCallbacks.onData = [&](const rcs_data_message& message) {
        std::string text(reinterpret_cast<const char*>(message.data), message.size);
        if (text == "leave me") { // From a callback: it returns at once, the viewer leaves after.
            disconnected = sharePointer->disconnectViewer(message.viewer_id);
        }
        std::lock_guard lock(mutex);
        fromViewers.emplace_back(message.viewer_id, std::move(text));
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, port, key).setTransport(transport).setViewers(3), shareCallbacks);
    sharePointer = &share;
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    const auto viewing = [&] {
        return config(RCS_ROLE_VIEW, port, key).setTransport(transport);
    };
    std::vector<std::unique_ptr<TestViewer>> viewers;
    for (int i = 0; i < 3; ++i) {
        const uint64_t keyframes = share.stats<rcs_video_send_stats>().keyframes;
        viewers.push_back(std::make_unique<TestViewer>(viewing()));
        CHECK(viewers.back()->session->start() == RCS_OK);
        CHECK(viewers.back()->watching());
        if (i) { // A keyframe was made at once for the viewer that joined.
            CHECK(share.stats<rcs_video_send_stats>().keyframes > keyframes);
        }
    }
    // One stream: every viewer decodes the same, from a keyframe on.
    const auto stream = share.streamInfo();
    for (auto& viewer : viewers) {
        const auto info = viewer->session->streamInfo();
        CHECK(info.codec == stream.codec && info.width == stream.width && info.height == stream.height);
        CHECK(info.cursor == RCS_CURSOR_IN_VIDEO);
        CHECK(viewer->session->stats<rcs_video_receive_stats>().decoder_recoveries == 0);
    }
    // Ids in the order they joined; the first is the main viewer.
    auto list = share.viewers();
    CHECK(list.size() == 3);
    for (uint32_t i = 0; i < list.size(); ++i) {
        CHECK(list[i].viewer_id == i + 1 && list[i].joined && list[i].is_main == (i == 0) && list[i].port != 0);
        CHECK(list[i].peer.platform == rcsdk::capabilities().platform && list[i].joined_ns > 0);
    }
    CHECK(std::string(list[0].address) == "127.0.0.1");
    const auto joins = shareEvents.of(RCS_EVENT_VIEWER);
    CHECK(joins.size() == 3);
    for (uint32_t i = 0; i < joins.size(); ++i) {
        CHECK(joins[i].viewer_id == i + 1 && joins[i].viewer.viewer_id == i + 1 && joins[i].viewer.joined);
        CHECK(eventsOf(shareEvents, RCS_EVENT_PEER, i + 1).size() == 1);
    }
    const auto states = shareEvents.of(RCS_EVENT_STATE);
    CHECK(std::count_if(states.begin(), states.end(),
                        [](const rcs_event& e) { return e.state.value == RCS_STATE_RUNNING; }) == 1);

    // Data to one viewer, to every viewer, and from one.
    CHECK(share.sendDataTo(2, 7, "two") == RCS_OK);
    CHECK(share.sendData(7, "all") == RCS_OK);
    CHECK(share.sendDataTo(9, 7, "nobody") == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(waitFor([&] {
        return viewers[0]->received().size() == 1 && viewers[1]->received().size() == 2 &&
               viewers[2]->received().size() == 1;
    }));
    CHECK(viewers[1]->received()[0] == "two" && viewers[0]->received()[0] == "all");
    CHECK(viewers[0]->session->sendData(7, "from one") == RCS_OK);
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return fromViewers.size() == 1 && fromViewers[0].first == 1 && fromViewers[0].second == "from one";
    }));
    // Each viewer's statistics, and the run's sums.
    uint64_t sent = 0;
    for (rcs_viewer_id id = 1; id <= 3; ++id) {
        const auto network = share.viewerStats<rcs_network_stats>(id);
        CHECK(network.bytes > 0 && network.connections == 1);
        sent += share.viewerStats<rcs_data_stats>(id).sent_messages;
    }
    CHECK(sent == 4 && share.stats<rcs_data_stats>().sent_messages == 4);
    CHECK(share.viewerStats<rcs_data_stats>(1).received_messages == 1);
    rcs_video_send_stats video = RCS_VIDEO_SEND_STATS_INIT;
    CHECK(rcs_session_get_viewer_stats(share.handle(), 1, RCS_STATS_VIDEO_SEND, &video) == RCS_ERROR_INVALID_ARGUMENT);
    rcs_network_stats network = RCS_NETWORK_STATS_INIT;
    CHECK(rcs_session_get_viewer_stats(share.handle(), 9, RCS_STATS_NETWORK, &network) == RCS_ERROR_INVALID_ARGUMENT);

    // The host chooses the main viewer, and gives the choice back.
    CHECK(share.setMainViewer(2) == RCS_OK);
    list = share.viewers();
    CHECK(!list[0].is_main && list[1].is_main);
    CHECK(share.setMainViewer(9) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(share.setMainViewer(0) == RCS_OK && share.viewers()[0].is_main);

    // A viewer is disconnected from inside a callback; the others go on.
    CHECK(viewers[2]->session->sendData(7, "leave me") == RCS_OK);
    CHECK(waitFor([&] { return viewers[2]->session->state() == RCS_STATE_FAILED; }));
    CHECK(viewers[2]->session->failure().result == RCS_ERROR_DISCONNECTED && disconnected == RCS_OK);
    const auto left = eventsOf(shareEvents, RCS_EVENT_VIEWER, 3);
    CHECK(left.size() == 2 && !left[1].viewer.joined && left[1].viewer.result == RCS_ERROR_DISCONNECTED);
    CHECK(share.disconnectViewer(3) == RCS_ERROR_INVALID_STATE &&
          share.disconnectViewer(9) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(viewers[0]->watching() && viewers[1]->watching());

    // The main viewer leaves: the next takes over, the run goes on.
    CHECK(viewers[0]->session->stop() == RCS_OK);
    CHECK(waitFor([&] {
        list = share.viewers();
        return list.size() == 1 && list[0].viewer_id == 2 && list[0].is_main;
    }));
    CHECK(viewers[1]->watching() && share.state() == RCS_STATE_RUNNING);
    CHECK(eventsOf(shareEvents, RCS_EVENT_VIEWER, 1).back().viewer.result == RCS_ERROR_DISCONNECTED);
    // What viewers that left counted stays the run's.
    CHECK(share.stats<rcs_data_stats>().received_messages == 2 && share.stats<rcs_data_stats>().sent_messages == 4);

    // Stopping with several viewers takes about one goodbye, not one each.
    for (int i = 0; i < 2; ++i) {
        viewers.push_back(std::make_unique<TestViewer>(viewing()));
        CHECK(viewers.back()->session->start() == RCS_OK);
        CHECK(viewers.back()->watching());
    }
    CHECK(share.viewers().size() == 3 && share.viewers()[2].viewer_id == 5); // Ids are never reused.
    const auto stopping = std::chrono::steady_clock::now();
    CHECK(share.stop() == RCS_OK);
    CHECK(std::chrono::steady_clock::now() - stopping < 1500ms);
    for (size_t i = 3; i < viewers.size(); ++i) {
        CHECK(waitFor([&] { return viewers[i]->session->state() == RCS_STATE_FAILED; }));
        CHECK(viewers[i]->session->failure().result == RCS_ERROR_DISCONNECTED);
    }
    CHECK(!shareEvents.overlapping);
}

// Each viewer's own control: the host decides as one joins, then changes one
// viewer's or everyone's; revoking one viewer discards nothing of another's;
// on_input names who sent each event. The host may refuse a viewer.
void viewerControl(const uint8_t (&key)[32]) {
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    {
        EventLog shareEvents;
        std::mutex mutex;
        std::vector<std::pair<rcs_viewer_id, uint32_t>> seen; // Who sent which key.
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        shareCallbacks.onInput = [&](const rcs_input_event& event) {
            std::lock_guard lock(mutex);
            if (event.type == RCS_INPUT_KEY) {
                seen.emplace_back(event.viewer_id, event.code);
            }
            return true;
        };
        shareCallbacks.onViewerJoin = [&](const rcs_viewer_info& viewer, rcs_viewer_decision& decision) {
            CHECK(!viewer.joined && viewer.viewer_id && decision.admission == RCS_ADMIT && decision.control == 1 &&
                  !decision.reason[0]);
            decision.control = viewer.viewer_id == 1;                              // Only the first may control...
            decision.admission = viewer.viewer_id != 3 ? RCS_ADMIT : RCS_REFUSE; // ...and the third may not watch.
        };
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47950, key).setControl(true).setViewers(3), shareCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        TestViewer a(config(RCS_ROLE_VIEW, 47950, key)), b(config(RCS_ROLE_VIEW, 47950, key));
        CHECK(a.session->start() == RCS_OK && a.watching());
        CHECK(b.session->start() == RCS_OK && b.watching());
        CHECK(waitFor([&] { return a.session->controlInfo().granted != 0; }));
        std::this_thread::sleep_for(300ms);
        CHECK(!b.session->controlInfo().granted);
        const auto press = [](uint32_t code) {
            rcs_input_event event = RCS_INPUT_EVENT_INIT;
            event.type = RCS_INPUT_KEY;
            event.code = code;
            event.down = 1;
            return event;
        };
        const auto executed = [&](rcs_viewer_id viewer, uint32_t code) {
            return waitFor([&] {
                std::lock_guard lock(mutex);
                return std::find(seen.begin(), seen.end(), std::pair{viewer, code}) != seen.end();
            });
        };
        CHECK(a.session->sendInput(press(4)) == RCS_OK && executed(1, 4));
        CHECK(b.session->sendInput(press(5)) == RCS_ERROR_INVALID_STATE);
        rcs_input_event named = press(5);
        named.viewer_id = 1; // The sharer names who sent it, never the viewer.
        CHECK(a.session->sendInput(named) == RCS_ERROR_INVALID_ARGUMENT);
        auto list = share.viewers();
        CHECK(list[0].control.allowed && list[0].control.granted && !list[1].control.allowed);
        // One viewer's control: granted to B, then revoked from A, which
        // releases what is held and discards nothing of B's.
        CHECK(share.setViewerControl(2, true) == RCS_OK);
        CHECK(waitFor([&] { return b.session->controlInfo().granted != 0; }));
        CHECK(b.session->sendInput(press(6)) == RCS_OK && executed(2, 6));
        const int releases = recorder.releases;
        CHECK(share.setViewerControl(1, false) == RCS_OK);
        CHECK(waitFor([&] { return !a.session->controlInfo().granted; }) && recorder.releases > releases);
        CHECK(b.session->controlInfo().granted && b.session->sendInput(press(7)) == RCS_OK && executed(2, 7));
        CHECK(share.setViewerControl(9, true) == RCS_ERROR_INVALID_ARGUMENT);
        // CONTROL events name the viewer.
        CHECK(!eventsOf(shareEvents, RCS_EVENT_CONTROL, 2).empty() &&
              eventsOf(shareEvents, RCS_EVENT_CONTROL, 0).empty());
        // The host's setting is everyone's.
        rcs_control_config control = RCS_CONTROL_CONFIG_INIT;
        CHECK(share.update(control) == RCS_OK);
        CHECK(waitFor([&] { return !b.session->controlInfo().granted; }));
        control.allowed = 1;
        CHECK(share.update(control) == RCS_OK);
        CHECK(waitFor([&] { return a.session->controlInfo().granted && b.session->controlInfo().granted; }));
        // The host refuses the third viewer: its run ends, without a reason of the host's.
        TestViewer c(config(RCS_ROLE_VIEW, 47950, key));
        CHECK(c.session->start() == RCS_OK);
        CHECK(waitFor([&] { return c.session->state() == RCS_STATE_FAILED; }));
        CHECK(c.session->failure().result == RCS_ERROR_REFUSED &&
              c.session->failure().message.find("The host refused this viewer") != std::string::npos);
        CHECK(share.viewers().size() == 2 && a.watching());
    }
    rcsdk::seams::setInputOverride({});
}

// A sharer that serves as many viewers as it takes refuses the next with
// BUSY; raised while running it takes more, lowered it disconnects nobody.
// The default takes 64, over any transport. A viewer reconnecting tries
// again while busy, within its window.
void viewersBusy(const uint8_t (&key)[32]) {
    uint16_t port = 47951;
    for (const rcs_transport transport : {RCS_TRANSPORT_TCP, RCS_TRANSPORT_UDP}) {
        ++port;
        rcsdk::Session share(
            config(RCS_ROLE_SHARE, port, key).setTransport(transport).setKeepListening(true).setViewers(1));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        const auto viewing = config(RCS_ROLE_VIEW, port, key).setTransport(transport).setReconnect(true);
        TestViewer a(viewing), b(viewing);
        CHECK(a.session->start() == RCS_OK && a.watching());
        CHECK(b.session->start() == RCS_OK);
        CHECK(waitFor([&] { return b.session->state() == RCS_STATE_FAILED; }));
        CHECK(b.session->failure().result == RCS_ERROR_BUSY &&
              b.session->failure().message.find("as many viewers") != std::string::npos);
        CHECK(a.watching(5) && share.viewers().size() == 1);
        auto viewers = rcsdk::defaults<rcs_viewers_config>();
        viewers.max_viewers = 2;
        CHECK(share.update(viewers) == RCS_OK);
        CHECK(b.session->start() == RCS_OK && b.watching());
        viewers.max_viewers = 1; // Below the viewers there are: nobody leaves, nobody more joins.
        CHECK(share.update(viewers) == RCS_OK);
        TestViewer c(viewing);
        CHECK(c.session->start() == RCS_OK);
        CHECK(waitFor([&] { return c.session->state() == RCS_STATE_FAILED; }));
        CHECK(c.session->failure().result == RCS_ERROR_BUSY && share.viewers().size() == 2);
        CHECK(a.watching(5) && b.watching(5));
        viewers.max_viewers = RCS_MAX_VIEWERS + 1;
        CHECK(share.update(viewers) == RCS_ERROR_INVALID_ARGUMENT);
        // Back to the default: 64.
        viewers.max_viewers = 0;
        CHECK(share.update(viewers) == RCS_OK);
        TestViewer d(viewing);
        CHECK(d.session->start() == RCS_OK && d.watching() && share.viewers().size() == 3);
    }
    // Before any stream runs, viewers that join side by side must share one:
    // the first waits for the host and decodes HEVC only; one that decodes
    // H.264 only is turned away instead of breaking the stream for both, one
    // that decodes either gets in.
    const auto caps = rcsdk::capabilities();
    const uint32_t both = testAcceleration() == RCS_ACCELERATION_HARDWARE
                              ? caps.hardware_encoders & caps.hardware_decoders
                              : caps.software_encoders & caps.software_decoders;
    if (both & hevcBit) {
        rcsdk::Callbacks callbacks;
        callbacks.onViewerJoin = [](const rcs_viewer_info& info, rcs_viewer_decision& decision) {
            decision.admission = info.viewer_id == 1 ? RCS_ADMIT_LATER : RCS_ADMIT;
        };
        rcsdk::Session share(
            config(RCS_ROLE_SHARE, 47942, key).setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264}).setViewers(3), callbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        TestViewer hevc(config(RCS_ROLE_VIEW, 47942, key).setCodecs({RCS_CODEC_HEVC}));
        CHECK(hevc.session->start() == RCS_OK);
        CHECK(waitFor([&] { return !hevc.events.of(RCS_EVENT_ADMISSION).empty(); }));
        TestViewer h264(config(RCS_ROLE_VIEW, 47942, key).setCodecs({RCS_CODEC_H264}));
        CHECK(h264.session->start() == RCS_OK);
        CHECK(waitFor([&] { return h264.session->state() == RCS_STATE_FAILED; }));
        CHECK(h264.session->failure().result == RCS_ERROR_BUSY &&
              h264.session->failure().message.find("share no stream") != std::string::npos);
        TestViewer either(config(RCS_ROLE_VIEW, 47942, key).setCodecs({RCS_CODEC_H264, RCS_CODEC_HEVC}));
        CHECK(either.session->start() == RCS_OK && either.watching());
        rcs_viewer_decision decision = RCS_VIEWER_DECISION_INIT;
        CHECK(share.admitViewer(1, decision) == RCS_OK && hevc.watching() && either.watching(5));
        CHECK(share.viewers().size() == 2);
    } else {
        std::printf("HEVC is not available with this acceleration; viewers joining side by side skipped\n");
    }

    // HOST carries several viewers as the others do (host_viewers runs them).
    rcsdk::test::FunctionTransport sink([](uint64_t, const uint8_t*, uint32_t) { return true; });
    auto many = config(RCS_ROLE_SHARE, 47958, key).setViewers(2);
    many.setTransport(RCS_TRANSPORT_HOST).use(rcsdk::hostTransport(sink, RCS_HOST_CHANNEL_DATAGRAM));
    rcsdk::Session hosted(many, {});
    CHECK(hosted.start() == RCS_OK && hosted.stop() == RCS_OK);

    // Busy while reconnecting: tried again. The viewer's path breaks; while its
    // connection is kept, another viewer takes the only place, which the kept
    // one gives up (it leaves, NETWORK). Once its path is back the first hears
    // that the sharer no longer knows its connection, and connects anew: BUSY
    // until that one leaves, then it gets back in.
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47957, key).setKeepListening(true).setViewers(1), shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47957));
    TestViewer a(reconnecting(config(RCS_ROLE_VIEW, proxy.address().port(), key), 30000, 300));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(a.session->start() == RCS_OK && a.watching());
    proxy.outage = true;
    CHECK(waitFor([&] { return a.session->state() == RCS_STATE_RECONNECTING; }, 8s));
    CHECK(share.state() == RCS_STATE_RUNNING && share.viewers().size() == 1); // Kept.
    TestViewer b(config(RCS_ROLE_VIEW, 47957, key));
    CHECK(b.session->start() == RCS_OK && b.watching());
    CHECK(waitFor([&] {
        const auto viewers = shareEvents.of(RCS_EVENT_VIEWER);
        return std::any_of(viewers.begin(), viewers.end(), [](const rcs_event& e) {
            return e.viewer_id == 1 && !e.viewer.joined && e.viewer.result == RCS_ERROR_NETWORK &&
                   std::string(e.viewer.message).find("took its place") != std::string::npos;
        });
    }));
    proxy.outage = false;
    std::this_thread::sleep_for(2s); // Its attempts get BUSY meanwhile.
    CHECK(a.session->state() == RCS_STATE_RECONNECTING && a.session->failure().result == RCS_OK);
    const auto states = a.events.of(RCS_EVENT_STATE);
    CHECK(states.back().state.value == RCS_STATE_RECONNECTING);
    CHECK(b.session->stop() == RCS_OK);
    CHECK(waitFor([&] { return a.session->state() == RCS_STATE_RUNNING; }, 15s));
    CHECK(a.watching());
}

// Several viewers over P2P or RELAY: the host routes signals by its own
// numbers. Each viewer's signal is answered on its route with an ICE exchange
// of its own (over RELAY an allocation of its own on each end), side by side;
// the viewers join as over UDP, and the host knows each by its route. One
// more than max_viewers is BUSY; a viewer whose attempt is on its way when
// the last one leaves gets in all the same; viewers of a sharer that restarts
// reconnect through new signals.
void viewersSignalled(const uint8_t (&key)[32], rcs_transport transport) {
    std::optional<rcsdk::test::TurnServer> turn;
    if (transport == RCS_TRANSPORT_RELAY) {
        turn.emplace("host", "s3cret");
    }
    const auto configured = [&](rcs_role role) {
        auto c = config(role, 0, key);
        if (turn) {
            c.setRelay(turn->addressText(), "host", "s3cret");
        } else {
            c.setTransport(RCS_TRANSPORT_P2P);
        }
        return c;
    };
    // The host's channel: each route leads to a viewer; an answer may be held back.
    std::mutex mutex;
    std::map<uint64_t, rcsdk::Session*> routes;
    std::set<uint64_t> holding;
    std::map<uint64_t, std::string> held;
    std::vector<uint64_t> asked; // Routes on_viewer_join saw.
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    shareCallbacks.onSignal = [&](const std::string& text, uint64_t route) {
        std::lock_guard lock(mutex);
        if (holding.count(route)) {
            held[route] = text;
        } else if (const auto found = routes.find(route); found != routes.end()) {
            CHECK(found->second->signal(text) == RCS_OK);
        }
    };
    shareCallbacks.onViewerJoin = [&](const rcs_viewer_info& viewer, rcs_viewer_decision& decision) {
        std::lock_guard lock(mutex);
        asked.push_back(viewer.route);
        decision.control = viewer.route == 101; // The host knows who it is: only 101 controls.
    };
    rcsdk::Session share(configured(RCS_ROLE_SHARE).setKeepListening(true).setViewers(3), shareCallbacks);
    struct Watcher {
        std::atomic<int> frames{0};
        std::unique_ptr<rcsdk::Session> session;
    };
    std::map<uint64_t, std::unique_ptr<Watcher>> viewers;
    const auto add = [&](uint64_t route, rcsdk::SessionConfig c) -> Watcher& {
        auto watcher = std::make_unique<Watcher>();
        rcsdk::Callbacks callbacks;
        callbacks.onSignal = [&share, route](const std::string& text, uint64_t) {
            CHECK(share.signal(text, route) == RCS_OK);
        };
        callbacks.onFrame = [w = watcher.get()](const rcs_frame&) { ++w->frames; };
        watcher->session = std::make_unique<rcsdk::Session>(c, callbacks);
        std::lock_guard lock(mutex);
        routes[route] = watcher->session.get();
        return *(viewers[route] = std::move(watcher));
    };
    const auto watching = [](Watcher& watcher) {
        const int before = watcher.frames;
        return waitFor([&] { return watcher.frames >= before + 10; }, 15s);
    };
    const auto remove = [&](uint64_t route) {
        CHECK(viewers.at(route)->session->stop() == RCS_OK);
        {
            std::lock_guard lock(mutex);
            routes.erase(route);
        }
        viewers.erase(route);
    };
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));

    // Three at once: their attempts run side by side.
    for (const uint64_t route : {101u, 102u, 103u}) {
        add(route, configured(RCS_ROLE_VIEW));
    }
    for (auto& [route, viewer] : viewers) {
        CHECK(viewer->session->start() == RCS_OK);
    }
    for (auto& [route, viewer] : viewers) {
        CHECK(watching(*viewer));
    }
    auto list = share.viewers();
    CHECK(list.size() == 3);
    std::set<uint64_t> joined;
    std::set<std::string> paths;
    for (const auto& viewer : list) {
        joined.insert(viewer.route);
        CHECK(viewer.control.allowed == (viewer.route == 101));
        paths.insert(share.viewerStats<rcs_network_stats>(viewer.viewer_id).path);
    }
    CHECK(joined == (std::set<uint64_t>{101, 102, 103}));
    CHECK(paths.size() == 3); // Each over a path of its own.
    {
        std::lock_guard lock(mutex);
        CHECK(asked.size() == 3 && std::set<uint64_t>(asked.begin(), asked.end()) == joined);
    }
    for (const auto& event : shareEvents.of(RCS_EVENT_VIEWER)) {
        CHECK(event.viewer.route == 101 || event.viewer.route == 102 || event.viewer.route == 103);
    }
    if (turn) {
        CHECK(turn->allocations == 6); // Each viewer's own, and the sharer's for each.
    }

    // One more than the sharer takes: BUSY, the others watch on.
    auto& extra = add(104, configured(RCS_ROLE_VIEW));
    CHECK(extra.session->start() == RCS_OK);
    CHECK(waitFor([&] { return extra.session->state() == RCS_STATE_FAILED; }, 15s));
    CHECK(extra.session->failure().result == RCS_ERROR_BUSY &&
          extra.session->failure().message.find("as many viewers") != std::string::npos);
    remove(104);
    // One leaves; another takes its place while the rest watch.
    remove(102);
    CHECK(waitFor([&] { return share.viewers().size() == 2; }, 8s));
    auto& late = add(105, configured(RCS_ROLE_VIEW));
    CHECK(late.session->start() == RCS_OK && watching(late));
    CHECK(share.viewers().size() == 3 && watching(*viewers.at(101)));

    // The last viewers leave while one's attempt is on its way (its answer
    // held back): it still gets in, through the next listening.
    {
        std::lock_guard lock(mutex);
        holding.insert(106);
    }
    auto& coming = add(106, configured(RCS_ROLE_VIEW));
    CHECK(coming.session->start() == RCS_OK);
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return held.count(106) == 1;
    }));
    const size_t listened = shareEvents.of(RCS_EVENT_STATE).size();
    for (const uint64_t route : {101u, 103u, 105u}) {
        remove(route);
    }
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 8s));
    CHECK(shareEvents.of(RCS_EVENT_STATE).size() > listened);
    {
        std::lock_guard lock(mutex);
        holding.erase(106);
        CHECK(coming.session->signal(held[106]) == RCS_OK);
    }
    CHECK(watching(coming));
    list = share.viewers();
    CHECK(list.size() == 1 && list[0].route == 106);
    remove(106);

    // The sharer restarts: its viewers reconnect, each with a new signal that
    // waits for the sharer's next run.
    for (const uint64_t route : {201u, 202u}) {
        auto& viewer = add(route, reconnecting(configured(RCS_ROLE_VIEW), 30'000, 300));
        CHECK(viewer.session->start() == RCS_OK && watching(viewer));
    }
    CHECK(share.stop(RCS_STOP_RESTART) == RCS_OK);
    for (auto& [route, viewer] : viewers) {
        CHECK(waitFor([&] { return viewer->session->state() == RCS_STATE_RECONNECTING; }, 8s));
    }
    std::this_thread::sleep_for(500ms); // Their new signals wait for the next run.
    CHECK(share.start() == RCS_OK);
    for (auto& [route, viewer] : viewers) {
        CHECK(waitFor([&] { return viewer->session->state() == RCS_STATE_RUNNING; }, 15s) && watching(*viewer));
    }
    CHECK(share.viewers().size() == 2);
    for (const uint64_t route : {201u, 202u}) {
        remove(route);
    }
    CHECK(share.stop() == RCS_OK);
    CHECK(!shareEvents.overlapping);
}

// A slow viewer: under MINIMUM its path sets the bitrate for all (RATE names
// it); under MAIN the main viewer's does, and the slow one skips to
// keyframes. Either way the others' pictures keep coming.
void viewersRate(const uint8_t (&key)[32]) {
    rcsdk::test::patternControl().grain = true; // Content that needs the bitrate.
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    std::atomic<int> reportsOfSlow{0};
    RateCallback decider([&](const rcs_rate_report& report, rcs_rate_decision&) {
        reportsOfSlow += report.viewer_id == 2;
    });
    rcsdk::Session share(
        config(RCS_ROLE_SHARE, 47959, key)
            .setVideo(640, 360, 30, 6'000'000)
            .setViewers(2, RCS_BITRATE_FROM_MINIMUM)
            .use(rcsdk::rateDecider(decider)),
        shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47959));
    proxy.bitsPerSecond = 1'500'000;
    TestViewer fast(config(RCS_ROLE_VIEW, 47959, key)), slow(config(RCS_ROLE_VIEW, proxy.address().port(), key));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(fast.session->start() == RCS_OK && fast.watching());
    CHECK(slow.session->start() == RCS_OK && slow.watching(5));
    const auto bitrate = [&] {
        return share.stats<rcs_video_send_stats>().bitrate_bps;
    };
    CHECK(waitFor([&] { return bitrate() < 1'500'000; }, 15s));
    const auto rates = eventsOf(shareEvents, RCS_EVENT_RATE, 2);
    CHECK(!rates.empty() && reportsOfSlow > 0);
    CHECK(fast.watching());
    const uint32_t squeezed = bitrate();
    // The main viewer's path from now on: the bitrate rises, the fast viewer
    // gets it, the slow one is not waited for.
    auto viewers = rcsdk::defaults<rcs_viewers_config>();
    viewers.max_viewers = 2;
    viewers.bitrate_from = RCS_BITRATE_FROM_MAIN;
    CHECK(share.update(viewers) == RCS_OK);
    CHECK(waitFor([&] { return bitrate() > squeezed * 3 / 2; }, 20s));
    for (int i = 0; i < 3; ++i) {
        const int before = fast.frames;
        std::this_thread::sleep_for(1s);
        CHECK(fast.frames - before >= 15); // 30 FPS, held up by no one.
    }
    CHECK(slow.session->state() == RCS_STATE_RUNNING);
    std::printf("viewers rate: %u kbit/s with the slow viewer's path, %u kbit/s with the main's\n", squeezed / 1000,
                bitrate() / 1000);
    rcsdk::test::patternControl().grain = false;
}

// ---- Group sharing (rcs_viewers_config, RCS_EXT_ENCODINGS) --------------------------------------

// A viewer that counts its pictures, their size, and the encoding it gets.
struct GroupMember {
    EventLog events;
    std::atomic<int> frames{0};
    std::atomic<uint32_t> width{0}, height{0};
    std::unique_ptr<rcsdk::Session> session;

    explicit GroupMember(const rcsdk::SessionConfig& config) {
        rcsdk::Callbacks callbacks;
        callbacks.onEvent = events.callback();
        callbacks.onFrame = [this](const rcs_frame& frame) {
            width = frame.width;
            height = frame.height;
            ++frames;
        };
        session = std::make_unique<rcsdk::Session>(config, callbacks);
    }
    bool watching(int count = 10, std::chrono::milliseconds within = 10s) {
        const int before = frames;
        return waitFor([&] { return frames >= before + count; }, within);
    }
    // Pictures a second, over `over`.
    double rate(std::chrono::milliseconds over = 2s) {
        const int before = frames;
        std::this_thread::sleep_for(over);
        return (frames - before) * 1000.0 / double(over.count());
    }
    uint32_t encoding() const { return session->streamInfo().encoding; }
};

// The viewers' moves the sharer reported, as (viewer, encoding, reason).
std::vector<std::tuple<rcs_viewer_id, uint32_t, rcs_viewer_video_reason>> movesOf(EventLog& log) {
    std::vector<std::tuple<rcs_viewer_id, uint32_t, rcs_viewer_video_reason>> moves;
    for (const auto& event : log.of(RCS_EVENT_VIEWER_VIDEO)) {
        moves.emplace_back(event.viewer_id, event.viewer_video.encoding, event.viewer_video.reason);
    }
    return moves;
}

// Ten viewers join within a second while one watches: their keyframes are
// merged (keyframe_min_interval_ms), not one each for everyone, and each sees
// its picture soon.
void viewersJoinStorm(const uint8_t (&key)[32]) {
    auto sharing = config(RCS_ROLE_SHARE, 47991, key).setViewers(12);
    sharing.extension<rcs_video_tuning_config>().keyframe_min_interval_ms = 1000;
    rcsdk::Session share(sharing);
    GroupMember first(config(RCS_ROLE_VIEW, 47991, key));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(first.session->start() == RCS_OK && first.watching());
    const uint64_t before = share.stats<rcs_video_send_stats>().keyframes;
    std::vector<std::unique_ptr<GroupMember>> storm;
    const auto began = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) {
        storm.push_back(std::make_unique<GroupMember>(config(RCS_ROLE_VIEW, 47991, key)));
        CHECK(storm.back()->session->start() == RCS_OK);
        std::this_thread::sleep_for(100ms);
    }
    for (auto& viewer : storm) {
        CHECK(waitFor([&] { return viewer->frames > 0; }, 5s));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const uint64_t keyframes = share.stats<rcs_video_send_stats>().keyframes - before;
    const auto group = share.stats<rcs_group_stats>();
    std::printf("join storm: %llu keyframes for 10 viewers joining over %.1f s (%llu for joins)\n",
                static_cast<unsigned long long>(keyframes), seconds,
                static_cast<unsigned long long>(group.keyframes_joined));
    CHECK(keyframes <= 2 + uint64_t(seconds)); // One a second at most, not ten.
    CHECK(group.viewers == 11 && group.viewers_on[0] == 11 && group.encodings == 1);
    CHECK(first.watching() && seconds < 3 + 1); // Each saw its picture within 3 s of the storm's end.
    for (auto& viewer : storm) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(first.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// One viewer keeps asking for keyframes (a lossy path whose pictures cannot
// be repaired): while the others watch its encoding it causes one every
// viewer_keyframe_interval_ms at most, its other requests wait, and after
// loss_keyframes_move of them it gets the lower encoding, where its
// keyframes are its own; the others' keyframes stop.
void viewersLossy(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    auto sharing = config(RCS_ROLE_SHARE, 47992, key).setViewers(3);
    auto& tuning = sharing.extension<rcs_video_tuning_config>();
    tuning.fec_mode = RCS_FEC_OFF;
    tuning.ref_invalidation = 0;
    tuning.move_up_ms = 120'000; // It stays down for the test.
    rcsdk::Session share(sharing, shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47992));
    proxy.lossPermille = 50;
    GroupMember a(config(RCS_ROLE_VIEW, 47992, key)), b(config(RCS_ROLE_VIEW, 47992, key)),
        lossy(config(RCS_ROLE_VIEW, proxy.address().port(), key));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    for (auto* viewer : {&a, &b, &lossy}) {
        CHECK(viewer->session->start() == RCS_OK && viewer->watching(5));
    }
    const auto viewers = share.viewers();
    CHECK(viewers.size() == 3);
    const rcs_viewer_id lossyId = viewers[2].viewer_id;
    std::atomic<bool> asking{true};
    std::thread asker([&] {
        while (asking) {
            lossy.session->requestKeyframe();
            std::this_thread::sleep_for(200ms);
        }
    });
    const auto began = std::chrono::steady_clock::now();
    CHECK(waitFor([&] { return lossy.encoding() == 1; }, 15s));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const auto mine = share.viewerStats<rcs_viewer_video_stats>(lossyId);
    std::printf("lossy viewer: moved after %.1f s, %llu keyframes caused, %llu requests waited\n", seconds,
                static_cast<unsigned long long>(mine.keyframes_caused),
                static_cast<unsigned long long>(mine.keyframes_waited));
    CHECK(mine.keyframes_caused == 3 && mine.keyframes_waited > 0 && mine.encoding == 1 && mine.moves_down == 1);
    CHECK(seconds >= 3.5); // 3 keyframes, 2 s apart.
    const auto moves = movesOf(shareEvents);
    CHECK(std::count(moves.begin(), moves.end(), std::tuple{lossyId, 1u, rcs_viewer_video_reason(RCS_VIEWER_VIDEO_LOSS)}) ==
          1);
    // From now on its requests cost the main encoding nothing.
    std::this_thread::sleep_for(500ms);
    const uint64_t mainKeyframes = share.stats<rcs_video_send_stats>().keyframes;
    std::this_thread::sleep_for(3s);
    CHECK(share.stats<rcs_video_send_stats>().keyframes - mainKeyframes <= 1);
    CHECK(share.encodingStats(0, 1).keyframes >= 3 && lossy.watching(5));
    CHECK(a.watching() && b.watching());
    asking = false;
    asker.join();
    const auto group = share.stats<rcs_group_stats>();
    CHECK(group.viewers_on[0] == 2 && group.viewers_on[1] == 1 && group.encodings == 2 && group.moves_down == 1);
    for (auto* viewer : {&a, &b, &lossy}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
}

// QUORUM, the default: of three viewers one's path carries 1.5 Mbit/s. It
// gets the lower encoding (half the frame rate, at its own bitrate) and the
// others keep theirs, instead of all following the slowest; once its path
// clears it comes back, and the lower encoding stops when no one uses it.
void viewersQuorum(const uint8_t (&key)[32]) {
    rcsdk::test::patternControl().grain = true; // Content that needs the bitrate.
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    auto sharing = config(RCS_ROLE_SHARE, 47993, key).setVideo(640, 360, 30, 6'000'000).setViewers(3);
    auto& tuning = sharing.extension<rcs_video_tuning_config>();
    tuning.move_up_ms = 3000;
    tuning.lower_idle_ms = 2000;
    tuning.increase_percent = tuning.recovery_increase_percent = 200; // The cleared path is found fast.
    rcsdk::Session share(sharing, shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47993));
    proxy.bitsPerSecond = 1'500'000;
    GroupMember fast(config(RCS_ROLE_VIEW, 47993, key)), other(config(RCS_ROLE_VIEW, 47993, key)),
        slow(config(RCS_ROLE_VIEW, proxy.address().port(), key));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    for (auto* viewer : {&fast, &other, &slow}) {
        CHECK(viewer->session->start() == RCS_OK && viewer->watching(5));
    }
    const rcs_viewer_id slowId = share.viewers()[2].viewer_id;
    CHECK(waitFor([&] { return slow.encoding() == 1; }, 20s));
    const auto moved = movesOf(shareEvents);
    CHECK((!moved.empty() && moved.back() == std::tuple{slowId, 1u, rcs_viewer_video_reason(RCS_VIEWER_VIDEO_PATH)}));
    CHECK(slow.session->streamInfo().frames_per_second == 15 && slow.session->streamInfo().encodings == 2);
    const auto started = shareEvents.of(RCS_EVENT_ENCODING);
    CHECK(!started.empty() && started.front().encoding.running && started.front().encoding.encoding == 1 &&
          started.front().encoding.frames_per_second == 15);
    // The others are not held to its path.
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().bitrate_bps > 3'000'000; }, 15s));
    const double fastRate = fast.rate(), slowRate = slow.rate();
    std::printf("quorum: main %u kbit/s, the others %.0f FPS; the slow one %.0f FPS on the lower encoding at %u "
                "kbit/s\n",
                share.stats<rcs_video_send_stats>().bitrate_bps / 1000, fastRate, slowRate,
                share.encodingStats(0, 1).bitrate_bps / 1000);
    CHECK(fastRate >= 20 && other.watching(20) && slowRate > 0);
    CHECK(share.encodingStats(0, 1).bitrate_bps < 1'500'000);
    // Its path clears: back on the main encoding; the lower one stops.
    proxy.bitsPerSecond = 0;
    CHECK(waitFor([&] { return slow.encoding() == 0; }, 40s));
    CHECK((movesOf(shareEvents).back() == std::tuple{slowId, 0u, rcs_viewer_video_reason(RCS_VIEWER_VIDEO_PATH)}));
    CHECK(slow.watching(10));
    CHECK(waitFor([&] { return share.stats<rcs_group_stats>().encodings == 1; }, 10s));
    const auto stopped = shareEvents.of(RCS_EVENT_ENCODING);
    CHECK(!stopped.back().encoding.running && stopped.back().encoding.result == RCS_OK);
    rcs_video_send_stats gone = RCS_VIDEO_SEND_STATS_INIT;
    CHECK(rcs_session_get_encoding_stats(share.handle(), 0, 1, &gone) == RCS_ERROR_INVALID_STATE);
    CHECK(waitFor([&] { return slow.session->streamInfo().encodings == 1; }));
    const auto group = share.stats<rcs_group_stats>();
    CHECK(group.moves_down >= 1 && group.moves_up >= 1);
    for (auto* viewer : {&fast, &other, &slow}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
    rcsdk::test::patternControl().grain = false;
}

// A viewer that decodes only H.264 joins viewers of HEVC: it gets a lower
// encoding in H.264 instead of being refused; one that decodes nothing this
// end offers still is (BUSY).
void viewersCodecs(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47994, key).setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264}).setViewers(3),
                         shareCallbacks);
    GroupMember both(config(RCS_ROLE_VIEW, 47994, key).setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264}));
    GroupMember h264(config(RCS_ROLE_VIEW, 47994, key));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(both.session->start() == RCS_OK && both.watching());
    CHECK(both.session->streamInfo().codec == RCS_CODEC_HEVC);
    CHECK(h264.session->start() == RCS_OK && h264.watching());
    const auto info = h264.session->streamInfo();
    CHECK(info.codec == RCS_CODEC_H264 && info.encoding == 1 && info.encodings == 2 && info.width == 640);
    CHECK(both.session->streamInfo().codec == RCS_CODEC_HEVC && both.watching());
    const rcs_viewer_id id = share.viewers()[1].viewer_id;
    CHECK(waitFor([&] { return !movesOf(shareEvents).empty(); }));
    CHECK((movesOf(shareEvents).front() == std::tuple{id, 1u, rcs_viewer_video_reason(RCS_VIEWER_VIDEO_DECODER)}));
    const auto encodings = shareEvents.of(RCS_EVENT_ENCODING);
    CHECK(!encodings.empty() && encodings.front().encoding.codec == RCS_CODEC_H264);
    CHECK(share.streamInfo().codec == RCS_CODEC_HEVC);
    // VP8 alone: nothing this end offers.
    auto vp8 = config(RCS_ROLE_VIEW, 47994, key);
    vp8.setCodecs({RCS_CODEC_VP8});
    vp8.setAcceleration(RCS_ACCELERATION_SOFTWARE);
    GroupMember none(vp8);
    CHECK(none.session->start() == RCS_OK);
    CHECK(waitFor([&] { return none.session->state() == RCS_STATE_FAILED; }, 10s));
    CHECK(none.session->failure().result == RCS_ERROR_BUSY);
    CHECK(both.watching() && h264.watching());
    for (auto* viewer : {&both, &h264}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
}

// FIXED encodings, a lower one made smaller (max_pixel_rate) in a codec of
// its own (VP8: a codec library's encoder takes the scaled pictures), the
// host's placing (on_viewer_join's decision, rcs_session_set_viewer_video),
// its own statistics, and the encoded packets the host gets (the main
// encoding's unless it asks for more). GPU pictures where the system has
// them, so that they are scaled on the GPU.
void viewersEncodings(const uint8_t (&key)[32]) {
    std::optional<rcsdk::test::UseSurfacePattern> surfaces;
    if (rcsdk::test::SurfacePatternCapture::available()) {
        surfaces.emplace();
    }
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    std::atomic<int> joined{0};
    shareCallbacks.onViewerJoin = [&](const rcs_viewer_info&, rcs_viewer_decision& decision) {
        CHECK(decision.video.struct_size == sizeof(decision.video) && decision.video.encoding == RCS_ENCODING_ANY);
        if (joined++ == 1) {
            decision.video.encoding = 1; // The second starts on the lower encoding.
        }
    };
    std::array<std::atomic<int>, RCS_MAX_ENCODINGS> packets{};
    rcs_encoded_output_config output = RCS_ENCODED_OUTPUT_CONFIG_INIT;
    output.user_data = &packets;
    output.on_video = [](const rcs_encoded_packet* packet, void* data) {
        ++(*static_cast<std::array<std::atomic<int>, RCS_MAX_ENCODINGS>*>(data))[packet->encoding];
    };
    const auto both = [](rcsdk::SessionConfig c) {
        c.setCodecs({RCS_CODEC_H264, RCS_CODEC_VP8}).setAcceleration(RCS_ACCELERATION_HARDWARE_FIRST);
        return c;
    };
    auto sharing = both(config(RCS_ROLE_SHARE, 47995, key));
    sharing.setViewers(2).use(output);
    auto& encodings = sharing.encodings();
    encodings.mode = RCS_ENCODINGS_FIXED;
    encodings.lower[0].max_pixel_rate = 640 * 360 * 15 / 2; // Half of each side.
    encodings.lower[0].codecs[0] = RCS_CODEC_VP8;
    rcsdk::Session share(sharing, shareCallbacks);
    GroupMember main(both(config(RCS_ROLE_VIEW, 47995, key))), lower(both(config(RCS_ROLE_VIEW, 47995, key)));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(main.session->start() == RCS_OK && main.watching());
    CHECK(waitFor([&] { return share.stats<rcs_group_stats>().encodings == 2; })); // FIXED: no one needs it yet.
    CHECK(lower.session->start() == RCS_OK && lower.watching());
    CHECK(lower.width == 320 && lower.height == 180 && main.width == 640 && main.height == 360);
    const auto info = lower.session->streamInfo();
    CHECK(info.encoding == 1 && info.width == 320 && info.frames_per_second == 15 && info.codec == RCS_CODEC_VP8);
    CHECK(main.session->streamInfo().codec == RCS_CODEC_H264);
    const auto started = shareEvents.of(RCS_EVENT_ENCODING);
    CHECK(!started.empty() && started.front().encoding.codec == RCS_CODEC_VP8 && started.front().encoding.width == 320);
    const rcs_viewer_id id = share.viewers()[1].viewer_id;
    CHECK((movesOf(shareEvents).front() == std::tuple{id, 1u, rcs_viewer_video_reason(RCS_VIEWER_VIDEO_JOINED)}));
    const auto encoder = share.encodingStats(0, 1);
    CHECK(encoder.struct_size == sizeof(encoder) && encoder.encoded_frames > 0 && encoder.encoder[0]);
    rcs_video_send_stats none = RCS_VIDEO_SEND_STATS_INIT;
    CHECK(rcs_session_get_encoding_stats(share.handle(), 0, 2, &none) == RCS_ERROR_INVALID_STATE);
    CHECK(rcs_session_get_encoding_stats(share.handle(), 0, RCS_MAX_ENCODINGS, &none) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(packets[0] > 0 && packets[1] == 0); // The main encoding's alone.
    // The host moves it up, and asks that it hold no one back.
    rcs_viewer_video video = RCS_VIEWER_VIDEO_INIT;
    video.encoding = 0;
    video.counts_for_bitrate = 0;
    CHECK(share.setViewerVideo(id, video) == RCS_OK);
    CHECK(waitFor([&] { return lower.width == 640; }) && lower.session->streamInfo().codec == RCS_CODEC_H264);
    CHECK((movesOf(shareEvents).back() == std::tuple{id, 0u, rcs_viewer_video_reason(RCS_VIEWER_VIDEO_HOST)}));
    const auto mine = share.viewerStats<rcs_viewer_video_stats>(id);
    CHECK(mine.encoding == 0 && !mine.counts_for_bitrate && mine.moves_up == 1 && mine.frames_sent > 0);
    video.encoding = 2; // Beyond count.
    CHECK(share.setViewerVideo(id, video) == RCS_ERROR_INVALID_ARGUMENT);
    video.encoding = RCS_ENCODING_ANY;
    video.counts_for_bitrate = 2;
    CHECK(share.setViewerVideo(id, video) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(share.setViewerVideo(99, RCS_VIEWER_VIDEO_INIT) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(main.watching() && lower.watching());
    for (auto* viewer : {&main, &lower}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
}

// Temporal layers (RCS_EXT_ENCODINGS scalability L1T2) on one encoding
// (SINGLE). The host gives a viewer layer 0 alone (rcs_viewer_video.layers):
// at once, half the frame rate, without a keyframe or a recovery, and both
// layers again from a picture of layer 0 on. A viewer whose path carries
// 1.2 Mbit/s gets layer 0 alone (half the frame rate), and the others'
// bitrate is what makes layer 0 fit its path, not its path's; once the path
// clears it gets both again. VP8 in software: the codec libraries layer on
// every system.
void viewersLayers(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    const auto vp8 = [](rcsdk::SessionConfig c) {
        c.setCodecs({RCS_CODEC_VP8}).setAcceleration(RCS_ACCELERATION_SOFTWARE);
        return c;
    };
    auto sharing = vp8(config(RCS_ROLE_SHARE, 47996, key));
    sharing.setVideo(640, 360, 30, 6'000'000).setViewers(3);
    auto& encodings = sharing.encodings();
    encodings.mode = RCS_ENCODINGS_SINGLE;
    encodings.scalability = RCS_SCALABILITY_L1T2;
    auto& tuning = sharing.extension<rcs_video_tuning_config>();
    tuning.move_up_ms = 3000;
    tuning.increase_percent = tuning.recovery_increase_percent = 200; // The cleared path is found fast.
    rcsdk::Session share(sharing, shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47996));
    proxy.bitsPerSecond = 700'000;
    GroupMember fast(vp8(config(RCS_ROLE_VIEW, 47996, key))), other(vp8(config(RCS_ROLE_VIEW, 47996, key))),
        slow(vp8(config(RCS_ROLE_VIEW, proxy.address().port(), key)));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    for (auto* viewer : {&fast, &other}) {
        CHECK(viewer->session->start() == RCS_OK && viewer->watching(5));
    }
    auto info = fast.session->streamInfo();
    CHECK(info.layers == 2 && info.received_layers == 2 && info.encodings == 1);
    const auto codecs = shareEvents.of(RCS_EVENT_CODEC);
    CHECK(!codecs.empty() && codecs.back().codec.temporal_layers == 2);
    auto sent = share.stats<rcs_video_send_stats>();
    CHECK(sent.temporal_layers == 2 && sent.base_layer_bytes > 0 && sent.base_layer_bytes < sent.encoded_bytes);
    const auto viewers = share.viewers();
    const rcs_viewer_id fastId = viewers[0].viewer_id;
    // The frame rates of two viewers over the same two seconds.
    const auto rates = [](GroupMember& a, GroupMember& b) {
        const int aBefore = a.frames, bBefore = b.frames;
        std::this_thread::sleep_for(2s);
        return std::pair{(a.frames - aBefore) / 2.0, (b.frames - bBefore) / 2.0};
    };
    // The layers it gets, as the sharer reported them last (0: never).
    const auto layersOf = [&](rcs_viewer_id id) {
        uint32_t layers = 0;
        for (const auto& event : shareEvents.of(RCS_EVENT_VIEWER_VIDEO)) {
            layers = event.viewer_id == id ? event.viewer_video.layers : layers;
        }
        return layers;
    };

    // The host's say: layer 0 alone, at once and without a keyframe.
    std::this_thread::sleep_for(1s);
    const uint64_t keyframes = share.stats<rcs_video_send_stats>().keyframes;
    const uint64_t recoveries = fast.session->stats<rcs_video_receive_stats>().recovery_requests;
    rcs_viewer_video video = RCS_VIEWER_VIDEO_INIT;
    video.layers = 1;
    CHECK(share.setViewerVideo(fastId, video) == RCS_OK);
    CHECK(waitFor([&] { return fast.session->streamInfo().received_layers == 1; }));
    CHECK(layersOf(fastId) == 1);
    const auto moved = shareEvents.of(RCS_EVENT_VIEWER_VIDEO).back();
    CHECK(moved.viewer_id == fastId && moved.viewer_video.encoding == 0 &&
          moved.viewer_video.reason == RCS_VIEWER_VIDEO_HOST);
    const auto [halfRate, fullRate] = rates(fast, other);
    std::printf("layers: the host's layer 0 alone: %.0f FPS, the other viewer %.0f FPS\n", halfRate, fullRate);
    CHECK(halfRate >= 5 && halfRate < fullRate * 0.7);
    auto mine = share.viewerStats<rcs_viewer_video_stats>(fastId);
    CHECK(mine.layers == 1 && mine.upper_layer_frames > 0 && mine.layer_moves_down == 1);
    video.layers = 0; // As the path says: both again.
    CHECK(share.setViewerVideo(fastId, video) == RCS_OK);
    CHECK(waitFor([&] { return fast.session->streamInfo().received_layers == 2; }));
    const auto [bothRate, otherRate] = rates(fast, other);
    CHECK(bothRate > otherRate * 0.8);
    CHECK(share.stats<rcs_video_send_stats>().keyframes == keyframes);
    CHECK(fast.session->stats<rcs_video_receive_stats>().recovery_requests == recoveries);
    video.layers = 4;
    CHECK(share.setViewerVideo(fastId, video) == RCS_ERROR_INVALID_ARGUMENT);

    // A slow path, content that needs the bitrate: layer 0 alone; the others
    // are not held to its bitrate.
    rcsdk::test::patternControl().grain = true;
    CHECK(slow.session->start() == RCS_OK && slow.watching(5));
    const rcs_viewer_id slowId = share.viewers()[2].viewer_id;
    CHECK(waitFor([&] { return slow.session->streamInfo().received_layers == 1; }, 20s));
    const auto down = shareEvents.of(RCS_EVENT_VIEWER_VIDEO).back();
    CHECK(down.viewer_id == slowId && down.viewer_video.encoding == 0 && down.viewer_video.layers == 1 &&
          down.viewer_video.reason == RCS_VIEWER_VIDEO_PATH);
    // The encoding's bitrate makes layer 0 fit the slow path: above what the path carries.
    const auto above = [&] {
        const uint32_t target = share.viewerStats<rcs_viewer_video_stats>(slowId).target_bps;
        return target && share.stats<rcs_video_send_stats>().bitrate_bps > target * 1.15;
    };
    CHECK(waitFor(above, 15s));
    const double slowRate = slow.rate(), fastRate = fast.rate();
    std::printf("layers: the slow viewer %.0f FPS on layer 0 (its path %u kbit/s), the others %.0f FPS at %u kbit/s\n",
                slowRate, share.viewerStats<rcs_viewer_video_stats>(slowId).target_bps / 1000, fastRate,
                share.stats<rcs_video_send_stats>().bitrate_bps / 1000);
    CHECK(slowRate > 0 && fastRate > slowRate);
    const auto group = share.stats<rcs_group_stats>();
    CHECK(group.layer_moves_down >= 2 && group.encodings == 1 && group.moves_down == 0);
    // Its path clears: both layers again.
    proxy.bitsPerSecond = 0;
    CHECK(waitFor([&] { return slow.session->streamInfo().received_layers == 2; }, 40s));
    CHECK(layersOf(slowId) == 2 && slow.watching(10));
    for (auto* viewer : {&fast, &other, &slow}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
    rcsdk::test::patternControl().grain = false;
}

// ---- Modules (rcsdk/module.h) --------------------------------------------------------------

// A module event of `module` and `kind` about `viewer` (and, with a result, of it).
bool moduleEvent(EventLog& log, const std::string& module, rcs_module_event_kind kind, rcs_viewer_id viewer,
                 rcs_result result = RCS_OK) {
    return waitFor([&] {
        for (const auto& e : log.ofModule(module, kind)) {
            if (e.viewer == viewer && e.result == result) {
                return true;
            }
        }
        return false;
    });
}

// Registering, the settings a module checks, then a sharer and a viewer
// whose module ("test.echo") works with its kind: they tell each other their
// hellos, send each other messages, reliable, unreliable and large, run their
// machine halves (the sharer's in its agent, the viewer's in its own
// process), emit events, take new settings, and stop. A module only the
// sharer uses ("test.local") is not active, and says why.
void moduleCore(const uint8_t (&key)[32]) {
    auto& echo = rcsdk::test::echoModule();
    auto& local = rcsdk::test::echoModule("test.local", RCS_MODULE_PEER);
    echo.clear();
    local.clear();
    CHECK(rcs_register_module(&echo.definition) == RCS_ERROR_INVALID_ARGUMENT); // Its id is taken.
    rcs_module_definition bad = echo.definition;
    bad.id = "not an id";
    CHECK(rcs_register_module(&bad) == RCS_ERROR_INVALID_ARGUMENT);
    bad.id = "test.bad";
    bad.max_message_bytes = 0;
    CHECK(rcs_register_module(&bad) == RCS_ERROR_INVALID_ARGUMENT);
    const auto registered = rcsdk::registeredModules();
    CHECK(std::any_of(registered.begin(), registered.end(), [](const rcs_module_info& m) {
        return std::string(m.id) == "test.echo" && m.version == 1 && m.flags == (RCS_MODULE_PEER | RCS_MODULE_MACHINE);
    }));
    // A module no one registered, settings it refuses (with its reason), the same one twice.
    CHECK(throws<rcsdk::Error>([&] {
        auto c = config(RCS_ROLE_SHARE, 47962, key);
        rcsdk::Session refused(c.useModule("test.unregistered", nullptr, 0));
    }));
    try {
        auto c = config(RCS_ROLE_SHARE, 47962, key);
        rcsdk::Session refused(c.useModule("test.echo", "refuse", 6));
        CHECK(false);
    } catch (const rcsdk::Error& error) {
        CHECK(error.result() == RCS_ERROR_INVALID_ARGUMENT &&
              std::string(error.what()).find("the test refuses these settings") != std::string::npos);
    }
    {
        rcs_module_config first = RCS_MODULE_CONFIG_INIT, second = RCS_MODULE_CONFIG_INIT;
        first.module = second.module = "test.echo";
        first.header.next = &second.header;
        rcs_session_config raw = config(RCS_ROLE_SHARE, 47962, key).get();
        raw.extensions = &first.header;
        rcs_session session = nullptr;
        CHECK(rcs_session_create(&raw, nullptr, &session) == RCS_ERROR_INVALID_ARGUMENT && !session);
    }

    EventLog shareEvents, viewEvents;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onEvent = viewEvents.callback();
    auto shareConfig = config(RCS_ROLE_SHARE, 47962, key);
    shareConfig.useModule("test.echo", "share settings", 14).useModule("test.local", nullptr, 0);
    auto viewConfig = config(RCS_ROLE_VIEW, 47962, key);
    viewConfig.useModule("test.echo", "view settings", 13);
    rcsdk::Session share(shareConfig, shareCallbacks);
    rcsdk::Session view(viewConfig, viewCallbacks);
    rcs_module found = nullptr;
    CHECK(rcs_module_find(share.handle(), "test.local", &found) == RCS_OK && found);
    CHECK(rcs_module_find(view.handle(), "test.local", &found) == RCS_ERROR_INVALID_STATE);
    CHECK(rcs_module_find(share.handle(), "test.echo", &found) == RCS_OK && rcs_module_session(found) == share.handle());
    CHECK(rcs_module_send(found, 0, reinterpret_cast<const uint8_t*>("x"), 1, 0, 0) == RCS_ERROR_INVALID_STATE);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING; }));

    // Each end's started with its settings and role; active with the other,
    // which said its hello; the module only the sharer uses is not.
    CHECK(echo.saw("start", share.handle(), "share settings") && echo.saw("start", view.handle(), "view settings"));
    CHECK(moduleEvent(shareEvents, "test.echo", RCS_MODULE_ACTIVE, 1));
    CHECK(moduleEvent(viewEvents, "test.echo", RCS_MODULE_ACTIVE, 0));
    CHECK(moduleEvent(shareEvents, "test.local", RCS_MODULE_INACTIVE, 1, RCS_ERROR_UNSUPPORTED));
    CHECK(shareEvents.ofModule("test.local", RCS_MODULE_INACTIVE)[0].message.find("does not use") !=
          std::string::npos);
    CHECK(waitFor([&] {
        return echo.saw("peer_joined", share.handle(), "hello from view") &&
               echo.saw("peer_joined", view.handle(), "hello from share");
    }));
    CHECK(!local.saw("peer_joined") && local.saw("start", share.handle()));
    // The machine halves: the sharer's in its agent, the viewer's in its process.
    CHECK(waitFor([&] { return echo.saw("machine_start", nullptr, "share") && echo.saw("machine_start", nullptr, "view"); }));
    CHECK(waitFor([&] {
        return echo.saw("machine_joined", share.handle()) && echo.saw("machine_joined", view.handle());
    }));

    // Messages both ways: the viewer pings, the sharer answers; sent() says
    // each went; a datagram; a large one in many parts.
    rcs_module viewHalf = nullptr, shareHalf = nullptr;
    CHECK(rcs_module_find(view.handle(), "test.echo", &viewHalf) == RCS_OK);
    CHECK(rcs_module_find(share.handle(), "test.echo", &shareHalf) == RCS_OK);
    const auto send = [](rcs_module half, rcs_viewer_id peer, const std::string& text, uint32_t flags = 0,
                         uint64_t tag = 1) {
        return rcs_module_send(half, peer, reinterpret_cast<const uint8_t*>(text.data()), uint32_t(text.size()),
                               flags, tag);
    };
    CHECK(send(viewHalf, 0, "ping:1") == RCS_OK);
    CHECK(waitFor([&] { return echo.saw("received", share.handle(), "ping:1"); }));
    CHECK(waitFor([&] { return echo.saw("received", view.handle(), "pong:1"); }));
    CHECK(waitFor([&] { return echo.saw("sent", view.handle(), "1") && echo.saw("sent", share.handle(), "7"); }));
    CHECK(send(viewHalf, 5, "to nobody") == RCS_ERROR_INVALID_ARGUMENT); // A viewer's peer is 0.
    CHECK(send(shareHalf, 9, "to nobody") == RCS_ERROR_INVALID_STATE);
    CHECK(send(shareHalf, 1, "x", RCS_MODULE_UNRELIABLE | RCS_MODULE_REPLACE) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(send(shareHalf, 1, std::string(RCS_DATA_MAX_DATAGRAM + 1, 'd'), RCS_MODULE_UNRELIABLE) ==
          RCS_ERROR_INVALID_ARGUMENT);
    CHECK(waitFor([&] {
        send(shareHalf, 1, "datagram", RCS_MODULE_UNRELIABLE); // May be lost: sent until one comes.
        for (const auto& call : echo.calls("received")) {
            if (rcs_module_session(call.module) == view.handle() && call.data == "datagram") {
                return call.flags == RCS_MODULE_UNRELIABLE;
            }
        }
        return false;
    }));
    CHECK(send(shareHalf, 0, std::string(5u << 20, 'L'), 0, 99) == RCS_OK); // Every viewer: the one there.
    CHECK(waitFor([&] { return echo.saw("received", view.handle(), "large:" + std::to_string(5u << 20)); }, 20s));
    CHECK(waitFor([&] { return echo.saw("sent", share.handle(), "99"); }));
    // A new hello reaches where the module is active.
    CHECK(rcs_module_set_hello(shareHalf, reinterpret_cast<const uint8_t*>("new hello"), 9) == RCS_OK);
    CHECK(waitFor([&] { return echo.saw("peer_hello", view.handle(), "new hello"); }));
    // The machine halves answer the session halves, a large message too.
    CHECK(rcs_module_send_machine(viewHalf, reinterpret_cast<const uint8_t*>("abc"), 3) == RCS_OK);
    CHECK(rcs_module_send_machine(shareHalf, reinterpret_cast<const uint8_t*>("def"), 3) == RCS_OK);
    CHECK(waitFor([&] {
        return echo.saw("from_machine", view.handle(), "machine:abc") &&
               echo.saw("from_machine", share.handle(), "machine:def");
    }));
    const std::string large(9u << 20, 'M');
    CHECK(rcs_module_send_machine(shareHalf, reinterpret_cast<const uint8_t*>(large.data()), uint32_t(large.size())) ==
          RCS_OK);
    CHECK(waitFor([&] { return echo.saw("from_machine", share.handle(), "large:" + std::to_string(large.size() + 8)); },
                  20s));
    // Events of its own reach the host, after the call that emitted them;
    // the SDK's kinds are not the module's to emit.
    CHECK(send(viewHalf, 0, "emit") == RCS_OK);
    CHECK(waitFor([&] {
        const auto own = shareEvents.ofModule("test.echo", 300);
        return !own.empty() && own[0].viewer == 1 && own[0].payload == std::vector<uint8_t>({'p', 'a', 'y', 'l', 'o', 'a', 'd'});
    }));
    CHECK(echo.saw("emit-low", share.handle()));
    // New settings while running reach the module.
    rcs_module_config update = RCS_MODULE_CONFIG_INIT;
    update.module = "test.echo";
    update.settings = "updated";
    update.size = 7;
    CHECK(share.update(update) == RCS_OK);
    CHECK(waitFor([&] { return echo.saw("update", share.handle(), "updated"); }));
    update.module = "test.unregistered";
    CHECK(share.update(update) == RCS_ERROR_INVALID_ARGUMENT);
    update.module = "test.local"; // Registered, but not one this session was created with...
    CHECK(view.update(update) == RCS_ERROR_INVALID_ARGUMENT);
    update.module = "test.echo";
    update.settings = "refuse"; // ...and settings it refuses.
    update.size = 6;
    CHECK(share.update(update) == RCS_ERROR_INVALID_ARGUMENT);
    // What went and came, the session's and the viewer's.
    const auto shareStats = share.moduleStats("test.echo");
    const auto viewerStats = share.moduleStats("test.echo", 1);
    CHECK(shareStats.sent_messages >= 3 && shareStats.received_messages >= 2 &&
          shareStats.sent_bytes >= (5u << 20) && viewerStats.sent_messages == shareStats.sent_messages);
    CHECK(view.moduleStats("test.echo").received_bytes >= (5u << 20));
    rcs_module_stats none = RCS_MODULE_STATS_INIT;
    CHECK(rcs_session_get_module_stats(view.handle(), 0, "test.local", &none) == RCS_ERROR_INVALID_STATE);
    CHECK(rcs_session_get_module_stats(view.handle(), 1, "test.echo", &none) == RCS_ERROR_UNSUPPORTED);

    // The viewer leaves: the sharer's module hears it; each end stops.
    CHECK(view.stop() == RCS_OK);
    CHECK(waitFor([&] { return echo.saw("peer_left", share.handle()); }));
    CHECK(echo.saw("peer_left", view.handle()) && echo.saw("machine_left", view.handle()) &&
          echo.saw("stop", view.handle()));
    CHECK(waitFor([&] { return echo.count("machine_stop") >= 1; }));
    CHECK(share.stop() == RCS_OK);
    CHECK(echo.saw("stop", share.handle()) && echo.count("machine_stop") == 2);
    CHECK(send(shareHalf, 0, "after the run") == RCS_ERROR_INVALID_STATE);
    CHECK(!echo.overlapped && !local.overlapped);
    CHECK(!shareEvents.overlapping && !viewEvents.overlapping);
}

// ---- The clipboard module (rcsdk/clipboard.h) -------------------------------------------------

using rcsdk::clipboard::test::clipboardItem;
using rcsdk::clipboard::test::FakeClipboard;
using rcsdk::clipboard::test::FakeClipboardAccess;
using rcsdk::clipboard::test::pngItem;
using rcsdk::clipboard::test::textCopy;

void registerClipboard() {
    static std::once_flag once;
    std::call_once(once, [] { rcsdk::registerModule(*rcs_clipboard_module()); });
}

// The files a copy names, as on_clipboard sees them.
struct SeenFiles {
    rcs_clipboard_flow flow = 0;
    rcs_viewer_id viewer = 0;
    std::vector<std::string> names, paths;
    std::array<uint8_t, RCS_FILE_PUBLICATION_SIZE> publication{};
    bool cut = false;
};

// Records what on_clipboard sees; refuses copies holding `refuse` (going or coming).
struct ClipboardLog {
    std::mutex mutex;
    std::vector<std::pair<rcs_clipboard_flow, std::string>> seen; // Its text, if any.
    std::vector<rcs_viewer_id> viewers;
    std::vector<SeenFiles> files;
    std::string refuse;

    void into(rcs_clipboard_settings& settings) {
        settings.user_data = this;
        settings.on_clipboard = [](const rcs_clipboard_content* content, void* self) -> int32_t {
            return static_cast<ClipboardLog*>(self)->see(*content) ? 1 : 0;
        };
    }
    bool see(const rcs_clipboard_content& content) {
        CHECK(content.struct_size == sizeof(content) && content.item_size == sizeof(rcs_clipboard_item));
        std::string text;
        for (uint32_t i = 0; i < content.count; ++i) {
            if (content.items[i].format == RCS_CLIPBOARD_TEXT) {
                text.assign(reinterpret_cast<const char*>(content.items[i].data), content.items[i].size);
            }
        }
        std::lock_guard lock(mutex);
        seen.emplace_back(content.flow, text);
        viewers.push_back(content.viewer_id);
        if (const auto* f = content.files) {
            CHECK(f->struct_size == sizeof(*f) && f->file_size == sizeof(rcs_clipboard_file));
            SeenFiles copy{content.flow, content.viewer_id, {}, {}, {}, f->cut != 0};
            for (uint32_t i = 0; i < f->count; ++i) {
                copy.names.push_back(f->files[i].name);
                copy.paths.push_back(f->files[i].path ? f->files[i].path : "");
            }
            std::copy(std::begin(f->publication), std::end(f->publication), copy.publication.begin());
            const bool refused = !refuse.empty() && copy.names.front() == refuse;
            files.push_back(std::move(copy));
            return !refused;
        }
        return refuse.empty() || text != refuse;
    }
    std::optional<SeenFiles> lastFiles(rcs_clipboard_flow flow) {
        std::lock_guard lock(mutex);
        for (auto at = files.rbegin(); at != files.rend(); ++at) {
            if (at->flow == flow) {
                return *at;
            }
        }
        return std::nullopt;
    }
    bool saw(rcs_clipboard_flow flow, const std::string& text) {
        std::lock_guard lock(mutex);
        return std::find(seen.begin(), seen.end(), std::pair(flow, text)) != seen.end();
    }
};

// The clipboard's RCS_CLIPBOARD_COPY events, and their viewers.
std::vector<std::pair<rcs_viewer_id, rcs_clipboard_event>> clipboardEvents(EventLog& log) {
    std::vector<std::pair<rcs_viewer_id, rcs_clipboard_event>> found;
    for (const auto& e : log.ofModule(RCS_CLIPBOARD_MODULE, RCS_CLIPBOARD_COPY)) {
        rcs_clipboard_event event{};
        CHECK(e.payload.size() == sizeof(event));
        std::memcpy(&event, e.payload.data(), sizeof(event));
        found.emplace_back(e.viewer, event);
    }
    return found;
}

bool clipboardEvent(EventLog& log, rcs_clipboard_flow flow, rcs_result result, rcs_viewer_id viewer = 0) {
    return waitFor([&] {
        for (const auto& [id, e] : clipboardEvents(log)) {
            if (e.flow == flow && e.result == result && id == viewer) {
                return true;
            }
        }
        return false;
    });
}

rcs_clipboard_settings clipboardSettings(rcs_clipboard_flow flow, rcs_clipboard_target target = RCS_CLIPBOARD_SYSTEM) {
    rcs_clipboard_settings settings = RCS_CLIPBOARD_SETTINGS_INIT;
    settings.flow = flow;
    settings.target = target;
    settings.check_interval_ms = 50;
    return settings;
}

rcsdk::SessionConfig& withClipboard(rcsdk::SessionConfig& c, const rcs_clipboard_settings& settings) {
    registerClipboard();
    return c.useModule(RCS_CLIPBOARD_MODULE, settings);
}

rcs_result updateClipboard(rcsdk::Session& session, const rcs_clipboard_settings& settings) {
    rcs_module_config use = RCS_MODULE_CONFIG_INIT;
    use.module = RCS_CLIPBOARD_MODULE;
    use.settings = &settings;
    use.size = sizeof(settings);
    return session.update(use);
}

rcs_clipboard_stats clipboardStats(rcsdk::Session& session, rcs_viewer_id viewer = 0) {
    rcs_clipboard_stats stats = RCS_CLIPBOARD_STATS_INIT;
    CHECK(rcs_clipboard_get_stats(session.handle(), viewer, &stats) == RCS_OK);
    return stats;
}

// What `viewer`'s end takes (0 and 0 where the clipboard is not active with it).
rcs_clipboard_peer clipboardPeer(rcsdk::Session& session, rcs_viewer_id viewer = 0) {
    rcs_clipboard_peer peer = RCS_CLIPBOARD_PEER_INIT;
    rcs_clipboard_get_peer(session.handle(), viewer, &peer);
    return peer;
}

rcs_result sendClipboardText(rcsdk::Session& session, std::string_view text, rcs_viewer_id viewer = 0) {
    const rcs_clipboard_item item{RCS_CLIPBOARD_TEXT, uint32_t(text.size()),
                                  reinterpret_cast<const uint8_t*>(text.data())};
    rcs_clipboard_content content = RCS_CLIPBOARD_CONTENT_INIT;
    content.viewer_id = viewer;
    content.count = 1;
    content.items = &item;
    return rcs_clipboard_send(session.handle(), &content);
}

// Off unless the host uses the module; both ways on the system's clipboard
// (the copy an end held before the connection goes only with send_existing,
// a copy placed never goes back); the formats and the size the other end
// takes; concealed copies; the host keeping copies back; the other end that
// stops taking copies; the host's own clipboard (target HOST) and its sends.
void clipboard(const uint8_t (&key)[32]) {
    FakeClipboard sharerBoard, viewerBoard;
    rcsdk::clipboard::setFactory([&](rcs_role role) -> std::unique_ptr<rcsdk::clipboard::SystemClipboard> {
        return std::make_unique<FakeClipboardAccess>(role == RCS_ROLE_SHARE ? sharerBoard : viewerBoard);
    });
    constexpr uint32_t both = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
    sharerBoard.copy(textCopy("copied before"));
    viewerBoard.copy(textCopy("the viewer held this"));
    {
        EventLog viewEvents;
        rcsdk::Callbacks viewCallbacks;
        viewCallbacks.onEvent = viewEvents.callback();
        auto viewConfig = config(RCS_ROLE_VIEW, 47960, key);
        rcsdk::Session share(config(RCS_ROLE_SHARE, 47960, key));
        rcsdk::Session view(withClipboard(viewConfig, clipboardSettings(both)), viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING; }));
        // A sharer that does not use the module: the viewer's says why it is not active.
        CHECK(moduleEvent(viewEvents, RCS_CLIPBOARD_MODULE, RCS_MODULE_INACTIVE, 0, RCS_ERROR_UNSUPPORTED));
        std::this_thread::sleep_for(300ms);
        CHECK(clipboardPeer(view).formats == 0);
        rcs_clipboard_stats stats = RCS_CLIPBOARD_STATS_INIT;
        CHECK(rcs_clipboard_get_stats(share.handle(), 0, &stats) == RCS_ERROR_INVALID_STATE);
        CHECK(viewerBoard.written() == 0 && sharerBoard.written() == 0);
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    {
        EventLog shareEvents, viewEvents;
        ClipboardLog shareSeen, viewSeen;
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        viewCallbacks.onEvent = viewEvents.callback();
        auto shareClipboard = clipboardSettings(both), viewClipboard = clipboardSettings(both);
        shareClipboard.send_existing = 1;
        shareSeen.into(shareClipboard);
        viewSeen.into(viewClipboard);
        auto shareConfig = config(RCS_ROLE_SHARE, 47960, key);
        auto viewConfig = config(RCS_ROLE_VIEW, 47960, key);
        rcsdk::Session share(withClipboard(shareConfig, shareClipboard), shareCallbacks);
        rcsdk::Session view(withClipboard(viewConfig, viewClipboard), viewCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        // What the sharer's clipboard held when the viewer came goes
        // (send_existing); the viewer's does not.
        CHECK(waitFor([&] { return viewerBoard.text() == "copied before"; }));
        std::this_thread::sleep_for(300ms);
        CHECK(sharerBoard.text() == "copied before" && sharerBoard.written() == 0);
        CHECK(clipboardPeer(view).formats == RCS_CLIPBOARD_ALL_FORMATS && clipboardPeer(view).max_bytes == 16u << 20 &&
              clipboardPeer(share, 1).formats == RCS_CLIPBOARD_ALL_FORMATS);
        CHECK(clipboardEvent(shareEvents, RCS_CLIPBOARD_SEND, RCS_OK, 1));
        CHECK(clipboardEvent(viewEvents, RCS_CLIPBOARD_RECEIVE, RCS_OK));
        CHECK(shareSeen.saw(RCS_CLIPBOARD_SEND, "copied before") &&
              viewSeen.saw(RCS_CLIPBOARD_RECEIVE, "copied before"));

        // Text and an image from the sharer; then the viewer's copy, which
        // the sharer places and never sends back.
        auto rich = textCopy("text and image");
        rich.items.push_back(pngItem(5000));
        sharerBoard.copy(rich);
        CHECK(waitFor([&] { return viewerBoard.held().items.size() == 2 && viewerBoard.text() == "text and image"; }));
        const int viewerWrites = viewerBoard.written();
        viewerBoard.copy(textCopy("from the viewer"));
        CHECK(waitFor([&] { return sharerBoard.text() == "from the viewer"; }));
        std::this_thread::sleep_for(300ms);
        CHECK(viewerBoard.written() == viewerWrites); // Not echoed.
        CHECK(waitFor([&] {
            const auto sent = clipboardStats(share);
            const auto got = clipboardStats(view);
            return sent.sent == 2 && sent.received == 1 && got.sent == 1 && got.received == 2 &&
                   got.received_bytes == 13 + 14 + 5000;
        }));
        CHECK(clipboardStats(share, 1).sent == 2);

        // A concealed copy stays, unless send_concealed; it is placed concealed.
        sharerBoard.copy(textCopy("a password", true));
        CHECK(waitFor([&] { return clipboardStats(share).concealed == 1; }));
        shareClipboard.send_concealed = 1;
        CHECK(updateClipboard(share, shareClipboard) == RCS_OK);
        sharerBoard.copy(textCopy("another password", true));
        CHECK(waitFor([&] { return viewerBoard.text() == "another password" && viewerBoard.held().concealed; }));
        // What a copy holds is never logged, a concealed one's least of all (the log is at DEBUG here).
        CHECK(!sessionLog().contains("a password") && !sessionLog().contains("another password"));

        // The viewer takes text up to 1 KiB only: what does not fit is left
        // out, a copy with nothing that fits does not go.
        viewClipboard.formats = 1u << RCS_CLIPBOARD_TEXT;
        viewClipboard.max_bytes = 1024;
        CHECK(updateClipboard(view, viewClipboard) == RCS_OK);
        CHECK(waitFor([&] { return clipboardPeer(share, 1).formats == (1u << RCS_CLIPBOARD_TEXT); }));
        auto large = textCopy(std::string(2000, 'x'));
        large.items.push_back(pngItem(24));
        sharerBoard.copy(large);
        CHECK(clipboardEvent(shareEvents, RCS_CLIPBOARD_SEND, RCS_ERROR_BUFFER_TOO_SMALL, 1));
        auto fitting = textCopy("small");
        fitting.items.push_back(pngItem(24));
        sharerBoard.copy(fitting);
        CHECK(waitFor([&] { return viewerBoard.text() == "small" && viewerBoard.held().items.size() == 1; }));
        CHECK(waitFor([&] {
            const auto events = clipboardEvents(shareEvents);
            return !events.empty() && events.back().second.result == RCS_OK &&
                   events.back().second.formats == (1u << RCS_CLIPBOARD_TEXT) &&
                   (events.back().second.left_out & (1u << RCS_CLIPBOARD_IMAGE));
        }));

        // The hosts keep copies back, going and coming.
        {
            std::lock_guard lock(shareSeen.mutex);
            shareSeen.refuse = "kept here";
        }
        sharerBoard.copy(textCopy("kept here"));
        CHECK(waitFor([&] { return clipboardStats(share).refused == 1; }));
        {
            std::lock_guard lock(viewSeen.mutex);
            viewSeen.refuse = "not wanted";
        }
        sharerBoard.copy(textCopy("not wanted"));
        CHECK(waitFor([&] { return clipboardStats(view).refused == 1; }));
        CHECK(viewerBoard.text() == "small");

        // The viewer stops taking copies: the sharer learns it and sends none.
        viewClipboard.flow = RCS_CLIPBOARD_SEND;
        CHECK(updateClipboard(view, viewClipboard) == RCS_OK);
        CHECK(waitFor([&] { return clipboardPeer(share, 1).formats == 0; }));
        CHECK(!shareEvents.ofModule(RCS_CLIPBOARD_MODULE, RCS_CLIPBOARD_PEER).empty());
        const auto sentBefore = clipboardStats(share).sent;
        sharerBoard.copy(textCopy("nobody takes it"));
        std::this_thread::sleep_for(300ms);
        CHECK(clipboardStats(share).sent == sentBefore && viewerBoard.text() == "small");
        CHECK(!viewEvents.overlapping && !shareEvents.overlapping);
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    {
        // The hosts' own clipboards: copies go through on_clipboard, the
        // system's are never touched.
        CHECK(throws<rcsdk::Error>([&] {
            auto c = config(RCS_ROLE_VIEW, 47960, key);
            rcsdk::Session invalid(withClipboard(c, clipboardSettings(RCS_CLIPBOARD_RECEIVE, RCS_CLIPBOARD_HOST)));
        }));
        ClipboardLog viewSeen;
        auto viewClipboard = clipboardSettings(RCS_CLIPBOARD_RECEIVE, RCS_CLIPBOARD_HOST);
        viewSeen.into(viewClipboard);
        auto shareConfig = config(RCS_ROLE_SHARE, 47960, key);
        auto viewConfig = config(RCS_ROLE_VIEW, 47960, key);
        auto shareClipboard = clipboardSettings(RCS_CLIPBOARD_SEND, RCS_CLIPBOARD_HOST);
        shareClipboard.max_bytes = 1024;
        rcsdk::Session share(withClipboard(shareConfig, shareClipboard));
        rcsdk::Session view(withClipboard(viewConfig, viewClipboard));
        CHECK(sendClipboardText(share, "too early") == RCS_ERROR_INVALID_STATE);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(view.start() == RCS_OK);
        CHECK(waitFor([&] { return clipboardPeer(share, 1).formats == RCS_CLIPBOARD_ALL_FORMATS; }));
        const int sharerWrites = sharerBoard.written(), viewerWrites = viewerBoard.written();
        CHECK(sendClipboardText(share, "from the host") == RCS_OK);
        CHECK(waitFor([&] { return viewSeen.saw(RCS_CLIPBOARD_RECEIVE, "from the host"); }));
        CHECK(sendClipboardText(view, "no SEND here") == RCS_ERROR_INVALID_STATE);
        CHECK(sendClipboardText(share, "a viewer not there", 5) == RCS_ERROR_INVALID_STATE);
        CHECK(sendClipboardText(share, "") == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(sendClipboardText(share, "\xff\xfe") == RCS_ERROR_INVALID_ARGUMENT); // Not UTF-8.
        CHECK(sendClipboardText(share, std::string(1025, 'm')) == RCS_ERROR_INVALID_ARGUMENT &&
              rcsdk::lastCallError().find("max_bytes") != std::string::npos);
        const rcs_clipboard_item twice[] = {{RCS_CLIPBOARD_TEXT, 1, reinterpret_cast<const uint8_t*>("a")},
                                            {RCS_CLIPBOARD_TEXT, 1, reinterpret_cast<const uint8_t*>("b")}};
        rcs_clipboard_content content = RCS_CLIPBOARD_CONTENT_INIT;
        content.count = 2;
        content.items = twice;
        CHECK(rcs_clipboard_send(share.handle(), &content) == RCS_ERROR_INVALID_ARGUMENT);
        const rcs_clipboard_item unknown{9, 1, reinterpret_cast<const uint8_t*>("a")};
        content.count = 1;
        content.items = &unknown;
        CHECK(rcs_clipboard_send(share.handle(), &content) == RCS_ERROR_INVALID_ARGUMENT);
        std::this_thread::sleep_for(200ms);
        CHECK(sharerBoard.written() == sharerWrites && viewerBoard.written() == viewerWrites);
        CHECK(view.stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::clipboard::setFactory({});
}

// A sharer with two viewers: one viewer's copy is placed on the sharer's
// clipboard and goes on to the other viewer, never back to the first.
void clipboardViewers(const uint8_t (&key)[32]) {
    FakeClipboard sharerBoard, firstBoard, secondBoard;
    std::atomic<int> views{0};
    rcsdk::clipboard::setFactory([&](rcs_role role) -> std::unique_ptr<rcsdk::clipboard::SystemClipboard> {
        if (role == RCS_ROLE_SHARE) {
            return std::make_unique<FakeClipboardAccess>(sharerBoard);
        }
        return std::make_unique<FakeClipboardAccess>(views++ == 0 ? firstBoard : secondBoard);
    });
    constexpr uint32_t both = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    auto shareConfig = config(RCS_ROLE_SHARE, 47961, key).setViewers(2);
    rcsdk::Session share(withClipboard(shareConfig, clipboardSettings(both)), shareCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    auto firstConfig = config(RCS_ROLE_VIEW, 47961, key);
    TestViewer first(withClipboard(firstConfig, clipboardSettings(both)));
    CHECK(first.session->start() == RCS_OK);
    CHECK(waitFor([&] { return views == 1 && share.viewers().size() == 1; }));
    auto secondConfig = config(RCS_ROLE_VIEW, 47961, key);
    TestViewer second(withClipboard(secondConfig, clipboardSettings(both)));
    CHECK(second.session->start() == RCS_OK);
    CHECK(waitFor([&] { return share.viewers().size() == 2; }));
    const rcs_viewer_id firstId = share.viewers()[0].viewer_id;
    CHECK(waitFor([&] { return clipboardPeer(share, firstId).formats && clipboardPeer(share, firstId + 1).formats; }));
    std::this_thread::sleep_for(300ms);
    const int firstWrites = firstBoard.written();
    firstBoard.copy(textCopy("from the first viewer"));
    CHECK(waitFor([&] { return sharerBoard.text() == "from the first viewer"; }));
    CHECK(waitFor([&] { return secondBoard.text() == "from the first viewer"; }));
    CHECK(clipboardEvent(shareEvents, RCS_CLIPBOARD_RECEIVE, RCS_OK, firstId));
    std::this_thread::sleep_for(300ms);
    CHECK(firstBoard.written() == firstWrites);
    CHECK(clipboardStats(share, firstId).received == 1);
    CHECK(second.session->stop() == RCS_OK);
    CHECK(first.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    rcsdk::clipboard::setFactory({});
}

// ---- File transfer (rcsdk/file_transfer.h) --------------------------------------------------

namespace fs = std::filesystem;

void registerFileTransfer() {
    static std::once_flag once;
    std::call_once(once, [] { rcsdk::registerModule(*rcs_file_transfer_module()); });
}

constexpr uint32_t bothWays = RCS_FILE_SEND | RCS_FILE_RECEIVE;

rcs_file_transfer_settings fileSettings(rcs_file_flow flow) {
    rcs_file_transfer_settings settings = RCS_FILE_TRANSFER_SETTINGS_INIT;
    settings.flow = flow;
    return settings;
}

rcsdk::SessionConfig& withFiles(rcsdk::SessionConfig& c, const rcs_file_transfer_settings& settings) {
    registerFileTransfer();
    return c.useModule(RCS_FILE_TRANSFER_MODULE, settings);
}

rcs_result updateFiles(rcsdk::Session& session, const rcs_file_transfer_settings& settings) {
    rcs_module_config use = RCS_MODULE_CONFIG_INIT;
    use.module = RCS_FILE_TRANSFER_MODULE;
    use.settings = &settings;
    use.size = sizeof(settings);
    return session.update(use);
}

std::string utf8Of(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// A directory of the test's own, gone afterwards.
struct Scratch {
    fs::path path;
    explicit Scratch(const std::string& name)
        : path(fs::temp_directory_path() / ("rcsdk-test-files-" + name + "-" + std::to_string(ownProcess()))) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path operator/(const std::string& rest) const { return path / rest; }
};

std::string pattern(size_t size, uint32_t seed) {
    std::string text(size, '\0');
    uint32_t state = seed * 2654435761u + 1;
    for (auto& c : text) {
        state = state * 1664525u + 1013904223u;
        c = char(state >> 24);
    }
    return text;
}

void writeText(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Large files of a pattern, made and checked 1 MiB at a time.
class LargePattern {
public:
    void next(std::vector<uint64_t>& block) {
        for (auto& word : block) {
            state_ ^= state_ << 13;
            state_ ^= state_ >> 7;
            state_ ^= state_ << 17;
            word = state_;
        }
    }
    static void make(const fs::path& path, uint64_t size) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        LargePattern pattern;
        std::vector<uint64_t> block(1u << 17);
        for (uint64_t written = 0; written < size; written += block.size() * 8) {
            pattern.next(block);
            out.write(reinterpret_cast<const char*>(block.data()), std::streamsize(block.size() * 8));
        }
    }
    static bool holds(const fs::path& path, uint64_t size) {
        if (fs::file_size(path) != size) {
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        LargePattern pattern;
        std::vector<uint64_t> block(1u << 17), read(1u << 17);
        for (uint64_t at = 0; at < size; at += block.size() * 8) {
            pattern.next(block);
            in.read(reinterpret_cast<char*>(read.data()), std::streamsize(read.size() * 8));
            if (!in || block != read) {
                return false;
            }
        }
        return true;
    }

private:
    uint64_t state_ = 88172645463325252ull;
};

// Partial files or records left in a directory.
bool leftovers(const fs::path& directory) {
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        const std::string name = utf8Of(entry.path().filename());
        if (name.find(".rcsdk-part") != std::string::npos || name.rfind(".rcsdk-resume-", 0) == 0) {
            return true;
        }
    }
    return false;
}

// The partial files of `name` in `directory` ("<name>.<token in hex>.rcsdk-part").
std::vector<fs::path> partials(const fs::path& directory, const std::string& name) {
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const std::string file = utf8Of(entry.path().filename());
        if (file.rfind(name + ".", 0) == 0 && file.size() == name.size() + 1 + 2 * RCS_FILE_TOKEN_SIZE + 11 &&
            file.substr(file.size() - 11) == ".rcsdk-part") {
            found.push_back(entry.path());
        }
    }
    return found;
}

// The module's events of `kind`, with their viewers.
template <class Payload>
std::vector<std::pair<rcs_viewer_id, Payload>> fileEvents(EventLog& log, rcs_module_event_kind kind) {
    std::vector<std::pair<rcs_viewer_id, Payload>> found;
    for (const auto& e : log.ofModule(RCS_FILE_TRANSFER_MODULE, kind)) {
        Payload payload{};
        CHECK(e.payload.size() == sizeof(payload));
        std::memcpy(&payload, e.payload.data(), sizeof(payload));
        found.emplace_back(e.viewer, payload);
    }
    return found;
}

std::optional<rcs_file_done_event> fileDone(EventLog& log, uint64_t transfer) {
    for (const auto& [viewer, e] : fileEvents<rcs_file_done_event>(log, RCS_FILE_DONE)) {
        if (e.transfer == transfer) {
            return e;
        }
    }
    return std::nullopt;
}

// RCS_FILE_DONE of a transfer this end received, as its host gets it in on_event; null for any other event.
const rcs_file_done_event* receivedDone(const rcs_event& event) {
    if (event.type != RCS_EVENT_MODULE || event.module.kind != RCS_FILE_DONE ||
        std::strcmp(event.module.module, RCS_FILE_TRANSFER_MODULE) != 0) {
        return nullptr;
    }
    const auto* done = static_cast<const rcs_file_done_event*>(event.module.payload);
    return done->direction == RCS_FILE_RECEIVE ? done : nullptr;
}

rcs_file_done_event waitDone(EventLog& log, uint64_t transfer, std::chrono::milliseconds timeout = 10s) {
    CHECK(waitFor([&] { return fileDone(log, transfer).has_value(); }, timeout));
    return *fileDone(log, transfer);
}

// The next offer the log holds after the `seen` before it.
rcs_file_offer_event waitOffer(EventLog& log, size_t& seen) {
    CHECK(waitFor([&] { return fileEvents<rcs_file_offer_event>(log, RCS_FILE_OFFER).size() > seen; }));
    return fileEvents<rcs_file_offer_event>(log, RCS_FILE_OFFER)[seen++].second;
}

std::vector<rcs_file_item_event> itemEvents(EventLog& log, uint64_t transfer) {
    std::vector<rcs_file_item_event> found;
    for (const auto& [viewer, e] : fileEvents<rcs_file_item_event>(log, RCS_FILE_ITEM_DONE)) {
        if (e.transfer == transfer) {
            found.push_back(e);
        }
    }
    return found;
}

rcs_file_transfer_info transferInfo(rcsdk::Session& session, uint64_t transfer) {
    rcs_file_transfer_info info = RCS_FILE_TRANSFER_INFO_INIT;
    CHECK(rcs_file_get_transfer(session.handle(), transfer, &info) == RCS_OK);
    return info;
}

std::vector<rcs_file_item_info> transferItems(rcsdk::Session& session, uint64_t transfer) {
    uint32_t count = 0;
    CHECK(rcs_file_get_items(session.handle(), transfer, 0, nullptr, sizeof(rcs_file_item_info), 0, &count) == RCS_OK);
    std::vector<rcs_file_item_info> items(count);
    CHECK(rcs_file_get_items(session.handle(), transfer, 0, items.data(), sizeof(rcs_file_item_info), count, &count) ==
          RCS_OK);
    return items;
}

rcs_file_peer filePeer(rcsdk::Session& session, rcs_viewer_id viewer = 0) {
    rcs_file_peer peer = RCS_FILE_PEER_INIT;
    rcs_file_get_peer(session.handle(), viewer, &peer);
    return peer;
}

// Offers files and directories of this machine (target SYSTEM).
rcs_result offerPaths(rcsdk::Session& session, rcs_viewer_id viewer, const std::vector<fs::path>& paths,
                      uint64_t* transfer, const uint8_t* token = nullptr) {
    std::vector<std::string> texts;
    for (const auto& path : paths) {
        texts.push_back(utf8Of(path));
    }
    std::vector<rcs_file_item> items(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        items[i].path = texts[i].c_str();
    }
    rcs_file_offer offer = RCS_FILE_OFFER_INIT;
    offer.viewer_id = viewer;
    offer.count = uint32_t(items.size());
    offer.items = items.data();
    if (token) {
        std::memcpy(offer.resume_token, token, RCS_FILE_TOKEN_SIZE);
    }
    uint64_t ignored = 0;
    return rcs_file_send(session.handle(), &offer, transfer ? transfer : &ignored);
}

uint64_t sendPaths(rcsdk::Session& session, rcs_viewer_id viewer, const std::vector<fs::path>& paths,
                   const uint8_t* token = nullptr) {
    uint64_t transfer = 0;
    const rcs_result sent = offerPaths(session, viewer, paths, &transfer, token);
    if (sent != RCS_OK) {
        std::printf("rcs_file_send: %s (%s)\n", rcs_result_message(sent), rcsdk::lastCallError().c_str());
    }
    CHECK(sent == RCS_OK && transfer);
    return transfer;
}

rcs_result acceptInto(rcsdk::Session& session, uint64_t transfer, const fs::path& directory,
                      rcs_file_conflict conflict = RCS_FILE_CONFLICT_RENAME) {
    return rcs_file_accept(session.handle(), transfer, utf8Of(directory).c_str(), conflict);
}

// A sharer and a viewer that both use file transfer, connected.
struct FilePair {
    EventLog shareEvents, viewEvents;
    std::atomic<int> frames{0};
    std::unique_ptr<rcsdk::Session> share, view;
    size_t shareOffers = 0, viewOffers = 0;
    // Each host deciding or looking in on_event (set before it runs), before the log has the event.
    std::function<void(const rcs_event&)> shareHook, viewHook;

    FilePair(rcsdk::SessionConfig shareConfig, rcsdk::SessionConfig viewConfig,
             const rcs_file_transfer_settings& shareFiles, const rcs_file_transfer_settings& viewFiles) {
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onEvent = [this, log = shareEvents.callback()](const rcs_event& event) {
            if (shareHook) {
                shareHook(event);
            }
            log(event);
        };
        viewCallbacks.onEvent = [this, log = viewEvents.callback()](const rcs_event& event) {
            if (viewHook) {
                viewHook(event);
            }
            log(event);
        };
        viewCallbacks.onFrame = [this](const rcs_frame&) { ++frames; };
        share = std::make_unique<rcsdk::Session>(withFiles(shareConfig, shareFiles), shareCallbacks);
        view = std::make_unique<rcsdk::Session>(withFiles(viewConfig, viewFiles), viewCallbacks);
    }
    void connect() {
        CHECK(share->start() == RCS_OK);
        CHECK(waitFor([&] { return share->state() == RCS_STATE_LISTENING; }));
        CHECK(view->start() == RCS_OK);
        CHECK(waitFor([&] { return view->state() == RCS_STATE_RUNNING; }));
        CHECK(waitFor([&] { return filePeer(*share, 1).max_items && filePeer(*view).max_items; }));
    }
    // A transfer from the sharer, accepted by the viewer into `directory`: both ends' numbers.
    std::pair<uint64_t, uint64_t> toViewer(const std::vector<fs::path>& paths, const fs::path& directory,
                                           rcs_file_conflict conflict = RCS_FILE_CONFLICT_RENAME,
                                           rcs_viewer_id viewer = 1) {
        const uint64_t sent = sendPaths(*share, viewer, paths);
        const uint64_t got = waitOffer(viewEvents, viewOffers).transfer;
        CHECK(acceptInto(*view, got, directory, conflict) == RCS_OK);
        return {sent, got};
    }
    std::pair<uint64_t, uint64_t> toSharer(const std::vector<fs::path>& paths, const fs::path& directory,
                                           rcs_file_conflict conflict = RCS_FILE_CONFLICT_RENAME) {
        const uint64_t sent = sendPaths(*view, 0, paths);
        const uint64_t got = waitOffer(shareEvents, shareOffers).transfer;
        CHECK(acceptInto(*share, got, directory, conflict) == RCS_OK);
        return {sent, got};
    }
};

// Both ends send and receive files and directories of their machines: the
// receiving host decides (accepting into a directory with a rule for names
// taken, rejecting, or not in time); what the other end does not allow,
// limits and arguments are refused at once; a path not there and too little
// space end a transfer with why.
void fileTransfer(const uint8_t (&key)[32]) {
    Scratch scratch("basic");
    const fs::path out = scratch / "out", shareIn = scratch / "share-in", viewIn = scratch / "view-in";
    fs::create_directories(shareIn);
    fs::create_directories(viewIn);
    const std::string photo = pattern(100000, 1), notes = pattern(600000, 2);
    writeText(out / "photos" / "a.jpg", photo);
    writeText(out / "photos" / "sub" / "b.txt", "bee");
    writeText(out / "photos" / "empty.txt", "");
    fs::create_directories(out / "photos" / "nothing");
    std::error_code noLink;
    fs::create_symlink(out / "photos" / "a.jpg", out / "photos" / "link", noLink);
    const bool linked = !noLink; // Windows lets only some processes make links.
    writeText(out / "notes.txt", notes);

    auto viewFiles = fileSettings(bothWays);
    FilePair pair(config(RCS_ROLE_SHARE, 47970, key), config(RCS_ROLE_VIEW, 47970, key), fileSettings(bothWays),
                  viewFiles);
    auto& share = *pair.share;
    auto& view = *pair.view;
    // What the viewer's host finds in its directory as RCS_FILE_DONE of a
    // transfer it received reaches it: everything in place by then.
    struct Seen {
        bool leftovers = false;
        fs::file_time_type sub, d19;
    };
    std::mutex seenMutex;
    std::map<uint64_t, Seen> seen;
    pair.viewHook = [&](const rcs_event& event) {
        if (const auto* done = receivedDone(event)) {
            std::error_code none;
            Seen now{leftovers(viewIn), fs::last_write_time(viewIn / "photos" / "sub", none),
                     fs::last_write_time(viewIn / "many" / "d19", none)};
            std::lock_guard lock(seenMutex);
            seen[done->transfer] = now;
        }
    };
    const auto seenAtDone = [&](uint64_t transfer) {
        std::lock_guard lock(seenMutex);
        CHECK(seen.count(transfer));
        return seen[transfer];
    };
    uint64_t transfer = 0;
    CHECK(offerPaths(share, 1, {out / "notes.txt"}, &transfer) == RCS_ERROR_INVALID_STATE); // Not running.
    pair.connect();
    CHECK(filePeer(view).flow == bothWays && filePeer(share, 1).flow == bothWays &&
          filePeer(share, 1).max_items == 10000);
    CHECK(!fileEvents<rcs_file_peer>(pair.shareEvents, RCS_FILE_PEER).empty());

    // The sharer's directory and file go to the viewer, in their order; the
    // link is listed and skipped, at both ends.
    const uint64_t sent = sendPaths(share, 1, {out / "photos", out / "notes.txt"});
    const auto offered = waitOffer(pair.viewEvents, pair.viewOffers);
    const uint64_t got = offered.transfer;
    CHECK(offered.items == (linked ? 8u : 7u) && offered.total_bytes == photo.size() + 3 + notes.size() &&
          !offered.resuming);
    {
        std::vector<std::string> paths;
        for (const auto& item : transferItems(view, got)) {
            paths.push_back(item.path);
            CHECK(item.state == RCS_FILE_ITEM_PENDING);
        }
        std::vector<std::string> expected = {"photos", "photos/a.jpg", "photos/empty.txt"};
        if (linked) {
            expected.push_back("photos/link");
        }
        expected.insert(expected.end(), {"photos/nothing", "photos/sub", "photos/sub/b.txt", "notes.txt"});
        CHECK(paths == expected);
        // A page of them at a time.
        rcs_file_item_info page[2];
        uint32_t count = 0;
        CHECK(rcs_file_get_items(view.handle(), got, 1, page, sizeof(rcs_file_item_info), 2, &count) ==
                  RCS_ERROR_BUFFER_TOO_SMALL &&
              count == offered.items - 1 && page[0].index == 1 && std::string(page[1].path) == "photos/empty.txt");
    }
    CHECK(transferInfo(view, got).state == RCS_FILE_OFFERED && transferInfo(share, sent).state == RCS_FILE_OFFERED);
    CHECK(rcs_file_accept(view.handle(), got, "relative/dir", RCS_FILE_CONFLICT_RENAME) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(acceptInto(view, got, viewIn, 9) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(acceptInto(view, 999, viewIn) == RCS_ERROR_INVALID_STATE);
    CHECK(acceptInto(view, got, viewIn) == RCS_OK);
    CHECK(acceptInto(view, got, viewIn) == RCS_ERROR_INVALID_STATE); // Decided.
    {
        const auto viewDone = waitDone(pair.viewEvents, got);
        const auto shareDone = waitDone(pair.shareEvents, sent);
        CHECK(viewDone.end == RCS_FILE_COMPLETED && viewDone.result == RCS_OK && viewDone.direction == RCS_FILE_RECEIVE);
        CHECK(shareDone.end == RCS_FILE_COMPLETED && shareDone.direction == RCS_FILE_SEND);
        CHECK(viewDone.items_completed == 7 && shareDone.items_completed == 7 &&
              viewDone.items_skipped == (linked ? 1u : 0u) && shareDone.items_skipped == viewDone.items_skipped);
        CHECK(viewDone.done_bytes == offered.total_bytes && shareDone.done_bytes == offered.total_bytes);
    }
    CHECK(readText(viewIn / "notes.txt") == notes && readText(viewIn / "photos" / "a.jpg") == photo &&
          readText(viewIn / "photos" / "sub" / "b.txt") == "bee");
    CHECK(fs::file_size(viewIn / "photos" / "empty.txt") == 0 && fs::is_directory(viewIn / "photos" / "nothing"));
    CHECK(!fs::exists(viewIn / "photos" / "link") && !leftovers(viewIn));
    CHECK(fs::last_write_time(viewIn / "notes.txt") == fs::last_write_time(out / "notes.txt"));
    CHECK(fs::last_write_time(viewIn / "photos" / "sub") == fs::last_write_time(out / "photos" / "sub"));
    CHECK(!seenAtDone(got).leftovers && seenAtDone(got).sub == fs::last_write_time(out / "photos" / "sub"));
    {
        const auto info = transferInfo(share, sent);
        CHECK(info.state == RCS_FILE_FINISHED && info.direction == RCS_FILE_SEND && info.viewer_id == 1 &&
              info.target == RCS_FILE_SYSTEM && info.done_bytes == info.total_bytes &&
              info.transferred_bytes == photo.size() + 3 + notes.size() && info.resumed_bytes == 0);
        const auto received = transferInfo(view, got);
        CHECK(std::string(received.directory) == utf8Of(viewIn) &&
              std::memcmp(received.token, info.token, RCS_FILE_TOKEN_SIZE) == 0);
        CHECK(itemEvents(pair.viewEvents, got).size() == offered.items &&
              itemEvents(pair.shareEvents, sent).size() == offered.items);
        for (const auto& item : transferItems(view, got)) {
            if (std::string(item.path) == "photos/link") {
                CHECK(item.state == RCS_FILE_ITEM_SKIPPED && item.result == RCS_ERROR_UNSUPPORTED);
            } else {
                CHECK(item.state == RCS_FILE_ITEM_COMPLETED && std::string(item.saved_as) == item.path &&
                      item.bytes == item.size);
            }
        }
        for (const auto& item : transferItems(share, sent)) {
            CHECK(item.state == (std::string(item.path) == "photos/link" ? RCS_FILE_ITEM_SKIPPED
                                                                         : RCS_FILE_ITEM_COMPLETED));
        }
    }

    // Many small files: one after the other without waiting for each.
    {
        for (int d = 0; d < 20; ++d) {
            for (int f = 0; f < 100; ++f) {
                writeText(out / "many" / ("d" + std::to_string(d)) / ("f" + std::to_string(f) + ".txt"),
                          std::to_string(d * 1000 + f));
            }
        }
        const auto started = std::chrono::steady_clock::now();
        const auto [s, g] = pair.toViewer({out / "many"}, viewIn);
        const auto done = waitDone(pair.viewEvents, g, 30s);
        CHECK(done.end == RCS_FILE_COMPLETED && done.items_completed == 2021 && waitDone(pair.shareEvents, s).items_completed == 2021);
        std::printf("file_transfer: 2 000 small files in %.2f s\n",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        CHECK(readText(viewIn / "many" / "d19" / "f99.txt") == "19099" && !leftovers(viewIn));
        CHECK(!seenAtDone(g).leftovers && seenAtDone(g).d19 == fs::last_write_time(out / "many" / "d19"));
    }

    // The viewer's file goes to the sharer.
    {
        const auto [sentBack, gotBack] = pair.toSharer({out / "photos" / "sub" / "b.txt"}, shareIn,
                                                       RCS_FILE_CONFLICT_FAIL);
        CHECK(fileEvents<rcs_file_offer_event>(pair.shareEvents, RCS_FILE_OFFER).back().first == 1);
        CHECK(waitDone(pair.shareEvents, gotBack).end == RCS_FILE_COMPLETED &&
              waitDone(pair.viewEvents, sentBack).end == RCS_FILE_COMPLETED);
        CHECK(readText(shareIn / "b.txt") == "bee" && transferInfo(share, gotBack).viewer_id == 1);
    }

    // Names taken: renamed, skipped, or the whole transfer fails before it starts.
    {
        const auto [s1, g1] = pair.toViewer({out / "notes.txt"}, viewIn, RCS_FILE_CONFLICT_RENAME);
        CHECK(waitDone(pair.viewEvents, g1).end == RCS_FILE_COMPLETED);
        CHECK(readText(viewIn / "notes (1).txt") == notes &&
              std::string(transferItems(view, g1)[0].saved_as) == "notes (1).txt");
        const auto [s2, g2] = pair.toViewer({out / "notes.txt"}, viewIn, RCS_FILE_CONFLICT_SKIP);
        const auto skipped = waitDone(pair.viewEvents, g2);
        CHECK(skipped.end == RCS_FILE_COMPLETED && skipped.items_skipped == 1 && !fs::exists(viewIn / "notes (2).txt"));
        CHECK(waitDone(pair.shareEvents, s2).items_skipped == 1);
        writeText(viewIn / "photos" / "sub" / "b.txt", "changed here");
        const auto [s3, g3] = pair.toViewer({out / "photos"}, viewIn, RCS_FILE_CONFLICT_OVERWRITE);
        CHECK(waitDone(pair.viewEvents, g3).end == RCS_FILE_COMPLETED && readText(viewIn / "photos" / "sub" / "b.txt") == "bee");
        const auto [s4, g4] = pair.toViewer({out / "notes.txt"}, viewIn, RCS_FILE_CONFLICT_FAIL);
        const auto failed = waitDone(pair.viewEvents, g4);
        CHECK(failed.end == RCS_FILE_FAILED && failed.result == RCS_ERROR_INVALID_STATE && !failed.by_peer &&
              std::string(failed.message).find("on_conflict FAIL") != std::string::npos);
        const auto told = waitDone(pair.shareEvents, s4);
        CHECK(told.end == RCS_FILE_FAILED && told.by_peer && told.result == RCS_ERROR_INVALID_STATE);
    }

    // Rejected with the host's reason; not decided in time.
    {
        const uint64_t s = sendPaths(share, 1, {out / "notes.txt"});
        const uint64_t g = waitOffer(pair.viewEvents, pair.viewOffers).transfer;
        CHECK(rcs_file_reject(view.handle(), g, "not now") == RCS_OK);
        CHECK(rcs_file_reject(view.handle(), g, "again") == RCS_ERROR_INVALID_STATE);
        const auto rejected = waitDone(pair.shareEvents, s);
        CHECK(rejected.end == RCS_FILE_REJECTED && rejected.result == RCS_ERROR_CANCELLED && rejected.by_peer &&
              std::string(rejected.message) == "not now");
        viewFiles.offer_timeout_ms = 1000;
        CHECK(updateFiles(view, viewFiles) == RCS_OK);
        const uint64_t late = sendPaths(share, 1, {out / "notes.txt"});
        const uint64_t lateGot = waitOffer(pair.viewEvents, pair.viewOffers).transfer;
        const auto timedOut = waitDone(pair.shareEvents, late, 5s);
        CHECK(timedOut.end == RCS_FILE_REJECTED && timedOut.result == RCS_ERROR_TIMEOUT && timedOut.by_peer);
        const auto here = waitDone(pair.viewEvents, lateGot);
        CHECK(here.end == RCS_FILE_REJECTED && here.result == RCS_ERROR_TIMEOUT && !here.by_peer);
        CHECK(acceptInto(view, lateGot, viewIn) == RCS_ERROR_INVALID_STATE);
    }

    // What the other end does not allow, and this end's own flow and limits.
    viewFiles.flow = RCS_FILE_SEND;
    CHECK(updateFiles(view, viewFiles) == RCS_OK);
    CHECK(waitFor([&] { return filePeer(share, 1).flow == RCS_FILE_SEND; }));
    CHECK(!fileEvents<rcs_file_peer>(pair.shareEvents, RCS_FILE_PEER).empty() &&
          fileEvents<rcs_file_peer>(pair.shareEvents, RCS_FILE_PEER).back().second.flow == RCS_FILE_SEND);
    CHECK(offerPaths(share, 1, {out / "notes.txt"}, &transfer) == RCS_ERROR_INVALID_STATE &&
          rcsdk::lastCallError().find("does not receive") != std::string::npos);
    viewFiles.flow = RCS_FILE_RECEIVE;
    viewFiles.max_items = 2;
    CHECK(updateFiles(view, viewFiles) == RCS_OK);
    CHECK(waitFor([&] { return filePeer(share, 1).flow == RCS_FILE_RECEIVE && filePeer(share, 1).max_items == 2; }));
    CHECK(offerPaths(view, 0, {out / "notes.txt"}, &transfer) == RCS_ERROR_INVALID_STATE &&
          rcsdk::lastCallError().find("RCS_FILE_SEND") != std::string::npos);
    CHECK(offerPaths(share, 1, {out / "notes.txt", out / "photos", out / "photos" / "a.jpg"}, &transfer) ==
              RCS_ERROR_INVALID_ARGUMENT &&
          rcsdk::lastCallError().find("at most 2 items") != std::string::npos);
    {
        // A directory of more items than the other end takes: refused once it is listed.
        const uint64_t many = sendPaths(share, 1, {out / "photos"});
        const auto done = waitDone(pair.shareEvents, many);
        CHECK(done.end == RCS_FILE_FAILED && done.result == RCS_ERROR_INVALID_ARGUMENT && !done.by_peer);
    }
    // Arguments.
    CHECK(offerPaths(share, 0, {out / "notes.txt"}, &transfer) == RCS_ERROR_INVALID_ARGUMENT); // Which viewer?
    CHECK(offerPaths(share, 1, {fs::path("relative.txt")}, &transfer) == RCS_ERROR_INVALID_ARGUMENT);
    {
        const std::string path = utf8Of(out / "notes.txt");
        rcs_file_item item{};
        item.path = path.c_str();
        item.name = "../escape.txt";
        rcs_file_offer offer = RCS_FILE_OFFER_INIT;
        offer.viewer_id = 1;
        offer.count = 1;
        offer.items = &item;
        CHECK(rcs_file_send(share.handle(), &offer, &transfer) == RCS_ERROR_INVALID_ARGUMENT);
        item.name = "CON.txt";
        CHECK(rcs_file_send(share.handle(), &offer, &transfer) == RCS_ERROR_INVALID_ARGUMENT);
        item.name = nullptr;
        item.size = 5; // SYSTEM: the file system says.
        CHECK(rcs_file_send(share.handle(), &offer, &transfer) == RCS_ERROR_INVALID_ARGUMENT);
        item.size = 0;
        offer.item_size = 4;
        CHECK(rcs_file_send(share.handle(), &offer, &transfer) == RCS_ERROR_INVALID_ARGUMENT);
        offer.struct_size = 4;
        CHECK(rcs_file_send(share.handle(), &offer, &transfer) == RCS_ERROR_INVALID_ARGUMENT);
    }
    CHECK(rcs_file_cancel(share.handle(), 12345) == RCS_ERROR_INVALID_STATE);
    CHECK(rcs_file_pause(share.handle(), sent, 2) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_file_pause(share.handle(), sent, 1) == RCS_ERROR_INVALID_STATE); // Finished.
    rcs_file_transfer_info unknown = RCS_FILE_TRANSFER_INFO_INIT;
    CHECK(rcs_file_get_transfer(share.handle(), 12345, &unknown) == RCS_ERROR_INVALID_STATE);
    rcs_file_peer nobody = RCS_FILE_PEER_INIT;
    CHECK(rcs_file_get_peer(share.handle(), 7, &nobody) == RCS_ERROR_INVALID_STATE);
    CHECK(rcs_file_get_peer(share.handle(), 1, nullptr) == RCS_ERROR_INVALID_ARGUMENT);
    {
        // Bad settings are refused with why.
        auto bad = fileSettings(bothWays);
        bad.max_items = 0;
        CHECK(updateFiles(view, bad) == RCS_ERROR_INVALID_ARGUMENT);
        bad = fileSettings(RCS_FILE_RECEIVE);
        bad.target = RCS_FILE_HOST; // Without write.
        CHECK(updateFiles(view, bad) == RCS_ERROR_INVALID_ARGUMENT &&
              rcsdk::lastCallError().find("needs write") != std::string::npos);
    }
    // A path not there: listed on the machine, then ended with why.
    {
        const uint64_t missing = sendPaths(share, 1, {out / "missing.txt"});
        const auto done = waitDone(pair.shareEvents, missing);
        CHECK(done.end == RCS_FILE_FAILED && done.result == RCS_ERROR_INVALID_ARGUMENT &&
              std::string(done.message).find("missing.txt") != std::string::npos);
    }
    // Too little space where it goes.
    {
        const std::string into = utf8Of(viewIn);
        rcsdk::file_transfer::files::setFreeSpace([into](const std::string& directory) {
            return directory == into ? std::optional<uint64_t>(1000) : std::nullopt;
        });
        const auto [s, g] = pair.toViewer({out / "notes.txt"}, viewIn);
        const auto here = waitDone(pair.viewEvents, g);
        CHECK(here.end == RCS_FILE_FAILED && here.result == RCS_ERROR_DEVICE &&
              std::string(here.message).find("Not enough space") != std::string::npos);
        const auto there = waitDone(pair.shareEvents, s);
        CHECK(there.end == RCS_FILE_FAILED && there.by_peer && there.result == RCS_ERROR_DEVICE);
        // Why, without where on the other machine (SA-16): its directories are its own.
        CHECK(std::string(there.message).find("Not enough space") != std::string::npos &&
              std::string(there.message).find(utf8Of(scratch.path)) == std::string::npos);
        rcsdk::file_transfer::files::setFreeSpace({});
    }
    CHECK(!leftovers(viewIn) && !leftovers(shareIn));
    {
        std::lock_guard lock(seenMutex);
        CHECK(seen.size() == 9 &&
              std::none_of(seen.begin(), seen.end(), [](const auto& s) { return s.second.leftovers; }));
    }
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    CHECK(!pair.shareEvents.overlapping && !pair.viewEvents.overlapping);
}

// Pausing from either end and going on; cancelling from either end (the
// partial file goes), also before a decision; the rate limit.
void fileTransferControl(const uint8_t (&key)[32]) {
    Scratch scratch("control");
    const fs::path out = scratch / "out", viewIn = scratch / "view-in";
    fs::create_directories(viewIn);
    const std::string big = pattern(6u << 20, 3);
    writeText(out / "big.bin", big);
    constexpr uint64_t rate = 3'000'000;
    auto shareFiles = fileSettings(bothWays);
    shareFiles.max_bytes_per_second = rate;
    FilePair pair(config(RCS_ROLE_SHARE, 47971, key), config(RCS_ROLE_VIEW, 47971, key), shareFiles,
                  fileSettings(bothWays));
    auto& share = *pair.share;
    auto& view = *pair.view;
    // Whether the viewer's directory held partial files or records as each
    // RCS_FILE_DONE of a transfer it received reached its host.
    std::mutex seenMutex;
    std::map<uint64_t, bool> leftAtDone;
    pair.viewHook = [&](const rcs_event& event) {
        if (const auto* done = receivedDone(event)) {
            const bool left = leftovers(viewIn);
            std::lock_guard lock(seenMutex);
            leftAtDone[done->transfer] = left;
        }
    };
    const auto cleanAtDone = [&](uint64_t transfer) {
        std::lock_guard lock(seenMutex);
        return leftAtDone.count(transfer) && !leftAtDone[transfer];
    };
    pair.connect();

    // Paused by the receiver: nothing more is written until it goes on.
    const auto started = std::chrono::steady_clock::now();
    const auto [sent, got] = pair.toViewer({out / "big.bin"}, viewIn);
    CHECK(waitFor([&] { return transferInfo(view, got).done_bytes > (512u << 10); }));
    CHECK(rcs_file_pause(view.handle(), got, 1) == RCS_OK);
    CHECK(waitFor([&] {
        const auto info = transferInfo(share, sent);
        return info.state == RCS_FILE_PAUSED && info.paused == RCS_FILE_PAUSED_THERE;
    }));
    CHECK(transferInfo(view, got).state == RCS_FILE_PAUSED && transferInfo(view, got).paused == RCS_FILE_PAUSED_HERE);
    std::this_thread::sleep_for(700ms); // What was on its way is written.
    const uint64_t held = transferInfo(view, got).done_bytes;
    std::this_thread::sleep_for(700ms);
    CHECK(transferInfo(view, got).done_bytes == held && held < big.size());
    CHECK(rcs_file_pause(view.handle(), got, 0) == RCS_OK);
    // Paused by the sender.
    CHECK(waitFor([&] { return transferInfo(view, got).done_bytes > held + (512u << 10); }));
    CHECK(rcs_file_pause(share.handle(), sent, 1) == RCS_OK);
    CHECK(waitFor([&] { return transferInfo(view, got).paused == RCS_FILE_PAUSED_THERE; }));
    std::this_thread::sleep_for(700ms);
    const uint64_t heldAgain = transferInfo(view, got).done_bytes;
    std::this_thread::sleep_for(700ms);
    CHECK(transferInfo(view, got).done_bytes == heldAgain && heldAgain < big.size());
    CHECK(rcs_file_pause(share.handle(), sent, 0) == RCS_OK);
    CHECK(waitDone(pair.viewEvents, got, 20s).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.shareEvents, sent).end == RCS_FILE_COMPLETED);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(readText(viewIn / "big.bin") == big && cleanAtDone(got));
    {
        // The state changes were told; progress at most every 250 ms, at about the rate limit.
        const auto states = fileEvents<rcs_file_state_event>(pair.shareEvents, RCS_FILE_STATE);
        CHECK(std::count_if(states.begin(), states.end(),
                            [&](const auto& e) { return e.second.transfer == sent && e.second.state == RCS_FILE_PAUSED; }) == 2);
        uint64_t fastest = 0;
        size_t progress = 0;
        for (const auto& [viewer, e] : fileEvents<rcs_file_progress_event>(pair.shareEvents, RCS_FILE_PROGRESS)) {
            if (e.transfer == sent) {
                ++progress;
                fastest = std::max(fastest, e.bytes_per_second);
                CHECK(e.total_bytes == big.size() && e.done_bytes <= big.size());
            }
        }
        std::printf("file_transfer_control: %.1f s with two pauses, %zu progress events, at most %llu bytes/s\n",
                    seconds, progress, static_cast<unsigned long long>(fastest));
        CHECK(progress >= 3 && double(progress) <= seconds * 4 + 2);
        CHECK(fastest > 0 && fastest < rate * 3 / 2);
        CHECK(seconds > double(big.size()) / double(rate) * 0.9);
    }

    // Cancelled by the sender midway: the receiver's partial file goes.
    {
        const auto [s, g] = pair.toViewer({out / "big.bin"}, viewIn);
        CHECK(waitFor([&] { return transferInfo(view, g).done_bytes > (512u << 10); }));
        CHECK(partials(viewIn, "big.bin").size() == 1);
        CHECK(rcs_file_cancel(share.handle(), s) == RCS_OK);
        const auto here = waitDone(pair.shareEvents, s);
        CHECK(here.end == RCS_FILE_CANCELLED && !here.by_peer && here.result == RCS_ERROR_CANCELLED);
        const auto there = waitDone(pair.viewEvents, g);
        CHECK(there.end == RCS_FILE_CANCELLED && there.by_peer && !there.partial_kept);
        CHECK(cleanAtDone(g) && !leftovers(viewIn)); // Its partial file and record went before DONE.
        CHECK(rcs_file_cancel(share.handle(), s) == RCS_ERROR_INVALID_STATE);
        CHECK(readText(viewIn / "big.bin") == big); // The earlier one stays.
    }
    // Cancelled by the receiver midway.
    {
        const auto [s, g] = pair.toViewer({out / "big.bin"}, viewIn);
        CHECK(waitFor([&] { return transferInfo(view, g).done_bytes > (512u << 10); }));
        CHECK(rcs_file_cancel(view.handle(), g) == RCS_OK);
        const auto here = waitDone(pair.viewEvents, g);
        CHECK(here.end == RCS_FILE_CANCELLED && !here.by_peer && !here.partial_kept);
        CHECK(cleanAtDone(g) && !leftovers(viewIn));
        CHECK(waitDone(pair.shareEvents, s).end == RCS_FILE_CANCELLED && waitDone(pair.shareEvents, s).by_peer);
    }
    // Cancelled by the sender before the receiver decided.
    {
        const uint64_t s = sendPaths(share, 1, {out / "big.bin"});
        const uint64_t g = waitOffer(pair.viewEvents, pair.viewOffers).transfer;
        CHECK(rcs_file_cancel(share.handle(), s) == RCS_OK);
        const auto there = waitDone(pair.viewEvents, g);
        CHECK(there.end == RCS_FILE_CANCELLED && there.by_peer);
        CHECK(acceptInto(view, g, viewIn) == RCS_ERROR_INVALID_STATE);
    }
    // Two files of the same name into one directory at once: each writes a
    // partial file of its own, and both arrive whole (the second renamed).
    {
        const fs::path twins = scratch / "twins";
        fs::create_directories(twins);
        const std::string one = pattern(2u << 20, 7), other = pattern(2u << 20, 8);
        writeText(out / "one" / "twin.bin", one);
        writeText(out / "other" / "twin.bin", other);
        const auto [s1, g1] = pair.toViewer({out / "one" / "twin.bin"}, twins);
        const auto [s2, g2] = pair.toViewer({out / "other" / "twin.bin"}, twins);
        CHECK(waitFor([&] { return partials(twins, "twin.bin").size() == 2; })); // Both under way.
        CHECK(waitDone(pair.viewEvents, g1, 30s).end == RCS_FILE_COMPLETED);
        CHECK(waitDone(pair.viewEvents, g2, 30s).end == RCS_FILE_COMPLETED);
        CHECK(waitDone(pair.shareEvents, s1).end == RCS_FILE_COMPLETED &&
              waitDone(pair.shareEvents, s2).end == RCS_FILE_COMPLETED);
        const std::set<std::string> got{readText(twins / "twin.bin"), readText(twins / "twin (1).bin")};
        CHECK((got == std::set<std::string>{one, other}) && !leftovers(twins));
    }
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// Offers straight through the module's messages, as an end of anyone's
// making could send them (rcs_module_send): `count` of them from wire number
// `first` on, each of `items` files of a byte, with `token` (null: each its own).
void rawOffers(rcsdk::Session& session, uint64_t first, uint32_t count, uint32_t items,
               const uint8_t* token = nullptr) {
    namespace ft = rcsdk::file_transfer;
    std::vector<ft::Bytes> messages;
    for (uint32_t k = 0; k < count; ++k) {
        ft::Offer offer;
        offer.id = first + k;
        uint8_t random[32];
        CHECK(rcs_generate_connection_key(random, sizeof(random)) == RCS_OK);
        std::copy(random, random + offer.token.size(), offer.token.begin());
        if (token) {
            std::copy(token, token + offer.token.size(), offer.token.begin());
        }
        for (uint32_t i = 0; i < items; ++i) {
            offer.items.push_back({RCS_FILE_REGULAR, 1, 0, "flood-" + std::to_string(k) + "/" + std::to_string(i)});
        }
        messages.push_back(ft::encode(ft::PeerMessage{offer}));
    }
    rcs_module module = nullptr;
    CHECK(rcs_module_find(session.handle(), RCS_FILE_TRANSFER_MODULE, &module) == RCS_OK);
    CHECK(rcs_module_run(
              module,
              [](rcs_module m, void* argument) -> rcs_result {
                  for (const auto& bytes : *static_cast<std::vector<ft::Bytes>*>(argument)) {
                      if (const rcs_result sent = rcs_module_send(m, 0, bytes.data(), uint32_t(bytes.size()), 0, 0);
                          sent != RCS_OK) {
                          return sent;
                      }
                  }
                  return RCS_OK;
              },
              &messages) == RCS_OK);
}

// A viewer floods the sharer with offers its host does not decide on (found
// in a review: 200 offers of 500 items each, which all reached the host and took
// memory until they timed out). Beyond the sharer's budget for offers waiting
// for its host (by default 20 000 items from one peer) they are refused at
// once, without RCS_FILE_OFFER; the connection and the picture go on, and
// once the host decides, offers within the budget go as before. The budget's
// other limits (offers from one peer, items from all) and their settings;
// two offers of one token are never accepted at once.
void fileTransferPending(const uint8_t (&key)[32]) {
    Scratch scratch("pending");
    const fs::path out = scratch / "out", shareIn = scratch / "share-in";
    fs::create_directories(shareIn);
    writeText(out / "note.txt", "within the budget");
    FilePair pair(config(RCS_ROLE_SHARE, 47977, key), config(RCS_ROLE_VIEW, 47977, key),
                  fileSettings(RCS_FILE_RECEIVE), fileSettings(RCS_FILE_SEND));
    auto& share = *pair.share;
    auto& view = *pair.view;
    pair.connect();
    const auto offered = [&] { return fileEvents<rcs_file_offer_event>(pair.shareEvents, RCS_FILE_OFFER); };

    rawOffers(view, 1'000'000, 200, 500);
    // The host's own offer after them: its answer comes after the flood's.
    const uint64_t late = sendPaths(view, 0, {out / "note.txt"});
    const auto refused = waitDone(pair.viewEvents, late);
    CHECK(refused.end == RCS_FILE_REJECTED && refused.result == RCS_ERROR_BUSY && refused.by_peer &&
          std::string(refused.message).find("max_pending_items") != std::string::npos);
    CHECK(waitFor([&] { return offered().size() == 40; }));
    CHECK(sessionLog().contains("Refused an offer") && sessionLog().contains("max_pending_items"));
    const int frames = pair.frames;
    CHECK(waitFor([&] { return pair.frames > frames + 10; })); // The picture goes on.
    CHECK(view.state() == RCS_STATE_RUNNING && share.viewers().size() == 1 && offered().size() == 40);

    // Decided (here rejected), they count no more: an offer goes as before.
    for (const auto& [viewer, offer] : offered()) {
        CHECK(viewer == 1 && offer.items == 500 && rcs_file_reject(share.handle(), offer.transfer, nullptr) == RCS_OK);
    }
    pair.shareOffers = 40;
    const auto [sent, got] = pair.toSharer({out / "note.txt"}, shareIn);
    CHECK(waitDone(pair.shareEvents, got).end == RCS_FILE_COMPLETED &&
          waitDone(pair.viewEvents, sent).end == RCS_FILE_COMPLETED);
    CHECK(readText(shareIn / "note.txt") == "within the budget");
    size_t seen = 41;
    // The offers that come next, from the `seen` on.
    const auto newOffers = [&](size_t count) {
        CHECK(waitFor([&] { return offered().size() == seen + count; }));
        std::vector<rcs_file_offer_event> next;
        for (size_t k = seen; k < seen + count; ++k) {
            next.push_back(offered()[k].second);
        }
        seen += count;
        pair.shareOffers = seen;
        return next;
    };
    // The host's own offer is refused with `limit` (all offered before it came first).
    const auto refusedWith = [&](const std::string& limit) {
        const auto done = waitDone(pair.viewEvents, sendPaths(view, 0, {out / "note.txt"}));
        CHECK(done.end == RCS_FILE_REJECTED && done.result == RCS_ERROR_BUSY &&
              std::string(done.message).find(limit) != std::string::npos);
    };

    // Two offers of one token: one at a time is accepted, the other once it ended.
    {
        const uint8_t token[RCS_FILE_TOKEN_SIZE] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
        rawOffers(view, 2'000'000, 2, 1, token);
        const auto twins = newOffers(2);
        CHECK(std::memcmp(twins[0].token, twins[1].token, RCS_FILE_TOKEN_SIZE) == 0);
        CHECK(acceptInto(share, twins[0].transfer, shareIn) == RCS_OK);
        CHECK(acceptInto(share, twins[1].transfer, shareIn) == RCS_ERROR_INVALID_STATE &&
              rcsdk::lastCallError().find("same token") != std::string::npos);
        CHECK(rcs_file_cancel(share.handle(), twins[0].transfer) == RCS_OK);
        CHECK(waitDone(pair.shareEvents, twins[0].transfer).end == RCS_FILE_CANCELLED);
        CHECK(acceptInto(share, twins[1].transfer, shareIn) == RCS_OK);
        CHECK(rcs_file_cancel(share.handle(), twins[1].transfer) == RCS_OK);
        CHECK(waitDone(pair.shareEvents, twins[1].transfer).end == RCS_FILE_CANCELLED);
    }
    // At most 3 offers from one end; accepting one makes room.
    auto shareFiles = fileSettings(RCS_FILE_RECEIVE);
    shareFiles.max_pending_offers = 3;
    CHECK(updateFiles(share, shareFiles) == RCS_OK);
    rawOffers(view, 3'000'000, 5, 1);
    refusedWith("max_pending_offers");
    {
        const auto three = newOffers(3);
        CHECK(acceptInto(share, three[0].transfer, shareIn) == RCS_OK);
        const auto [s, g] = pair.toSharer({out / "note.txt"}, shareIn);
        CHECK(waitDone(pair.shareEvents, g).end == RCS_FILE_COMPLETED);
        seen = pair.shareOffers;
        for (const auto& offer : three) {
            CHECK(rcs_file_cancel(share.handle(), offer.transfer) == RCS_OK);
        }
    }
    // At most 1 500 items from all ends, fewer than from one: the offers that
    // did not come in time (TIMEOUT) count no more.
    shareFiles.max_pending_offers = 100;
    shareFiles.max_items = 500;
    shareFiles.max_pending_items = 2000;
    shareFiles.max_pending_items_total = 1500;
    shareFiles.offer_timeout_ms = 2000;
    CHECK(updateFiles(share, shareFiles) == RCS_OK);
    rawOffers(view, 4'000'000, 5, 500);
    refusedWith("max_pending_items_total");
    for (const auto& offer : newOffers(3)) {
        CHECK(waitDone(pair.shareEvents, offer.transfer).result == RCS_ERROR_TIMEOUT);
    }
    {
        const auto [s, g] = pair.toSharer({out / "note.txt"}, shareIn);
        CHECK(waitDone(pair.shareEvents, g).end == RCS_FILE_COMPLETED);
    }
    // Settings out of range, with why.
    auto bad = fileSettings(RCS_FILE_RECEIVE);
    bad.max_pending_offers = 0;
    CHECK(updateFiles(share, bad) == RCS_ERROR_INVALID_ARGUMENT &&
          rcsdk::lastCallError().find("max_pending_offers") != std::string::npos);
    bad = fileSettings(RCS_FILE_RECEIVE);
    bad.max_items = 50'000; // An offer of max_items would never fit.
    CHECK(updateFiles(share, bad) == RCS_ERROR_INVALID_ARGUMENT &&
          rcsdk::lastCallError().find("max_pending_items must be max_items") != std::string::npos);
    bad.max_pending_items = 50'000;
    bad.max_pending_items_total = 1'000'001;
    CHECK(updateFiles(share, bad) == RCS_ERROR_INVALID_ARGUMENT &&
          rcsdk::lastCallError().find("max_pending_items_total") != std::string::npos);
    bad.max_pending_items_total = 1'000'000;
    CHECK(updateFiles(share, bad) == RCS_OK);
    CHECK(view.state() == RCS_STATE_RUNNING);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// A sanitizer slows the data path several times: there the large file is a quarter.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define RCSDK_TEST_SANITIZED 1
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define RCSDK_TEST_SANITIZED 1
#endif
#ifdef RCSDK_TEST_SANITIZED
constexpr uint64_t largeFileBytes = 256ull << 20;
#else
constexpr uint64_t largeFileBytes = 1ull << 30;
#endif

// A large file (1 GiB) goes whole, beside the picture (which goes on), its
// progress at most every 250 ms.
void fileTransferLarge(const uint8_t (&key)[32]) {
    Scratch scratch("large");
    const fs::path out = scratch / "out", viewIn = scratch / "view-in";
    fs::create_directories(viewIn);
    constexpr uint64_t size = largeFileBytes;
    LargePattern::make(out / "large.bin", size);
    FilePair pair(config(RCS_ROLE_SHARE, 47972, key), config(RCS_ROLE_VIEW, 47972, key), fileSettings(RCS_FILE_SEND),
                  fileSettings(RCS_FILE_RECEIVE));
    pair.connect();
    CHECK(waitFor([&] { return pair.frames >= 10; }));
    const auto started = std::chrono::steady_clock::now();
    const int framesBefore = pair.frames;
    const auto [sent, got] = pair.toViewer({out / "large.bin"}, viewIn);
    const auto done = waitDone(pair.viewEvents, got, 160s);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const int frames = pair.frames - framesBefore;
    CHECK(done.end == RCS_FILE_COMPLETED && waitDone(pair.shareEvents, sent).end == RCS_FILE_COMPLETED);
    CHECK(LargePattern::holds(viewIn / "large.bin", size));
    const auto info = transferInfo(*pair.share, sent);
    CHECK(info.transferred_bytes == size && info.done_bytes == size);
    size_t progress = 0;
    for (const auto& [viewer, e] : fileEvents<rcs_file_progress_event>(pair.viewEvents, RCS_FILE_PROGRESS)) {
        progress += e.transfer == got ? 1 : 0;
    }
    std::printf("file_transfer_large: %llu MiB in %.1f s (%.0f MB/s), %d frames meanwhile, %zu progress events\n",
                static_cast<unsigned long long>(size >> 20), seconds, double(size) / seconds / 1e6, frames, progress);
    CHECK(progress >= 1 && double(progress) <= seconds * 4 + 2);
    CHECK(frames >= int(seconds * 5)); // The picture did not wait for the file.
    CHECK(pair.view->stop() == RCS_OK);
    CHECK(pair.share->stop() == RCS_OK);
}

// The path breaks in the middle of a transfer each way: both wait, and once
// the viewer is back (a new connection without migration, a new viewer
// number at the sharer) they go on from what is written.
void fileTransferReconnect(const uint8_t (&key)[32]) {
    Scratch scratch("reconnect");
    const fs::path out = scratch / "out", shareIn = scratch / "share-in", viewIn = scratch / "view-in";
    fs::create_directories(shareIn);
    fs::create_directories(viewIn);
    const std::string down = pattern(6u << 20, 4), up = pattern(5u << 20, 5);
    writeText(out / "down.bin", down);
    writeText(out / "up.bin", up);
    auto files = fileSettings(bothWays);
    files.max_bytes_per_second = 2'000'000;
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47973));
    FilePair pair(config(RCS_ROLE_SHARE, 47973, key).setKeepListening(true),
                  reconnecting(config(RCS_ROLE_VIEW, proxy.address().port(), key), 30000, 500).setMigrate(false), files,
                  files);
    auto& share = *pair.share;
    auto& view = *pair.view;
    pair.connect();
    const auto [downSent, downGot] = pair.toViewer({out / "down.bin"}, viewIn);
    const auto [upSent, upGot] = pair.toSharer({out / "up.bin"}, shareIn);
    CHECK(waitFor([&] {
        return transferInfo(view, downGot).done_bytes > (1u << 20) && transferInfo(share, upGot).done_bytes > (1u << 20);
    }));
    proxy.outage = true;
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RECONNECTING; }, 8s));
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 8s));
    CHECK(waitFor([&] {
        return transferInfo(share, downSent).state == RCS_FILE_WAITING &&
               transferInfo(view, downGot).state == RCS_FILE_WAITING &&
               transferInfo(share, upGot).state == RCS_FILE_WAITING &&
               transferInfo(view, upSent).state == RCS_FILE_WAITING;
    }));
    const uint64_t downHad = transferInfo(view, downGot).done_bytes, upHad = transferInfo(share, upGot).done_bytes;
    CHECK(downHad < down.size() && upHad < up.size());
    proxy.outage = false;
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING; }, 15s));
    CHECK(waitDone(pair.viewEvents, downGot, 30s).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.shareEvents, downSent).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.shareEvents, upGot, 30s).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.viewEvents, upSent).end == RCS_FILE_COMPLETED);
    CHECK(readText(viewIn / "down.bin") == down && readText(shareIn / "up.bin") == up && !leftovers(viewIn) &&
          !leftovers(shareIn));
    // It went on: what was written before the break did not go again; what
    // was on its way (at most the transfer's window, 4 MiB) did.
    const auto downInfo = transferInfo(share, downSent), upInfo = transferInfo(view, upSent);
    std::printf("file_transfer_reconnect: down %llu of %zu bytes written before the break, %llu sent in all; up %llu "
                "of %zu, %llu sent\n",
                static_cast<unsigned long long>(downHad), down.size(),
                static_cast<unsigned long long>(downInfo.transferred_bytes), static_cast<unsigned long long>(upHad),
                up.size(), static_cast<unsigned long long>(upInfo.transferred_bytes));
    CHECK(downInfo.transferred_bytes <= down.size() + (4u << 20) && upInfo.transferred_bytes <= up.size() + (4u << 20));
    CHECK(transferInfo(share, downSent).viewer_id == 2 && transferInfo(share, upGot).viewer_id == 2);
    {
        const auto states = fileEvents<rcs_file_state_event>(pair.viewEvents, RCS_FILE_STATE);
        CHECK(std::any_of(states.begin(), states.end(), [&](const auto& e) {
            return e.second.transfer == downGot && e.second.state == RCS_FILE_WAITING;
        }));
    }
    CHECK(share.stop() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_FAILED; }, 3s));
}

// Another run goes on where one stopped: the viewer's run ends in the middle
// of a transfer (its partial file and the record stay); the sharer offers
// the same items with the transfer's token, the viewer accepts into the same
// directory, and only what was missing goes. Discarding what was kept.
void fileTransferResume(const uint8_t (&key)[32]) {
    Scratch scratch("resume");
    const fs::path out = scratch / "out", viewIn = scratch / "view-in";
    fs::create_directories(viewIn);
    const std::string brief = pattern(1000, 6), big = pattern(6u << 20, 7);
    writeText(out / "set" / "a-small.txt", brief);
    writeText(out / "set" / "b-big.bin", big);
    auto shareFiles = fileSettings(RCS_FILE_SEND);
    shareFiles.max_bytes_per_second = 3'000'000;
    shareFiles.reconnect_wait_ms = 500;
    FilePair pair(config(RCS_ROLE_SHARE, 47974, key).setKeepListening(true), config(RCS_ROLE_VIEW, 47974, key),
                  shareFiles, fileSettings(RCS_FILE_RECEIVE));
    auto& share = *pair.share;
    auto& view = *pair.view;
    pair.connect();
    const auto [first, firstGot] = pair.toViewer({out / "set"}, viewIn);
    CHECK(waitFor([&] { return transferInfo(view, firstGot).done_bytes > (1u << 20); }));
    CHECK(view.stop() == RCS_OK); // The run ends midway.
    {
        const auto here = waitDone(pair.viewEvents, firstGot);
        CHECK(here.end == RCS_FILE_FAILED && here.result == RCS_ERROR_DISCONNECTED && here.partial_kept == 1 &&
              here.items_completed == 2); // The directory and the small file.
        const auto there = waitDone(pair.shareEvents, first);
        CHECK(there.end == RCS_FILE_FAILED && there.result == RCS_ERROR_DISCONNECTED && !there.by_peer);
    }
    CHECK(partials(viewIn / "set", "b-big.bin").size() == 1);
    const auto kept = fs::file_size(partials(viewIn / "set", "b-big.bin").at(0));
    CHECK(kept > 0 && kept < big.size() && readText(viewIn / "set" / "a-small.txt") == brief && leftovers(viewIn));
    const auto firstInfo = transferInfo(share, first);
    const uint8_t* token = firstInfo.token;
    {
        rcsdk::file_transfer::Token whole{};
        std::copy(token, token + whole.size(), whole.begin());
        CHECK(utf8Of(partials(viewIn / "set", "b-big.bin").at(0).filename()) ==
              "b-big.bin." + rcsdk::file_transfer::hex(whole) + ".rcsdk-part"); // All of the token.
    }

    // The next run: the same items with the token; the viewer's host knows
    // the token and accepts into the same directory.
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING && filePeer(share, 2).max_items; }));
    uint64_t again = 0;
    CHECK(offerPaths(share, 2, {out / "set"}, &again, token) == RCS_OK);
    const auto offer = waitOffer(pair.viewEvents, pair.viewOffers);
    CHECK(offer.resuming == 1 && std::memcmp(offer.token, token, RCS_FILE_TOKEN_SIZE) == 0);
    CHECK(acceptInto(*pair.view, offer.transfer, viewIn) == RCS_OK);
    CHECK(waitDone(pair.viewEvents, offer.transfer, 20s).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.shareEvents, again).end == RCS_FILE_COMPLETED);
    CHECK(readText(viewIn / "set" / "b-big.bin") == big && readText(viewIn / "set" / "a-small.txt") == brief);
    CHECK(!fs::exists(viewIn / "set" / "a-small (1).txt") && !leftovers(viewIn));
    {
        const auto here = transferInfo(view, offer.transfer), there = transferInfo(share, again);
        std::printf("file_transfer_resume: %llu bytes kept, %llu resumed, %llu sent again\n",
                    static_cast<unsigned long long>(kept), static_cast<unsigned long long>(there.resumed_bytes),
                    static_cast<unsigned long long>(there.transferred_bytes));
        CHECK(here.resuming && there.resuming && here.resumed_bytes == kept + brief.size() &&
              there.resumed_bytes == here.resumed_bytes);
        CHECK(there.transferred_bytes == big.size() - kept);
        const auto items = transferItems(view, offer.transfer);
        CHECK(items[1].state == RCS_FILE_ITEM_COMPLETED &&
              std::string(items[1].message).find("earlier run") != std::string::npos);
    }
    // A token in use is refused.
    CHECK(offerPaths(share, 2, {out / "set"}, &again, token) == RCS_OK);
    const auto busy = waitOffer(pair.viewEvents, pair.viewOffers);
    CHECK(offerPaths(share, 2, {out / "set"}, nullptr, token) == RCS_ERROR_INVALID_STATE);
    CHECK(rcs_file_reject(view.handle(), busy.transfer, nullptr) == RCS_OK);

    // Stopped midway again, and what was kept discarded instead.
    fs::remove_all(viewIn / "set");
    const auto [third, thirdGot] = pair.toViewer({out / "set"}, viewIn, RCS_FILE_CONFLICT_RENAME, 2);
    CHECK(waitFor([&] { return transferInfo(view, thirdGot).done_bytes > (1u << 20); }));
    const auto thirdInfo = transferInfo(view, thirdGot);
    const uint8_t* thirdToken = thirdInfo.token;
    CHECK(rcs_file_discard(view.handle(), utf8Of(viewIn).c_str(), thirdToken) == RCS_ERROR_INVALID_STATE); // It goes.
    CHECK(view.stop() == RCS_OK);
    CHECK(waitDone(pair.viewEvents, thirdGot).partial_kept == 1 && leftovers(viewIn));
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING && share.viewers().empty(); }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING; }));
    CHECK(rcs_file_discard(view.handle(), "relative", thirdToken) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_file_discard(view.handle(), utf8Of(viewIn).c_str(), thirdToken) == RCS_OK);
    CHECK(waitFor([&] { return !fileEvents<rcs_file_discard_event>(pair.viewEvents, RCS_FILE_DISCARDED).empty(); }));
    const auto discarded = fileEvents<rcs_file_discard_event>(pair.viewEvents, RCS_FILE_DISCARDED).back().second;
    CHECK(discarded.result == RCS_OK && discarded.files == 1 &&
          std::memcmp(discarded.token, thirdToken, RCS_FILE_TOKEN_SIZE) == 0 && !leftovers(viewIn));
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// target HOST: the hosts' own storage (here memory), through their
// callbacks: what the sender's read gives arrives in the receiver's write,
// in order; a read that fails fails its item at both ends with the host's
// reason; the receiver's resume says what it holds of an earlier run.
struct MemoryStore {
    std::mutex mutex;
    std::map<std::string, std::string> files;      // Send: what each item holds.
    std::map<std::string, std::string> written;    // Receive: what came.
    std::map<std::string, uint64_t> held;          // Receive: what resume says.
    bool ordered = true;

    void into(rcs_file_transfer_settings& settings) {
        settings.target = RCS_FILE_HOST;
        settings.user_data = this;
        settings.read = [](const rcs_file_chunk* c, void* self) -> rcs_result {
            auto& store = *static_cast<MemoryStore*>(self);
            std::lock_guard lock(store.mutex);
            const auto found = store.files.find(c->name);
            if (found == store.files.end() || c->offset + c->size > found->second.size()) {
                return rcs_set_last_call_error(RCS_ERROR_DEVICE, "the host's storage lost it");
            }
            std::memcpy(c->data, found->second.data() + c->offset, c->size);
            return RCS_OK;
        };
        settings.write = [](const rcs_file_chunk* c, void* self) -> rcs_result {
            auto& store = *static_cast<MemoryStore*>(self);
            std::lock_guard lock(store.mutex);
            auto& text = store.written[c->name];
            const uint64_t start = store.held.count(c->name) ? store.held[c->name] : 0;
            store.ordered = store.ordered && c->offset == start + text.size() && c->struct_size == sizeof(*c);
            text.append(reinterpret_cast<const char*>(c->data), c->size);
            return RCS_OK;
        };
        settings.resume = [](const rcs_file_chunk* c, void* self) -> uint64_t {
            auto& store = *static_cast<MemoryStore*>(self);
            std::lock_guard lock(store.mutex);
            const auto found = store.held.find(c->name);
            return found == store.held.end() ? 0 : found->second;
        };
    }
};

void fileTransferHost(const uint8_t (&key)[32]) {
    MemoryStore sending, receiving;
    const std::string a = pattern(700000, 8), b = pattern(5000, 9);
    sending.files = {{"mem/a.bin", a}, {"mem/b.bin", b}};
    auto shareFiles = fileSettings(RCS_FILE_SEND), viewFiles = fileSettings(RCS_FILE_RECEIVE);
    sending.into(shareFiles);
    receiving.into(viewFiles);
    FilePair pair(config(RCS_ROLE_SHARE, 47975, key), config(RCS_ROLE_VIEW, 47975, key), shareFiles, viewFiles);
    auto& share = *pair.share;
    auto& view = *pair.view;
    pair.connect();
    const auto send = [&](const std::vector<rcs_file_item>& items, const uint8_t* token = nullptr) {
        rcs_file_offer offer = RCS_FILE_OFFER_INIT;
        offer.viewer_id = 1;
        offer.count = uint32_t(items.size());
        offer.items = items.data();
        if (token) {
            std::memcpy(offer.resume_token, token, RCS_FILE_TOKEN_SIZE);
        }
        uint64_t transfer = 0;
        CHECK(rcs_file_send(share.handle(), &offer, &transfer) == RCS_OK);
        return transfer;
    };
    const std::vector<rcs_file_item> items = {{nullptr, "mem", RCS_FILE_DIRECTORY, 0, 0},
                                              {nullptr, "mem/a.bin", RCS_FILE_REGULAR, a.size(), 77},
                                              {nullptr, "mem/lost.bin", RCS_FILE_REGULAR, 10, 0},
                                              {nullptr, "mem/b.bin", RCS_FILE_REGULAR, b.size(), 0},
                                              {nullptr, "mem/empty", RCS_FILE_REGULAR, 0, 0}};
    {
        // HOST items name themselves; a path is the file system's.
        std::vector<rcs_file_item> wrong = {{"/tmp/x", "x", RCS_FILE_REGULAR, 1, 0}};
        rcs_file_offer offer = RCS_FILE_OFFER_INIT;
        offer.viewer_id = 1;
        offer.count = 1;
        offer.items = wrong.data();
        uint64_t transfer = 0;
        CHECK(rcs_file_send(share.handle(), &offer, &transfer) == RCS_ERROR_INVALID_ARGUMENT);
        wrong = {{nullptr, "d", RCS_FILE_DIRECTORY, 5, 0}};
        offer.items = wrong.data();
        CHECK(rcs_file_send(share.handle(), &offer, &transfer) == RCS_ERROR_INVALID_ARGUMENT);
    }
    const uint64_t sent = send(items);
    const uint64_t got = waitOffer(pair.viewEvents, pair.viewOffers).transfer;
    CHECK(acceptInto(view, got, fs::temp_directory_path()) == RCS_ERROR_INVALID_ARGUMENT); // HOST: no directory.
    CHECK(rcs_file_accept(view.handle(), got, nullptr, RCS_FILE_CONFLICT_RENAME) == RCS_OK);
    const auto done = waitDone(pair.viewEvents, got);
    CHECK(done.end == RCS_FILE_COMPLETED && done.items_completed == 4 && done.items_failed == 1);
    CHECK(waitDone(pair.shareEvents, sent).items_failed == 1);
    {
        std::lock_guard lock(receiving.mutex);
        CHECK(receiving.written["mem/a.bin"] == a && receiving.written["mem/b.bin"] == b && receiving.ordered);
        CHECK(!receiving.written.count("mem/empty") && !receiving.written.count("mem/lost.bin"));
    }
    for (auto* session : {&share, &view}) {
        const uint64_t id = session == &share ? sent : got;
        const auto lost = transferItems(*session, id)[2];
        CHECK(lost.state == RCS_FILE_ITEM_FAILED && lost.result == RCS_ERROR_DEVICE &&
              std::string(lost.message) == "the host's storage lost it");
        CHECK(transferItems(*session, id)[1].modified_ns == 77 && transferInfo(*session, id).target == RCS_FILE_HOST);
    }
    CHECK(rcs_file_discard(view.handle(), utf8Of(fs::temp_directory_path()).c_str(), transferInfo(view, got).token) ==
          RCS_ERROR_INVALID_STATE);

    // Again with the token: the receiver holds 300 000 bytes of a.bin from
    // before; its host accepts in on_event, where it sees the offer.
    std::atomic<rcs_result> acceptedThere{RCS_ERROR_INTERNAL};
    pair.viewHook = [&](const rcs_event& event) {
        if (event.type == RCS_EVENT_MODULE && event.module.kind == RCS_FILE_OFFER) {
            const auto* offer = static_cast<const rcs_file_offer_event*>(event.module.payload);
            acceptedThere = rcs_file_accept(pair.view->handle(), offer->transfer, nullptr, RCS_FILE_CONFLICT_RENAME);
        }
    };
    {
        std::lock_guard lock(receiving.mutex);
        receiving.held["mem/a.bin"] = 300000;
        receiving.held["mem/b.bin"] = b.size(); // All of it.
        receiving.written.clear();
    }
    const auto sentInfo = transferInfo(share, sent);
    const uint64_t again = send(items, sentInfo.token);
    const auto offer = waitOffer(pair.viewEvents, pair.viewOffers);
    CHECK(offer.resuming == 1);
    CHECK(waitDone(pair.viewEvents, offer.transfer).end == RCS_FILE_COMPLETED && acceptedThere == RCS_OK);
    {
        // What happened in the host's call reached it all the same, in order.
        std::vector<rcs_file_state> states;
        for (const auto& [viewer, e] : fileEvents<rcs_file_state_event>(pair.viewEvents, RCS_FILE_STATE)) {
            if (e.transfer == offer.transfer) {
                states.push_back(e.state);
            }
        }
        CHECK(!states.empty() && states.front() == RCS_FILE_PREPARING);
    }
    CHECK(waitDone(pair.shareEvents, again).end == RCS_FILE_COMPLETED);
    {
        std::lock_guard lock(receiving.mutex);
        CHECK(receiving.written["mem/a.bin"] == a.substr(300000) && !receiving.written.count("mem/b.bin") &&
              receiving.ordered);
    }
    const auto info = transferInfo(share, again);
    CHECK(info.resumed_bytes == 300000 + b.size() && info.transferred_bytes == a.size() - 300000);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// A sharer whose agent runs through the agent link (here in the same
// process): the module's machine half is in the agent, where it reads and
// writes; an agent that leaves midway holds the transfer up until another
// attaches, and it goes on from what was written.
void fileTransferAgent(const uint8_t (&key)[32]) {
    Scratch scratch("agent");
    const fs::path out = scratch / "out", shareIn = scratch / "share-in", viewIn = scratch / "view-in";
    fs::create_directories(shareIn);
    fs::create_directories(viewIn);
    const std::string up = pattern(5u << 20, 10), down = pattern(300000, 11);
    writeText(out / "up.bin", up);
    writeText(out / "down.txt", down);
    const std::string endpoint = agentEndpoint("files");
    const auto linkKey = rcsdk::generateConnectionKey();
    auto viewFiles = fileSettings(bothWays);
    viewFiles.max_bytes_per_second = 2'000'000;
    FilePair pair(config(RCS_ROLE_SHARE, 47976, key).use(rcsdk::agentLink(endpoint, linkKey)).setKeepListening(true),
                  config(RCS_ROLE_VIEW, 47976, key), fileSettings(bothWays), viewFiles);
    auto& share = *pair.share;
    auto& view = *pair.view;
    // The agent is through with what it wrote as RCS_FILE_DONE reaches the sharer's host.
    std::atomic<int> doneHere{0}, leftAtDone{0};
    pair.shareHook = [&](const rcs_event& event) {
        if (receivedDone(event)) {
            leftAtDone += leftovers(shareIn) ? 1 : 0;
            ++doneHere;
        }
    };
    CHECK(share.start() == RCS_OK);
    auto agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
    CHECK(agent->start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING && filePeer(share, 1).max_items; }));
    CHECK(moduleEvent(pair.shareEvents, RCS_FILE_TRANSFER_MODULE, RCS_MODULE_ACTIVE, 1));

    // Read by the agent, to the viewer.
    {
        const auto [s, g] = pair.toViewer({out / "down.txt"}, viewIn);
        CHECK(waitDone(pair.viewEvents, g).end == RCS_FILE_COMPLETED && readText(viewIn / "down.txt") == down);
        CHECK(waitDone(pair.shareEvents, s).end == RCS_FILE_COMPLETED);
    }
    // Written by the agent; it leaves midway and another takes its place.
    const auto [sent, got] = pair.toSharer({out / "up.bin"}, shareIn);
    CHECK(waitFor([&] { return transferInfo(share, got).done_bytes > (1u << 20); }));
    agent.reset();
    CHECK(waitFor([&] { return !rcsdk::agentInfo(share).attached; }));
    CHECK(waitFor([&] { return transferInfo(share, got).state == RCS_FILE_WAITING; }));
    uint64_t transfer = 0;
    CHECK(offerPaths(share, 1, {out / "down.txt"}, &transfer) == RCS_ERROR_INVALID_STATE &&
          rcsdk::lastCallError().find("does not run on the machine") != std::string::npos);
    std::this_thread::sleep_for(500ms);
    const uint64_t had = transferInfo(share, got).done_bytes;
    CHECK(had < up.size() && transferInfo(view, sent).state != RCS_FILE_FINISHED);
    agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
    CHECK(agent->start() == RCS_OK);
    CHECK(waitDone(pair.shareEvents, got, 20s).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.viewEvents, sent).end == RCS_FILE_COMPLETED);
    CHECK(readText(shareIn / "up.bin") == up && !leftovers(shareIn) && doneHere == 1 && leftAtDone == 0);
    std::printf("file_transfer_agent: %llu of %zu bytes written before the agent left, %llu sent in all\n",
                static_cast<unsigned long long>(had), up.size(),
                static_cast<unsigned long long>(transferInfo(view, sent).transferred_bytes));
    CHECK(transferInfo(view, sent).transferred_bytes <= up.size() + (4u << 20));

    // Cancelled while no agent is attached: RCS_FILE_DONE does not wait for
    // one; what the agent that left kept (keep_partial) stays, and the next
    // agent discards it.
    {
        fs::create_directories(shareIn / "again");
        const auto [s, g] = pair.toSharer({out / "up.bin"}, shareIn / "again");
        CHECK(waitFor([&] { return transferInfo(share, g).done_bytes > (1u << 20); }));
        agent.reset();
        CHECK(waitFor([&] { return transferInfo(share, g).state == RCS_FILE_WAITING; }));
        CHECK(rcs_file_cancel(share.handle(), g) == RCS_OK);
        const auto here = waitDone(pair.shareEvents, g);
        CHECK(here.end == RCS_FILE_CANCELLED && !here.partial_kept && leftAtDone == 1 && leftovers(shareIn));
        CHECK(sessionLog().contains("ended without the module's machine half, which left: its partial files and its "
                                    "record may remain in its directory"));
        CHECK(!sessionLog().contains(utf8Of(shareIn))); // The host knows where; a log may go elsewhere (SA-16).
        CHECK(waitDone(pair.viewEvents, s).end == RCS_FILE_CANCELLED);
        const auto info = transferInfo(share, g);
        agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
        CHECK(agent->start() == RCS_OK);
        CHECK(waitFor([&] {
            return rcs_file_discard(share.handle(), utf8Of(shareIn / "again").c_str(), info.token) == RCS_OK;
        }));
        const auto discards = [&] { return fileEvents<rcs_file_discard_event>(pair.shareEvents, RCS_FILE_DISCARDED); };
        CHECK(waitFor([&] { return !discards().empty(); }));
        const auto discarded = discards().back().second;
        CHECK(discarded.result == RCS_OK && discarded.files == 1 && !leftovers(shareIn));
    }
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    agent.reset();
}

// Stands for the machine's system in the power module (rcsdk::power::Machine):
// what it can do, what it lists, and what it was asked to carry out. Nothing
// here restarts, shuts down, sleeps or signs out the machine the test runs on.
struct PowerMachine final : rcsdk::power::Machine {
    std::mutex mutex;
    uint32_t can = RCS_POWER_ALL_ACTIONS;
    std::string refusal; // check() refuses with it (UNSUPPORTED) where not empty...
    std::string failure; // ...and act() fails with it (INTERNAL).
    std::vector<std::pair<rcs_power_action, uint32_t>> acts;
    std::vector<rcs_power_interface> list;

    rcsdk::power::Abilities abilities() override {
        std::lock_guard lock(mutex);
        return {can, can == RCS_POWER_ALL_ACTIONS ? std::string() : "Not on this test's machine"};
    }
    void check(rcs_power_action, uint32_t) override {
        std::lock_guard lock(mutex);
        if (!refusal.empty()) {
            throw rcsdk::power::Refused(RCS_ERROR_UNSUPPORTED, refusal);
        }
    }
    void act(rcs_power_action action, uint32_t flags) override {
        std::lock_guard lock(mutex);
        acts.emplace_back(action, flags);
        if (!failure.empty()) {
            throw rcsdk::power::Refused(RCS_ERROR_INTERNAL, failure);
        }
    }
    std::vector<rcs_power_interface> interfaces() override {
        std::lock_guard lock(mutex);
        return list;
    }
    std::string noInterfaces() override { return {}; }

    void set(std::string PowerMachine::*field, const std::string& value) {
        std::lock_guard lock(mutex);
        this->*field = value;
    }
    size_t carriedOut() {
        std::lock_guard lock(mutex);
        return acts.size();
    }
    std::pair<rcs_power_action, uint32_t> last() {
        std::lock_guard lock(mutex);
        return acts.back();
    }
};

// The test's machine in the system's place, for good: nothing in this process
// acts on the machine it runs on.
std::shared_ptr<PowerMachine> testPowerMachine() {
    auto machine = std::make_shared<PowerMachine>();
    rcsdk::power::setMachine(machine);
    static std::once_flag once;
    std::call_once(once, [] { rcsdk::registerModule(*rcs_power_module()); });
    return machine;
}

rcs_power_settings powerActions(uint32_t actions, uint32_t delayMs = 1000) {
    rcs_power_settings settings = RCS_POWER_SETTINGS_INIT;
    settings.actions = actions;
    settings.action_delay_ms = delayMs;
    return settings;
}

rcs_result updatePower(rcsdk::Session& session, const rcs_power_settings& settings) {
    rcs_module_config update = RCS_MODULE_CONFIG_INIT;
    update.module = RCS_POWER_MODULE;
    update.settings = &settings;
    update.size = sizeof(settings);
    return session.update(update);
}

rcs_power_peer powerPeer(rcsdk::Session& view) {
    rcs_power_peer peer = RCS_POWER_PEER_INIT;
    CHECK(rcs_power_get_peer(view.handle(), &peer) == RCS_OK);
    return peer;
}

// The steps of power requests in `log`, with the viewer each is about.
std::vector<std::pair<rcs_viewer_id, rcs_power_action_event>> powerSteps(EventLog& log) {
    std::vector<std::pair<rcs_viewer_id, rcs_power_action_event>> steps;
    for (const auto& e : log.ofModule(RCS_POWER_MODULE, RCS_POWER_ACTION)) {
        CHECK(e.payload.size() == sizeof(rcs_power_action_event));
        rcs_power_action_event step;
        std::memcpy(&step, e.payload.data(), sizeof(step));
        steps.emplace_back(e.viewer, step);
    }
    return steps;
}

// Waits for request `request`'s step `state` in `log`.
std::optional<std::pair<rcs_viewer_id, rcs_power_action_event>>
powerStep(EventLog& log, uint64_t request, rcs_power_action_state state, std::chrono::milliseconds timeout = 5s) {
    std::optional<std::pair<rcs_viewer_id, rcs_power_action_event>> found;
    waitFor(
        [&] {
            for (const auto& step : powerSteps(log)) {
                if (step.second.request == request && step.second.state == state) {
                    found = step;
                    return true;
                }
            }
            return false;
        },
        timeout);
    return found;
}

uint64_t askPower(rcsdk::Session& view, rcs_power_action action, uint32_t flags = 0) {
    uint64_t request = 0;
    CHECK(rcs_power_request_action(view.handle(), action, flags, &request) == RCS_OK && request);
    return request;
}

// What the host allows, decided by its callback (on_action): what it saw.
struct PowerDecider {
    std::atomic<int> allow{1};
    std::mutex mutex;
    std::vector<rcs_power_request> seen;

    static int32_t decide(const rcs_power_request* request, void* self) {
        auto& decider = *static_cast<PowerDecider*>(self);
        std::lock_guard lock(decider.mutex);
        decider.seen.push_back(*request);
        return decider.allow;
    }
};

// A viewer's actions on the sharer's machine (the power module), on the
// test's machine: what the sharer's host allows and its machine can do is
// offered; a request the host does not allow, from a viewer without remote
// control (by default), that the host's callback refuses, that the machine
// cannot carry out or fails at, or while another waits, is refused or fails
// with the reason, at both ends; one accepted is carried out after the delay,
// also when the session that accepted it stopped meanwhile.
void powerActionsScenario(const uint8_t (&key)[32]) {
    const auto machine = testPowerMachine();
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    // Settings no version defines, and a viewer's that are a sharer's.
    for (const auto& [role, settings] : std::vector<std::pair<rcs_role, rcs_power_settings>>{
             {RCS_ROLE_SHARE, powerActions(1u)},
             {RCS_ROLE_SHARE, powerActions(1u << RCS_POWER_RESTART, 999)},
             {RCS_ROLE_SHARE, powerActions(1u << RCS_POWER_RESTART, 60001)},
             {RCS_ROLE_VIEW, powerActions(1u << RCS_POWER_RESTART)}}) {
        const auto copy = settings;
        CHECK(throws<rcsdk::Error>(
            [&] { rcsdk::Session invalid(config(role, 47841, key).useModule(RCS_POWER_MODULE, copy)); }));
    }
    rcs_power_settings told = RCS_POWER_SETTINGS_INIT;
    told.share_wake_info = 1;
    CHECK(throws<rcsdk::Error>(
        [&] { rcsdk::Session invalid(config(RCS_ROLE_VIEW, 47841, key).useModule(RCS_POWER_MODULE, told)); }));

    const uint32_t allowed = 1u << RCS_POWER_RESTART | 1u << RCS_POWER_SLEEP | 1u << RCS_POWER_SIGN_OUT;
    EventLog shareEvents, viewEvents;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onEvent = viewEvents.callback();
    auto share = std::make_unique<rcsdk::Session>(
        config(RCS_ROLE_SHARE, 47841, key).setKeepListening(true).useModule(RCS_POWER_MODULE, powerActions(allowed)),
        shareCallbacks);
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47841, key).useModule(RCS_POWER_MODULE, nullptr, 0), viewCallbacks);
    CHECK(rcs_power_request_action(view.handle(), RCS_POWER_RESTART, 0, nullptr) == RCS_ERROR_INVALID_STATE);
    CHECK(share->start() == RCS_OK);
    CHECK(waitFor([&] { return share->state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    // The viewer hears what the sharer offers: what its host allows and its machine can do.
    CHECK(waitFor([&] { return powerPeer(view).actions == allowed; }));
    CHECK(!powerPeer(view).wake_info && !powerPeer(view).interfaces);
    rcs_power_status status = RCS_POWER_STATUS_INIT;
    CHECK(rcs_power_get_status(share->handle(), &status) == RCS_OK && status.actions == RCS_POWER_ALL_ACTIONS);
    // What no version defines; a sharer asks nobody.
    CHECK(rcs_power_request_action(view.handle(), 0, 0, nullptr) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_power_request_action(view.handle(), RCS_POWER_SIGN_OUT + 1, 0, nullptr) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_power_request_action(view.handle(), RCS_POWER_SLEEP, 2, nullptr) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_power_request_action(share->handle(), RCS_POWER_RESTART, 0, nullptr) == RCS_ERROR_UNSUPPORTED);
    rcs_power_peer peer = RCS_POWER_PEER_INIT;
    CHECK(rcs_power_get_peer(share->handle(), &peer) == RCS_ERROR_UNSUPPORTED);

    // Not one the host allows.
    uint64_t asked = askPower(view, RCS_POWER_SHUT_DOWN);
    auto step = powerStep(viewEvents, asked, RCS_POWER_ACTION_REFUSED);
    CHECK(step && step->first == 0 && step->second.result == RCS_ERROR_REFUSED &&
          std::string(step->second.message) == "The sharer's host does not let viewers ask to shut down");
    step = powerStep(shareEvents, asked, RCS_POWER_ACTION_REFUSED);
    CHECK(step && step->first == 1 && step->second.action == RCS_POWER_SHUT_DOWN);
    // Without remote control, the default refuses.
    asked = askPower(view, RCS_POWER_RESTART);
    step = powerStep(viewEvents, asked, RCS_POWER_ACTION_REFUSED);
    CHECK(step && step->second.result == RCS_ERROR_REFUSED &&
          std::string(step->second.message) == "Viewer 1 does not hold remote control");

    // With it, the machine carries it out after the delay; both ends hear each step.
    auto control = rcsdk::defaults<rcs_control_config>();
    control.allowed = 1;
    CHECK(share->update(control) == RCS_OK);
    CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
    const auto before = std::chrono::steady_clock::now();
    asked = askPower(view, RCS_POWER_RESTART, RCS_POWER_FORCE);
    step = powerStep(viewEvents, asked, RCS_POWER_ACTION_ACCEPTED);
    CHECK(step && step->second.result == RCS_OK && step->second.delay_ms == 1000 &&
          step->second.flags == RCS_POWER_FORCE);
    CHECK(machine->carriedOut() == 0);
    step = powerStep(viewEvents, asked, RCS_POWER_ACTION_STARTED);
    CHECK(step && step->second.result == RCS_OK && step->second.action == RCS_POWER_RESTART);
    CHECK(std::chrono::steady_clock::now() - before >= 1s);
    CHECK(machine->carriedOut() == 1 &&
          machine->last() == std::make_pair(rcs_power_action(RCS_POWER_RESTART), uint32_t(RCS_POWER_FORCE)));
    CHECK(powerStep(shareEvents, asked, RCS_POWER_ACTION_ACCEPTED) &&
          powerStep(shareEvents, asked, RCS_POWER_ACTION_STARTED));
    CHECK(sessionLog().contains("Viewer 1 asked to restart (force): accepted, carried out in 1000 ms"));
    CHECK(sessionLog().contains("Carrying out: restart (force)"));

    // The host's callback decides, seeing who asked what.
    PowerDecider decider;
    auto decided = powerActions(allowed);
    decided.on_action = PowerDecider::decide;
    decided.user_data = &decider;
    CHECK(updatePower(*share, decided) == RCS_OK);
    decider.allow = 0;
    asked = askPower(view, RCS_POWER_SLEEP);
    step = powerStep(viewEvents, asked, RCS_POWER_ACTION_REFUSED);
    CHECK(step && step->second.result == RCS_ERROR_REFUSED &&
          std::string(step->second.message) == "The sharer's host refused it");
    {
        std::lock_guard lock(decider.mutex);
        CHECK(decider.seen.size() == 1);
        const auto& r = decider.seen[0];
        CHECK(r.struct_size == sizeof(r) && r.viewer_id == 1 && r.request == asked && r.action == RCS_POWER_SLEEP &&
              r.flags == 0 && r.controlling == 1);
    }
    decider.allow = 1;
    // The machine refuses it at once: its reason...
    machine->set(&PowerMachine::refusal, "Not now");
    asked = askPower(view, RCS_POWER_SLEEP);
    step = powerStep(viewEvents, asked, RCS_POWER_ACTION_REFUSED);
    CHECK(step && step->second.result == RCS_ERROR_UNSUPPORTED && std::string(step->second.message) == "Not now");
    machine->set(&PowerMachine::refusal, {});
    // ...or fails when the time comes.
    machine->set(&PowerMachine::failure, "The system said no");
    asked = askPower(view, RCS_POWER_SIGN_OUT);
    CHECK(powerStep(viewEvents, asked, RCS_POWER_ACTION_ACCEPTED));
    step = powerStep(viewEvents, asked, RCS_POWER_ACTION_FAILED);
    CHECK(step && step->second.result == RCS_ERROR_INTERNAL &&
          std::string(step->second.message) == "The system said no");
    CHECK(powerStep(shareEvents, asked, RCS_POWER_ACTION_FAILED));
    CHECK(machine->carriedOut() == 2);
    machine->set(&PowerMachine::failure, {});
    // One at a time: another waits.
    CHECK(updatePower(*share, powerActions(allowed, 3000)) == RCS_OK);
    const uint64_t first = askPower(view, RCS_POWER_SLEEP);
    CHECK(powerStep(viewEvents, first, RCS_POWER_ACTION_ACCEPTED));
    asked = askPower(view, RCS_POWER_RESTART);
    step = powerStep(viewEvents, asked, RCS_POWER_ACTION_REFUSED);
    CHECK(step && step->second.result == RCS_ERROR_BUSY);
    CHECK(powerStep(viewEvents, first, RCS_POWER_ACTION_STARTED));
    CHECK(machine->carriedOut() == 3 && machine->last().first == RCS_POWER_SLEEP);

    // What the machine cannot do is not offered, and refused with its reason.
    {
        std::lock_guard lock(machine->mutex);
        machine->can = 1u << RCS_POWER_RESTART | 1u << RCS_POWER_SLEEP;
    }
    CHECK(updatePower(*share, powerActions(RCS_POWER_ALL_ACTIONS)) == RCS_OK);
    CHECK(waitFor([&] { return powerPeer(view).actions == RCS_POWER_ALL_ACTIONS; })); // The half told it all before.
    view.stop();
    CHECK(waitFor([&] { return share->state() == RCS_STATE_LISTENING; }));
    CHECK(share->stop() == RCS_OK); // The machine's half starts again, and says what it can now.
    CHECK(share->start() == RCS_OK);
    CHECK(waitFor([&] { return share->state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return powerPeer(view).actions == (1u << RCS_POWER_RESTART | 1u << RCS_POWER_SLEEP); }));
    CHECK(waitFor([&] { return view.controlInfo().granted != 0; }));
    asked = askPower(view, RCS_POWER_SHUT_DOWN);
    step = powerStep(viewEvents, asked, RCS_POWER_ACTION_REFUSED);
    CHECK(step && step->second.result == RCS_ERROR_UNSUPPORTED &&
          std::string(step->second.message) == "Not on this test's machine");

    // An action accepted outlives the session that accepted it.
    CHECK(updatePower(*share, powerActions(RCS_POWER_ALL_ACTIONS, 1000)) == RCS_OK);
    asked = askPower(view, RCS_POWER_RESTART);
    CHECK(powerStep(viewEvents, asked, RCS_POWER_ACTION_ACCEPTED));
    CHECK(view.stop() == RCS_OK);
    share.reset();
    CHECK(waitFor([&] { return machine->carriedOut() == 4; }, 3s));
    CHECK(machine->last().first == RCS_POWER_RESTART);
    // The viewer keeps nothing it cannot ask for.
    CHECK(powerPeer(view).actions == 0);
    rcsdk::seams::setInputOverride({});
}

// A sharer tells its viewers its machine's wake information when its host
// turns it on, and not before; it follows the setting while running; the
// viewer keeps what it heard after the connection ended.
void powerWakeScenario(const uint8_t (&key)[32]) {
    const auto machine = testPowerMachine();
    const uint8_t wired[6] = {0x00, 0x1b, 0x21, 0x3a, 0x4b, 0x5c}, radio[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    machine->list = {
        rcsdk::power::makeInterface(wired, RCS_POWER_LINK_ETHERNET, RCS_POWER_WAKE_ENABLED, 0xC0A80114, 24, "Ethernet"),
        rcsdk::power::makeInterface(radio, RCS_POWER_LINK_WIFI, RCS_POWER_WAKE_UNKNOWN, 0x0A000105, 16, "Wi-Fi")};
    EventLog viewEvents;
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onEvent = viewEvents.callback();
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47842, key).setKeepListening(true).useModule(RCS_POWER_MODULE, nullptr,
                                                                                             0));
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47842, key).useModule(RCS_POWER_MODULE, nullptr, 0), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return !viewEvents.ofModule(RCS_POWER_MODULE, RCS_POWER_PEER).empty(); }));
    // Off by default: nothing told.
    auto peer = powerPeer(view);
    CHECK(!peer.wake_info && !peer.interfaces && !peer.actions);
    uint32_t count = 9;
    CHECK(rcs_power_get_peer_interfaces(view.handle(), nullptr, sizeof(rcs_power_interface), 0, &count) == RCS_OK &&
          count == 0);
    // Turned on: the viewer hears it.
    rcs_power_settings tells = RCS_POWER_SETTINGS_INIT;
    tells.share_wake_info = 1;
    CHECK(updatePower(share, tells) == RCS_OK);
    CHECK(waitFor([&] { return powerPeer(view).interfaces == 2; }));
    CHECK(powerPeer(view).wake_info == 1);
    const auto told = [&] {
        uint32_t n = 0;
        CHECK(rcs_power_get_peer_interfaces(view.handle(), nullptr, sizeof(rcs_power_interface), 0, &n) == RCS_OK);
        std::vector<rcs_power_interface> list(n);
        CHECK(rcs_power_get_peer_interfaces(view.handle(), list.data(), sizeof(rcs_power_interface), n, &n) ==
              RCS_OK);
        return list;
    };
    auto list = told();
    CHECK(list.size() == 2 && std::memcmp(list[0].mac, wired, 6) == 0 && list[0].link == RCS_POWER_LINK_ETHERNET &&
          list[0].wake == RCS_POWER_WAKE_ENABLED && std::string(list[0].address) == "192.168.1.20" &&
          list[0].prefix_length == 24 && std::string(list[0].broadcast) == "192.168.1.255" &&
          std::string(list[0].name) == "Ethernet");
    CHECK(std::memcmp(list[1].mac, radio, 6) == 0 && std::string(list[1].broadcast) == "10.0.255.255" &&
          list[1].link == RCS_POWER_LINK_WIFI && list[1].wake == RCS_POWER_WAKE_UNKNOWN);
    rcs_power_interface one{};
    CHECK(rcs_power_get_peer_interfaces(view.handle(), &one, sizeof(one), 1, &count) == RCS_ERROR_BUFFER_TOO_SMALL &&
          count == 2 && std::memcmp(one.mac, wired, 6) == 0);
    CHECK(rcs_power_get_peer_interfaces(view.handle(), &one, 4, 1, &count) == RCS_ERROR_INVALID_ARGUMENT);
    // Turned off: gone.
    CHECK(updatePower(share, RCS_POWER_SETTINGS_INIT) == RCS_OK);
    CHECK(waitFor([&] { return powerPeer(view).wake_info == 0 && powerPeer(view).interfaces == 0; }));
    // A viewer that connects hears them as they are then; they stay after the connection ended.
    CHECK(updatePower(share, tells) == RCS_OK);
    CHECK(waitFor([&] { return powerPeer(view).interfaces == 2; }));
    CHECK(view.stop() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    {
        std::lock_guard lock(machine->mutex);
        machine->list.pop_back();
    }
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return powerPeer(view).interfaces == 1; }));
    CHECK(share.stop() == RCS_OK);
    CHECK(waitFor([&] { return view.state() != RCS_STATE_RUNNING; }));
    CHECK(powerPeer(view).interfaces == 1 && told().size() == 1 && std::memcmp(told()[0].mac, wired, 6) == 0);
}

// The machine's half in an agent (here in the same process, behind the
// link): it carries out what the session accepts; while no agent runs it,
// nothing is offered and a request is refused (INVALID_STATE).
void powerActionsAgentScenario(const uint8_t (&key)[32]) {
    const auto machine = testPowerMachine();
    const std::string endpoint = agentEndpoint("power");
    const auto linkKey = rcsdk::generateConnectionKey();
    EventLog viewEvents;
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onEvent = viewEvents.callback();
    PowerDecider decider; // Any viewer, control or not.
    auto settings = powerActions(RCS_POWER_ALL_ACTIONS);
    settings.on_action = PowerDecider::decide;
    settings.user_data = &decider;
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47843, key)
                             .use(rcsdk::agentLink(endpoint, linkKey))
                             .setKeepListening(true)
                             .useModule(RCS_POWER_MODULE, settings));
    rcsdk::Session view(config(RCS_ROLE_VIEW, 47843, key).useModule(RCS_POWER_MODULE, nullptr, 0), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    auto agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
    CHECK(agent->start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return powerPeer(view).actions == RCS_POWER_ALL_ACTIONS; }));
    uint64_t asked = askPower(view, RCS_POWER_SHUT_DOWN);
    CHECK(powerStep(viewEvents, asked, RCS_POWER_ACTION_STARTED));
    CHECK(machine->carriedOut() == 1 && machine->last().first == RCS_POWER_SHUT_DOWN);
    // The agent leaves: nothing is offered, and nothing can be carried out.
    agent.reset();
    CHECK(waitFor([&] { return !rcsdk::agentInfo(share).attached; }));
    CHECK(waitFor([&] { return powerPeer(view).actions == 0; }));
    rcs_power_status status = RCS_POWER_STATUS_INIT;
    CHECK(rcs_power_get_status(share.handle(), &status) == RCS_OK && status.actions == 0);
    asked = askPower(view, RCS_POWER_RESTART);
    const auto step = powerStep(viewEvents, asked, RCS_POWER_ACTION_REFUSED);
    CHECK(step && step->second.result == RCS_ERROR_INVALID_STATE);
    // Another comes: offered again.
    agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
    CHECK(agent->start() == RCS_OK);
    CHECK(waitFor([&] { return powerPeer(view).actions == RCS_POWER_ALL_ACTIONS; }));
    asked = askPower(view, RCS_POWER_SIGN_OUT);
    CHECK(powerStep(viewEvents, asked, RCS_POWER_ACTION_STARTED));
    CHECK(machine->carriedOut() == 2 && machine->last().first == RCS_POWER_SIGN_OUT);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    agent.reset();
}

using Publication = std::array<uint8_t, RCS_FILE_PUBLICATION_SIZE>;

// What a publication's callbacks are told.
struct PublicationLog {
    std::mutex mutex;
    std::vector<std::pair<Publication, uint64_t>> fetches;
    std::vector<std::pair<Publication, rcs_result>> withdrawn;

    void into(rcs_file_publication& p) {
        p.user_data = this;
        p.on_fetch = [](const uint8_t* id, uint64_t transfer, void* self) {
            auto& log = *static_cast<PublicationLog*>(self);
            std::lock_guard lock(log.mutex);
            Publication which{};
            std::memcpy(which.data(), id, which.size());
            log.fetches.emplace_back(which, transfer);
        };
        p.on_unpublished = [](const uint8_t* id, rcs_result result, const char*, void* self) {
            auto& log = *static_cast<PublicationLog*>(self);
            std::lock_guard lock(log.mutex);
            Publication which{};
            std::memcpy(which.data(), id, which.size());
            log.withdrawn.emplace_back(which, result);
        };
    }
    std::optional<rcs_result> withdrawnAs(const Publication& id) {
        std::lock_guard lock(mutex);
        for (const auto& [which, result] : withdrawn) {
            if (which == id) {
                return result;
            }
        }
        return std::nullopt;
    }
    size_t fetched(const Publication& id) {
        std::lock_guard lock(mutex);
        return size_t(std::count_if(fetches.begin(), fetches.end(), [&](const auto& f) { return f.first == id; }));
    }
    // The transfer of the n-th fetch.
    uint64_t transfer(size_t n) {
        std::lock_guard lock(mutex);
        return fetches.at(n).second;
    }
};

// What a fetch's callbacks are told: its items once listed (read in
// on_listed, as a host may), each read's answer by tag, and its end.
struct FetchLog {
    rcs_session session = nullptr;
    std::mutex mutex;
    std::atomic<bool> listed{false};
    uint32_t items = 0;
    std::map<uint64_t, std::pair<rcs_result, std::string>> answers;
    std::optional<std::pair<rcs_result, std::string>> ended;
    bool afterEnd = false; // Something came after on_ended.

    void into(rcs_file_fetch_request& r, bool reading) {
        r.user_data = this;
        r.on_listed = [](uint64_t transfer, void* self) {
            auto& log = *static_cast<FetchLog*>(self);
            uint32_t count = 0;
            CHECK(rcs_file_get_items(log.session, transfer, 0, nullptr, sizeof(rcs_file_item_info), 0, &count) ==
                  RCS_OK);
            std::lock_guard lock(log.mutex);
            log.items = count;
            log.afterEnd = log.afterEnd || log.ended;
            log.listed = true;
        };
        if (reading) {
            r.on_read = [](uint64_t, uint64_t tag, rcs_result result, const uint8_t* data, uint32_t size, void* self) {
                auto& log = *static_cast<FetchLog*>(self);
                std::lock_guard lock(log.mutex);
                CHECK(!log.answers.count(tag));
                log.answers[tag] = {result, std::string(reinterpret_cast<const char*>(data), size)};
                log.afterEnd = log.afterEnd || log.ended;
            };
        }
        r.on_ended = [](uint64_t, rcs_result result, const char* message, void* self) {
            auto& log = *static_cast<FetchLog*>(self);
            std::lock_guard lock(log.mutex);
            log.afterEnd = log.afterEnd || log.ended;
            log.ended = std::pair(result, std::string(message));
        };
    }
    std::optional<std::pair<rcs_result, std::string>> answer(uint64_t tag) {
        std::lock_guard lock(mutex);
        const auto found = answers.find(tag);
        return found == answers.end() ? std::nullopt : std::optional(found->second);
    }
    std::optional<std::pair<rcs_result, std::string>> end() {
        std::lock_guard lock(mutex);
        return ended;
    }
};

// Publishes `paths` of `session`'s machine to `viewer` (a viewer's sharer: 0).
rcs_result publishPaths(rcsdk::Session& session, rcs_viewer_id viewer, const std::vector<fs::path>& paths,
                        Publication& id, PublicationLog* log = nullptr) {
    std::vector<std::string> texts;
    for (const auto& path : paths) {
        texts.push_back(utf8Of(path));
    }
    std::vector<rcs_file_item> items(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        items[i].path = texts[i].c_str();
    }
    rcs_file_publication p = RCS_FILE_PUBLICATION_INIT;
    p.viewer_id = viewer;
    p.count = uint32_t(items.size());
    p.items = items.data();
    if (log) {
        log->into(p);
    }
    return rcs_file_publish(session.handle(), &p, id.data());
}

rcs_file_fetch_request fetchOf(rcs_viewer_id viewer, const Publication& id, rcs_file_fetch_mode mode) {
    rcs_file_fetch_request r = RCS_FILE_FETCH_REQUEST_INIT;
    r.viewer_id = viewer;
    std::memcpy(r.publication, id.data(), id.size());
    r.mode = mode;
    return r;
}

// One end publishes, the other fetches: into a directory, as a transfer
// received, or read piece by piece; nothing goes before it fetches, and no
// host decides on an offer. A publication withdrawn is fetched no more, a
// fetch under way of it goes on (unless withdrawn with its fetches); the
// limits on publications and fetches; reads beyond the window, past an end,
// of a file that changed; reads not answered as a fetch ends.
void fileTransferPublish(const uint8_t (&key)[32]) {
    Scratch scratch("publish");
    const fs::path out = scratch / "out", viewIn = scratch / "view-in", shareIn = scratch / "share-in";
    fs::create_directories(viewIn);
    fs::create_directories(shareIn);
    const std::string a = pattern(100000, 21), b = pattern(600000, 22), single = pattern(5000, 23);
    writeText(out / "folder" / "a.txt", a);
    writeText(out / "folder" / "sub" / "b.bin", b);
    writeText(out / "single.txt", single);
    FilePair pair(config(RCS_ROLE_SHARE, 47978, key), config(RCS_ROLE_VIEW, 47978, key), fileSettings(bothWays),
                  fileSettings(bothWays));
    auto& share = *pair.share;
    auto& view = *pair.view;
    Publication id{};
    CHECK(publishPaths(share, 1, {out / "single.txt"}, id) == RCS_ERROR_INVALID_STATE); // Not running.
    pair.connect();
    PublicationLog published;
    CHECK(publishPaths(share, 5, {out / "single.txt"}, id) == RCS_ERROR_INVALID_STATE); // No such viewer.
    CHECK(publishPaths(share, 1, {"relative.txt"}, id) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(publishPaths(view, 1, {out / "single.txt"}, id) == RCS_ERROR_INVALID_ARGUMENT); // A viewer's is 0.
    CHECK(publishPaths(share, 1, {out / "folder", out / "single.txt"}, id, &published) == RCS_OK);
    std::this_thread::sleep_for(200ms);
    CHECK(fileEvents<rcs_file_offer_event>(pair.viewEvents, RCS_FILE_OFFER).empty() &&
          fileEvents<rcs_file_state_event>(pair.shareEvents, RCS_FILE_STATE).empty()); // Nothing went.

    // Into a directory: as a transfer received, without an offer at either end.
    uint64_t into = 0;
    {
        FetchLog log;
        log.session = view.handle();
        auto r = fetchOf(0, id, RCS_FILE_FETCH_INTO);
        const std::string directory = utf8Of(viewIn);
        r.directory = directory.c_str();
        log.into(r, false);
        CHECK(rcs_file_fetch(view.handle(), &r, &into) == RCS_OK);
        const auto done = waitDone(pair.viewEvents, into);
        CHECK(done.end == RCS_FILE_COMPLETED && done.items_completed == 5);
        CHECK(readText(viewIn / "folder" / "a.txt") == a && readText(viewIn / "folder" / "sub" / "b.bin") == b &&
              readText(viewIn / "single.txt") == single && !leftovers(viewIn));
        CHECK(waitFor([&] { return log.end().has_value(); }) && log.end()->first == RCS_OK && log.listed &&
              log.items == 5 && !log.afterEnd);
        const auto info = transferInfo(view, into);
        CHECK(info.origin == RCS_FILE_ORIGIN_FETCHED && info.mode == RCS_FILE_FETCH_INTO &&
              Publication(std::to_array(info.publication)) == id && info.direction == RCS_FILE_RECEIVE);
        CHECK(waitFor([&] { return published.fetched(id) == 1; }));
        const uint64_t sent = published.transfer(0);
        CHECK(waitDone(pair.shareEvents, sent).end == RCS_FILE_COMPLETED &&
              transferInfo(share, sent).origin == RCS_FILE_ORIGIN_FETCHED);
        CHECK(fileEvents<rcs_file_offer_event>(pair.viewEvents, RCS_FILE_OFFER).empty());
    }

    // Read piece by piece: in any order, as often as wanted; a file read
    // whole is COMPLETED; past its end is cut; a full window is QUEUE_FULL.
    {
        FetchLog log;
        log.session = view.handle();
        auto r = fetchOf(0, id, RCS_FILE_FETCH_READ);
        CHECK(rcs_file_fetch(view.handle(), &r, &into) == RCS_ERROR_INVALID_ARGUMENT); // READ needs on_read.
        log.into(r, true);
        uint64_t reading = 0;
        CHECK(rcs_file_fetch(view.handle(), &r, &reading) == RCS_OK);
        CHECK(waitFor([&] { return log.listed.load(); }));
        const auto items = transferItems(view, reading);
        CHECK(items.size() == 5 && std::string(items[3].path) == "folder/sub/b.bin" && items[3].size == b.size());
        std::string whole;
        uint64_t tag = 100;
        for (uint64_t offset = 0; offset < b.size(); offset += RCS_FILE_READ_MAX) {
            CHECK(rcs_file_read(view.handle(), reading, 3, offset, RCS_FILE_READ_MAX, tag++) == RCS_OK);
        }
        for (uint64_t t = 100; t < tag; ++t) {
            CHECK(waitFor([&] { return log.answer(t).has_value(); }));
            CHECK(log.answer(t)->first == RCS_OK);
            whole += log.answer(t)->second;
        }
        CHECK(whole == b);
        CHECK(waitFor([&] { return transferItems(view, reading)[3].state == RCS_FILE_ITEM_COMPLETED; }));
        CHECK(rcs_file_read(view.handle(), reading, 1, 50000, 100, 1) == RCS_OK);              // Anywhere.
        CHECK(rcs_file_read(view.handle(), reading, 1, a.size() - 10, 1000, 2) == RCS_OK);     // Cut at its end.
        CHECK(waitFor([&] { return log.answer(1) && log.answer(2); }));
        CHECK(log.answer(1)->second == a.substr(50000, 100) && log.answer(2)->second == a.substr(a.size() - 10));
        CHECK(rcs_file_read(view.handle(), reading, 1, a.size(), 1, 3) == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(rcs_file_read(view.handle(), reading, 0, 0, 1, 3) == RCS_ERROR_INVALID_ARGUMENT); // A directory.
        CHECK(rcs_file_read(view.handle(), reading, 1, 0, 0, 3) == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(rcs_file_read(view.handle(), reading, 1, 0, RCS_FILE_READ_MAX + 1, 3) == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(rcs_file_read(view.handle(), into, 1, 0, 1, 3) == RCS_ERROR_INVALID_STATE); // Not a READ fetch.
        // Paused here, reads wait: 16 MiB of them asked fill the window.
        CHECK(rcs_file_pause(view.handle(), reading, 1) == RCS_OK);
        uint32_t asked = 0;
        rcs_result last = RCS_OK;
        while ((last = rcs_file_read(view.handle(), reading, 3, 0, RCS_FILE_READ_MAX, 1000 + asked)) == RCS_OK) {
            ++asked;
        }
        CHECK(last == RCS_ERROR_QUEUE_FULL && asked == RCS_FILE_READS_MAX / RCS_FILE_READ_MAX);
        CHECK(rcs_file_pause(view.handle(), reading, 0) == RCS_OK);
        CHECK(waitFor([&] { return log.answer(1000 + asked - 1).has_value(); }, 60s)); // 16 MiB read.
        CHECK(log.answer(1000)->second == b.substr(0, RCS_FILE_READ_MAX));
        // A file that changed since it was listed: its reads fail, the item with them.
        writeText(out / "single.txt", "changed");
        CHECK(rcs_file_read(view.handle(), reading, 4, 0, 10, 4) == RCS_OK);
        CHECK(waitFor([&] { return log.answer(4).has_value(); }) && log.answer(4)->first == RCS_ERROR_INVALID_STATE);
        CHECK(waitFor([&] { return transferItems(view, reading)[4].state == RCS_FILE_ITEM_FAILED; }));
        writeText(out / "single.txt", single);
        // Closed: what was read whole COMPLETED (the directories too), the rest SKIPPED.
        CHECK(rcs_file_close(view.handle(), reading, RCS_OK, nullptr) == RCS_OK);
        const auto done = waitDone(pair.viewEvents, reading);
        CHECK(done.end == RCS_FILE_COMPLETED && done.items_completed == 3 && done.items_skipped == 1 &&
              done.items_failed == 1);
        CHECK(waitFor([&] { return log.end().has_value(); }) && !log.afterEnd);
        CHECK(rcs_file_close(view.handle(), reading, RCS_OK, nullptr) == RCS_ERROR_INVALID_STATE);
        CHECK(waitFor([&] { return published.fetched(id) == 2; }));
        const uint64_t served = published.transfer(1);
        const auto there = waitDone(pair.shareEvents, served);
        CHECK(there.end == RCS_FILE_COMPLETED && there.by_peer && there.items_completed == 3 &&
              there.items_skipped == 1 && there.items_failed == 1);
        CHECK(transferInfo(share, served).mode == RCS_FILE_FETCH_READ &&
              transferInfo(share, served).transferred_bytes >= b.size());
    }

    // Withdrawn: fetched no more; one under way goes on, unless withdrawn with its fetches.
    {
        FetchLog log;
        log.session = view.handle();
        auto r = fetchOf(0, id, RCS_FILE_FETCH_READ);
        log.into(r, true);
        uint64_t going = 0;
        CHECK(rcs_file_fetch(view.handle(), &r, &going) == RCS_OK);
        CHECK(waitFor([&] { return log.listed.load(); }));
        CHECK(rcs_file_unpublish(share.handle(), id.data(), 0) == RCS_OK);
        CHECK(waitFor([&] { return published.withdrawnAs(id) == RCS_ERROR_CANCELLED; }));
        CHECK(rcs_file_unpublish(share.handle(), id.data(), 0) == RCS_ERROR_INVALID_STATE);
        CHECK(rcs_file_read(view.handle(), going, 1, 0, 10, 1) == RCS_OK);
        CHECK(waitFor([&] { return log.answer(1).has_value(); }) && log.answer(1)->second == a.substr(0, 10));
        FetchLog refusedLog;
        refusedLog.session = view.handle();
        auto again = fetchOf(0, id, RCS_FILE_FETCH_READ);
        refusedLog.into(again, true);
        uint64_t refused = 0;
        CHECK(rcs_file_fetch(view.handle(), &again, &refused) == RCS_OK);
        const auto no = waitDone(pair.viewEvents, refused);
        CHECK(no.end == RCS_FILE_REJECTED && no.result == RCS_ERROR_INVALID_STATE && no.by_peer);
        CHECK(waitFor([&] { return refusedLog.end().has_value(); }) && !refusedLog.listed);
        CHECK(rcs_file_close(view.handle(), going, RCS_ERROR_DEVICE, "the reader gave up") == RCS_OK);
        const auto closed = waitDone(pair.viewEvents, going);
        CHECK(closed.end == RCS_FILE_FAILED && closed.result == RCS_ERROR_DEVICE);
        CHECK(waitFor([&] { return log.end().has_value(); })); // Its user_data is the test's until then.
        CHECK(waitFor([&] { return published.fetched(id) == 3; }));
        CHECK(waitDone(pair.shareEvents, published.transfer(2)).message == std::string("the reader gave up"));

        Publication other{};
        CHECK(publishPaths(share, 1, {out / "folder"}, other, &published) == RCS_OK);
        FetchLog endedLog;
        endedLog.session = view.handle();
        auto r2 = fetchOf(0, other, RCS_FILE_FETCH_READ);
        endedLog.into(r2, true);
        CHECK(rcs_file_fetch(view.handle(), &r2, &going) == RCS_OK);
        CHECK(waitFor([&] { return endedLog.listed.load(); }));
        CHECK(rcs_file_pause(view.handle(), going, 1) == RCS_OK); // Its reads wait, unanswered.
        CHECK(rcs_file_read(view.handle(), going, 1, 0, 10, 7) == RCS_OK);
        CHECK(rcs_file_unpublish(share.handle(), other.data(), RCS_FILE_UNPUBLISH_END_FETCHES) == RCS_OK);
        const auto cut = waitDone(pair.viewEvents, going);
        CHECK(cut.end == RCS_FILE_CANCELLED && cut.by_peer);
        CHECK(waitFor([&] { return endedLog.end().has_value(); }) && endedLog.answer(7) &&
              endedLog.answer(7)->first == RCS_ERROR_CANCELLED && !endedLog.afterEnd);
    }

    // The limits: fetches going from one end, publications kept.
    {
        auto shareFiles = fileSettings(bothWays);
        shareFiles.max_fetches = 2;
        shareFiles.max_publications = 2;
        CHECK(updateFiles(share, shareFiles) == RCS_OK);
        Publication first{}, second{}, third{};
        CHECK(publishPaths(share, 1, {out / "single.txt"}, first, &published) == RCS_OK);
        CHECK(publishPaths(share, 1, {out / "single.txt"}, second, &published) == RCS_OK);
        CHECK(publishPaths(share, 1, {out / "single.txt"}, third, &published) == RCS_ERROR_BUSY);
        std::vector<std::unique_ptr<FetchLog>> logs;
        std::vector<uint64_t> fetches(3);
        for (size_t i = 0; i < 3; ++i) {
            logs.push_back(std::make_unique<FetchLog>());
            logs.back()->session = view.handle();
            auto r = fetchOf(0, first, RCS_FILE_FETCH_READ);
            logs.back()->into(r, true);
            CHECK(rcs_file_fetch(view.handle(), &r, &fetches[i]) == RCS_OK);
            CHECK(waitFor([&] { return logs.back()->listed || logs.back()->end(); }));
        }
        CHECK(logs[0]->listed && logs[1]->listed && !logs[2]->listed);
        const auto busy = waitDone(pair.viewEvents, fetches[2]);
        CHECK(busy.end == RCS_FILE_REJECTED && busy.result == RCS_ERROR_BUSY);
        // This end stops sending: what it published is withdrawn.
        shareFiles.flow = RCS_FILE_RECEIVE;
        CHECK(updateFiles(share, shareFiles) == RCS_OK);
        CHECK(waitFor([&] {
            return published.withdrawnAs(first) == RCS_ERROR_INVALID_STATE &&
                   published.withdrawnAs(second) == RCS_ERROR_INVALID_STATE;
        }));
        for (size_t i = 0; i < 2; ++i) {
            CHECK(rcs_file_cancel(view.handle(), fetches[i]) == RCS_OK);
            CHECK(waitDone(pair.viewEvents, fetches[i]).end == RCS_FILE_CANCELLED);
        }
        for (const auto& log : logs) {
            CHECK(waitFor([&] { return log->end().has_value(); }));
        }
        CHECK(updateFiles(share, fileSettings(bothWays)) == RCS_OK);
    }

    // The other way: the viewer publishes, its sharer fetches into a directory;
    // an id nobody published is refused.
    {
        CHECK(waitFor([&] { return filePeer(view).flow == bothWays; }));
        Publication up{};
        CHECK(publishPaths(view, 0, {out / "single.txt"}, up) == RCS_OK);
        auto r = fetchOf(1, up, RCS_FILE_FETCH_INTO);
        const std::string directory = utf8Of(shareIn);
        r.directory = directory.c_str();
        uint64_t got = 0;
        CHECK(rcs_file_fetch(share.handle(), &r, &got) == RCS_OK);
        CHECK(waitDone(pair.shareEvents, got).end == RCS_FILE_COMPLETED && readText(shareIn / "single.txt") == single);
        Publication nobody{};
        nobody.fill(7);
        auto bogus = fetchOf(1, nobody, RCS_FILE_FETCH_INTO);
        bogus.directory = directory.c_str();
        CHECK(rcs_file_fetch(share.handle(), &bogus, &got) == RCS_OK);
        const auto no = waitDone(pair.shareEvents, got);
        CHECK(no.end == RCS_FILE_REJECTED && no.result == RCS_ERROR_INVALID_STATE);
    }
    // The run ends: what is published is withdrawn.
    Publication last{};
    CHECK(publishPaths(share, 1, {out / "single.txt"}, last, &published) == RCS_OK);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    CHECK(waitFor([&] { return published.withdrawnAs(last) == RCS_ERROR_DISCONNECTED; }));
}

// ---- Copies of files (rcsdk/clipboard.h FILES over rcsdk/file_transfer.h) ------------------

using rcsdk::clipboard::FileEntry;
using rcsdk::clipboard::test::filesCopy;

std::vector<std::pair<rcs_viewer_id, rcs_clipboard_paste_event>> pasteEvents(EventLog& log) {
    std::vector<std::pair<rcs_viewer_id, rcs_clipboard_paste_event>> found;
    for (const auto& e : log.ofModule(RCS_CLIPBOARD_MODULE, RCS_CLIPBOARD_PASTE)) {
        rcs_clipboard_paste_event event{};
        CHECK(e.payload.size() == sizeof(event));
        std::memcpy(&event, e.payload.data(), sizeof(event));
        found.emplace_back(e.viewer, event);
    }
    return found;
}

// A file or directory of the test's, as a copy names it.
FileEntry entryOf(const fs::path& path) {
    FileEntry entry;
    entry.path = utf8Of(path);
    entry.name = utf8Of(path.filename());
    entry.kind = fs::is_directory(path) ? RCS_FILE_DIRECTORY : RCS_FILE_REGULAR;
    entry.size = entry.kind == RCS_FILE_REGULAR ? fs::file_size(path) : 0;
    return entry;
}

rcs_clipboard_settings filesClipboard(rcs_clipboard_flow flow) {
    auto settings = clipboardSettings(flow);
    settings.formats = RCS_CLIPBOARD_ALL_FORMATS | (1u << RCS_CLIPBOARD_FILES);
    return settings;
}

// Files copied at one end are pasted at the other: nothing of what they hold
// goes before a paste; a paste into a directory (Finder's) and one read
// file by file (Explorer's) are fetches the hosts see at both ends; a newer
// copy withdraws the files (a paste under way goes on, a later one of them
// fails); without file transfer receiving at the other end they stay; files
// fetched ahead are placed as themselves; a host that takes copies itself
// fetches their files where it likes.
void clipboardFiles(const uint8_t (&key)[32]) {
    Scratch scratch("clipboard-files");
    const fs::path out = scratch / "out", pasted = scratch / "pasted", ahead = scratch / "ahead";
    fs::create_directories(pasted);
    fs::create_directories(ahead);
    const std::string report = pattern(700000, 31), photo = pattern(40000, 32);
    writeText(out / "report.txt", report);
    writeText(out / "photos" / "a.jpg", photo);
    writeText(out / "photos" / "sub" / "b.txt", "bee");
    FakeClipboard sharerBoard, viewerBoard;
    rcsdk::clipboard::setFactory([&](rcs_role role) -> std::unique_ptr<rcsdk::clipboard::SystemClipboard> {
        return std::make_unique<FakeClipboardAccess>(role == RCS_ROLE_SHARE ? sharerBoard : viewerBoard);
    });
    constexpr uint32_t both = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
    const uint32_t files = 1u << RCS_CLIPBOARD_FILES;
    EventLog shareEvents, viewEvents;
    ClipboardLog shareSeen, viewSeen;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onEvent = viewEvents.callback();
    auto shareClipboard = filesClipboard(both), viewClipboard = filesClipboard(both);
    shareSeen.into(shareClipboard);
    viewSeen.into(viewClipboard);
    auto shareConfig = config(RCS_ROLE_SHARE, 47979, key);
    auto viewConfig = config(RCS_ROLE_VIEW, 47979, key);
    rcsdk::Session share(withFiles(withClipboard(shareConfig, shareClipboard), fileSettings(bothWays)),
                         shareCallbacks);
    rcsdk::Session view(withFiles(withClipboard(viewConfig, viewClipboard), fileSettings(bothWays)), viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    // Each end takes FILES once its machine half said its clipboard carries them.
    CHECK(waitFor([&] { return (clipboardPeer(share, 1).formats & files) && (clipboardPeer(view).formats & files); }));
    std::this_thread::sleep_for(300ms); // Both watchers looked at what their clipboards held before.

    // Copied: only names and sizes go, published to the viewer.
    sharerBoard.copy(filesCopy({entryOf(out / "report.txt"), entryOf(out / "photos")}, true));
    CHECK(waitFor([&] {
        const auto held = viewerBoard.held();
        return held.files && held.files->entries.size() == 2 && held.files->entries[0].path.empty();
    }));
    const auto sentFiles = shareSeen.lastFiles(RCS_CLIPBOARD_SEND), gotFiles = viewSeen.lastFiles(RCS_CLIPBOARD_RECEIVE);
    CHECK(sentFiles && sentFiles->paths[0] == utf8Of(out / "report.txt") && sentFiles->cut &&
          sentFiles->publication == Publication{});
    CHECK(gotFiles && gotFiles->names == std::vector<std::string>({"report.txt", "photos"}) &&
          gotFiles->paths[0].empty() && gotFiles->publication != Publication{} && gotFiles->cut);
    CHECK(clipboardEvent(shareEvents, RCS_CLIPBOARD_SEND, RCS_OK, 1));
    std::this_thread::sleep_for(200ms);
    CHECK(fileEvents<rcs_file_state_event>(viewEvents, RCS_FILE_STATE).empty() && pasteEvents(viewEvents).empty());

    // Pasted into a directory, as Finder does: a fetch at both ends.
    const auto into = viewerBoard.pasteInto(utf8Of(pasted));
    CHECK(into && into->result == RCS_OK && into->complete);
    CHECK(readText(pasted / "report.txt") == report && readText(pasted / "photos" / "a.jpg") == photo &&
          readText(pasted / "photos" / "sub" / "b.txt") == "bee" && !leftovers(pasted));
    CHECK(waitFor([&] { return pasteEvents(viewEvents).size() == 1 && pasteEvents(shareEvents).size() == 1; }));
    const auto here = pasteEvents(viewEvents)[0].second, there = pasteEvents(shareEvents)[0].second;
    CHECK(here.flow == RCS_CLIPBOARD_RECEIVE && here.result == RCS_OK && here.cut && !here.ahead && here.transfer);
    CHECK(there.flow == RCS_CLIPBOARD_SEND && there.result == RCS_OK && there.cut && pasteEvents(shareEvents)[0].first == 1);
    CHECK(waitDone(viewEvents, here.transfer).end == RCS_FILE_COMPLETED &&
          transferInfo(view, here.transfer).origin == RCS_FILE_ORIGIN_FETCHED);
    CHECK(waitDone(shareEvents, there.transfer).end == RCS_FILE_COMPLETED);

    // Read file by file, as Explorer does (one left unread).
    const auto read = viewerBoard.pasteRead("photos/sub/b.txt");
    CHECK(read && read->failed == RCS_OK && read->ended && read->ended->result == RCS_OK && !read->ended->complete);
    CHECK(read->items.size() == 5 && read->files.at("report.txt") == report &&
          read->files.at("photos/a.jpg") == photo && !read->files.count("photos/sub/b.txt"));
    CHECK(waitFor([&] { return clipboardStats(view).pastes_here == 2 && clipboardStats(share).pastes_there == 2; }));

    // A newer copy withdraws them: a paste under way goes on; a later one of
    // the copy that was (its stand-in kept by a program) fails.
    const auto broker = viewerBoard.pastes;
    const uint64_t older = viewerBoard.held().files->placing;
    const auto going = broker->fetch(older, RCS_FILE_FETCH_READ);
    CHECK(going->waitFor([&] { return going->listed; }, 10s));
    sharerBoard.copy(textCopy("something else"));
    CHECK(waitFor([&] { return viewerBoard.text() == "something else"; }));
    broker->read(*going, 0, 0, 100, 1); // report.txt, listed first.
    CHECK(going->waitFor([&] { return going->answers.count(1) != 0; }, 10s));
    {
        std::lock_guard lock(going->mutex);
        CHECK(going->answers[1].first == RCS_OK &&
              going->answers[1].second == rcsdk::clipboard::Bytes(report.begin(), report.begin() + 100));
    }
    broker->close(*going, RCS_OK);
    std::this_thread::sleep_for(300ms); // The withdrawal reached the viewer.
    const auto late = broker->fetch(older, RCS_FILE_FETCH_READ);
    CHECK(late->waitFor([&] { return late->ended && late->ended->result == RCS_ERROR_INVALID_STATE; }, 10s));
    CHECK(waitFor([&] {
        const auto events = pasteEvents(viewEvents);
        return !events.empty() && events.back().second.result == RCS_ERROR_INVALID_STATE &&
               clipboardStats(view).pastes_failed == 1;
    }));

    // The viewer's file transfer stops receiving: files alone do not go
    // (why says so), beside text they are left out.
    auto viewFiles = fileSettings(RCS_FILE_SEND);
    CHECK(updateFiles(view, viewFiles) == RCS_OK);
    CHECK(waitFor([&] { return filePeer(share, 1).flow == RCS_FILE_SEND; }));
    sharerBoard.copy(filesCopy({entryOf(out / "report.txt")}));
    CHECK(waitFor([&] {
        for (const auto& [viewer, event] : clipboardEvents(shareEvents)) {
            if (event.result == RCS_ERROR_UNSUPPORTED && std::string(event.message).find("does not receive") !=
                                                             std::string::npos) {
                return true;
            }
        }
        return false;
    }));
    auto mixed = filesCopy({entryOf(out / "report.txt")});
    mixed.items.push_back(clipboardItem(RCS_CLIPBOARD_TEXT, "names beside"));
    sharerBoard.copy(mixed);
    CHECK(waitFor([&] { return viewerBoard.text() == "names beside" && !viewerBoard.held().files; }));
    CHECK(waitFor([&] {
        const auto events = clipboardEvents(shareEvents);
        return !events.empty() && events.back().second.result == RCS_OK && (events.back().second.left_out & files);
    }));
    CHECK(updateFiles(view, fileSettings(bothWays)) == RCS_OK);
    CHECK(waitFor([&] { return filePeer(share, 1).flow == bothWays; }));

    // Fetched ahead (small files only): placed as themselves once here.
    viewClipboard.files_prefetch_bytes = 1u << 20;
    std::snprintf(viewClipboard.files_directory, sizeof(viewClipboard.files_directory), "%s", utf8Of(ahead).c_str());
    CHECK(updateClipboard(view, viewClipboard) == RCS_OK);
    sharerBoard.copy(filesCopy({entryOf(out / "report.txt")}));
    CHECK(waitFor([&] {
        const auto held = viewerBoard.held();
        return held.files && !held.files->entries[0].path.empty();
    }));
    const auto whole = viewerBoard.held().files->entries[0];
    CHECK(fs::path(whole.path).parent_path().parent_path() == ahead && readText(fs::path(whole.path)) == report);
    CHECK(waitFor([&] {
        const auto events = pasteEvents(viewEvents);
        return !events.empty() && events.back().second.ahead && events.back().second.result == RCS_OK;
    }));
    // Not ahead: more than the most, or a directory.
    sharerBoard.copy(filesCopy({entryOf(out / "photos")}));
    CHECK(waitFor([&] {
        const auto held = viewerBoard.held();
        return held.files && held.files->entries[0].name == "photos";
    }));
    std::this_thread::sleep_for(300ms);
    CHECK(viewerBoard.held().files->entries[0].path.empty());

    // The viewer copies files: the sharer pastes them (both ways go).
    viewerBoard.copy(filesCopy({entryOf(out / "photos" / "a.jpg")}));
    CHECK(waitFor([&] {
        const auto held = sharerBoard.held();
        return held.files && held.files->entries[0].name == "a.jpg";
    }));
    fs::create_directories(scratch / "share-pasted");
    const auto back = sharerBoard.pasteInto(utf8Of(scratch / "share-pasted"));
    CHECK(back && back->result == RCS_OK && readText(scratch / "share-pasted" / "a.jpg") == photo);
    CHECK(!viewEvents.overlapping && !shareEvents.overlapping);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);

    // A host that takes copies itself (target HOST): on_clipboard has the
    // publication, and the host fetches its files where it likes.
    {
        ClipboardLog hostSeen;
        auto hostClipboard = filesClipboard(RCS_CLIPBOARD_RECEIVE);
        hostClipboard.target = RCS_CLIPBOARD_HOST;
        hostSeen.into(hostClipboard);
        auto shareAgain = config(RCS_ROLE_SHARE, 47979, key);
        auto viewAgain = config(RCS_ROLE_VIEW, 47979, key);
        EventLog hostEvents;
        rcsdk::Callbacks hostCallbacks;
        hostCallbacks.onEvent = hostEvents.callback();
        rcsdk::Session sharer(withFiles(withClipboard(shareAgain, filesClipboard(RCS_CLIPBOARD_SEND)),
                                        fileSettings(RCS_FILE_SEND)));
        rcsdk::Session host(withFiles(withClipboard(viewAgain, hostClipboard), fileSettings(RCS_FILE_RECEIVE)),
                            hostCallbacks);
        CHECK(sharer.start() == RCS_OK);
        CHECK(waitFor([&] { return sharer.state() == RCS_STATE_LISTENING; }));
        CHECK(host.start() == RCS_OK);
        CHECK(waitFor([&] { return clipboardPeer(sharer, 1).formats & files; }));
        std::this_thread::sleep_for(300ms);
        sharerBoard.copy(filesCopy({entryOf(out / "photos")}));
        CHECK(waitFor([&] { return hostSeen.lastFiles(RCS_CLIPBOARD_RECEIVE).has_value(); }));
        const auto offered = *hostSeen.lastFiles(RCS_CLIPBOARD_RECEIVE);
        fs::create_directories(scratch / "host");
        rcs_file_fetch_request r = RCS_FILE_FETCH_REQUEST_INIT;
        std::copy(offered.publication.begin(), offered.publication.end(), r.publication);
        const std::string directory = utf8Of(scratch / "host");
        r.directory = directory.c_str();
        uint64_t transfer = 0;
        CHECK(rcs_file_fetch(host.handle(), &r, &transfer) == RCS_OK);
        CHECK(waitDone(hostEvents, transfer).end == RCS_FILE_COMPLETED &&
              readText(scratch / "host" / "photos" / "sub" / "b.txt") == "bee");
        CHECK(host.stop() == RCS_OK);
        CHECK(sharer.stop() == RCS_OK);
    }
    rcsdk::clipboard::setFactory({});
}

// A sharer whose agent runs through the agent link (here in the same
// process): the agent's machine half says its clipboard carries files, so
// the sharer takes them; a paste there fetches into the agent's machine; an
// agent that leaves takes FILES with it until another attaches.
void clipboardFilesAgent(const uint8_t (&key)[32]) {
    Scratch scratch("clipboard-files-agent");
    const fs::path out = scratch / "out", pasted = scratch / "pasted";
    fs::create_directories(pasted);
    writeText(out / "from-viewer.txt", "to the agent's machine");
    FakeClipboard sharerBoard, viewerBoard;
    rcsdk::clipboard::setFactory([&](rcs_role role) -> std::unique_ptr<rcsdk::clipboard::SystemClipboard> {
        return std::make_unique<FakeClipboardAccess>(role == RCS_ROLE_SHARE ? sharerBoard : viewerBoard);
    });
    constexpr uint32_t both = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
    const uint32_t files = 1u << RCS_CLIPBOARD_FILES;
    const std::string endpoint = agentEndpoint("clipboard-files");
    const auto linkKey = rcsdk::generateConnectionKey();
    auto shareConfig = config(RCS_ROLE_SHARE, 47995, key).use(rcsdk::agentLink(endpoint, linkKey));
    auto viewConfig = config(RCS_ROLE_VIEW, 47995, key);
    rcsdk::Session share(withFiles(withClipboard(shareConfig, filesClipboard(both)), fileSettings(bothWays)));
    rcsdk::Session view(withFiles(withClipboard(viewConfig, filesClipboard(both)), fileSettings(bothWays)));
    CHECK(share.start() == RCS_OK);
    auto agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
    CHECK(agent->start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return clipboardPeer(view).formats & files; }));
    std::this_thread::sleep_for(300ms);
    viewerBoard.copy(filesCopy({entryOf(out / "from-viewer.txt")}));
    CHECK(waitFor([&] { return sharerBoard.held().files.has_value(); }));
    const auto into = sharerBoard.pasteInto(utf8Of(pasted));
    CHECK(into && into->result == RCS_OK && readText(pasted / "from-viewer.txt") == "to the agent's machine");
    // The agent leaves: nothing places files at the sharer's machine now.
    agent.reset();
    CHECK(waitFor([&] { return !(clipboardPeer(view).formats & files); }));
    agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
    CHECK(agent->start() == RCS_OK);
    CHECK(waitFor([&] { return clipboardPeer(view).formats & files; }, 10s));
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    agent.reset();
    rcsdk::clipboard::setFactory({});
}

// A sharer with two viewers: its files go to each by a publication of that
// viewer's own (one viewer's is no other's); a viewer's files placed on the
// sharer's clipboard go on to the other viewer without them.
void clipboardFilesViewers(const uint8_t (&key)[32]) {
    Scratch scratch("clipboard-files-viewers");
    const fs::path out = scratch / "out";
    writeText(out / "note.txt", "for both");
    FakeClipboard sharerBoard, firstBoard, secondBoard;
    std::atomic<int> views{0};
    rcsdk::clipboard::setFactory([&](rcs_role role) -> std::unique_ptr<rcsdk::clipboard::SystemClipboard> {
        if (role == RCS_ROLE_SHARE) {
            return std::make_unique<FakeClipboardAccess>(sharerBoard);
        }
        return std::make_unique<FakeClipboardAccess>(views++ == 0 ? firstBoard : secondBoard);
    });
    constexpr uint32_t both = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
    const uint32_t files = 1u << RCS_CLIPBOARD_FILES;
    ClipboardLog shareSeen;
    auto shareClipboard = filesClipboard(both);
    shareSeen.into(shareClipboard);
    auto shareConfig = config(RCS_ROLE_SHARE, 47981, key).setViewers(2);
    rcsdk::Session share(withFiles(withClipboard(shareConfig, shareClipboard), fileSettings(bothWays)));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    std::vector<std::unique_ptr<TestViewer>> viewers;
    std::vector<std::unique_ptr<ClipboardLog>> seen;
    for (int i = 0; i < 2; ++i) {
        seen.push_back(std::make_unique<ClipboardLog>());
        auto viewClipboard = filesClipboard(both);
        seen.back()->into(viewClipboard);
        auto viewConfig = config(RCS_ROLE_VIEW, 47981, key);
        viewers.push_back(
            std::make_unique<TestViewer>(withFiles(withClipboard(viewConfig, viewClipboard), fileSettings(bothWays))));
        CHECK(viewers.back()->session->start() == RCS_OK);
        CHECK(waitFor([&] { return share.viewers().size() == size_t(i + 1); }));
    }
    const rcs_viewer_id firstId = share.viewers()[0].viewer_id, secondId = share.viewers()[1].viewer_id;
    CHECK(waitFor([&] {
        return (clipboardPeer(share, firstId).formats & files) && (clipboardPeer(share, secondId).formats & files);
    }));
    std::this_thread::sleep_for(300ms);
    sharerBoard.copy(filesCopy({entryOf(out / "note.txt")}));
    CHECK(waitFor([&] { return firstBoard.held().files && secondBoard.held().files; }));
    const auto first = seen[0]->lastFiles(RCS_CLIPBOARD_RECEIVE), second = seen[1]->lastFiles(RCS_CLIPBOARD_RECEIVE);
    CHECK(first && second && first->publication != second->publication);
    // The second viewer fetches what was published to the first: refused.
    EventLog& secondEvents = viewers[1]->events;
    rcs_file_fetch_request r = RCS_FILE_FETCH_REQUEST_INIT;
    std::copy(first->publication.begin(), first->publication.end(), r.publication);
    r.mode = RCS_FILE_FETCH_READ;
    r.on_read = [](uint64_t, uint64_t, rcs_result, const uint8_t*, uint32_t, void*) {};
    uint64_t transfer = 0;
    CHECK(rcs_file_fetch(viewers[1]->session->handle(), &r, &transfer) == RCS_OK);
    const auto refused = waitDone(secondEvents, transfer);
    CHECK(refused.end == RCS_FILE_REJECTED && refused.result == RCS_ERROR_INVALID_STATE);
    // Its own goes.
    fs::create_directories(scratch / "second");
    const auto pasted = secondBoard.pasteInto(utf8Of(scratch / "second"));
    CHECK(pasted && pasted->result == RCS_OK && readText(scratch / "second" / "note.txt") == "for both");
    // The first viewer's files (with text) are placed at the sharer; the
    // second gets the text alone.
    auto mine = filesCopy({entryOf(out / "note.txt")});
    mine.items.push_back(clipboardItem(RCS_CLIPBOARD_TEXT, "from the first"));
    firstBoard.copy(mine);
    CHECK(waitFor([&] { return sharerBoard.text() == "from the first" && sharerBoard.held().files; }));
    CHECK(waitFor([&] { return secondBoard.text() == "from the first"; }));
    std::this_thread::sleep_for(300ms);
    CHECK(!secondBoard.held().files);
    for (auto& viewer : viewers) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
    rcsdk::clipboard::setFactory({});
}

// ---- Network changes (rcs_paths_config) -------------------------------------------------

// What the paths' scenarios tune: a path stalls after half a second of
// silence and is lost after two; a sharer keeps a lost viewer `holdMs`.
template <class Config>
rcsdk::SessionConfig& quickPaths(Config&& c, uint32_t holdMs = 30'000) {
    auto& network = c.template extension<rcs_network_tuning_config>();
    network.idle_timeout_ms = 2000;
    network.path_stall_ms = 500;
    network.path_hold_ms = holdMs;
    return c;
}

// The PATH events of `log` that are `change` (and of `cause`, unless 0), of `viewer`.
std::vector<rcs_event> pathEvents(EventLog& log, rcs_path_change change, rcs_path_cause cause = 0,
                                  rcs_viewer_id viewer = 0) {
    auto events = eventsOf(log, RCS_EVENT_PATH, viewer);
    std::erase_if(events,
                  [&](const rcs_event& e) { return e.path.change != change || (cause && e.path.cause != cause); });
    return events;
}

// The viewer's NAT remaps it, then the viewer itself changes networks (its
// address stops working, and the system says so): the connection follows
// both. Still one connection: no new viewer and no new admission, the data
// messages arrive once each and in order, the viewer stays RUNNING, and both
// hosts hear of each change.
void migration(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    DataLog shareData;
    std::atomic<int> joins{0};
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    shareCallbacks.onData = shareData.callback();
    shareCallbacks.onViewerJoin = [&](const rcs_viewer_info&, rcs_viewer_decision&) {
        ++joins;
    };
    rcsdk::Session share(quickPaths(config(RCS_ROLE_SHARE, 47963, key)), shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47963));
    TestViewer viewer(quickPaths(reconnecting(config(RCS_ROLE_VIEW, proxy.address().port(), key), 30000, 500)));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(viewer.session->start() == RCS_OK && viewer.watching());
    std::atomic<bool> sending{true};
    std::atomic<int> sent{0};
    std::thread sender([&] {
        while (sending) {
            if (viewer.session->sendData(1, std::to_string(sent.load())) == RCS_OK) {
                ++sent;
            }
            std::this_thread::sleep_for(5ms);
        }
    });

    // The NAT remaps the viewer: the sharer follows it to its new port.
    const uint32_t before = share.viewers().at(0).port;
    proxy.rebind();
    CHECK(waitFor([&] { return !pathEvents(shareEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_REBINDING, 1).empty(); }));
    CHECK(viewer.watching());
    CHECK(share.viewers().at(0).port == proxy.backAddress().port() && share.viewers()[0].port != before);
    {
        const auto changed = pathEvents(shareEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_REBINDING, 1).at(0).path;
        CHECK(changed.from.route == RCS_ROUTE_DIRECT && changed.to.route == RCS_ROUTE_DIRECT);
        CHECK(std::string(changed.to.remote_address) == proxy.backAddress().toString() && !changed.signaled);
    }

    // The viewer's own address stops working, and the system says the network changed.
    proxy.block(proxy.viewers().back());
    rcsdk::notifyNetworkChange();
    CHECK(waitFor([&] { return !pathEvents(viewer.events, RCS_PATH_CHANGED, RCS_PATH_CAUSE_NETWORK).empty(); }));
    CHECK(!pathEvents(viewer.events, RCS_PATH_CHANGING, RCS_PATH_CAUSE_NETWORK).empty());
    CHECK(waitFor([&] { return !pathEvents(shareEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_PEER, 1).empty(); }));
    CHECK(viewer.watching());
    const auto moved = viewer.session->pathInfo();
    CHECK(moved.route == RCS_ROUTE_DIRECT && std::string(moved.remote_address) == proxy.address().toString());
    CHECK(std::string(moved.local_address) != proxy.viewers().front().toString());
    CHECK(std::string(share.pathInfo(1).remote_address) == proxy.backAddress().toString());

    sending = false;
    sender.join();
    CHECK(waitFor([&] { return shareData.count(true) == size_t(sent.load()); }));
    {
        std::lock_guard lock(shareData.mutex);
        for (size_t i = 0; i < shareData.reliable.size(); ++i) {
            CHECK(std::string(shareData.reliable[i].begin(), shareData.reliable[i].end()) == std::to_string(i));
        }
    }
    for (auto* session : {&share, viewer.session.get()}) { // Published a few times a second.
        CHECK(waitFor([&] { return session->stats<rcs_network_stats>().path_changes >= 1; }));
        const auto network = session->stats<rcs_network_stats>();
        CHECK(network.connections == 1 && network.route == RCS_ROUTE_DIRECT && !network.path_lost);
    }
    CHECK(joins == 1 && shareEvents.of(RCS_EVENT_VIEWER).size() == 1);
    const auto states = viewer.events.of(RCS_EVENT_STATE);
    CHECK(std::none_of(states.begin(), states.end(),
                       [](const rcs_event& e) { return e.state.value == RCS_STATE_RECONNECTING; }));
    CHECK(viewer.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// The path goes away for longer than idle_timeout: the viewer reports
// RECONNECTING, the sharer keeps it; once the path is back the same
// connection goes on: RUNNING again, without new PEER or STREAM events. The
// key the viewer held when the path stalled is released; its control is
// revoked while the path is lost, for the host to grant again, unless the host
// has it restored (restore_control).
void pathHold(const uint8_t (&key)[32]) {
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    {
        EventLog shareEvents;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        rcsdk::Session share(quickPaths(config(RCS_ROLE_SHARE, 47964, key).setControl(true)), shareCallbacks);
        rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47964));
        TestViewer viewer(quickPaths(reconnecting(config(RCS_ROLE_VIEW, proxy.address().port(), key), 30000, 500)));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(viewer.session->start() == RCS_OK && viewer.watching());
        CHECK(waitFor([&] { return viewer.session->controlInfo().granted != 0; }));
        rcs_input_event press = RCS_INPUT_EVENT_INIT;
        press.type = RCS_INPUT_KEY;
        press.code = 4;
        press.down = 1;
        CHECK(viewer.session->sendInput(press) == RCS_OK && waitFor([&] { return recorder.count() == 1; }));
        const int releases = recorder.releases;

        proxy.outage = true;
        CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RECONNECTING; }, 5s));
        CHECK(viewer.events.of(RCS_EVENT_STATE).back().state.result == RCS_ERROR_NETWORK);
        CHECK(!pathEvents(viewer.events, RCS_PATH_CHANGING, RCS_PATH_CAUSE_STALLED).empty() &&
              !pathEvents(viewer.events, RCS_PATH_LOST).empty());
        CHECK(waitFor([&] { return !pathEvents(shareEvents, RCS_PATH_LOST, 0, 1).empty(); }));
        CHECK(recorder.releases > releases); // At the stall.
        CHECK(share.state() == RCS_STATE_RUNNING && share.viewers().size() == 1);
        CHECK(waitFor([&] { return share.viewerStats<rcs_network_stats>(1).path_lost == 1; }));
        CHECK(waitFor([&] { return !share.viewers().at(0).control.allowed; })); // Revoked.
        CHECK(viewer.session->sendInput(press) == RCS_ERROR_INVALID_STATE);

        proxy.outage = false;
        CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RUNNING; }, 5s) && viewer.watching());
        CHECK(!pathEvents(viewer.events, RCS_PATH_CHANGED, RCS_PATH_CAUSE_STALLED).empty());
        const auto back = pathEvents(viewer.events, RCS_PATH_CHANGED).back().path;
        CHECK(back.interruption_ns >= 2'000'000'000ull);
        CHECK(viewer.events.of(RCS_EVENT_PEER).size() == 1);
        const auto streams = viewer.events.of(RCS_EVENT_STREAM);
        CHECK(std::count_if(streams.begin(), streams.end(),
                            [](const rcs_event& e) { return (e.stream.changes & RCS_STREAM_CODEC) != 0; }) == 1);
        CHECK(viewer.session->stats<rcs_network_stats>().connections == 1 &&
              share.stats<rcs_network_stats>().connections == 1);
        CHECK(waitFor([&] { return !share.viewerStats<rcs_network_stats>(1).path_lost; }));
        std::this_thread::sleep_for(300ms);
        CHECK(!viewer.session->controlInfo().granted); // Until the host grants it again.
        CHECK(share.setViewerControl(1, true) == RCS_OK);
        CHECK(waitFor([&] { return viewer.session->controlInfo().granted != 0; }));

        // The host has it restored: the next lost path gives it back by itself.
        auto paths = rcsdk::defaults<rcs_paths_config>();
        paths.restore_control = 1;
        CHECK(share.update(paths) == RCS_OK);
        proxy.outage = true;
        CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RECONNECTING; }, 5s));
        proxy.outage = false;
        CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RUNNING; }, 5s));
        CHECK(waitFor([&] { return viewer.session->controlInfo().granted != 0; }));
        CHECK(waitFor([&] { return share.stats<rcs_network_stats>().path_changes >= 2; }));
        CHECK(viewer.session->stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::seams::setInputOverride({});
}

// A sharer keeps a lost viewer for path_hold_ms only. Once the viewer's path
// is back it hears that the sharer no longer knows its connection, and
// connects anew at once, not after the rest of its reconnect window.
void pathHoldExpired(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(quickPaths(config(RCS_ROLE_SHARE, 47965, key).setKeepListening(true), 1000), shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47965));
    TestViewer viewer(quickPaths(reconnecting(config(RCS_ROLE_VIEW, proxy.address().port(), key), 30000, 5000)));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(viewer.session->start() == RCS_OK && viewer.watching());
    proxy.outage = true;
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 6s));
    {
        const auto left = shareEvents.of(RCS_EVENT_VIEWER).back().viewer;
        CHECK(!left.joined && left.result == RCS_ERROR_NETWORK);
    }
    CHECK(viewer.session->state() == RCS_STATE_RECONNECTING);
    const auto restored = std::chrono::steady_clock::now();
    proxy.outage = false;
    CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RUNNING; }, 5s) && viewer.watching());
    CHECK(std::chrono::steady_clock::now() - restored < 4s); // Not after its 5 s interval.
    CHECK(viewer.session->stats<rcs_network_stats>().connections == 2 &&
          share.stats<rcs_network_stats>().connections == 2);
    CHECK(viewer.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// HOST: the host's stream is replaced by a new one to the same end, cut in
// the middle of a datagram (rcs_session_transport_restarted): the connection
// goes on over it, still the one connection, and both ends hear of it.
void hostRestart(const uint8_t (&key)[32]) {
    rcsdk::test::TestChannel channel(RCS_HOST_CHANNEL_STREAM);
    std::atomic<int> frames{0};
    EventLog shareEvents, viewEvents;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    rcsdk::test::FunctionTransport shareSend(channel.sender(0));
    rcsdk::test::FunctionTransport viewSend(channel.sender(1));
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
    };
    viewCallbacks.onEvent = viewEvents.callback();
    rcsdk::Session share(hostConfig(RCS_ROLE_SHARE, key, RCS_HOST_CHANNEL_STREAM, shareSend), shareCallbacks);
    rcsdk::Session view(hostConfig(RCS_ROLE_VIEW, key, RCS_HOST_CHANNEL_STREAM, viewSend), viewCallbacks);
    const auto shareEnd = channel.attachment(0, share);
    const auto viewEnd = channel.attachment(1, view);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return frames >= 10; }, 10s));
    for (int restart = 1; restart <= 3; ++restart) {
        channel.restart();
        const int before = frames;
        CHECK(waitFor([&] { return frames >= before + 10; }, 10s));
        CHECK(waitFor([&] {
            return pathEvents(viewEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_HOST).size() == size_t(restart) &&
                   pathEvents(shareEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_HOST, 1).size() == size_t(restart);
        }));
    }
    const auto path = pathEvents(viewEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_HOST).back().path;
    CHECK(path.to.route == RCS_ROUTE_HOST && !path.to.remote_address[0]);
    CHECK(view.state() == RCS_STATE_RUNNING && view.stats<rcs_network_stats>().connections == 1 &&
          share.stats<rcs_network_stats>().connections == 1);
    CHECK(channel.unexpected == 0);
    CHECK(rcsdk::transportRestarted(share, 1) == RCS_OK);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
    rcsdk::Session tcp(config(RCS_ROLE_VIEW, 47966, key).setTransport(RCS_TRANSPORT_TCP));
    CHECK(rcsdk::transportRestarted(tcp, 0) == RCS_ERROR_UNSUPPORTED);
}

// HOST with several viewers, each over its own channel to the sharer (route
// 1, 2, 3): all three watch at once and name their routes; one channel
// closes and only its viewer goes, the others keep their picture; a new
// viewer on that route joins again. Over a lossy datagram channel and a
// stream cut into pieces.
void hostViewers(const uint8_t (&key)[32]) {
    for (const rcs_host_channel kind : {RCS_HOST_CHANNEL_DATAGRAM, RCS_HOST_CHANNEL_STREAM}) {
        rcsdk::test::TestChannel channel(kind);
        if (kind == RCS_HOST_CHANNEL_DATAGRAM) {
            channel.loss = 0.02;
            channel.reorder = 0.05;
        } else {
            channel.maxPiece = 2000;
        }
        rcsdk::test::FunctionTransport shareSend(channel.sender(0));
        rcsdk::Session share(hostConfig(RCS_ROLE_SHARE, key, kind, shareSend).setKeepListening(true));
        const auto shareEnd = channel.attachment(0, share);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        struct Watcher {
            std::atomic<int> frames{0};
            std::unique_ptr<rcsdk::test::FunctionTransport> send;
            std::unique_ptr<rcsdk::Session> session;
            std::unique_ptr<rcsdk::test::TestChannel::Attachment> end;
        };
        std::array<Watcher, 4> watchers; // Ends 1..3.
        const auto watch = [&](int end) {
            auto& w = watchers[end];
            w.end.reset();
            w.session.reset();
            w.frames = 0;
            w.send = std::make_unique<rcsdk::test::FunctionTransport>(channel.sender(end));
            rcsdk::Callbacks callbacks;
            callbacks.onFrame = [&w](const rcs_frame&) { ++w.frames; };
            w.session = std::make_unique<rcsdk::Session>(hostConfig(RCS_ROLE_VIEW, key, kind, *w.send), callbacks);
            w.end = channel.attachment(end, *w.session);
            CHECK(w.session->start() == RCS_OK);
        };
        for (int end = 1; end <= 3; ++end) {
            watch(end);
        }
        CHECK(waitFor([&] { return watchers[1].frames >= 20 && watchers[2].frames >= 20 && watchers[3].frames >= 20; },
                      20s));
        std::set<uint64_t> routes;
        for (const auto& viewer : share.viewers()) {
            routes.insert(viewer.route);
        }
        CHECK(routes == std::set<uint64_t>({1, 2, 3}));
        // Viewer 2's channel closes: it goes (NETWORK), 1 and 3 keep watching.
        channel.close(2);
        CHECK(waitFor([&] { return watchers[2].session->state() == RCS_STATE_FAILED; }, 10s));
        CHECK(watchers[2].session->failure().result == RCS_ERROR_NETWORK);
        CHECK(waitFor([&] { return share.viewers().size() == 2; }, 10s));
        const int one = watchers[1].frames, three = watchers[3].frames;
        CHECK(waitFor([&] { return watchers[1].frames >= one + 20 && watchers[3].frames >= three + 20; }, 10s));
        CHECK(watchers[1].session->state() == RCS_STATE_RUNNING && watchers[3].session->state() == RCS_STATE_RUNNING);
        // A new viewer on route 2 joins again.
        watch(2);
        CHECK(waitFor([&] { return watchers[2].frames >= 10; }, 15s));
        CHECK(waitFor([&] { return share.viewers().size() == 3; }));
        CHECK(share.stats<rcs_network_stats>().connections == 4);
        for (int end = 1; end <= 3; ++end) {
            CHECK(watchers[end].session->stats<rcs_network_stats>().connections == 1);
            CHECK(watchers[end].session->stop() == RCS_OK);
        }
        CHECK(channel.unexpected == 0);
        CHECK(share.stop() == RCS_OK);
        for (auto& w : watchers) {
            w.end.reset(); // Before the sessions go.
        }
    }
}

// ---- Through a forwarder (RCS_TRANSPORT_FORWARDER) -------------------------------------------

#ifdef RCSDK_TEST_FORWARDER
// A forwarder on loopback ports of its own (UDP, TCP, WebSocket), its
// identity and a secret its tokens are signed with; what it reported.
struct TestForwarder {
    rcsfwd_secret secret{7, {}};
    std::array<uint8_t, 32> keySha256{};
    std::mutex mutex; // Before the forwarder: its last events come as it stops.
    std::vector<rcsfwd_event> events;
    std::unique_ptr<rcsdk::forwarder::Forwarder> forwarder;
    uint16_t port = 0;

    // `tune`: settings other than the defaults.
    explicit TestForwarder(const std::function<void(rcsdk::forwarder::Settings&)>& tune = {}) {
        rcsdk::forwarder::Settings s;
        const auto loopback = rcsdk::transport::Endpoint::parse("127.0.0.1", 0);
        s.listenUdp = s.listenTcp = s.listenWebSocket = loopback;
        s.identity = rcsdk::crypto::DeviceKey::generate();
        rcsdk::crypto::random(secret.secret);
        s.secrets = {secret};
        if (tune) {
            tune(s);
        }
        s.onEvent = [](const rcsfwd_event* event, void* self) {
            auto& f = *static_cast<TestForwarder*>(self);
            std::lock_guard lock(f.mutex);
            f.events.push_back(*event);
        };
        s.userData = this;
        const rcsdk::crypto::DeviceKey pair(s.identity);
        keySha256 = rcsdk::crypto::sha256(pair.publicKey());
        forwarder = std::make_unique<rcsdk::forwarder::Forwarder>(std::move(s));
        forwarder->start();
        port = forwarder->port();
    }
    std::string token(rcsfwd_role role, const std::string& room, uint64_t route, int64_t expiresIn = 3600) const {
        rcsdk::forwarder::Grant grant;
        grant.room = room;
        grant.role = role;
        grant.route = route;
        grant.expires = uint64_t(int64_t(rcsdk::forwarder::unixNow()) + expiresIn);
        grant.audience = keySha256;
        return rcsdk::forwarder::sign(secret, grant);
    }
    rcsdk::SessionConfig config(rcs_role role, const uint8_t (&key)[32], const std::string& token,
                                rcs_forwarder_link link = RCS_FORWARDER_UDP) const {
        const uint16_t entry = link == RCS_FORWARDER_TCP ? forwarder->port(RCSFWD_ENTRY_TCP) : port;
        return ::config(role, port, key).setForwarder("127.0.0.1", entry, keySha256, token, link);
    }
    // Over its WebSocket entry (ws://).
    rcsdk::SessionConfig webSocket(rcs_role role, const uint8_t (&key)[32], const std::string& token) const {
        return ::config(role, port, key)
            .setForwarderWebSocket("ws://127.0.0.1:" + std::to_string(forwarder->port(RCSFWD_ENTRY_WEBSOCKET)) + "/rcs",
                                   keySha256, token);
    }
    size_t count(rcsfwd_event_type type) {
        std::lock_guard lock(mutex);
        return size_t(std::count_if(events.begin(), events.end(), [&](const rcsfwd_event& e) { return e.type == type; }));
    }
};

// A host's own channel to a forwarder's entry (RCS_FORWARDER_HOST), as a
// browser's page opens it: a WebSocket to its WebSocket entry, each datagram
// one binary message, masked as a browser masks it (DATAGRAM), or a TCP
// connection to its TCP entry, which takes the SDK's STREAM framing as it
// comes. A thread of its own reads and hands what arrives to the session, and
// tells it when the forwarder closed the connection; close() ends it as a
// page's closed WebSocket does (rcs_session_transport_closed), open() makes a
// new one. Declared before the session it carries.
class PageChannel final : public rcsdk::HostTransport {
public:
    PageChannel(bool webSocket, uint16_t port) : webSocket_(webSocket), port_(port) {}
    ~PageChannel() { close(false); }
    PageChannel(const PageChannel&) = delete;
    PageChannel& operator=(const PageChannel&) = delete;

    void attach(rcsdk::Session& session) { session_ = &session; }
    void open() {
        namespace ws = rcsdk::transport::websocket;
        static const std::atomic<bool> never{false};
        const std::string what = "the forwarder's entry";
        const auto entry = rcsdk::transport::Endpoint::parse("127.0.0.1", port_);
        auto stream = rcsdk::transport::tcpBytes(rcsdk::transport::connectStreamTcp(entry, what, never, 5s, 0), what);
        std::unique_ptr<ws::Decoder> frames;
        if (webSocket_) { // The opening, as a browser's: any path; the answer, and what came after it.
            const auto opening = ws::open("127.0.0.1:" + std::to_string(port_), "/page");
            std::span<const uint8_t> rest(reinterpret_cast<const uint8_t*>(opening.request.data()),
                                          opening.request.size());
            while (!rest.empty()) {
                rest = rest.subspan(stream->writeSome(rest));
            }
            rcsdk::Bytes answer, chunk(4096);
            for (;;) {
                rcsdk::transport::waitReady(stream->handle(), false, never, rcsdk::Clock::now() + 5s);
                const size_t taken = stream->readSome(chunk);
                answer.insert(answer.end(), chunk.begin(), chunk.begin() + ptrdiff_t(taken));
                if (const auto consumed = ws::opened(answer, opening.key)) {
                    frames = std::make_unique<ws::Decoder>(false);
                    frames->push(std::span<const uint8_t>(answer).subspan(*consumed));
                    break;
                }
            }
        }
        std::lock_guard lock(mutex_);
        stream_ = std::move(stream);
        frames_ = std::move(frames);
        stopping_ = false;
        reader_ = std::thread([this] { read(); });
    }
    void close(bool tell = true) {
        stopping_ = true;
        if (reader_.joinable()) {
            reader_.join();
        }
        {
            std::lock_guard lock(mutex_);
            stream_.reset();
            outbound_.clear();
        }
        if (tell) {
            rcsdk::transportClosed(*session_, 0);
        }
    }
    bool send(uint64_t, const uint8_t* data, uint32_t size) override {
        namespace ws = rcsdk::transport::websocket;
        std::lock_guard lock(mutex_);
        if (!stream_ || outbound_.size() > (1u << 20)) {
            return false; // No channel now, or no room: lost, as on a page.
        }
        const std::span<const uint8_t> datagram(data, size);
        const auto wire = webSocket_ ? ws::frame(ws::binary, datagram, true) : rcsdk::Bytes(data, data + size);
        outbound_.insert(outbound_.end(), wire.begin(), wire.end());
        try {
            flush();
        } catch (const rcsdk::Failure&) {
            // Broken: the reader finds out.
        }
        return true;
    }

private:
    void flush() { // Holding mutex_.
        size_t written = 0;
        while (written < outbound_.size()) {
            const size_t taken = stream_->writeSome(std::span<const uint8_t>(outbound_).subspan(written));
            if (!taken) {
                break;
            }
            written += taken;
        }
        outbound_.erase(outbound_.begin(), outbound_.begin() + ptrdiff_t(written));
    }
    void read() noexcept {
        rcsdk::Bytes chunk(64 * 1024);
        bool ended = false;
        while (!stopping_ && !ended) {
            rcsdk::transport::Handle handle;
            {
                std::lock_guard lock(mutex_);
                handle = stream_->handle();
            }
            const rcsdk::transport::Readiness readable{handle, false};
            rcsdk::transport::waitAny(std::span(&readable, 1), 10ms);
            std::vector<rcsdk::Bytes> arrived;
            {
                std::lock_guard lock(mutex_);
                try {
                    flush();
                    if (const size_t taken = stream_->readSome(chunk)) {
                        const auto bytes = std::span<const uint8_t>(chunk).first(taken);
                        if (frames_) {
                            frames_->push(bytes);
                            while (auto event = frames_->next()) {
                                ended = ended || event->kind == rcsdk::transport::websocket::close;
                                if (event->kind == rcsdk::transport::websocket::binary) {
                                    arrived.push_back(std::move(event->payload));
                                }
                            }
                        } else {
                            arrived.emplace_back(bytes.begin(), bytes.end());
                        }
                    }
                } catch (const rcsdk::Failure&) {
                    ended = true; // The forwarder closed it, or it broke.
                }
            }
            for (const auto& bytes : arrived) {
                rcsdk::transportReceive(*session_, 0, bytes.data(), uint32_t(bytes.size()));
            }
        }
        if (ended && !stopping_) {
            rcsdk::transportClosed(*session_, 0);
        }
    }

    const bool webSocket_;
    const uint16_t port_;
    rcsdk::Session* session_ = nullptr;
    std::mutex mutex_; // Below.
    std::unique_ptr<rcsdk::transport::ByteStream> stream_;
    std::unique_ptr<rcsdk::transport::websocket::Decoder> frames_;
    rcsdk::Bytes outbound_;
    std::atomic<bool> stopping_{false};
    std::thread reader_;
};
#endif

// A sharer and three viewers through a forwarder: each picture goes up once
// (the forwarder sends about three times what the sharer sends it), sealed
// with the stream key, which steps on as each viewer is let in; each viewer
// decodes and is known by the route its token names; data passes the pipes
// end to end. Pictures the forwarder changes are refused and counted, and the
// stream goes on. A viewer that leaves makes a new generation of the key,
// which the others take within a second or two and decode on.
void forwarderGroup(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    TestForwarder fwd;
    std::mutex mutex;
    std::vector<std::string> fromViewers;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onData = [&](const rcs_data_message& message) {
        std::lock_guard lock(mutex);
        fromViewers.emplace_back(reinterpret_cast<const char*>(message.data), message.size);
    };
    rcsdk::Session share(fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "class", 0)).setViewers(4),
                         shareCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    std::vector<std::unique_ptr<TestViewer>> viewers;
    for (uint64_t route : {101, 102, 103}) {
        viewers.push_back(std::make_unique<TestViewer>(
            fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "class", route))));
        CHECK(viewers.back()->session->start() == RCS_OK);
        CHECK(viewers.back()->watching());
    }
    std::set<uint64_t> routes;
    for (const auto& viewer : share.viewers()) {
        routes.insert(viewer.route);
    }
    CHECK(routes == std::set<uint64_t>({101, 102, 103}));
    for (auto& viewer : viewers) {
        CHECK(viewer->watching());
        const auto f = viewer->session->stats<rcs_forwarder_stats>();
        CHECK(f.media_frames > 0 && f.media_rejected == 0 && f.media_replayed == 0 && f.peer != 0);
        CHECK(f.generation == 1 && f.link == RCS_FORWARDER_UDP && f.hop_round_trip_ns > 0 && f.hop_bytes > 0);
        CHECK(std::string(viewer->session->stats<rcs_network_stats>().path).find("forwarder") != std::string::npos);
    }
    // Up once, out three times (over a second of all three watching).
    std::this_thread::sleep_for(1s);
    const auto before = fwd.forwarder->roomStats("class");
    std::this_thread::sleep_for(1s);
    rcsfwd_room_stats room = fwd.forwarder->roomStats("class");
    const auto sent = share.stats<rcs_forwarder_stats>();
    const uint64_t up = room.media_bytes - before.media_bytes, out = room.sent_bytes - before.sent_bytes;
    std::printf("forwarder: %llu bytes up, %llu out in a second; %llu viewers started on what was kept\n",
                (unsigned long long)up, (unsigned long long)out, (unsigned long long)room.cache_starts);
    CHECK(room.sharer == 1 && room.viewers == 3 && up > 0);
    CHECK(out > 2 * up && out <= 3 * up + 3 * 1500);
    CHECK(sent.media_frames > 0 && sent.media_bytes <= room.media_bytes + 1'000'000 && sent.generation == 1);
    // The newcomers started on what the forwarder kept: no keyframe of the sharer's for them.
    CHECK(room.cache_starts >= 1 && share.stats<rcs_group_stats>().keyframes_joined == 0);
    // The sharer's sound, encoded once and sent up once, reaches each of them.
    for (auto& viewer : viewers) {
        CHECK(waitFor([&] { return viewer->session->stats<rcs_audio_receive_stats>().received_packets > 20; }, 5s));
    }
    const auto voice = share.stats<rcs_audio_send_stats>();
    std::printf("forwarder: the sound: %llu packets encoded, %llu sealed and sent up, %llu into the room\n",
                (unsigned long long)voice.encoded_packets, (unsigned long long)sent.sound_packets,
                (unsigned long long)room.sound_packets);
    CHECK(sent.sound_packets > 0 && voice.encoded_packets <= sent.sound_packets + 20);
    for (const auto& v : fwd.forwarder->viewers("class")) {
        CHECK(v.sound == 1 && v.entry == RCSFWD_ENTRY_UDP);
    }
    // Data through the pipes, end to end.
    CHECK(viewers[0]->session->sendData(3, "through") == RCS_OK);
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return !fromViewers.empty() && fromViewers.back() == "through";
    }));
    // The forwarder changes a byte of every fifth picture: refused, counted, and the stream goes on.
    std::atomic<uint64_t> changed{0};
    fwd.forwarder->setMediaHook([&](rcsdk::protocol::SealedVideo& video) {
        if (video.counter % 5 == 0) {
            video.sealed[video.sealed.size() / 2] ^= 0x40;
            ++changed;
        }
    });
    CHECK(waitFor([&] { return viewers[0]->session->stats<rcs_forwarder_stats>().media_rejected >= 3; }));
    fwd.forwarder->setMediaHook({});
    for (auto& viewer : viewers) {
        CHECK(viewer->watching());
        CHECK(viewer->session->state() == RCS_STATE_RUNNING);
    }
    std::printf("forwarder: %llu pictures changed on the way, viewer 1 refused %llu\n", (unsigned long long)changed.load(),
                (unsigned long long)viewers[0]->session->stats<rcs_forwarder_stats>().media_rejected);
    // Viewer 3 leaves: a new generation, which the two that stay open.
    CHECK(viewers[2]->session->stop() == RCS_OK);
    CHECK(waitFor([&] { return share.viewers().size() == 2; }));
    CHECK(waitFor([&] {
        const auto f = share.stats<rcs_forwarder_stats>();
        return f.rekeys == 1 && f.generation == 2;
    }));
    for (int i = 0; i < 2; ++i) {
        CHECK(waitFor([&] { return viewers[i]->session->stats<rcs_forwarder_stats>().generation == 2; }));
        CHECK(viewers[i]->watching());
    }
    CHECK(fwd.count(RCSFWD_EVENT_JOINED) == 4 && fwd.count(RCSFWD_EVENT_LEFT) == 1);
    for (int i = 0; i < 2; ++i) {
        CHECK(viewers[i]->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
#else
    (void)key;
#endif
}

// What the forwarder and the ends refuse: an expired token, a token of the
// other role, a garbled one, a room the forwarder does not serve, a second
// sharer; a forwarder that proves another key; a room without its sharer.
void forwarderRefusals(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    TestForwarder fwd([](rcsdk::forwarder::Settings& s) { s.maxRooms = 1; });
    const auto failure = [&](rcsdk::SessionConfig c) {
        rcsdk::Session session(std::move(c));
        CHECK(session.start() == RCS_OK);
        CHECK(waitFor([&] { return session.state() == RCS_STATE_FAILED; }, 10s));
        return session.failure();
    };
    // A room without its sharer: as no sharer listening.
    auto lonely = failure(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "room", 1)));
    CHECK(lonely.result == RCS_ERROR_NETWORK && lonely.message.find("forwarder's room") != std::string::npos);
    rcsdk::Session share(fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "room", 0)));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    auto expired = failure(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "room", 1, -10)));
    CHECK(expired.result == RCS_ERROR_REFUSED && expired.message.find("expired") != std::string::npos);
    auto publishing = failure(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_PUBLISH, "room", 1)));
    CHECK(publishing.result == RCS_ERROR_REFUSED && publishing.message.find("sharer's") != std::string::npos);
    auto garbled = fwd.token(RCSFWD_ROLE_SUBSCRIBE, "room", 1);
    garbled[garbled.size() - 3] = garbled[garbled.size() - 3] == 'A' ? 'B' : 'A';
    auto forged = failure(fwd.config(RCS_ROLE_VIEW, key, garbled));
    CHECK(forged.result == RCS_ERROR_REFUSED && forged.message.find("signature") != std::string::npos);
    auto elsewhere = failure(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "other room", 1)));
    CHECK(elsewhere.result == RCS_ERROR_BUSY && elsewhere.message.find("as many rooms") != std::string::npos);
    // A token of the same secret for another forwarder.
    rcsdk::forwarder::Grant stranger{"room", RCSFWD_ROLE_SUBSCRIBE, 1, rcsdk::forwarder::unixNow() + 60,
                                     0,      fwd.keySha256};
    stranger.audience[0] ^= 1;
    auto astray = failure(fwd.config(RCS_ROLE_VIEW, key, rcsdk::forwarder::sign(fwd.secret, stranger)));
    CHECK(astray.result == RCS_ERROR_REFUSED && astray.message.find("another forwarder") != std::string::npos);
    auto second = failure(fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "room", 0)));
    CHECK(second.result == RCS_ERROR_REFUSED && second.message.find("has its sharer") != std::string::npos);
    auto viewing = failure(fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "room", 0)));
    CHECK(viewing.result == RCS_ERROR_REFUSED && viewing.message.find("viewer's") != std::string::npos);
    auto impostor = fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "room", 1));
    impostor.extension<rcs_forwarder_config>().forwarder_key_sha256[0] ^= 1;
    auto wrongKey = failure(std::move(impostor));
    CHECK(wrongKey.result == RCS_ERROR_AUTHENTICATION && wrongKey.message.find("another key") != std::string::npos);
    // Each refusal reached the host of the forwarder; the sharer was never disturbed.
    CHECK(fwd.count(RCSFWD_EVENT_REFUSED) == 7 && fwd.forwarder->stats().refused == 7);
    CHECK(share.state() == RCS_STATE_LISTENING);
    // And a viewer with a good token gets in.
    TestViewer viewer(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "room", 5)));
    CHECK(viewer.session->start() == RCS_OK);
    CHECK(viewer.watching());
    // The transport needs its extension, and the extension its transport; a link needs where it goes.
    const auto invalid = [](rcsdk::SessionConfig c, const char* words) {
        try {
            rcsdk::Session made(std::move(c));
            return false;
        } catch (const rcsdk::Error& error) {
            std::printf("invalid: %s\n", error.what());
            return error.result() == RCS_ERROR_INVALID_ARGUMENT &&
                   std::string(error.what()).find(words) != std::string::npos;
        }
    };
    CHECK(invalid(config(RCS_ROLE_VIEW, fwd.port, key).setTransport(RCS_TRANSPORT_FORWARDER), "RCS_EXT_FORWARDER"));
    const auto token = fwd.token(RCSFWD_ROLE_SUBSCRIBE, "room", 5);
    CHECK(invalid(fwd.config(RCS_ROLE_VIEW, key, token).setForwarderWebSocket("http://x/", fwd.keySha256, token),
                  "ws://"));
    CHECK(invalid(fwd.config(RCS_ROLE_VIEW, key, token).setForwarderWebSocket("ws://x:0/", fwd.keySha256, token),
                  "port"));
    CHECK(invalid(fwd.config(RCS_ROLE_VIEW, key, token).setForwarderWebSocket("wss://192.0.2.1/", fwd.keySha256, token),
                  "tls_certificate_sha256"));
    auto pinned = fwd.webSocket(RCS_ROLE_VIEW, key, token);
    pinned.extension<rcs_forwarder_config>().tls_certificate_sha256[0] = 1; // ws:// has no certificate to pin.
    CHECK(invalid(pinned, "wss://"));
    auto addressed = fwd.webSocket(RCS_ROLE_VIEW, key, token);
    addressed.setAddress("127.0.0.1", fwd.port); // The URL says where.
    CHECK(invalid(addressed, "connect.address"));
    // Another link set over a pinned wss:// one keeps nothing of it (the pin would be refused there).
    rcsdk::CertificateSha256 pin{};
    pin[0] = 1;
    const auto unpinned = [](rcsdk::SessionConfig& c) {
        const auto& sha = c.extension<rcs_forwarder_config>().tls_certificate_sha256;
        return std::all_of(std::begin(sha), std::end(sha), [](uint8_t b) { return b == 0; });
    };
    auto relinked = fwd.webSocket(RCS_ROLE_VIEW, key, token);
    relinked.setForwarderWebSocket("wss://forwarder.example/", fwd.keySha256, token, pin)
        .setForwarder("127.0.0.1", fwd.port, fwd.keySha256, token);
    CHECK(unpinned(relinked) && !invalid(relinked, "tls_certificate_sha256"));
    auto hosted = fwd.webSocket(RCS_ROLE_VIEW, key, token);
    hosted.setForwarderWebSocket("wss://forwarder.example/", fwd.keySha256, token, pin)
        .setForwarderHost(fwd.keySha256, token);
    CHECK(unpinned(hosted));
    CHECK(viewer.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
#else
    (void)key;
#endif
}

// Through a forwarder, of three viewers one's path from the forwarder carries
// 1.5 Mbit/s: it gets the lower encoding (moved by the sharer's policy from
// the paths the forwarder reports) while the others keep the main one at the
// bitrate their paths carry; once its path clears it comes back. With both
// encodings running (FIXED) and the policy slow to act, the forwarder moves
// it itself, at once, as its backlog runs far behind.
void forwarderMoves(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    rcsdk::test::patternControl().grain = true; // Content that needs the bitrate.
    TestForwarder fwd;
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    auto sharing = fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "group", 0))
                       .setVideo(640, 360, 30, 6'000'000)
                       .setViewers(3);
    auto& tuning = sharing.extension<rcs_video_tuning_config>();
    tuning.move_up_ms = 3000;
    tuning.lower_idle_ms = 2000;
    rcsdk::Session share(sharing, shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", fwd.port));
    proxy.bitsPerSecond = 1'500'000;
    const auto viewing = [&](uint16_t port, uint64_t route) {
        auto c = fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "group", route));
        return c.setForwarder("127.0.0.1", port, fwd.keySha256, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "group", route));
    };
    GroupMember fast(viewing(fwd.port, 1)), other(viewing(fwd.port, 2)), slow(viewing(proxy.address().port(), 3));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    for (auto* viewer : {&fast, &other, &slow}) {
        CHECK(viewer->session->start() == RCS_OK && viewer->watching(5, 15s));
    }
    const rcs_viewer_id slowId = share.viewers()[2].viewer_id;
    CHECK(waitFor([&] { return slow.encoding() == 1; }, 20s));
    const auto moved = movesOf(shareEvents);
    CHECK(!moved.empty() && std::get<0>(moved.back()) == slowId && std::get<1>(moved.back()) == 1 &&
          std::get<2>(moved.back()) == RCS_VIEWER_VIDEO_PATH);
    CHECK(fast.encoding() == 0 && other.encoding() == 0);
    // The others are not held to its path.
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().bitrate_bps > 3'000'000; }, 15s));
    const double fastRate = fast.rate(), slowRate = slow.rate();
    const auto room = fwd.forwarder->roomStats("group");
    std::printf("forwarder moves: main %u kbit/s, the others %.0f FPS; the slow one %.0f FPS on the lower encoding "
                "at %u kbit/s; moved on the spot %llu times\n",
                share.stats<rcs_video_send_stats>().bitrate_bps / 1000, fastRate, slowRate,
                share.encodingStats(0, 1).bitrate_bps / 1000, (unsigned long long)room.moves_down);
    CHECK(fastRate >= 20 && other.watching(20) && slowRate > 0);
    // Its path clears: back on the main encoding.
    proxy.bitsPerSecond = 0;
    CHECK(waitFor([&] { return slow.encoding() == 0; }, 40s));
    CHECK(slow.watching(10));
    for (auto* viewer : {&fast, &other, &slow}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);

    // Both encodings run; the policy would wait 10 s and not move for losses: the forwarder moves the slow one
    // on the spot.
    auto fixed = fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "group", 0))
                     .setVideo(640, 360, 30, 6'000'000)
                     .setViewers(2);
    fixed.extension<rcs_video_tuning_config>().move_down_ms = 10'000;
    fixed.extension<rcs_video_tuning_config>().loss_keyframes_move = 0;
    auto& encodings = fixed.extension<rcs_encodings_config>();
    encodings.mode = RCS_ENCODINGS_FIXED;
    encodings.count = 2;
    EventLog fixedEvents;
    rcsdk::Callbacks fixedCallbacks;
    fixedCallbacks.onEvent = fixedEvents.callback();
    rcsdk::Session two(fixed, fixedCallbacks);
    CHECK(two.start() == RCS_OK);
    CHECK(waitFor([&] { return two.state() == RCS_STATE_LISTENING; }));
    GroupMember steady(viewing(fwd.port, 4)), behind(viewing(proxy.address().port(), 5));
    for (auto* viewer : {&steady, &behind}) {
        CHECK(viewer->session->start() == RCS_OK && viewer->watching(5, 15s));
    }
    // Both on the main encoding first, then its path narrows.
    CHECK(waitFor([&] { return behind.encoding() == 0; }, 20s));
    proxy.bitsPerSecond = 1'500'000;
    CHECK(waitFor([&] { return behind.encoding() == 1; }, 9s));
    CHECK(fwd.forwarder->roomStats("group").moves_down >= 1);
    const auto onTheSpot = movesOf(fixedEvents);
    CHECK(!onTheSpot.empty() && std::get<1>(onTheSpot.back()) == 1 &&
          std::get<2>(onTheSpot.back()) == RCS_VIEWER_VIDEO_PATH);
    const bool behindWatches = behind.watching(10), steadyWatches = steady.watching(20);
    const auto opened = behind.session->stats<rcs_forwarder_stats>();
    const auto spot = fwd.forwarder->roomStats("group");
    std::printf("forwarder moves: on the spot %llu times; the slow one %s, %llu pictures opened, %llu without their "
                "key; %llu started on what was kept, %llu keyframes asked\n",
                (unsigned long long)spot.moves_down, behindWatches ? "watches" : "does not watch",
                (unsigned long long)opened.media_frames, (unsigned long long)opened.media_unkeyed,
                (unsigned long long)spot.cache_starts, (unsigned long long)spot.keyframes_asked);
    CHECK(behindWatches && steadyWatches && steady.encoding() == 0);
    for (auto* viewer : {&steady, &behind}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(two.stop() == RCS_OK);
    rcsdk::test::patternControl().grain = false;
#else
    (void)key;
#endif
}

// Through a forwarder over each of its entries: a sharer over TCP, viewers
// over its WebSocket entry (ws://) and over UDP. All watch, data passes end
// to end, and each end and the forwarder say how it came.
void forwarderEntries(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    TestForwarder fwd;
    std::mutex mutex;
    std::vector<std::string> fromViewers;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onData = [&](const rcs_data_message& message) {
        std::lock_guard lock(mutex);
        fromViewers.emplace_back(reinterpret_cast<const char*>(message.data), message.size);
    };
    rcsdk::Session share(
        fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "entries", 0), RCS_FORWARDER_TCP).setViewers(4),
        shareCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    TestViewer overWebSocket(fwd.webSocket(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "entries", 1)));
    TestViewer overUdp(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "entries", 2)));
    for (auto* viewer : {&overWebSocket, &overUdp}) {
        CHECK(viewer->session->start() == RCS_OK);
        CHECK(viewer->watching(30));
    }
    CHECK(share.stats<rcs_forwarder_stats>().link == RCS_FORWARDER_TCP);
    CHECK(overWebSocket.session->stats<rcs_forwarder_stats>().link == RCS_FORWARDER_WEBSOCKET);
    CHECK(overUdp.session->stats<rcs_forwarder_stats>().link == RCS_FORWARDER_UDP);
    const std::string path = overWebSocket.session->stats<rcs_network_stats>().path;
    std::printf("forwarder entries: %s\n", path.c_str());
    CHECK(path.find("WebSocket ws://127.0.0.1:") != std::string::npos);
    CHECK(overWebSocket.session->sendData(5, "over a WebSocket") == RCS_OK);
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return !fromViewers.empty() && fromViewers.back() == "over a WebSocket";
    }));
    const auto viewers = fwd.forwarder->viewers("entries");
    CHECK(viewers.size() == 2 && viewers[0].entry == RCSFWD_ENTRY_WEBSOCKET && viewers[1].entry == RCSFWD_ENTRY_UDP);
    CHECK(fwd.forwarder->stats().connections == 2); // The sharer's TCP connection and the viewer's WebSocket.
    for (auto* viewer : {&overWebSocket, &overUdp}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
#else
    (void)key;
#endif
}

// A viewer that comes later starts at once on what the forwarder kept (the
// newest keyframe and what followed, which its key opens: the sharer gives
// it that keyframe's step), with no keyframe of the sharer's; one that comes
// when nothing kept is young enough waits for the keyframe the forwarder asks
// the sharer for, once; and so does one let in by a sharer whose viewers open
// nothing from before they were let in (from_admission).
void forwarderLate(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    TestForwarder fwd;
    auto sharing = fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "late", 0)).setViewers(4);
    rcsdk::Session share(sharing);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    TestViewer first(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "late", 1)));
    CHECK(first.session->start() == RCS_OK && first.watching(5));
    const uint64_t keyframes = share.stats<rcs_video_send_stats>().keyframes;
    const auto joined = std::chrono::steady_clock::now();
    TestViewer soon(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "late", 2)));
    CHECK(soon.session->start() == RCS_OK);
    CHECK(waitFor([&] { return soon.frames > 0; }, 5s));
    const double firstPicture = std::chrono::duration<double>(std::chrono::steady_clock::now() - joined).count();
    CHECK(soon.watching(10));
    auto room = fwd.forwarder->roomStats("late");
    std::printf("forwarder late: the second viewer's first picture after %.0f ms, %llu started on what was kept, "
                "%llu keyframes asked\n",
                firstPicture * 1000, (unsigned long long)room.cache_starts, (unsigned long long)room.keyframes_asked);
    CHECK(room.cache_starts >= 1 && share.stats<rcs_video_send_stats>().keyframes == keyframes);
    CHECK(soon.session->stats<rcs_forwarder_stats>().media_unkeyed == 0);
    // Past the forwarder's 3 s with no keyframe since: the next one waits for a keyframe, asked for it.
    std::this_thread::sleep_for(3500ms);
    const uint64_t quiet = share.stats<rcs_video_send_stats>().keyframes;
    TestViewer later(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "late", 3)));
    CHECK(later.session->start() == RCS_OK && later.watching(10));
    room = fwd.forwarder->roomStats("late");
    std::printf("forwarder late: the third viewer: %llu keyframes asked, %llu made\n",
                (unsigned long long)room.keyframes_asked,
                (unsigned long long)(share.stats<rcs_video_send_stats>().keyframes - quiet));
    CHECK(quiet != keyframes || (room.keyframes_asked >= 1 && share.stats<rcs_video_send_stats>().keyframes > quiet));
    // What was kept is young again (that keyframe), but the sharer gives the next one no key from before.
    sharing.extension<rcs_forwarder_config>().from_admission = 1;
    CHECK(share.update(sharing.extension<rcs_forwarder_config>()) == RCS_OK);
    room = fwd.forwarder->roomStats("late");
    TestViewer fresh(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "late", 4)));
    CHECK(fresh.session->start() == RCS_OK && fresh.watching(10));
    const auto since = fwd.forwarder->roomStats("late");
    std::printf("forwarder late: from admission: %llu started on what was kept, %llu keyframes asked\n",
                (unsigned long long)(since.cache_starts - room.cache_starts),
                (unsigned long long)(since.keyframes_asked - room.keyframes_asked));
    CHECK(since.cache_starts == room.cache_starts && since.keyframes_asked > room.keyframes_asked);
    for (auto* viewer : {&first, &soon, &later, &fresh}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
#else
    (void)key;
#endif
}

// The path up to the forwarder carries 2 Mbit/s (the sharer's uplink,
// throttled): the sharer's hop measures it and the encodings keep within it,
// so the viewers watch on; RCS_EVENT_RATE with viewer 0 says the uplink held
// the bitrate. Once the path clears, the bitrate comes back up.
void forwarderUplink(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    rcsdk::test::patternControl().grain = true; // Content that needs the bitrate.
    TestForwarder fwd;
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", fwd.port));
    proxy.upstream = true;
    proxy.bitsPerSecond = 2'000'000;
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    auto sharing = fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "uplink", 0))
                       .setVideo(640, 360, 30, 6'000'000)
                       .setViewers(2);
    sharing.setAddress("127.0.0.1", proxy.address().port()); // Up through the throttled path.
    rcsdk::Session share(sharing, shareCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 10s));
    GroupMember a(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "uplink", 1)));
    GroupMember b(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "uplink", 2)));
    for (auto* viewer : {&a, &b}) {
        CHECK(viewer->session->start() == RCS_OK);
    }
    for (auto* viewer : {&a, &b}) {
        CHECK(viewer->watching(5, 15s));
    }
    CHECK(waitFor(
        [&] {
            const auto f = share.stats<rcs_forwarder_stats>();
            return f.uplink_bps > 0 && f.uplink_bps < 2'500'000 && f.uplink_limited > 0;
        },
        20s));
    std::printf("forwarder uplink: %llu pictures lost on the way up, the viewers started again at keyframes\n",
                (unsigned long long)fwd.forwarder->roomStats("uplink").media_lost);
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().bitrate_bps <= 2'000'000; }, 15s));
    const auto f = share.stats<rcs_forwarder_stats>();
    const double rateA = a.rate();
    std::printf("forwarder uplink: measured %u kbit/s, held back %llu turns, encoding at %u kbit/s, viewer %.0f FPS\n",
                f.uplink_bps / 1000, (unsigned long long)f.uplink_limited,
                share.stats<rcs_video_send_stats>().bitrate_bps / 1000, rateA);
    CHECK(rateA > 5 && b.watching(5));
    const auto rates = shareEvents.of(RCS_EVENT_RATE);
    CHECK(std::any_of(rates.begin(), rates.end(), [](const rcs_event& e) { return e.viewer_id == 0; }));
    // The path clears: up again.
    proxy.bitsPerSecond = 0;
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().bitrate_bps > 3'000'000; }, 30s));
    std::printf("forwarder uplink: cleared, encoding at %u kbit/s\n",
                share.stats<rcs_video_send_stats>().bitrate_bps / 1000);
    for (auto* viewer : {&a, &b}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
    rcsdk::test::patternControl().grain = false;
#else
    (void)key;
#endif
}

// Two rooms through one forwarder, each its own sharer and viewer; a room
// beyond what it serves is refused (BUSY).
void forwarderRooms(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    TestForwarder fwd([](rcsdk::forwarder::Settings& s) { s.maxRooms = 2; });
    std::vector<std::unique_ptr<rcsdk::Session>> sharers;
    std::vector<std::unique_ptr<TestViewer>> viewers;
    for (const char* room : {"one", "two"}) {
        sharers.push_back(
            std::make_unique<rcsdk::Session>(fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, room, 0))));
        CHECK(sharers.back()->start() == RCS_OK);
        CHECK(waitFor([&] { return sharers.back()->state() == RCS_STATE_LISTENING; }));
        viewers.push_back(std::make_unique<TestViewer>(
            fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, room, sharers.size() * 10))));
        CHECK(viewers.back()->session->start() == RCS_OK);
    }
    for (size_t i = 0; i < 2; ++i) {
        CHECK(viewers[i]->watching());
        const auto list = sharers[i]->viewers();
        CHECK(list.size() == 1 && list[0].route == (i + 1) * 10);
    }
    CHECK(fwd.forwarder->stats().rooms == 2);
    rcsdk::Session third(fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "three", 0)));
    CHECK(third.start() == RCS_OK);
    CHECK(waitFor([&] { return third.state() == RCS_STATE_FAILED; }, 10s));
    CHECK(third.failure().result == RCS_ERROR_BUSY &&
          third.failure().message.find("as many rooms") != std::string::npos);
    for (auto& viewer : viewers) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    for (auto& sharer : sharers) {
        CHECK(sharer->stop() == RCS_OK);
    }
#else
    (void)key;
#endif
}

// A viewer's token expires while it watches, and the forwarder ends the room
// (as on a restart): the viewer reconnects with the token its host gives it
// then (rcs_forwarder_config.refresh_token), and the sharer, which keeps
// listening, comes back too. A viewer whose host gives none is refused.
void forwarderTokenRefresh(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    TestForwarder fwd;
    struct Refresh {
        TestForwarder* forwarder;
        uint64_t route;
        bool give;
        std::atomic<int> calls{0};
    };
    const auto refresh = [](char* token, uint32_t capacity, void* user) -> int32_t {
        auto& r = *static_cast<Refresh*>(user);
        ++r.calls;
        if (!r.give) {
            return 0;
        }
        const auto fresh = r.forwarder->token(RCSFWD_ROLE_SUBSCRIBE, "refresh", r.route);
        CHECK(fresh.size() < capacity);
        std::memcpy(token, fresh.c_str(), fresh.size() + 1);
        return 1;
    };
    auto sharing = fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "refresh", 0)).setViewers(4);
    sharing.setKeepListening(true);
    rcsdk::Session share(sharing);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    Refresh gives{&fwd, 1, true}, keeps{&fwd, 2, false};
    std::vector<std::unique_ptr<TestViewer>> viewers;
    for (auto* r : {&gives, &keeps}) {
        auto c = reconnecting(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "refresh", r->route, 2)),
                              15'000, 500);
        auto& f = c.extension<rcs_forwarder_config>();
        f.refresh_token = refresh;
        f.user_data = r;
        viewers.push_back(std::make_unique<TestViewer>(c));
        CHECK(viewers.back()->session->start() == RCS_OK && viewers.back()->watching());
    }
    std::this_thread::sleep_for(2500ms); // Their tokens expired.
    fwd.forwarder->closeRoom("refresh");
    CHECK(waitFor([&] { return viewers[1]->session->state() == RCS_STATE_FAILED; }, 15s));
    std::printf("forwarder refresh: without a new token: %s\n", viewers[1]->session->failure().message.c_str());
    CHECK(viewers[1]->session->failure().result == RCS_ERROR_REFUSED && keeps.calls >= 1 &&
          viewers[1]->session->failure().message.find("expired") != std::string::npos);
    CHECK(viewers[0]->watching(10) && gives.calls >= 1);
    CHECK(waitFor([&] { return share.viewers().size() == 1; }, 10s));
    std::printf("forwarder refresh: with one: %d asked, watching again\n", gives.calls.load());
    CHECK(viewers[0]->session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
#else
    (void)key;
#endif
}

// Through a forwarder over the host's own channel to it (RCS_FORWARDER_HOST),
// as the browser package carries it: a viewer whose hop runs over a WebSocket
// of the host's to the forwarder's WebSocket entry, one datagram a message
// (what a page's connectWebSocket does), and one over a TCP connection of the
// host's to its TCP entry (STREAM). The first watches, hears the sharer's
// sound (sealed once) and sends data end to end. The second comes after a
// keyframe and starts on what the forwarder kept, with no keyframe of the
// sharer's; the host moves it to the lower encoding and back, and the
// forwarder starts it on each. The page's WebSocket closes: the first asks
// its host for a new token and connects anew over the next WebSocket. The
// sharer's hop runs over a TCP connection of its host's too.
void forwarderHost(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    TestForwarder fwd;
    const uint16_t webSocketEntry = fwd.forwarder->port(RCSFWD_ENTRY_WEBSOCKET);
    const uint16_t tcpEntry = fwd.forwarder->port(RCSFWD_ENTRY_TCP);
    PageChannel socket(true, webSocketEntry), stream(false, tcpEntry), publishing(false, tcpEntry);
    std::mutex mutex;
    std::vector<std::string> fromViewers;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onData = [&](const rcs_data_message& message) {
        std::lock_guard lock(mutex);
        fromViewers.emplace_back(reinterpret_cast<const char*>(message.data), message.size);
    };
    auto sharing = fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "page", 0)).setViewers(4);
    sharing.setForwarderHost(fwd.keySha256, fwd.token(RCSFWD_ROLE_PUBLISH, "page", 0))
        .use(rcsdk::hostTransport(publishing, RCS_HOST_CHANNEL_STREAM));
    auto& encodings = sharing.extension<rcs_encodings_config>();
    encodings.mode = RCS_ENCODINGS_FIXED;
    encodings.count = 2;
    rcsdk::Session share(sharing, shareCallbacks);
    publishing.attach(share);
    publishing.open();
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(share.stats<rcs_forwarder_stats>().link == RCS_FORWARDER_HOST);
    {
        std::lock_guard lock(fwd.mutex);
        CHECK(std::any_of(fwd.events.begin(), fwd.events.end(), [](const rcsfwd_event& e) {
            return e.type == RCSFWD_EVENT_JOINED && e.role == RCSFWD_ROLE_PUBLISH && e.entry == RCSFWD_ENTRY_TCP;
        }));
    }
    const auto viewing = [&](PageChannel& channel, rcs_host_channel kind, uint64_t route) {
        auto c = reconnecting(::config(RCS_ROLE_VIEW, 0, key), 15'000, 500);
        c.setForwarderHost(fwd.keySha256, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "page", route))
            .use(rcsdk::hostTransport(channel, kind));
        return c;
    };
    struct Refresh {
        TestForwarder* forwarder;
        std::atomic<int> calls{0};
    } refresh{&fwd};
    auto pageConfig = viewing(socket, RCS_HOST_CHANNEL_DATAGRAM, 1);
    auto& f = pageConfig.extension<rcs_forwarder_config>();
    f.refresh_token = [](char* token, uint32_t capacity, void* user) -> int32_t {
        auto& r = *static_cast<Refresh*>(user);
        const auto fresh = r.forwarder->token(RCSFWD_ROLE_SUBSCRIBE, "page", 1);
        CHECK(fresh.size() < capacity);
        std::memcpy(token, fresh.c_str(), fresh.size() + 1);
        ++r.calls;
        return 1;
    };
    f.user_data = &refresh;
    TestViewer page(pageConfig);
    socket.attach(*page.session);
    socket.open();
    CHECK(page.session->start() == RCS_OK && page.watching(10));
    const auto opened = page.session->stats<rcs_forwarder_stats>();
    const std::string path = page.session->stats<rcs_network_stats>().path;
    std::printf("forwarder host: %s, %llu pictures opened\n", path.c_str(), (unsigned long long)opened.media_frames);
    CHECK(opened.link == RCS_FORWARDER_HOST && opened.media_frames > 0 && opened.media_rejected == 0);
    CHECK(path.find("the host's datagram channel to the forwarder") != std::string::npos);
    CHECK(waitFor([&] { return page.session->stats<rcs_audio_receive_stats>().received_packets > 20; }, 5s));
    CHECK(page.session->sendData(4, "from the page") == RCS_OK);
    CHECK(waitFor([&] {
        std::lock_guard lock(mutex);
        return !fromViewers.empty() && fromViewers.back() == "from the page";
    }));

    // Over the host's TCP connection, after a keyframe the forwarder keeps: it starts on that.
    TestViewer late(viewing(stream, RCS_HOST_CHANNEL_STREAM, 2));
    stream.attach(*late.session);
    stream.open();
    const auto before = fwd.forwarder->roomStats("page");
    const uint64_t joinedBefore = share.stats<rcs_group_stats>().keyframes_joined;
    CHECK(share.requestKeyframe() == RCS_OK);
    std::this_thread::sleep_for(200ms);
    CHECK(late.session->start() == RCS_OK && late.watching(10));
    const auto room = fwd.forwarder->roomStats("page");
    std::printf("forwarder host: the later one started on what was kept %llu times, %llu keyframes asked\n",
                (unsigned long long)(room.cache_starts - before.cache_starts),
                (unsigned long long)(room.keyframes_asked - before.keyframes_asked));
    CHECK(room.cache_starts > before.cache_starts && share.stats<rcs_group_stats>().keyframes_joined == joinedBefore);
    std::set<rcsfwd_entry> entries;
    for (const auto& v : fwd.forwarder->viewers("page")) {
        entries.insert(v.entry);
    }
    CHECK((entries == std::set<rcsfwd_entry>{RCSFWD_ENTRY_TCP, RCSFWD_ENTRY_WEBSOCKET}));

    // The host moves it to the lower encoding, and back.
    rcs_viewer_id lateId = 0;
    for (const auto& viewer : share.viewers()) {
        lateId = viewer.route == 2 ? viewer.viewer_id : lateId;
    }
    rcs_viewer_video video = RCS_VIEWER_VIDEO_INIT;
    video.encoding = 1;
    CHECK(share.setViewerVideo(lateId, video) == RCS_OK);
    CHECK(waitFor([&] { return late.session->streamInfo().encoding == 1; }) && late.watching(10));
    video.encoding = 0;
    CHECK(share.setViewerVideo(lateId, video) == RCS_OK);
    CHECK(waitFor([&] { return late.session->streamInfo().encoding == 0; }) && late.watching(10));
    CHECK(page.watching(10) && page.session->streamInfo().encoding == 0);

    // The page's WebSocket closes: a new token, a new WebSocket, a new hop.
    socket.close();
    CHECK(waitFor([&] { return refresh.calls >= 1; }, 10s));
    socket.open();
    CHECK(waitFor([&] { return page.session->state() == RCS_STATE_RUNNING; }, 15s) && page.watching(10));
    std::printf("forwarder host: after its WebSocket closed: %d new token, watching again over another\n",
                refresh.calls.load());
    for (auto* viewer : {&page, &late}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
#else
    (void)key;
#endif
}

// ---- The remote features through a forwarder -----------------------------------------------
//
// Everything but the pictures and the sharer's sound rides each viewer's own
// end-to-end connection, which the forwarder passes through as pipe
// datagrams: control and input, the pointer, the clipboard, file transfer,
// the viewer's sound. The scenarios below run each as the direct ones do.

#ifdef RCSDK_TEST_FORWARDER
// The sharer's number for the viewer whose token names `route` (0: none such).
rcs_viewer_id viewerOfRoute(rcsdk::Session& share, uint64_t route) {
    for (const auto& viewer : share.viewers()) {
        if (viewer.route == route) {
            return viewer.viewer_id;
        }
    }
    return 0;
}

// What the room's pipes carried and dropped (over max_pipe_bps).
void printPipes(TestForwarder& fwd, const char* scenario, const std::string& room) {
    const auto stats = fwd.forwarder->roomStats(room);
    std::printf("%s: the pipes carried %llu bytes, dropped %llu datagrams\n", scenario,
                (unsigned long long)stats.pipe_bytes, (unsigned long long)stats.pipe_dropped);
}
#endif

// Control through a forwarder, two viewers in one room (one over the
// WebSocket entry, one over UDP). The host lets the one whose token names
// route 1 control: its pointer, buttons, wheel, keys and text reach the
// sharer whole and in order, named as its (on_input), and are executed in
// that order; the other's is refused. Its pointer's shapes and positions
// come apart from the picture (SEPARATE). The host hands control to the
// other and takes it from the first (what it held is released), then from
// everyone.
void forwarderControl(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    {
        auto& pattern = rcsdk::test::patternControl();
        pattern.cursorX = 1000;
        TestForwarder fwd;
        std::mutex mutex;
        std::vector<rcs_input_event> handled;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onInput = [&](const rcs_input_event& event) {
            std::lock_guard lock(mutex);
            handled.push_back(event);
            return true;
        };
        shareCallbacks.onViewerJoin = [](const rcs_viewer_info& viewer, rcs_viewer_decision& decision) {
            decision.control = viewer.route == 1; // The host knows its viewers by their tokens' routes.
        };
        rcsdk::Session share(fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "control", 0))
                                 .setControl(true)
                                 .setViewers(4),
                             shareCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        auto controlling = fwd.webSocket(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "control", 1));
        TestViewer a(controlling.setCursorMode(RCS_CURSOR_SEPARATE));
        TestViewer b(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "control", 2)));
        CHECK(a.session->start() == RCS_OK && a.watching());
        CHECK(b.session->start() == RCS_OK && b.watching());
        const rcs_viewer_id aId = viewerOfRoute(share, 1), bId = viewerOfRoute(share, 2);
        CHECK(aId && bId && aId != bId);
        CHECK(waitFor([&] { return a.session->controlInfo().granted != 0; }));
        std::this_thread::sleep_for(300ms);
        CHECK(!b.session->controlInfo().granted);

        // The pointer apart from the picture: its shape once, then where it goes.
        const auto cursorAt = [&](uint32_t x) {
            return waitFor([&] {
                const auto events = a.events.of(RCS_EVENT_CURSOR);
                return std::any_of(events.begin(), events.end(),
                                   [&](const rcs_event& e) { return e.cursor.visible && e.cursor.x == x; });
            });
        };
        CHECK(cursorAt(1000));
        pattern.cursorX = 30000;
        CHECK(cursorAt(30000));
        {
            const auto events = a.events.of(RCS_EVENT_CURSOR);
            CHECK(std::count_if(events.begin(), events.end(), [](const rcs_event& e) { return e.cursor.shape; }) == 1);
        }

        // A run of every kind of input, all at once: whole and in order.
        std::vector<rcs_input_event> sent;
        for (uint32_t i = 0; i < 36; ++i) {
            rcs_input_event e = RCS_INPUT_EVENT_INIT;
            e.type = RCS_INPUT_MOVE;
            e.x = 1000 + i * 1000;
            e.y = 500 + i;
            sent.push_back(e);
            e = RCS_INPUT_EVENT_INIT;
            e.type = RCS_INPUT_BUTTON;
            e.code = RCS_BUTTON_LEFT;
            e.down = 1;
            sent.push_back(e);
            e.type = RCS_INPUT_WHEEL;
            e.code = 0;
            e.down = 0;
            e.wheel_y = i % 2 ? 1 : -1;
            sent.push_back(e);
            e = RCS_INPUT_EVENT_INIT;
            e.type = RCS_INPUT_KEY;
            e.code = 4 + i % 26; // HID: a..z
            e.down = 1;
            sent.push_back(e);
            e.down = 0;
            sent.push_back(e);
            e = RCS_INPUT_EVENT_INIT;
            e.type = RCS_INPUT_TEXT;
            e.code = i % 2 ? 0x4E2D : 0x61 + i % 26; // 中, or a letter.
            sent.push_back(e);
            e = RCS_INPUT_EVENT_INIT;
            e.type = RCS_INPUT_BUTTON;
            e.code = RCS_BUTTON_LEFT;
            sent.push_back(e);
        }
        for (const auto& e : sent) {
            CHECK(a.session->sendInput(e) == RCS_OK);
        }
        const auto same = [](const rcs_input_event& x, const rcs_input_event& y) {
            return x.type == y.type && x.x == y.x && x.y == y.y && x.code == y.code && x.down == y.down &&
                   x.wheel_y == y.wheel_y;
        };
        const auto sameRun = [&](std::vector<rcs_input_event> got) {
            std::erase_if(got, [](const rcs_input_event& e) { return e.type == RCS_INPUT_RELEASE_ALL; });
            return got.size() == sent.size() && std::equal(got.begin(), got.end(), sent.begin(), same);
        };
        CHECK(waitFor([&] {
            std::lock_guard lock(mutex);
            return std::count_if(handled.begin(), handled.end(), [&](const auto& e) { return e.viewer_id == aId; }) >=
                   ptrdiff_t(sent.size());
        }));
        {
            std::lock_guard lock(mutex);
            std::vector<rcs_input_event> fromA;
            std::copy_if(handled.begin(), handled.end(), std::back_inserter(fromA),
                         [&](const auto& e) { return e.viewer_id == aId; });
            CHECK(sameRun(fromA));
        }
        CHECK(waitFor([&] { return recorder.count() >= sent.size(); }));
        {
            std::lock_guard lock(recorder.mutex);
            CHECK(sameRun(recorder.applied));
        }
        const auto received = share.stats<rcs_input_stats>();
        std::printf("forwarder control: %zu events through the pipe in order, %llu received, %llu executed\n",
                    sent.size(), (unsigned long long)received.received, (unsigned long long)received.applied);
        CHECK(received.received >= sent.size() && a.session->stats<rcs_input_stats>().sent >= sent.size());

        // The other may not; once the host lets it, its keys arrive as its own.
        rcs_input_event press = RCS_INPUT_EVENT_INIT;
        press.type = RCS_INPUT_KEY;
        press.code = 40;
        press.down = 1;
        CHECK(b.session->sendInput(press) == RCS_ERROR_INVALID_STATE);
        CHECK(share.setViewerControl(bId, true) == RCS_OK);
        CHECK(waitFor([&] { return b.session->controlInfo().granted != 0; }));
        CHECK(b.session->sendInput(press) == RCS_OK);
        CHECK(waitFor([&] {
            std::lock_guard lock(mutex);
            return handled.back().viewer_id == bId && handled.back().code == 40;
        }));
        // Taken from the first: what it held is released, it sends no more.
        const int releases = recorder.releases;
        CHECK(share.setViewerControl(aId, false) == RCS_OK);
        CHECK(waitFor([&] { return !a.session->controlInfo().granted; }) && recorder.releases > releases);
        CHECK(a.session->sendInput(press) == RCS_ERROR_INVALID_STATE);
        CHECK(b.session->controlInfo().granted != 0);
        // From everyone.
        rcs_control_config none = RCS_CONTROL_CONFIG_INIT;
        CHECK(share.update(none) == RCS_OK);
        CHECK(waitFor([&] { return !b.session->controlInfo().granted; }));
        CHECK(!share.controlInfo().allowed && a.watching() && b.watching());
        printPipes(fwd, "forwarder control", "control");
        CHECK(fwd.forwarder->roomStats("control").pipe_dropped == 0);
        for (auto* viewer : {&a, &b}) {
            CHECK(viewer->session->stop() == RCS_OK);
        }
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::seams::setInputOverride({});
#else
    (void)key;
#endif
}

// The clipboard through a forwarder, two viewers in one room (one over the
// TCP entry, one over the WebSocket entry): text and a 3 MiB image from the
// sharer reach both; the first viewer's text and image go to the sharer and
// on to the other, never back. Files copied at the sharer are pasted at the
// first viewer into a directory (Finder's way) and read file by file at the
// other (Explorer's); a file copied at the first viewer is pasted at the
// sharer. The tests' clipboards only, never the system's.
void forwarderClipboard(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    Scratch scratch("forwarder-clipboard");
    const fs::path out = scratch / "out", pasted = scratch / "pasted", sharePasted = scratch / "share-pasted";
    fs::create_directories(pasted);
    fs::create_directories(sharePasted);
    const std::string report = pattern(700000, 41), photo = pattern(40000, 42);
    writeText(out / "report.txt", report);
    writeText(out / "photos" / "a.jpg", photo);
    writeText(out / "photos" / "sub" / "b.txt", "bee");
    FakeClipboard sharerBoard, firstBoard, secondBoard;
    std::atomic<int> views{0};
    rcsdk::clipboard::setFactory([&](rcs_role role) -> std::unique_ptr<rcsdk::clipboard::SystemClipboard> {
        if (role == RCS_ROLE_SHARE) {
            return std::make_unique<FakeClipboardAccess>(sharerBoard);
        }
        return std::make_unique<FakeClipboardAccess>(views++ == 0 ? firstBoard : secondBoard);
    });
    {
        constexpr uint32_t both = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
        const uint32_t files = 1u << RCS_CLIPBOARD_FILES;
        TestForwarder fwd;
        EventLog shareEvents;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        auto sharing = fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "clipboard", 0)).setViewers(4);
        rcsdk::Session share(withFiles(withClipboard(sharing, filesClipboard(both)), fileSettings(bothWays)),
                             shareCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        auto overTcp =
            fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "clipboard", 1), RCS_FORWARDER_TCP);
        TestViewer first(withFiles(withClipboard(overTcp, filesClipboard(both)), fileSettings(bothWays)));
        CHECK(first.session->start() == RCS_OK && first.watching());
        CHECK(views == 1);
        auto overWebSocket = fwd.webSocket(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "clipboard", 2));
        TestViewer second(withFiles(withClipboard(overWebSocket, filesClipboard(both)), fileSettings(bothWays)));
        CHECK(second.session->start() == RCS_OK && second.watching());
        const rcs_viewer_id firstId = viewerOfRoute(share, 1), secondId = viewerOfRoute(share, 2);
        CHECK(waitFor([&] {
            return (clipboardPeer(share, firstId).formats & files) &&
                   (clipboardPeer(share, secondId).formats & files) &&
                   (clipboardPeer(*first.session).formats & files) && (clipboardPeer(*second.session).formats & files);
        }));
        std::this_thread::sleep_for(300ms); // The watchers looked at what their clipboards held before.

        // Text and an image from the sharer, to both.
        auto rich = textCopy("from the sharer, through the forwarder");
        rich.items.push_back(pngItem(3u << 20));
        const auto image = rich.items.back().bytes;
        sharerBoard.copy(rich);
        const auto holdsRich = [&](FakeClipboard& board, const std::string& text,
                                   const rcsdk::clipboard::Bytes& bytes) {
            return waitFor(
                [&] {
                    const auto held = board.held();
                    return board.text() == text && held.items.size() == 2 &&
                           std::any_of(held.items.begin(), held.items.end(), [&](const auto& item) {
                               return item.format == RCS_CLIPBOARD_IMAGE && item.bytes == bytes;
                           });
                },
                10s);
        };
        CHECK(holdsRich(firstBoard, "from the sharer, through the forwarder", image));
        CHECK(holdsRich(secondBoard, "from the sharer, through the forwarder", image));
        // The first viewer's text and image: placed at the sharer, on to the other, not back.
        const int firstWrites = firstBoard.written();
        auto back = textCopy("from the first viewer");
        back.items.push_back(pngItem(1u << 20));
        const auto backImage = back.items.back().bytes;
        firstBoard.copy(back);
        CHECK(holdsRich(sharerBoard, "from the first viewer", backImage));
        CHECK(holdsRich(secondBoard, "from the first viewer", backImage));
        std::this_thread::sleep_for(300ms);
        CHECK(firstBoard.written() == firstWrites);
        CHECK(clipboardEvent(shareEvents, RCS_CLIPBOARD_RECEIVE, RCS_OK, firstId));
        CHECK(clipboardStats(share, firstId).received == 1 && clipboardStats(share, secondId).sent == 2);

        // Files copied at the sharer: names only, then pasted at each viewer.
        sharerBoard.copy(filesCopy({entryOf(out / "report.txt"), entryOf(out / "photos")}));
        CHECK(waitFor([&] {
            const auto a = firstBoard.held(), b = secondBoard.held();
            return a.files && a.files->entries.size() == 2 && b.files && b.files->entries.size() == 2;
        }));
        const auto started = std::chrono::steady_clock::now();
        const auto into = firstBoard.pasteInto(utf8Of(pasted));
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        CHECK(into && into->result == RCS_OK && into->complete);
        CHECK(readText(pasted / "report.txt") == report && readText(pasted / "photos" / "a.jpg") == photo &&
              readText(pasted / "photos" / "sub" / "b.txt") == "bee" && !leftovers(pasted));
        const auto read = secondBoard.pasteRead();
        CHECK(read && read->failed == RCS_OK && read->ended && read->ended->result == RCS_OK);
        CHECK(read->items.size() == 5 && read->files.at("report.txt") == report &&
              read->files.at("photos/a.jpg") == photo && read->files.at("photos/sub/b.txt") == "bee");
        CHECK(waitFor([&] {
            return clipboardStats(*first.session).pastes_here == 1 &&
                   clipboardStats(*second.session).pastes_here == 1 && clipboardStats(share).pastes_there == 2;
        }));
        std::printf("forwarder clipboard: files pasted at the first viewer in %.2f s, read at the other file by file\n",
                    seconds);
        // A file copied at the first viewer, pasted at the sharer.
        firstBoard.copy(filesCopy({entryOf(out / "photos" / "a.jpg")}));
        CHECK(waitFor([&] {
            const auto held = sharerBoard.held();
            return held.files && held.files->entries[0].name == "a.jpg";
        }));
        const auto there = sharerBoard.pasteInto(utf8Of(sharePasted));
        CHECK(there && there->result == RCS_OK && readText(sharePasted / "a.jpg") == photo);
        printPipes(fwd, "forwarder clipboard", "clipboard");
        CHECK(first.watching() && second.watching());
        CHECK(!shareEvents.overlapping);
        for (auto* viewer : {&first, &second}) {
            CHECK(viewer->session->stop() == RCS_OK);
        }
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::clipboard::setFactory({});
#else
    (void)key;
#endif
}

#ifdef RCSDK_TEST_FORWARDER
// A file's SHA-256, in hex (its first 8 bytes).
std::string fileHash(const fs::path& path) {
    const std::string bytes = readText(path);
    const auto hash = rcsdk::crypto::sha256(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    char text[17];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(text + 2 * i, 3, "%02x", hash[i]);
    }
    return text;
}
#endif

// File transfer through a forwarder, two viewers in one room (one over UDP,
// one over the WebSocket entry): a directory (with an empty file, an empty
// directory and a subdirectory) to each and back from each; 300 small files;
// a 32 MiB file to both at once and a 16 MiB one up from each: each arrives
// with the hash it left with. Meanwhile the pictures go on without a stall
// (the pipes' datagrams leave ahead of them on the hops, but hold them back
// by a burst at most) and the UDP viewer's keys reach the sharer.
void forwarderFiles(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    {
        Scratch scratch("forwarder-files");
        const fs::path out = scratch / "out";
        const std::string photo = pattern(100000, 51), notes = pattern(600000, 52);
        writeText(out / "photos" / "a.jpg", photo);
        writeText(out / "photos" / "sub" / "b.txt", "bee");
        writeText(out / "photos" / "empty.txt", "");
        fs::create_directories(out / "photos" / "nothing");
        writeText(out / "notes.txt", notes);
        for (int d = 0; d < 10; ++d) {
            for (int f = 0; f < 30; ++f) {
                writeText(out / "many" / ("d" + std::to_string(d)) / ("f" + std::to_string(f) + ".txt"),
                          std::to_string(d * 1000 + f));
            }
        }
        constexpr uint64_t down = 32u << 20, up = 16u << 20;
        LargePattern::make(out / "down.bin", down);
        LargePattern::make(out / "up.bin", up);
        TestForwarder fwd;
        EventLog shareEvents;
        std::mutex mutex;
        std::vector<std::chrono::steady_clock::time_point> keysArrived;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        shareCallbacks.onInput = [&](const rcs_input_event& event) {
            if (event.type == RCS_INPUT_KEY) {
                std::lock_guard lock(mutex);
                keysArrived.push_back(std::chrono::steady_clock::now());
            }
            return true;
        };
        auto sharing =
            fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "files", 0)).setViewers(4).setControl(true);
        rcsdk::Session share(withFiles(sharing, fileSettings(bothWays)), shareCallbacks);
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        auto overUdp = fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "files", 1));
        auto overWebSocket = fwd.webSocket(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "files", 2));
        struct Viewer {
            const char* name;
            std::unique_ptr<TestViewer> end;
            rcs_viewer_id id = 0;
            size_t offers = 0;
            fs::path in, sharerIn;
        };
        std::array<Viewer, 2> viewers{
            Viewer{"UDP", std::make_unique<TestViewer>(withFiles(overUdp, fileSettings(bothWays)))},
            Viewer{"WebSocket", std::make_unique<TestViewer>(withFiles(overWebSocket, fileSettings(bothWays)))}};
        for (uint64_t route = 1; auto& v : viewers) {
            CHECK(v.end->session->start() == RCS_OK && v.end->watching());
            v.id = viewerOfRoute(share, route++);
            v.in = scratch / (std::string("in-") + v.name);
            v.sharerIn = scratch / (std::string("sharer-from-") + v.name);
            fs::create_directories(v.in);
            fs::create_directories(v.sharerIn);
            CHECK(v.id &&
                  waitFor([&] { return filePeer(share, v.id).max_items && filePeer(*v.end->session).max_items; }));
        }
        CHECK(waitFor([&] { return viewers[0].end->session->controlInfo().granted != 0; }));
        size_t shareOffers = 0;
        // From the sharer to `v`, accepted into its directory; both ends' numbers.
        const auto toViewer = [&](Viewer& v, const std::vector<fs::path>& paths) {
            const uint64_t sent = sendPaths(share, v.id, paths);
            const uint64_t got = waitOffer(v.end->events, v.offers).transfer;
            CHECK(acceptInto(*v.end->session, got, v.in) == RCS_OK);
            return std::pair(sent, got);
        };
        const auto toSharer = [&](Viewer& v, const std::vector<fs::path>& paths) {
            const uint64_t sent = sendPaths(*v.end->session, 0, paths);
            const auto offer = waitOffer(shareEvents, shareOffers);
            CHECK(acceptInto(share, offer.transfer, v.sharerIn) == RCS_OK);
            return std::pair(sent, offer.transfer);
        };
        const auto completed = [](EventLog& log, uint64_t transfer, std::chrono::milliseconds timeout = 10s) {
            return waitDone(log, transfer, timeout).end == RCS_FILE_COMPLETED;
        };

        // A directory and a file to each, a directory back from each.
        for (auto& v : viewers) {
            const auto [sent, got] = toViewer(v, {out / "photos", out / "notes.txt"});
            CHECK(completed(v.end->events, got) && completed(shareEvents, sent));
            CHECK(readText(v.in / "notes.txt") == notes && readText(v.in / "photos" / "a.jpg") == photo &&
                  readText(v.in / "photos" / "sub" / "b.txt") == "bee" &&
                  fs::file_size(v.in / "photos" / "empty.txt") == 0 && fs::is_directory(v.in / "photos" / "nothing") &&
                  !leftovers(v.in));
            const auto [sentBack, gotBack] = toSharer(v, {out / "photos"});
            CHECK(completed(shareEvents, gotBack) && completed(v.end->events, sentBack));
            CHECK(readText(v.sharerIn / "photos" / "a.jpg") == photo &&
                  fs::is_directory(v.sharerIn / "photos" / "nothing") &&
                  transferInfo(share, gotBack).viewer_id == v.id);
        }
        // Many small files.
        {
            const auto started = std::chrono::steady_clock::now();
            const auto [sent, got] = toViewer(viewers[0], {out / "many"});
            CHECK(completed(viewers[0].end->events, got, 30s) && completed(shareEvents, sent));
            std::printf("forwarder files: 300 small files in %.2f s\n",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            CHECK(readText(viewers[0].in / "many" / "d9" / "f29.txt") == "9029" && !leftovers(viewers[0].in));
        }
        // A large file to both at once and one up from each, beside the
        // pictures (the longest each went without one) and a key every 50 ms.
        {
            const std::string downHash = fileHash(out / "down.bin"), upHash = fileHash(out / "up.bin");
            const int framesBefore[2] = {viewers[0].end->frames, viewers[1].end->frames};
            const uint64_t droppedBefore = viewers[0].end->session->stats<rcs_video_receive_stats>().dropped_packets;
            std::vector<std::chrono::steady_clock::time_point> keysSent;
            std::array<std::chrono::steady_clock::duration, 2> stalls{};
            std::atomic<bool> going{true};
            std::thread watcher([&] {
                std::array<int, 2> seen{framesBefore[0], framesBefore[1]};
                std::array<std::chrono::steady_clock::time_point, 2> since;
                since.fill(std::chrono::steady_clock::now());
                for (int tick = 0; going; ++tick) {
                    const auto now = std::chrono::steady_clock::now();
                    for (size_t i = 0; i < viewers.size(); ++i) {
                        if (const int frames = viewers[i].end->frames; frames != seen[i]) {
                            seen[i] = frames;
                            since[i] = now;
                        }
                        stalls[i] = std::max(stalls[i], now - since[i]);
                    }
                    if (tick % 5 == 0) {
                        rcs_input_event press = RCS_INPUT_EVENT_INIT;
                        press.type = RCS_INPUT_KEY;
                        press.code = 4 + uint32_t(keysSent.size() % 26);
                        press.down = 1;
                        keysSent.push_back(now);
                        CHECK(viewers[0].end->session->sendInput(press) == RCS_OK);
                    }
                    std::this_thread::sleep_for(10ms);
                }
            });
            const auto started = std::chrono::steady_clock::now();
            std::vector<std::tuple<Viewer*, uint64_t, uint64_t, uint64_t, uint64_t>> transfers;
            for (auto& v : viewers) {
                const auto [downSent, downGot] = toViewer(v, {out / "down.bin"});
                const auto [upSent, upGot] = toSharer(v, {out / "up.bin"});
                transfers.emplace_back(&v, downSent, downGot, upSent, upGot);
            }
            for (auto& [v, downSent, downGot, upSent, upGot] : transfers) {
                CHECK(completed(v->end->events, downGot, 60s) && completed(shareEvents, downSent));
                CHECK(completed(shareEvents, upGot, 60s) && completed(v->end->events, upSent));
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            going = false;
            watcher.join();
            CHECK(waitFor([&] {
                std::lock_guard lock(mutex);
                return keysArrived.size() == keysSent.size();
            }));
            std::chrono::steady_clock::duration slowest{}, total{};
            for (size_t i = 0; i < keysSent.size(); ++i) {
                slowest = std::max(slowest, keysArrived[i] - keysSent[i]);
                total += keysArrived[i] - keysSent[i];
            }
            const auto ms = [](std::chrono::steady_clock::duration d) {
                return std::chrono::duration<double, std::milli>(d).count();
            };
            for (size_t i = 0; i < viewers.size(); ++i) {
                auto& v = viewers[i];
                const std::string gotDown = fileHash(v.in / "down.bin"), gotUp = fileHash(v.sharerIn / "up.bin");
                std::printf("forwarder files: over %s: down %s (sent %s), up %s (sent %s); %d frames meanwhile, "
                            "%.0f ms the longest without one\n",
                            v.name, gotDown.c_str(), downHash.c_str(), gotUp.c_str(), upHash.c_str(),
                            v.end->frames - framesBefore[i], ms(stalls[i]));
                CHECK(gotDown == downHash && gotUp == upHash);
                CHECK(LargePattern::holds(v.in / "down.bin", down) && LargePattern::holds(v.sharerIn / "up.bin", up));
                // The pictures did not wait for the files.
                CHECK(v.end->frames - framesBefore[i] >= int(seconds * 15) && stalls[i] < 500ms);
            }
            std::printf("forwarder files: %llu MiB down and %llu MiB up to and from each of two viewers in %.1f s "
                        "(%.1f MB/s through the forwarder); %zu keys meanwhile, %.1f ms on average, %.1f ms at most\n",
                        (unsigned long long)(down >> 20), (unsigned long long)(up >> 20), seconds,
                        double(2 * (down + up)) / seconds / 1e6, keysSent.size(),
                        ms(total) / double(std::max<size_t>(keysSent.size(), 1)), ms(slowest));
            CHECK(viewers[0].end->session->stats<rcs_video_receive_stats>().dropped_packets == droppedBefore);
        }
        printPipes(fwd, "forwarder files", "files");
        CHECK(!shareEvents.overlapping);
        for (auto& v : viewers) {
            CHECK(v.end->session->stop() == RCS_OK);
        }
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::seams::setInputOverride({});
#else
    (void)key;
#endif
}

// File transfer through a forwarder across breaks. The viewer's hop to the
// forwarder goes silent in the middle of a transfer each way: the forwarder
// drops it, both ends wait, and once the viewer is back through the
// forwarder (a new connection, a new viewer number) they go on from what is
// written. Then the viewer's run ends midway: the next run's offer of the
// same items with the transfer's token sends only what was missing.
void forwarderFilesResume(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    Scratch scratch("forwarder-resume");
    const fs::path out = scratch / "out", shareIn = scratch / "share-in", viewIn = scratch / "view-in";
    fs::create_directories(shareIn);
    fs::create_directories(viewIn);
    const std::string downBytes = pattern(6u << 20, 61), upBytes = pattern(5u << 20, 62);
    const std::string brief = pattern(1000, 63), big = pattern(6u << 20, 64);
    writeText(out / "down.bin", downBytes);
    writeText(out / "up.bin", upBytes);
    writeText(out / "set" / "a-small.txt", brief);
    writeText(out / "set" / "b-big.bin", big);
    TestForwarder fwd;
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", fwd.port));
    auto files = fileSettings(bothWays);
    files.max_bytes_per_second = 2'000'000;
    auto sharing = fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "resume", 0)).setKeepListening(true);
    auto viewing =
        reconnecting(fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "resume", 1)), 30000, 500)
            .setMigrate(false);
    viewing.setAddress("127.0.0.1", proxy.address().port()); // Through the proxy to the forwarder's UDP entry.
    FilePair pair(sharing, viewing, files, files);
    auto& share = *pair.share;
    auto& view = *pair.view;
    pair.connect();
    const auto [downSent, downGot] = pair.toViewer({out / "down.bin"}, viewIn);
    const auto [upSent, upGot] = pair.toSharer({out / "up.bin"}, shareIn);
    CHECK(waitFor([&] {
        return transferInfo(view, downGot).done_bytes > (1u << 20) &&
               transferInfo(share, upGot).done_bytes > (1u << 20);
    }));
    proxy.outage = true;
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RECONNECTING; }, 10s));
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 15s));
    CHECK(waitFor([&] {
        return transferInfo(share, downSent).state == RCS_FILE_WAITING &&
               transferInfo(view, downGot).state == RCS_FILE_WAITING &&
               transferInfo(share, upGot).state == RCS_FILE_WAITING &&
               transferInfo(view, upSent).state == RCS_FILE_WAITING;
    }));
    const uint64_t downHad = transferInfo(view, downGot).done_bytes, upHad = transferInfo(share, upGot).done_bytes;
    CHECK(downHad < downBytes.size() && upHad < upBytes.size());
    proxy.outage = false;
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING; }, 20s));
    CHECK(waitDone(pair.viewEvents, downGot, 30s).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.shareEvents, downSent).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.shareEvents, upGot, 30s).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.viewEvents, upSent).end == RCS_FILE_COMPLETED);
    CHECK(readText(viewIn / "down.bin") == downBytes && readText(shareIn / "up.bin") == upBytes && !leftovers(viewIn) &&
          !leftovers(shareIn));
    const auto downInfo = transferInfo(share, downSent), upInfo = transferInfo(view, upSent);
    std::printf("forwarder resume: down %llu of %zu bytes written before the break, %llu sent in all; up %llu of %zu, "
                "%llu sent\n",
                (unsigned long long)downHad, downBytes.size(), (unsigned long long)downInfo.transferred_bytes,
                (unsigned long long)upHad, upBytes.size(), (unsigned long long)upInfo.transferred_bytes);
    CHECK(downInfo.transferred_bytes <= downBytes.size() + (4u << 20) &&
          upInfo.transferred_bytes <= upBytes.size() + (4u << 20));
    const rcs_viewer_id again = viewerOfRoute(share, 1);
    CHECK(again >= 2 && transferInfo(share, downSent).viewer_id == again &&
          transferInfo(share, upGot).viewer_id == again);
    CHECK(fwd.count(RCSFWD_EVENT_LEFT) >= 1);

    // Another run goes on where this one stopped.
    files.reconnect_wait_ms = 500;
    CHECK(updateFiles(share, files) == RCS_OK);
    const auto [first, firstGot] = pair.toViewer({out / "set"}, viewIn, RCS_FILE_CONFLICT_RENAME, again);
    CHECK(waitFor([&] { return transferInfo(view, firstGot).done_bytes > (1u << 20); }));
    CHECK(view.stop() == RCS_OK); // The run ends midway.
    CHECK(waitDone(pair.viewEvents, firstGot).partial_kept == 1);
    CHECK(waitDone(pair.shareEvents, first).end == RCS_FILE_FAILED);
    CHECK(partials(viewIn / "set", "b-big.bin").size() == 1);
    const auto kept = fs::file_size(partials(viewIn / "set", "b-big.bin").at(0));
    const auto token = transferInfo(share, first);
    CHECK(waitFor([&] { return share.viewers().empty(); }));
    CHECK(view.start() == RCS_OK);
    CHECK(waitFor([&] { return view.state() == RCS_STATE_RUNNING && share.viewers().size() == 1; }));
    const rcs_viewer_id next = share.viewers()[0].viewer_id;
    CHECK(waitFor([&] { return filePeer(share, next).max_items != 0; }));
    uint64_t resumed = 0;
    CHECK(offerPaths(share, next, {out / "set"}, &resumed, token.token) == RCS_OK);
    const auto offer = waitOffer(pair.viewEvents, pair.viewOffers);
    CHECK(offer.resuming == 1 && acceptInto(view, offer.transfer, viewIn) == RCS_OK);
    CHECK(waitDone(pair.viewEvents, offer.transfer, 20s).end == RCS_FILE_COMPLETED);
    CHECK(waitDone(pair.shareEvents, resumed).end == RCS_FILE_COMPLETED);
    CHECK(readText(viewIn / "set" / "b-big.bin") == big && readText(viewIn / "set" / "a-small.txt") == brief &&
          !leftovers(viewIn));
    const auto there = transferInfo(share, resumed);
    std::printf("forwarder resume: the next run: %llu bytes kept, %llu resumed, %llu sent again\n",
                (unsigned long long)kept, (unsigned long long)there.resumed_bytes,
                (unsigned long long)there.transferred_bytes);
    CHECK(there.resumed_bytes == kept + brief.size() && there.transferred_bytes == big.size() - kept);
    printPipes(fwd, "forwarder resume", "resume");
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
#else
    (void)key;
#endif
}

// The viewers' sound through a forwarder: one pushes its own (750 Hz) over
// UDP, the other's microphone (the tests', 500 Hz) goes over the TCP entry;
// the sharer hears each, by its viewer, as it was sent, while both hear the
// sharer's (sealed once, through the forwarder). One stops sending, then the
// sharer stops receiving: each other end hears so. Nothing plays out loud.
void forwarderMicrophone(const uint8_t (&key)[32]) {
#ifdef RCSDK_TEST_FORWARDER
    rcsdk::test::UseTestMicrophone microphone(500);
    TestForwarder fwd;
    std::mutex mutex;
    std::map<rcs_viewer_id, std::vector<float>> heard; // Left channel.
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    shareCallbacks.onAudio = [&](const rcs_audio_frame& frame) {
        std::lock_guard lock(mutex);
        auto& samples = heard[frame.viewer_id];
        for (uint32_t i = 0; i < frame.frames; ++i) {
            samples.push_back(frame.samples[i * frame.channels]);
        }
    };
    auto sharing = fwd.config(RCS_ROLE_SHARE, key, fwd.token(RCSFWD_ROLE_PUBLISH, "voices", 0))
                       .setViewers(4)
                       .setReceiveAudio(true)
                       .setAudioPlayback(false);
    auto& receive = sharing.receiveAudio();
    rcsdk::Session share(sharing, shareCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    auto pushing = fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "voices", 1));
    pushing.setSendAudio(true).setAudioSource(RCS_AUDIO_HOST);
    auto talking = fwd.config(RCS_ROLE_VIEW, key, fwd.token(RCSFWD_ROLE_SUBSCRIBE, "voices", 2), RCS_FORWARDER_TCP);
    talking.setMicrophone();
    auto& microphoneSettings = talking.sendAudio();
    TestViewer a(pushing);
    CHECK(a.session->start() == RCS_OK && a.watching());
    TestViewer b(talking);
    CHECK(b.session->start() == RCS_OK && b.watching());
    const rcs_viewer_id aId = viewerOfRoute(share, 1), bId = viewerOfRoute(share, 2);
    CHECK(aId && bId);
    std::atomic<bool> going{true};
    std::thread pusher([&] {
        std::vector<float> block(480 * 2);
        uint64_t produced = 0;
        for (auto next = std::chrono::steady_clock::now(); going; next += 10ms) {
            for (uint32_t i = 0; i < 480; ++i) {
                block[i * 2] = block[i * 2 + 1] =
                    float(0.5 * std::sin(2 * 3.14159265358979 * 750 * double(produced + i) / 48000));
            }
            produced += 480;
            a.session->pushAudio(block.data(), 480);
            std::this_thread::sleep_until(next + 10ms);
        }
    });
    const auto enough = [&](rcs_viewer_id viewer) {
        std::lock_guard lock(mutex);
        return heard[viewer].size() >= 48000;
    };
    CHECK(waitFor([&] { return enough(aId) && enough(bId); }, 10s));
    for (const auto& [viewer, hz] : {std::pair<rcs_viewer_id, double>{aId, 750}, {bId, 500}}) {
        std::vector<float> tail;
        {
            std::lock_guard lock(mutex);
            const auto& samples = heard[viewer];
            tail.assign(samples.end() - 12000, samples.end());
        }
        const auto measured = rcsdk::test::measureTone(tail);
        std::printf("forwarder microphone: the sharer hears viewer %u at %.0f Hz, rms %.3f\n", viewer, measured.hz,
                    measured.rms);
        CHECK(std::abs(measured.hz - hz) < 10 && std::abs(measured.rms - 0.354) < 0.05);
        const auto got = share.viewerStats<rcs_audio_receive_stats>(viewer);
        CHECK(got.active && got.codec == RCS_AUDIO_OPUS && got.received_packets > 0);
    }
    // The sharer's own sound reaches both meanwhile.
    for (auto* viewer : {&a, &b}) {
        CHECK(waitFor([&] { return viewer->session->stats<rcs_audio_receive_stats>().received_packets > 20; }));
    }
    const auto audioOf = [](EventLog& log, rcs_viewer_id viewer, rcs_audio_direction direction) {
        auto events = eventsOf(log, RCS_EVENT_AUDIO, viewer);
        std::erase_if(events, [&](const rcs_event& e) { return e.audio.direction != direction; });
        return events;
    };
    // B stops sending: the sharer hears so.
    microphoneSettings.enabled = 0;
    CHECK(b.session->update(microphoneSettings) == RCS_OK);
    CHECK(waitFor([&] {
        const auto events = audioOf(shareEvents, bId, RCS_AUDIO_RECEIVING);
        return !events.empty() && !events.back().audio.active &&
               std::string(events.back().audio.message) == "The viewer sends no sound";
    }));
    // The sharer stops receiving: A hears so.
    receive.enabled = 0;
    CHECK(share.update(receive) == RCS_OK);
    CHECK(waitFor([&] {
        const auto events = audioOf(a.events, 0, RCS_AUDIO_SENDING);
        return !events.empty() && !events.back().audio.active &&
               std::string(events.back().audio.message) == "The sharer receives no sound";
    }));
    going = false;
    pusher.join();
    printPipes(fwd, "forwarder microphone", "voices");
    for (auto* viewer : {&a, &b}) {
        CHECK(viewer->session->stop() == RCS_OK);
    }
    CHECK(share.stop() == RCS_OK);
#else
    (void)key;
#endif
}

// The port of an address rcs_path_info carries ("192.168.1.5:50000", "[::1]:5").
uint16_t portOf(const char* address) {
    const std::string text(address);
    return uint16_t(std::stoul(text.substr(text.rfind(':') + 1)));
}

// A sharer and a viewer whose hosts pass each other's signals at any time,
// over P2P or RELAY, with quick paths (see quickPaths).
struct SignalledPair {
    EventLog shareEvents, viewEvents;
    std::atomic<int> frames{0};
    std::mutex mutex;
    std::vector<std::string> viewSignals, shareSignals; // As each end handed them over.
    std::unique_ptr<rcsdk::Session> share, view;

    SignalledPair(rcsdk::SessionConfig shareConfig, rcsdk::SessionConfig viewConfig) {
        rcsdk::Callbacks shareCallbacks, viewCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        shareCallbacks.onSignal = [this](const std::string& text, uint64_t route) {
            CHECK(route == 5);
            {
                std::lock_guard lock(mutex);
                shareSignals.push_back(text);
            }
            view->signal(text);
        };
        viewCallbacks.onEvent = viewEvents.callback();
        viewCallbacks.onSignal = [this](const std::string& text, uint64_t) {
            {
                std::lock_guard lock(mutex);
                viewSignals.push_back(text);
            }
            share->signal(text, 5);
        };
        viewCallbacks.onFrame = [this](const rcs_frame&) {
            ++frames;
        };
        share = std::make_unique<rcsdk::Session>(quickPaths(shareConfig), shareCallbacks);
        view = std::make_unique<rcsdk::Session>(quickPaths(reconnecting(viewConfig, 30000, 500)), viewCallbacks);
    }
    bool watching(int count = 10) {
        const int before = frames;
        return waitFor([&] { return frames >= before + count; }, 10s);
    }
    // What a viewer's own path's socket sends and receives is lost from now on.
    void cutViewerPath() {
        const uint16_t port = portOf(view->pathInfo().local_address);
        rcsdk::transport::setDatagramFilter([port](const rcsdk::transport::Endpoint& local,
                                                   const rcsdk::transport::Endpoint&) { return local.port() != port; });
    }
};

// P2P: the path breaks (the viewer's socket reaches nothing any more). Both
// ends restart it through the host's signalling, sealed with the
// connection's keys; ICE finds a new direct path and the connection moves
// there (signaled), still the one connection, never through a relay. A
// restart changed on the way is refused; one for a connection the sharer
// does not have is answered so.
void migrationP2p(const uint8_t (&key)[32]) {
    SignalledPair pair(config(RCS_ROLE_SHARE, 0, key).setTransport(RCS_TRANSPORT_P2P),
                       config(RCS_ROLE_VIEW, 0, key).setTransport(RCS_TRANSPORT_P2P));
    CHECK(pair.share->start() == RCS_OK);
    CHECK(waitFor([&] { return pair.share->state() == RCS_STATE_LISTENING; }));
    CHECK(pair.view->start() == RCS_OK && pair.watching(20));
    const auto before = pair.view->pathInfo();
    pair.cutViewerPath();
    const auto signaled = [](EventLog& log, rcs_viewer_id viewer) {
        auto changed = pathEvents(log, RCS_PATH_CHANGED, 0, viewer);
        std::erase_if(changed, [](const rcs_event& e) { return !e.path.signaled; });
        return changed;
    };
    CHECK(
        waitFor([&] { return !signaled(pair.viewEvents, 0).empty() && !signaled(pair.shareEvents, 1).empty(); }, 20s));
    rcsdk::transport::setDatagramFilter({});
    CHECK(pair.watching());
    const auto after = pair.view->pathInfo();
    CHECK((after.route == RCS_ROUTE_DIRECT || after.route == RCS_ROUTE_NAT) &&
          portOf(after.local_address) != portOf(before.local_address));
    CHECK(signaled(pair.viewEvents, 0).back().path.to.route != RCS_ROUTE_RELAY);
    CHECK(pair.view->stats<rcs_network_stats>().connections == 1 &&
          pair.share->stats<rcs_network_stats>().connections == 1 && pair.view->state() == RCS_STATE_RUNNING);
    std::string restart;
    {
        std::lock_guard lock(pair.mutex);
        for (const auto& text : pair.viewSignals) {
            if (rcsdk::transport::ice::signalKind(text) == rcsdk::transport::ice::SignalKind::restart) {
                restart = text;
            }
        }
    }
    CHECK(!restart.empty());
    auto changed = restart;
    changed[changed.size() - 5] = changed[changed.size() - 5] == 'A' ? 'B' : 'A';
    CHECK(pair.share->signal(changed, 5) == RCS_ERROR_AUTHENTICATION);
    // A restart for a connection the sharer does not have: it says so on the route.
    const std::array<uint8_t, 32> stranger{4, 4, 4};
    rcsdk::transport::ice::Restart unknown{
        {1, 2, 3, 4, 5, 6, 7, 8}, 1, false, rcsdk::transport::ice::decode(pair.viewSignals.at(0))};
    const size_t answers = [&] {
        std::lock_guard lock(pair.mutex);
        return pair.shareSignals.size();
    }();
    CHECK(pair.share->signal(rcsdk::transport::ice::encodeRestart(unknown, stranger), 5) == RCS_OK);
    {
        std::lock_guard lock(pair.mutex);
        CHECK(pair.shareSignals.size() == answers + 1 && rcsdk::transport::ice::signalKind(pair.shareSignals.back()) ==
                                                             rcsdk::transport::ice::SignalKind::unknown);
    }
    CHECK(pair.view->stop() == RCS_OK);
    CHECK(pair.share->stop() == RCS_OK);
}

// RELAY: the viewer's network changes under its allocation, and the TURN
// server grants mobility (RFC 8016): the allocation moves to a new socket,
// its relayed address stays and the sharer notices nothing, nothing
// signaled. A server that refuses the move: a new allocation on each end,
// through the host's signalling (signaled). Never a direct path.
void migrationRelay(const uint8_t (&key)[32]) {
    rcsdk::test::TurnServer server("host", "s3cret");
    const auto relay = [&](rcs_role role) {
        return config(role, 0, key).setRelay(server.addressText(), "host", "s3cret");
    };
    SignalledPair pair(relay(RCS_ROLE_SHARE), relay(RCS_ROLE_VIEW));
    CHECK(pair.share->start() == RCS_OK);
    CHECK(waitFor([&] { return pair.share->state() == RCS_STATE_LISTENING; }));
    CHECK(pair.view->start() == RCS_OK && pair.watching(20));
    CHECK(server.allocations == 2);
    const auto before = pair.view->pathInfo();
    CHECK(before.route == RCS_ROUTE_RELAY);

    pair.cutViewerPath();
    rcsdk::notifyNetworkChange();
    CHECK(waitFor([&] { return !pathEvents(pair.viewEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_NETWORK).empty(); }, 10s));
    rcsdk::transport::setDatagramFilter({});
    CHECK(pair.watching());
    // Both ends in this process hear of the change: the sharer, whose path
    // went silent as well, may move its allocation too.
    CHECK(server.moves >= 1 && server.allocations == 2);
    {
        const auto changed = pathEvents(pair.viewEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_NETWORK).back().path;
        CHECK(!changed.signaled && changed.to.route == RCS_ROUTE_RELAY &&
              portOf(changed.to.local_address) != portOf(before.local_address));
    }
    CHECK(pathEvents(pair.shareEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_REBINDING, 1).empty());

    // The server moves no more: the allocations are made anew, through the host's signalling.
    server.mobility = false;
    pair.cutViewerPath();
    rcsdk::notifyNetworkChange();
    CHECK(waitFor(
        [&] {
            auto changed = pathEvents(pair.viewEvents, RCS_PATH_CHANGED);
            return std::any_of(changed.begin(), changed.end(), [](const rcs_event& e) { return e.path.signaled != 0; });
        },
        20s));
    rcsdk::transport::setDatagramFilter({});
    CHECK(pair.watching());
    // New allocations: the viewer's, and the sharer's in answer (or of its own as well).
    CHECK(server.allocations >= 4 && pair.view->pathInfo().route == RCS_ROUTE_RELAY);
    CHECK(pair.view->stats<rcs_network_stats>().connections == 1 &&
          pair.share->stats<rcs_network_stats>().connections == 1);
    CHECK(pair.view->stop() == RCS_OK);
    CHECK(pair.share->stop() == RCS_OK);
}

// This machine's networks as a test sets them (seams::setNetworks), all of
// them the loopback: a socket that is to use one is bound to none.
struct FakeNetworks {
    std::mutex mutex;
    std::vector<rcsdk::platform::Network> list;
    FakeNetworks() {
        rcsdk::seams::setNetworks([this] {
            std::lock_guard lock(mutex);
            return list;
        });
    }
    ~FakeNetworks() { rcsdk::seams::setNetworks({}); }
    FakeNetworks(const FakeNetworks&) = delete;
    FakeNetworks& operator=(const FakeNetworks&) = delete;
    // The networks from now on (the default first), and the system says they changed.
    void set(std::vector<rcsdk::platform::Network> networks) {
        {
            std::lock_guard lock(mutex);
            list = std::move(networks);
        }
        rcsdk::notifyNetworkChange();
    }
};
const rcsdk::platform::Network fakeWifi{"wifi0", 101, RCS_NETWORK_WIFI, 0, true, false};
const rcsdk::platform::Network fakeCellular{"cell0", 102, RCS_NETWORK_CELLULAR, RCS_NETWORK_FLAG_METERED, true, false};
rcsdk::platform::Network preferred(rcsdk::platform::Network network) {
    network.flags |= RCS_NETWORK_FLAG_DEFAULT;
    return network;
}

template <class Config>
rcsdk::SessionConfig& withPaths(Config&& c, uint32_t excluded, rcs_path_policy policy, uint32_t standby = 0) {
    auto& paths = c.template extension<rcs_paths_config>();
    paths.excluded_networks = excluded;
    paths.policy = policy;
    paths.standby = standby;
    return c;
}

// This machine's Wi-Fi as the system says (seams::setWifi), and which of
// it AWDL takes the radio from: up beside a network on a 2.4 or 5 GHz
// channel other than 6, 44 and 149 (other_channel); not on those, not on
// 6 GHz, not with AWDL down.
// Then a path that stalls 80 ms every 1049 ms (as such a radio does): the
// viewer finds the pace in what it receives (RCS_EVENT_STALLS from_peer 0,
// with its path; its Wi-Fi none: the loopback is no network the system
// lists), and tells the sharer (from_peer 1, the viewer's id); the
// statistics have both. Steady again: the pace is over (period 0) on both
// ends within 10 s.
void pathStalls(const uint8_t (&key)[32]) {
    const auto wifi = [](rcs_wifi_band band, uint32_t channel, uint32_t awdl) {
        rcs_wifi_info w{};
        std::snprintf(w.name, sizeof(w.name), "wifi%u", channel);
        w.band = band;
        w.channel = channel;
        w.peer_to_peer = awdl;
        return w;
    };
    rcsdk::seams::setWifi([&] {
        return std::vector<rcs_wifi_info>{wifi(RCS_WIFI_BAND_2_4_GHZ, 2, 1),  wifi(RCS_WIFI_BAND_2_4_GHZ, 6, 1),
                                          wifi(RCS_WIFI_BAND_5_GHZ, 44, 1),   wifi(RCS_WIFI_BAND_5_GHZ, 149, 1),
                                          wifi(RCS_WIFI_BAND_5_GHZ, 36, 1),   wifi(RCS_WIFI_BAND_6_GHZ, 37, 1),
                                          wifi(RCS_WIFI_BAND_5_GHZ, 157, 0)};
    });
    const auto listed = rcsdk::wifi();
    CHECK(listed.size() == 7);
    for (size_t i = 0; i < listed.size(); ++i) {
        CHECK(listed[i].other_channel == (i == 0 || i == 4));
    }
    rcs_capabilities capabilities = RCS_CAPABILITIES_INIT;
    CHECK(rcs_get_capabilities(&capabilities) == RCS_OK && (capabilities.wifi_facts & RCS_WIFI_FACT_PEER_TO_PEER));
    rcsdk::seams::setWifi({});

    EventLog shareEvents, viewEvents;
    rcsdk::Callbacks shareCallbacks, viewCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    viewCallbacks.onEvent = viewEvents.callback();
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47997, key).setTransport(RCS_TRANSPORT_UDP).setVideo(640, 360, 60,
                                                                                                   2'000'000),
                         shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47997));
    proxy.delayMs = 3;
    proxy.sleepEveryMs = 1049;
    proxy.sleepMs = 80;
    rcsdk::Session view(config(RCS_ROLE_VIEW, proxy.address().port(), key).setTransport(RCS_TRANSPORT_UDP),
                        viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    const auto paced = [](EventLog& log, uint32_t fromPeer) {
        for (const auto& e : log.of(RCS_EVENT_STALLS)) {
            if (e.stalls.from_peer == fromPeer && e.stalls.period_ms >= 1000 && e.stalls.period_ms <= 1100) {
                return std::optional(e);
            }
        }
        return std::optional<rcs_event>();
    };
    CHECK(waitFor([&] { return paced(viewEvents, 0) && paced(shareEvents, 1); }, 20s));
    const auto seen = *paced(viewEvents, 0), told = *paced(shareEvents, 1);
    const auto network = view.stats<rcs_network_stats>();
    std::printf("stalls: the viewer sees %u in 10 s, median %u ms, longest %u ms, pace %u ms; the sharer was told "
                "%u ms (viewer %u)\n",
                network.stalls, network.stall_ms, network.longest_stall_ms, network.stall_period_ms,
                told.stalls.period_ms, told.viewer_id);
    CHECK(seen.viewer_id == 0 && seen.stalls.stalls >= 4 && seen.stalls.stall_ms >= 50 && seen.stalls.stall_ms <= 130);
    CHECK(seen.stalls.path.route == RCS_ROUTE_DIRECT && seen.stalls.path.remote_address[0] && !seen.stalls.wifi.name[0]);
    CHECK(told.viewer_id == 1 && told.stalls.stall_ms == seen.stalls.stall_ms);
    CHECK(network.stall_period_ms >= 1000 && network.stall_period_ms <= 1100 && network.stalls >= 4);
    const auto heard = share.viewerStats<rcs_network_stats>(1);
    CHECK(heard.peer_stall_period_ms == told.stalls.period_ms && heard.peer_stall_ms == told.stalls.stall_ms);
    // Steady again.
    proxy.sleepEveryMs = 0;
    const auto over = [](EventLog& log, uint32_t fromPeer) {
        const auto events = log.of(RCS_EVENT_STALLS);
        return !events.empty() && std::any_of(events.begin(), events.end(), [&](const rcs_event& e) {
                   return e.stalls.from_peer == fromPeer && e.stalls.period_ms == 0;
               });
    };
    CHECK(waitFor([&] { return over(viewEvents, 0) && over(shareEvents, 1); }, 15s));
    CHECK(view.stats<rcs_network_stats>().stall_period_ms == 0 &&
          share.viewerStats<rcs_network_stats>(1).peer_stall_period_ms == 0);
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// rcs_paths_config choosing this end's networks, on networks the test makes
// up: where nothing is this end's to choose, it is refused. A viewer whose
// network may no longer be used loses its path at once and keeps its
// connection; allowing another network moves it there (SETTINGS). STAY keeps
// it where it is while the system's default network moves; FOLLOW_SYSTEM
// moves with it.
void networkPolicy(const uint8_t (&key)[32]) {
    FakeNetworks networks;
    networks.set({preferred(fakeWifi), fakeCellular});
    const auto listed = rcsdk::networks();
    CHECK(listed.size() == 2 && std::string(listed[0].name) == "wifi0" &&
          (listed[0].flags & RCS_NETWORK_FLAG_DEFAULT) && listed[1].kind == RCS_NETWORK_CELLULAR &&
          (listed[1].flags & RCS_NETWORK_FLAG_METERED) && listed[1].index == 102);
    rcs_capabilities capabilities = RCS_CAPABILITIES_INIT;
    CHECK(rcs_get_capabilities(&capabilities) == RCS_OK && (capabilities.network_facts & RCS_NETWORK_FACT_KIND));
    const uint32_t noCellular = 1u << RCS_NETWORK_CELLULAR;
    {
        rcsdk::Session listening(
            withPaths(config(RCS_ROLE_SHARE, 47967, key), noCellular, RCS_PATH_POLICY_FOLLOW_SYSTEM));
        CHECK(listening.start() == RCS_ERROR_UNSUPPORTED);
        auto both = withPaths(config(RCS_ROLE_VIEW, 47967, key), noCellular, RCS_PATH_POLICY_FOLLOW_SYSTEM);
        std::strcpy(both.extension<rcs_network_tuning_config>().local_interface, "wifi0");
        CHECK(rcsdk::Session(both).start() == RCS_ERROR_INVALID_ARGUMENT);
        rcsdk::Session tcp(withPaths(config(RCS_ROLE_SHARE, 47967, key).setTransport(RCS_TRANSPORT_TCP), 0, 0, 1));
        CHECK(tcp.start() == RCS_ERROR_UNSUPPORTED);
        auto undefined = rcsdk::defaults<rcs_paths_config>();
        undefined.excluded_networks = 1u << (RCS_NETWORK_OTHER + 1);
        CHECK(listening.update(undefined) == RCS_ERROR_INVALID_ARGUMENT);
    }

    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(quickPaths(config(RCS_ROLE_SHARE, 47967, key)), shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47967));
    TestViewer viewer(
        withPaths(quickPaths(reconnecting(config(RCS_ROLE_VIEW, proxy.address().port(), key), 30000, 500)), noCellular,
                  RCS_PATH_POLICY_FOLLOW_SYSTEM));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(viewer.session->start() == RCS_OK && viewer.watching());
    const auto settled = [&](size_t changes) {
        return waitFor([&] {
                   return pathEvents(viewer.events, RCS_PATH_CHANGED, RCS_PATH_CAUSE_SETTINGS).size() == changes;
               }) &&
               viewer.watching();
    };

    // Wi-Fi goes, and cellular may not be used: the path is lost at once, the connection kept.
    networks.set({preferred(fakeCellular)});
    CHECK(waitFor([&] { return !pathEvents(viewer.events, RCS_PATH_LOST, RCS_PATH_CAUSE_NETWORK).empty(); }));
    CHECK(std::string(pathEvents(viewer.events, RCS_PATH_LOST).back().path.message).find("excluded") !=
          std::string::npos);
    CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RECONNECTING; }));
    // Cellular is allowed: the connection moves there.
    auto paths = rcsdk::defaults<rcs_paths_config>();
    CHECK(viewer.session->update(paths) == RCS_OK && settled(1));
    CHECK(viewer.session->state() == RCS_STATE_RUNNING);

    // STAY: on the default network, Wi-Fi again, and there it stays while the default moves.
    networks.set({preferred(fakeWifi), fakeCellular});
    paths.policy = RCS_PATH_POLICY_STAY;
    CHECK(viewer.session->update(paths) == RCS_OK && settled(2));
    networks.set({preferred(fakeCellular), fakeWifi});
    std::this_thread::sleep_for(1s);
    CHECK(pathEvents(viewer.events, RCS_PATH_CHANGED, RCS_PATH_CAUSE_SETTINGS).size() == 2);
    // FOLLOW_SYSTEM (and a choice to make): to the default network.
    paths.policy = RCS_PATH_POLICY_FOLLOW_SYSTEM;
    paths.excluded_networks = 1u << RCS_NETWORK_OTHER;
    CHECK(viewer.session->update(paths) == RCS_OK && settled(3));
    const auto moves = pathEvents(viewer.events, RCS_PATH_CHANGED, RCS_PATH_CAUSE_SETTINGS);
    CHECK(portOf(moves[2].path.to.local_address) != portOf(moves[1].path.to.local_address));
    CHECK(viewer.session->stats<rcs_network_stats>().connections == 1 &&
          share.stats<rcs_network_stats>().connections == 1 && shareEvents.of(RCS_EVENT_VIEWER).size() == 1);
    CHECK(viewer.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// rcs_paths_config.standby, on networks the test makes up: the viewer keeps a
// second path ready over its other network (STANDBY), takes it as soon as
// its path stalls (no signalling, no new socket to try), and goes back once
// the first one answers again. The sharer just follows.
void standbyPath(const uint8_t (&key)[32]) {
    FakeNetworks networks;
    networks.set({preferred(fakeWifi), fakeCellular});
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(quickPaths(config(RCS_ROLE_SHARE, 47968, key)), shareCallbacks);
    rcsdk::test::ShapingProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47968));
    auto viewConfig =
        withPaths(quickPaths(reconnecting(config(RCS_ROLE_VIEW, proxy.address().port(), key), 30000, 500)), 0,
                  RCS_PATH_POLICY_FOLLOW_SYSTEM, 1);
    viewConfig.extension<rcs_network_tuning_config>().standby_probe_ms = 1000;
    TestViewer viewer(viewConfig);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(viewer.session->start() == RCS_OK && viewer.watching());
    const auto ready = [&](size_t count) {
        return waitFor([&] {
            auto events = pathEvents(viewer.events, RCS_PATH_STANDBY);
            return std::count_if(events.begin(), events.end(), [](const rcs_event& e) {
                       return e.path.result == RCS_OK && e.path.to.route == RCS_ROUTE_DIRECT;
                   }) == ptrdiff_t(count);
        });
    };
    CHECK(ready(1));
    CHECK(waitFor([&] { return viewer.session->stats<rcs_network_stats>().standby == 1; }));
    rcs_path_info active{}, standby{};
    CHECK(rcs_session_get_path_info(viewer.session->handle(), 0, &active, &standby) == RCS_OK);
    CHECK(standby.route == RCS_ROUTE_DIRECT && portOf(standby.local_address) != portOf(active.local_address));
    const auto stalled = [&] {
        return pathEvents(viewer.events, RCS_PATH_CHANGED, RCS_PATH_CAUSE_STALLED);
    };

    // Wi-Fi stops carrying anything: the standby path takes over.
    const auto first = rcsdk::transport::Endpoint::parse("127.0.0.1", portOf(active.local_address));
    proxy.block(first);
    CHECK(waitFor([&] { return stalled().size() == 1; }, 5s));
    CHECK(portOf(stalled()[0].path.to.local_address) == portOf(standby.local_address));
    CHECK(viewer.watching());
    CHECK(waitFor([&] { return !pathEvents(shareEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_PEER, 1).empty(); }));
    // It answers again: back to it, and the standby path is ready again.
    proxy.unblock(first);
    CHECK(waitFor([&] { return stalled().size() == 2; }, 5s));
    CHECK(portOf(stalled()[1].path.to.local_address) == portOf(active.local_address));
    CHECK(ready(2) && viewer.watching());
    const auto states = viewer.events.of(RCS_EVENT_STATE);
    CHECK(std::none_of(states.begin(), states.end(),
                       [](const rcs_event& e) { return e.state.value == RCS_STATE_RECONNECTING; }));

    // Standby off: the standby path goes.
    CHECK(viewer.session->update(rcsdk::defaults<rcs_paths_config>()) == RCS_OK);
    CHECK(waitFor([&] {
        const auto events = pathEvents(viewer.events, RCS_PATH_STANDBY, RCS_PATH_CAUSE_SETTINGS);
        return !events.empty() && events.back().path.to.route == RCS_ROUTE_NONE;
    }));
    CHECK(waitFor([&] { return viewer.session->stats<rcs_network_stats>().standby == 0; }));
    CHECK(viewer.session->stats<rcs_network_stats>().connections == 1 &&
          share.stats<rcs_network_stats>().connections == 1);
    CHECK(viewer.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// P2P with a standby path (the viewer's, on networks the test makes up): it
// is found through the host's signalling beside the path in use, and neither
// end takes it while that works. When the path in use breaks, both move to
// it without a signal; once the first one answers again, back to it.
void standbyP2p(const uint8_t (&key)[32]) {
    FakeNetworks networks;
    networks.set({preferred(fakeWifi), fakeCellular});
    auto viewConfig =
        withPaths(config(RCS_ROLE_VIEW, 0, key).setTransport(RCS_TRANSPORT_P2P), 0, RCS_PATH_POLICY_FOLLOW_SYSTEM, 1);
    viewConfig.extension<rcs_network_tuning_config>().standby_probe_ms = 1000;
    SignalledPair pair(config(RCS_ROLE_SHARE, 0, key).setTransport(RCS_TRANSPORT_P2P), viewConfig);
    CHECK(pair.share->start() == RCS_OK);
    CHECK(waitFor([&] { return pair.share->state() == RCS_STATE_LISTENING; }));
    CHECK(pair.view->start() == RCS_OK && pair.watching(20));
    // (Its first attempt may come before the sharer has the viewer: it hears so, and tries again.)
    CHECK(waitFor(
        [&] {
            const auto ready = pathEvents(pair.viewEvents, RCS_PATH_STANDBY);
            return !ready.empty() && ready.back().path.result == RCS_OK && ready.back().path.to.route != RCS_ROUTE_NONE;
        },
        10s));
    rcs_path_info active{}, standby{};
    CHECK(rcs_session_get_path_info(pair.view->handle(), 0, &active, &standby) == RCS_OK);
    CHECK(standby.route != RCS_ROUTE_NONE && portOf(standby.local_address) != portOf(active.local_address));
    CHECK(pathEvents(pair.viewEvents, RCS_PATH_CHANGED).empty() &&
          pathEvents(pair.shareEvents, RCS_PATH_CHANGED).empty());
    const auto restarts = [&] {
        std::lock_guard lock(pair.mutex);
        return std::count_if(pair.viewSignals.begin(), pair.viewSignals.end(), [](const std::string& text) {
            return rcsdk::transport::ice::signalKind(text) == rcsdk::transport::ice::SignalKind::restart;
        });
    };
    const auto signalled = restarts();
    CHECK(signalled >= 1);
    const auto stalled = [&] {
        return pathEvents(pair.viewEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_STALLED);
    };

    pair.cutViewerPath();
    CHECK(waitFor([&] { return stalled().size() == 1; }, 5s));
    CHECK(portOf(stalled()[0].path.to.local_address) ==
          portOf(standby.local_address)); // Found through the signalling before.
    CHECK(pair.watching());
    CHECK(waitFor([&] { return !pathEvents(pair.shareEvents, RCS_PATH_CHANGED, 0, 1).empty(); }));
    // Back to the first path, whichever end heard it answer first (the other follows it).
    rcsdk::transport::setDatagramFilter({});
    CHECK(waitFor(
        [&] {
            const auto changed = pathEvents(pair.viewEvents, RCS_PATH_CHANGED);
            return changed.size() >= 2 && portOf(changed.back().path.to.local_address) == portOf(active.local_address);
        },
        5s));
    CHECK(pair.watching() && restarts() == signalled);
    CHECK(pair.view->stats<rcs_network_stats>().connections == 1 &&
          pair.share->stats<rcs_network_stats>().connections == 1 && pair.view->state() == RCS_STATE_RUNNING);
    CHECK(pair.view->stop() == RCS_OK);
    CHECK(pair.share->stop() == RCS_OK);
}

// ---- Network changes over TCP -------------------------------------------------------------

// A TCP viewer of the sharer behind `proxy`, tuned as quickPaths.
rcsdk::SessionConfig tcpViewer(rcsdk::test::TcpProxy& proxy, const uint8_t (&key)[32]) {
    rcsdk::SessionConfig c = config(RCS_ROLE_VIEW, proxy.address().port(), key);
    c.setTransport(RCS_TRANSPORT_TCP);
    return quickPaths(reconnecting(c, 30000, 500));
}

// TCP: the viewer's TCP connection breaks (reset on the way); later it stays
// open but carries nothing while the system says the network changed; then it
// breaks while the sharer cannot be reached for a moment. Each time the
// viewer connects to the sharer anew over TCP, and the same connection goes
// on there. One viewer (id 1), one admission, control kept (its input
// arrives), the data messages once each and in order, the viewer never
// RECONNECTING; both hosts hear of each change.
void migrationTcp(const uint8_t (&key)[32]) {
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    {
        EventLog shareEvents;
        DataLog shareData;
        std::atomic<int> joins{0};
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        shareCallbacks.onData = shareData.callback();
        shareCallbacks.onViewerJoin = [&](const rcs_viewer_info&, rcs_viewer_decision&) {
            ++joins;
        };
        rcsdk::Session share(
            quickPaths(config(RCS_ROLE_SHARE, 47931, key).setTransport(RCS_TRANSPORT_TCP).setControl(true)),
            shareCallbacks);
        rcsdk::test::TcpProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47931));
        TestViewer viewer(tcpViewer(proxy, key));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(viewer.session->start() == RCS_OK && viewer.watching());
        CHECK(waitFor([&] { return viewer.session->controlInfo().granted != 0; }));
        std::atomic<bool> sending{true};
        std::atomic<int> sent{0};
        std::thread sender([&] {
            while (sending) {
                if (viewer.session->sendData(1, std::to_string(sent.load())) == RCS_OK) {
                    ++sent;
                }
                std::this_thread::sleep_for(5ms);
            }
        });
        const auto changed = [&](EventLog& log, rcs_path_cause cause, rcs_viewer_id id) {
            return pathEvents(log, RCS_PATH_CHANGED, cause, id).size();
        };

        // The TCP connection breaks: the viewer connects anew at once.
        proxy.cut();
        CHECK(waitFor([&] { return changed(viewer.events, RCS_PATH_CAUSE_NETWORK, 0) == 1; }));
        CHECK(!pathEvents(viewer.events, RCS_PATH_CHANGING, RCS_PATH_CAUSE_NETWORK).empty());
        CHECK(!pathEvents(shareEvents, RCS_PATH_CHANGING, RCS_PATH_CAUSE_PEER, 1).empty());
        CHECK(waitFor([&] { return changed(shareEvents, RCS_PATH_CAUSE_PEER, 1) == 1; }));
        CHECK(viewer.watching() && proxy.accepted() == 2);

        // It stays open but carries nothing, and the system says the network changed.
        proxy.freeze();
        rcsdk::notifyNetworkChange();
        CHECK(waitFor([&] { return changed(viewer.events, RCS_PATH_CAUSE_NETWORK, 0) == 2; }));
        CHECK(waitFor([&] { return changed(shareEvents, RCS_PATH_CAUSE_PEER, 1) == 2; }));
        CHECK(viewer.watching());
        {
            const auto moved = pathEvents(viewer.events, RCS_PATH_CHANGED, RCS_PATH_CAUSE_NETWORK).back().path;
            CHECK(moved.to.route == RCS_ROUTE_DIRECT && !moved.signaled &&
                  std::string(moved.to.remote_address) == proxy.address().toString() &&
                  std::string(moved.to.local_address) == proxy.clients().back().toString());
        }

        // It breaks while the sharer cannot be reached for a moment (the
        // network under it is changing): the viewer tries every second, and
        // goes on over the next TCP connection before the path is lost.
        proxy.refuse(true);
        proxy.cut();
        std::this_thread::sleep_for(600ms);
        proxy.refuse(false);
        CHECK(waitFor([&] { return changed(viewer.events, RCS_PATH_CAUSE_NETWORK, 0) == 3; }));
        CHECK(waitFor([&] { return changed(shareEvents, RCS_PATH_CAUSE_PEER, 1) == 3; }));
        CHECK(viewer.watching() && pathEvents(viewer.events, RCS_PATH_LOST).empty() && proxy.accepted() == 4);

        rcs_input_event press = RCS_INPUT_EVENT_INIT;
        press.type = RCS_INPUT_KEY;
        press.code = 4;
        press.down = 1;
        CHECK(viewer.session->sendInput(press) == RCS_OK && waitFor([&] { return recorder.count() == 1; }));

        sending = false;
        sender.join();
        CHECK(waitFor([&] { return shareData.count(true) == size_t(sent.load()); }));
        {
            std::lock_guard lock(shareData.mutex);
            for (size_t i = 0; i < shareData.reliable.size(); ++i) {
                CHECK(std::string(shareData.reliable[i].begin(), shareData.reliable[i].end()) == std::to_string(i));
            }
        }
        for (auto* session : {&share, viewer.session.get()}) {
            CHECK(waitFor([&] { return session->stats<rcs_network_stats>().path_changes >= 3; }));
            const auto network = session->stats<rcs_network_stats>();
            CHECK(network.connections == 1 && network.route == RCS_ROUTE_DIRECT && !network.path_lost);
        }
        CHECK(joins == 1 && shareEvents.of(RCS_EVENT_VIEWER).size() == 1);
        CHECK(share.viewers().size() == 1 && share.viewers()[0].viewer_id == 1 && share.viewers()[0].control.allowed);
        const auto states = viewer.events.of(RCS_EVENT_STATE);
        CHECK(std::none_of(states.begin(), states.end(),
                           [](const rcs_event& e) { return e.state.value == RCS_STATE_RECONNECTING; }));
        CHECK(viewer.session->stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::seams::setInputOverride({});
}

// TCP: the network goes away for longer than idle_timeout (the TCP connection
// stays open and carries nothing, new ones are refused): the viewer reports
// RECONNECTING and keeps trying, the sharer keeps it and revokes its control.
// Once the network is back (the old TCP connection broke meanwhile) the
// viewer's next TCP connection carries the same connection: RUNNING again
// without new PEER or STREAM events, its control for the host to grant.
void pathHoldTcp(const uint8_t (&key)[32]) {
    InputRecorder recorder;
    rcsdk::seams::InputOverride input;
    input.factory = [&](const rcsdk::platform::CaptureTarget&, const rcsdk::platform::InputOptions&) {
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [] {
        return RCS_PERMISSION_GRANTED;
    };
    overrideInput(std::move(input));
    {
        EventLog shareEvents;
        rcsdk::Callbacks shareCallbacks;
        shareCallbacks.onEvent = shareEvents.callback();
        rcsdk::Session share(
            quickPaths(config(RCS_ROLE_SHARE, 47932, key).setTransport(RCS_TRANSPORT_TCP).setControl(true)),
            shareCallbacks);
        rcsdk::test::TcpProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47932));
        TestViewer viewer(tcpViewer(proxy, key));
        CHECK(share.start() == RCS_OK);
        CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
        CHECK(viewer.session->start() == RCS_OK && viewer.watching());
        CHECK(waitFor([&] { return viewer.session->controlInfo().granted != 0; }));

        proxy.freeze();
        proxy.refuse(true);
        CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RECONNECTING; }, 5s));
        CHECK(!pathEvents(viewer.events, RCS_PATH_LOST).empty());
        CHECK(waitFor([&] { return !pathEvents(shareEvents, RCS_PATH_LOST, 0, 1).empty(); }));
        CHECK(waitFor([&] { return !share.viewers().at(0).control.allowed; }));
        std::this_thread::sleep_for(1500ms); // Its attempts are refused meanwhile.
        CHECK(viewer.session->state() == RCS_STATE_RECONNECTING && share.viewers().size() == 1);

        proxy.cut();
        proxy.refuse(false);
        CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RUNNING; }, 5s) && viewer.watching());
        const auto back = pathEvents(viewer.events, RCS_PATH_CHANGED).back().path;
        CHECK(back.interruption_ns >= 2'000'000'000ull && back.to.route == RCS_ROUTE_DIRECT);
        CHECK(viewer.events.of(RCS_EVENT_PEER).size() == 1);
        const auto streams = viewer.events.of(RCS_EVENT_STREAM);
        CHECK(std::count_if(streams.begin(), streams.end(),
                            [](const rcs_event& e) { return (e.stream.changes & RCS_STREAM_CODEC) != 0; }) == 1);
        CHECK(viewer.session->stats<rcs_network_stats>().connections == 1 &&
              share.stats<rcs_network_stats>().connections == 1);
        CHECK(waitFor([&] { return !share.viewerStats<rcs_network_stats>(1).path_lost; }));
        CHECK(!viewer.session->controlInfo().granted); // Until the host grants it again.
        CHECK(share.setViewerControl(1, true) == RCS_OK);
        CHECK(waitFor([&] { return viewer.session->controlInfo().granted != 0; }));
        CHECK(viewer.session->stop() == RCS_OK);
        CHECK(share.stop() == RCS_OK);
    }
    rcsdk::seams::setInputOverride({});
}

// TCP: a sharer keeps a lost viewer for path_hold_ms only. Once the network
// is back, the viewer's next TCP connection hears that the sharer no longer
// knows its connection, and it connects anew at once.
void pathHoldExpiredTcp(const uint8_t (&key)[32]) {
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(
        quickPaths(config(RCS_ROLE_SHARE, 47933, key).setTransport(RCS_TRANSPORT_TCP).setKeepListening(true), 1000),
        shareCallbacks);
    rcsdk::test::TcpProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47933));
    auto viewConfig = tcpViewer(proxy, key);
    viewConfig.extension<rcs_network_tuning_config>().reconnect_interval_ms = 5000;
    TestViewer viewer(viewConfig);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(viewer.session->start() == RCS_OK && viewer.watching());
    proxy.freeze();
    proxy.refuse(true);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }, 6s));
    {
        const auto left = shareEvents.of(RCS_EVENT_VIEWER).back().viewer;
        CHECK(!left.joined && left.result == RCS_ERROR_NETWORK);
    }
    CHECK(viewer.session->state() == RCS_STATE_RECONNECTING);
    const auto restored = std::chrono::steady_clock::now();
    proxy.cut();
    proxy.refuse(false);
    CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RUNNING; }, 5s) && viewer.watching());
    CHECK(std::chrono::steady_clock::now() - restored < 4s); // Not after its 5 s interval.
    CHECK(viewer.session->stats<rcs_network_stats>().connections == 2 &&
          share.stats<rcs_network_stats>().connections == 2);
    const auto states = viewer.events.of(RCS_EVENT_STATE);
    CHECK(std::count_if(states.begin(), states.end(),
                        [](const rcs_event& e) { return e.state.value == RCS_STATE_RUNNING; }) == 2);
    CHECK(viewer.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// TCP without migration (rcs_paths_config.migrate 0, the viewer's): a broken
// TCP connection ends the connection as before, and the viewer connects anew,
// as a new viewer.
void tcpNoMigrate(const uint8_t (&key)[32]) {
    std::atomic<int> joins{0};
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onViewerJoin = [&](const rcs_viewer_info&, rcs_viewer_decision&) {
        ++joins;
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47934, key).setTransport(RCS_TRANSPORT_TCP).setKeepListening(true),
                         shareCallbacks);
    rcsdk::test::TcpProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47934));
    auto viewConfig = tcpViewer(proxy, key);
    TestViewer viewer(viewConfig.setMigrate(false));
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(viewer.session->start() == RCS_OK && viewer.watching());
    proxy.cut();
    CHECK(waitFor([&] { return viewer.session->stats<rcs_network_stats>().connections == 2; }, 5s));
    CHECK(waitFor([&] { return viewer.session->state() == RCS_STATE_RUNNING; }) && viewer.watching());
    CHECK(joins == 2 && share.stats<rcs_network_stats>().connections == 2);
    const auto states = viewer.events.of(RCS_EVENT_STATE);
    CHECK(std::any_of(states.begin(), states.end(),
                      [](const rcs_event& e) { return e.state.value == RCS_STATE_RECONNECTING; }));
    CHECK(pathEvents(viewer.events, RCS_PATH_CHANGED).empty());
    CHECK(viewer.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// TCP with a standby path (on networks the test makes up): a second TCP
// connection is kept proven beside the one in use (STANDBY); when that one
// carries nothing, the standby takes the traffic as soon as the path stalls,
// with no connection to make, and once the first answers again, back to it.
// A standby TCP connection that breaks is made anew, and so is one when the
// path the standby took over from breaks.
void standbyTcp(const uint8_t (&key)[32]) {
    FakeNetworks networks;
    networks.set({preferred(fakeWifi), fakeCellular});
    EventLog shareEvents;
    rcsdk::Callbacks shareCallbacks;
    shareCallbacks.onEvent = shareEvents.callback();
    rcsdk::Session share(quickPaths(config(RCS_ROLE_SHARE, 47935, key).setTransport(RCS_TRANSPORT_TCP)),
                         shareCallbacks);
    rcsdk::test::TcpProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47935));
    auto viewConfig = withPaths(tcpViewer(proxy, key), 0, RCS_PATH_POLICY_FOLLOW_SYSTEM, 1);
    viewConfig.extension<rcs_network_tuning_config>().standby_probe_ms = 1000;
    TestViewer viewer(viewConfig);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(viewer.session->start() == RCS_OK && viewer.watching());
    const auto ready = [&](size_t count) {
        return waitFor([&] {
            auto events = pathEvents(viewer.events, RCS_PATH_STANDBY);
            return std::count_if(events.begin(), events.end(), [](const rcs_event& e) {
                       return e.path.result == RCS_OK && e.path.to.route == RCS_ROUTE_DIRECT;
                   }) == ptrdiff_t(count);
        });
    };
    CHECK(ready(1) && proxy.accepted() == 2);
    rcs_path_info active{}, standby{};
    CHECK(rcs_session_get_path_info(viewer.session->handle(), 0, &active, &standby) == RCS_OK);
    CHECK(standby.route == RCS_ROUTE_DIRECT && portOf(standby.local_address) != portOf(active.local_address));
    const auto stalled = [&] {
        return pathEvents(viewer.events, RCS_PATH_CHANGED, RCS_PATH_CAUSE_STALLED);
    };

    // The TCP connection in use carries nothing: the standby one takes over.
    const auto first = rcsdk::transport::Endpoint::parse("127.0.0.1", portOf(active.local_address));
    proxy.freeze(first);
    CHECK(waitFor([&] { return stalled().size() == 1; }, 5s));
    CHECK(portOf(stalled()[0].path.to.local_address) == portOf(standby.local_address));
    CHECK(viewer.watching());
    CHECK(waitFor([&] { return !pathEvents(shareEvents, RCS_PATH_CHANGED, RCS_PATH_CAUSE_PEER, 1).empty(); }));
    // It carries again: back to it, and the standby path is ready again.
    proxy.thaw(first);
    CHECK(waitFor([&] { return stalled().size() == 2; }, 5s));
    CHECK(portOf(stalled()[1].path.to.local_address) == portOf(active.local_address));
    CHECK(ready(2) && viewer.watching());
    CHECK(proxy.accepted() == 2); // No connection was made for it.

    // The standby one's TCP connection breaks: another is made for it,
    // standby_probe_ms later.
    CHECK(rcs_session_get_path_info(viewer.session->handle(), 0, &active, &standby) == RCS_OK);
    proxy.cut(rcsdk::transport::Endpoint::parse("127.0.0.1", portOf(standby.local_address)));
    CHECK(ready(3) && proxy.accepted() == 3);
    // It takes over again, and the one it took over from breaks while it
    // waits to come back: a new standby path is made instead.
    CHECK(rcs_session_get_path_info(viewer.session->handle(), 0, &active, &standby) == RCS_OK);
    proxy.freeze(first);
    CHECK(waitFor([&] { return stalled().size() == 3; }, 5s));
    CHECK(portOf(stalled()[2].path.to.local_address) == portOf(standby.local_address));
    proxy.cut(first);
    CHECK(ready(4) && proxy.accepted() == 4 && viewer.watching());
    const auto states = viewer.events.of(RCS_EVENT_STATE);
    CHECK(std::none_of(states.begin(), states.end(),
                       [](const rcs_event& e) { return e.state.value == RCS_STATE_RECONNECTING; }));
    CHECK(viewer.session->stats<rcs_network_stats>().connections == 1 &&
          share.stats<rcs_network_stats>().connections == 1);
    CHECK(viewer.session->stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

// RELAY with the viewer reaching the TURN server over TCP: the server drops
// that TCP connection (a NAT that forgot it), and the allocation with it. The
// viewer's path fails; after its grace a new one is found through the host's
// signalling, over a new TCP connection and allocation, and the same
// connection goes on there (signaled).
void migrationRelayTcp(const uint8_t (&key)[32]) {
    rcsdk::test::TurnServer server("host", "s3cret");
    SignalledPair pair(config(RCS_ROLE_SHARE, 0, key).setRelay(server.addressText(), "host", "s3cret"),
                       config(RCS_ROLE_VIEW, 0, key)
                           .setRelay(server.tcpAddress().toString(), "host", "s3cret", RCS_TURN_TCP));
    CHECK(pair.share->start() == RCS_OK);
    CHECK(waitFor([&] { return pair.share->state() == RCS_STATE_LISTENING; }));
    CHECK(pair.view->start() == RCS_OK && pair.watching(20));
    CHECK(server.tcpClients == 1 && pair.view->pathInfo().route == RCS_ROUTE_RELAY);
    server.dropTcp();
    CHECK(waitFor(
        [&] {
            auto changed = pathEvents(pair.viewEvents, RCS_PATH_CHANGED);
            return std::any_of(changed.begin(), changed.end(), [](const rcs_event& e) { return e.path.signaled != 0; });
        },
        20s));
    CHECK(!pathEvents(pair.viewEvents, RCS_PATH_CHANGING, RCS_PATH_CAUSE_NETWORK).empty());
    CHECK(pair.watching());
    CHECK(server.tcpClients >= 2 && pair.view->pathInfo().route == RCS_ROUTE_RELAY);
    CHECK(pair.view->stats<rcs_network_stats>().connections == 1 &&
          pair.share->stats<rcs_network_stats>().connections == 1 && pair.view->state() == RCS_STATE_RUNNING);
    CHECK(pair.view->stop() == RCS_OK);
    CHECK(pair.share->stop() == RCS_OK);
}

// TCP adapts its bitrate as the datagram transports do: behind a 2 Mbit/s
// link the queue in the TCP connections' buffers shows in what the viewer
// measures, the sharer's target comes down to what the link carries and the
// picture keeps coming; once the link opens, the target goes back up.
void adaptiveTcp(const uint8_t (&key)[32]) {
    auto& pattern = rcsdk::test::patternControl();
    pattern.grain = true;
    std::atomic<int> frames{0};
    rcsdk::Callbacks viewCallbacks;
    viewCallbacks.onFrame = [&](const rcs_frame&) {
        ++frames;
    };
    rcsdk::Session share(config(RCS_ROLE_SHARE, 47936, key)
                             .setTransport(RCS_TRANSPORT_TCP)
                             .setVideo(640, 360, 30, 6'000'000)
                             .setRateMode(RCS_RATE_DEFAULT, 200'000));
    rcsdk::test::TcpProxy proxy(rcsdk::transport::Endpoint::parse("127.0.0.1", 47936));
    proxy.downBps = 2'000'000;
    rcsdk::Session view(config(RCS_ROLE_VIEW, proxy.address().port(), key).setTransport(RCS_TRANSPORT_TCP),
                        viewCallbacks);
    CHECK(share.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING; }));
    CHECK(view.start() == RCS_OK);
    std::this_thread::sleep_for(9s);
    const int settled = frames;
    std::this_thread::sleep_for(3s);
    const auto sent = share.stats<rcs_video_send_stats>();
    const auto network = share.stats<rcs_network_stats>();
    std::printf("TCP behind 2 Mbit/s: target %u kbit/s, delivered %llu kbit/s, loss %u per mille, queue %u ms, "
                "%d frames in 3 s\n",
                sent.bitrate_bps / 1000, (unsigned long long)network.received_bps / 1000, network.loss_permille,
                network.queue_delay_us / 1000, frames - settled);
    CHECK(sent.bitrate_bps < 2'400'000 && sent.bitrate_bps >= 500'000);
    CHECK(frames - settled >= 45);
    CHECK(std::string(network.path).find("TCP") != std::string::npos);
    proxy.downBps = 0;
    CHECK(waitFor([&] { return share.stats<rcs_video_send_stats>().bitrate_bps > 3'500'000; }, 15s));
    pattern.grain = false;
    CHECK(view.stop() == RCS_OK);
    CHECK(share.stop() == RCS_OK);
}

struct Scenario {
    const char* name;
    void (*run)(const uint8_t (&key)[32]);
    bool streams;                       // Needs H.264 encoding and decoding with one acceleration.
    std::vector<const char*> logged{};  // Lines the host's log must have afterwards.
};

const std::vector<Scenario>& scenarios() {
    static const std::vector<Scenario> list = {
        {"refusals", refusals, false, {"session: Start refused (Not supported on this system)"}},
        {"streaming_gpu",
         [](const uint8_t (&key)[32]) { streaming(key, 47821, true); },
         true,
         {"network: Listening on UDP 127.0.0.1", "session: Negotiated H.264", "encoder: H.264", "decoder: H.264"}},
        {"streaming_cpu",
         [](const uint8_t (&key)[32]) { streaming(key, 47831, false); },
         true,
         {"network: Listening on UDP 127.0.0.1", "session: Negotiated H.264", "encoder: H.264", "decoder: H.264"}},
        {"recovery", recovery, true, {"decoder: Decode backlog"}},
        {"updates", updates, true},
        {"keyframes", keyframes, true},
        {"host_sources", hostSources, true},
        {"host_gpu_frames", hostGpuFrames, true},
        {"sample_buffers", sampleBuffers, true},
        {"hardware_buffers", hardwareBuffers, true},
        {"stream_changes", streamChanges, true},
        {"large_pictures", largePictures, true},
        {"cursor", cursorStream, true},
        {"audio_changes", audioChanges, true},
        {"udp", udpStreaming, true},
        {"keep_listening", keepListening, true},
        {"listen_after_refusals", [](const uint8_t (&key)[32]) { listenAfterRefusals(key, RCS_TRANSPORT_TCP); }, true},
        {"host_listen_after_refusals", [](const uint8_t (&key)[32]) { listenAfterRefusals(key, RCS_TRANSPORT_HOST); },
         true},
        {"reconnect", reconnect, true},
        {"p2p", p2pStreaming, true},
        {"p2p_keep_listening", p2pKeepListening, true},
        {"p2p_stale_signal", p2pStaleSignal, true},
        {"data", dataChannels, true},
        {"data_between_connections", dataBetweenConnections, true},
        {"relay", relayStreaming, true},
        {"adaptive", adaptiveBitrate, true},
        {"degradation", degradation, true},
        {"wireless", wirelessPath, true},
        {"wifi_sound", wifiSound, true},
        {"rate_decisions", rateDecisions, true},
        {"host_encoder", hostEncoder, true},
        {"host_codec", hostCodec, true}, // H.264 is the alternative it offers.
        {"picture_formats", pictureFormats, false},
        {"hardware_first", hardwareFirst, false},
        {"host_decoder", hostDecoder, true},
        {"encoded_source", encodedSource, true},
        {"encoded_packets", encodedPackets, true},
        {"record_only", recordOnly, true},
        {"host_codec_errors", hostCodecErrors, true},
        {"host_encoded", hostEncoded, false},
        {"negotiation", negotiation, true},
        {"audio", audioStreaming, true},
        {"control", control, true, {"control: Remote control granted"}},
        {"host_input", hostInput, true},
        {"host_input_types", hostInputTypes, true},
        {"caps_lock", capsLock, false},
        {"text_caret", textCaret, true},
        {"power", power, true},
        {"power_actions", powerActionsScenario, true, {"power: Viewer 1 asked to shut down: refused"}},
        {"power_wake", powerWakeScenario, true},
        {"power_actions_agent", powerActionsAgentScenario, true},
        {"modules", moduleCore, true},
        {"clipboard", clipboard, true},
        {"clipboard_viewers", clipboardViewers, true},
        {"file_transfer", fileTransfer, true},
        {"file_transfer_control", fileTransferControl, true},
        {"file_transfer_large", fileTransferLarge, true},
        {"file_transfer_reconnect", fileTransferReconnect, true},
        {"file_transfer_resume", fileTransferResume, true},
        {"file_transfer_host", fileTransferHost, true},
        {"file_transfer_agent", fileTransferAgent, true},
        {"file_transfer_pending", fileTransferPending, true},
        {"file_transfer_publish", fileTransferPublish, true},
        {"clipboard_files", clipboardFiles, true},
        {"clipboard_files_viewers", clipboardFilesViewers, true},
        {"clipboard_files_agent", clipboardFilesAgent, true},
        {"agents", agents, true},
        {"machine", machineState, true},
        {"restart", restartNotice, true},
        {"capture_interruption", captureInterruption, true, {"capture: Capture paused"}},
        {"identity", identities, true},
        {"pin", pins, true},
        {"callback_lifecycle", callbackLifecycle, true},
        {"host_datagram", hostDatagram, true, {"network: Waiting for viewers over the host datagram channel"}},
        {"host_stream", hostStream, true},
        {"host_keep_listening", hostKeepListening, true},
        {"host_reconnect", hostReconnect, true},
        {"host_backpressure", hostBackpressure, true},
        {"host_errors", hostErrors, true},
        {"host_inline", hostInline, true},
#ifdef _WIN32
        {"host_device", hostDevice, true},
#endif
        {"session_logs", sessionLogs, true},
        {"viewers_tcp", [](const uint8_t (&key)[32]) { viewers(key, RCS_TRANSPORT_TCP, 47955); }, true},
        {"viewers_udp", [](const uint8_t (&key)[32]) { viewers(key, RCS_TRANSPORT_UDP, 47956); }, true},
        {"viewers_p2p", [](const uint8_t (&key)[32]) { viewersSignalled(key, RCS_TRANSPORT_P2P); }, true},
        {"viewers_relay", [](const uint8_t (&key)[32]) { viewersSignalled(key, RCS_TRANSPORT_RELAY); }, true},
        {"viewer_control", viewerControl, true, {"control: Remote control granted to viewer 1"}},
        {"viewers_busy", viewersBusy, true, {"session: Refused a viewer (The sharer cannot take this viewer now)"}},
        {"admission", admission, true, {"session: Viewer 1 waits for the host's decision"}},
        {"parity", parity, true},
        {"lost_pictures", lostPictures, true},
        {"viewer_audio", viewerAudio, true},
        {"streams", streams, true},
        {"pausing", pausing, true},
        {"viewers_rate", viewersRate, true},
        {"viewers_join_storm", viewersJoinStorm, true},
        {"viewers_lossy", viewersLossy, true, {"session: Viewer 3 gets encoding 1, 1 temporal layer (its lost pictures"}},
        {"viewers_quorum", viewersQuorum, true, {"session: Stream 0, encoding 1: H.264"}},
        {"viewers_codecs", viewersCodecs, true},
        {"viewers_encodings", viewersEncodings, true},
        {"viewers_layers", viewersLayers, true, {"session: Viewer 3 gets encoding 0, 1 temporal layer (its path"}},
        {"migration", migration, true},
        {"path_hold", pathHold, true},
        {"path_hold_expired", pathHoldExpired, true},
        {"host_restart", hostRestart, true},
        {"host_viewers", hostViewers, true},
        {"forwarder", forwarderGroup, true},
        {"forwarder_refusals", forwarderRefusals, true},
        {"forwarder_moves", forwarderMoves, true},
        {"forwarder_entries", forwarderEntries, true},
        {"forwarder_late", forwarderLate, true},
        {"forwarder_uplink", forwarderUplink, true},
        {"forwarder_rooms", forwarderRooms, true},
        {"forwarder_token_refresh", forwarderTokenRefresh, true},
        {"forwarder_host", forwarderHost, true},
        {"forwarder_control", forwarderControl, true},
        {"forwarder_clipboard", forwarderClipboard, true},
        {"forwarder_files", forwarderFiles, true},
        {"forwarder_files_resume", forwarderFilesResume, true},
        {"forwarder_microphone", forwarderMicrophone, true},
        {"migration_p2p", migrationP2p, true},
        {"migration_relay", migrationRelay, true},
        {"network_policy", networkPolicy, true},
        {"standby", standbyPath, true},
        {"standby_p2p", standbyP2p, true},
        {"migration_tcp", migrationTcp, true},
        {"path_hold_tcp", pathHoldTcp, true},
        {"path_hold_expired_tcp", pathHoldExpiredTcp, true},
        {"tcp_no_migrate", tcpNoMigrate, true},
        {"standby_tcp", standbyTcp, true},
        {"migration_relay_tcp", migrationRelayTcp, true},
        {"adaptive_tcp", adaptiveTcp, true},
        {"path_stalls", pathStalls, true},
        {"refused_change", refusedChange, true},
        {"failures",
         failures,
         true,
         {"network: Dropped a connection that failed the handshake",
          "session: Session failed (Network connection failed)"}},
    };
    return list;
}

// What ctest takes as "skipped" (SKIP_RETURN_CODE).
constexpr int skipped = 77;

int runScenario(const Scenario& scenario) {
    const auto caps = rcsdk::capabilities();
    const bool hardware = testAcceleration() == RCS_ACCELERATION_HARDWARE;
    const uint32_t h264 = hardware ? caps.hardware_encoders & caps.hardware_decoders & h264Bit
                                   : caps.software_encoders & caps.software_decoders & h264Bit;
    if (scenario.streams && !h264) {
        std::printf("%s: no H.264 encoder and decoder with one acceleration here; skipped\n", scenario.name);
        return skipped;
    }
#ifndef RCSDK_TEST_FORWARDER
    if (std::string(scenario.name).rfind("forwarder", 0) == 0) {
        std::printf("%s: the forwarder is not built here (RCSDK_BUILD_FORWARDER); skipped\n", scenario.name);
        return skipped;
    }
#endif
    // A browser has no sockets of its own: only what runs over the host's channels.
    if (!(caps.transports & (1u << RCS_TRANSPORT_UDP)) && std::string(scenario.name) != "host_encoded") {
        std::printf("%s: no sockets here (a browser); skipped\n", scenario.name);
        return skipped;
    }
    if (std::string(scenario.name) == "streaming_gpu" && !hardware && !caps.software_decoding_gpu_frames) {
        std::printf("%s: software decoding yields no GPU frames here; skipped\n", scenario.name);
        return skipped;
    }
    if ((std::string(scenario.name) == "large_pictures" || std::string(scenario.name) == "viewers_codecs") &&
        (!hardware || !(caps.hardware_encoders & caps.hardware_decoders & hevcBit))) {
        std::printf("%s: no HEVC encoder and decoder in hardware here; skipped\n", scenario.name);
        return skipped;
    }
#ifdef __ANDROID__
    // Android's SELinux lets a program run from adb create no Unix sockets in
    // files (an application can, in its own directory): no agent link.
    for (const char* linked : {"agents", "file_transfer_agent", "power_actions_agent", "clipboard_files_agent"}) {
        if (std::string(scenario.name) == linked) {
            std::printf("%s: needs an agent link, whose socket a program run from adb cannot make here; skipped\n",
                        scenario.name);
            return skipped;
        }
    }
#endif
#ifdef _WIN32
    if (std::string(scenario.name) == "host_device" && !hardware) {
        std::printf("%s: needs hardware decoding; skipped\n", scenario.name);
        return skipped;
    }
#endif
    return runTest(scenario.name, [&] {
        rcsdk::test::UseTestPattern pattern;
        rcsdk::test::UseTestTone tone;
        rcsdk::test::UseSilentPlayer silent;
        uint8_t key[32];
        CHECK(rcs_generate_connection_key(key, 32) == RCS_OK);
        scenario.run(key);
        auto& log = sessionLog();
        CHECK(log.nested == RCS_ERROR_CALLBACK_THREAD || log.lines.empty());
        for (const char* text : scenario.logged) {
            if (!log.contains(text)) {
                std::printf("missing log line: %s\n", text);
                CHECK(false);
            }
        }
    });
}

} // namespace

// rcsdk_test_session [scenario]: one scenario, or all in order without one.
// --list prints the names; --check-list NAME... fails unless they are exactly
// the scenarios (keeps tests/CMakeLists.txt in step with the table).
int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && (args[0] == "--list" || args[0] == "--check-list")) {
        std::vector<std::string> names;
        for (const auto& scenario : scenarios()) {
            names.emplace_back(scenario.name);
        }
        if (args[0] == "--list") {
            for (const auto& name : names) {
                std::printf("%s\n", name.c_str());
            }
            return 0;
        }
        std::vector<std::string> listed(args.begin() + 1, args.end());
#ifndef _WIN32
        listed.erase(std::remove(listed.begin(), listed.end(), "host_device"), listed.end());
#endif
        std::sort(names.begin(), names.end());
        std::sort(listed.begin(), listed.end());
        if (names != listed) {
            std::fprintf(stderr, "tests/CMakeLists.txt does not list exactly the scenarios of tests/session.cpp\n");
            return 1;
        }
        return 0;
    }
    collectLog();
    CHECK(rcs_set_log_callback(
              9, [](const rcs_log_record*, void*) {}, nullptr) == RCS_ERROR_INVALID_ARGUMENT);
    int result = args.empty() ? 0 : -1;
    for (const auto& scenario : scenarios()) {
        if (!args.empty() && args[0] != scenario.name) {
            continue;
        }
        const int outcome = runScenario(scenario);
        if (!args.empty()) {
            result = outcome;
            break;
        }
        result = std::max(result, outcome == skipped ? 0 : outcome);
    }
    if (result < 0) {
        std::fprintf(stderr, "No scenario named %s (see --list)\n", args[0].c_str());
        result = 1;
    }
    rcsdk::setLogCallback(RCS_LOG_DEBUG, {});
    return result;
}
