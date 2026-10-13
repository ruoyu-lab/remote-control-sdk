// What each codec implementation of this system costs per picture in real
// time: an encoder from a picture handed over to its packet, a decoder from
// a packet to its picture done on the GPU (as on_gpu_frame gets it), each
// with the process's CPU time per picture. The numbers docs/PERFORMANCE.md
// gives for NVDEC and oneVPL. Not part of the SDK; it uses the SDK's
// internal headers.
//
//   rcsdk_codec_timing [--codec h264|hevc|vp9|av1] [--format 420|420-10|444|444-10]
//                      [--size 1920x1080] [--fps 60] [--bitrate 20m] [--seconds 10]
//                      [--encoders | --decoders] [--backend NAME] [--gpu NAME]
//
// Pictures: a page of coloured text whose upper half scrolls 3 pixels a
// picture, the lower half still, made here; on Windows, BGRA textures on the
// encoder's GPU, as captured pictures are. The decoders decode one stream,
// encoded beforehand by the first hardware encoder of the format (else a
// software one). --backend: only implementations whose name contains NAME.
// --gpu (Windows): Media Foundation's hardware decoders, which decode on any
// GPU, on the one whose name contains NAME instead of the default GPU.
#include "codecs/codecs.hpp"
#include "platform/platform.hpp"
#include "util/util.hpp"

#ifdef _WIN32
#include "platform/windows/adapters.hpp"
#include "platform/windows/d3d.hpp"
#include "platform/windows/win.hpp"
#else
#include <sys/resource.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace rcsdk;
using namespace std::chrono_literals;

namespace {

struct Options {
    rcs_codec codec = RCS_CODEC_HEVC;
    rcs_picture_format format = RCS_PICTURE_444_8;
    uint32_t width = 1920, height = 1080, fps = 60, bitrate = 20'000'000, seconds = 10;
    bool encoders = true, decoders = true;
    std::string backend, gpu;
};

[[noreturn]] void usage(const std::string& why) {
    std::fprintf(stderr,
                 "%s\nrcsdk_codec_timing [--codec h264|hevc|vp9|av1] [--format 420|420-10|444|444-10]\n"
                 "                   [--size 1920x1080] [--fps 60] [--bitrate 20m] [--seconds 10]\n"
                 "                   [--encoders | --decoders] [--backend NAME] [--gpu NAME]\n",
                 why.c_str());
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage(arg + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--codec") {
            const auto name = value();
            o.codec = name == "h264"   ? RCS_CODEC_H264
                      : name == "hevc" ? RCS_CODEC_HEVC
                      : name == "vp9"  ? RCS_CODEC_VP9
                      : name == "av1"  ? RCS_CODEC_AV1
                                       : (usage("Unknown codec " + name), 0);
        } else if (arg == "--format") {
            const auto name = value();
            o.format = name == "420"      ? RCS_PICTURE_420_8
                       : name == "420-10" ? RCS_PICTURE_420_10
                       : name == "444"    ? RCS_PICTURE_444_8
                       : name == "444-10" ? RCS_PICTURE_444_10
                                          : (usage("Unknown format " + name), 0);
        } else if (arg == "--size") {
            if (std::sscanf(value().c_str(), "%ux%u", &o.width, &o.height) != 2 || o.width < 64 || o.height < 64) {
                usage("--size takes WxH");
            }
            o.width &= ~1u;
            o.height &= ~1u;
        } else if (arg == "--fps") {
            o.fps = uint32_t(std::stoul(value()));
        } else if (arg == "--bitrate") {
            std::string text = value();
            const char last = char(std::tolower(text.back()));
            const double scale = last == 'k' ? 1e3 : last == 'm' ? 1e6 : 1;
            if (scale > 1) {
                text.pop_back();
            }
            o.bitrate = uint32_t(std::stod(text) * scale);
        } else if (arg == "--seconds") {
            o.seconds = uint32_t(std::stoul(value()));
        } else if (arg == "--encoders") {
            o.decoders = false;
        } else if (arg == "--decoders") {
            o.encoders = false;
        } else if (arg == "--backend") {
            o.backend = value();
        } else if (arg == "--gpu") {
            o.gpu = value();
        } else {
            usage("Unknown option " + arg);
        }
    }
    if (!o.fps || o.fps > 240 || !o.seconds || !o.bitrate) {
        usage("--fps 1..240; --seconds and --bitrate above 0");
    }
    return o;
}

// The process's CPU time (user and kernel, every thread) so far.
uint64_t cpuNs() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto ns = [](const FILETIME& t) {
        return ((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100;
    };
    return ns(kernel) + ns(user);
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto ns = [](const timeval& t) {
        return uint64_t(t.tv_sec) * 1'000'000'000 + uint64_t(t.tv_usec) * 1000;
    };
    return ns(usage.ru_utime) + ns(usage.ru_stime);
#endif
}

// ---- Pictures ----------------------------------------------------------------------------------

// A page of coloured text, `rows` pixels high: characters of 10x20 pixels,
// strokes one or two pixels wide in dark grey, blue, red or green on white.
std::vector<uint8_t> page(uint32_t width, uint32_t rows) {
    std::vector<uint8_t> pixels(size_t(width) * rows * 4, 255);
    uint32_t state = 12345;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    static constexpr uint8_t colours[4][3] = {{40, 40, 40}, {200, 80, 0}, {0, 0, 210}, {60, 140, 20}}; // BGR
    for (uint32_t line = 0; (line + 1) * 20 <= rows; ++line) {
        const uint32_t length = next() % 4 == 0 ? 0 : next() % std::max(1u, (width - 32) / 10);
        for (uint32_t c = 0; c < length; ++c) {
            const auto& colour = colours[next() % 10 < 6 ? 0 : 1 + next() % 3];
            const uint32_t shape = next();
            for (uint32_t y = 4; y < 17; ++y) {
                for (uint32_t x = 1; x < 8; ++x) {
                    if ((shape >> ((y * 3 + x) % 24)) & 1 && (x == 1 || x == 6 || y == 4 || y == 10 || y == 16)) {
                        uint8_t* p = pixels.data() + (size_t(line * 20 + y) * width + 16 + c * 10 + x) * 4;
                        p[0] = colour[0], p[1] = colour[1], p[2] = colour[2];
                    }
                }
            }
        }
    }
    return pixels;
}

// Picture n: the page's upper half 3n pixels further on, its lower half still.
VideoFrame scrolled(const std::vector<uint8_t>& document, uint32_t width, uint32_t height, uint32_t n) {
    auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * 4);
    const uint32_t half = height / 2, rows = uint32_t(document.size() / 4 / width);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t from = y < half ? (y + 3 * n) % (rows - half) : rows - height + y;
        std::copy_n(document.data() + size_t(from) * width * 4, size_t(width) * 4,
                    pixels->data() + size_t(y) * width * 4);
    }
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4;
    frame.pixels = std::move(pixels);
    return frame;
}

#ifdef _WIN32
// The GPU whose name contains `name` (its packed LUID).
uint64_t gpuNamed(const std::string& name) {
    for (const auto& adapter : platform::win::hardwareAdapters()) {
        if (platform::win::narrow(adapter.description).find(name) != std::string::npos) {
            return (uint64_t(uint32_t(adapter.luid.HighPart)) << 32) | adapter.luid.LowPart;
        }
    }
    usage("No GPU here is named like " + name);
}

// A device on the GPU of `luid` (packed; 0: none).
std::shared_ptr<void> deviceOn(uint64_t luid) {
    if (!luid) {
        return nullptr;
    }
    auto adapter = platform::win::adapterWith(LUID{DWORD(luid), LONG(luid >> 32)});
    auto d3d = std::make_shared<platform::win::Device>();
    d3d->create(adapter.value);
    return std::shared_ptr<void>(d3d, d3d->device.value);
}

// `frame` as a BGRA texture on `device`, as capture gives pictures.
VideoFrame onGpu(const VideoFrame& frame, const std::shared_ptr<void>& device) {
    if (!device) {
        return frame;
    }
    auto* d3d = static_cast<ID3D11Device*>(device.get());
    auto desc = platform::win::textureDesc(frame.width, frame.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    const D3D11_SUBRESOURCE_DATA data{frame.pixels->data(), frame.stride, 0};
    platform::win::Com<ID3D11Texture2D> texture;
    platform::win::check(d3d->CreateTexture2D(&desc, &data, texture.out()), "Picture upload");
    VideoFrame gpu = frame;
    gpu.pixels.reset();
    gpu.stride = 0;
    gpu.surface = platform::win::share(texture.value);
    gpu.device = device;
    return gpu;
}
#else
uint64_t gpuNamed(const std::string& name) {
    usage("--gpu " + name + ": only on Windows");
}
std::shared_ptr<void> deviceOn(uint64_t) {
    return nullptr;
}
VideoFrame onGpu(const VideoFrame& frame, const std::shared_ptr<void>&) {
    return frame;
}
#endif

// ---- Timing ------------------------------------------------------------------------------------

struct Timing {
    double p50 = 0, p95 = 0, mean = 0; // ms
    double cpu = 0;                    // CPU ms per picture.
    size_t pictures = 0;
};

Timing timing(std::vector<uint64_t> ns, uint64_t cpu) {
    Timing t;
    t.pictures = ns.size();
    if (ns.empty()) {
        return t;
    }
    std::sort(ns.begin(), ns.end());
    t.p50 = double(ns[ns.size() / 2]) / 1e6;
    t.p95 = double(ns[std::min(ns.size() - 1, ns.size() * 95 / 100)]) / 1e6;
    uint64_t total = 0;
    for (const uint64_t n : ns) {
        total += n;
    }
    t.mean = double(total) / double(ns.size()) / 1e6;
    t.cpu = double(cpu) / double(ns.size()) / 1e6;
    return t;
}

const std::vector<platform::CodecImplementation>& implementations() {
    static const auto list = codecs::implementations();
    return list;
}

std::string label(const platform::CodecImplementation& i) {
    return i.name + (i.acceleration == RCS_ACCELERATION_HARDWARE ? " (hardware)" : " (software)");
}

platform::EncoderConfig encoding(const Options& o, const platform::CodecImplementation& i) {
    platform::EncoderConfig config;
    config.codec = o.codec;
    config.width = o.width;
    config.height = o.height;
    config.framesPerSecond = o.fps;
    config.bitrate = o.bitrate;
    config.acceleration = i.acceleration;
    config.backend = i.backend;
    config.device = i.acceleration == RCS_ACCELERATION_HARDWARE ? deviceOn(i.adapterLuid) : nullptr;
    config.picture = picture(o.format, RCS_MATRIX_BT709, RCS_RANGE_LIMITED);
    return config;
}

// `count` pictures through `i` at the frame rate: their packets, and the
// encoder's time per picture.
std::pair<std::vector<EncodedPacket>, Timing> encode(const Options& o, const platform::CodecImplementation& i,
                                                     const std::vector<uint8_t>& document, uint32_t count) {
    const auto config = encoding(o, i);
    auto encoder = codecs::createEncoder(config, implementations());
    encoder->open(config);
    std::vector<VideoFrame> ring;
    for (uint32_t n = 0; n < 60; ++n) {
        ring.push_back(onGpu(scrolled(document, o.width, o.height, n), config.device));
    }
    std::vector<EncodedPacket> packets;
    std::vector<uint64_t> ns;
    const auto take = [&](uint32_t n) {
        for (EncodedPacket packet; encoder->receive(packet, encoder->inFlight() >= 3 ? 1s : 0ms);) {
            packet.picture = config.picture;
            if (n >= o.fps) { // The first second warms up.
                ns.push_back(packet.encodeNs);
            }
            packets.push_back(std::move(packet));
        }
    };
    const auto interval = std::chrono::nanoseconds(1'000'000'000 / o.fps);
    const auto began = std::chrono::steady_clock::now();
    uint64_t cpu = 0;
    for (uint32_t n = 0; n < count + o.fps; ++n) {
        std::this_thread::sleep_until(began + interval * n);
        if (n == o.fps) {
            cpu = cpuNs();
        }
        VideoFrame frame = ring[n % ring.size()];
        frame.sequence = n + 1;
        frame.timestampNs = monotonicNs();
        encoder->submit(frame);
        take(n);
    }
    for (EncodedPacket packet; encoder->inFlight() && encoder->receive(packet, 2s);) {
        packet.picture = config.picture;
        ns.push_back(packet.encodeNs);
        packets.push_back(std::move(packet));
    }
    return {std::move(packets), timing(std::move(ns), cpuNs() - cpu)};
}

// `packets` through decoder `i` at the frame rate, each picture waited for
// on the GPU: the decoder's time per picture.
Timing decode(const Options& o, const platform::CodecImplementation& i, const std::vector<EncodedPacket>& packets) {
    platform::DecoderConfig config;
    config.codec = o.codec;
    config.acceleration = i.acceleration;
    config.backend = i.backend;
    config.picture = picture(o.format, RCS_MATRIX_BT709, RCS_RANGE_LIMITED);
    const bool anyGpu = i.backend == RCS_BACKEND_MEDIA_FOUNDATION && !o.gpu.empty();
    config.device =
        i.acceleration == RCS_ACCELERATION_HARDWARE ? deviceOn(anyGpu ? gpuNamed(o.gpu) : i.adapterLuid) : nullptr;
    auto decoder = codecs::createDecoder(config, implementations());
    decoder->open(config);
    std::atomic<bool> cancelled{false};
    std::vector<uint64_t> ns;
    const auto interval = std::chrono::nanoseconds(1'000'000'000 / o.fps);
    const auto began = std::chrono::steady_clock::now();
    uint64_t cpu = 0;
    for (size_t n = 0; n < packets.size(); ++n) {
        std::this_thread::sleep_until(began + interval * int64_t(n)); // Signed: an unsigned time point never passes.
        if (n == o.fps) {
            cpu = cpuNs();
        }
        const uint64_t started = monotonicNs();
        VideoFrame frame;
        if (decoder->decode(packets[n], frame, cancelled) && frame.waitGpu) {
            frame.waitGpu(1000);
        }
        if (n >= o.fps) {
            ns.push_back(monotonicNs() - started);
        }
    }
    return timing(std::move(ns), cpuNs() - cpu);
}

void print(const char* what, const std::string& name, const Timing& t, const Options& o) {
    std::printf("%s %s: %.2f / %.2f ms (p50 / p95), mean %.2f ms, CPU %.2f ms a picture (%.0f%% of a core at %u "
                "FPS), %zu pictures\n",
                what, name.c_str(), t.p50, t.p95, t.mean, t.cpu, t.cpu * o.fps / 10, o.fps, t.pictures);
}

} // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    std::printf("%s %s, %ux%u at %u FPS, %u kbit/s, %u s\n", codecName(o.codec), pictureFormatName(o.format), o.width,
                o.height, o.fps, o.bitrate / 1000, o.seconds);
    const auto document = page(o.width, o.height * 4);
    const auto takes = [&](const platform::CodecImplementation& i, rcs_codec_direction direction) {
        return i.codec == o.codec && i.direction == direction && (i.pictureFormats & (1u << o.format)) &&
               o.width <= i.maxWidth && o.height <= i.maxHeight;
    };
    const uint32_t count = o.seconds * o.fps;
    std::vector<EncodedPacket> stream;
    for (const rcs_acceleration acceleration : {RCS_ACCELERATION_HARDWARE, RCS_ACCELERATION_SOFTWARE}) {
        for (const auto& i : implementations()) {
            if (!takes(i, RCS_CODEC_ENCODER) || i.acceleration != acceleration) {
                continue;
            }
            const bool chosen = o.backend.empty() || i.name.find(o.backend) != std::string::npos;
            if (!(o.encoders && chosen) && !stream.empty()) {
                continue;
            }
            try {
                auto [packets, t] = encode(o, i, document, count);
                if (o.encoders && chosen) {
                    uint64_t bytes = 0;
                    for (const auto& packet : packets) {
                        bytes += packet.data.size();
                    }
                    print("encoder", label(i), t, o);
                    std::printf("  %.0f kbit/s\n", double(bytes) * 8 / (double(packets.size()) / o.fps) / 1000);
                }
                if (stream.empty()) {
                    stream = std::move(packets);
                    std::printf("(the decoders' stream: %s)\n", label(i).c_str());
                }
            } catch (const Failure& failure) {
                std::printf("encoder %s: %s\n", label(i).c_str(), failure.what());
            }
            std::fflush(stdout);
        }
    }
    if (o.decoders && stream.empty()) {
        usage("No encoder here makes the stream");
    }
    for (const auto& i : implementations()) {
        if (!o.decoders || !takes(i, RCS_CODEC_DECODER) ||
            (!o.backend.empty() && i.name.find(o.backend) == std::string::npos)) {
            continue;
        }
        try {
            print("decoder", label(i), decode(o, i, stream), o);
        } catch (const Failure& failure) {
            std::printf("decoder %s: %s\n", label(i).c_str(), failure.what());
        }
        std::fflush(stdout);
    }
    return 0;
}
