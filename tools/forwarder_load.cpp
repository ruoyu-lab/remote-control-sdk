// The load a forwarder (server/forwarder: rcs_forwarder) carries: synthetic
// sharers and many light viewers, each over the forwarder's own hop (the
// SDK's UDP connection under the magic RCSFWD, over its UDP, TCP or WebSocket
// entry), measured at the viewers and on the forwarder's process
// (docs/PERFORMANCE.md). Not part of the SDK; it uses the SDK's internal
// headers.
//
//   rcsdk_forwarder_load run --forwarder HOST:PORT --secret ID:HEX (--identity FILE | --audience HEX)
//        [--tcp-port N] [--websocket-port N] [--entry udp|tcp|websocket|mix] [--sharer-entry udp|tcp|websocket]
//        [--rooms 1] [--viewers 5] [--fps 30] [--bitrate 4m] [--encodings 1|2]
//        [--keyframe-factor 6] [--keyframe-interval-ms 0] [--audio on|off]
//        [--sound-packets 100] [--sound-bytes 120] [--sharer-rate adaptive|fixed]
//        [--warmup 5] [--seconds 20] [--connect-parallel 8]
//        [--slow N --slow-bps 2m [--slow-loss PERMILLE] [--slow-delay MS]]
//        [--pid PID] [--bind ADDRESS] [--per-viewer] [--label TEXT]
//   rcsdk_forwarder_load monitor --pid PID [--interface NAME] [--seconds N]
//
// Each room has a sharer: a hop to the forwarder as the SDK's sharer makes
// one (ForwarderHello publish), which sends sealed pictures of the size the
// bitrate and frame rate make (a keyframe --keyframe-factor times the average
// at the start, when the forwarder asks for one, and every
// --keyframe-interval-ms; the others the average, +-25 %), and with --audio
// --sound-packets a second of --sound-bytes (100 of 120: Opus at about 96
// kbit/s in 10 ms packets, as WASAPI captures; a Mac's Core Audio tap
// captures 5 ms, ScreenCaptureKit 20 ms); with --encodings 2 a second
// encoding at a quarter of the bitrate, which the forwarder moves a viewer
// that falls behind down to. Its hop's bitrate is adaptive, as the SDK's
// sharer's is by default (from 4 Mbit/s, up to four times what it sends),
// and the pictures are kept within what that measures up to the forwarder,
// as the SDK keeps its encodings (--sharer-rate fixed: neither). It echoes
// each viewer's pipe datagrams, and at
// a viewer's first (which stands for its end-to-end handshake: the SDK's
// sharer subscribes a viewer once it was let in) subscribes it to stream 0
// of encoding 0 (and the sound).
//
// A viewer is a hop as the SDK's viewer makes one (ForwarderHello
// subscribe): it acknowledges, reports and asks for what is missing as every
// viewer's hop does, but it runs no end-to-end connection inside its pipe and
// neither opens nor decodes the pictures: each sealed picture carries its
// send time and number in clear (the forwarder never looks inside), which the
// viewer reads. That is what the forwarder sees of a real viewer, at a
// fraction of a real viewer's cost here (no decoding), so one machine loads
// a forwarder with hundreds. Every 250 ms a viewer sends a pipe datagram (as
// the end-to-end connection's pings) that the sharer echoes: the round trip
// of control and input through the forwarder.
//
// Measured over --seconds after --warmup: per viewer the pictures that came
// (delivered FPS); their latency, from the sharer handing a picture to its
// hop to the viewer's hop giving it whole (both hops' pacing and the
// forwarder; one clock, since sharers and viewers run here); pictures that
// never came (by number); the hop's repairs (resends, parity) and the
// pictures it gave up on; the sound's latency and losses; the pipe's round
// trip; the bytes the viewers' hops carried (the forwarder's egress to them
// and their few reports back). With --pid, the forwarder process's CPU (100 %
// = one core), resident memory and threads, and this process's CPU apart.
// Viewers made slow (--slow, UDP only) reach the forwarder through a shaping
// proxy of --slow-bps (queue 150 ms) and count apart from the others.
//
// monitor: the CPU, memory and threads of a process and (Linux) the bytes an
// interface sent and received and the UDP datagrams the system dropped for
// want of room in a socket's buffers, each second, with Unix time: run it beside a
// forwarder on another machine, and take the lines within the run's window
// (the run prints it).
#include "../server/forwarder/src/token.hpp"
#include "../tests/support/shaping_proxy.hpp"
#include "crypto/crypto.hpp"
#include "protocol/messages.hpp"
#include "protocol/udp_connection.hpp"
#include "transport/framed_socket.hpp"
#include "transport/websocket.hpp"
#include "util/bytes.hpp"

#include <rcsfwd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#endif
#ifdef __linux__
#include <unistd.h>
#endif

namespace {

using namespace rcsdk;
using namespace rcsdk::protocol;
using namespace std::chrono_literals;

[[noreturn]] void usage(const std::string& why = {}) {
    if (!why.empty()) {
        std::fprintf(stderr, "rcsdk_forwarder_load: %s\n", why.c_str());
    }
    std::fprintf(stderr,
                 "usage: rcsdk_forwarder_load run --forwarder HOST:PORT --secret ID:HEX (--identity FILE | --audience "
                 "HEX)\n"
                 "         [--tcp-port N] [--websocket-port N] [--entry udp|tcp|websocket|mix]\n"
                 "         [--sharer-entry udp|tcp|websocket] [--rooms 1] [--viewers 5] [--fps 30] [--bitrate 4m]\n"
                 "         [--encodings 1|2] [--keyframe-factor 6] [--keyframe-interval-ms 0] [--audio on|off]\n"
                 "         [--sound-packets 100] [--sound-bytes 120] [--sharer-rate adaptive|fixed]\n"
                 "         [--warmup 5] [--seconds 20] [--connect-parallel 8]\n"
                 "         [--slow N --slow-bps 2m [--slow-loss PERMILLE] [--slow-delay MS]]\n"
                 "         [--pid PID] [--bind ADDRESS] [--per-viewer] [--label TEXT]\n"
                 "       rcsdk_forwarder_load monitor --pid PID [--interface NAME] [--seconds N]\n");
    std::exit(2);
}

// 4000000, 4000k, 4m, 1.5g.
uint64_t amount(const std::string& text) {
    size_t used = 0;
    double value = 0;
    try {
        value = std::stod(text, &used);
    } catch (...) {
        usage("not a number: " + text);
    }
    const std::string unit = text.substr(used);
    const double scale = unit.empty() ? 1 : unit == "k" ? 1e3 : unit == "m" ? 1e6 : unit == "g" ? 1e9 : -1;
    if (scale < 0 || value < 0) {
        usage("not a number: " + text);
    }
    return uint64_t(std::llround(value * scale));
}

std::vector<uint8_t> unhex(const std::string& text, size_t size, const std::string& what) {
    if (text.size() != size * 2 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        usage(what + ": " + std::to_string(size * 2) + " hex digits");
    }
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = uint8_t(std::stoul(text.substr(i * 2, 2), nullptr, 16));
    }
    return bytes;
}

uint64_t nowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

double unixNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The value at `fraction` of `values` (sorted here), in the unit given; 0 for none.
double percentile(std::vector<uint32_t>& values, double fraction, double unit = 1) {
    if (values.empty()) {
        return 0;
    }
    const size_t at = std::min(values.size() - 1, size_t(fraction * double(values.size())));
    std::nth_element(values.begin(), values.begin() + ptrdiff_t(at), values.end());
    return values[at] / unit;
}

// ---- A process's CPU, memory and threads; an interface's bytes -------------------------------

struct ProcessSample {
    bool ok = false;
    double cpuSeconds = 0; // User and system, since it started.
    uint64_t rss = 0;
    uint32_t threads = 0;
};

ProcessSample sampleProcess(int pid) {
#ifdef __APPLE__
    proc_taskinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, sizeof(info)) != int(sizeof(info))) {
        return {};
    }
    // In Mach time units on Apple silicon.
    static const double scale = [] {
        mach_timebase_info_data_t base{};
        mach_timebase_info(&base);
        return double(base.numer) / double(base.denom);
    }();
    return {true, double(info.pti_total_user + info.pti_total_system) * scale / 1e9, info.pti_resident_size,
            uint32_t(info.pti_threadnum)};
#elif defined(__linux__)
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto close = text.rfind(')');
    if (close == std::string::npos) {
        return {};
    }
    std::istringstream fields(text.substr(close + 2));
    std::vector<std::string> f;
    for (std::string field; fields >> field;) {
        f.push_back(field);
    }
    if (f.size() < 22) {
        return {};
    }
    // From the state (field 3): utime 14, stime 15, num_threads 20, rss 24.
    const double ticks = double(sysconf(_SC_CLK_TCK));
    return {true, double(std::stoull(f[11]) + std::stoull(f[12])) / ticks,
            std::stoull(f[21]) * uint64_t(sysconf(_SC_PAGESIZE)), uint32_t(std::stoul(f[17]))};
#else
    (void)pid;
    return {};
#endif
}

double selfCpuSeconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto seconds = [](const timeval& t) {
        return double(t.tv_sec) + double(t.tv_usec) / 1e6;
    };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

struct NetSample {
    bool ok = false;
    uint64_t rxBytes = 0, rxPackets = 0, txBytes = 0, txPackets = 0;
    uint64_t receiveErrors = 0, bufferErrors = 0; // UDP, the whole system: RcvbufErrors, SndbufErrors.
};

// Linux: the system's UDP counters of datagrams dropped for want of room in a socket's buffers (/proc/net/snmp).
void sampleUdpErrors(NetSample& s) {
    std::ifstream in("/proc/net/snmp");
    std::vector<std::string> names;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("Udp: ", 0) != 0) {
            continue;
        }
        std::istringstream fields(line.substr(5));
        std::vector<std::string> values;
        for (std::string field; fields >> field;) {
            values.push_back(field);
        }
        if (names.empty()) {
            names = values;
            continue;
        }
        for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
            if (names[i] == "RcvbufErrors") {
                s.receiveErrors = std::stoull(values[i]);
            } else if (names[i] == "SndbufErrors") {
                s.bufferErrors = std::stoull(values[i]);
            }
        }
    }
}

// Linux: what `name` received and sent so far (/proc/net/dev).
NetSample sampleInterface(const std::string& name) {
    NetSample s;
    std::ifstream in("/proc/net/dev");
    for (std::string line; std::getline(in, line);) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string device = line.substr(0, colon);
        device.erase(0, device.find_first_not_of(' '));
        if (device != name) {
            continue;
        }
        std::istringstream fields(line.substr(colon + 1));
        uint64_t v[16] = {};
        for (auto& value : v) {
            fields >> value;
        }
        s.ok = true;
        s.rxBytes = v[0];
        s.rxPackets = v[1];
        s.txBytes = v[8];
        s.txPackets = v[9];
    }
    sampleUdpErrors(s);
    return s;
}

// ---- Options ---------------------------------------------------------------------------------

enum class Way { udp, tcp, websocket };

const char* nameOf(Way way) {
    return way == Way::tcp ? "tcp" : way == Way::websocket ? "websocket" : "udp";
}

Way wayOf(const std::string& text) {
    if (text == "udp") {
        return Way::udp;
    }
    if (text == "tcp") {
        return Way::tcp;
    }
    if (text == "websocket" || text == "ws") {
        return Way::websocket;
    }
    usage("no such entry: " + text);
}

struct Options {
    std::string host = "127.0.0.1", bind;
    uint16_t udpPort = 0, tcpPort = 0, webSocketPort = 0;
    rcsfwd_secret secret{};
    bool haveSecret = false;
    std::array<uint8_t, 32> audience{};
    bool haveAudience = false;
    std::optional<crypto::PublicKey> forwarderKey; // With --identity: pinned as the ends pin it.
    std::string entry = "udp";
    Way sharerWay = Way::udp;
    uint32_t rooms = 1, viewers = 5, fps = 30, encodings = 1;
    uint64_t bitrate = 4'000'000;
    double keyframeFactor = 6;
    uint32_t keyframeIntervalMs = 0;
    bool audio = true;
    uint32_t audioPerSecond = 100, audioBytes = 120;
    uint32_t warmup = 5, seconds = 20, connectParallel = 8;
    uint32_t slow = 0, slowLoss = 0, slowDelay = 0;
    uint64_t slowBps = 2'000'000;
    int pid = 0;
    bool perViewer = false;
    std::string label;

    bool sharerAdaptive = true;

    uint64_t bitrateOf(uint8_t encoding) const { return encoding ? bitrate / 4 : bitrate; }
    uint64_t totalBitrate() const { return bitrate + (encodings > 1 ? bitrate / 4 : 0); }
    Way viewerWay(uint32_t index) const {
        if (entry == "mix") {
            return index % 3 == 0 ? Way::udp : index % 3 == 1 ? Way::tcp : Way::websocket;
        }
        return wayOf(entry);
    }
    uint16_t portOf(Way way) const {
        const uint16_t port = way == Way::tcp ? tcpPort : way == Way::websocket ? webSocketPort : udpPort;
        if (!port) {
            usage(std::string("no port for the ") + nameOf(way) + " entry (--forwarder, --tcp-port, --websocket-port)");
        }
        return port;
    }
};

// ---- Hops ------------------------------------------------------------------------------------

// A WebSocket's opening over `stream`: what came after the answer.
Bytes openWebSocket(transport::ByteStream& stream, const std::string& host) {
    static const std::atomic<bool> never{false};
    const auto opening = transport::websocket::open(host, "/rcs");
    std::span<const uint8_t> rest(reinterpret_cast<const uint8_t*>(opening.request.data()), opening.request.size());
    const auto until = Clock::now() + 5s;
    while (!rest.empty()) {
        rest = rest.subspan(stream.writeSome(rest));
        if (Clock::now() > until) {
            fail(RCS_ERROR_NETWORK, "The WebSocket opening did not go out");
        }
    }
    Bytes answer;
    uint8_t chunk[4096];
    while (Clock::now() < until) {
        if (const size_t taken = stream.readSome(chunk)) {
            answer.insert(answer.end(), chunk, chunk + taken);
            if (const auto consumed = transport::websocket::opened(answer, opening.key)) {
                return Bytes(answer.begin() + ptrdiff_t(*consumed), answer.end());
            }
        } else {
            try {
                transport::waitReady(stream.handle(), false, never, Clock::now() + 50ms);
            } catch (const Failure&) {
            }
        }
    }
    fail(RCS_ERROR_NETWORK, "The forwarder did not answer the WebSocket opening");
}

// A hop to the forwarder's `way` entry (UDP: by way of `via`, where given), its handshake done.
std::unique_ptr<UdpConnection> connectHop(const Options& o, Way way, bool sendsVideo,
                                          std::optional<transport::Endpoint> via, CancelToken cancelled) {
    transport::Credentials credentials;
    credentials.secret = RCS_SECRET_NONE;
    credentials.forwarder = true;
    if (o.forwarderKey) {
        credentials.trusted = {*o.forwarderKey};
    }
    UdpOptions options;
    options.handshakeTimeout = 5s;
    if (sendsVideo) { // As the SDK's sharer makes it (session/forwarder.cpp): up to four times what it sends.
        options.videoFrom = transport::Side::client;
        const auto most = uint32_t(std::min<uint64_t>(4 * o.totalBitrate(), 1'000'000'000));
        options.rate =
            o.sharerAdaptive
                ? Connection::VideoRate{true, 300'000, most, uint32_t(std::min<uint64_t>(4'000'000, most)), {}, {}}
                : Connection::VideoRate{false, most, most, most, {}, {}};
    }
    const auto server = transport::Endpoint::parse(o.host, o.portOf(way));
    const auto to = via.value_or(server);
    std::unique_ptr<transport::DatagramSocket> socket;
    if (way == Way::udp) {
        socket = std::make_unique<transport::UdpSocket>(transport::Endpoint::parse(o.bind, 0));
    } else {
        auto stream =
            transport::tcpBytes(transport::connectStreamTcp(to, "the forwarder", cancelled, 5s, 0), "the forwarder");
        const auto arrived = way == Way::websocket ? openWebSocket(*stream, o.host) : Bytes{};
        socket = std::make_unique<transport::FramedSocket>(
            std::move(stream), to, way == Way::tcp ? transport::lengthFraming() : transport::webSocketFraming(),
            arrived);
    }
    return UdpConnection::establish(UdpConnection::Side::client, std::move(socket), directLink(to), credentials,
                                    options, cancelled);
}

std::string token(const Options& o, rcsfwd_role role, uint32_t room, uint64_t route) {
    return forwarder::sign(
        o.secret, {"load-" + std::to_string(room), role, route, forwarder::unixNow() + 24 * 3600, 0, o.audience});
}

// Its welcome, or why not.
ForwarderWelcome enter(UdpConnection& hop, ForwarderHello hello) {
    hop.send(hello);
    auto first = hop.receive();
    if (const auto* welcome = std::get_if<ForwarderWelcome>(&first)) {
        return *welcome;
    }
    if (const auto* refused = std::get_if<Refused>(&first)) {
        fail(refused->result, "The forwarder refused: " + refused->reason);
    }
    fail(RCS_ERROR_PROTOCOL,
         "The forwarder's first answer was not its welcome (message " + std::to_string(first.index()) + ")");
}

// ---- What is measured ------------------------------------------------------------------------

std::atomic<bool> measuring{false};

// The pictures of the last second, for the line printed each second: the others (not slow) only.
struct Second {
    std::mutex mutex;
    std::vector<uint32_t> latencyUs;
};
Second second;

struct Measured {
    uint64_t frames = 0, mediaBytes = 0, lost = 0, gaps = 0, missing = 0;
    uint64_t sounds = 0, soundsLost = 0, pipes = 0;
    std::vector<uint32_t> latencyUs, soundUs, roundTripUs;
};

// ---- A sharer --------------------------------------------------------------------------------

class Sharer {
public:
    Sharer(const Options& o, uint32_t room, CancelToken cancelled)
        : o_(o), room_(room), hop_(connectHop(o, o.sharerWay, true, std::nullopt, cancelled)) {
        enter(*hop_, {ForwarderHello::publish, token(o, RCSFWD_ROLE_PUBLISH, room, 0)});
        keyframeWanted_ = (1u << o.encodings) - 1;
        reader_ = std::thread([this] { read(); });
        sender_ = std::thread([this] { send(); });
    }
    ~Sharer() {
        stop_ = true;
        sender_.join();
        hop_->cancel();
        reader_.join();
    }
    std::atomic<uint64_t> frames{0}, bytes{0}, keyframes{0}, asked{0}, moves{0}, echoed{0}, joined{0}, left{0},
        sounds{0}, reports{0};
    std::string failure() const {
        std::lock_guard lock(mutex_);
        return failure_;
    }
    // The forwarder's latest report of `peer`'s path.
    std::optional<PathReport::Peer> path(uint64_t peer) const {
        std::lock_guard lock(mutex_);
        const auto found = paths_.find(peer);
        return found == paths_.end() ? std::nullopt : std::optional(found->second);
    }

private:
    void read() noexcept {
        try {
            for (;;) {
                auto message = hop_->receive();
                if (const auto* joinedPeer = std::get_if<PeerJoined>(&message)) {
                    ++joined;
                    std::lock_guard lock(mutex_);
                    waiting_.insert(joinedPeer->peer);
                } else if (const auto* need = std::get_if<NeedKeyframe>(&message)) {
                    ++asked;
                    if (need->stream == 0 && need->encoding < o_.encodings) {
                        keyframeWanted_ |= 1u << need->encoding;
                    }
                } else if (auto* pipe = std::get_if<Pipe>(&message)) {
                    // A viewer's first pipe datagram stands for its end-to-end handshake: as the
                    // SDK's sharer, it subscribes the viewer once that is done.
                    bool first = false;
                    {
                        std::lock_guard lock(mutex_);
                        first = waiting_.erase(pipe->peer) > 0;
                    }
                    if (first) {
                        hop_->send(Subscribe{pipe->peer, 1, 0, uint8_t(o_.audio ? 1 : 0), 0, 0});
                    }
                    ++echoed;
                    hop_->send(Pipe{pipe->peer, std::move(pipe->bytes)});
                } else if (const auto* report = std::get_if<PathReport>(&message)) {
                    ++reports;
                    std::lock_guard lock(mutex_);
                    for (const auto& peer : report->peers) {
                        paths_[peer.peer] = peer;
                    }
                } else if (std::holds_alternative<PeerMoved>(message)) {
                    ++moves;
                } else if (std::holds_alternative<PeerLeft>(message)) {
                    ++left;
                } else if (const auto* refused = std::get_if<Refused>(&message)) {
                    fail(refused->result, refused->reason);
                }
            }
        } catch (const Failure& failure) {
            if (failure.result() != RCS_ERROR_CANCELLED) {
                std::lock_guard lock(mutex_);
                failure_ = failure.what();
            }
        }
    }

    void send() noexcept {
        std::minstd_rand random(room_ + 1);
        std::uniform_real_distribution<double> spread(0.75, 1.25);
        const auto frameEvery = std::chrono::duration<double>(1.0 / o_.fps);
        const auto soundEvery = std::chrono::duration<double>(1.0 / o_.audioPerSecond);
        auto nextFrame = Clock::now(), nextSound = nextFrame;
        uint64_t counter = 0, frameIndex = 0, soundIndex = 0;
        std::array<uint64_t, 2> sequence{}; // Of each encoding.
        std::array<Clock::time_point, 2> keyframeAt{};
        try {
            while (!stop_) {
                const auto due = o_.audio ? std::min(nextFrame, nextSound) : nextFrame;
                std::this_thread::sleep_until(due);
                const auto now = Clock::now();
                if (now >= nextFrame) {
                    // Adaptive: the encodings kept within what the hop measures up to the forwarder.
                    const uint32_t target = o_.sharerAdaptive ? hop_->status().targetBitrate : 0;
                    const double scale = target ? std::min(1.0, double(target) / double(o_.totalBitrate())) : 1.0;
                    for (uint8_t e = 0; e < o_.encodings; ++e) {
                        const double average = double(o_.bitrateOf(e)) * scale / 8 / o_.fps;
                        const bool keyframe = (keyframeWanted_.fetch_and(~(1u << e)) & (1u << e)) ||
                                              (o_.keyframeIntervalMs &&
                                               now - keyframeAt[e] >= std::chrono::milliseconds(o_.keyframeIntervalMs));
                        if (keyframe) {
                            keyframeAt[e] = now;
                            ++keyframes;
                        }
                        const size_t size =
                            std::max<size_t>(64, size_t(average * (keyframe ? o_.keyframeFactor : spread(random))));
                        Bytes sealed(size);
                        storeBigEndian(sealed, 8, ++sequence[e], 8);
                        storeBigEndian(sealed, 0, nowNs(), 8);
                        hop_->send(SealedVideo{1, 0, ++counter, 0, e, 0,
                                               uint8_t(keyframe ? SealedVideo::keyframeFlag : 0), std::move(sealed)});
                        ++frames;
                        bytes += size;
                    }
                    ++frameIndex;
                    nextFrame += std::chrono::duration_cast<Clock::duration>(frameEvery);
                    if (now - nextFrame > 1s) { // Far behind: from now on.
                        nextFrame = now;
                    }
                }
                if (o_.audio && now >= nextSound) {
                    Bytes sealed(o_.audioBytes);
                    storeBigEndian(sealed, 8, ++soundIndex, 8);
                    storeBigEndian(sealed, 0, nowNs(), 8);
                    hop_->send(SealedAudio{1, 0, ++counter, 1, std::move(sealed)});
                    ++sounds;
                    nextSound += std::chrono::duration_cast<Clock::duration>(soundEvery);
                    if (now - nextSound > 1s) {
                        nextSound = now;
                    }
                }
            }
        } catch (const Failure& failure) {
            std::lock_guard lock(mutex_);
            failure_ = failure.what();
        }
    }

    const Options& o_;
    const uint32_t room_;
    std::unique_ptr<UdpConnection> hop_;
    std::atomic<uint32_t> keyframeWanted_{0};
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::string failure_;
    std::map<uint64_t, PathReport::Peer> paths_;
    std::set<uint64_t> waiting_; // Viewers that came, not subscribed yet.
    std::thread reader_, sender_;
};

// ---- A viewer --------------------------------------------------------------------------------

class Viewer {
public:
    Viewer(const Options& o, uint32_t room, uint32_t index, bool slow, CancelToken cancelled)
        : room(room), index(index), way(o.viewerWay(index)), slow(slow) {
        std::optional<transport::Endpoint> via;
        if (slow) {
            // The viewer reaches it on loopback; it reaches the forwarder as the others do.
            proxy_ = std::make_unique<test::ShapingProxy>(transport::Endpoint::parse(o.host, o.portOf(Way::udp)),
                                                          "127.0.0.1", 0, o.bind);
            proxy_->bitsPerSecond = o.slowBps;
            proxy_->lossPermille = o.slowLoss;
            proxy_->delayMs = o.slowDelay;
            via = proxy_->address();
        }
        hop_ = connectHop(o, slow ? Way::udp : way, false, via, cancelled);
        peer = enter(*hop_, {ForwarderHello::subscribe, token(o, RCSFWD_ROLE_SUBSCRIBE, room, index + 1)}).peer;
        reader_ = std::thread([this] { read(); });
        ping(); // Its "handshake": the sharer subscribes it then.
    }
    ~Viewer() {
        hop_->cancel();
        reader_.join();
    }

    const uint32_t room, index;
    const Way way;
    const bool slow;
    uint64_t peer = 0;
    std::atomic<uint64_t> framesSoFar{0};

    UdpConnection& hop() { return *hop_; }
    void ping() {
        Bytes stamp(16);
        storeBigEndian(stamp, 0, nowNs(), 8);
        try {
            hop_->send(Pipe{0, std::move(stamp)});
        } catch (const Failure&) {
        }
    }
    Measured measured() {
        std::lock_guard lock(mutex_);
        return measured_;
    }
    std::string failure() {
        std::lock_guard lock(mutex_);
        return failure_;
    }

private:
    void read() noexcept {
        uint64_t lastSequence = 0, lastSound = 0;
        int lastEncoding = -1;
        try {
            for (;;) {
                const auto message = hop_->receive();
                const uint64_t now = nowNs();
                const bool counting = measuring.load(std::memory_order_relaxed);
                std::lock_guard lock(mutex_);
                auto& m = measured_;
                if (const auto* video = std::get_if<SealedVideo>(&message)) {
                    const uint64_t sent = loadBigEndian(video->sealed, 0, 8);
                    const uint64_t sequence = loadBigEndian(video->sealed, 8, 8);
                    const auto latency = uint32_t(std::min<uint64_t>((now - sent) / 1000, UINT32_MAX));
                    if (counting) {
                        ++m.frames;
                        m.mediaBytes += video->sealed.size();
                        m.latencyUs.push_back(latency);
                        if (lastEncoding == video->encoding && sequence > lastSequence + 1) {
                            m.lost += sequence - lastSequence - 1;
                        }
                        if (!slow) {
                            std::lock_guard bucket(second.mutex);
                            second.latencyUs.push_back(latency);
                        }
                    }
                    ++framesSoFar;
                    lastSequence = sequence;
                    lastEncoding = video->encoding;
                } else if (const auto* sound = std::get_if<SealedAudio>(&message)) {
                    const uint64_t sent = loadBigEndian(sound->sealed, 0, 8);
                    const uint64_t number = loadBigEndian(sound->sealed, 8, 8);
                    if (counting) {
                        ++m.sounds;
                        m.mediaBytes += sound->sealed.size();
                        m.soundUs.push_back(uint32_t(std::min<uint64_t>((now - sent) / 1000, UINT32_MAX)));
                        if (lastSound && number > lastSound + 1) {
                            m.soundsLost += number - lastSound - 1;
                        }
                    }
                    lastSound = std::max(lastSound, number);
                } else if (const auto* pipe = std::get_if<Pipe>(&message)) {
                    if (counting && pipe->bytes.size() >= 8) {
                        ++m.pipes;
                        m.roundTripUs.push_back(
                            uint32_t(std::min<uint64_t>((now - loadBigEndian(pipe->bytes, 0, 8)) / 1000, UINT32_MAX)));
                    }
                } else if (std::holds_alternative<StreamGap>(message)) {
                    m.gaps += counting;
                    lastEncoding = -1; // Its stream starts again at a keyframe.
                } else if (std::holds_alternative<VideoMissing>(message)) {
                    m.missing += counting;
                } else if (const auto* refused = std::get_if<Refused>(&message)) {
                    failure_ = "refused: " + refused->reason;
                    return;
                }
            }
        } catch (const Failure& failure) {
            if (failure.result() != RCS_ERROR_CANCELLED) {
                std::lock_guard lock(mutex_);
                failure_ = failure.what();
            }
        }
    }

    std::unique_ptr<test::ShapingProxy> proxy_;
    std::unique_ptr<UdpConnection> hop_;
    std::mutex mutex_;
    Measured measured_;
    std::string failure_;
    std::thread reader_; // Last.
};

// ---- run -------------------------------------------------------------------------------------

struct Group {
    uint32_t viewers = 0, ended = 0;
    double fpsMin = 1e9, fpsSum = 0;
    uint64_t frames = 0, lost = 0, lostMost = 0, gaps = 0, missing = 0, sounds = 0, soundsLost = 0, pipes = 0;
    uint64_t resent = 0, parity = 0, hopLost = 0;
    std::vector<uint32_t> latencyUs, soundUs, roundTripUs;

    void add(Measured m, const UdpConnection::PathStatus& before, const UdpConnection::PathStatus& after,
             double seconds, bool failed) {
        ++viewers;
        ended += failed;
        const double fps = double(m.frames) / seconds;
        fpsMin = std::min(fpsMin, fps);
        fpsSum += fps;
        frames += m.frames;
        lost += m.lost;
        lostMost = std::max(lostMost, m.lost);
        gaps += m.gaps;
        missing += m.missing;
        sounds += m.sounds;
        soundsLost += m.soundsLost;
        pipes += m.pipes;
        resent += after.resendRecovered - before.resendRecovered;
        parity += after.fecRecovered - before.fecRecovered;
        hopLost += after.framesLost - before.framesLost;
        latencyUs.insert(latencyUs.end(), m.latencyUs.begin(), m.latencyUs.end());
        soundUs.insert(soundUs.end(), m.soundUs.begin(), m.soundUs.end());
        roundTripUs.insert(roundTripUs.end(), m.roundTripUs.begin(), m.roundTripUs.end());
    }
    void print(const char* name, FILE* out) {
        if (!viewers) {
            return;
        }
        std::fprintf(
            out,
            "%s: %u viewers (%u ended), FPS mean %.1f min %.1f, picture latency p50 %.1f p95 %.1f p99 %.1f "
            "max %.1f ms, pictures lost %" PRIu64 " (most of one viewer %" PRIu64 "), gaps %" PRIu64
            ", repaired by resends %" PRIu64 " by parity %" PRIu64 "; sound p50 %.1f p95 %.1f ms, lost %" PRIu64
            " of %" PRIu64 "; pipe round trip p50 %.1f p95 %.1f max %.1f ms (%" PRIu64 ")\n",
            name, viewers, ended, fpsSum / viewers, fpsMin, percentile(latencyUs, 0.5, 1000),
            percentile(latencyUs, 0.95, 1000), percentile(latencyUs, 0.99, 1000), percentile(latencyUs, 1.0, 1000),
            lost, lostMost, gaps, resent, parity, percentile(soundUs, 0.5, 1000), percentile(soundUs, 0.95, 1000),
            soundsLost, sounds + soundsLost, percentile(roundTripUs, 0.5, 1000), percentile(roundTripUs, 0.95, 1000),
            percentile(roundTripUs, 1.0, 1000), pipes);
    }
};

int run(const Options& o) {
    std::atomic<bool> cancelled{false};
    std::printf("load: %u room(s) x %u viewer(s) over %s, sharers over %s, %u FPS, %.2f Mbit/s%s, %u encoding(s), "
                "sound %s%s\n",
                o.rooms, o.viewers, o.entry.c_str(), nameOf(o.sharerWay), o.fps, double(o.bitrate) / 1e6,
                o.keyframeIntervalMs ? (", keyframe every " + std::to_string(o.keyframeIntervalMs) + " ms").c_str()
                                     : "",
                o.encodings, o.audio ? "on" : "off",
                o.slow ? (", " + std::to_string(o.slow) + " slow viewer(s) in room 0 at " +
                          std::to_string(double(o.slowBps) / 1e6) + " Mbit/s")
                             .c_str()
                       : "");
    std::fflush(stdout);
    const double startCpu = selfCpuSeconds();
    const auto connectStart = Clock::now();
    std::vector<std::unique_ptr<Sharer>> sharers;
    for (uint32_t r = 0; r < o.rooms; ++r) {
        sharers.push_back(std::make_unique<Sharer>(o, r, cancelled));
    }
    // The viewers, a few at a time (the forwarder takes 32 that did not send their token yet).
    const uint32_t total = o.rooms * o.viewers;
    std::vector<std::unique_ptr<Viewer>> viewers(total);
    std::atomic<uint32_t> next{0}, refused{0};
    std::mutex errorMutex;
    std::string firstError;
    {
        std::vector<std::thread> connecting;
        for (uint32_t t = 0; t < std::min(o.connectParallel, total); ++t) {
            connecting.emplace_back([&] {
                for (uint32_t i; (i = next++) < total;) {
                    const uint32_t room = i % o.rooms, index = i / o.rooms;
                    try {
                        viewers[i] = std::make_unique<Viewer>(o, room, index, room == 0 && index < o.slow, cancelled);
                    } catch (const Failure& failure) {
                        ++refused;
                        std::lock_guard lock(errorMutex);
                        if (firstError.empty()) {
                            firstError = failure.what();
                        }
                    }
                }
            });
        }
        for (auto& thread : connecting) {
            thread.join();
        }
    }
    std::erase(viewers, nullptr);
    std::printf("connected: %u sharer(s), %zu viewer(s) in %.1f s (%.2f s of this process's CPU)%s%s\n", o.rooms,
                viewers.size(), std::chrono::duration<double>(Clock::now() - connectStart).count(),
                selfCpuSeconds() - startCpu, refused ? ", not connected: " : "", firstError.c_str());
    std::fflush(stdout);

    std::atomic<bool> stopPinging{false};
    std::thread pinger([&] {
        while (!stopPinging) {
            for (auto& viewer : viewers) {
                viewer->ping();
            }
            std::this_thread::sleep_for(250ms);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(o.warmup));
    // The window.
    std::vector<UdpConnection::PathStatus> before;
    for (auto& viewer : viewers) {
        before.push_back(viewer->hop().status());
    }
    std::vector<uint64_t> framesBefore;
    for (auto& viewer : viewers) {
        framesBefore.push_back(viewer->framesSoFar);
    }
    uint64_t sharerFramesBefore = 0, sharerBytesBefore = 0, keyframesBefore = 0;
    for (auto& sharer : sharers) {
        sharerFramesBefore += sharer->frames;
        sharerBytesBefore += sharer->bytes;
        keyframesBefore += sharer->keyframes;
    }
    const auto forwarderBefore = o.pid ? sampleProcess(o.pid) : ProcessSample{};
    const double selfBefore = selfCpuSeconds();
    uint64_t hopBytes = 0;
    for (auto& status : before) {
        hopBytes += status.bytes;
    }
    const uint64_t hopBytesBefore = hopBytes;
    const double windowStart = unixNow();
    const auto start = Clock::now();
    {
        std::lock_guard lock(second.mutex);
        second.latencyUs.clear();
    }
    measuring = true;
    std::printf("window: unix %.3f\n", windowStart);
    std::printf("   s  viewers  FPS mean/min  latency p50/p95 ms  forwarder CPU%%  RSS MB  threads  load CPU%%  "
                "hops Mbit/s\n");
    double forwarderMost = 0;
    uint64_t rssMost = 0;
    uint32_t threadsMost = 0;
    auto lastForwarder = forwarderBefore;
    double lastSelf = selfBefore;
    uint64_t lastHopBytes = hopBytesBefore;
    std::vector<uint64_t> lastFrames = framesBefore;
    auto lastTick = start;
    for (uint32_t s = 1; s <= o.seconds; ++s) {
        std::this_thread::sleep_until(start + std::chrono::seconds(s));
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastTick).count();
        lastTick = now;
        std::vector<uint32_t> latency;
        {
            std::lock_guard lock(second.mutex);
            latency.swap(second.latencyUs);
        }
        double fpsSum = 0, fpsMin = 1e9;
        uint32_t counted = 0;
        uint64_t bytesNow = 0;
        for (size_t i = 0; i < viewers.size(); ++i) {
            const uint64_t frames = viewers[i]->framesSoFar;
            bytesNow += viewers[i]->hop().status().bytes;
            if (!viewers[i]->slow) {
                const double fps = double(frames - lastFrames[i]) / elapsed;
                fpsSum += fps;
                fpsMin = std::min(fpsMin, fps);
                ++counted;
            }
            lastFrames[i] = frames;
        }
        const double selfNow = selfCpuSeconds();
        std::string forwarder = "      -      -        -";
        if (o.pid) {
            const auto sample = sampleProcess(o.pid);
            if (sample.ok && lastForwarder.ok) {
                const double cpu = (sample.cpuSeconds - lastForwarder.cpuSeconds) / elapsed * 100;
                forwarderMost = std::max(forwarderMost, cpu);
                rssMost = std::max(rssMost, sample.rss);
                threadsMost = std::max(threadsMost, sample.threads);
                char text[64];
                std::snprintf(text, sizeof(text), "%14.1f %7.1f %8u", cpu, double(sample.rss) / 1048576,
                              sample.threads);
                forwarder = text;
            }
            lastForwarder = sample;
        }
        std::printf("%4u %8u %6.1f/%5.1f %9.1f/%7.1f %s %10.1f %12.1f\n", s, counted, counted ? fpsSum / counted : 0.0,
                    counted ? fpsMin : 0.0, percentile(latency, 0.5, 1000), percentile(latency, 0.95, 1000),
                    forwarder.c_str(), (selfNow - lastSelf) / elapsed * 100,
                    double(bytesNow - lastHopBytes) * 8 / elapsed / 1e6);
        std::fflush(stdout);
        lastSelf = selfNow;
        lastHopBytes = bytesNow;
    }
    measuring = false;
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const double windowEnd = unixNow();
    const auto forwarderAfter = o.pid ? sampleProcess(o.pid) : ProcessSample{};
    const double selfAfter = selfCpuSeconds();
    stopPinging = true;
    pinger.join();

    Group others, slow;
    uint64_t hopBytesAfter = 0;
    std::vector<std::string> lines;
    for (size_t i = 0; i < viewers.size(); ++i) {
        auto& viewer = *viewers[i];
        const auto after = viewer.hop().status();
        hopBytesAfter += after.bytes;
        const auto m = viewer.measured();
        const auto failure = viewer.failure();
        (viewer.slow ? slow : others).add(m, before[i], after, seconds, !failure.empty());
        if (o.perViewer || viewer.slow) {
            auto latency = m.latencyUs;
            const auto path = sharers[viewer.room]->path(viewer.peer);
            char line[512];
            std::snprintf(line, sizeof(line),
                          "viewer %u.%u (%s%s, peer %" PRIu64 "): %.1f FPS, latency p50 %.1f p95 %.1f ms, lost %" PRIu64
                          ", gaps %" PRIu64 ", resent %" PRIu64 ", hop loss %u per mille; the forwarder's report: "
                          "target %.2f Mbit/s, loss %u per mille, backlog %u ms, drops %u%s%s\n",
                          viewer.room, viewer.index, nameOf(viewer.way), viewer.slow ? ", slow" : "", viewer.peer,
                          double(m.frames) / seconds, percentile(latency, 0.5, 1000), percentile(latency, 0.95, 1000),
                          m.lost, m.gaps, after.resendRecovered - before[i].resendRecovered, after.lossPermille,
                          path ? path->targetBps / 1e6 : 0.0, path ? path->lossPermille : 0,
                          path ? path->backlogUs / 1000 : 0, path ? path->drops : 0,
                          failure.empty() ? "" : "; ended: ", failure.c_str());
            lines.push_back(line);
        }
    }
    uint64_t sharerFrames = 0, sharerBytes = 0, keyframes = 0, asked = 0, moves = 0, reports = 0;
    std::string sharerFailure;
    for (auto& sharer : sharers) {
        sharerFrames += sharer->frames;
        sharerBytes += sharer->bytes;
        keyframes += sharer->keyframes;
        asked += sharer->asked;
        moves += sharer->moves;
        reports += sharer->reports;
        if (sharerFailure.empty()) {
            sharerFailure = sharer->failure();
        }
    }
    for (const auto& line : lines) {
        std::fputs(line.c_str(), stdout);
    }
    std::printf("window: unix %.3f .. %.3f (%.1f s)\n", windowStart, windowEnd, seconds);
    std::printf("sharers: %.1f pictures/s each, %.2f Mbit/s each, %" PRIu64 " keyframes in the window (%" PRIu64
                " asked for in all), %" PRIu64 " viewers moved down, %" PRIu64 " path reports%s%s\n",
                double(sharerFrames - sharerFramesBefore) / seconds / o.rooms,
                double(sharerBytes - sharerBytesBefore) * 8 / seconds / 1e6 / o.rooms, keyframes - keyframesBefore,
                asked, moves, reports, sharerFailure.empty() ? "" : "; a sharer ended: ", sharerFailure.c_str());
    others.print("viewers", stdout);
    slow.print("slow viewers", stdout);
    const double hopMbps = double(hopBytesAfter - hopBytesBefore) * 8 / seconds / 1e6;
    const double forwarderCpu = forwarderBefore.ok && forwarderAfter.ok
                                    ? (forwarderAfter.cpuSeconds - forwarderBefore.cpuSeconds) / seconds * 100
                                    : 0;
    if (o.pid) {
        std::printf("forwarder (pid %d): CPU %.1f %% (most in a second %.1f %%), RSS %.1f MB at the end (most %.1f "
                    "MB), %u threads\n",
                    o.pid, forwarderCpu, forwarderMost, double(forwarderAfter.rss) / 1048576, double(rssMost) / 1048576,
                    threadsMost);
    }
    std::printf("this process: CPU %.1f %%; the viewers' hops carried %.1f Mbit/s (%.2f each)\n",
                (selfAfter - selfBefore) / seconds * 100, hopMbps,
                viewers.empty() ? 0.0 : hopMbps / double(viewers.size()));
    // One line for scripts.
    std::printf(
        "RESULT label=%s entry=%s rooms=%u viewers=%zu fps=%u bitrate=%" PRIu64
        " audio=%d slow=%u fps_mean=%.2f fps_min=%.2f lat_p50=%.2f lat_p95=%.2f lat_p99=%.2f lat_max=%.1f "
        "lost=%" PRIu64 " gaps=%" PRIu64 " resent=%" PRIu64 " sound_p50=%.2f sound_lost=%" PRIu64
        " rtt_p50=%.2f rtt_p95=%.2f fwd_cpu=%.1f fwd_cpu_max=%.1f fwd_rss_mb=%.1f fwd_threads=%u "
        "load_cpu=%.1f hops_mbps=%.1f ended=%u slow_fps=%.2f slow_lat_p50=%.1f moves=%" PRIu64 " window=%.3f..%.3f\n",
        o.label.empty() ? "-" : o.label.c_str(), o.entry.c_str(), o.rooms, viewers.size(), o.fps, o.bitrate, o.audio,
        o.slow, others.viewers ? others.fpsSum / others.viewers : 0.0, others.viewers ? others.fpsMin : 0.0,
        percentile(others.latencyUs, 0.5, 1000), percentile(others.latencyUs, 0.95, 1000),
        percentile(others.latencyUs, 0.99, 1000), percentile(others.latencyUs, 1.0, 1000), others.lost, others.gaps,
        others.resent, percentile(others.soundUs, 0.5, 1000), others.soundsLost,
        percentile(others.roundTripUs, 0.5, 1000), percentile(others.roundTripUs, 0.95, 1000), forwarderCpu,
        forwarderMost, double(forwarderAfter.rss) / 1048576, threadsMost, (selfAfter - selfBefore) / seconds * 100,
        hopMbps, others.ended + slow.ended, slow.viewers ? slow.fpsSum / slow.viewers : 0.0,
        percentile(slow.latencyUs, 0.5, 1000), moves, windowStart, windowEnd);
    std::fflush(stdout);
    viewers.clear();
    sharers.clear();
    return 0;
}

// ---- monitor ---------------------------------------------------------------------------------

int monitor(int pid, const std::string& interface, uint32_t seconds) {
    auto last = sampleProcess(pid);
    if (!last.ok) {
        std::fprintf(stderr, "rcsdk_forwarder_load: no process %d\n", pid);
        return 1;
    }
    auto net = interface.empty() ? NetSample{} : sampleInterface(interface);
    auto lastAt = Clock::now();
    const auto start = lastAt;
    for (uint32_t s = 1; !seconds || s <= seconds; ++s) {
        std::this_thread::sleep_until(start + std::chrono::seconds(s));
        const auto sample = sampleProcess(pid);
        if (!sample.ok) {
            std::printf("process %d ended\n", pid);
            return 0;
        }
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastAt).count();
        const auto netNow = interface.empty() ? NetSample{} : sampleInterface(interface);
        std::printf("t=%.3f cpu=%.1f rss_mb=%.1f threads=%u", unixNow(),
                    (sample.cpuSeconds - last.cpuSeconds) / elapsed * 100, double(sample.rss) / 1048576,
                    sample.threads);
        if (netNow.ok && net.ok) {
            std::printf(" tx_mbps=%.1f rx_mbps=%.1f tx_pps=%.0f rx_pps=%.0f udp_rcvbuf_errors=%" PRIu64
                        " udp_sndbuf_errors=%" PRIu64,
                        double(netNow.txBytes - net.txBytes) * 8 / elapsed / 1e6,
                        double(netNow.rxBytes - net.rxBytes) * 8 / elapsed / 1e6,
                        double(netNow.txPackets - net.txPackets) / elapsed,
                        double(netNow.rxPackets - net.rxPackets) / elapsed, netNow.receiveErrors - net.receiveErrors,
                        netNow.bufferErrors - net.bufferErrors);
        }
        std::printf("\n");
        std::fflush(stdout);
        last = sample;
        net = netNow;
        lastAt = now;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
    }
    const std::string command = argv[1];
    std::map<std::string, std::string> args;
    std::vector<std::string> flags;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--per-viewer") {
            flags.push_back(arg);
        } else if (arg.rfind("--", 0) == 0 && i + 1 < argc) {
            args[arg] = argv[++i];
        } else {
            usage("what is " + arg + "?");
        }
    }
    const auto take = [&](const std::string& name) -> std::optional<std::string> {
        const auto found = args.find(name);
        if (found == args.end()) {
            return std::nullopt;
        }
        auto value = found->second;
        args.erase(found);
        return value;
    };
    try {
        if (command == "monitor") {
            const auto pid = take("--pid");
            if (!pid) {
                usage("monitor needs --pid");
            }
            const int result = monitor(std::stoi(*pid), take("--interface").value_or(""),
                                       uint32_t(std::stoul(take("--seconds").value_or("0"))));
            return result;
        }
        if (command != "run") {
            usage();
        }
        Options o;
        if (const auto forwarder = take("--forwarder")) {
            const auto colon = forwarder->rfind(':');
            if (colon == std::string::npos) {
                usage("--forwarder HOST:PORT");
            }
            o.host = forwarder->substr(0, colon);
            o.udpPort = uint16_t(std::stoul(forwarder->substr(colon + 1)));
        }
        if (const auto v = take("--tcp-port")) {
            o.tcpPort = uint16_t(std::stoul(*v));
        }
        if (const auto v = take("--websocket-port")) {
            o.webSocketPort = uint16_t(std::stoul(*v));
        }
        if (const auto v = take("--secret")) {
            const auto colon = v->find(':');
            if (colon == std::string::npos) {
                usage("--secret ID:HEX");
            }
            o.secret.key_id = uint32_t(std::stoul(v->substr(0, colon)));
            const auto bytes = unhex(v->substr(colon + 1), sizeof(o.secret.secret), "--secret");
            std::memcpy(o.secret.secret, bytes.data(), bytes.size());
            o.haveSecret = true;
        }
        if (const auto v = take("--identity")) {
            std::ifstream in(*v);
            std::string text;
            in >> text;
            const auto key = unhex(text, RCSFWD_IDENTITY_KEY_SIZE, "the identity key");
            const crypto::DeviceKey pair(key);
            o.forwarderKey = pair.publicKey();
            o.audience = crypto::sha256(pair.publicKey());
            o.haveAudience = true;
        }
        if (const auto v = take("--audience")) {
            const auto bytes = unhex(*v, 32, "--audience");
            std::copy(bytes.begin(), bytes.end(), o.audience.begin());
            o.haveAudience = true;
        }
        if (!o.haveSecret || !o.haveAudience || !o.udpPort) {
            usage("run needs --forwarder, --secret and --identity or --audience");
        }
        o.entry = take("--entry").value_or("udp");
        if (o.entry != "mix") {
            wayOf(o.entry);
        }
        o.sharerWay = wayOf(take("--sharer-entry").value_or("udp"));
        o.sharerAdaptive = take("--sharer-rate").value_or("adaptive") != "fixed";
        const auto count = [&](const char* name, uint32_t otherwise) {
            const auto v = take(name);
            return v ? uint32_t(amount(*v)) : otherwise;
        };
        o.rooms = std::max(1u, count("--rooms", 1));
        o.viewers = count("--viewers", 5);
        o.fps = std::clamp(count("--fps", 30), 1u, 240u);
        o.encodings = std::clamp(count("--encodings", 1), 1u, 2u);
        o.keyframeIntervalMs = count("--keyframe-interval-ms", 0);
        o.audioPerSecond = std::clamp(count("--sound-packets", 100), 1u, 1000u);
        o.audioBytes = std::clamp(count("--sound-bytes", 120), 24u, 1000u);
        o.warmup = count("--warmup", 5);
        o.seconds = std::max(1u, count("--seconds", 20));
        o.connectParallel = std::max(1u, count("--connect-parallel", 8));
        o.slow = count("--slow", 0);
        o.slowLoss = count("--slow-loss", 0);
        o.slowDelay = count("--slow-delay", 0);
        o.pid = int(count("--pid", 0));
        if (const auto v = take("--bitrate")) {
            o.bitrate = amount(*v);
        }
        if (const auto v = take("--slow-bps")) {
            o.slowBps = amount(*v);
        }
        if (const auto v = take("--keyframe-factor")) {
            o.keyframeFactor = std::stod(*v);
        }
        if (const auto v = take("--audio")) {
            o.audio = *v != "off";
        }
        o.label = take("--label").value_or("");
        const bool loopback = o.host == "127.0.0.1" || o.host == "::1" || o.host == "localhost";
        o.bind = take("--bind").value_or(loopback ? "127.0.0.1" : "0.0.0.0");
        o.perViewer = std::find(flags.begin(), flags.end(), "--per-viewer") != flags.end();
        if (!args.empty()) {
            usage("what is " + args.begin()->first + "?");
        }
        if (o.slow && o.viewerWay(0) != Way::udp && o.entry != "mix") {
            std::printf("note: slow viewers go over UDP (through the shaping proxy)\n");
        }
        return run(o);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "rcsdk_forwarder_load: %s\n", error.what());
        return 1;
    }
}
