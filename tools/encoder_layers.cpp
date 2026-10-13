// What temporal layers cost each encoder of this system, and what a second
// encoding session costs the first: the numbers that decide whether an
// encoder layers by default (docs/PERFORMANCE.md).
// Not part of the SDK; it uses the SDK's internal headers.
//
//   rcsdk_encoder_layers [--codecs h264,hevc,av1,vp8,vp9] [--backend NAME]
//                        [--acceleration hardware|software] [--size 1920x1080]
//                        [--fps 60] [--frames 480] [--bitrates 450k,1.5m,5m]
//                        [--content typing,scrolling] [--input FILE]
//                        [--layers-only | --sessions-only] [--seconds 5]
//                        [--csv FILE]
//
// For each encoder (codec, acceleration, backend; on Windows each GPU's),
// content and bitrate, the same pictures are encoded as fast as the encoder
// takes them, with the timestamps of --fps:
//   L1T1      one layer: the bitrate it made, the pictures it made, and the
//             luma PSNR of what a viewer decodes, of the pictures made
//             ("coded") and of every picture as shown (a picture the encoder
//             left out shows the one before: "shown").
//   L1T2/T3   the same with two (three) temporal layers, where the encoder
//             makes them; base: the share of the bitrate in layer 0 (without
//             keyframes), and a viewer that gets layer 0 alone: decoder
//             errors and its PSNR as shown.
//   half      one layer at half the frame rate, at the bitrate layer 0 of
//             L1T2 took: what a lower encoding gives the viewer the base
//             layer would serve.
// Then, in real time at --fps for --seconds each, an encoder's time per
// picture (submit to packet, p50 and p95) alone and with a second session of
// the same implementation encoding the same pictures at half the frame rate
// (a lower encoding of the same size), and whether that changed how the
// first works (VideoToolbox: its pipelined encoder).
// Content: typing (an editor's page half full of text, one more character
// each picture) and scrolling (the upper half of such a page scrolls 3
// pixels each picture), made here; or
// --input, a recording of a real screen (rcsdk_cli --role view --record
// PREFIX: PREFIX.h264, .h265, .obu or .ivf), decoded here, at its size.
// The results print as a table; --csv writes them too.
#include "codecs/codecs.hpp"
#include "media/av1.hpp"
#include "media/encoder_output.hpp"
#include "media/layers.hpp"
#include "media/nal.hpp"
#include "media/vpx.hpp"
#include "platform/platform.hpp"
#include "util/util.hpp"

#ifdef _WIN32
#include "platform/windows/adapters.hpp"
#include "platform/windows/d3d.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace rcsdk;
using namespace std::chrono_literals;

namespace {

struct Options {
    std::set<rcs_codec> codecs{RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_AV1, RCS_CODEC_VP8, RCS_CODEC_VP9};
    std::string backend; // An implementation's name contains it; empty: all.
    std::optional<rcs_acceleration> acceleration;
    uint32_t width = 1920, height = 1080, fps = 60, frames = 480, seconds = 5;
    std::vector<uint32_t> bitrates{450'000, 1'500'000, 5'000'000};
    std::vector<std::string> contents{"typing", "scrolling"};
    std::string input;
    bool layers = true, sessions = true;
    std::string csv;
};

[[noreturn]] void usage(const std::string& why) {
    std::fprintf(stderr,
                 "%s\nrcsdk_encoder_layers [--codecs h264,hevc,av1,vp8,vp9] [--backend NAME]\n"
                 "                     [--acceleration hardware|software] [--size 1920x1080] [--fps 60]\n"
                 "                     [--frames 480] [--bitrates 450k,1.5m,5m] [--content typing,scrolling]\n"
                 "                     [--input FILE] [--layers-only | --sessions-only] [--seconds 5] [--csv FILE]\n",
                 why.c_str());
    std::exit(2);
}

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> parts;
    std::stringstream stream(text);
    for (std::string part; std::getline(stream, part, ',');) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

uint32_t rate(std::string text) {
    double scale = 1;
    const char last = text.empty() ? 0 : char(std::tolower(text.back()));
    if (last == 'k' || last == 'm') {
        scale = last == 'k' ? 1e3 : 1e6;
        text.pop_back();
    }
    return uint32_t(std::stod(text) * scale);
}

rcs_codec codecNamed(const std::string& name) {
    static const std::map<std::string, rcs_codec> names{{"h264", RCS_CODEC_H264},
                                                        {"hevc", RCS_CODEC_HEVC},
                                                        {"av1", RCS_CODEC_AV1},
                                                        {"vp8", RCS_CODEC_VP8},
                                                        {"vp9", RCS_CODEC_VP9}};
    const auto found = names.find(name);
    if (found == names.end()) {
        usage("Unknown codec " + name);
    }
    return found->second;
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
        if (arg == "--codecs") {
            o.codecs.clear();
            for (const auto& name : split(value())) {
                o.codecs.insert(codecNamed(name));
            }
        } else if (arg == "--backend") {
            o.backend = value();
        } else if (arg == "--acceleration") {
            const auto a = value();
            o.acceleration = a == "hardware" ? RCS_ACCELERATION_HARDWARE : RCS_ACCELERATION_SOFTWARE;
        } else if (arg == "--size") {
            const auto text = value();
            if (std::sscanf(text.c_str(), "%ux%u", &o.width, &o.height) != 2 || o.width < 16 || o.height < 16) {
                usage("--size takes WxH");
            }
            o.width &= ~1u;
            o.height &= ~1u;
        } else if (arg == "--fps") {
            o.fps = uint32_t(std::stoul(value()));
        } else if (arg == "--frames") {
            o.frames = uint32_t(std::stoul(value()));
        } else if (arg == "--seconds") {
            o.seconds = uint32_t(std::stoul(value()));
        } else if (arg == "--bitrates") {
            o.bitrates.clear();
            for (const auto& text : split(value())) {
                o.bitrates.push_back(rate(text));
            }
        } else if (arg == "--content") {
            o.contents = split(value());
        } else if (arg == "--input") {
            o.input = value();
            o.contents = {"input"};
        } else if (arg == "--layers-only") {
            o.sessions = false;
        } else if (arg == "--sessions-only") {
            o.layers = false;
        } else if (arg == "--csv") {
            o.csv = value();
        } else {
            usage("Unknown option " + arg);
        }
    }
    if (!o.fps || o.fps > 240 || !o.frames || o.bitrates.empty()) {
        usage("--fps 1..240, --frames and --bitrates must not be empty");
    }
    return o;
}

// ---- Pictures --------------------------------------------------------------------------------

using Pixels = std::shared_ptr<std::vector<uint8_t>>;

VideoFrame frameOf(Pixels pixels, uint32_t width, uint32_t height) {
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4;
    frame.pixels = std::move(pixels);
    return frame;
}

// Text as an editor shows it: characters of 10x20 pixels in lines, each a
// 5x7 pattern from its number drawn twice the size with soft edges (as a
// font's smoothing makes them), dark grey, blue or green on white.
uint32_t mix(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

constexpr uint32_t cellWidth = 10, cellHeight = 20;

// How much of pixel (x, y) of its cell character `c` covers, 0..4.
uint32_t coverage(uint32_t c, uint32_t x, uint32_t y) {
    if (y < 3 || y >= 3 + 14) {
        return 0;
    }
    const auto dot = [&](uint32_t dx, uint32_t dy) {
        return dx < 5 && dy < 7 && (mix(c * 64 + dy * 8 + dx) & 7) < 4 ? 1u : 0u;
    };
    const uint32_t gx = x, gy = y - 3; // Twice the pattern's size; each pixel the mean of the two dots around it.
    return dot(gx / 2, gy / 2) + dot((gx + 1) / 2, gy / 2) + dot(gx / 2, (gy + 1) / 2) +
           dot((gx + 1) / 2, (gy + 1) / 2);
}

// Character `c` with its top left at (x0, y0), clipped to rows [top, bottom).
void draw(std::vector<uint8_t>& pixels, uint32_t width, uint32_t c, uint32_t x0, int64_t y0, uint32_t top,
          uint32_t bottom) {
    static constexpr uint8_t colours[3][3] = {{40, 40, 40}, {200, 80, 0}, {60, 140, 20}}; // BGR
    const auto& colour = colours[mix(c) % 10 < 7 ? 0 : mix(c) % 10 < 9 ? 1 : 2];
    for (uint32_t y = 0; y < cellHeight; ++y) {
        const int64_t row = y0 + y;
        if (row < top || row >= bottom) {
            continue;
        }
        for (uint32_t x = 0; x < cellWidth; ++x) {
            if (const uint32_t a = coverage(c, x, y)) {
                uint8_t* p = pixels.data() + (size_t(row) * width + x0 + x) * 4;
                for (int i = 0; i < 3; ++i) {
                    p[i] = uint8_t((p[i] * (4 - a) + colour[i] * a) / 4);
                }
            }
        }
    }
}

void fill(std::vector<uint8_t>& pixels, uint8_t value) {
    std::fill(pixels.begin(), pixels.end(), value);
    for (size_t i = 3; i < pixels.size(); i += 4) {
        pixels[i] = 255;
    }
}

// The characters of line `line` of a document: its length, from its number.
uint32_t lineLength(uint64_t line, uint32_t most) {
    return (mix(uint32_t(line) * 31 + 7) % 4 == 0) ? 0 : std::min(most, 20 + mix(uint32_t(line)) % most);
}

// typing: an editor's page half full of text, picture n with n more
// characters typed after it.
VideoFrame typing(uint32_t width, uint32_t height, uint32_t n) {
    auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * 4);
    fill(*pixels, 255);
    const uint32_t perLine = std::max(1u, (width - 32) / cellWidth), lines = std::max(1u, (height - 32) / cellHeight);
    uint32_t line = 0;
    for (; line < lines / 2; ++line) {
        const uint32_t length = lineLength(line, perLine);
        for (uint32_t column = 0; column < length; ++column) {
            draw(*pixels, width, line * 1000 + column, 16 + column * cellWidth, 16 + line * cellHeight, 0, height);
        }
    }
    for (uint32_t c = 0; c < n; ++c) { // A full page starts again at its middle.
        const uint32_t at = c % (perLine * (lines - lines / 2));
        const uint32_t l = line + at / perLine, column = at % perLine;
        draw(*pixels, width, l * 1000 + column, 16 + column * cellWidth, 16 + l * cellHeight, 0, height);
    }
    return frameOf(std::move(pixels), width, height);
}

// scrolling: the upper half a document 3 pixels further on each picture,
// the lower half still.
VideoFrame scrolling(uint32_t width, uint32_t height, uint32_t n) {
    auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(width) * height * 4);
    fill(*pixels, 255);
    const uint32_t perLine = std::max(1u, (width - 32) / cellWidth), half = height / 2;
    const uint64_t offset = uint64_t(n) * 3;
    for (uint64_t line = offset / cellHeight; line * cellHeight < offset + half; ++line) {
        const uint32_t length = lineLength(line, perLine);
        for (uint32_t column = 0; column < length; ++column) {
            draw(*pixels, width, uint32_t(line) * 1000 + column, 16 + column * cellWidth,
                 int64_t(line * cellHeight) - int64_t(offset), 0, half);
        }
    }
    for (uint64_t line = 0; (line + 1) * cellHeight <= height - half; ++line) {
        const uint32_t length = lineLength(line + 5000, perLine);
        for (uint32_t column = 0; column < length; ++column) {
            draw(*pixels, width, uint32_t(line + 5000) * 1000 + column, 16 + column * cellWidth,
                 int64_t(half + line * cellHeight), half, height);
        }
    }
    return frameOf(std::move(pixels), width, height);
}

// A decoded picture as BGRA on the CPU.
void withImage(const VideoFrame& frame, const std::function<void(const uint8_t*, size_t)>& use) {
    if (frame.pixels) {
        use(frame.pixels->data(), frame.stride);
        return;
    }
    platform::withCpuImage(
        frame, [&](const rcs_frame& image) { use(static_cast<const uint8_t*>(image.data), image.stride_bytes); });
}

// The luma (BT.709 weights) of a BGRA picture.
std::vector<uint8_t> luma(const uint8_t* bgra, size_t stride, uint32_t width, uint32_t height) {
    std::vector<uint8_t> y(size_t(width) * height);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* p = bgra + row * stride;
        for (uint32_t x = 0; x < width; ++x, p += 4) {
            y[size_t(row) * width + x] = uint8_t((p[2] * 54 + p[1] * 183 + p[0] * 19 + 128) >> 8);
        }
    }
    return y;
}

double mse(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    uint64_t sum = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const int d = int(a[i]) - int(b[i]);
        sum += uint64_t(d * d);
    }
    return double(sum) / double(std::max<size_t>(1, a.size()));
}

double psnr(double meanSquared) {
    return meanSquared <= 1e-10 ? 99 : 10 * std::log10(255.0 * 255.0 / meanSquared);
}

#ifdef _WIN32
// A device on the GPU of `luid` (packed; 0: none), for that GPU's encoders.
std::shared_ptr<void> deviceOn(uint64_t luid) {
    if (!luid) {
        return nullptr;
    }
    auto adapter = platform::win::adapterWith(LUID{DWORD(luid), LONG(luid >> 32)});
    auto d3d = std::make_shared<platform::win::Device>();
    d3d->create(adapter.value);
    return std::shared_ptr<void>(d3d, d3d->device.value);
}
#else
std::shared_ptr<void> deviceOn(uint64_t) {
    return nullptr;
}
#endif

// ---- A recording as the source -----------------------------------------------------------------

// The pictures of a recording (rcsdk_cli --record), decoded, one at a time.
class Recording {
public:
    explicit Recording(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            usage("Cannot read " + path);
        }
        bytes_.assign(std::istreambuf_iterator<char>(in), {});
        const auto ends = [&](const char* suffix) {
            return path.size() >= std::strlen(suffix) &&
                   path.compare(path.size() - std::strlen(suffix), std::strlen(suffix), suffix) == 0;
        };
        codec_ = ends(".h264") ? RCS_CODEC_H264 : ends(".h265") ? RCS_CODEC_HEVC : ends(".obu") ? RCS_CODEC_AV1 : 0;
        if (ends(".ivf")) {
            if (bytes_.size() < 32 || std::memcmp(bytes_.data(), "DKIF", 4) != 0) {
                usage(path + " is no IVF file");
            }
            codec_ = std::memcmp(bytes_.data() + 8, "VP90", 4) == 0 ? RCS_CODEC_VP9 : RCS_CODEC_VP8;
        }
        if (!codec_) {
            usage(path + ": a recording is .h264, .h265, .obu or .ivf");
        }
        pictures_ = split();
        if (pictures_.empty()) {
            usage(path + " holds no picture");
        }
    }

    // The next picture, from the first again after the last; its size is the recording's.
    VideoFrame next() {
        for (;;) {
            if (at_ >= pictures_.size() || !decoder_) {
                at_ = 0;
                output_.emplace(codec_, Picture{}, 0, 0, 30);
                platform::DecoderConfig config;
                config.codec = codec_;
                config.cpuImages = true;
                config.acceleration = RCS_ACCELERATION_SOFTWARE;
                for (const auto& i : implementations()) {
                    if (i.codec == codec_ && i.direction == RCS_CODEC_DECODER) {
                        config.acceleration = i.acceleration; // The first: hardware where there is one.
                        break;
                    }
                }
                decoder_ = codecs::createDecoder(config, implementations());
                decoder_->open(config);
            }
            const auto [begin, size] = pictures_[at_++];
            EncodedPacket packet;
            packet.codec = codec_;
            output_->split({bytes_.data() + begin, size}, packet);
            if (codec_ == RCS_CODEC_AV1) {
                packet.keyframe = at_ == 1;
            }
            std::atomic<bool> cancel{false};
            VideoFrame decoded;
            if (!decoder_->decode(packet, decoded, cancel)) {
                continue;
            }
            auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(decoded.width) * decoded.height * 4);
            withImage(decoded, [&](const uint8_t* bgra, size_t stride) {
                for (uint32_t y = 0; y < decoded.height; ++y) {
                    std::memcpy(pixels->data() + size_t(y) * decoded.width * 4, bgra + y * stride, decoded.width * 4);
                }
            });
            return frameOf(std::move(pixels), decoded.width & ~1u, decoded.height & ~1u);
        }
    }

    static const std::vector<platform::CodecImplementation>& implementations() {
        static const auto list = codecs::implementations();
        return list;
    }

private:
    // Where each picture is: Annex B access units (a new one at the first
    // slice of a picture, its parameter sets with it), OBUs between temporal
    // delimiters, IVF's frames.
    std::vector<std::pair<size_t, size_t>> split() const {
        std::vector<std::pair<size_t, size_t>> list;
        if (vpCodec(codec_)) {
            for (size_t at = 32; at + 12 <= bytes_.size();) {
                const size_t size = bytes_[at] | (bytes_[at + 1] << 8) | (size_t(bytes_[at + 2]) << 16) |
                                    (size_t(bytes_[at + 3]) << 24);
                if (at + 12 + size > bytes_.size()) {
                    break;
                }
                list.push_back({at + 12, size});
                at += 12 + size;
            }
            return list;
        }
        if (codec_ == RCS_CODEC_AV1) {
            size_t start = 0;
            for (const auto& obu : av1::obus(bytes_)) {
                const size_t at = size_t(obu.bytes.data() - bytes_.data());
                if (obu.type == av1::obuTemporalDelimiter && at > start) {
                    list.push_back({start, at - start});
                    start = at;
                }
            }
            list.push_back({start, bytes_.size() - start});
            return list;
        }
        // Annex B: each unit's start code, and whether it begins a picture.
        std::optional<size_t> picture, pending; // Where the current picture began; units waiting for the next.
        bool slices = false;
        for (size_t i = 0; i + 3 < bytes_.size(); ++i) {
            if (bytes_[i] || bytes_[i + 1] || bytes_[i + 2] != 1) {
                continue;
            }
            const size_t start = i > 0 && !bytes_[i - 1] ? i - 1 : i;
            const uint8_t* unit = bytes_.data() + i + 3;
            const size_t left = bytes_.size() - i - 3;
            const bool h264 = codec_ == RCS_CODEC_H264;
            const unsigned type = h264 ? unit[0] & 31 : (unit[0] >> 1) & 63;
            const bool vcl = h264 ? type >= 1 && type <= 5 : type < 32;
            const bool first = vcl && left > 2 && (unit[h264 ? 1 : 2] & 0x80);
            if (first && slices) {
                list.push_back({*picture, pending.value_or(start) - *picture});
                picture = pending.value_or(start);
                slices = false;
            }
            if (!vcl && slices && !pending) {
                pending = start; // Parameter sets, SEI: of the next picture.
            }
            if (vcl) {
                pending.reset();
                slices = true;
            }
            if (!picture) {
                picture = start;
            }
            i += 2;
        }
        if (picture && slices) {
            list.push_back({*picture, bytes_.size() - *picture});
        }
        return list;
    }

    std::vector<uint8_t> bytes_;
    rcs_codec codec_ = 0;
    std::vector<std::pair<size_t, size_t>> pictures_;
    size_t at_ = 0;
    std::optional<EncoderOutput> output_;
    std::unique_ptr<platform::VideoDecoder> decoder_;
};

// ---- Encoding a run ----------------------------------------------------------------------------

// What the source of a run gives: its pictures, by number.
class Source {
public:
    Source(const Options& o, const std::string& content) : content_(content), path_(o.input) {
        if (content == "input") {
            recording_ = std::make_unique<Recording>(path_);
            const auto first = recording_->next();
            width_ = first.width;
            height_ = first.height;
        } else if (content != "typing" && content != "scrolling") {
            usage("Unknown content " + content);
        } else {
            width_ = o.width;
            height_ = o.height;
        }
    }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    // Picture n, in order from 0 on (a recording starts again from its first for 0).
    VideoFrame picture(uint32_t n) {
        if (!recording_) {
            return content_ == "typing" ? typing(width_, height_, n) : scrolling(width_, height_, n);
        }
        if (!n) {
            recording_ = std::make_unique<Recording>(path_);
        }
        auto frame = recording_->next();
        frame.width = width_;
        frame.height = height_;
        return frame;
    }

private:
    std::string content_, path_;
    std::unique_ptr<Recording> recording_;
    uint32_t width_ = 0, height_ = 0;
};

// A viewer: decodes what it gets and compares it with the pictures it
// stands for, as made ("coded") and as shown (until the next one decodes).
struct Viewer {
    std::unique_ptr<platform::VideoDecoder> decoder;
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> waiting; // Sources not shown yet: (sequence, luma).
    std::vector<uint8_t> shown;
    double codedError = 0, shownError = 0;
    uint64_t coded = 0, shownCount = 0, errors = 0;
    uint32_t width = 0, height = 0;

    void source(uint64_t sequence, const std::vector<uint8_t>& y) { waiting.push_back({sequence, y}); }

    void take(const EncodedPacket& packet) {
        std::atomic<bool> cancel{false};
        VideoFrame frame;
        try {
            if (!decoder->decode(packet, frame, cancel)) {
                return;
            }
        } catch (const Failure&) {
            ++errors;
            return;
        }
        const uint64_t sequence = frame.sequence ? frame.sequence : packet.sequence;
        std::vector<uint8_t> picture;
        withImage(frame, [&](const uint8_t* bgra, size_t stride) { picture = luma(bgra, stride, width, height); });
        while (!waiting.empty() && waiting.front().first < sequence) {
            show(waiting.front().second);
            waiting.pop_front();
        }
        shown = std::move(picture);
        if (!waiting.empty() && waiting.front().first == sequence) {
            codedError += mse(waiting.front().second, shown);
            ++coded;
            show(waiting.front().second);
            waiting.pop_front();
        }
    }
    void finish() {
        for (const auto& [sequence, y] : waiting) {
            show(y);
        }
        waiting.clear();
    }
    void show(const std::vector<uint8_t>& source) {
        if (!shown.empty()) {
            shownError += mse(source, shown);
            ++shownCount;
        }
    }
    double codedPsnr() const { return coded ? psnr(codedError / double(coded)) : 0; }
    double shownPsnr() const { return shownCount ? psnr(shownError / double(shownCount)) : 0; }
};

struct RunResult {
    uint32_t layers = 1;           // What the encoder made.
    double kbps = 0, baseKbps = 0; // All of it, and layer 0's (keyframes in both).
    uint64_t pictures = 0, keyframes = 0, bytes = 0, keyframeBytes = 0, baseBytes = 0, discardable = 0;
    std::array<uint64_t, 3> perLayer{};
    double coded = 0, shown = 0;              // PSNR of the viewer of every layer...
    double baseShown = 0;                     // ...and of one of layer 0 alone,
    uint64_t baseErrors = 0, baseDecoded = 0; // its decoder errors and pictures.
    uint64_t errors = 0;
};

std::unique_ptr<platform::VideoDecoder> decoderOf(rcs_codec codec, const Picture& picture) {
    platform::DecoderConfig config;
    config.codec = codec;
    config.cpuImages = true;
    config.picture = picture;
    config.acceleration = RCS_ACCELERATION_SOFTWARE;
    for (const auto& i : Recording::implementations()) {
        if (i.codec == codec && i.direction == RCS_CODEC_DECODER) {
            config.acceleration = i.acceleration;
            break;
        }
    }
    auto decoder = codecs::createDecoder(config, Recording::implementations());
    decoder->open(config);
    return decoder;
}

// Encodes `frames` pictures of `source` (every `step`-th: a lower frame rate)
// at `bitrate`, with `layers`, and has a viewer of all of it and one of layer 0 decode them.
RunResult encodeRun(const platform::CodecImplementation& impl, Source& source, const Options& o, uint32_t bitrate,
                    uint32_t layers, uint32_t step) {
    platform::EncoderConfig config;
    config.codec = impl.codec;
    config.width = source.width();
    config.height = source.height();
    config.framesPerSecond = o.fps / step;
    config.bitrate = bitrate;
    config.acceleration = impl.acceleration;
    config.backend = impl.backend;
    config.device = deviceOn(impl.adapterLuid);
    config.picture = picture(RCS_PICTURE_420_8, RCS_MATRIX_BT709, RCS_RANGE_LIMITED);
    config.temporalLayers = layers;
    auto encoder = codecs::createEncoder(config, Recording::implementations());
    encoder->open(config);
    RunResult r;
    r.layers = encoder->temporalLayers();
    Viewer all, base;
    for (Viewer* v : {&all, &base}) {
        v->decoder = decoderOf(impl.codec, config.picture);
        v->width = config.width;
        v->height = config.height;
    }
    const auto take = [&](EncodedPacket& packet) {
        packet.picture = config.picture;
        readTemporalLayer(packet); // What the bitstream says, as the SDK takes it.
        ++r.pictures;
        r.bytes += packet.data.size();
        r.keyframes += packet.keyframe;
        r.keyframeBytes += packet.keyframe ? packet.data.size() : 0;
        r.discardable += packet.discardable;
        ++r.perLayer[std::min<uint32_t>(packet.temporalId, 2)];
        all.take(packet);
        if (packet.temporalId == 0) {
            r.baseBytes += packet.data.size();
            base.take(packet);
        }
    };
    for (uint32_t n = 0; n < o.frames; ++n) {
        auto frame = source.picture(n);
        frame.sequence = n + 1;
        frame.timestampNs = uint64_t(n) * 1'000'000'000 / o.fps;
        withImage(frame, [&](const uint8_t* bgra, size_t stride) {
            const auto y = luma(bgra, stride, frame.width, frame.height);
            all.source(frame.sequence, y);
            base.source(frame.sequence, y);
        });
        if (n % step) {
            continue;
        }
        encoder->submit(frame);
        for (EncodedPacket packet; encoder->receive(packet, encoder->inFlight() >= 3 ? 1s : 0ms);) {
            take(packet);
        }
    }
    for (EncodedPacket packet; encoder->inFlight() && encoder->receive(packet, 2s);) {
        take(packet);
    }
    all.finish();
    base.finish();
    const double seconds = double(o.frames) / o.fps;
    r.kbps = double(r.bytes) * 8 / seconds / 1000;
    r.baseKbps = double(r.baseBytes) * 8 / seconds / 1000;
    r.coded = all.codedPsnr();
    r.shown = all.shownPsnr();
    r.errors = all.errors;
    r.baseShown = base.shownPsnr();
    r.baseErrors = base.errors;
    r.baseDecoded = base.coded;
    return r;
}

// ---- A second session --------------------------------------------------------------------------

struct Timing {
    double p50 = 0, p95 = 0;
    uint64_t pictures = 0;
    bool lowLatencyChanged = false;
};

Timing percentiles(std::vector<uint64_t> ns) {
    Timing t;
    t.pictures = ns.size();
    if (ns.empty()) {
        return t;
    }
    std::sort(ns.begin(), ns.end());
    t.p50 = double(ns[ns.size() / 2]) / 1e6;
    t.p95 = double(ns[std::min(ns.size() - 1, ns.size() * 95 / 100)]) / 1e6;
    return t;
}

// The first encoder's time per picture in real time, with a second of the
// same implementation at half the frame rate beside it or not; and the second's.
std::pair<Timing, Timing> sessions(const platform::CodecImplementation& impl, const std::vector<VideoFrame>& ring,
                                   const Options& o, uint32_t bitrate, bool second) {
    platform::EncoderConfig config;
    config.codec = impl.codec;
    config.width = ring.front().width;
    config.height = ring.front().height;
    config.framesPerSecond = o.fps;
    config.bitrate = bitrate;
    config.acceleration = impl.acceleration;
    config.backend = impl.backend;
    config.device = deviceOn(impl.adapterLuid);
    config.picture = picture(RCS_PICTURE_420_8, RCS_MATRIX_BT709, RCS_RANGE_LIMITED);
    auto main = codecs::createEncoder(config, Recording::implementations());
    main->open(config);
    const bool lowLatency = main->lowLatency();
    std::unique_ptr<platform::VideoEncoder> lower;
    if (second) {
        auto half = config;
        half.framesPerSecond = std::max(1u, o.fps / 2);
        half.bitrate = bitrate / 2;
        lower = codecs::createEncoder(half, Recording::implementations());
        lower->open(half);
    }
    // The lower session on a thread of its own, fed every other picture.
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<VideoFrame> offered;
    bool quit = false;
    std::vector<uint64_t> lowerNs;
    std::thread thread;
    if (lower) {
        thread = std::thread([&] {
            for (;;) {
                std::optional<VideoFrame> frame;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return quit || offered; });
                    if (quit) {
                        break;
                    }
                    frame = std::exchange(offered, std::nullopt);
                }
                lower->submit(*frame);
                for (EncodedPacket packet; lower->receive(packet, lower->inFlight() >= 3 ? 1s : 0ms);) {
                    lowerNs.push_back(packet.encodeNs);
                }
            }
        });
    }
    std::vector<uint64_t> mainNs;
    const auto interval = std::chrono::nanoseconds(1'000'000'000 / o.fps);
    const auto began = std::chrono::steady_clock::now();
    const uint32_t count = o.seconds * o.fps;
    for (uint32_t n = 0; n < count; ++n) {
        std::this_thread::sleep_until(began + interval * n);
        VideoFrame frame = ring[n % ring.size()];
        frame.sequence = n + 1;
        frame.timestampNs = monotonicNs();
        if (lower && n % 2 == 0) {
            std::lock_guard lock(mutex);
            offered = frame;
            wake.notify_one();
        }
        main->submit(frame);
        for (EncodedPacket packet; main->receive(packet, main->inFlight() >= 3 ? 1s : 0ms);) {
            mainNs.push_back(packet.encodeNs);
        }
    }
    for (EncodedPacket packet; main->inFlight() && main->receive(packet, 2s);) {
        mainNs.push_back(packet.encodeNs);
    }
    if (lower) {
        {
            std::lock_guard lock(mutex);
            quit = true;
        }
        wake.notify_one();
        thread.join();
    }
    auto timing = percentiles(mainNs);
    timing.lowLatencyChanged = main->lowLatency() != lowLatency;
    return {timing, percentiles(lowerNs)};
}

// ---- Output ------------------------------------------------------------------------------------

std::string implementationText(const platform::CodecImplementation& i) {
    return i.name + (i.acceleration == RCS_ACCELERATION_HARDWARE ? " (hardware)" : " (software)");
}

} // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    std::vector<platform::CodecImplementation> encoders;
    std::set<std::tuple<rcs_codec, rcs_acceleration, rcs_backend, uint64_t>> seen;
    for (const auto& i : Recording::implementations()) {
        if (i.direction != RCS_CODEC_ENCODER || !o.codecs.count(i.codec) ||
            (o.acceleration && i.acceleration != *o.acceleration) ||
            (!o.backend.empty() && i.name.find(o.backend) == std::string::npos) ||
            !seen.insert({i.codec, i.acceleration, i.backend, i.adapterLuid}).second) {
            continue;
        }
        encoders.push_back(i);
    }
    if (encoders.empty()) {
        usage("No encoder of this system matches");
    }
    std::FILE* csv = o.csv.empty() ? nullptr : std::fopen(o.csv.c_str(), "w");
    if (csv) {
        std::fprintf(csv, "encoder,content,target_kbps,layers,kbps,pictures,keyframes,psnr_coded,psnr_shown,"
                          "base_share,base_errors,base_pictures,base_psnr_shown,half_kbps,half_psnr_shown,"
                          "alone_p50_ms,alone_p95_ms,beside_p50_ms,beside_p95_ms,second_p50_ms,mode_changed\n");
    }
    std::printf("Encoders listed: ");
    for (const auto& i : encoders) {
        std::printf("%s [%s, temporal_layers %u]; ", implementationText(i).c_str(), codecName(i.codec),
                    i.temporalLayers);
    }
    std::printf("\n");
    for (const auto& impl : encoders) {
        const std::string name = implementationText(impl);
        try {
            if (o.layers) {
                for (const auto& content : o.contents) {
                    Source source(o, content);
                    std::printf("\n%s, %s %ux%u at %u FPS, %u pictures\n", name.c_str(), content.c_str(),
                                source.width(), source.height(), o.fps, o.frames);
                    std::printf("  %-9s %-6s %9s %8s %6s %7s %7s %7s %11s %9s | %9s %7s\n", "target", "layers",
                                "kbit/s", "pictures", "keys", "coded", "shown", "base%", "base alone", "base shown",
                                "half kbit", "shown");
                    for (const uint32_t bitrate : o.bitrates) {
                        RunResult one = encodeRun(impl, source, o, bitrate, 1, 1);
                        std::vector<RunResult> runs{one};
                        for (const uint32_t layers : {2u, 3u}) {
                            auto layered = encodeRun(impl, source, o, bitrate, layers, 1);
                            if (layered.layers == layers) {
                                runs.push_back(layered);
                            }
                        }
                        for (const auto& r : runs) {
                            const double share =
                                r.bytes > r.keyframeBytes
                                    ? 100.0 * double(r.baseBytes - std::min(r.baseBytes, r.keyframeBytes)) /
                                          double(r.bytes - r.keyframeBytes)
                                    : 100;
                            std::optional<RunResult> half;
                            if (r.layers == 2) { // A lower encoding at what layer 0 took.
                                half = encodeRun(impl, source, o, std::max(50'000u, uint32_t(r.baseKbps * 1000)), 1, 2);
                            }
                            char baseAlone[32] = "-", halfRate[16] = "-", halfShown[16] = "-";
                            if (r.layers > 1) {
                                std::snprintf(baseAlone, sizeof(baseAlone), "%llu err %llu pic",
                                              (unsigned long long)r.baseErrors, (unsigned long long)r.baseDecoded);
                            }
                            if (half) {
                                std::snprintf(halfRate, sizeof(halfRate), "%.0f", half->kbps);
                                std::snprintf(halfShown, sizeof(halfShown), "%.2f", half->shown);
                            }
                            std::printf("  %-9u L1T%-3u %9.0f %8llu %6llu %7.2f %7.2f %7.1f %11s %9.2f | %9s %7s\n",
                                        bitrate / 1000, r.layers, r.kbps, (unsigned long long)r.pictures,
                                        (unsigned long long)r.keyframes, r.coded, r.shown, r.layers > 1 ? share : 100.0,
                                        baseAlone, r.layers > 1 ? r.baseShown : r.shown, halfRate, halfShown);
                            if (r.errors) {
                                std::printf("    (%llu decoder errors with every layer)\n",
                                            (unsigned long long)r.errors);
                            }
                            if (csv) {
                                std::fprintf(
                                    csv, "\"%s\",%s,%u,%u,%.0f,%llu,%llu,%.2f,%.2f,%.1f,%llu,%llu,%.2f,%s,%s,,,,,,\n",
                                    name.c_str(), content.c_str(), bitrate / 1000, r.layers, r.kbps,
                                    (unsigned long long)r.pictures, (unsigned long long)r.keyframes, r.coded, r.shown,
                                    r.layers > 1 ? share : 100.0, (unsigned long long)r.baseErrors,
                                    (unsigned long long)r.baseDecoded, r.layers > 1 ? r.baseShown : r.shown,
                                    half ? halfRate : "", half ? halfShown : "");
                            }
                        }
                    }
                }
            }
            if (o.sessions) {
                Source source(o, o.contents.front());
                std::vector<VideoFrame> ring;
                for (uint32_t n = 0; n < 30; ++n) {
                    ring.push_back(source.picture(n));
                }
                const uint32_t bitrate = o.bitrates.back();
                const auto [alone, unused] = sessions(impl, ring, o, bitrate, false);
                const auto [beside, lower] = sessions(impl, ring, o, bitrate, true);
                std::printf("\n%s, %ux%u at %u FPS, %u kbit/s: per picture %.2f / %.2f ms (p50 / p95) alone, "
                            "%.2f / %.2f ms with a second session at %u FPS (that one %.2f / %.2f ms)%s%s\n",
                            name.c_str(), ring.front().width, ring.front().height, o.fps, bitrate / 1000, alone.p50,
                            alone.p95, beside.p50, beside.p95, o.fps / 2, lower.p50, lower.p95,
                            alone.lowLatencyChanged ? "; alone it changed its mode" : "",
                            beside.lowLatencyChanged ? "; beside the second it changed its mode" : "");
                if (csv) {
                    std::fprintf(csv, "\"%s\",sessions,%u,1,,%llu,,,,,,,,,,%.2f,%.2f,%.2f,%.2f,%.2f,%d\n", name.c_str(),
                                 bitrate / 1000, (unsigned long long)beside.pictures, alone.p50, alone.p95, beside.p50,
                                 beside.p95, lower.p50, beside.lowLatencyChanged ? 1 : 0);
                }
            }
        } catch (const Failure& failure) {
            std::printf("\n%s: %s\n", name.c_str(), failure.what());
        }
        std::fflush(stdout);
    }
    if (csv) {
        std::fclose(csv);
    }
    return 0;
}
