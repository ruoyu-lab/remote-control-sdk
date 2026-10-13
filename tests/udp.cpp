// The UDP transport over loopback, with links that lose datagrams on purpose,
// a bottleneck, and peers made by hand that send what a real one would not.
#include "protocol/udp_connection.hpp"
#include "support/check.hpp"
#include "support/shaping_proxy.hpp"
#include "transport/handshake.hpp"
#include "util/bytes.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <future>
#include <numeric>
#include <set>
#include <mutex>
#include <thread>

using namespace rcsdk;
using namespace rcsdk::protocol;
using transport::Endpoint;
using transport::UdpSocket;

namespace {

const std::array<uint8_t, 32> key{7, 7, 7};
const auto credentials = transport::Credentials::fromKey(key);

// Drops datagrams it sends: those `drop` says, by number from the first and size.
using Drop = std::function<bool(uint64_t number, size_t size)>;
class LossyLink final : public Link {
public:
    LossyLink(std::unique_ptr<Link> inner, Drop drop) : inner_(std::move(inner)), drop_(std::move(drop)) {}
    bool send(transport::DatagramSocket& socket, std::span<const uint8_t> datagram, uint8_t dscp) override {
        if (drop_(++sent_, datagram.size())) {
            ++dropped;
            return false;
        }
        return inner_->send(socket, datagram, dscp);
    }
    std::optional<std::span<const uint8_t>> unwrap(transport::DatagramSocket& socket, Endpoint& from,
                                                   std::span<const uint8_t> datagram) override {
        return inner_->unwrap(socket, from, datagram);
    }
    size_t overhead() const noexcept override { return 0; }
    std::string describe() const override { return "lossy " + inner_->describe(); }
    std::optional<Endpoint> peer() const override { return inner_->peer(); }
    void setPeer(const Endpoint& peer) override { inner_->setPeer(peer); }
    void sendTo(transport::DatagramSocket& socket, const Endpoint& to, std::span<const uint8_t> datagram) override {
        if (!drop_(++sent_, datagram.size())) {
            inner_->sendTo(socket, to, datagram);
        }
    }
    rcs_path_route route() const override { return inner_->route(); }
    std::atomic<uint64_t> dropped{0};

private:
    std::unique_ptr<Link> inner_;
    Drop drop_;
    uint64_t sent_ = 0;
};

EncodedPacket picture(uint64_t sequence, size_t size, bool keyframe) {
    EncodedPacket packet;
    packet.codec = RCS_CODEC_H264;
    packet.width = 1920;
    packet.height = 1080;
    packet.keyframe = keyframe;
    packet.sequence = sequence;
    packet.timestampNs = monotonicNs();
    // Valid H.264 framing: one length-prefixed NAL unit (IDR or non-IDR slice).
    const size_t payload = size - 4;
    packet.data = {uint8_t(payload >> 24), uint8_t(payload >> 16), uint8_t(payload >> 8), uint8_t(payload),
                   uint8_t(keyframe ? 0x65 : 0x41)};
    for (size_t i = 5; i < size; ++i) {
        packet.data.push_back(uint8_t(i * 31 + sequence));
    }
    packet.config = {0, 0, 0, 2, 0x67, 0x42, 0, 0, 0, 2, 0x68, 0xce};
    return packet;
}

struct Pair {
    std::unique_ptr<UdpConnection> server, client;
    LossyLink* clientLink = nullptr; // Viewer -> sharer datagrams.
};

// The server sends (as a sharer would); `serverDrop` and `clientDrop` lose
// their datagrams. Through `proxy` (made for 127.0.0.1:port) when given.
Pair connect(uint16_t port, Drop serverDrop, UdpOptions options = {}, std::array<uint8_t, 32> clientKey = key,
             UdpConnection::Rejected rejected = {}, Drop clientDrop = {}, test::ShapingProxy* proxy = nullptr) {
    static std::atomic<bool> never{false};
    const auto local = Endpoint::parse("127.0.0.1", port);
    auto serverSocket = std::make_unique<UdpSocket>(local);
    auto clientSocket = std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0));
    const auto clientAddress = proxy ? proxy->backAddress() : clientSocket->local();
    const auto serverAddress = proxy ? proxy->address() : local;
    auto server = std::async(std::launch::async, [&, socket = std::move(serverSocket)]() mutable {
        auto link = std::make_unique<LossyLink>(directLink(clientAddress), serverDrop);
        return UdpConnection::establish(UdpConnection::Side::server, std::move(socket), std::move(link), credentials,
                                        options, never, rejected);
    });
    Pair pair;
    auto clientLink =
        clientDrop ? std::make_unique<LossyLink>(directLink(serverAddress), clientDrop) : directLink(serverAddress);
    UdpOptions clientOptions; // The numbering and the paths' settings are both ends'.
    clientOptions.firstMessageId = options.firstMessageId;
    clientOptions.idleTimeout = options.idleTimeout;
    clientOptions.migrate = options.migrate;
    clientOptions.pathStall = options.pathStall;
    clientOptions.hold = options.hold;
    clientOptions.maxInbound = options.maxInbound;
    pair.client = UdpConnection::establish(UdpConnection::Side::client, std::move(clientSocket), std::move(clientLink),
                                           transport::Credentials::fromKey(clientKey), clientOptions, never);
    pair.server = server.get();
    return pair;
}

// Receives until `count` pictures arrived; returns how many gaps came with
// them. Other messages may overtake video and go to `others`.
int receivePictures(UdpConnection& connection, size_t count, std::vector<EncodedPacket>& pictures,
                    std::vector<Message>* others = nullptr) {
    int gaps = 0;
    while (pictures.size() < count) {
        auto message = connection.receive();
        if (auto* packet = std::get_if<EncodedPacket>(&message)) {
            pictures.push_back(std::move(*packet));
        } else if (std::holds_alternative<StreamGap>(message)) {
            ++gaps;
        } else if (others) {
            others->push_back(std::move(message));
        }
    }
    return gaps;
}

void reliableAndLarge() {
    auto pair = connect(47851, [](uint64_t, size_t) { return false; });
    // Messages arrive in order; a 300 KB keyframe arrives whole.
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    for (uint64_t epoch = 1; epoch <= 20; ++epoch) {
        pair.client->send(Input{move, epoch});
    }
    for (uint64_t epoch = 1; epoch <= 20; ++epoch) {
        const auto message = pair.server->receive();
        CHECK(std::get<Input>(message).epoch == epoch);
    }
    const auto big = picture(1, 300'000, true);
    pair.server->send(big);
    pair.server->send(ControlGrant{true, 3});
    std::vector<EncodedPacket> pictures;
    std::vector<Message> others;
    CHECK(receivePictures(*pair.client, 1, pictures, &others) == 0);
    CHECK(pictures[0].data == big.data && pictures[0].config == big.config && pictures[0].keyframe);
    // The small control message travels apart from the queued video and may overtake it.
    if (others.empty()) {
        others.push_back(pair.client->receive());
    }
    CHECK(std::get<ControlGrant>(others[0]).epoch == 3);
    CHECK(pair.client->status().roundTripNs < 50'000'000);
    CHECK(pair.client->bytesTransferred() > 300'000);
}

// Message numbers go past 2^32 (the wire carries their low 32 bits): in
// order both ways, through loss, resending and acknowledgement.
void numbersWrap() {
    UdpOptions options;
    options.firstMessageId = (uint64_t(1) << 32) - 100;
    const auto lossy = [](uint64_t n, size_t) { return n % 9 == 4; };
    auto pair = connect(47848, lossy, options, key, {}, lossy);
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    for (uint64_t epoch = 1; epoch <= 300; ++epoch) {
        pair.client->send(Input{move, epoch});
        pair.server->send(ControlGrant{true, epoch});
    }
    for (uint64_t epoch = 1; epoch <= 300; ++epoch) {
        CHECK(std::get<Input>(pair.server->receive()).epoch == epoch);
        CHECK(std::get<ControlGrant>(pair.client->receive()).epoch == epoch);
    }
}

void lossRecovered() {
    // 5% of the sharer's datagrams vanish, in a fixed pattern: every picture
    // still arrives complete, by resending, without gaps.
    auto pair = connect(47852, [](uint64_t n, size_t) { return n % 20 == 7; },
                        {Connection::VideoRate{false, 0, 50'000'000, 0}});
    std::vector<EncodedPacket> sent, received;
    for (uint64_t i = 1; i <= 120; ++i) {
        sent.push_back(picture(i, i % 30 == 1 ? 120'000 : 9'000, i % 30 == 1));
        pair.server->send(sent.back());
        std::this_thread::sleep_for(4ms);
    }
    CHECK(receivePictures(*pair.client, sent.size(), received) == 0);
    for (size_t i = 0; i < sent.size(); ++i) {
        CHECK(received[i].sequence == sent[i].sequence && received[i].data == sent[i].data);
    }
}

// About one datagram in 20, spread like random loss (a fixed pattern lines
// up with pictures of a fixed size and keeps hitting the same fragment).
bool scattered(uint64_t n) {
    return (n * 0x9E3779B97F4A7C15ull) >> 58 < 3;
}

// Parity (FEC): fixed parity rebuilds steady loss as the pictures arrive,
// hardly asking again; without parity the same loss is resent; bursts beyond
// the parity are resent too. Every picture arrives whole either way.
void parityRepairs() {
    using Statuses = std::pair<Connection::PathStatus, Connection::PathStatus>;
    const auto run = [](uint16_t port, FecSetting fec, Drop drop) -> Statuses {
        auto pair = connect(port, std::move(drop), {Connection::VideoRate{false, 0, 50'000'000, 0, {}, fec}});
        std::vector<EncodedPacket> sent, received;
        for (uint64_t i = 1; i <= 120; ++i) {
            sent.push_back(picture(i, i % 30 == 1 ? 120'000 : 9'000, i % 30 == 1));
            pair.server->send(sent.back());
            std::this_thread::sleep_for(4ms);
        }
        CHECK(receivePictures(*pair.client, sent.size(), received) == 0);
        for (size_t i = 0; i < sent.size(); ++i) {
            CHECK(received[i].sequence == sent[i].sequence && received[i].data == sent[i].data);
        }
        return {pair.server->status(), pair.client->status()};
    };
    const auto steady = [](uint64_t n, size_t) { return scattered(n); };
    const auto [fixed, fixedRepaired] = run(47842, {RCS_FEC_FIXED, 20}, steady);
    std::printf("parity: 20%% against 5%% loss: %llu parity sent, %llu fragments rebuilt, %llu resent\n",
                (unsigned long long)fixed.paritySent, (unsigned long long)fixedRepaired.fecRecovered,
                (unsigned long long)fixedRepaired.resendRecovered);
    CHECK(fixed.fecPercent == 20 && fixed.paritySent > 0 && fixedRepaired.framesLost == 0);
    CHECK(fixedRepaired.fecRecovered > 0 && fixedRepaired.resendRecovered * 4 < fixedRepaired.fecRecovered);
    const auto [off, offRepaired] = run(47843, {RCS_FEC_OFF, 50}, steady);
    CHECK(off.fecPercent == 0 && off.paritySent == 0);
    CHECK(offRepaired.fecRecovered == 0 && offRepaired.resendRecovered > 0);
    // Ten in a row every 200 datagrams: more than 5% parity covers.
    const auto [bursts, burstsRepaired] =
        run(47844, {RCS_FEC_FIXED, 5}, [](uint64_t n, size_t) { return n > 20 && n % 200 < 10; });
    CHECK(bursts.paritySent > 0 && burstsRepaired.resendRecovered > 0 && burstsRepaired.framesLost == 0);
}

// AUTO: no parity on a clean path; parity once the viewer's reports show
// loss; none again 5 s after the last.
void parityFollowsLoss() {
    std::atomic<bool> lossy{false};
    auto pair = connect(47845, [&](uint64_t n, size_t) { return lossy && scattered(n); },
                        {Connection::VideoRate{false, 0, 50'000'000, 0}});
    uint64_t sequence = 0;
    const auto stream = [&](std::chrono::milliseconds most, const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + most;
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            ++sequence;
            pair.server->send(picture(sequence, 9'000, sequence == 1));
            std::this_thread::sleep_for(16ms);
        }
        return done();
    };
    CHECK(!stream(1s, [&] { return pair.server->status().fecPercent != 0; }));
    CHECK(pair.server->status().paritySent == 0);
    lossy = true;
    CHECK(stream(3s, [&] { return pair.server->status().fecPercent >= 10; }));
    stream(2s, [] { return false; }); // Lossy with parity.
    CHECK(pair.server->status().paritySent > 0 && pair.server->status().fecPercent <= 50);
    lossy = false;
    const auto clean = std::chrono::steady_clock::now();
    CHECK(stream(9s, [&] { return pair.server->status().fecPercent == 0; }));
    CHECK(std::chrono::steady_clock::now() - clean >= 4s);
    std::vector<EncodedPacket> pictures;
    receivePictures(*pair.client, sequence, pictures);
    const auto repaired = pair.client->status();
    std::printf("parity, AUTO: %llu sent, %llu fragments rebuilt, %llu resent, %llu pictures lost\n",
                (unsigned long long)pair.server->status().paritySent, (unsigned long long)repaired.fecRecovered,
                (unsigned long long)repaired.resendRecovered, (unsigned long long)repaired.framesLost);
    CHECK(repaired.fecRecovered > 0 && repaired.framesLost == 0);
}

AudioPacket sound(uint32_t sequence) {
    return AudioPacket{sequence, 960, monotonicNs(), std::vector<uint8_t>(120, uint8_t(sequence))};
}

// Up to `count` datagrams that reach `receiver` within a second, each with
// the traffic class of its IP header (receiveTos; Windows: -1, it shows none).
std::vector<std::pair<Bytes, int>> receiveMarked(UdpSocket& receiver, size_t count) {
    std::vector<std::pair<Bytes, int>> seen;
    for (const auto deadline = Clock::now() + 1s; seen.size() < count && Clock::now() < deadline;) {
        Bytes data(2048);
#ifdef _WIN32
        Endpoint from;
        if (const auto size = receiver.receive(data, from)) {
            data.resize(*size);
            seen.emplace_back(std::move(data), -1);
            continue;
        }
#else
        uint8_t control[64];
        iovec io{data.data(), data.size()};
        msghdr message{};
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        if (const auto size = recvmsg(receiver.handle(), &message, 0); size > 0) {
            int tos = -1;
            for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
                if (c->cmsg_level == IPPROTO_IP && (c->cmsg_type == IP_TOS || c->cmsg_type == IP_RECVTOS)) {
                    tos = *CMSG_DATA(c);
                } else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_TCLASS) {
                    std::memcpy(&tos, CMSG_DATA(c), sizeof(tos));
                }
            }
            data.resize(size_t(size));
            seen.emplace_back(std::move(data), tos);
            continue;
        }
#endif
        std::this_thread::sleep_for(10ms);
    }
    return seen;
}

// `receiver` is told each datagram's traffic class (not on Windows).
void receiveTos(UdpSocket& receiver) {
#ifndef _WIN32
    int on = 1;
    CHECK(receiver.local().ipv6() ? setsockopt(receiver.handle(), IPPROTO_IPV6, IPV6_RECVTCLASS, &on, sizeof(on)) == 0
                                  : setsockopt(receiver.handle(), IPPROTO_IP, IP_RECVTOS, &on, sizeof(on)) == 0);
#else
    (void)receiver;
#endif
}

// A datagram sent marked carries the DSCP in its IP header, the next one
// unmarked does not; Windows shows no class to the receiver here.
void marking() {
    UdpSocket sender(Endpoint::parse("127.0.0.1", 0)), receiver(Endpoint::parse("127.0.0.1", 0));
    receiveTos(receiver);
    const Bytes marked{1}, plain{2};
    CHECK(sender.sendMarked(receiver.local(), marked, 46));
    sender.send(receiver.local(), plain);
    const auto seen = receiveMarked(receiver, 2);
    CHECK(seen.size() == 2);
    CHECK(seen[0].first == marked && seen[1].first == plain);
#ifndef _WIN32
    CHECK(seen[0].second == 46 << 2 && seen[1].second == 0);
#endif
}

// How a batch leaves a socket in few system calls (DatagramBatcher), against
// a system played here: runs of datagrams to one address with one mark, each
// as large as the first but the last, within a run's and a call's limits;
// calls that send fewer messages than asked, a full socket, runs the system
// refuses (no UDP GSO there after all), and errors that drop one message.
void batching() {
    using transport::DatagramBatcher;
    using transport::Outgoing;
    using Message = DatagramBatcher::Message;
    const auto a = Endpoint::parse("10.0.0.1", 1), alsoA = Endpoint::parse("10.0.0.1", 1),
               b = Endpoint::parse("10.0.0.2", 1);
    std::vector<Bytes> datagrams;
    std::vector<Outgoing> batch;
    const auto clear = [&] {
        datagrams.clear();
        batch.clear();
    };
    const auto add = [&](const Endpoint& to, size_t size, uint8_t dscp = 0, size_t count = 1) {
        for (size_t i = 0; i < count; ++i) {
            datagrams.emplace_back(size, uint8_t(datagrams.size()));
            batch.push_back({&to, datagrams.back(), dscp});
        }
    };
    std::vector<std::vector<Message>> calls;
    std::deque<int> answers;  // Messages that go (> 0) or -errno, call by call; then every message goes.
    std::vector<size_t> sent; // The datagrams the system took, in order.
    DatagramBatcher batcher([&](std::span<const Outgoing> given, std::span<const Message> messages, int& error) {
        CHECK(given.data() == batch.data());
        calls.emplace_back(messages.begin(), messages.end());
        int answer = int(messages.size());
        if (!answers.empty()) {
            answer = answers.front();
            answers.pop_front();
        }
        if (answer < 0) {
            error = -answer;
            return -1;
        }
        for (const auto& message : messages.first(size_t(answer))) {
            for (size_t i = message.first; i < message.first + message.count; ++i) {
                sent.push_back(i);
            }
        }
        return answer;
    });
    std::atomic<bool> failing{false};
    const auto send = [&] {
        calls.clear();
        sent.clear();
        batcher.send(batch, failing, a);
    };
    const auto shape = [](const std::vector<Message>& messages) { // first:count:dscp, a run's segment after '/'.
        std::string text;
        for (const auto& m : messages) {
            text += (text.empty() ? "" : " ") + std::to_string(m.first) + ":" + std::to_string(m.count) + ":" +
                    std::to_string(m.dscp) + (m.count > 1 ? "/" + std::to_string(m.segment) : "");
        }
        return text;
    };
    const auto all = [&](size_t count) {
        std::vector<size_t> expected(count);
        std::iota(expected.begin(), expected.end(), size_t(0));
        return sent == expected;
    };

    // Runs: equal sizes to one address (another Endpoint of the same one is
    // the same), one mark; a shorter one ends a run; larger than any IPv6
    // path takes leaves alone.
    add(a, 1200, 0, 10);
    add(alsoA, 500);
    add(b, 1200, 0, 3);
    add(a, 120, 46);
    add(a, 1200, 46, 2);
    add(a, 1200, 0, 2);
    add(a, 1400, 0, 2);
    batcher.setSegmenting(true);
    send();
    CHECK(calls.size() == 1 && all(batch.size()));
    CHECK(shape(calls[0]) == "0:11:0/1200 11:3:0/1200 14:1:46 15:2:46/1200 17:2:0/1200 19:1:0 20:1:0");
    // Without UDP GSO, a message a datagram.
    batcher.setSegmenting(false);
    send();
    CHECK(calls.size() == 1 && calls[0].size() == batch.size() && all(batch.size()));
    CHECK(calls[0][14].dscp == 46 && calls[0][15].dscp == 46 && calls[0][17].dscp == 0);

    // A run's limits (65 507 bytes, 64 datagrams) and a call's (256 datagrams, 64 messages).
    clear();
    add(a, 1200, 0, 300);
    batcher.setSegmenting(true);
    send();
    CHECK(all(300) && calls.size() == 2);
    CHECK(shape(calls[0]) == "0:54:0/1200 54:54:0/1200 108:54:0/1200 162:54:0/1200 216:40:0/1200");
    CHECK(shape(calls[1]) == "256:44:0/1200");
    clear();
    add(a, 500, 0, 130);
    send();
    CHECK(all(130) && calls.size() == 1 && shape(calls[0]) == "0:64:0/500 64:64:0/500 128:2:0/500");
    clear();
    for (int i = 0; i < 100; ++i) {
        add(i % 2 ? a : b, 100);
    }
    batcher.setSegmenting(false);
    send();
    CHECK(all(100) && calls.size() == 2 && calls[0].size() == 64 && calls[1].size() == 36);

    // A call that sends fewer messages than asked: the next starts where it stopped.
    clear();
    for (int i = 0; i < 5; ++i) {
        add(i % 2 ? a : b, 100);
    }
    answers = {2, 1};
    send();
    CHECK(all(5) && calls.size() == 3 && calls[1][0].first == 2 && calls[2][0].first == 3);
    // A full socket: the rest of the batch is dropped, as datagrams are that find no room.
    answers = {2, -EAGAIN};
    send();
    CHECK(sent == std::vector<size_t>({0, 1}) && calls.size() == 2 && !failing);
    // Interrupted before it sent anything: again.
    answers = {-EINTR};
    send();
    CHECK(all(5) && calls.size() == 2);
    // An error of one message's: that message is dropped, the rest go; one
    // that stays (no route) makes the socket failing until a send goes.
    answers = {1, -ENETUNREACH};
    send();
    CHECK(sent == std::vector<size_t>({0, 2, 3, 4}) && !failing);
    answers = {4, -ENETUNREACH};
    send();
    CHECK(sent == std::vector<size_t>({0, 1, 2, 3}) && failing);
    answers = {-ENOBUFS, -ENOBUFS, -ENOBUFS, -ENOBUFS, -ENOBUFS};
    send();
    CHECK(sent.empty() && calls.size() == 5 && !failing); // No buffers for a moment: dropped, not failing.

    // A run the system refuses (UDP_SEGMENT known, but the network card
    // computes no checksums: EIO; or a path's MTU: EINVAL): runs are off for
    // good, and its datagrams go again one by one.
    for (const int refusal : {EIO, EINVAL}) {
        clear();
        add(a, 1200, 0, 4);
        add(b, 100);
        batcher.setSegmenting(true);
        answers = {-refusal};
        send();
        CHECK(all(5) && calls.size() == 2 && shape(calls[0]) == "0:4:0/1200 4:1:0" && calls[1].size() == 5);
        CHECK(!batcher.segmenting());
        send();
        CHECK(all(5) && calls.size() == 1 && calls[0].size() == 5);
    }
    // EINVAL for a single datagram is that datagram's: dropped, runs stay on.
    clear();
    add(a, 1200, 0, 3);
    add(b, 100);
    batcher.setSegmenting(true);
    answers = {1, -EINVAL};
    send();
    CHECK(shape(calls[0]) == "0:3:0/1200 3:1:0" && sent == std::vector<size_t>({0, 1, 2}));
    CHECK(batcher.segmenting() && failing);
}

// A batch through a socket (Linux: sendmmsg, UDP GSO cutting runs on
// loopback; elsewhere one by one): every datagram arrives whole, in order,
// at its receiver, the marked one marked.
void batchedSocket(const std::string& loopback) {
    UdpSocket sender(Endpoint::parse(loopback, 0)), first(Endpoint::parse(loopback, 0)),
        second(Endpoint::parse(loopback, 0));
    receiveTos(first);
    const auto one = first.local(), two = second.local();
    std::vector<Bytes> datagrams;
    std::vector<transport::Outgoing> batch;
    const auto add = [&](const Endpoint& to, size_t size, uint8_t dscp = 0) {
        Bytes datagram(size);
        for (size_t i = 0; i < size; ++i) {
            datagram[i] = uint8_t(datagrams.size() + i * 7);
        }
        datagrams.push_back(std::move(datagram));
        batch.push_back({&to, datagrams.back(), dscp});
    };
    for (int i = 0; i < 40; ++i) {
        add(one, 1200);
    }
    add(one, 300);
    for (int i = 0; i < 5; ++i) {
        add(two, 1200);
    }
    add(one, 120, 46);
    for (int i = 0; i < 3; ++i) {
        add(one, 1000);
    }
    CHECK(sender.sendBatch(batch) == 1);
    const auto atFirst = receiveMarked(first, 45), atSecond = receiveMarked(second, 5);
    CHECK(atFirst.size() == 45 && atSecond.size() == 5);
    for (size_t i = 0; i < atFirst.size(); ++i) {
        const size_t index = i <= 40 ? i : i + 5; // The five for the second receiver between.
        CHECK(atFirst[i].first == datagrams[index]);
#ifndef _WIN32
        CHECK(atFirst[i].second == (index == 46 ? 46 << 2 : 0));
#endif
    }
    for (size_t i = 0; i < atSecond.size(); ++i) {
        CHECK(atSecond[i].first == datagrams[41 + i]);
    }
    CHECK(!sender.failing());
}

// Sound marked as the connection was told (audioDscp), every packet of it,
// though it leaves in the same batches as the pictures around it.
void soundMarkedInBatches() {
    UdpOptions options;
    options.audioDscp = 46;
    auto pair = connect(47888, [](uint64_t, size_t) { return false; }, options);
    for (uint32_t i = 1; i <= 30; ++i) {
        pair.server->send(picture(i, 20'000, i == 1));
        pair.server->send(sound(i));
        std::this_thread::sleep_for(5ms);
    }
    size_t pictures = 0, sounds = 0;
    while (pictures < 30 || sounds < 30) {
        const auto message = pair.client->receive();
        pictures += std::holds_alternative<EncodedPacket>(message);
        sounds += std::holds_alternative<AudioPacket>(message);
    }
    // Counted once the system took them, which may be after they arrived.
    for (int i = 0; i < 100 && pair.server->status().markedPackets < 30; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(pair.server->status().markedPackets == 30);
}

void soundBetweenPictures() {
    // A still screen sends no video while sound keeps flowing; the first
    // pictures after it must not report the sound in between as lost, or
    // adaptive bitrate drops at every movement.
    auto pair = connect(47859, [](uint64_t, size_t) { return false; }, 
                        {Connection::VideoRate{true, 1'000'000, 20'000'000, 4'000'000}});
    uint64_t sequence = 0;
    uint32_t audio = 0;
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 50; ++i) { // One second of sound alone.
            pair.server->send(sound(++audio));
            std::this_thread::sleep_for(20ms);
        }
        for (int i = 0; i < 30; ++i) { // Half a second of pictures with sound.
            ++sequence;
            pair.server->send(picture(sequence, 6'000, sequence == 1));
            pair.server->send(sound(++audio));
            std::this_thread::sleep_for(16ms);
        }
        const auto status = pair.server->status();
        CHECK(status.lossPermille < 20);
        CHECK(status.targetBitrate >= 4'000'000);
    }
}

void outages() {
    // A 400 ms outage: the sharer still holds those pictures; the viewer asks
    // for them once the next one arrives, and nothing is lost.
    std::atomic<bool> outage{false};
    auto pair = connect(47853, [&](uint64_t, size_t) { return outage.load(); });
    pair.server->send(picture(1, 5'000, true));
    std::vector<EncodedPacket> received;
    CHECK(receivePictures(*pair.client, 1, received) == 0);
    outage = true;
    pair.server->send(picture(2, 5'000, false));
    pair.server->send(picture(3, 5'000, false));
    std::this_thread::sleep_for(400ms);
    outage = false;
    pair.server->send(picture(4, 5'000, false));
    CHECK(receivePictures(*pair.client, 4, received) == 0);
    CHECK(received[1].sequence == 2 && received[2].sequence == 3 && received[3].sequence == 4);

    // 1.3 s: longer than the sharer keeps pictures. They become a gap (the
    // viewer then waits for a keyframe) and the stream continues.
    outage = true;
    pair.server->send(picture(5, 5'000, false));
    std::this_thread::sleep_for(1300ms);
    outage = false;
    pair.server->send(picture(6, 5'000, true));
    const int gaps = receivePictures(*pair.client, 5, received);
    CHECK(gaps == 1 && received.back().sequence == 6);
}

void reorderBufferFull() {
    // Message 1 stays lost while 5000 later ones arrive: more than the
    // receiver holds out of order. What it could not hold it did not
    // acknowledge, so those come again, and everything arrives in order.
    rcs_input_event press = RCS_INPUT_EVENT_INIT;
    press.type = RCS_INPUT_KEY;
    press.code = 4;
    const size_t inputDatagram = 1 + 8 + 1 + 4 + encode(Input{press, 1}).size() + crypto::Aead::tagSize;
    std::atomic<bool> holdInput{true};
    auto pair = connect(
        47857, [](uint64_t, size_t) { return false; }, {}, key, {},
        [&](uint64_t, size_t size) { return holdInput && size == inputDatagram; });
    pair.client->send(Input{press, 1});
    constexpr int later = 5000;
    for (int i = 0; i < later; ++i) {
        pair.client->send(KeyframeRequest{});
    }
    std::this_thread::sleep_for(300ms);
    holdInput = false;
    auto received = std::async(std::launch::async, [&] {
        CHECK(std::get<Input>(pair.server->receive()).epoch == 1);
        for (int i = 0; i < later; ++i) {
            CHECK(std::holds_alternative<KeyframeRequest>(pair.server->receive()));
        }
    });
    CHECK(received.wait_for(10s) == std::future_status::ready);
    received.get();
}

// A picture that fills exactly `fragments` full datagrams (direct link).
EncodedPacket fullPicture(uint64_t sequence, size_t fragments, bool keyframe) {
    constexpr size_t fragment = 1200 - 9 - 16 - 13; // Datagram, its header and tag, the fragment header.
    const size_t overhead = encode(picture(1, 1000, false)).size() - 1000;
    return picture(sequence, fragments * fragment - overhead, keyframe);
}

// Fragments that come ~10 ms apart (a slow path's bottleneck, a sharer pacing
// at a low rate) are not taken for lost while the rest of their frame is on
// its way: nothing is resent that was not lost.
void slowPaths() {
    const auto check = [](uint16_t port, UdpOptions options, test::ShapingProxy* proxy) {
        std::atomic<uint64_t> full{0}; // Datagrams of a whole fragment: first copies and resent ones.
        auto pair = connect(
            port, [&](uint64_t, size_t size) { return full += size == 1200, false; }, options, key, {}, {}, proxy);
        constexpr int frames = 20, fragments = 6;
        auto receiving = std::async(std::launch::async, [&] {
            std::vector<EncodedPacket> received;
            return receivePictures(*pair.client, frames, received);
        });
        for (int i = 1; i <= frames; ++i) {
            pair.server->send(fullPicture(uint64_t(i), fragments, i == 1));
            std::this_thread::sleep_for(100ms);
        }
        CHECK(receiving.get() == 0);
        std::this_thread::sleep_for(100ms);
        CHECK(full >= frames * fragments && full <= frames * fragments + 2);
    };
    // A 1 Mbit/s bottleneck with room in its queue: a fragment every 9.6 ms,
    // although the sharer sends at 20 Mbit/s.
    test::ShapingProxy proxy(Endpoint::parse("127.0.0.1", 47860));
    proxy.bitsPerSecond = 1'000'000;
    proxy.queueMs = 5000;
    check(47860, {Connection::VideoRate{false, 0, 800'000, 0}}, &proxy);
    CHECK(proxy.dropped == 0);
    // The sharer's own pacing at 1.5 Mbit/s: the host's maximum.
    RateRules slow;
    slow.maxPacingBps = 1'500'000;
    check(47861, {Connection::VideoRate{true, 1'000'000, 1'000'000, 1'000'000, slow}}, nullptr);
}

// While the viewer's reports find the path clear, each picture leaves within
// the frame spread (1 ms unless the host says otherwise), never faster than
// the host's maximum; once they show loss, at the slower pace (a fixed
// bitrate: four times it, at least 20 Mbit/s).
void pacing() {
    // First fragment to complete picture, the median of 15 pictures of 60 KB,
    // at a fixed 2 Mbit/s: at the slower pace each would take ~22 ms.
    const auto spread = [](uint16_t port, const RateRules& rules) {
        auto pair = connect(port, [](uint64_t, size_t) { return false; },
                            {Connection::VideoRate{false, 0, 2'000'000, 0, rules}});
        std::vector<uint64_t> spreads;
        std::vector<EncodedPacket> received;
        for (uint64_t i = 1; i <= 15; ++i) {
            pair.server->send(picture(i, 60'000, i == 1));
            CHECK(receivePictures(*pair.client, i, received) == 0);
            spreads.push_back(pair.client->lastReceiveNs());
            std::this_thread::sleep_for(20ms);
        }
        std::sort(spreads.begin(), spreads.end());
        return std::chrono::nanoseconds(spreads[spreads.size() / 2]);
    };
    const auto atOnce = spread(47864, {});
    RateRules spreadOut;
    spreadOut.frameSpread = 15ms;
    const auto spreadOver = spread(47865, spreadOut);
    RateRules capped;
    capped.maxPacingBps = 8'000'000;
    const auto atMost = spread(47866, capped);
    std::printf("pacing: a 60 KB picture in %.2f ms by default, %.2f ms with a 15 ms spread, %.2f ms at most at "
                "8 Mbit/s\n",
                double(atOnce.count()) / 1e6, double(spreadOver.count()) / 1e6, double(atMost.count()) / 1e6);
    CHECK(atOnce < 10ms);
    CHECK(spreadOver > 8ms);
    CHECK(atMost > 40ms);

    // 8% of the sharer's datagrams lost (and resent), above the clear 5%: the
    // reports are not clear, and a picture of 300 KB waits as long as 20 Mbit/s
    // takes (120 ms); on a clear path as long as 800 Mbit/s takes (3 ms).
    const auto backlog = [](uint16_t port, bool lossy) {
        auto pair = connect(port, [lossy](uint64_t n, size_t) { return lossy && n % 12 == 7; },
                            {Connection::VideoRate{false, 0, 2'000'000, 0}});
        std::vector<EncodedPacket> received;
        for (uint64_t i = 1; i <= 20; ++i) { // Reports come while video arrives.
            pair.server->send(picture(i, 3'000, i == 1));
            std::this_thread::sleep_for(20ms);
        }
        CHECK(receivePictures(*pair.client, 20, received) == 0);
        pair.server->send(picture(21, 300'000, true));
        const auto queued = std::chrono::nanoseconds(pair.server->sendBacklogNs());
        CHECK(receivePictures(*pair.client, 21, received) == 0);
        return queued;
    };
    const auto clear = backlog(47867, false);
    const auto lossy = backlog(47868, true);
    std::printf("pacing: 300 KB queued for %.1f ms on a clear path, %.1f ms on a lossy one\n",
                double(clear.count()) / 1e6, double(lossy.count()) / 1e6);
    CHECK(clear < 20ms);
    CHECK(lossy > 60ms);
}

// A 20 Mbit/s bottleneck (a wire) that spaces each picture: the viewer
// measures it, and from then on a large picture leaves only its first 8 ms of
// the path at once and the rest at 1.1 times the path's rate, so that sound
// sent after it overtakes it at the sharer and finds little ahead of it at the
// bottleneck; without (pathPacing 0) it waits there behind the whole picture.
// The viewer measures the sound's queueing on the path, the sharer its wait.
void pathPacing() {
    struct Outcome {
        Clock::duration late;
        uint64_t pathBps, queueUs, waitUs;
    };
    const auto outcome = [](uint16_t port, bool paced) {
        test::ShapingProxy proxy(Endpoint::parse("127.0.0.1", port));
        proxy.bitsPerSecond = 20'000'000;
        proxy.queueMs = 2000;
        RateRules rules;
        rules.pathPacing = paced ? rules.pathPacing : 0;
        auto pair = connect(port, [](uint64_t, size_t) { return false; },
                            {Connection::VideoRate{false, 0, 10'000'000, 0, rules}}, key, {}, {}, &proxy);
        std::vector<EncodedPacket> received;
        std::vector<Message> sounds;
        uint32_t audio = 0;
        for (uint64_t i = 1; i <= 25; ++i) {
            pair.server->send(picture(i, 30'000, i == 1));
            std::this_thread::sleep_for(20ms);
            pair.server->send(sound(++audio)); // Sound after the picture left the bottleneck: its least delay.
            std::this_thread::sleep_for(20ms);
        }
        CHECK(receivePictures(*pair.client, 25, received, &sounds) == 0);
        Outcome o{};
        o.pathBps = pair.server->status().pathBps;
        pair.server->send(picture(26, 600'000, true)); // 240 ms of the path.
        std::this_thread::sleep_for(5ms);
        const auto sent = Clock::now();
        pair.server->send(sound(++audio));
        for (;;) {
            const auto message = pair.client->receive();
            if (const auto* packet = std::get_if<AudioPacket>(&message); packet && packet->sequence == audio) {
                break;
            }
        }
        o.late = Clock::now() - sent;
        o.queueUs = pair.client->status().soundQueueUs;
        o.waitUs = pair.server->status().soundWaitUs;
        CHECK(receivePictures(*pair.client, 1, received) == 0);
        return o;
    };
    const auto paced = outcome(47815, true), burst = outcome(47816, false);
    std::printf("path pacing: the path measured at %.1f Mbit/s; sound sent behind a 600 KB picture arrived after "
                "%.1f ms (queued %.1f ms on the path, waited %.1f ms at the sharer), without %.1f ms (%.1f ms, "
                "%.1f ms)\n",
                double(paced.pathBps) / 1e6, double(paced.late.count()) / 1e6, double(paced.queueUs) / 1e3,
                double(paced.waitUs) / 1e3, double(burst.late.count()) / 1e6, double(burst.queueUs) / 1e3,
                double(burst.waitUs) / 1e3);
    CHECK(paced.pathBps > 15'000'000 && paced.pathBps < 25'000'000 && burst.pathBps > 15'000'000);
    // Without pacing the picture queues on the path (how long depends on how fast this machine sends it).
    CHECK(paced.late < 60ms && burst.late > 2 * paced.late);
    CHECK(paced.queueUs < 60'000 && burst.queueUs > 2 * paced.queueUs);
    CHECK(paced.waitUs < 20'000 && burst.waitUs < 20'000);
}

// Sound sent while a picture is paced out (300 KB at 20 Mbit/s, 120 ms) goes
// between its datagrams: every packet arrives, in order and long before the
// picture is complete, and the picture loses no fragment to it (nothing is
// asked for again).
void soundOvertakesVideo() {
    RateRules rules;
    rules.maxPacingBps = 20'000'000;
    auto pair = connect(47817, [](uint64_t, size_t) { return false; },
                        {Connection::VideoRate{false, 0, 10'000'000, 0, rules}});
    pair.server->send(picture(1, 300'000, true));
    const auto started = Clock::now();
    for (uint32_t audio = 1; audio <= 20; ++audio) {
        // Waited out, not slept: Windows sleeps whole timer ticks (15.6 ms).
        for (const auto next = started + audio * 2ms; Clock::now() < next;) {
        }
        pair.server->send(sound(audio));
    }
    uint32_t expected = 1;
    bool pictureFirst = false;
    while (expected <= 20) {
        const auto message = pair.client->receive();
        if (const auto* packet = std::get_if<AudioPacket>(&message)) {
            CHECK(packet->sequence == expected);
            expected = packet->sequence + 1;
        } else {
            pictureFirst = pictureFirst || std::holds_alternative<EncodedPacket>(message);
        }
    }
    const auto soundDone = Clock::now() - started;
    std::vector<EncodedPacket> received;
    if (!pictureFirst) {
        CHECK(receivePictures(*pair.client, 1, received) == 0);
    }
    const auto status = pair.client->status();
    std::printf("sound beside a paced picture: 20 packets in %.1f ms, the sharer's longest wait %.2f ms\n",
                double(soundDone.count()) / 1e6, double(pair.server->status().soundWaitUs) / 1e3);
    CHECK(!pictureFirst && soundDone < 90ms);
    CHECK(status.resendRecovered == 0 && status.framesLost == 0);
    CHECK(pair.server->status().soundWaitUs < 10'000);
}

// Video far beyond what the path carries is dropped at the sharer, and so is
// what the viewer would ask for again of it: the viewer learns that those
// pictures are gone, and the next keyframe follows at once.
void backlogDropped() {
    RateRules path; // The path carries 20 Mbit/s.
    path.maxPacingBps = 20'000'000;
    auto pair =
        connect(47862, [](uint64_t, size_t) { return false; }, {Connection::VideoRate{false, 0, 2'000'000, 0, path}});
    const auto queueing = Clock::now();
    for (uint64_t i = 1; i <= 9; ++i) { // 9 MB at once: more than the sharer queues.
        pair.server->send(picture(i, 1'000'000, i == 1));
    }
    const auto started = Clock::now();
    if (started - queueing > 200ms) {
        // A slow build (sanitizers) queues slower than the path drains, and the
        // pacing follows what it is given: there is no backlog to drop.
        std::printf("backlogDropped skipped: queueing 9 MB took %lld ms\n",
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(started - queueing).count());
        return;
    }
    pair.server->send(picture(10, 20'000, true));
    std::vector<EncodedPacket> received;
    const int gaps = receivePictures(*pair.client, 1, received);
    CHECK(received[0].sequence == 10 && gaps >= 1);
    CHECK(Clock::now() - started < 1s);
    const auto status = pair.server->status(); // Counted, for rcs_network_stats.
    CHECK(status.backlogDrops >= 1 && status.backlogDroppedBytes >= 8'000'000);
}

// A picture larger than the queue's limit of 8 MiB (a keyframe of a large
// stream at a high bitrate) goes whole, and so do the pictures that follow it
// while it is still queued: the queue holds twice the largest picture.
void largePictureQueued() {
    auto pair =
        connect(47857, [](uint64_t, size_t) { return false; }, {Connection::VideoRate{false, 0, 400'000'000, 0}});
    const auto big = picture(1, 12'000'000, true);
    pair.server->send(big);
    pair.server->send(picture(2, 50'000, false));
    std::vector<EncodedPacket> received;
    CHECK(receivePictures(*pair.client, 2, received) == 0);
    CHECK(received[0].data == big.data && received[1].sequence == 2);
}

// Reliable data never goes ahead of queued video and backs off at the first
// queueing on the path, long before the video's bitrate would: through a
// bottleneck the video keeps its bitrate while data takes the rest. A still
// screen leaves data all the room in_flight_bytes allows. Control messages
// never queue behind data.
void dataYieldsToVideo() {
    test::ShapingProxy proxy(Endpoint::parse("127.0.0.1", 47863));
    proxy.bitsPerSecond = 8'000'000;
    proxy.queueMs = 500;
    auto pair = connect(
        47863, [](uint64_t, size_t) { return false; }, {Connection::VideoRate{true, 500'000, 3'000'000, 3'000'000}},
        key, {}, {}, &proxy);
    std::atomic<bool> stop{false};
    auto data = std::async(std::launch::async, [&] { // As the session's data sender: 64 KiB in flight.
        while (!stop) {
            pair.server->waitForUnackedBelow(64 * 1024, 20ms);
            if (pair.server->unackedBytes() <= 64 * 1024) {
                pair.server->send(DataPart{DataPart::data, 0, 1, 0, 1, crypto::Bytes(1024, 7)});
            }
        }
    });
    std::atomic<uint64_t> dataReceived{0}, grantSentNs{0};
    std::atomic<int64_t> grantLatencyNs{-1};
    std::atomic<int> gaps{0};
    auto receiving = std::async(std::launch::async, [&] {
        for (;;) {
            auto message = pair.client->receive();
            if (const auto* part = std::get_if<DataPart>(&message)) {
                dataReceived += part->bytes.size();
            } else if (const auto* grant = std::get_if<ControlGrant>(&message)) {
                if (grant->epoch == 2) {
                    return;
                }
                grantLatencyNs = int64_t(monotonicNs() - grantSentNs);
            } else if (std::holds_alternative<StreamGap>(message)) {
                ++gaps;
            }
        }
    });
    // 1 Mbit/s of video: the data's share of the 8 Mbit/s settles in the
    // first second; then the video's target stays where it is.
    uint32_t lowest = UINT32_MAX;
    uint64_t dataBefore = 0;
    const auto measured = Clock::now() + 1s;
    for (uint64_t i = 1; i <= 80; ++i) {
        pair.server->send(picture(i, 6'000, i == 1));
        if (i == 50) {
            grantSentNs = monotonicNs();
            pair.server->send(ControlGrant{true, 1});
        }
        std::this_thread::sleep_for(50ms);
        if (Clock::now() >= measured) {
            if (!dataBefore) {
                dataBefore = dataReceived;
            }
            lowest = std::min(lowest, pair.server->status().targetBitrate);
        }
    }
    const double runningDataBps = double(dataReceived - dataBefore) * 8 / 3;
    std::this_thread::sleep_for(600ms); // The screen is still.
    const uint64_t stillBefore = dataReceived;
    std::this_thread::sleep_for(1s);
    const double stillDataBps = double(dataReceived - stillBefore) * 8;
    pair.server->send(ControlGrant{true, 2});
    stop = true;
    data.get();
    receiving.get();
    CHECK(lowest >= 2'500'000 && gaps == 0); // The video kept its bitrate,
    CHECK(runningDataBps > 2'000'000);       // and data had a good share of the rest.
    CHECK(stillDataBps > 5'000'000);         // More than the video's pacing rate: in_flight_bytes alone holds it.
    CHECK(grantLatencyNs >= 0 && grantLatencyNs < 100'000'000);
    CHECK(proxy.dropped == 0);
}

// The other end of a connection, made by hand on its own socket.
struct RawEnd {
    UdpSocket socket;
    explicit RawEnd(uint16_t port = 0) : socket(Endpoint::parse("127.0.0.1", port)) {}
    // The next datagram of `kind` within 2 s, with its sender.
    crypto::Bytes await(uint8_t kind, Endpoint* sender = nullptr) {
        std::array<uint8_t, 2048> buffer;
        const auto deadline = Clock::now() + 2s;
        while (Clock::now() < deadline) {
            Endpoint from;
            if (const auto size = socket.receive(buffer, from)) {
                if (*size && buffer[0] == kind) {
                    if (sender) {
                        *sender = from;
                    }
                    return crypto::Bytes(buffer.begin(), buffer.begin() + ptrdiff_t(*size));
                }
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
        CHECK(false);
        return {};
    }
};

constexpr auto derivationGapForTests = 500ms; // udp_connection.cpp's derivationGap.

crypto::Bytes framed(uint8_t kind, std::span<const uint8_t> body) {
    crypto::Bytes datagram{kind};
    datagram.insert(datagram.end(), body.begin(), body.end());
    return datagram;
}

// A client that handshakes, then sends frames the test makes up, sealed as a
// real peer's are; `forgedFirst`: a finished message that fails goes ahead of
// the real one, as anyone could send it from the client's address.
// An authenticated peer that sends frames of its own making.
struct RawPeer : RawEnd {
    using RawEnd::RawEnd;
    Endpoint peer;
    transport::TrafficKeys keys;
    uint64_t packet = 0;
    crypto::Bytes seal(std::span<const uint8_t> frame) {
        crypto::Bytes datagram{0xD8};
        appendBigEndian(datagram, keys.sendId, 4);
        appendBigEndian(datagram, ++packet, 8);
        const auto sealed = keys.send->seal(packet, datagram, frame);
        datagram.insert(datagram.end(), sealed.begin(), sealed.end());
        return datagram;
    }
    void send(std::span<const uint8_t> frame) { socket.send(peer, seal(frame)); }
    // A ping, as a real peer's (it follows to new paths).
    static crypto::Bytes ping() {
        crypto::Bytes frame{1};
        appendBigEndian(frame, monotonicNs(), 8);
        frame.push_back(1);
        return frame;
    }
};

struct RawClient : RawPeer {
    RawClient(const Endpoint& serverAddress, bool forgedFirst = false) {
        peer = serverAddress;
        transport::ClientHandshake client(credentials);
        socket.send(peer, helloDatagram(client.hello()));
        const auto answer = await(0xD2);
        const auto finished = client.finish(std::span(answer).subspan(1), keys);
        if (forgedFirst) {
            auto forged = finished;
            forged.back() ^= 1;
            socket.send(peer, framed(0xD3, forged));
        }
        socket.send(peer, framed(0xD3, finished));
    }
};

// The server's end: answers the first client's handshake.
struct RawServer : RawPeer {
    explicit RawServer(uint16_t port) : RawPeer(port) {}
    void accept() {
        const auto hello = await(0xD1, &peer);
        const transport::ServerHandshake server(credentials, std::span(hello).subspan(1, transport::clientHelloSize));
        socket.send(peer, framed(0xD2, server.answer()));
        const auto finished = await(0xD3);
        server.finish(std::span(finished).subspan(1), keys);
    }
};

std::unique_ptr<UdpConnection> listen(const Endpoint& local, std::atomic<bool>& stop,
                                      const UdpConnection::Rejected& rejected = {}, const UdpOptions& options = {}) {
    return UdpConnection::establish(UdpConnection::Side::server, std::make_unique<UdpSocket>(local), nullptr,
                                    credentials, options, stop, rejected);
}

// Only the authenticated peer can send frames, so one that no peer of this
// version makes ends the connection with the reason. The server sends video
// and the client receives it: neither takes what only the other sends (a
// viewer cannot make its sharer hold pictures).
void invalidFrames() {
    const auto local = Endpoint::parse("127.0.0.1", 47864);
    const auto ended = [](UdpConnection& connection) {
        try {
            for (;;) {
                connection.receive();
            }
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_PROTOCOL);
            CHECK(std::string(failure.what()).find("Invalid") != std::string::npos);
        }
    };
    // A frame: these bytes, then `body`.
    const auto frame = [](crypto::Bytes head, std::span<const uint8_t> body = {}) {
        head.insert(head.end(), body.begin(), body.end());
        return head;
    };
    const auto fragment = [](uint32_t id, uint16_t index, uint16_t count, size_t size) {
        crypto::Bytes bytes{5};
        appendBigEndian(bytes, id, 4);
        appendBigEndian(bytes, index, 2);
        appendBigEndian(bytes, count, 2);
        appendBigEndian(bytes, 0, 4);
        bytes.resize(bytes.size() + size, 0x42);
        return bytes;
    };
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    const auto input = encode(Input{move, 1}), request = encode(KeyframeRequest{});
    const std::vector<crypto::Bytes> toServer{
        fragment(1, 1, 2, 1),               // Video, its first fragment held back.
        frame({11, 0, 0, 0, 0}),            // Video the sender no longer has.
        frame({99, 1, 2, 3}),               // A type no version defines.
        frame({8, 0, 0, 0, 1}),             // A report cut short.
        frame({6, 0, 0, 0, 9, 0, 0}),       // A resend request for video never sent.
        frame({9}, input),                  // Sound that is input.
        frame({10}, request),               // A datagram that is a control message.
        frame({3, 1, 0, 0, 0, 1}, request), // A control message on the data stream.
        frame({3, 7, 0, 0, 0, 1}, request), // A stream no version defines.
    };
    for (const auto& frame : toServer) {
        std::atomic<bool> stop{false};
        auto server = std::async(std::launch::async, [&] { return listen(local, stop); });
        std::this_thread::sleep_for(30ms);
        RawClient client(local);
        const auto connection = server.get();
        client.send(frame);
        ended(*connection);
    }
    const std::vector<crypto::Bytes> toClient{
        frame({5, 0, 0, 0, 1, 0, 0, 0, 1, 0}), // A video fragment cut short of its header,
        fragment(1, 0, 1, 0),                  // one without any video,
        fragment(1u << 31, 0, 1, 1),           // one far ahead of the frames before it,
        fragment(1, 0, 65'000, 1),             // one of more than the largest message takes,
        fragment(1, 2, 2, 1),                  // one beyond its frame's count.
        frame({6, 0, 0, 0, 1, 0, 0}),          // A resend request.
    };
    static std::atomic<bool> never{false};
    for (const auto& frame : toClient) {
        RawServer server(47876);
        auto accepted = std::async(std::launch::async, [&] { server.accept(); });
        const auto connection = UdpConnection::establish(
            UdpConnection::Side::client, std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)),
            directLink(Endpoint::parse("127.0.0.1", 47876)), credentials, {}, never);
        accepted.get();
        server.send(frame);
        ended(*connection);
    }
}

// A request to resend comes in the peer's datagrams, which may be larger than
// this end's (a host's channel of another size): the answer that the video is
// gone still goes in this end's.
void goneInOwnDatagrams() {
    const auto local = Endpoint::parse("127.0.0.1", 47878);
    std::atomic<bool> stop{false};
    auto server = std::async(std::launch::async, [&] { return listen(local, stop); });
    std::this_thread::sleep_for(30ms);
    RawClient client(local);
    const auto connection = server.get();
    constexpr uint32_t frames = 250;
    for (uint32_t id = 1; id <= frames; ++id) {
        connection->send(picture(id, 100, id == 1));
    }
    std::this_thread::sleep_for(1300ms); // Kept for a second after they left: gone now.
    crypto::Bytes nack{6};
    for (uint32_t id = 1; id <= frames; ++id) {
        nack.push_back(0);
        appendBigEndian(nack, id, 4);
        appendBigEndian(nack, 0, 2);
    }
    CHECK(nack.size() + 13 + crypto::Aead::tagSize > udpDatagram); // More than this end's datagram.
    client.send(nack);
    std::set<uint32_t> gone;
    for (const auto deadline = Clock::now() + 2s; gone.size() < frames && Clock::now() < deadline;) {
        const auto datagram = client.await(0xD8);
        const auto frame = client.keys.receive->open(loadBigEndian(datagram, 5, 8), std::span(datagram).first(13),
                                                     std::span(datagram).subspan(13));
        if (!frame || frame->empty() || (*frame)[0] != 11) {
            continue;
        }
        CHECK(datagram.size() <= udpDatagram);
        for (size_t offset = 1; offset + 5 <= frame->size(); offset += 5) {
            gone.insert(uint32_t(loadBigEndian(*frame, offset + 1, 4)));
        }
    }
    CHECK(gone.size() == frames);
}

// SA-03: what waits for the session's receive() is bounded (maxInbound) when
// the session falls behind (a host's callback holds its thread) or the
// authenticated peer floods it. Reliable messages wait unacknowledged, as
// TCP's would, and all of them arrive in order once the session reads again;
// unreliable data beyond the bound is dropped; complete pictures beyond it
// are given up on, the session told by a gap before the stream's next picture.
void behindTheSession() {
    constexpr size_t bound = 1u << 20;
    UdpOptions options;
    options.maxInbound = bound;
    {
        // 12 MiB of reliable data to a server that reads nothing for a while:
        // the sender gets no further than the bound, what the receiver holds
        // early (4096 parts) and its own window.
        auto pair = connect(47846, [](uint64_t, size_t) { return false; }, options);
        constexpr size_t part = 1024, parts = 12 * 1024, window = 1u << 20;
        std::atomic<size_t> sent{0};
        auto sending = std::async(std::launch::async, [&] {
            for (uint32_t i = 0; i < parts; ++i) {
                while (pair.client->unackedBytes() > window) {
                    pair.client->waitForUnackedBelow(window, 20ms);
                }
                pair.client->send(DataPart{DataPart::data, 0, 1, i, parts, crypto::Bytes(part, uint8_t(i))});
                ++sent;
            }
        });
        const auto deadline = Clock::now() + 3s;
        while (Clock::now() < deadline && !(sent == parts && pair.client->unackedBytes() == 0)) {
            std::this_thread::sleep_for(20ms);
        }
        const size_t acknowledged = sent * (part + 20) - std::min(sent * (part + 20), pair.client->unackedBytes());
        CHECK(acknowledged < bound + 4096 * (part + 20) + (1u << 20));
        for (uint32_t i = 0; i < parts; ++i) {
            const auto message = pair.server->receive();
            const auto& got = std::get<DataPart>(message);
            CHECK(got.index == i && got.bytes == crypto::Bytes(part, uint8_t(i)));
        }
        sending.get();
    }
    {
        // An authenticated peer floods unreliable data, then sends a control
        // message: no more than the bound of the data waits ahead of it.
        const auto local = Endpoint::parse("127.0.0.1", 47847);
        std::atomic<bool> stop{false};
        auto server = std::async(std::launch::async, [&] { return listen(local, stop, {}, options); });
        std::this_thread::sleep_for(30ms);
        RawClient client(local);
        const auto connection = server.get();
        const auto datagram = encode(DataDatagram{0, 1, crypto::Bytes(1000, 7)});
        for (int i = 0; i < 8000; ++i) { // 8 MB.
            client.send(framed(10, datagram));
            if (i % 50 == 49) {
                std::this_thread::sleep_for(1ms);
            }
        }
        crypto::Bytes control{3, 0, 0, 0, 0, 1};
        const auto request = encode(KeyframeRequest{});
        control.insert(control.end(), request.begin(), request.end());
        client.send(control);
        std::this_thread::sleep_for(300ms);
        size_t waited = 0;
        for (;;) {
            const auto message = connection->receive();
            if (std::holds_alternative<KeyframeRequest>(message)) {
                break;
            }
            waited += std::get<DataDatagram>(message).bytes.size();
        }
        CHECK(waited > 0 && waited <= bound);
    }
    {
        // 30 pictures of 100 KB while the viewer reads nothing: those beyond
        // the bound are given up on; a gap goes before the next picture.
        auto pair = connect(47849, [](uint64_t, size_t) { return false; }, options);
        for (uint64_t i = 1; i <= 30; ++i) {
            pair.server->send(picture(i, 100'000, i == 1));
        }
        std::this_thread::sleep_for(500ms);
        pair.server->send(picture(31, 10'000, true));
        std::vector<EncodedPacket> received;
        size_t before = 0;
        bool gap = false;
        while (received.empty() || received.back().sequence != 31) {
            auto message = pair.client->receive();
            if (auto* packet = std::get_if<EncodedPacket>(&message)) {
                before += gap ? 0 : packet->data.size();
                received.push_back(std::move(*packet));
            } else if (std::holds_alternative<StreamGap>(message)) {
                CHECK(!received.empty() && received.back().sequence < 30); // Told before what follows it.
                gap = true;
            }
        }
        CHECK(gap && before <= bound && received.size() < 31);
        CHECK(pair.client->status().framesLost >= 1);
    }
}

// Handshake datagrams are not authenticated by themselves: junk or a forged
// one ahead of the real one costs neither end its handshake. An answer that
// fails is reported only when no valid one came in the meantime.
void spoofedHandshakes() {
    {
        const auto local = Endpoint::parse("127.0.0.1", 47865);
        std::atomic<bool> stop{false};
        auto server = std::async(std::launch::async, [&] { return listen(local, stop); });
        std::this_thread::sleep_for(30ms);
        RawClient client(local, true);
        const auto connection = server.get();
        crypto::Bytes message{3, 0, 0, 0, 0, 1};
        const auto body = encode(KeyframeRequest{});
        message.insert(message.end(), body.begin(), body.end());
        client.send(message);
        CHECK(std::holds_alternative<KeyframeRequest>(connection->receive()));
    }
    const auto connectTo = [](const Endpoint& server, std::atomic<bool>& stop) {
        return std::async(std::launch::async, [server, &stop] {
            return UdpConnection::establish(UdpConnection::Side::client,
                                            std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)),
                                            directLink(server), credentials, {}, stop);
        });
    };
    auto otherKey = key;
    otherKey[0] = 9;
    const auto other = transport::Credentials::fromKey(otherKey);
    {
        RawEnd server(47868);
        std::atomic<bool> stop{false};
        auto client = connectTo(server.socket.local(), stop);
        Endpoint from;
        const auto hello = server.await(0xD1, &from);
        const auto body = std::span(hello).subspan(1, transport::clientHelloSize);
        server.socket.send(from, crypto::Bytes{0xD2, 1, 2, 3});
        const transport::ServerHandshake forged(other, body), real(credentials, body);
        server.socket.send(from, framed(0xD2, forged.answer()));
        server.socket.send(from, framed(0xD2, real.answer()));
        const auto finished = server.await(0xD3);
        transport::TrafficKeys keys;
        real.finish(std::span(finished).subspan(1), keys);
        CHECK(client.get() != nullptr);
    }
    {
        RawEnd server(47868);
        std::atomic<bool> stop{false};
        auto client = connectTo(server.socket.local(), stop);
        Endpoint from;
        const auto hello = server.await(0xD1, &from);
        const transport::ServerHandshake forged(other, std::span(hello).subspan(1, transport::clientHelloSize));
        server.socket.send(from, framed(0xD2, forged.answer()));
        const auto answered = Clock::now();
        try {
            const auto connection = client.get();
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_AUTHENTICATION);
            CHECK(Clock::now() - answered >= 900ms && Clock::now() - answered < 2s);
        }
    }
}

// A hello from an address with a handshake under way, which anyone could
// send with that address, has to prove the address with a cookie before it
// replaces that handshake: the real client finishes all the same.
void helloNotReplaced() {
    const auto local = Endpoint::parse("127.0.0.1", 47841);
    std::atomic<bool> stop{false};
    auto server = std::async(std::launch::async, [&] { return listen(local, stop); });
    std::this_thread::sleep_for(30ms);
    RawPeer client;
    client.peer = local;
    transport::ClientHandshake real(credentials), forged(credentials);
    client.socket.send(local, helloDatagram(real.hello()));
    const auto answer = client.await(0xD2);
    client.socket.send(local, helloDatagram(forged.hello()));
    CHECK(client.await(0xD4).size() == 1 + 16); // A cookie, not an answer.
    client.socket.send(local, framed(0xD3, real.finish(std::span(answer).subspan(1), client.keys)));
    const auto connection = server.get();
    crypto::Bytes message{3, 0, 0, 0, 0, 1};
    const auto body = encode(KeyframeRequest{});
    message.insert(message.end(), body.begin(), body.end());
    client.send(message);
    CHECK(std::holds_alternative<KeyframeRequest>(connection->receive()));
}

// Answers from the sharer's address that would each make a password viewer
// run PBKDF2 (salts it has no key for) are tried one per half second, the
// newest first: a flood of forged ones costs it two derivations, not one each.
void forgedAnswersCostLittle() {
    transport::Credentials password;
    password.secret = RCS_SECRET_PASSWORD;
    password.password = "correct horse";
    password.passwordKeys = std::make_shared<transport::PasswordKeys>();
    const auto timed = Clock::now();
    transport::PasswordKeys().derive("timing", transport::PasswordKeys::Salt{});
    const auto derivation = Clock::now() - timed;

    RawEnd sharer(47840);
    std::atomic<bool> stop{false};
    const auto started = Clock::now();
    auto viewer = std::async(std::launch::async, [&] {
        return UdpConnection::establish(UdpConnection::Side::client,
                                        std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)),
                                        directLink(sharer.socket.local()), password, {}, stop);
    });
    Endpoint from;
    const auto hello = sharer.await(0xD1, &from);
    for (uint8_t salt = 1; salt <= 10; ++salt) {
        crypto::Bytes forged(transport::serverHelloSize, salt);
        forged[0] = RCS_SECRET_PASSWORD;
        sharer.socket.send(from, framed(0xD2, forged));
    }
    const transport::ServerHandshake real(password, std::span(hello).subspan(1, transport::clientHelloSize));
    sharer.socket.send(from, framed(0xD2, real.answer()));
    const auto finished = sharer.await(0xD3);
    transport::TrafficKeys keys;
    real.finish(std::span(finished).subspan(1), keys);
    CHECK(viewer.get() != nullptr);
    // The sharer's own derivation, the first forged answer's and the real one's.
    CHECK(Clock::now() - started < derivationGapForTests + 5 * derivation);
}

// Another protocol version learns this end's, at the sharer and at the
// viewer; and a flood of bad hellos reaches the host as at most 16 reports
// in 5 s.
void versionsAndFloods() {
    const auto local = Endpoint::parse("127.0.0.1", 47869);
    std::atomic<bool> stop{false};
    std::atomic<int> reported{0};
    auto server = std::async(std::launch::async, [&] {
        try {
            listen(local, stop, [&](const Failure&) { ++reported; });
        } catch (const Failure&) {
        }
    });
    std::this_thread::sleep_for(30ms);
    RawEnd peer;
    auto hello = transport::ClientHandshake(credentials).hello();
    hello[7] ^= 1;
    peer.socket.send(local, helloDatagram(hello));
    CHECK(peer.await(0xD5) == framed(0xD5, transport::versionNotice()));
    for (int i = 0; i < 200; ++i) {
        peer.socket.send(local, crypto::Bytes{0xD1, 1, 2, 3});
    }
    std::this_thread::sleep_for(200ms);
    CHECK(reported == 16);
    stop = true;
    server.get();

    RawEnd sharer(47870);
    stop = false;
    auto viewer = std::async(std::launch::async, [&] {
        return UdpConnection::establish(UdpConnection::Side::client,
                                        std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)),
                                        directLink(sharer.socket.local()), credentials, {}, stop);
    });
    Endpoint from;
    sharer.await(0xD1, &from);
    auto notice = framed(0xD5, transport::versionNotice());
    notice.back() ^= 1;
    sharer.socket.send(from, notice);
    try {
        const auto connection = viewer.get();
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_UNSUPPORTED);
        CHECK(std::string(failure.what()).find("version " + std::to_string(notice.back())) != std::string::npos);
    }
}

void busyServer() {
    // Strays fill the server's 16 handshakes and never finish them, and a
    // flood of hellos from other addresses goes on. The viewer still gets in
    // at once: the busy server hands it a cookie, and its hello with the
    // cookie replaces the oldest stray.
    std::atomic<bool> stop{false}, flooding{true};
    const auto local = Endpoint::parse("127.0.0.1", 47858);
    auto server = std::async(std::launch::async, [&] {
        return UdpConnection::establish(UdpConnection::Side::server, std::make_unique<UdpSocket>(local), nullptr,
                                        credentials, {}, stop);
    });
    std::this_thread::sleep_for(50ms);
    const auto hello = [] { return helloDatagram(transport::ClientHandshake(credentials).hello()); };
    std::vector<std::unique_ptr<UdpSocket>> strays;
    for (int i = 0; i < 16; ++i) {
        strays.push_back(std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)));
        strays.back()->send(local, hello());
    }
    auto flood = std::async(std::launch::async, [&] {
        while (flooding) {
            UdpSocket(Endpoint::parse("127.0.0.1", 0)).send(local, hello());
            std::this_thread::sleep_for(1ms);
        }
    });
    struct Finish { // Also when establish() throws, so the threads above end.
        std::atomic<bool>& stop;
        std::atomic<bool>& flooding;
        ~Finish() {
            flooding = false;
            stop = true;
        }
    } finish{stop, flooding};
    std::this_thread::sleep_for(100ms);
    // A stray that starts over from its address must prove it receives there
    // first: a new hello replaces no attempt (and costs no key pair) without a cookie.
    strays[0]->send(local, hello());
    std::array<uint8_t, 2048> buffer;
    Endpoint from;
    bool retried = false;
    for (const auto deadline = Clock::now() + 1s; !retried && Clock::now() < deadline;) {
        const auto size = strays[0]->receive(buffer, from);
        retried = size && buffer[0] == 0xD4;
        if (!size) {
            std::this_thread::sleep_for(1ms);
        }
    }
    CHECK(retried);
    const auto started = Clock::now();
    auto client = UdpConnection::establish(UdpConnection::Side::client,
                                           std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)),
                                           directLink(local), credentials, {}, stop);
    CHECK(Clock::now() - started < 2s);
    flooding = false;
    const auto connection = server.get();
    client->send(KeyframeRequest{});
    CHECK(std::holds_alternative<KeyframeRequest>(connection->receive()));
}

void closeAndKeys() {
    {
        auto pair = connect(47854, [](uint64_t, size_t) { return false; });
        pair.client.reset(); // Stopping tells the peer at once.
        const auto started = Clock::now();
        try {
            pair.server->receive();
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_DISCONNECTED);
        }
        CHECK(Clock::now() - started < 1s);
    }
    {
        auto pair = connect(47858, [](uint64_t, size_t) { return false; });
        pair.client->leaving(Goodbye::restarting); // A restart says it will be back.
        pair.client.reset();
        try {
            pair.server->receive();
            CHECK(false);
        } catch (const Failure& failure) {
            CHECK(failure.result() == RCS_ERROR_RESTARTING);
        }
    }
    // A message queued just before the connection is dropped still arrives, ahead of the close.
    for (uint64_t round = 1; round <= 20; ++round) {
        auto pair = connect(47866, [](uint64_t, size_t) { return false; });
        pair.server->send(ControlGrant{true, round});
        pair.server.reset();
        CHECK(std::get<ControlGrant>(pair.client->receive()).epoch == round);
        CHECK(throws<Failure>([&] { pair.client->receive(); }));
    }
    // A wrong key: the client fails authentication (once a valid answer had
    // time to follow the one that failed). The server, which only answered a
    // hello, reports the attempt when it expires.
    std::atomic<int> rejected{0};
    std::mutex reasonMutex;
    std::string reason;
    auto wrong = key;
    wrong[3] = 1;
    static std::atomic<bool> stop{false};
    stop = false;
    const auto local = Endpoint::parse("127.0.0.1", 47855);
    auto server = std::async(std::launch::async, [&] {
        try {
            listen(local, stop, [&](const Failure& failure) {
                std::lock_guard lock(reasonMutex);
                ++rejected;
                reason = failure.what();
            });
        } catch (const Failure&) {
        }
    });
    std::this_thread::sleep_for(50ms);
    try {
        UdpConnection::establish(UdpConnection::Side::client,
                                 std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)), directLink(local),
                                 transport::Credentials::fromKey(wrong), {}, stop);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_AUTHENTICATION);
    }
    // Nothing listening: the client gives up after 5 s with a reason.
    const auto started = Clock::now();
    try {
        UdpConnection::establish(UdpConnection::Side::client,
                                 std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)),
                                 directLink(Endpoint::parse("127.0.0.1", 47856)), credentials, {}, stop);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_NETWORK &&
              std::string(failure.what()).find("No answer") != std::string::npos);
    }
    CHECK(Clock::now() - started >= 4900ms);
    stop = true;
    server.get();
    std::lock_guard lock(reasonMutex);
    CHECK(rejected == 1 && reason.find("No finished message") != std::string::npos);
}

// A sharer on a path chosen for one peer (P2P, relay) that only sees that
// peer fail its handshake says why when it gives up.
void linkedGivesUp() {
    static std::atomic<bool> never{false};
    auto wrong = key;
    wrong[5] = 1;
    const auto local = Endpoint::parse("127.0.0.1", 47871);
    auto clientSocket = std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0));
    const auto clientAddress = clientSocket->local();
    std::atomic<int> rejected{0};
    auto server = std::async(std::launch::async, [&] {
        return UdpConnection::establish(UdpConnection::Side::server, std::make_unique<UdpSocket>(local),
                                        directLink(clientAddress), credentials, {}, never,
                                        [&](const Failure&) { ++rejected; });
    });
    CHECK(throws<Failure>([&] {
        UdpConnection::establish(UdpConnection::Side::client, std::move(clientSocket), directLink(local),
                                 transport::Credentials::fromKey(wrong), {}, never);
    }));
    try {
        const auto connection = server.get();
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_NETWORK);
        CHECK(std::string(failure.what()).find("(last: No finished message") != std::string::npos);
    }
    CHECK(rejected == 1);
}

void endpoints() {
    CHECK(Endpoint::parse("192.168.1.2", 5).toString() == "192.168.1.2:5");
    CHECK(Endpoint::parse("[2001:db8::1]", 6).toString() == "[2001:db8::1]:6");
    CHECK(Endpoint::parse("::1", 7).ipv6() && !Endpoint::parse("10.0.0.1", 7).ipv6());
    CHECK(throws<Failure>([] { Endpoint::parse("example.com", 1); }));
    CHECK(Endpoint::parse("10.0.0.1", 9) == Endpoint::fromBytes(Endpoint::parse("10.0.0.1", 9).addressBytes(), 9));
    // A link-local address on two interfaces is two endpoints.
    CHECK(Endpoint::parse("fe80::1%1", 9) != Endpoint::parse("fe80::1%2", 9));
    CHECK(Endpoint::parse("fe80::1%1", 9) == Endpoint::parse("fe80::1%1", 9));

    // A datagram larger than the buffer is dropped, not cut short.
    UdpSocket receiver(Endpoint::parse("127.0.0.1", 0)), sender(Endpoint::parse("127.0.0.1", 0));
    sender.send(receiver.local(), crypto::Bytes(1500, 1));
    sender.send(receiver.local(), crypto::Bytes(50, 2));
    std::this_thread::sleep_for(20ms);
    std::array<uint8_t, 100> buffer;
    Endpoint from;
    const auto size = receiver.receive(buffer, from);
    CHECK(size == 50u && buffer[0] == 2 && from == sender.local());
    CHECK(!receiver.receive(buffer, from));
}

// Whether `done` came true within `wait`.
template <class Done>
bool waitFor(Done&& done, std::chrono::milliseconds wait = 3s) {
    for (const auto deadline = Clock::now() + wait; !done();) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// The path changes the connection reported since, waiting up to `wait` for `count` of them.
std::vector<PathEvent> pathEvents(UdpConnection& connection, size_t count, std::chrono::milliseconds wait = 3s) {
    std::vector<PathEvent> events;
    for (const auto deadline = Clock::now() + wait; events.size() < count && Clock::now() < deadline;) {
        for (auto& event : connection.takePathEvents()) {
            events.push_back(std::move(event));
        }
        std::this_thread::sleep_for(10ms);
    }
    return events;
}

// Receives pictures (and other messages) in the background until it goes.
struct Watching {
    explicit Watching(UdpConnection& connection)
        : connection(connection), thread([this, &connection] {
              try {
                  for (;;) {
                      auto message = connection.receive();
                      std::lock_guard lock(mutex);
                      if (auto* packet = std::get_if<EncodedPacket>(&message)) {
                          sequences.push_back(packet->sequence);
                      } else if (std::holds_alternative<StreamGap>(message)) {
                          ++gaps;
                      } else if (const auto* input = std::get_if<Input>(&message)) {
                          inputs.push_back(input->epoch);
                      }
                  }
              } catch (const Failure& failure) {
                  std::lock_guard lock(mutex);
                  ended = failure.result();
              }
          }) {}
    ~Watching() {
        connection.cancel();
        thread.join();
    }
    size_t pictures() {
        std::lock_guard lock(mutex);
        return sequences.size();
    }
    UdpConnection& connection;
    std::mutex mutex;
    std::vector<uint64_t> sequences, inputs;
    int gaps = 0;
    std::optional<rcs_result> ended;
    std::thread thread; // Last.
};

// The peer's NAT remaps it (another port, the same address): the sharer
// proves the new port and follows it there, keeping its bitrate (only the
// port changed); the pictures go on, and the viewer sees no change at all.
void followsRebinding() {
    test::ShapingProxy proxy(Endpoint::parse("127.0.0.1", 47882));
    UdpOptions options;
    options.rate = Connection::VideoRate{true, 500'000, 8'000'000, 4'000'000};
    auto pair = connect(47882, [](uint64_t, size_t) { return false; }, options, key, {}, {}, &proxy);
    Watching viewer(*pair.client);
    uint64_t sequence = 0;
    const auto pictures = [&](int count) {
        for (int i = 0; i < count; ++i) {
            ++sequence;
            pair.server->send(picture(sequence, 3'000, sequence == 1));
            std::this_thread::sleep_for(10ms);
        }
    };
    pictures(30);
    const auto before = *pair.server->peerAddress();
    const uint32_t target = pair.server->status().targetBitrate;
    proxy.rebind();
    pictures(50);
    const auto events = pathEvents(*pair.server, 1);
    CHECK(events.size() == 1 && events[0].change == RCS_PATH_CHANGED && events[0].cause == RCS_PATH_CAUSE_REBINDING);
    const auto after = *pair.server->peerAddress();
    CHECK(after != before && after.address() == before.address() && after == proxy.backAddress());
    CHECK(events[0].from.remote == before && events[0].to.remote == after);
    const auto status = pair.server->status();
    CHECK(status.pathChanges == 1 && status.targetBitrate >= target / 2);
    CHECK(pair.client->takePathEvents().empty());
    pictures(30);
    CHECK(waitFor([&] { return viewer.pictures() + size_t(viewer.gaps) >= 100; }, 5s));
}

// This end moves to a socket of its own (a new network): it proves the path
// first, then the traffic goes there; the peer follows (it was challenged
// first: it knows), and the messages arrive once each, in order.
void movesItself() {
    auto pair = connect(47883, [](uint64_t, size_t) { return false; });
    Watching sharer(*pair.server);
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    uint64_t epoch = 0;
    for (; epoch < 20; ++epoch) {
        pair.client->send(Input{move, epoch + 1});
    }
    auto socket = std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0));
    const auto moved = socket->local();
    pair.client->addRoute(std::move(socket), directLink(Endpoint::parse("127.0.0.1", 47883)));
    for (; epoch < 60; ++epoch) {
        pair.client->send(Input{move, epoch + 1});
        std::this_thread::sleep_for(5ms);
    }
    const auto own = pathEvents(*pair.client, 1), followed = pathEvents(*pair.server, 1);
    CHECK(own.size() == 1 && own[0].change == RCS_PATH_CHANGED && own[0].cause == RCS_PATH_CAUSE_NETWORK);
    CHECK(own[0].to.local && own[0].to.local->port() == moved.port());
    CHECK(followed.size() == 1 && followed[0].cause == RCS_PATH_CAUSE_PEER && followed[0].to.remote == moved);
    CHECK(pair.server->peerAddress() == moved);
    CHECK(waitFor([&] {
        std::lock_guard lock(sharer.mutex);
        return sharer.inputs.size() >= 60;
    }));
    std::lock_guard lock(sharer.mutex);
    for (size_t i = 0; i < sharer.inputs.size(); ++i) {
        CHECK(sharer.inputs[i] == i + 1);
    }
}

// Someone with the keys (here: a test that reuses them; on the network, an
// attacker that rewrote a captured datagram's source) sends from an address
// that cannot answer: the connection challenges it, never more than three
// times what came from there, the real peer's newer traffic keeps the
// connection where it is, and the proof is given up.
void unprovenAddress() {
    const auto local = Endpoint::parse("127.0.0.1", 47884);
    std::atomic<bool> stop{false};
    auto server = std::async(std::launch::async, [&] { return listen(local, stop); });
    std::this_thread::sleep_for(30ms);
    RawClient client(local);
    const auto connection = server.get();
    client.send(RawPeer::ping());
    std::this_thread::sleep_for(100ms);
    RawEnd stranger;
    const auto forged = client.seal(RawPeer::ping());
    stranger.socket.send(local, forged);
    size_t toStranger = 0;
    std::array<uint8_t, 2048> buffer;
    for (const auto deadline = Clock::now() + 2500ms; Clock::now() < deadline;) {
        client.send(RawPeer::ping()); // The real peer goes on where it was.
        Endpoint from;
        while (const auto size = stranger.socket.receive(buffer, from)) {
            toStranger += *size;
        }
        std::this_thread::sleep_for(100ms);
    }
    CHECK(toStranger > 0 && toStranger <= 3 * forged.size()); // A challenge went there, within the limit.
    CHECK(connection->peerAddress() == client.socket.local());
    CHECK(connection->takePathEvents().empty());
}

// The path goes silent: changing after path_stall, lost after idle_timeout,
// kept for `hold`; once it is back the connection goes on and what was sent
// meanwhile arrives. Silent longer than idle_timeout and `hold`: it fails.
// Without migration at one end, it fails at idle_timeout as before.
void keptWhileLost() {
    std::atomic<bool> outage{false};
    const auto cut = [&](uint64_t, size_t) {
        return outage.load();
    };
    UdpOptions options;
    options.idleTimeout = 1s;
    options.pathStall = 300ms;
    options.hold = 3s;
    auto pair = connect(47885, cut, options, key, {}, cut);
    Watching sharer(*pair.server);
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    outage = true;
    std::this_thread::sleep_for(100ms);
    pair.client->send(Input{move, 1}); // On its way while the path is out.
    const auto silent = pathEvents(*pair.server, 2, 2s);
    CHECK(silent.size() == 2 && silent[0].change == RCS_PATH_CHANGING && silent[0].cause == RCS_PATH_CAUSE_STALLED);
    CHECK(silent[1].change == RCS_PATH_LOST && pair.server->status().lost);
    outage = false;
    const auto back = pathEvents(*pair.server, 1);
    CHECK(back.size() == 1 && back[0].change == RCS_PATH_CHANGED && back[0].cause == RCS_PATH_CAUSE_STALLED);
    CHECK(back[0].interruptionNs >= 1'000'000'000 && back[0].to == back[0].from);
    CHECK(waitFor([&] {
        std::lock_guard lock(sharer.mutex);
        return sharer.inputs.size() == 1;
    }));
    CHECK(!pair.server->status().lost && pair.server->status().pathChanges == 1);
    outage = true; // For good.
    CHECK(waitFor(
        [&] {
            std::lock_guard lock(sharer.mutex);
            return sharer.ended.has_value();
        },
        6s));
    {
        std::lock_guard lock(sharer.mutex);
        CHECK(*sharer.ended == RCS_ERROR_NETWORK);
    }
    // Without migration at one end: silence fails it at idle_timeout.
    outage = false;
    options.migrate = false;
    auto plain = connect(47886, cut, options, key, {}, cut);
    Watching viewer(*plain.client);
    outage = true;
    const auto started = Clock::now();
    CHECK(waitFor(
        [&] {
            std::lock_guard lock(viewer.mutex);
            return viewer.ended.has_value();
        },
        4s));
    CHECK(Clock::now() - started < 2500ms);
}

// A path that comes and goes, where nobody takes the connection's path
// events (a forwarder's ends): the connection keeps the latest only.
void pathEventsBounded() {
    std::atomic<bool> outage{false};
    const auto cut = [&](uint64_t, size_t) {
        return outage.load();
    };
    UdpOptions options;
    options.pathStall = 20ms;
    auto pair = connect(47889, cut, options, key, {}, cut);
    Watching sharer(*pair.server), viewer(*pair.client);
    rcs_input_event move = RCS_INPUT_EVENT_INIT;
    move.type = RCS_INPUT_MOVE;
    uint64_t epoch = 0;
    // Both ends send all along (they look for silence every few milliseconds then).
    for (const auto deadline = Clock::now() + 20s; pair.server->status().pathChanges < 50 && Clock::now() < deadline;) {
        outage = !outage;
        for (const auto until = Clock::now() + (outage ? 80ms : 40ms); Clock::now() < until;) {
            pair.client->send(Input{move, ++epoch});
            pair.server->send(Input{move, epoch});
            std::this_thread::sleep_for(5ms);
        }
    }
    outage = false;
    const auto changes = pair.server->status().pathChanges;
    const auto events = pair.server->takePathEvents();
    CHECK(changes >= 50);
    CHECK(!events.empty() && events.size() <= 64);
    std::printf("path events: %zu kept of the %llu changes and as many stalls\n", events.size(),
                (unsigned long long)changes);
}

// A sharer that no longer knows the connection (the viewer's place went to
// another while its path was out) answers its datagrams so; the viewer, whose
// path is silent, takes it and fails at once: it connects anew.
void unknownConnection() {
    static std::atomic<bool> never{false};
    const auto local = Endpoint::parse("127.0.0.1", 47887);
    UdpDemux demux(local);
    UdpAcceptor acceptor(demux, transport::fixedCredentials(credentials), {}, 5s);
    test::ShapingProxy proxy(local);
    UdpOptions options;
    options.pathStall = 300ms;
    options.hold = 30s;
    auto client = std::async(std::launch::async, [&] {
        return UdpConnection::establish(UdpConnection::Side::client,
                                        std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)),
                                        directLink(proxy.address()), credentials, options, never);
    });
    auto server = acceptor.accept([] { return UdpOptions{}; }, never);
    auto viewer = client.get();
    Watching watching(*viewer);
    std::atomic<bool> stop{false};
    auto listening = std::async(std::launch::async, [&] { // Answers what comes for no connection.
        try {
            acceptor.accept([] { return UdpOptions{}; }, stop);
        } catch (const Failure&) {
        }
    });
    proxy.outage = true;
    server.reset(); // Its goodbye is lost.
    std::this_thread::sleep_for(600ms);
    const auto started = Clock::now();
    proxy.outage = false;
    CHECK(waitFor([&] {
        std::lock_guard lock(watching.mutex);
        return watching.ended.has_value();
    }));
    const auto events = viewer->takePathEvents();
    CHECK(events.size() == 1 && events[0].change == RCS_PATH_CHANGING);
    CHECK(Clock::now() - started < 2s);
    CHECK(throws<UnknownConnection>([&] { viewer->receive(); }));
    stop = true;
    listening.get();
}

} // namespace

// One port for several connections (a sharer's viewers): handshakes that run
// at once all complete, each connection gets its own peer's data only, a
// peer that fails its handshake keeps no one out, the credentials are taken
// at each hello, and a connection that ends leaves the others as they were.
void demultiplexed() {
    static std::atomic<bool> never{false};
    const auto local = Endpoint::parse("127.0.0.1", 47859);
    UdpDemux demux(local);
    std::mutex mutex;
    auto current = std::make_shared<const transport::Credentials>(credentials);
    UdpAcceptor acceptor(demux,
                         [&] {
                             std::lock_guard lock(mutex);
                             return current;
                         },
                         {}, 5s);
    const auto connect = [&](const transport::Credentials& own) {
        return UdpConnection::establish(UdpConnection::Side::client,
                                        std::make_unique<UdpSocket>(Endpoint::parse("127.0.0.1", 0)), directLink(local),
                                        own, {}, never);
    };
    // Three clients at once, and one with another key.
    std::vector<std::future<std::unique_ptr<UdpConnection>>> clients;
    for (int i = 0; i < 3; ++i) {
        clients.push_back(std::async(std::launch::async, [&] { return connect(credentials); }));
    }
    const std::array<uint8_t, 32> otherKey{1, 2, 3};
    const auto other = transport::Credentials::fromKey(otherKey);
    auto stranger = std::async(std::launch::async, [&] { return throws<Failure>([&] { connect(other); }); });
    std::vector<std::unique_ptr<UdpConnection>> servers;
    for (int i = 0; i < 3; ++i) {
        servers.push_back(acceptor.accept([] { return UdpOptions{}; }, never));
    }
    std::vector<std::unique_ptr<UdpConnection>> connected;
    for (auto& client : clients) {
        connected.push_back(client.get());
    }
    // Each server connection hears its own client only, whichever order they
    // completed in, and answers it alone.
    for (size_t i = 0; i < connected.size(); ++i) {
        connected[i]->send(DataDatagram{0, uint32_t(i), {uint8_t(i)}});
    }
    std::vector<uint32_t> clientOf; // Of each server connection.
    for (auto& server : servers) {
        const auto message = server->receive();
        const auto* datagram = std::get_if<DataDatagram>(&message);
        CHECK(datagram && datagram->bytes.size() == 1 && datagram->bytes[0] == datagram->channel);
        clientOf.push_back(datagram->channel);
        server->send(DataDatagram{0, datagram->channel, {uint8_t(datagram->channel + 10)}});
    }
    CHECK(std::set<uint32_t>(clientOf.begin(), clientOf.end()).size() == 3);
    for (size_t i = 0; i < connected.size(); ++i) {
        const auto message = connected[i]->receive();
        const auto* datagram = std::get_if<DataDatagram>(&message);
        CHECK(datagram && datagram->channel == i && datagram->bytes[0] == i + 10);
    }
    // Video through each: its fragments, and the requests for them, routed by sender.
    for (auto& server : servers) {
        server->send(picture(1, 100'000, true));
    }
    for (auto& client : connected) {
        std::vector<EncodedPacket> pictures;
        CHECK(receivePictures(*client, 1, pictures) == 0 && pictures[0].data.size() == 100'000);
    }
    // The stranger cannot finish its handshake; the others got in meanwhile.
    CHECK(stranger.get());
    // New credentials apply from the next hello.
    {
        std::lock_guard lock(mutex);
        current = std::make_shared<const transport::Credentials>(other);
    }
    auto renewed = std::async(std::launch::async, [&] { return connect(other); });
    auto fourth = acceptor.accept([] { return UdpOptions{}; }, never);
    auto client = renewed.get();
    client->send(DataDatagram{0, 4, {4}});
    CHECK(std::holds_alternative<DataDatagram>(fourth->receive()));
    // A connection that ends: its client hears so, the others go on.
    const uint32_t ended = clientOf[0];
    servers[0].reset();
    for (size_t i = 1; i < servers.size(); ++i) {
        servers[i]->send(DataDatagram{0, 9, {9}});
    }
    CHECK(throws<Failure>([&] {
        for (;;) {
            connected[ended]->receive();
        }
    }));
    for (size_t i = 0; i < connected.size(); ++i) {
        if (i != ended) {
            const auto message = connected[i]->receive();
            CHECK(std::holds_alternative<DataDatagram>(message) && std::get<DataDatagram>(message).channel == 9);
        }
    }
}

int main() {
    return runTest("udp", [] {
        auto linked = std::async(std::launch::async, linkedGivesUp); // 10 s, beside the rest.
        endpoints();
        reliableAndLarge();
        numbersWrap();
        lossRecovered();
        parityRepairs();
        parityFollowsLoss();
        marking();
        batching();
        batchedSocket("127.0.0.1");
        batchedSocket("::1");
        soundMarkedInBatches();
        soundBetweenPictures();
        outages();
        reorderBufferFull();
        slowPaths();
        pacing();
        pathPacing();
        soundOvertakesVideo();
        backlogDropped();
        largePictureQueued();
        dataYieldsToVideo();
        invalidFrames();
        goneInOwnDatagrams();
        behindTheSession();
        spoofedHandshakes();
        helloNotReplaced();
        forgedAnswersCostLittle();
        versionsAndFloods();
        busyServer();
        demultiplexed();
        closeAndKeys();
        followsRebinding();
        movesItself();
        unprovenAddress();
        keptWhileLost();
        pathEventsBounded();
        unknownConnection();
        linked.get();
    });
}
