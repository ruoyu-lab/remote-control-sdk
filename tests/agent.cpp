// The agent and its link: the link's prefix and messages, the endpoint, and a
// session's side (RemoteAgent) with agents (AgentServer, or the link spoken by
// hand) attaching to it, with the pattern, tone and input hooks standing in for
// the machine; then the same through a session's public interface.
#include "agent/agent_server.hpp"
#include "agent/link_messages.hpp"
#include "agent/media_agent.hpp"
#include "agent/remote.hpp"
#include "media/nal.hpp"
#include "support/check.hpp"
#include "support/echo_module.hpp"
#include "support/test_pattern.hpp"
#include "support/test_tone.hpp"
#include "transport/handshake.hpp"
#include "transport/local_stream.hpp"
#include "transport/secure_channel.hpp"

#include <rcsdk/agent.hpp>
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/thumbnail.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include "platform/windows/win.hpp"

#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <libproc.h>
#endif
#endif

using namespace rcsdk;
using namespace rcsdk::agent;
using namespace std::chrono_literals;

namespace {

template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 8s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// The thumbnail module, for this process's sessions and agents alike: once.
void registerThumbnails() {
    static const bool registered = rcs_register_module(rcs_thumbnail_module()) == RCS_OK;
    CHECK(registered);
}

std::string testEndpoint(const std::string& name = "") {
#ifdef _WIN32
    return "rcsdk-test-agent-" + std::to_string(GetCurrentProcessId()) + name;
#elif defined(__ANDROID__) // No /tmp: where the test runner says (TMPDIR).
    return std::getenv("TMPDIR") + std::string("/rcsdk-test-agent-") + std::to_string(getpid()) + name + ".sock";
#else
    return "/tmp/rcsdk-test-agent-" + std::to_string(getpid()) + name + ".sock";
#endif
}

uint32_t ownProcess() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return uint32_t(getpid());
#endif
}

rcs_acceleration acceleration() {
    const auto caps = localMachine().capabilities;
    return (caps.hardware_encoders & (1u << RCS_CODEC_H264)) ? RCS_ACCELERATION_HARDWARE : RCS_ACCELERATION_SOFTWARE;
}

bool canEncode() {
    const auto caps = localMachine().capabilities;
    return ((caps.hardware_encoders | caps.software_encoders) & (1u << RCS_CODEC_H264)) != 0;
}

// A session that shares the display runs: an H.264 encoder and display capture (not on iOS).
bool canShareDisplay() {
    return canEncode() && (localMachine().capabilities.capture_sources & (1u << RCS_CAPTURE_DISPLAY));
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// ---- The messages ------------------------------------------------------------------------------

EncodedPacket videoPacket() {
    EncodedPacket p;
    p.codec = RCS_CODEC_H264;
    p.width = 640;
    p.height = 360;
    p.keyframe = true;
    p.sequence = 7;
    p.timestampNs = 99;
    p.encodeNs = 1234;
    nal::appendUnit(p.config, std::vector<uint8_t>{0x67, 1, 2, 3});
    nal::appendUnit(p.config, std::vector<uint8_t>{0x68, 4});
    nal::appendUnit(p.data, std::vector<uint8_t>{0x65, 9, 9, 9, 9});
    return p;
}

template <class T>
T roundTrip(const T& message) {
    return std::get<T>(link::decode(link::encode(message)));
}

void messages() {
    // The hello carries what the machine offers; the process is not the agent's to say.
    Machine machine = localMachine();
    machine.displays = {{1, 3840, 2160, 120, 0, 1}, {2, 1080, 1920, 60, 90, 0}};
    machine.hostEncoders = (1u << RCS_CODEC_H264) | (1u << RCS_CODEC_HOST_FIRST);
    machine.hostEncoderFormats = (1u << RCS_PICTURE_420_8) | (1u << RCS_PICTURE_444_8);
    machine.capabilities.picture_formats = (1u << RCS_PICTURE_420_10) | (1u << RCS_PICTURE_HDR10_420);
    machine.implementations.push_back({RCS_CODEC_HEVC, RCS_CODEC_DECODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_MEDIA_FOUNDATION,
                                       0, 5120, 2880, (1u << RCS_PICTURE_444_10) | (1u << RCS_PICTURE_420_8),
                                       "sized"});
    machine.implementations.push_back({RCS_CODEC_HEVC, RCS_CODEC_ENCODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_NVENC,
                                       0, 8192, 8192, 1u << RCS_PICTURE_420_8, "invalidating", true});
    const auto hello = roundTrip(link::AgentHello{machine});
    CHECK(hello.machine.displays.size() == 2 && hello.machine.displays[0].refresh_hz == 120);
    CHECK(hello.machine.displays[1].rotation_degrees == 90 && hello.machine.displays[1].pixel_height == 1920);
    CHECK(hello.machine.hostEncoders == machine.hostEncoders &&
          hello.machine.hostEncoderFormats == machine.hostEncoderFormats);
    CHECK(hello.machine.implementations.size() == machine.implementations.size());
    const auto& sized = hello.machine.implementations[hello.machine.implementations.size() - 2];
    CHECK(hello.machine.implementations.back().refInvalidation && !sized.refInvalidation);
    CHECK(sized.maxWidth == 5120 && sized.maxHeight == 2880 &&
          sized.pictureFormats == machine.implementations[machine.implementations.size() - 2].pictureFormats);
    CHECK(hello.machine.capabilities.picture_formats == machine.capabilities.picture_formats);
    CHECK(hello.machine.capabilities.platform == machine.capabilities.platform &&
          hello.machine.capabilities.hardware_encoders == machine.capabilities.hardware_encoders &&
          hello.machine.capabilities.input_types == machine.capabilities.input_types);
    CHECK(hello.machine.inputPermission == machine.inputPermission);
    CHECK(machine.process.processId == ownProcess() && hello.machine.process.processId == 0 &&
          hello.machine.process.account.empty());
    CHECK(std::holds_alternative<link::Accepted>(link::decode(link::encode(link::Accepted{}))));
    CHECK(std::holds_alternative<link::Replaced>(link::decode(link::encode(link::Replaced{}))));
    const auto refused = roundTrip(link::Refused{RCS_ERROR_AUTHENTICATION, "the host refused it"});
    CHECK(refused.result == RCS_ERROR_AUTHENTICATION && refused.reason == "the host refused it");

    // The session's asks.
    VideoStart start;
    start.stream = {RCS_CODEC_HEVC,
                    {RCS_PICTURE_444_10, RCS_MATRIX_BT601, RCS_RANGE_FULL},
                    1920,
                    1080,
                    60,
                    false,
                    {RCS_CAPTURE_DISPLAY, 2},
                    true,
                    RCS_ACCELERATION_SOFTWARE,
                    RCS_BACKEND_AUTO,
                    RCS_ENCODER_BALANCED};
    start.bitrate = 8'000'000;
    start.budgeted = true;
    start.cursorInVideo = false;
    start.inputBoostMs = 40;
    const auto started = roundTrip(link::StartVideo{start}).start;
    CHECK(started.stream == start.stream && started.bitrate == 8'000'000 && started.budgeted && !started.cursorInVideo &&
          started.inputBoostMs == 40);
    CHECK(roundTrip(link::SetRate{0, 0, 3'000'000, true}).bitrate == 3'000'000);
    CHECK(roundTrip(link::SetRate{2, 1, 3'000'000, true}).index == 2);
    CHECK(roundTrip(link::SetRate{2, 1, 3'000'000, true}).encoding == 1);
    CHECK(roundTrip(link::InvalidateAfter{3, 0, 77}).index == 3 && roundTrip(link::RequestKeyframe{1, 0}).index == 1);
    CHECK(roundTrip(link::InvalidateAfter{3, 2, 77}).encoding == 2 && roundTrip(link::RequestKeyframe{1, 2}).encoding == 2);
    // A lower encoding of a stream (RCS_EXT_ENCODINGS), its start and its end;
    // its temporal layers (rcs_scalability).
    auto lower = start;
    lower.encoding = 1;
    lower.stream.framesPerSecond = 30;
    lower.stream.temporalLayers = 3;
    const auto lowerStarted = roundTrip(link::StartVideo{lower}).start;
    CHECK(lowerStarted.encoding == 1 && lowerStarted.stream.framesPerSecond == 30 &&
          lowerStarted.stream.temporalLayers == 3);
    auto layered = lower;
    layered.stream.temporalLayers = 4;
    CHECK(throws<Failure>([&] { link::decode(link::encode(link::StartVideo{layered})); }));
    const auto stopped = roundTrip(link::StopVideo{2, 1});
    CHECK(stopped.index == 2 && stopped.encoding == 1);
    const auto lowerFailed = roundTrip(link::EncodingFailed{0, 1, RCS_ERROR_UNSUPPORTED, "no session to spare"});
    CHECK(lowerFailed.encoding == 1 && lowerFailed.result == RCS_ERROR_UNSUPPORTED &&
          lowerFailed.message == "no session to spare");
    CHECK(throws<Failure>([] { link::decode(link::encode(link::EncodingFailed{0, 0, RCS_ERROR_CODEC, "Failed's"})); }));
    CHECK(throws<Failure>([] { link::decode(link::encode(link::StopVideo{0, RCS_MAX_ENCODINGS})); }));
    const auto displays = roundTrip(link::ConfigureInput{{RCS_CAPTURE_DISPLAY, 3}, {5, 6}, false}).streamDisplays;
    CHECK((displays == std::vector<uint32_t>{5, 6}));
    CHECK(!roundTrip(link::SetCursorInVideo{false}).inVideo);
    CHECK(roundTrip(link::SetInputBoost{25}).ms == 25);
    CHECK((roundTrip(link::Backlog{{123'456, 7, 0}}).ns == std::array<uint64_t, RCS_MAX_ENCODINGS>{123'456, 7, 0}));
    const auto audio = roundTrip(link::StartAudio{{RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK, true, 480}}).start;
    CHECK(audio.backend == RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK && audio.excludeCurrentProcess && audio.periodFrames == 480);
    const auto configure = roundTrip(link::ConfigureInput{{RCS_CAPTURE_DISPLAY, 3}, {}, true});
    CHECK(configure.target.displayId == 3 && configure.secureDesktop);
    // What is captured: a region of a display, a window, an application's windows.
    const platform::CaptureTarget region{RCS_CAPTURE_REGION, 2, {100, 50, 640, 480}};
    CHECK(roundTrip(link::ConfigureInput{region, {}, false}).target == region);
    const platform::CaptureTarget window{RCS_CAPTURE_WINDOW, 0, {}, 0x1234'5678'9ABCull};
    CHECK(roundTrip(link::ConfigureInput{window, {}, false}).target == window);
    start.stream.target = {RCS_CAPTURE_APPLICATION, 1, {}, 0, 4321};
    CHECK(roundTrip(link::StartVideo{start}).start.stream.target == start.stream.target);
    start.stream.target = {RCS_CAPTURE_DISPLAY, 2};
    const auto desktop = roundTrip(link::Desktop{{RCS_DESKTOP_SECURE, true}}).state;
    CHECK(desktop.kind == RCS_DESKTOP_SECURE && desktop.userLoggedOn);
    rcs_input_event wheel = RCS_INPUT_EVENT_INIT;
    wheel.type = RCS_INPUT_WHEEL;
    wheel.x = 100;
    wheel.wheel_y = -3;
    const auto input = roundTrip(link::ApplyInput{wheel}).event;
    CHECK(input.type == RCS_INPUT_WHEEL && input.x == 100 && input.wheel_y == -3 && input.struct_size == sizeof(input));
    rcs_input_event pen = RCS_INPUT_EVENT_INIT;
    pen.type = RCS_INPUT_PEN;
    pen.down = 1;
    pen.pressure = 512;
    pen.tilt_x = 30;
    pen.pen = RCS_PEN_BARREL;
    const auto penInput = roundTrip(link::ApplyInput{pen}).event;
    CHECK(penInput.pressure == 512 && penInput.tilt_x == 30 && penInput.pen == RCS_PEN_BARREL && penInput.down == 1);
    const auto moduleStart = roundTrip(link::ModuleStart{"test.echo", 3});
    CHECK(moduleStart.id == "test.echo" && moduleStart.version == 3);
    CHECK(roundTrip(link::ModuleStop{"test.echo"}).id == "test.echo");
    const auto moduleData = roundTrip(link::ModuleData{"com.example.x", 1, 2, {9, 8}});
    CHECK(moduleData.id == "com.example.x" && moduleData.index == 1 && moduleData.count == 2 &&
          moduleData.bytes == crypto::Bytes({9, 8}));
    const auto moduleStatus = roundTrip(link::ModuleStatus{"test.echo", RCS_ERROR_UNSUPPORTED, "not registered"});
    CHECK(moduleStatus.id == "test.echo" && moduleStatus.result == RCS_ERROR_UNSUPPORTED &&
          moduleStatus.message == "not registered");
    // A module's message in parts, each module's on its own.
    link::ModuleAssembly assembly;
    CHECK(!assembly.add({"a", 0, 2, {1}}, 16) && !assembly.add({"b", 0, 2, {7}}, 16));
    CHECK(assembly.add({"a", 1, 2, {2}}, 16) == crypto::Bytes({1, 2}));
    CHECK(assembly.add({"b", 1, 2, {8}}, 16) == crypto::Bytes({7, 8}));
    CHECK(throws<Failure>([&] { assembly.add({"a", 1, 2, {1}}, 16); }));       // Out of order.
    CHECK(throws<Failure>([&] { assembly.add({"a", 0, 1, crypto::Bytes(17)}, 16); })); // Larger than it takes.
    crypto::Bytes whole(9u << 20, 'w');
    std::vector<link::ModuleData> parts;
    link::sendModuleData([&](const link::Message& part) { parts.push_back(std::get<link::ModuleData>(part)); }, "a",
                         whole);
    CHECK(parts.size() == 3);
    std::optional<crypto::Bytes> assembled;
    for (auto& part : parts) {
        assembled = assembly.add(std::move(part), 16u << 20);
    }
    CHECK(assembled == whole);

    // What the agent reports.
    VideoStarted reply;
    reply.stream = start.stream;
    reply.content = {0, 60, 1920, 960};
    reply.display = {3840, 2160, 120};
    reply.lowLatency = true;
    reply.refInvalidation = true;
    reply.encoder = "Test encoder";
    reply.hdr = {34000, 16000, 13250, 34500, 7500, 3000, 15635, 16450, 10'000'000, 50, 1000, 400};
    reply.encoding = 2;
    reply.stream.temporalLayers = 3;
    reply.temporalLayers = 2; // Of the 3 asked for.
    const auto reported = roundTrip(link::Started{reply}).started;
    CHECK(reported.stream == reply.stream && reported.content == reply.content && reported.display.refreshHz == 120);
    CHECK(reported.encoding == 2 && reported.temporalLayers == 2);
    CHECK(reported.lowLatency && reported.refInvalidation && reported.encoder == "Test encoder" &&
          sameHdr(reported.hdr, reply.hdr));
    CHECK(roundTrip(link::InvalidateAfter{0, 0, 77}).sequence == 77);
    auto recovering = videoPacket();
    recovering.keyframe = false;
    recovering.recoversAfter = 5;
    CHECK(roundTrip(link::Packet{recovering}).packet.recoversAfter == 5u);
    auto pictured = videoPacket();
    pictured.picture = {RCS_PICTURE_444_8, RCS_MATRIX_BT601, RCS_RANGE_FULL};
    pictured.encoding = 1;
    pictured.temporalId = 2;
    pictured.discardable = true;
    const auto packet = roundTrip(link::Packet{pictured}).packet;
    CHECK(packet.keyframe && packet.sequence == 7 && packet.encodeNs == 1234 && packet.data == videoPacket().data &&
          packet.config == videoPacket().config && packet.picture == pictured.picture);
    CHECK(packet.encoding == 1 && packet.temporalId == 2 && packet.discardable);
    const auto content = roundTrip(link::Content{0, {10, 0, 600, 360}, {1280, 720, 60}});
    CHECK(content.content.x == 10 && content.display.height == 720);
    CHECK(link::fits({10, 0, 600, 360}, 640, 360) && !link::fits({10, 0, 640, 360}, 640, 360));
    const auto mode = roundTrip(link::Mode{0, 1, RCS_CODEC_H264, true, true, 2, "VT"});
    CHECK(mode.lowLatency && mode.refInvalidation && mode.temporalLayers == 2 && mode.name == "VT" &&
          mode.encoding == 1);
    const auto capture = roundTrip(link::Capture{0, {RCS_CAPTURE_PAUSE_HIDDEN, 100, 5, 2, {1200, 300, 0}}}).status;
    CHECK(capture.paused == RCS_CAPTURE_PAUSE_HIDDEN && capture.captured == 100 && capture.overwritten == 5 &&
          capture.interruptions == 2 && capture.limitedMs[0] == 1200 && capture.limitedMs[1] == 300);
    rcs_window_info listed{};
    listed.window_id = 0x5'0000'0001ull;
    listed.process_id = 77;
    listed.display_id = 2;
    listed.x = -8;
    listed.y = 40;
    listed.width = 1280;
    listed.height = 720;
    listed.minimized = 1;
    std::snprintf(listed.title, sizeof(listed.title), "%s", "Notes \xE2\x80\x94 draft");
    std::snprintf(listed.application, sizeof(listed.application), "%s", "Notes");
    CHECK(roundTrip(link::ListWindows{9}).serial == 9);
    const auto windows = roundTrip(link::WindowList{9, {listed}});
    CHECK(windows.serial == 9 && windows.windows.size() == 1);
    const auto& back = windows.windows[0];
    CHECK(back.window_id == listed.window_id && back.process_id == 77 && back.display_id == 2 && back.x == -8 &&
          back.y == 40 && back.width == 1280 && back.height == 720 && back.minimized && !back.focused &&
          std::string(back.title) == listed.title && std::string(back.application) == "Notes");
    CHECK(roundTrip(link::Availability{true}).available);
    const auto results = roundTrip(link::InputResults{RCS_ERROR_PERMISSION_REQUIRED, 12});
    CHECK(results.last == RCS_ERROR_PERMISSION_REQUIRED && results.applied == 12);
    CHECK(roundTrip(link::DisplayList{machine.displays}).list.size() == 2);

    // The pointer: the image comes once; its position alone later.
    CursorState pointer;
    pointer.visible = true;
    pointer.x = 40000;
    pointer.y = 123;
    pointer.imageSerial = 5;
    auto image = std::make_shared<CursorImage>();
    image->type = RCS_CURSOR_HAND;
    image->width = 8;
    image->height = 4;
    image->hotspotX = 3;
    image->pixels.assign(8 * 4 * 4, 0);
    image->pixels[0] = 200;
    pointer.image = image;
    const auto moved = roundTrip(link::Pointer{0, pointer}).state;
    CHECK(moved.visible && moved.x == 40000 && moved.imageSerial == 5 && moved.image && *moved.image == *image);
    pointer.image = nullptr;
    const auto alone = roundTrip(link::Pointer{0, pointer}).state;
    CHECK(alone.x == 40000 && !alone.image);

    // Sound: the samples as they are.
    link::Pcm pcm;
    pcm.timestampNs = 777;
    pcm.periodFrames = 480;
    for (int i = 0; i < 960; ++i) {
        pcm.samples.push_back(std::sin(float(i)));
    }
    const auto sound = roundTrip(pcm);
    CHECK(sound.timestampNs == 777 && sound.periodFrames == 480 && sound.samples == pcm.samples);
    const auto state = roundTrip(link::AudioState{{false, RCS_ERROR_DEVICE, "No output device"}}).status;
    CHECK(!state.active && state.result == RCS_ERROR_DEVICE && state.message == "No output device");
    const auto failed = roundTrip(link::Failed{RCS_ERROR_CAPTURE, "No display"});
    CHECK(failed.result == RCS_ERROR_CAPTURE && failed.message == "No display");
    CHECK(std::holds_alternative<link::Heartbeat>(link::decode(link::encode(link::Heartbeat{}))));

    // Malformed or invalid messages never get through.
    const auto bad = [](const crypto::Bytes& bytes) { return throws<Failure>([&] { link::decode(bytes); }); };
    CHECK(bad(link::encode(link::StopVideo{4}))); // Beyond RCS_MAX_STREAMS.
    CHECK(bad({}));
    CHECK(bad({200}));
    CHECK(bad(link::encode(link::ModuleStart{"not an id", 1})));
    CHECK(bad(link::encode(link::ModuleStart{"", 1})));
    CHECK(bad(link::encode(link::ModuleData{"test.echo", 2, 2, {1}}))); // Past its count.
    CHECK(bad(link::encode(link::ModuleData{"test.echo", 0, 1, {}})));  // Empty.
    CHECK(bad(link::encode(link::ModuleData{"test.echo", 0, 1000, {1}}))); // More than any module's message.
    CHECK(bad(link::encode(link::ModuleStatus{"test.echo", -100, ""})));    // A result no version defines.
    auto slow = start;
    slow.stream.speed = 4; // No such speed.
    CHECK(bad(link::encode(link::StartVideo{slow})));
    auto truncated = link::encode(link::StartVideo{start});
    truncated.pop_back();
    CHECK(bad(truncated));
    auto padded = link::encode(link::SetRate{0, 0, 3'000'000, false});
    padded.push_back(0);
    CHECK(bad(padded));
    VideoStart host = start;
    host.stream.hostFrames = true; // The host's own pictures are not the link's: the field does not travel.
    CHECK(!roundTrip(link::StartVideo{host}).start.stream.hostFrames);
    VideoStart wrong = start;
    wrong.stream.width = 17;
    CHECK(bad(link::encode(link::StartVideo{wrong})));
    wrong = start;
    wrong.stream.framesPerSecond = 241;
    CHECK(bad(link::encode(link::StartVideo{wrong})));
    wrong = start;
    wrong.bitrate = 10;
    CHECK(bad(link::encode(link::StartVideo{wrong})));
    CHECK(bad(link::encode(link::SetRate{0, 0, 50'000, false})));
    CHECK(bad(link::encode(link::SetInputBoost{5000})));
    CHECK(bad(link::encode(link::StartAudio{{99, false, 0}})));
    rcs_input_event key = RCS_INPUT_EVENT_INIT;
    key.type = RCS_INPUT_KEY;
    key.code = 2; // Not a keyboard usage.
    CHECK(bad(link::encode(link::ApplyInput{key})));
    CHECK(bad(link::encode(link::Content{0, {0, 0, 0, 0}, {}})));
    VideoStarted outside = reply;
    outside.content = {0, 60, 1920, 1080}; // Below the 1080 rows of the picture.
    CHECK(bad(link::encode(link::Started{outside})));
    VideoStarted moreLayers = reply;
    moreLayers.temporalLayers = 3;
    moreLayers.stream.temporalLayers = 2; // More than asked for.
    CHECK(bad(link::encode(link::Started{moreLayers})));
    CHECK(bad(link::encode(link::DisplayList{{{0, 1, 1, 60, 1}}})));
    CHECK(bad(link::encode(link::Pcm{1, 480, {0.0f, 1.0f, 2.0f}}))); // An odd number of samples.
    CHECK(bad(link::encode(link::Pcm{1, 480, {}})));
    link::Packet notVideo;
    notVideo.packet = videoPacket();
    notVideo.packet.data = {1, 2, 3}; // Not NAL units.
    CHECK(bad(link::encode(notVideo)));
    CursorState huge;
    auto tooBig = std::make_shared<CursorImage>();
    tooBig->width = tooBig->height = 300;
    tooBig->pixels.assign(300 * 300 * 4, 1);
    huge.image = tooBig;
    CHECK(bad(link::encode(link::Pointer{0, huge})));
    CHECK(bad(link::encode(link::Desktop{{7, false}}))); // A desktop no version defines.
    // Targets: the machine captures a display, a region, a window or an
    // application, each with what it needs; never the host's own pictures.
    CHECK(bad(link::encode(link::ConfigureInput{{RCS_CAPTURE_HOST}, {}, false})));
    CHECK(bad(link::encode(link::ConfigureInput{{RCS_CAPTURE_WINDOW}, {}, false})));      // No window.
    CHECK(bad(link::encode(link::ConfigureInput{{RCS_CAPTURE_APPLICATION}, {}, false}))); // No process.
    CHECK(bad(link::encode(link::ConfigureInput{{RCS_CAPTURE_REGION, 0, {0, 0, 8, 600}}, {}, false})));
    // Only the session says that no agent is there.
    CHECK(bad(link::encode(link::Capture{0, {RCS_CAPTURE_PAUSE_AGENT, 0, 0, 0}})));
    rcs_window_info nameless{}; // A window without an id.
    CHECK(bad(link::encode(link::WindowList{1, {nameless}})));
    auto twoFlags = link::encode(link::ConfigureInput{{RCS_CAPTURE_DISPLAY, 1}, {}, true});
    twoFlags.back() = 2; // A flag is 0 or 1.
    CHECK(bad(twoFlags));
    // Results: only those a version defines, and a failure is never success.
    CHECK(bad(link::encode(link::Failed{RCS_OK, "nothing"})));
    CHECK(bad(link::encode(link::Failed{-99, "an error no version defines"})));
    CHECK(bad(link::encode(link::Failed{5, "positive"})));
    CHECK(bad(link::encode(link::Refused{RCS_OK, "refused, but not"})));
    CHECK(bad(link::encode(link::InputResults{-99, 1})));
    CHECK(bad(link::encode(link::AudioState{{true, RCS_ERROR_DEVICE, "active, but failed"}})));
    CHECK(bad(link::encode(link::AudioState{{false, -77, "?"}})));
    CHECK(roundTrip(link::Failed{RCS_ERROR_REPLACED, "the last error"}).result == RCS_ERROR_REPLACED);
    VideoStart secure = start;
    secure.stream.secureDesktop = true;
    CHECK(roundTrip(link::StartVideo{secure}).start.stream.secureDesktop);
    Machine odd = machine;
    odd.screenPermission = 7;
    CHECK(bad(link::encode(link::AgentHello{odd})));
    odd = machine;
    odd.hostEncoders = 1u << 9; // No codec of any version.
    CHECK(bad(link::encode(link::AgentHello{odd})));
    odd = machine;
    odd.capabilities.secure_desktop = 2; // A flag is 0 or 1.
    CHECK(bad(link::encode(link::AgentHello{odd})));
    odd = machine;
    odd.displays[1].rotation_degrees = 45; // Displays turn by quarters.
    CHECK(bad(link::encode(link::AgentHello{odd})));
    odd = machine;
    odd.implementations.back().codec = RCS_CODEC_H264; // 5120 wide: more than H.264 takes.
    CHECK(bad(link::encode(link::AgentHello{odd})));
    odd.implementations.back().maxWidth = 8; // Less than any picture.
    odd.implementations.back().maxHeight = 2880;
    CHECK(bad(link::encode(link::AgentHello{odd})));
    // Streams beyond 4096x2160: what their codec takes.
    VideoStart large = start;
    large.stream.width = 8192;
    large.stream.height = 4352;
    CHECK(roundTrip(link::StartVideo{large}).start.stream.width == 8192);
    large.stream.codec = RCS_CODEC_H264;
    CHECK(bad(link::encode(link::StartVideo{large})));
    large.stream.width = 2304;
    large.stream.height = 4096;
    CHECK(roundTrip(link::StartVideo{large}).start.stream.height == 4096);
}

// ---- The prefix and the link's key -----------------------------------------------------------

void prefixes() {
    const auto own = link::prefix();
    CHECK(own.size() == link::prefixSize && std::equal(own.begin(), own.begin() + 7, "RCSLINK"));
    link::checkPrefix(own, "agent"); // The same version: nothing to say.
    // Another version, whatever else it changed: told apart, with both versions.
    try {
        link::checkPrefix(link::prefix(RCSDK_VERSION + 1), "agent");
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_UNSUPPORTED && contains(failure.what(), link::versionText(RCSDK_VERSION)) &&
              contains(failure.what(), link::versionText(RCSDK_VERSION + 1)) && contains(failure.what(), "agent"));
    }
    auto format = own;
    format[7] = 99; // A later link format.
    try {
        link::checkPrefix(format, "session");
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_UNSUPPORTED && contains(failure.what(), "format 99"));
    }
    // What is not a link at all: a network client's hello, garbage.
    const crypto::Bytes network(std::begin(transport::protocolMagic), std::end(transport::protocolMagic));
    crypto::Bytes networkPrefix = network;
    networkPrefix.resize(link::prefixSize);
    for (const auto& bytes : {networkPrefix, crypto::Bytes(link::prefixSize, 0), crypto::Bytes{'R', 'C', 'S'}}) {
        try {
            link::checkPrefix(bytes, "agent");
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_NETWORK);
        }
    }
    // The channel's key is bound to the link: a network connection holding the same key does not open it.
    crypto::Key key;
    crypto::random(key);
    CHECK(link::linkCredentials(key).key != key && link::linkCredentials(key).key == link::linkCredentials(key).key);
}

// ---- Wait limits -------------------------------------------------------------------------------

void timeouts() {
    CHECK(!platform::waitLimit(UINT32_MAX));
    CHECK(platform::waitLimit(0) == 0ms && platform::waitLimit(UINT32_MAX - 1) == std::chrono::milliseconds(UINT32_MAX - 1));
    CHECK(platform::windowsTimeout(std::nullopt) == 0xFFFFFFFFu); // INFINITE.
    CHECK(platform::windowsTimeout(1500ms) == 1500);
    CHECK(platform::windowsTimeout(std::chrono::milliseconds(UINT32_MAX - 1)) == 0xFFFFFFFEu);
    // A limit beyond what Windows can count waits as long as it can, not a remainder of it.
    CHECK(platform::windowsTimeout(std::chrono::hours(24 * 365)) == 0xFFFFFFFEu);
    CHECK(platform::windowsTimeout(-5ms) == 0);
    // An agent's wait without a limit returns once it ended.
    AgentServer lonely({testEndpoint("-lonely"), crypto::Key{1}, transport::LocalAccess::privileged, false, 100ms}, {});
    CHECK(lonely.start());
    CHECK(lonely.wait(std::nullopt) && lonely.state() == RCS_STATE_FAILED && lonely.failure().first == RCS_ERROR_NETWORK);
}

// ---- The endpoint ------------------------------------------------------------------------------

void endpoints() {
    const std::string endpoint = testEndpoint("-endpoint");
    CHECK(transport::endpointProblem(endpoint).empty());
    CHECK(!transport::endpointProblem("").empty());
#ifdef _WIN32
    CHECK(!transport::endpointProblem("a\\b").empty());
#else
    CHECK(!transport::endpointProblem("relative.sock").empty());
    CHECK(!transport::endpointProblem("/" + std::string(200, 'x')).empty());
#endif
    std::atomic<bool> never{false};
    CHECK(throws<Failure>([&] { transport::connectLocal(endpoint, never, 200ms); })); // Nothing listens.
    const auto& self = transport::thisProcess();
    CHECK(self.processId == ownProcess() && !self.account.empty() && !self.user.empty());
#ifndef _WIN32
    CHECK(self.privileged == (geteuid() == 0) && self.user == std::to_string(geteuid()));
#endif
    {
        transport::LocalListener listener(endpoint, transport::LocalAccess::privileged);
#ifndef _WIN32
        struct stat info {};
        CHECK(::stat(endpoint.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600); // Its own user only.
#endif
        // A stream both ways, in pieces, with both ends reading while writing; each end
        // knows the other's process as the system reports it.
        transport::LocalConnection server;
        std::thread accepter([&] { server = listener.accept(never, never); });
        auto client = transport::connectLocal(endpoint, never, 2s);
        accepter.join();
        CHECK(server.stream && client.stream);
        CHECK(server.peer.processId == ownProcess() && server.peer.user == self.user && server.peer.account == self.account);
        CHECK(client.peer.processId == ownProcess() && client.peer.user == self.user);
        std::vector<uint8_t> big(3'000'000);
        for (size_t i = 0; i < big.size(); ++i) {
            big[i] = uint8_t(i * 31);
        }
        std::vector<uint8_t> echoed(big.size());
        std::thread echo([&] {
            std::vector<uint8_t> received(big.size());
            try {
                server.stream->read(received, 5s);
                server.stream->write(received, 5s);
            } catch (const Failure& failure) {
                std::fprintf(stderr, "echo: %s\n", failure.what());
            }
        });
        std::thread sender([&] {
            try {
                client.stream->write(big, 5s);
            } catch (const Failure& failure) {
                std::fprintf(stderr, "sender: %s\n", failure.what());
            }
        });
        client.stream->read(echoed, 5s);
        sender.join();
        echo.join();
        CHECK(echoed == big);
        CHECK(client.stream->bytesTransferred() >= big.size() * 2);
        // A closed peer ends a read with DISCONNECTED; a quiet one times out.
        std::vector<uint8_t> byte(1);
        CHECK(throws<Failure>([&] { server.stream->read(byte, 100ms); }));
        server.stream.reset();
        try {
            client.stream->read(byte, 2s);
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_DISCONNECTED);
        }
        // Cancelling ends a blocked accept.
        std::atomic<bool> cancel{false};
        std::thread waiting([&] {
            try {
                listener.accept(cancel, cancel);
                CHECK(false);
            } catch (const Failure& failure) {
                CHECK(failure.result() == RCS_ERROR_CANCELLED);
            }
        });
        std::this_thread::sleep_for(100ms);
        cancel = true;
        waiting.join();
        // One process holds an endpoint.
        CHECK(throws<Failure>([&] { transport::LocalListener second(endpoint, transport::LocalAccess::privileged); }));
#ifndef _WIN32
        // Even when its queue is full (nobody accepts): the socket is not taken for one left behind,
        // and a client that comes then waits its turn instead of failing at once.
        std::vector<int> waitingClients;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
        for (int i = 0; i < 256; ++i) {
            const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
            ::fcntl(socket, F_SETFL, O_NONBLOCK);
            if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), SUN_LEN(&address)) != 0) {
                ::close(socket);
                break;
            }
            waitingClients.push_back(socket);
        }
        CHECK(throws<Failure>([&] { transport::LocalListener second(endpoint, transport::LocalAccess::anyUser); }));
        CHECK(::stat(endpoint.c_str(), &info) == 0);
        std::thread draining([&] {
            std::this_thread::sleep_for(300ms);
            // Those that waited, not the client after them: accepted and dropped, it could not tell who served it.
            for (size_t i = 0; i < waitingClients.size(); ++i) {
                listener.accept(never, never);
            }
        });
        CHECK(transport::connectLocal(endpoint, never, 3s).stream != nullptr);
        draining.join();
        for (const int socket : waitingClients) {
            ::close(socket);
        }
#endif
    }
    // The endpoint is gone with its listener.
    CHECK(throws<Failure>([&] { transport::connectLocal(endpoint, never, 200ms); }));
#ifdef _WIN32
    // Another user may open an ANY_USER endpoint and not a PRIVILEGED one. The
    // other user is this thread impersonating a token in which this user and
    // Administrators are deny-only, so of the pipe's access list only
    // "Authenticated Users" lets it in.
    {
        HANDLE own = nullptr;
        CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &own));
        const platform::win::Handle ownToken(own);
        std::vector<uint8_t> user(256);
        DWORD size = 0;
        CHECK(GetTokenInformation(own, TokenUser, user.data(), DWORD(user.size()), &size));
        BYTE administrators[SECURITY_MAX_SID_SIZE];
        DWORD administratorsSize = sizeof(administrators);
        CHECK(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &administratorsSize));
        SID_AND_ATTRIBUTES denyOnly[] = {{reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, 0},
                                         {administrators, 0}};
        HANDLE restricted = nullptr;
        CHECK(CreateRestrictedToken(own, 0, 2, denyOnly, 0, nullptr, 0, nullptr, &restricted));
        const platform::win::Handle other(restricted);
        const auto otherUserOpens = [&](transport::LocalAccess access) {
            transport::LocalListener listener(endpoint, access);
            CHECK(ImpersonateLoggedOnUser(other.get()));
            rcs_result result = RCS_OK;
            try {
                transport::connectLocal(endpoint, never, 500ms);
            } catch (const Failure& failure) {
                result = failure.result();
            }
            CHECK(RevertToSelf());
            return result;
        };
        CHECK(otherUserOpens(transport::LocalAccess::anyUser) == RCS_OK);
        CHECK(otherUserOpens(transport::LocalAccess::privileged) == RCS_ERROR_PERMISSION_REQUIRED);
    }
#endif
#ifndef _WIN32
    // What a process that died left behind is taken over; anyone may open it with ANY_USER.
    {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
        const int stale = ::socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(::bind(stale, reinterpret_cast<const sockaddr*>(&address), SUN_LEN(&address)) == 0);
        ::close(stale);
        transport::LocalListener taken(endpoint, transport::LocalAccess::anyUser);
        struct stat info {};
        CHECK(::stat(endpoint.c_str(), &info) == 0 && (info.st_mode & 0777) == 0666);
    }
    struct stat gone {};
    CHECK(::stat((endpoint + ".lock").c_str(), &gone) != 0); // Its lock goes with it.
#endif
}

#ifndef _WIN32
// The reason a listener on `endpoint` fails with, "" when it does not.
std::string listenFailure(const std::string& endpoint) {
    try {
        transport::LocalListener listener(endpoint, transport::LocalAccess::anyUser);
        return {};
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_NETWORK);
        return failure.what();
    }
}

bool says(const std::string& text, const std::string& words) {
    if (text.find(words) == std::string::npos) {
        std::fprintf(stderr, "unexpected: \"%s\"\n", text.c_str());
        return false;
    }
    return true;
}

// Where an endpoint may be made (SA-05, SA-10): in a directory no other
// account may change, never through a link or into a lock with another name,
// and a name another process holds fails naming whose it is.
void endpointPlaces() {
    const char* base = std::getenv("TMPDIR");
    std::string root = std::string(base ? base : "/tmp") + "/rcsdk-places-XXXXXX";
    CHECK(::mkdtemp(root.data()));
    const auto directory = [&](const char* name, mode_t mode) {
        const std::string path = root + "/" + name;
        CHECK(::mkdir(path.c_str(), 0700) == 0 && ::chmod(path.c_str(), mode) == 0);
        return path;
    };
    // Writable by others or the group, not sticky: refused (others could swap the socket or the lock).
    const std::string open = directory("open", 0777), group = directory("group", 0770);
    CHECK(says(listenFailure(open + "/e.sock"), "other accounts may change its entries"));
#ifdef __ANDROID__
    CHECK(listenFailure(group + "/e.sock").empty()); // The application's own group.
#else
    CHECK(says(listenFailure(group + "/e.sock"), "other accounts may change its entries"));
#endif
    // Sticky, as /tmp: nobody renames or removes another's entries there.
    const std::string sticky = directory("sticky", 01777), own = directory("own", 0700);
    CHECK(listenFailure(sticky + "/e.sock").empty());
    // A link where the socket goes: refused, and what it points at stays as it was.
    const std::string target = own + "/target";
    CHECK(::close(::open(target.c_str(), O_CREAT | O_WRONLY, 0600)) == 0);
    CHECK(::symlink(target.c_str(), (own + "/linked.sock").c_str()) == 0);
    CHECK(says(listenFailure(own + "/linked.sock"), "it exists and is not a socket"));
    // A lock that is another name of some file (a hard link), or a link: refused, untouched.
    CHECK(::link(target.c_str(), (own + "/hard.sock.lock").c_str()) == 0);
    CHECK(says(listenFailure(own + "/hard.sock"), "is not a file of its own"));
    CHECK(::symlink(target.c_str(), (own + "/soft.sock.lock").c_str()) == 0);
    CHECK(!listenFailure(own + "/soft.sock").empty());
    struct stat info {};
    CHECK(::stat(target.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600 && info.st_nlink == 2);
    // A name that is taken: the reason names the account that listens there.
    {
        const std::string endpoint = own + "/taken.sock";
        transport::LocalListener holder(endpoint, transport::LocalAccess::privileged);
        CHECK(says(listenFailure(endpoint), "a process of " + transport::thisProcess().account + " listens there"));
        CHECK(::stat(endpoint.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    }
    for (const char* name : {"target", "linked.sock", "hard.sock.lock", "soft.sock.lock"}) {
        ::unlink((own + "/" + name).c_str());
    }
    for (const char* name : {"open", "group", "sticky", "own"}) {
        CHECK(::rmdir((root + "/" + name).c_str()) == 0); // Each listener took what it made with it.
    }
    ::rmdir(root.c_str());
}
#endif

#if defined(__APPLE__) && TARGET_OS_OSX
// The process at the other end and its program come from its audit token
// (SA-07), which names the process that used the connection last, as it runs
// now: a client that hands its connection to another process, or runs
// another program, after it was accepted fails confirm(); its process ID is
// never looked up again.
void peerPrograms() {
    char own[PROC_PIDPATHINFO_MAXSIZE]{};
    CHECK(proc_pidpath(getpid(), own, sizeof(own)) > 0);
    CHECK(transport::thisProcess().program == own);
    const std::string endpoint = testEndpoint("-programs");
    transport::LocalListener listener(endpoint, transport::LocalAccess::privileged);
    std::atomic<bool> never{false};
    {
        transport::LocalConnection server;
        std::thread accepter([&] { server = listener.accept(never, never); });
        const auto client = transport::connectLocal(endpoint, never, 2s);
        accepter.join();
        CHECK(server.peer.program == own && server.peer.processId == ownProcess());
        CHECK(client.peer.program.empty()); // A listener the client reached: no audit token before it accepts.
        std::vector<uint8_t> byte{1};
        client.stream->write(byte, 1s);
        server.stream->read(byte, 1s);
        server.confirm(); // The same process, still running this program.
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
    char sleep[] = "/bin/sleep", seconds[] = "5";
    char* const arguments[] = {sleep, seconds, nullptr};
    enum Case { handsOver, execsAfter, execsBefore };
    for (const Case what : {handsOver, execsAfter, execsBefore}) {
        // A child connects; the test says when it goes on (one byte on `go`).
        int go[2];
        CHECK(::pipe(go) == 0);
        const pid_t child = ::fork();
        if (child == 0) {
            ::setpgid(0, 0); // Its own group, which the test ends whole.
            const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
            char c = 0;
            if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), SUN_LEN(&address)) != 0) {
                ::_exit(1);
            }
            if (what != execsBefore && ::read(go[0], &c, 1) != 1) {
                ::_exit(1);
            }
            if (what == handsOver) {
                if (::fork() == 0) { // Another process, the connection inherited, speaks on it.
                    ::write(socket, "x", 1);
                    ::sleep(5);
                }
                ::sleep(5);
                ::_exit(0);
            }
            ::execv(sleep, arguments); // The connection stays open in /bin/sleep.
            ::_exit(1);
        }
        CHECK(child > 0);
        const auto runsSleep = [&] {
            char path[PROC_PIDPATHINFO_MAXSIZE]{};
            return proc_pidpath(child, path, sizeof(path)) > 0 && std::string(path) == sleep;
        };
        if (what == execsBefore) {
            CHECK(waitFor(runsSleep));
        }
        auto server = listener.accept(never, never);
        CHECK(server.peer.processId == uint32_t(child));
        CHECK(server.peer.program == (what == execsBefore ? sleep : own)); // What the process runs as it is accepted.
        CHECK(::write(go[1], "g", 1) == 1);
        std::string reason;
        if (what == handsOver) {
            std::vector<uint8_t> byte(1);
            server.stream->read(byte, 2s);
        } else if (what == execsAfter) {
            CHECK(waitFor(runsSleep));
        }
        try {
            server.confirm();
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_AUTHENTICATION);
            reason = failure.what();
        }
        CHECK(what == execsBefore ? reason.empty()
              : what == handsOver ? says(reason, "used the connection")
                                  : says(reason, "it runs another program"));
        ::kill(-child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        ::close(go[0]);
        ::close(go[1]);
    }
}
#endif

// ---- The link spoken by hand -------------------------------------------------------------------

// An agent the test speaks for: the prefix and the secure channel, then
// whatever messages it wants, in the order it wants.
struct HandAgent {
    std::atomic<bool> cancel{false};
    transport::LocalConnection connection;
    std::unique_ptr<transport::SecureChannel> channel;

    HandAgent(const std::string& endpoint, const crypto::Key& key) {
        connection = transport::connectLocal(endpoint, cancel, 2s);
        connection.stream->write(link::prefix(), 2s);
        crypto::Bytes theirs(link::prefixSize);
        connection.stream->read(theirs, 5s);
        link::checkPrefix(theirs, "session");
        channel = std::make_unique<transport::SecureChannel>(
            std::move(connection.stream), transport::SecureChannel::Side::client, link::linkCredentials(key));
    }
    void send(const link::Message& message) { channel->send(link::encode(message)); }
    link::Message receive() { return link::decode(channel->receive()); }
    // Says hello and waits for the answer.
    link::Message attach() {
        send(link::AgentHello{localMachine()});
        return receive();
    }
};

// ---- A session's side with agents ----------------------------------------------------------------

struct Recorder final : Events, RemoteAgent::Observer {
    std::mutex mutex;
    std::vector<VideoStarted> started;
    std::vector<bool> keyframes;
    std::vector<size_t> packetSizes;
    std::vector<CaptureStatus> captures;
    std::vector<CursorState> cursors;
    std::vector<bool> availability;
    std::vector<platform::DesktopState> desktops;
    std::vector<std::tuple<std::string, rcs_result, std::string>> moduleStatuses;
    std::vector<std::pair<std::string, crypto::Bytes>> moduleMessages;
    uint64_t inputApplied = 0;
    rcs_result lastInput = RCS_OK;
    size_t pcmChunks = 0;
    uint32_t pcmPeriod = 0;
    double pcmEnergy = 0;
    size_t pcmSamples = 0;
    std::vector<platform::AudioStatus> audioStates;
    std::vector<std::pair<rcs_result, std::string>> failures;
    int attachedCount = 0, detachedCount = 0;
    Machine attachedMachine;
    std::vector<Machine> asked;                                   // What asked to attach.
    std::vector<std::pair<rcs_result, std::string>> departures; // Why agents left.
    std::thread::id departureThread;
    std::atomic<bool> refuse{false};
    std::function<void()> onDetached;

    template <class Read>
    auto read(Read&& action) {
        std::lock_guard lock(mutex);
        return action(*this);
    }

    void videoStarted(const VideoStarted& s) override {
        std::lock_guard lock(mutex);
        started.push_back(s);
    }
    void videoPacket(EncodedPacket&& packet) override {
        std::lock_guard lock(mutex);
        keyframes.push_back(packet.keyframe);
        packetSizes.push_back(packet.data.size());
    }
    void videoContent(uint32_t, const Rect&, const platform::DisplayMode&) override {}
    void encoderMode(uint32_t, uint32_t, rcs_codec, bool, bool, uint32_t, const std::string&) override {}
    void encodingFailed(uint32_t, uint32_t, rcs_result, const std::string&) override {}
    void captureStatus(uint32_t, const CaptureStatus& s) override {
        std::lock_guard lock(mutex);
        captures.push_back(s);
    }
    void cursor(uint32_t, const CursorState& state) override {
        std::lock_guard lock(mutex);
        cursors.push_back(state);
    }
    void displays(const std::vector<rcs_display_info>&) override {}
    void desktopState(const platform::DesktopState& state) override {
        std::lock_guard lock(mutex);
        desktops.push_back(state);
    }
    void inputAvailability(bool available) override {
        std::lock_guard lock(mutex);
        availability.push_back(available);
    }
    void inputResult(rcs_result last, uint64_t applied) override {
        std::lock_guard lock(mutex);
        lastInput = last;
        inputApplied += applied;
    }
    void moduleStatus(const std::string& id, rcs_result result, const std::string& message) override {
        std::lock_guard lock(mutex);
        moduleStatuses.emplace_back(id, result, message);
    }
    void moduleData(const std::string& id, crypto::Bytes bytes) override {
        std::lock_guard lock(mutex);
        moduleMessages.emplace_back(id, std::move(bytes));
    }
    void audioPcm(std::span<const float> samples, uint64_t, uint32_t period) override {
        std::lock_guard lock(mutex);
        ++pcmChunks;
        pcmPeriod = period;
        for (const float value : samples) {
            pcmEnergy += double(value) * value;
        }
        pcmSamples += samples.size();
    }
    void audioStatus(const platform::AudioStatus& status) override {
        std::lock_guard lock(mutex);
        audioStates.push_back(status);
    }
    void failed(rcs_result result, const std::string& message) override {
        std::lock_guard lock(mutex);
        failures.emplace_back(result, message);
    }
    void admit(const Machine& machine) override {
        {
            std::lock_guard lock(mutex);
            asked.push_back(machine);
        }
        if (refuse) {
            fail(RCS_ERROR_AUTHENTICATION, "The host refused it (test)");
        }
    }
    void attached(const Machine& machine) override {
        std::lock_guard lock(mutex);
        ++attachedCount;
        attachedMachine = machine;
    }
    void detached(rcs_result result, const std::string& message) override {
        std::function<void()> then;
        {
            std::lock_guard lock(mutex);
            ++detachedCount;
            departures.emplace_back(result, message);
            departureThread = std::this_thread::get_id();
            then = onDetached;
        }
        if (then) {
            then();
        }
    }
};

struct InputRecorder {
    std::mutex mutex;
    std::vector<rcs_input_event> applied;
    std::atomic<int> secureFlag{-1}; // Whether the injector last made followed secure desktops.
    std::atomic<int> releases{0};
    std::atomic<bool> available{true};
    std::atomic<bool> refuse{false};
    std::atomic<bool> failNext{false}; // The next injector cannot be made.
    platform::CaptureTarget target;    // What the last injector was made for (under mutex).
    size_t count() {
        std::lock_guard lock(mutex);
        return applied.size();
    }
};

class RecordingInjector final : public platform::InputInjector {
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


struct States {
    std::mutex mutex;
    std::vector<std::pair<rcs_state, rcs_result>> list;
    AgentServer::StateHandler handler() {
        return [this](rcs_state state, rcs_result result) {
            std::lock_guard lock(mutex);
            list.emplace_back(state, result);
        };
    }
    bool has(rcs_state state, rcs_result result) {
        std::lock_guard lock(mutex);
        return std::any_of(list.begin(), list.end(), [&](const auto& s) { return s == std::pair{state, result}; });
    }
    size_t count(rcs_state state) {
        std::lock_guard lock(mutex);
        return size_t(std::count_if(list.begin(), list.end(), [&](const auto& s) { return s.first == state; }));
    }
};

AgentServer::Options options(const std::string& endpoint, const crypto::Key& key, bool reconnect) {
    return {endpoint, key, transport::LocalAccess::privileged, reconnect, 100ms};
}

void attaching() {
    test::UseTestPattern pattern;
    test::UseTestTone tone;
    test::UseSilentPlayer silent;
    InputRecorder recorder;
    seams::InputOverride input;
    input.factory = [&](const platform::CaptureTarget& target,
                        bool secure) -> std::unique_ptr<platform::InputInjector> {
        {
            std::lock_guard lock(recorder.mutex);
            recorder.target = target;
        }
        if (recorder.failNext.exchange(false)) {
            fail(RCS_ERROR_UNSUPPORTED, "No injector here (test)");
        }
        recorder.secureFlag = secure ? 1 : 0;
        return std::make_unique<RecordingInjector>(recorder);
    };
    input.permission = [&] { return recorder.available ? RCS_PERMISSION_GRANTED : RCS_PERMISSION_REQUIRED; };
    seams::setInputOverride(std::move(input));
    // The machine's desktop: 0 the user's, 1 a lock screen, 2 the sign-in screen.
    std::atomic<int> desktop{0};
    seams::setDesktopState(
        [&] {
            return platform::DesktopState{rcs_desktop_kind(desktop == 0 ? RCS_DESKTOP_USER : RCS_DESKTOP_SECURE),
                                          desktop != 2};
        });

    const std::string endpoint = testEndpoint();
    crypto::Key key;
    crypto::random(key);
    Recorder events;
    auto hubPointer = std::make_unique<RemoteAgent>(events, events,
                                                    RemoteAgent::Options{endpoint, key, transport::LocalAccess::privileged});
    auto& hub = *hubPointer;
    CHECK(!hub.attached());
    CHECK(hub.machine().displays.empty()); // Nothing is known before an agent attached.

    // The host decides who attaches: it sees the agent's process as the system reports it.
    {
        events.refuse = true;
        States states;
        AgentServer refused(options(endpoint, key, false), states.handler());
        CHECK(refused.start());
        CHECK(refused.wait(8s));
        CHECK(refused.state() == RCS_STATE_FAILED && refused.failure().first == RCS_ERROR_AUTHENTICATION &&
              contains(refused.failure().second, "host refused"));
        CHECK(!hub.attached());
        events.read([&](Recorder& r) {
            CHECK(r.asked.size() == 1 && r.asked[0].process.processId == ownProcess() &&
                  r.asked[0].process.account == transport::thisProcess().account && r.attachedCount == 0);
            return 0;
        });
        events.refuse = false;
    }
    // An agent without the key is turned away at the handshake.
    {
        crypto::Key wrong = key;
        wrong[0] ^= 1;
        States states;
        AgentServer stranger(options(endpoint, wrong, false), states.handler());
        CHECK(stranger.start());
        CHECK(stranger.wait(8s));
        CHECK(!hub.attached() && stranger.failure().first != RCS_OK);
    }
    // Another version, or no link at all, is told apart by the prefix: the session answers with its own
    // and drops it, before any handshake. (After the failed tries above this account waits a little
    // before it is heard again: until then it is dropped unread, and tries again.)
    for (const auto& theirs : {link::prefix(RCSDK_VERSION + 1),
                               crypto::Bytes(std::begin(transport::protocolMagic), std::end(transport::protocolMagic))}) {
        std::atomic<bool> never{false};
        auto padded = theirs;
        padded.resize(link::prefixSize, 0);
        bool answered = false;
        for (const auto until = std::chrono::steady_clock::now() + 8s; !answered && std::chrono::steady_clock::now() < until;) {
            transport::LocalConnection raw;
            crypto::Bytes answer(link::prefixSize);
            try {
                raw = transport::connectLocal(endpoint, never, 2s);
                raw.stream->write(padded, 2s);
                raw.stream->read(answer, 2s);
            } catch (const Failure&) {
                std::this_thread::sleep_for(100ms); // Not heard yet.
                continue;
            }
            answered = true;
            CHECK(answer == link::prefix());
            std::vector<uint8_t> more(1);
            CHECK(throws<Failure>([&] { raw.stream->read(more, 2s); })); // Dropped.
        }
        CHECK(answered && !hub.attached());
    }
    // An agent finds a session of another version: it ends at once, whatever its reconnect setting says.
    {
        const std::string other = testEndpoint("-other");
        transport::LocalListener fake(other, transport::LocalAccess::privileged);
        std::atomic<bool> never{false};
        std::thread session([&] {
            auto connection = fake.accept(never, never);
            crypto::Bytes theirs(link::prefixSize);
            connection.stream->read(theirs, 2s);
            CHECK(theirs == link::prefix());
            auto prefix = link::prefix(RCSDK_VERSION + 0x100); // Another minor version...
            prefix[7] = 2;                                      // ...and another layout.
            connection.stream->write(prefix, 2s);
            std::vector<uint8_t> rest(1);
            CHECK(throws<Failure>([&] { connection.stream->read(rest, 2s); })); // It left without a handshake.
        });
        States states;
        AgentServer agent(options(other, key, true), states.handler());
        const auto began = std::chrono::steady_clock::now();
        CHECK(agent.start());
        CHECK(agent.wait(3s));
        CHECK(std::chrono::steady_clock::now() - began < 2s);
        CHECK(agent.state() == RCS_STATE_FAILED && agent.failure().first == RCS_ERROR_UNSUPPORTED &&
              contains(agent.failure().second, link::versionText(RCSDK_VERSION + 0x100)));
        CHECK(states.count(RCS_STATE_CONNECTING) == 1); // Not tried again.
        session.join();
    }

    // The agent attaches and says what its machine offers; its process is the system's word.
    // (The tries that failed above make this account wait a little: it tries again.)
    States firstStates;
    auto first = std::make_unique<AgentServer>(options(endpoint, key, true), firstStates.handler());
    CHECK(first->start() && !first->start());
    CHECK(waitFor([&] { return hub.attached(); }));
    CHECK(waitFor([&] { return first->state() == RCS_STATE_RUNNING; }));
    const Machine own = localMachine();
    CHECK(events.read([&](Recorder& r) {
        return r.attachedCount == 1 && r.attachedMachine.process.processId == ownProcess() &&
               r.attachedMachine.process.user == transport::thisProcess().user;
    }));
    CHECK(hub.machine().implementations.size() == own.implementations.size());
    CHECK(hub.machine().capabilities.platform == own.capabilities.platform);

    if (!canEncode()) {
        std::printf("No H.264 encoder; the agent's picture is not tested\n");
    } else {
        // The picture: a start is answered with what the capture and the encoder are, then packets.
        VideoStart start;
        start.stream = {RCS_CODEC_H264, {}, 640, 360, 30, false, {}, false, acceleration()};
        start.bitrate = 2'000'000;
        start.cursorInVideo = false;
        hub.startVideo(start);
        CHECK(waitFor([&] { return events.read([](Recorder& r) { return !r.started.empty() && r.keyframes.size() >= 10; }); }));
        events.read([&](Recorder& r) {
            const auto& s = r.started.front();
            CHECK(s.stream == start.stream && s.content == (Rect{0, 0, 640, 360}) && !s.encoder.empty());
            CHECK(s.display.width == 1920 && s.display.height == 1080 && s.display.refreshHz == 60);
            CHECK(r.keyframes.front()); // A stream starts with a keyframe.
            CHECK(std::all_of(r.packetSizes.begin(), r.packetSizes.end(), [](size_t size) { return size > 0; }));
            return 0;
        });
        CHECK(waitFor([&] { return events.read([](Recorder& r) { return !r.captures.empty() && r.captures.back().captured >= 5; }); }));

        // A keyframe on request: one arrives after it was asked for.
        const size_t before = events.read([](Recorder& r) { return r.keyframes.size(); });
        hub.requestKeyframe(0, 0);
        CHECK(waitFor([&] {
            return events.read([&](Recorder& r) {
                return std::find(r.keyframes.begin() + ptrdiff_t(before), r.keyframes.end(), true) != r.keyframes.end();
            });
        }));
        hub.setRate(0, 0, 1'000'000, true); // A new target does not interrupt the stream.
        const size_t flowing = events.read([](Recorder& r) { return r.keyframes.size(); });
        CHECK(waitFor([&] { return events.read([&](Recorder& r) { return r.keyframes.size() >= flowing + 10; }); }));

        // The pointer goes beside the picture: its image once, its position as it moves, the image again from the cache.
        CHECK(waitFor([&] { return events.read([](Recorder& r) { return !r.cursors.empty(); }); }));
        events.read([](Recorder& r) {
            CHECK(r.cursors.front().image && r.cursors.front().image->width == 16 && r.cursors.front().imageSerial == 1);
            return 0;
        });
        test::patternControl().cursorX = 30000;
        CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.cursors.back().x == 30000 && r.cursors.back().image; }); }));
        test::patternControl().cursorImage = 2;
        CHECK(waitFor([&] {
            return events.read([](Recorder& r) {
                return r.cursors.back().imageSerial == 2 && r.cursors.back().image &&
                       r.cursors.back().image->type == RCS_CURSOR_HAND;
            });
        }));
        hub.setCursorInVideo(true); // The pointer goes into the picture: it stops arriving apart (no failure).

        // The agent's capture pausing (a lock screen) is reported, and resuming.
        test::patternControl().paused = RCS_CAPTURE_PAUSE_DESKTOP;
        CHECK(waitFor([&] {
            return events.read([](Recorder& r) { return r.captures.back().paused == RCS_CAPTURE_PAUSE_DESKTOP; });
        }));
        test::patternControl().paused = RCS_CAPTURE_PAUSE_GONE; // A window closed: the reason is reported too.
        CHECK(waitFor([&] {
            return events.read([](Recorder& r) { return r.captures.back().paused == RCS_CAPTURE_PAUSE_GONE; });
        }));
        test::patternControl().paused = false;
        CHECK(waitFor([&] { return events.read([](Recorder& r) { return !r.captures.back().paused; }); }));
    }

    // The sound the machine plays arrives as PCM, in the periods the capture delivers.
    hub.startAudio({RCS_AUDIO_CAPTURE_AUTO, false, 0});
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.pcmChunks >= 20; }); }));
    events.read([](Recorder& r) {
        CHECK(r.pcmPeriod == 480 && !r.audioStates.empty() && r.audioStates.front().active);
        const double rms = std::sqrt(r.pcmEnergy / double(r.pcmSamples));
        CHECK(rms > 0.3 && rms < 0.4); // The test tone: 0.5 amplitude.
        return 0;
    });

    // What the machine shows: reported at the first look and whenever it changes.
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return !r.desktops.empty(); }); }));
    CHECK(events.read(
        [](Recorder& r) { return r.desktops.front().kind == RCS_DESKTOP_USER && r.desktops.front().userLoggedOn; }));
    desktop.store(1); // A lock screen: secure, with someone signed in.
    CHECK(
        waitFor([&] { return events.read([](Recorder& r) { return r.desktops.back().kind == RCS_DESKTOP_SECURE; }); }));
    CHECK(events.read([](Recorder& r) { return r.desktops.back().userLoggedOn; }));
    desktop.store(2); // The sign-in screen: nobody signed in.
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return !r.desktops.back().userLoggedOn; }); }));
    desktop.store(0);
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.desktops.back().kind == RCS_DESKTOP_USER; }); }));

    // Input: executed on the agent's machine, results reported, availability followed.
    hub.configureInput({RCS_CAPTURE_WINDOW, 0, {}, 42}, {}, true);
    CHECK(waitFor([&] { return recorder.secureFlag.load() == 1; })); // The injector was made to follow secure desktops.
    {
        std::lock_guard lock(recorder.mutex);
        CHECK(recorder.target.source == RCS_CAPTURE_WINDOW && recorder.target.windowId == 42); // For what is shared.
    }
    hub.configureInput({}, {}, false);
    CHECK(waitFor([&] { return recorder.secureFlag.load() == 0; }));
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return !r.availability.empty() && bool(r.availability.back()); }); }));
    rcs_input_event press = RCS_INPUT_EVENT_INIT;
    press.type = RCS_INPUT_KEY;
    press.code = 4;
    press.down = 1;
    CHECK(hub.applyInput(press) == kInputDeferred);
    CHECK(waitFor([&] { return recorder.count() == 1; }));
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.inputApplied == 1; }); }));
    recorder.refuse = true; // An elevated window: the agent releases what is held, and says why.
    const int releases = recorder.releases;
    CHECK(hub.applyInput(press) == kInputDeferred);
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.lastInput == RCS_ERROR_PERMISSION_REQUIRED; }); }));
    CHECK(recorder.releases > releases);
    recorder.refuse = false;
    recorder.available = false; // A lock screen: noticed within a couple of seconds.
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return !bool(r.availability.back()); }); }, 4s));
    CHECK(hub.machine().inputPermission == RCS_PERMISSION_GRANTED); // Not taking input now is no matter of permission.
    recorder.available = true;
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return bool(r.availability.back()); }); }, 4s));
    const int beforeRelease = recorder.releases;
    hub.releaseInput();
    CHECK(waitFor([&] { return recorder.releases > beforeRelease; }));

    // A module's machine half in the agent: started there (a module the
    // agent's process does not have, or has in another version, says so),
    // messages both ways in order and in parts, stopped.
    auto& echo = test::echoModule();
    echo.clear();
    hub.startModule("test.echo", 1);
    hub.startModule("test.unregistered", 1);
    test::echoModule("test.other");
    hub.startModule("test.other", 2);
    CHECK(waitFor([&] {
        return events.read([](Recorder& r) {
            int answered = 0;
            for (const auto& [id, result, message] : r.moduleStatuses) {
                answered += id == "test.echo" && result == RCS_OK;
                answered += id == "test.unregistered" && result == RCS_ERROR_UNSUPPORTED &&
                            message.find("has not registered") != std::string::npos;
                answered += id == "test.other" && result == RCS_ERROR_UNSUPPORTED &&
                            message.find("has version 1") != std::string::npos;
            }
            return answered == 3;
        });
    }));
    CHECK(echo.saw("machine_start", nullptr, "share"));
    const crypto::Bytes brief{'a', 'b'};
    const crypto::Bytes large(9u << 20, 'm'); // Three link parts each way.
    hub.moduleData("test.echo", brief);
    hub.moduleData("test.echo", large);
    CHECK(waitFor([&] {
        return events.read([](Recorder& r) { return r.moduleMessages.size() == 2; });
    }, 20s));
    CHECK(events.read([&](Recorder& r) {
        const auto prefixed = [](const crypto::Bytes& bytes) {
            crypto::Bytes answer{'m', 'a', 'c', 'h', 'i', 'n', 'e', ':'};
            answer.insert(answer.end(), bytes.begin(), bytes.end());
            return answer;
        };
        return r.moduleMessages[0] == std::pair<std::string, crypto::Bytes>("test.echo", prefixed(brief)) &&
               r.moduleMessages[1].second == prefixed(large);
    }));
    hub.stopModule("test.echo");
    CHECK(waitFor([&] { return echo.saw("machine_stop"); }));
    hub.moduleData("test.echo", brief); // Nothing runs it now.
    std::this_thread::sleep_for(200ms);
    CHECK(events.read([](Recorder& r) { return r.moduleMessages.size() == 2; }) && !echo.overlapped);

    // The machine's windows: asked over the link, listed by the agent.
    seams::setWindowList([] {
        rcs_window_info shared{};
        shared.window_id = 99;
        shared.process_id = 7;
        std::snprintf(shared.title, sizeof(shared.title), "%s", "Shared");
        return std::vector<rcs_window_info>(3, shared);
    });
    const auto listed = hub.windows();
    CHECK(listed.size() == 3 && listed[0].window_id == 99 && std::string(listed[0].title) == "Shared");
    seams::setWindowList({});

    // Input that cannot be set up (no display to inject into): input is
    // unavailable and the agent stays; set up again, it is available.
    recorder.failNext = true;
    hub.configureInput({}, {}, false);
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return !bool(r.availability.back()); }); }));
    hub.configureInput({}, {}, false);
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return bool(r.availability.back()); }); }));
    CHECK(events.read([](Recorder& r) { return r.detachedCount == 0; }));

    // The agent fails (its capture cannot start): it leaves the session with why, the run's
    // asks wait, and it attaches again by itself, all set up anew.
    test::patternControl().failStart = true;
    VideoStart failing;
    failing.stream = {RCS_CODEC_H264, {}, 640, 360, 30, false, {}, false, acceleration()};
    failing.bitrate = 2'000'000;
    hub.startVideo(failing);
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.detachedCount == 1; }); }));
    if (!canEncode()) {
        hub.stopVideo(0, 0); // The next agent could not encode it either.
    }
    events.read([](Recorder& r) {
        CHECK(r.departures.back().first == RCS_ERROR_CAPTURE && contains(r.departures.back().second, "cannot start"));
        CHECK(r.failures.empty()); // An agent in another process does not end the run.
        return 0;
    });
    CHECK(waitFor([&] { return firstStates.has(RCS_STATE_RECONNECTING, RCS_ERROR_CAPTURE); }));
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.attachedCount == 2; }); }));
    CHECK(waitFor([&] { return first->state() == RCS_STATE_RUNNING && recorder.secureFlag.load() == 0; }));
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return bool(r.availability.back()); }); }));

    // A newer agent replaces this one, which is told so and ends for good although it would
    // reconnect; the session asked for nothing again, and the new agent works.
    States secondStates;
    auto second = std::make_unique<AgentServer>(options(endpoint, key, false), secondStates.handler());
    CHECK(second->start());
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.attachedCount == 3; }); }));
    CHECK(first->wait(8s));
    CHECK(first->state() == RCS_STATE_STOPPED && first->failure().first == RCS_ERROR_REPLACED);
    CHECK(firstStates.has(RCS_STATE_STOPPED, RCS_ERROR_REPLACED));
    CHECK(events.read([](Recorder& r) { return r.detachedCount == 1; })); // Replaced is not gone.
    const size_t chunks = events.read([](Recorder& r) { return r.pcmChunks; });
    CHECK(waitFor([&] { return events.read([&](Recorder& r) { return r.pcmChunks >= chunks + 20; }); })); // The sound again.
    const size_t startedCount = events.read([](Recorder& r) { return r.started.size(); });
    if (canEncode()) {
        CHECK(startedCount >= 2); // The picture started again on the new agent.
        const size_t packets = events.read([](Recorder& r) { return r.keyframes.size(); });
        CHECK(waitFor([&] { return events.read([&](Recorder& r) { return r.keyframes.size() >= packets + 10; }); }));
    }
    const size_t pressed = recorder.count();
    CHECK(hub.applyInput(press) == kInputDeferred); // Input is set up again: nothing was asked of the session.
    CHECK(waitFor([&] { return recorder.count() > pressed; }));

    // Without an agent the session hears the picture stop, the sound go and input go away.
    second.reset(); // Ends it: the agent's capture, sound and keys go with it.
    CHECK(waitFor([&] { return !hub.attached() && events.read([](Recorder& r) { return r.detachedCount == 2; }); }));
    events.read([&](Recorder& r) {
        CHECK(r.departures.back().first == RCS_OK); // It closed the link.
        CHECK(!bool(r.availability.back()));
        CHECK(r.desktops.back().kind == RCS_DESKTOP_UNKNOWN); // Nothing is known of the desktop now.
        CHECK(!r.audioStates.back().active && r.audioStates.back().result == RCS_ERROR_DEVICE);
        if (canEncode()) {
            CHECK(r.captures.back().paused == RCS_CAPTURE_PAUSE_AGENT);
        }
        return 0;
    });
    CHECK(hub.applyInput(press) == RCS_ERROR_INVALID_STATE); // Nobody to execute it...
    try {                                                    // ...nor to list its windows.
        hub.windows();
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_INVALID_STATE);
    }
    CHECK(!hub.machine().implementations.empty());           // What was known stays.

    // The next agent finds the asks waiting: everything starts again by itself.
    const size_t chunksBefore = events.read([](Recorder& r) { return r.pcmChunks; });
    const size_t startedBefore = events.read([](Recorder& r) { return r.started.size(); });
    States thirdStates;
    AgentServer third(options(endpoint, key, false), thirdStates.handler());
    CHECK(third.start());
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.attachedCount == 4; }); }));
    CHECK(waitFor([&] { return events.read([&](Recorder& r) { return r.pcmChunks >= chunksBefore + 20; }); }));
    if (canEncode()) {
        CHECK(waitFor([&] { return events.read([&](Recorder& r) { return r.started.size() > startedBefore && !r.captures.back().paused; }); }));
    }
    CHECK(waitFor([&] { return events.read([](Recorder& r) { return bool(r.availability.back()); }); }));

    // What the session stops stays stopped for the next agent too.
    hub.stopAudio();
    hub.stopVideo(0, 0);
    std::this_thread::sleep_for(300ms);
    const size_t quietChunks = events.read([](Recorder& r) { return r.pcmChunks; });
    const size_t quietPackets = events.read([](Recorder& r) { return r.keyframes.size(); });
    std::this_thread::sleep_for(500ms);
    CHECK(events.read([&](Recorder& r) { return r.pcmChunks == quietChunks && r.keyframes.size() == quietPackets; }));

    // The agent follows its session: when the session's side ends it looks for the next one.
    States patientStates;
    AgentServer patient(options(endpoint, key, true), patientStates.handler());
    CHECK(patient.start());
    CHECK(waitFor([&] { return patient.state() == RCS_STATE_RUNNING; }));
    CHECK(third.wait(8s) && third.failure().first == RCS_ERROR_REPLACED); // The newest takes over.
    CHECK(events.read([](Recorder& r) { return r.failures.empty(); }));
    hubPointer.reset();
    CHECK(waitFor([&] { return patient.state() == RCS_STATE_RECONNECTING; }));
    {
        Recorder again;
        RemoteAgent next(again, again, RemoteAgent::Options{endpoint, key, transport::LocalAccess::privileged});
        CHECK(waitFor([&] { return again.read([](Recorder& r) { return r.attachedCount == 1; }); }));
        CHECK(waitFor([&] { return patient.state() == RCS_STATE_RUNNING; })); // The agent hears it a moment later.
        patient.stop();
        CHECK(patient.state() == RCS_STATE_STOPPED);
    }
    seams::setInputOverride({});
    seams::setDesktopState({});
}

// Connections are checked side by side: one that stays silent does not keep
// an agent out, and is dropped after a short while.
void concurrency() {
    test::UseTestPattern pattern;
    const std::string endpoint = testEndpoint("-busy");
    crypto::Key key;
    crypto::random(key);
    Recorder events;
    RemoteAgent hub(events, events, RemoteAgent::Options{endpoint, key, transport::LocalAccess::privileged});
    std::atomic<bool> never{false};
    auto silent = transport::connectLocal(endpoint, never, 2s);
    std::this_thread::sleep_for(100ms); // It is being checked now.
    const auto began = std::chrono::steady_clock::now();
    States states;
    AgentServer agent(options(endpoint, key, false), states.handler());
    CHECK(agent.start());
    CHECK(waitFor([&] { return hub.attached(); }));
    CHECK(std::chrono::steady_clock::now() - began < 1500ms); // Before the silent one's time is up.
    std::vector<uint8_t> byte(1);
    try {
        silent.stream->read(byte, 5s); // The session's prefix comes only after the agent's.
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_DISCONNECTED); // Dropped, not left hanging.
    }
    CHECK(agent.state() == RCS_STATE_RUNNING);
}

// A stream's start takes its time (the capture, the encoder): where the
// session puts the pointer meanwhile holds, not what the start said before.
void pointerDuringStart() {
    if (!canEncode()) {
        std::printf("No H.264 encoder; the pointer during a start is not tested\n");
        return;
    }
    test::UseTestPattern pattern;
    test::patternControl().startMs = 300;
    Recorder events;
    {
        MediaAgent media(events);
        VideoStart start;
        start.stream = {RCS_CODEC_H264, {}, 640, 360, 30, false, {}, false, acceleration()};
        start.bitrate = 2'000'000;
        start.cursorInVideo = true;
        media.startVideo(start);
        std::this_thread::sleep_for(100ms); // The capture is starting.
        media.setCursorInVideo(false);
        CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.keyframes.size() >= 3; }); }));
        CHECK(!test::patternControl().cursorInVideo);
    }
    test::patternControl().startMs = 0;
}

// Why agents leave, said by the agent or seen by the session, and heard on
// the link's own thread, never on the caller's of the session's asks.
void departures() {
    test::UseTestPattern pattern;
    const std::string endpoint = testEndpoint("-departures");
    crypto::Key key;
    crypto::random(key);
    Recorder events;
    RemoteAgent hub(events, events, RemoteAgent::Options{endpoint, key, transport::LocalAccess::privileged});

    // An agent that says it failed leaves with its reason; the session goes on.
    {
        HandAgent agent(endpoint, key);
        CHECK(std::holds_alternative<link::Accepted>(agent.attach()));
        CHECK(waitFor([&] { return hub.attached(); }));
        agent.send(link::Failed{RCS_ERROR_CAPTURE, "The new desktop could not be attached (test)"});
        CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.detachedCount == 1; }); }));
        CHECK(events.read([](Recorder& r) {
            return r.departures.back() == std::pair<rcs_result, std::string>{RCS_ERROR_CAPTURE,
                                                                             "The new desktop could not be attached (test)"};
        }));
        CHECK(!hub.attached() && events.read([](Recorder& r) { return r.failures.empty(); }));
    }
    // A content rectangle outside the picture breaks the link: the agent leaves, the session goes on.
    {
        HandAgent agent(endpoint, key);
        CHECK(std::holds_alternative<link::Accepted>(agent.attach()));
        CHECK(waitFor([&] { return hub.attached(); }));
        VideoStarted started;
        started.stream = {RCS_CODEC_H264, {}, 640, 360, 30, false, {}, false, RCS_ACCELERATION_SOFTWARE};
        started.content = {0, 0, 640, 360};
        agent.send(link::Started{started});
        agent.send(link::Content{0, {0, 0, 640, 400}, {640, 400, 60}});
        CHECK(waitFor([&] { return events.read([](Recorder& r) { return r.detachedCount == 2; }); }));
        CHECK(events.read([](Recorder& r) {
            return r.departures.back().first == RCS_ERROR_PROTOCOL && contains(r.departures.back().second, "content");
        }));
    }
    // An agent that takes nothing of what the session sends is dropped; the session hears it
    // on the link's thread, so the host may call the session from there.
    {
        std::atomic<bool> calledBack{false};
        events.read([&](Recorder& r) {
            r.onDetached = [&] {
                hub.releaseInput(); // The session's asks, from the departure's handler.
                hub.requestKeyframe(0, 0);
                calledBack = true;
            };
            return 0;
        });
        HandAgent agent(endpoint, key);
        CHECK(std::holds_alternative<link::Accepted>(agent.attach()));
        CHECK(waitFor([&] { return hub.attached(); }));
        // It reads nothing from now on.
        for (int i = 0; i < 200'000 && hub.attached(); ++i) {
            hub.requestKeyframe(0, 0);
        }
        CHECK(waitFor([&] { return calledBack.load(); }));
        CHECK(events.read([](Recorder& r) {
            return r.departures.back().first == RCS_ERROR_NETWORK && contains(r.departures.back().second, "does not take") &&
                   r.departureThread != std::this_thread::get_id();
        }));
        events.read([](Recorder& r) {
            r.onDetached = {};
            return 0;
        });
    }
}

// The next ask of type Ask the session makes of a hand-driven agent.
template <class Ask>
Ask awaitAsk(HandAgent& agent) {
    for (;;) {
        auto message = agent.receive();
        if (auto* ask = std::get_if<Ask>(&message)) {
            return *ask;
        }
    }
}

// An agent that leaves before it lists its windows: its asker hears it at
// once, not after a second.
void windowsLeft() {
    const std::string endpoint = testEndpoint("-windows");
    crypto::Key key;
    crypto::random(key);
    Recorder events;
    RemoteAgent hub(events, events, RemoteAgent::Options{endpoint, key, transport::LocalAccess::privileged});
    HandAgent agent(endpoint, key);
    CHECK(std::holds_alternative<link::Accepted>(agent.attach()));
    CHECK(waitFor([&] { return hub.attached(); }));
    std::thread leaver([&] {
        awaitAsk<link::ListWindows>(agent);
        agent.channel.reset();
    });
    const auto asked = std::chrono::steady_clock::now();
    try {
        hub.windows();
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_INVALID_STATE && contains(failure.what(), "left before"));
    }
    CHECK(std::chrono::steady_clock::now() - asked < 900ms);
    leaver.join();
}

// Thumbnails through the thumbnail module, its machine's half in an agent
// over the link (this process's): taken there on a thread of the module's
// own, so the session's other asks go on meanwhile, and sent in parts (10
// MB); why one could not be taken comes back as the agent said it; an asker
// hears at once when the agent leaves or its session stops.
void thumbnails() {
    test::UseTestPattern pattern;
    registerThumbnails();
    std::atomic<int> slowMs{0};
    seams::setThumbnailCapture([&](const platform::ThumbnailRequest& request) {
        std::this_thread::sleep_for(std::chrono::milliseconds(slowMs.load()));
        if (request.target.source == RCS_CAPTURE_WINDOW) {
            fail(RCS_ERROR_INVALID_STATE, "The window does not show (test)");
        }
        platform::Thumbnail picture{request.maxWidth, request.maxHeight,
                                    std::vector<uint8_t>(size_t(request.maxWidth) * request.maxHeight * 4)};
        for (size_t i = 0; i < picture.pixels.size(); ++i) {
            picture.pixels[i] = uint8_t(i % 253);
        }
        return picture;
    });
    seams::setWindowList([] {
        rcs_window_info window{};
        window.window_id = 1;
        return std::vector<rcs_window_info>{window};
    });
    const std::string endpoint = testEndpoint("-thumbnails");
    const auto linkKey = rcsdk::generateConnectionKey();
    rcsdk::SessionConfig config(RCS_ROLE_SHARE);
    config.setAddress("127.0.0.1", 47941)
        .setKey(rcsdk::generateConnectionKey())
        .use(rcsdk::agentLink(endpoint, linkKey))
        .useModule(RCS_THUMBNAIL_MODULE, nullptr, 0);
    rcsdk::Session share(config);
    CHECK(share.start() == RCS_OK);
    rcs_capture_config display = RCS_CAPTURE_CONFIG_INIT;
    display.display_id = 1;
    const auto take = [&](const rcs_capture_config& what, uint32_t side) {
        return rcsdk::detail::picture(what, side, side, [&](const rcs_capture_config& alone, rcs_picture& out) {
            return rcs_thumbnail_capture(share.handle(), &alone, side, side, &out);
        });
    };
    const auto refusedWith = [&](const rcs_capture_config& what, uint32_t side) {
        try {
            take(what, side);
        } catch (const rcsdk::Error& error) {
            return std::pair<rcs_result, std::string>(error.result(), error.what());
        }
        return std::pair<rcs_result, std::string>(RCS_OK, "");
    };
    CHECK(refusedWith(display, 8).first == RCS_ERROR_INVALID_STATE); // Nobody to take it.
    const auto attach = [&] {
        auto agent = std::make_unique<rcsdk::Agent>(rcsdk::AgentConfig(endpoint, linkKey));
        CHECK(agent->start() == RCS_OK);
        CHECK(waitFor([&] { return refusedWith(display, 8).first != RCS_ERROR_INVALID_STATE; }));
        return agent;
    };
    {
        const auto agent = attach();
        const auto picture = take(display, 1600); // 1600 x 1600: 10 MB, in parts.
        CHECK(picture.width == 1600 && picture.height == 1600 && picture.pixels.size() == 10'240'000);
        bool same = true;
        for (size_t i = 0; i < picture.pixels.size(); ++i) {
            same = same && picture.pixels[i] == uint8_t(i % 253);
        }
        CHECK(same);
        rcs_capture_config window = RCS_CAPTURE_CONFIG_INIT;
        window.source = RCS_CAPTURE_WINDOW;
        window.window_id = 9;
        const auto hidden = refusedWith(window, 64);
        CHECK(hidden.first == RCS_ERROR_INVALID_STATE && contains(hidden.second, "does not show (test)"));

        // While one is taken, the agent goes on with the session's other asks.
        slowMs = 1500;
        std::thread slow([&] { CHECK(take(display, 16).width == 16); });
        std::this_thread::sleep_for(200ms);
        const auto began = std::chrono::steady_clock::now();
        CHECK(share.windows().size() == 1);
        CHECK(std::chrono::steady_clock::now() - began < 900ms);
        slow.join();

        // The agent leaves while one is taken: its asker hears it.
        std::thread leaver([&] {
            std::this_thread::sleep_for(200ms);
            CHECK(agent->stop() == RCS_OK);
        });
        const auto left = refusedWith(display, 16);
        CHECK(left.first == RCS_ERROR_INVALID_STATE && contains(left.second, "left before"));
        leaver.join();
    }
    {
        // The session stops while one is taken: its asker does not wait for it.
        slowMs = 0;
        const auto agent = attach();
        slowMs = 1500;
        std::thread stopper([&] {
            std::this_thread::sleep_for(200ms);
            CHECK(share.stop() == RCS_OK);
        });
        const auto asked = std::chrono::steady_clock::now();
        CHECK(refusedWith(display, 16).first == RCS_ERROR_INVALID_STATE);
        CHECK(std::chrono::steady_clock::now() - asked < 1200ms); // Not the agent's 1.5 s.
        stopper.join();
        slowMs = 0;
    }
    seams::setThumbnailCapture({});
    seams::setWindowList({});
}

// ---- Through a session --------------------------------------------------------------------------

// The same through the public interface: the host decides which agents attach,
// and a machine that cannot carry out the settings ends the run at once, also
// while it waits for a viewer.
void sessions() {
    test::UseTestPattern pattern;
    test::UseTestTone tone;
    test::UseSilentPlayer silent;
    struct Record {
        rcs_log_level level;
        rcs_log_source source;
        rcs_session session;
        rcs_agent agent;
        std::string message;
    };
    static std::mutex logMutex;
    static std::vector<Record> records;
    CHECK(rcsdk::setLogCallback(RCS_LOG_INFO, [](const rcs_log_record& r) {
              std::lock_guard lock(logMutex);
              records.push_back({r.level, r.source, r.session, r.agent, r.message});
          }) == RCS_OK);
    const auto logged = [](auto&& match) {
        std::lock_guard lock(logMutex);
        return std::any_of(records.begin(), records.end(), match);
    };
    seams::setSecureCapabilities({true, std::nullopt}); // The agents' machine follows secure desktops...
    const std::string endpoint = testEndpoint("-session");
    const auto linkKey = rcsdk::generateConnectionKey();
    std::mutex mutex;
    std::vector<rcs_agent_info> asked, reported;
    std::atomic<bool> accept{false};
    rcsdk::Callbacks callbacks;
    struct Gate final : rcsdk::AgentGate {
        std::function<bool(const rcs_agent_info&)> decide;
        bool attach(const rcs_agent_info& info) override { return decide(info); }
    } gate;
    gate.decide = [&](const rcs_agent_info& info) {
        std::lock_guard lock(mutex);
        asked.push_back(info);
        return accept.load();
    };
    callbacks.onEvent = [&](const rcs_event& event) {
        if (event.type == RCS_EVENT_AGENT) {
            std::lock_guard lock(mutex);
            reported.push_back(event.agent);
        }
    };
    auto& echo = test::echoModule(); // Its machine half runs in the agent.
    echo.clear();
    registerThumbnails();
    rcsdk::SessionConfig config(RCS_ROLE_SHARE);
    config.setAddress("127.0.0.1", 47940)
        .setKey(rcsdk::generateConnectionKey())
        .use(rcsdk::agentLink(endpoint, linkKey, RCS_AGENT_ACCESS_PRIVILEGED, &gate))
        .setSecureDesktop(true)
        .setKeepListening(true)
        .useModule("test.echo", nullptr, 0)
        .useModule(RCS_THUMBNAIL_MODULE, nullptr, 0);
    rcsdk::Session share(config, callbacks);
    CHECK(share.start() == RCS_OK);

    std::mutex statesMutex;
    std::vector<std::pair<rcs_state, rcs_result>> states;
    rcsdk::Agent agent(rcsdk::AgentConfig(endpoint, linkKey).setReconnect(true, 100),
                       [&](const rcs_agent_event& event) {
                           CHECK(event.type == RCS_AGENT_EVENT_STATE && event.struct_size == sizeof(event) &&
                                 event.timestamp_ns);
                           std::lock_guard lock(statesMutex);
                           states.emplace_back(event.state.value, event.state.result);
                       });
    CHECK(agent.start() == RCS_OK);
    CHECK(waitFor([&] {
        std::lock_guard lock(statesMutex);
        const std::pair<rcs_state, rcs_result> refused{RCS_STATE_CONNECTING, RCS_ERROR_AUTHENTICATION};
        return std::find(states.begin(), states.end(), refused) != states.end(); // Refused, it tries again.
    }));
    {
        std::lock_guard lock(mutex);
        const auto& info = asked.front();
        CHECK(!info.attached && info.process_id == ownProcess() &&
              std::string(info.account) == transport::thisProcess().account.substr(0, sizeof(info.account) - 1));
    }
    CHECK(share.state() == RCS_STATE_STARTING && !rcsdk::agentInfo(share).attached);
    // A thumbnail of the agent's machine (the thumbnail module), taken there.
    const auto thumbnail = [&] {
        return rcsdk::detail::picture(rcsdk::defaults<rcs_capture_config>(), 64, 48,
                                      [&](const rcs_capture_config& what, rcs_picture& out) {
                                          return rcs_thumbnail_capture(share.handle(), &what, 64, 48, &out);
                                      });
    };
    const auto thumbnailRefused = [&]() -> rcs_result {
        try {
            thumbnail();
        } catch (const rcsdk::Error& error) {
            return error.result();
        }
        return RCS_OK;
    };
    CHECK(thumbnailRefused() == RCS_ERROR_INVALID_STATE); // No agent to take it yet.
    accept = true;
    CHECK(waitFor([&] { return share.state() == RCS_STATE_LISTENING && agent.state() == RCS_STATE_RUNNING; }));
    {
        const auto info = rcsdk::agentInfo(share);
        CHECK(info.attached && info.process_id == ownProcess() && info.account[0] && info.result == RCS_OK);
    }
    seams::setThumbnailCapture([](const platform::ThumbnailRequest& request) {
        return platform::Thumbnail{request.maxWidth, request.maxHeight,
                                   std::vector<uint8_t>(size_t(request.maxWidth) * request.maxHeight * 4, 9)};
    });
    CHECK(waitFor([&] { return thumbnailRefused() != RCS_ERROR_INVALID_STATE; })); // Its machine's half started.
    const auto picture = thumbnail();
    CHECK(picture.width == 64 && picture.height == 48 && picture.pixels.size() == 64u * 48 * 4 &&
          picture.pixels[100] == 9);
    seams::setThumbnailCapture({});
    CHECK(logged([&](const Record& r) {
        return r.source == RCS_LOG_SOURCE_AGENT && r.agent == agent.handle() && !r.session &&
               contains(r.message, "Attached to the session");
    }));
    CHECK(logged([&](const Record& r) {
        return r.source == RCS_LOG_SOURCE_SESSION && r.session == share.handle() && !r.agent &&
               contains(r.message, "An agent attached");
    }));

    // The module's machine half runs in the agent: the session's half reaches it over the link.
    rcs_module half = nullptr;
    CHECK(rcs_module_find(share.handle(), "test.echo", &half) == RCS_OK);
    CHECK(waitFor([&] { return echo.saw("machine_joined", share.handle()); }));
    CHECK(rcs_module_send_machine(half, reinterpret_cast<const uint8_t*>("over the link"), 13) == RCS_OK);
    CHECK(waitFor([&] { return echo.saw("from_machine", share.handle(), "machine:over the link"); }));

    // A newer agent replaces it, which ends for good (the host may start it again). This one
    // says it failed: it leaves, and the run goes on; why is in the agent's info.
    crypto::Key key{};
    std::copy(linkKey.begin(), linkKey.end(), key.begin());
    {
        HandAgent hand(endpoint, key);
        CHECK(std::holds_alternative<link::Accepted>(hand.attach()));
        CHECK(agent.wait(8000) == RCS_OK && agent.state() == RCS_STATE_STOPPED &&
              agent.failure().result == RCS_ERROR_REPLACED);
        hand.send(link::Failed{RCS_ERROR_CAPTURE, "The new desktop could not be attached (test)"});
        CHECK(waitFor([&] { return !rcsdk::agentInfo(share).attached; }));
        const auto info = rcsdk::agentInfo(share);
        CHECK(info.result == RCS_ERROR_CAPTURE && std::string(info.message) == "The new desktop could not be attached (test)");
        CHECK(info.process_id == ownProcess()); // The last agent's, as the system reported it.
        std::lock_guard lock(mutex);
        CHECK(!reported.back().attached && reported.back().result == RCS_ERROR_CAPTURE);
    }
    // The halves the replaced agent ran are gone; the hand-spoken one ran none.
    CHECK(echo.saw("machine_left", share.handle()) && echo.saw("machine_stop"));
    CHECK(rcs_module_send_machine(half, reinterpret_cast<const uint8_t*>("nobody"), 6) == RCS_ERROR_INVALID_STATE);
    CHECK(share.state() == RCS_STATE_LISTENING);

    // ...and an agent whose machine cannot carry out the settings attaches while the session
    // waits for a viewer: the run ends at once, with why.
    seams::setSecureCapabilities({false, std::nullopt});
    // It logs errors only, whatever the process logs.
    rcsdk::Agent incapable(rcsdk::AgentConfig(endpoint, linkKey).setReconnect(true, 100).setLogLevel(RCS_LOG_ERROR));
    CHECK(incapable.start() == RCS_OK);
    CHECK(waitFor([&] { return share.state() == RCS_STATE_FAILED; }, 4s));
    CHECK(share.failure().result == RCS_ERROR_UNSUPPORTED && contains(share.failure().message, "secure_desktop"));
    CHECK(incapable.stop() == RCS_OK);
    CHECK(!logged([&](const Record& r) { return r.agent == incapable.handle() && r.level > RCS_LOG_ERROR; }));
    seams::setSecureCapabilities({});
    rcsdk::setLogCallback(RCS_LOG_INFO, {});
}

} // namespace

int main() {
#ifdef __ANDROID__
    // Android's SELinux lets a program run from adb create no Unix sockets in
    // files (an application can, in its own directory).
    std::printf("agent: the agent link's socket cannot be made by a program run from adb here; skipped\n");
    return 77;
#endif
    return runTest("agent", [] {
        // RCSDK_TEST_ONLY=messages|prefixes|timeouts|endpoints|attaching|concurrency|departures|pointerDuringStart|
        // windowsLeft|thumbnails|sessions
        // runs that case alone (debugging).
        const char* only = std::getenv("RCSDK_TEST_ONLY");
        const auto wanted = [&](const char* name) { return !only || std::string(only) == name; };
        if (wanted("messages")) {
            messages();
        }
        if (wanted("prefixes")) {
            prefixes();
        }
        if (wanted("timeouts")) {
            timeouts();
        }
        if (wanted("endpoints")) {
            endpoints();
        }
#ifndef _WIN32
        if (wanted("endpointPlaces")) {
            endpointPlaces();
        }
#endif
#if defined(__APPLE__) && TARGET_OS_OSX
        if (wanted("peerPrograms")) {
            peerPrograms();
        }
#endif
        if (wanted("attaching")) {
            attaching();
        }
        if (wanted("concurrency")) {
            concurrency();
        }
        if (wanted("departures")) {
            departures();
        }
        if (wanted("pointerDuringStart")) {
            pointerDuringStart();
        }
        if (wanted("windowsLeft")) {
            windowsLeft();
        }
        // These two need a session that shares the display.
        if (!canShareDisplay() && (wanted("thumbnails") || wanted("sessions"))) {
            std::printf("No display sharing; thumbnails and sessions through an agent are not tested\n");
            return;
        }
        if (wanted("thumbnails")) {
            thumbnails();
        }
        if (wanted("sessions")) {
            sessions();
        }
    });
}
