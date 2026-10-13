// Measures input-to-picture response and throughput against a latency target.
//
// Connects as a viewer, presses Space through the SDK and times until a decoded
// frame shows the target's next marker. Both timestamps come from this
// machine's monotonic clock; the end point is SDK frame delivery (no display).
// The marker is read from GPU frames by copying only a few rows back, or from
// CPU frames where the decoding acceleration yields no GPU frames.
//
//   rcsdk_response_probe --address IP --key-file key.bin [--port 47990]
//                        [--codec hevc,h264] [--acceleration hardware|software]
//                        [--samples 30] [--seconds 20] [--report response.csv]
//                        [--preset balanced|low-latency|low-bandwidth|quality]
//   (or --transport host --relay ADDR:PORT [--room NAME] [--relay-datagram]: through
//    rcsdk_host_relay, as rcsdk_cli does; or --transport relay --turn HOST:PORT
//    --turn-user NAME --turn-pass SECRET [--turn-transport udp|tcp|tls]
//    [--turn-pin HEX] [--turn-name NAME], with signals through --signal-out/--signal-in)
//
// By default the probe offers HEVC and H.264, so the sharer's list decides.
#include "visual_marker.hpp"

#include <rcsdk/rcsdk.hpp>
#include <rcsdk/tuning.hpp>

#include "../examples/common/relay_channel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#elif defined(_WIN32)
#include <d3d11.h>
#include <d3d10.h>
#include <wrl/client.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void pump(std::chrono::duration<double> duration) {
#ifdef __APPLE__
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, duration.count(), false);
#else
    std::this_thread::sleep_for(duration);
#endif
}

// Reads the marker from a native GPU frame.
int gpuMarker(const rcs_gpu_frame_info& info) {
#ifdef __APPLE__
    auto buffer = static_cast<CVPixelBufferRef>(info.resource);
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        return -1;
    }
    int value = -1;
    if (info.format == RCS_PIXEL_BGRA8) {
        value = marker::decodeBgra(static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer)), info.width,
                                   info.height, CVPixelBufferGetBytesPerRow(buffer));
    } else { // NV12: the luma plane is already gray.
        const auto* luma = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
        value =
            marker::decode(luma + marker::row(info.height) * CVPixelBufferGetBytesPerRowOfPlane(buffer, 0), info.width);
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return value;
#elif defined(_WIN32)
    using Microsoft::WRL::ComPtr;
    auto* texture = static_cast<ID3D11Texture2D*>(info.resource);
    auto* device = static_cast<ID3D11Device*>(info.device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const UINT top = (marker::row(info.height) - 2) & ~1u; // NV12 copies need even coordinates.
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Height = 4;
    staging.MipLevels = staging.ArraySize = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> rows;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &rows))) {
        return -1;
    }
    ComPtr<ID3D10Multithread> lock;
    context.As(&lock);
    lock->Enter();
    const D3D11_BOX box{0, top, 0, desc.Width, top + 4, 1};
    context->CopySubresourceRegion(rows.Get(), 0, 0, 0, 0, texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE map{};
    const bool mapped = SUCCEEDED(context->Map(rows.Get(), 0, D3D11_MAP_READ, 0, &map));
    lock->Leave();
    if (!mapped) {
        return -1;
    }
    std::vector<uint8_t> gray(info.width);
    const auto* bytes = static_cast<const uint8_t*>(map.pData);
    for (uint32_t x = 0; x < info.width; ++x) {
        unsigned sum = 0;
        for (unsigned r = 1; r < 4; ++r) {
            sum += info.format == RCS_PIXEL_NV12
                       ? bytes[r * map.RowPitch + x] // Luma plane.
                       : (bytes[r * map.RowPitch + x * 4] + bytes[r * map.RowPitch + x * 4 + 1] +
                          bytes[r * map.RowPitch + x * 4 + 2]) /
                             3;
        }
        gray[x] = uint8_t(sum / 3);
    }
    lock->Enter();
    context->Unmap(rows.Get(), 0);
    lock->Leave();
    return marker::decode(gray.data(), info.width);
#else
    (void)info;
    return -1;
#endif
}

struct Observation {
    std::atomic<int> marker{-1};
    std::atomic<int64_t> markerNs{0};
    std::atomic<uint64_t> frames{0};
    std::atomic<int64_t> firstNs{0}, lastNs{0};

    void frame(int value) {
        const int64_t t = nowNs();
        if (frames++ == 0) {
            firstNs = t;
        }
        lastNs = t;
        if (value >= 0 && value != marker) {
            marker = value;
            markerNs = t;
        }
    }
};

std::string argument(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return argv[i + 1];
        }
    }
    return fallback;
}

} // namespace

int main(int argc, char** argv) {
    const std::string preset = argument(argc, argv, "--preset", "balanced");
    if (preset != "balanced" && preset != "low-latency" && preset != "low-bandwidth" && preset != "quality") {
        std::cerr << "--preset takes balanced, low-latency, low-bandwidth or quality\n";
        return 2;
    }
    rcsdk::SessionConfig config(RCS_ROLE_VIEW, preset == "low-latency"     ? RCS_PRESET_LOW_LATENCY
                                               : preset == "low-bandwidth" ? RCS_PRESET_LOW_BANDWIDTH
                                               : preset == "quality"       ? RCS_PRESET_QUALITY
                                                                           : RCS_PRESET_BALANCED);
    config.setAddress(argument(argc, argv, "--address", "127.0.0.1"),
                   uint32_t(std::stoul(argument(argc, argv, "--port", "47990"))));
    config.setAudioPlayback(false); // The sound is decoded as a viewer's is, never played aloud.
    auto& identity = config.core().identity;
    std::ifstream key(argument(argc, argv, "--key-file", "key.bin"), std::ios::binary);
    if (!key.read(reinterpret_cast<char*>(identity.key), sizeof(identity.key))) {
        std::cerr << "--key-file must name the 32-byte connection key\n";
        return 2;
    }
    // Comma-separated, preferred first: e.g. "av1,hevc".
    std::stringstream codecs(argument(argc, argv, "--codec", "hevc,h264"));
    auto& offer = config.decoder();
    std::fill(std::begin(offer.codecs), std::end(offer.codecs), 0u);
    size_t count = 0;
    for (std::string name; std::getline(codecs, name, ',');) {
        const rcs_codec codec = name == "h264"   ? RCS_CODEC_H264
                                : name == "hevc" ? RCS_CODEC_HEVC
                                : name == "av1"  ? RCS_CODEC_AV1
                                                 : 0;
        if (!codec || count == RCS_MAX_CODECS) {
            std::cerr << "--codec takes up to three of h264, hevc, av1\n";
            return 2;
        }
        offer.codecs[count++] = codec;
    }
    const bool software = argument(argc, argv, "--acceleration", "hardware") == "software";
    offer.acceleration = software ? RCS_ACCELERATION_SOFTWARE : RCS_ACCELERATION_HARDWARE;
    const std::string transport = argument(argc, argv, "--transport", "udp");
    config.setTransport(transport == "host"    ? RCS_TRANSPORT_HOST
                     : transport == "relay" ? RCS_TRANSPORT_RELAY
                     : transport == "p2p" ? RCS_TRANSPORT_P2P
                     : transport == "udp" ? RCS_TRANSPORT_UDP
                                          : RCS_TRANSPORT_TCP);
    if (transport == "relay") {
        const std::string via = argument(argc, argv, "--turn-transport", "udp");
        if (via != "udp" && via != "tcp" && via != "tls") {
            std::cerr << "--turn-transport takes udp, tcp or tls\n";
            return 2;
        }
        config.setRelay(argument(argc, argv, "--turn", ""), argument(argc, argv, "--turn-user", ""),
                        argument(argc, argv, "--turn-pass", ""),
                        via == "tls"   ? RCS_TURN_TLS
                        : via == "tcp" ? RCS_TURN_TCP
                                       : RCS_TURN_UDP);
        // TLS: the server's certificate by its SHA-256 (64 hex digits), and its name.
        if (const auto hex = argument(argc, argv, "--turn-pin", ""); !hex.empty()) {
            rcsdk::CertificateSha256 pin{};
            if (hex.size() != 2 * pin.size() || hex.find_first_not_of("0123456789abcdefABCDEF") != hex.npos) {
                std::cerr << "--turn-pin takes the SHA-256 as 64 hex digits\n";
                return 2;
            }
            for (size_t i = 0; i < pin.size(); ++i) {
                pin[i] = uint8_t(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
            }
            config.setRelayCertificate(pin);
        }
        if (const auto name = argument(argc, argv, "--turn-name", ""); !name.empty()) {
            config.setRelayServerName(name);
        }
    }
    if (const auto stun = argument(argc, argv, "--stun", ""); !stun.empty()) {
        config.setStunServers({stun});
    }
    if (const auto name = argument(argc, argv, "--interface", ""); !name.empty()) {
        auto& network = config.extension<rcs_network_tuning_config>();
        if (name.size() >= sizeof(network.local_interface)) {
            std::cerr << "--interface takes a name of at most 63 bytes\n";
            return 2;
        }
        std::memcpy(network.local_interface, name.c_str(), name.size() + 1);
    }
    // P2P and relay: signals through files, as the CLI does.
    const std::string signalOut = argument(argc, argv, "--signal-out", ""),
                      signalIn = argument(argc, argv, "--signal-in", "");
    const int samples = std::stoi(argument(argc, argv, "--samples", "30"));
    const double seconds = std::stod(argument(argc, argv, "--seconds", "20"));
    const std::string report = argument(argc, argv, "--report", "");

    // HOST: through rcsdk_host_relay.
    std::unique_ptr<relay::RelayChannel> relayChannel;
    if (transport == "host") {
        const bool datagrams = std::find(argv, argv + argc, std::string("--relay-datagram")) != argv + argc;
        const auto kind = datagrams ? RCS_HOST_CHANNEL_DATAGRAM : RCS_HOST_CHANNEL_STREAM;
        relayChannel = std::make_unique<relay::RelayChannel>(argument(argc, argv, "--relay", "127.0.0.1:47910"),
                                                             argument(argc, argv, "--room", "rcsdk"), kind);
        config.use(rcsdk::hostTransport(*relayChannel, kind));
    }

    Observation observed;
    rcsdk::Callbacks callbacks;
#if defined(__APPLE__) || defined(_WIN32)
    constexpr bool gpuReadable = true;
#else
    constexpr bool gpuReadable = false; // Android's decoders keep layouts the CPU cannot read: the SDK's BGRA.
#endif
    if (gpuReadable && (!software || rcsdk::capabilities().software_decoding_gpu_frames)) {
        callbacks.onGpuFrame = [&](const rcsdk::GpuFrame& frame) {
            observed.frame(gpuMarker(frame.info()));
        };
    } else { // Software decoding on Windows yields CPU frames only.
        callbacks.onFrame = [&](const rcs_frame& f) {
            observed.frame(marker::decodeBgra(f.data, f.width, f.height, f.stride_bytes));
        };
    }
    callbacks.onSignal = [signalOut](const std::string& text, uint64_t) {
        std::ofstream(signalOut + ".part") << text;
        std::rename((signalOut + ".part").c_str(), signalOut.c_str());
    };
    rcsdk::Session session(config, std::move(callbacks));
    if (session.start() != RCS_OK) {
        std::cerr << "Could not start the session: " << session.failure().message << "\n";
        return 1;
    }
    struct RelayStop { // The relay delivers into the session until this goes, before the session.
        relay::RelayChannel* channel;
        ~RelayStop() {
            if (channel) {
                channel->stop();
            }
        }
    } relayStop{relayChannel.get()};
    if (relayChannel) {
        relayChannel->start(session);
    }
    for (const auto until = Clock::now() + 30s; !signalIn.empty() && Clock::now() < until;) {
        std::ifstream file(signalIn);
        std::string text;
        if (file && std::getline(file, text) && !text.empty()) {
            session.signal(text);
            break;
        }
        pump(100ms);
    }
    const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
    const auto ready = [&] {
        return samples == 0 ? observed.frames > 0 : session.controlInfo().granted && observed.marker >= 0;
    };
    while (!ready() && Clock::now() < deadline && session.state() != RCS_STATE_FAILED) {
        pump(50ms);
    }
    if (!ready()) {
        std::cerr << "Not ready: granted=" << session.controlInfo().granted << " marker=" << observed.marker
                  << " frames=" << observed.frames << " error=" << session.failure().message << "\n";
        return 3;
    }

    std::vector<double> results;
    int timeouts = 0;
    std::ofstream csv;
    if (!report.empty()) {
        csv.open(report);
        csv << "sample,response_ms\n";
    }
    for (int i = 0; i < samples && Clock::now() < deadline; ++i) {
        pump(1s * (0.15 + (i % 5) * 0.03)); // Irregular spacing avoids locking onto the frame cadence.
        const int expected = (observed.marker + 1) & 255;
        rcs_input_event press = RCS_INPUT_EVENT_INIT;
        press.type = RCS_INPUT_KEY;
        press.code = 44; // Space.
        const int64_t sent = nowNs();
        press.down = 1;
        session.sendInput(press);
        press.down = 0;
        session.sendInput(press);
        const auto limit = Clock::now() + 1500ms;
        while (observed.marker != expected && Clock::now() < limit) {
            pump(500us);
        }
        if (observed.marker == expected) {
            results.push_back((observed.markerNs - sent) / 1e6);
            if (csv) {
                csv << i << ',' << results.back() << '\n';
            }
        } else {
            ++timeouts;
            if (csv) {
                csv << i << ",timeout\n";
            }
        }
    }
    while (Clock::now() < deadline) {
        pump(50ms);
    }
    const auto stream = session.streamInfo();
    const auto stats = session.stats<rcs_video_receive_stats>();
    const auto traffic = session.stats<rcs_network_stats>();
    const double fps = observed.frames > 1 ? (observed.frames - 1) / ((observed.lastNs - observed.firstNs) / 1e9) : 0;
    session.stop();

    std::sort(results.begin(), results.end());
    const auto percentile = [&](double q) {
        return results.empty() ? 0.0 : results[std::min(results.size() - 1, size_t(q * results.size()))];
    };
    std::cout << "path " << traffic.path << "  round trip " << traffic.round_trip_ns / 1e6 << " ms\n";
    std::cout << "stream " << stream.width << "x" << stream.height << "  fps " << fps << "  samples " << results.size()
              << "  timeouts " << timeouts << "  p50 " << percentile(.5) << " ms  p95 " << percentile(.95)
              << " ms  p99 " << percentile(.99) << " ms\n"
              << "codec " << stream.codec << "  received " << stats.received_packets << "  decoded "
              << stats.decoded_frames << "  dropped " << stats.dropped_packets << "  recoveries "
              << stats.recovery_requests << "  last decode " << stats.last_decode_ns / 1e6 << " ms  last queue "
              << stats.last_queue_ns / 1e6 << " ms  network " << traffic.bytes / 1'000'000 << " MB  decoder "
              << stats.decoder << "\n";
    return samples && results.empty() ? 4 : 0;
}
