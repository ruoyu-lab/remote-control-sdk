#include "protocol/udp_connection.hpp"

#include "protocol/fec.hpp"
#include "protocol/rate_control.hpp"
#include "transport/host_channel.hpp"
#include "transport/network_changes.hpp"
#include "transport/secure_channel.hpp"
#include "transport/stun.hpp"
#include "util/bytes.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

namespace rcsdk::protocol {
namespace {

using crypto::Bytes;
using transport::DatagramSocket;
using transport::Endpoint;

constexpr size_t maxAttempts = 16; // Handshakes a server works on at once.
constexpr size_t cookieSize = 16;
// A hello's datagram after its kind: the hello, the cookie, zeros up to the
// ServerHello's size (helloDatagram).
constexpr size_t helloBody = transport::serverHelloSize;
static_assert(helloBody >= transport::clientHelloSize + cookieSize);
constexpr size_t dataHeader = 1 + 4 + 8;
constexpr size_t unknownSize = 1 + 4; // 0xD7 cid(4).
enum FrameType : uint8_t {
    pingFrame = 1,
    pongFrame = 2,
    messageFrame = 3,
    ackFrame = 4,
    videoFrame = 5,
    nackFrame = 6,
    closeFrame = 7,
    reportFrame = 8,
    audioFrame = 9,
    datagramFrame = 10,  // An unreliable data message: like audio, never resent.
    goneFrame = 11,      // Video the sender no longer has, in answer to a NACK.
    parityFrame = 12,    // Reed-Solomon parity of a group of a video frame's fragments (fec.hpp).
    challengeFrame = 13, // PATH_CHALLENGE: a path's proof...
    responseFrame = 14   // ...and its answer.
};
// The reliable streams, each delivered in order: control messages go at once,
// data parts after the video and within a congestion window of their own.
enum StreamId : uint8_t { controlStream = 0, dataStream = 1 };
constexpr size_t messageHeader = 1 + 1 + 4;
// VIDEO: stream(1) frame(4) index(2) count(2) sent(4) parity(1), the parity
// fragments of the fragment's group (0: none). PARITY: stream(1) frame(4)
// group(2) count(2) sent(4) parity(1) which(1) last(2), the length of the
// group's last video fragment (the others are as long as the parity). Video
// fragments go in groups of fecGroup, each followed by its parity. Each
// stream numbers its frames from 1.
constexpr size_t videoHeader = 1 + 1 + 4 + 2 + 2 + 4 + 1;
constexpr size_t parityHeader = 1 + 1 + 4 + 2 + 2 + 4 + 1 + 1 + 2;
constexpr size_t parityExtra = parityHeader - videoHeader; // Less video per fragment while parity goes.
constexpr size_t fecGroup = 64;
constexpr auto parityHold = 5s; // AUTO: parity goes on this long after the last loss reported.
constexpr size_t sentOffset = 10; // The send time inside a VIDEO or PARITY frame.
constexpr size_t nackEntry = 1 + 4 + 2, goneEntry = 1 + 4;
constexpr size_t streams = RCS_MAX_STREAMS;
constexpr size_t timeFrameSize = 1 + 8 + 1;
constexpr uint8_t migrates = 1; // A ping's flag: this end follows its peer to a new path.
constexpr size_t reportSize = 1 + 4 + 4 + 4 + 8 + 8 + 4 + 4 + 8;
constexpr size_t audioHeader = 1 + 4; // AUDIO sent(4).
constexpr size_t probeData = 8;
constexpr size_t probeSize = 1 + probeData;
constexpr auto reportInterval = 50ms;
// The path's rate from a picture of at least rateFragments none of which were
// asked for again, from what came after its first bunch (an aggregate a Wi-Fi
// receiver gets at once, whose airtime went before it: arrivals less than
// bunchGap apart): where the path spaced those fragments (they took a
// twentieth and 200 us longer to arrive than to leave), its bottleneck's rate,
// their bytes over the time since the bunch; where it did not, and the sharer
// took at least pacedTime to send them (it paced them), the path carried at
// least the rate they left at. Either way no faster than the bottleneck: what
// comes after an aggregate takes airtime of its own.
constexpr auto bunchGap = 50us;
constexpr uint16_t rateFragments = 8;
constexpr auto pacedTime = 2ms;
constexpr uint16_t wholeFrame = 0xFFFF;

constexpr auto handshakeInterval = 250ms;
// Anyone can send an answer that fails; the real server's answer to the
// repeated hellos still wins within this time.
constexpr auto refusalGrace = 1s;
// A client tries an answer that costs PBKDF2 (a salt it has no key for) at
// once, and another one only after this: forged answers from the server's
// address cannot keep it deriving keys.
constexpr auto derivationGap = 500ms;
constexpr auto pingInterval = 250ms;
constexpr auto keepSent = 1s;      // Video kept for resending after it left.
constexpr auto reorderDelay = 3ms; // Arriving out of order before a missing fragment counts as lost.
// Silence within a frame before its rest counts as lost, when nothing later
// arrived: senders pause between batches (pacing, a busy phone's scheduler)
// and Wi-Fi between its bursts for milliseconds. Longer pauses seen lately
// count too, up to mostPause.
constexpr auto leastQuiet = 10ms, mostPause = 20ms;
constexpr int maxNacks = 6;
// Queued video beyond this, and beyond twice the largest picture queued since
// the queue was last empty (a large stream's keyframe goes, however large),
// means the path cannot keep up: drop and restart.
constexpr size_t maxQueuedVideo = 8u << 20;
// What came early on a reliable stream, held: messages, and their bytes (a
// host's channel carries datagrams of up to 64 KiB).
constexpr size_t maxOutOfOrder = 4096;
constexpr size_t maxEarlyBytes = 16u << 20;
constexpr size_t maxDataWindow = 16u << 20; // Reliable data in flight at most.
// Datagrams sealed before they go to the system together (Link::sendBatch):
// no more wait for the first of them, nor are held in memory. Windows sends
// each as it is sealed, as before (batches not measured there).
#ifdef _WIN32
constexpr size_t maxBatch = 1;
#else
constexpr size_t maxBatch = 64;
#endif
// The smallest datagram a sender fills: a UDP one that a TURN Send
// indication over IPv6 (what adds the most) wraps, the smallest a host's
// channel may carry. Every message fits its fragment, and so the most
// fragments one video message takes follows from it.
static_assert(RCS_HOST_MIN_DATAGRAM == udpDatagram - 64);
constexpr size_t minFragment = RCS_HOST_MIN_DATAGRAM - dataHeader - crypto::Aead::tagSize - parityHeader;
constexpr size_t maxFragments = transport::maxRecord / minFragment + 1;
static_assert(maxFragments < wholeFrame, "a message's fragments are numbered in 16 bits, 0xFFFF meaning all of them");
// Fragments of frames waiting to be completed: four of the largest messages,
// more than a sender keeps.
constexpr size_t maxPendingFragments = 4 * maxFragments;
// How far a frame may be ahead of the oldest incomplete one: more than arrive
// in 17 s at 240 FPS, far longer than an incomplete frame is waited for.
constexpr uint32_t frameWindow = 4096;
constexpr auto summaryInterval = 5s;
constexpr int maxReports = 16; // Handshake failures told to the host per summaryInterval.
// Paths: proving one tries for this long at least (three retransmission
// timeouts where longer), sending its challenge again with backoff; at most
// this many paths begin to be proven a second; a proven one is taken again
// without a new proof for a minute; so many of the peer's addresses are
// followed at once (beyond, the oldest unproven goes); a route nothing went
// over for this long goes.
constexpr auto leastProving = 1s;
constexpr int provingPerSecond = 4;
constexpr auto provenFor = 60s;
constexpr size_t maxCandidates = 16;
constexpr auto unusedRoute = 10s;
// After the network changed, a path that answers nothing within a round trip
// and this long is looked past.
constexpr auto networkCheckGrace = 300ms;
// Path events not taken yet, at most: a peer whose path comes and goes adds
// a few a second, and some owners never take them (a forwarder's ends).
constexpr size_t maxPathEvents = 64;
// An ordered path (Link::ordered) loses nothing on the way, only delays it:
// what is missing on it is asked for, and what is unacknowledged sent again,
// after this at the soonest; a picture that does not complete is given up
// after twice this (by then the sender answered what it no longer has).
constexpr auto orderedWait = 1s;

uint64_t get(std::span<const uint8_t> in, size_t offset, size_t width) {
    if (offset + width > in.size()) {
        fail(RCS_ERROR_PROTOCOL, "Invalid frame from the peer: truncated");
    }
    return loadBigEndian(in, offset, width);
}

// Only the authenticated peer sends frames: a malformed one ends the connection.
[[noreturn]] void invalid(const std::string& what) {
    fail(RCS_ERROR_PROTOCOL, "Invalid frame from the peer: " + what);
}

Bytes framed(uint8_t kind, std::span<const uint8_t> body) {
    Bytes datagram{kind};
    datagram.insert(datagram.end(), body.begin(), body.end());
    return datagram;
}

bool transmit(DatagramSocket& socket, const Endpoint& to, std::span<const uint8_t> datagram, uint8_t dscp) {
    if (dscp) {
        return socket.sendMarked(to, datagram, dscp);
    }
    socket.send(to, datagram);
    return false;
}

size_t transmit(DatagramSocket& socket, const Endpoint& to, std::span<transport::Outgoing> batch) {
    for (auto& datagram : batch) {
        datagram.to = &to;
    }
    return socket.sendBatch(batch);
}

class DirectLink final : public Link {
public:
    explicit DirectLink(const Endpoint& peer) : peer_(peer) {}
    bool send(DatagramSocket& socket, std::span<const uint8_t> datagram, uint8_t dscp) override {
        return transmit(socket, peer_, datagram, dscp);
    }
    size_t sendBatch(DatagramSocket& socket, std::span<transport::Outgoing> batch) override {
        return transmit(socket, peer_, batch);
    }
    void sendTo(DatagramSocket& socket, const Endpoint& to, std::span<const uint8_t> datagram) override {
        socket.send(to, datagram);
    }
    std::optional<std::span<const uint8_t>> unwrap(DatagramSocket&, Endpoint&,
                                                   std::span<const uint8_t> datagram) override {
        return datagram;
    }
    size_t overhead() const noexcept override { return 0; }
    std::string describe() const override { return "direct UDP to " + peer_.toString(); }
    std::optional<Endpoint> peer() const override { return peer_; }
    void setPeer(const Endpoint& peer) override { peer_ = peer; }
    rcs_path_route route() const override { return RCS_ROUTE_DIRECT; }

private:
    Endpoint peer_;
};

class HostLink final : public Link {
public:
    // To the other end at `to` (a route's stand-in), over a channel of `route`'s kind.
    HostLink(std::string description, const Endpoint& to, rcs_path_route route)
        : description_(std::move(description)), to_(to), route_(route) {}
    bool send(DatagramSocket& socket, std::span<const uint8_t> datagram, uint8_t) override {
        socket.send(to_, datagram);
        return false;
    }
    // At once where the channel takes batches (a forwarder's TCP and WebSocket entries).
    size_t sendBatch(DatagramSocket& socket, std::span<transport::Outgoing> batch) override {
        return transmit(socket, to_, batch);
    }
    std::optional<std::span<const uint8_t>> unwrap(DatagramSocket&, Endpoint&,
                                                   std::span<const uint8_t> datagram) override {
        return datagram; // Only the other end sends over the host's channel.
    }
    size_t overhead() const noexcept override { return 0; }
    std::string describe() const override { return description_; }
    bool awaitsPeer() const noexcept override { return true; }
    std::string silence() const override {
        return route_ == RCS_ROUTE_FORWARDER
                   ? "the sharer is not in the forwarder's room, or does not answer through it"
                   : "the other end's session does not run, or the host's channel does not reach it";
    }
    rcs_path_route route() const override { return route_; }

private:
    std::string description_;
    Endpoint to_;
    rcs_path_route route_;
};

// A TCP connection to the peer: what goes over it arrives in order, unmarked.
// A send that finds it broken is a datagram lost: reading it says so.
class TcpLink final : public Link {
public:
    explicit TcpLink(const Endpoint& peer) : peer_(peer) {}
    bool send(DatagramSocket& socket, std::span<const uint8_t> datagram, uint8_t) override {
        sendTo(socket, peer_, datagram);
        return false;
    }
    void sendTo(DatagramSocket& socket, const Endpoint& to, std::span<const uint8_t> datagram) override {
        try {
            socket.send(to, datagram);
        } catch (const Failure&) {
        }
    }
    // In one write (FramedSocket, TcpDemux).
    size_t sendBatch(DatagramSocket& socket, std::span<transport::Outgoing> batch) override {
        try {
            transmit(socket, peer_, batch);
        } catch (const Failure&) {
        }
        return 0;
    }
    std::optional<std::span<const uint8_t>> unwrap(DatagramSocket&, Endpoint&,
                                                   std::span<const uint8_t> datagram) override {
        return datagram;
    }
    size_t overhead() const noexcept override { return 0; }
    std::string describe() const override { return "TCP to " + peer_.toString(); }
    std::optional<Endpoint> peer() const override { return peer_; }
    void setPeer(const Endpoint& peer) override { peer_ = peer; }
    rcs_path_route route() const override { return RCS_ROUTE_DIRECT; }
    bool ordered() const noexcept override { return true; }
    std::string silence() const override { return "nothing listens there over TCP, or it is not the sharer"; }

private:
    Endpoint peer_;
};

// The peer may still be checking or nominating (a lost answer): ICE links keep answering.
class IceLink final : public Link {
public:
    IceLink(const Endpoint& peer, const std::string& ufrag, const std::string& password, transport::ice::Type type,
            const Endpoint& local)
        : peer_(peer), ufrag_(ufrag), key_(password.begin(), password.end()), type_(type), local_(local) {}
    bool send(DatagramSocket& socket, std::span<const uint8_t> datagram, uint8_t dscp) override {
        return transmit(socket, peer_, datagram, dscp);
    }
    size_t sendBatch(DatagramSocket& socket, std::span<transport::Outgoing> batch) override {
        return transmit(socket, peer_, batch);
    }
    void sendTo(DatagramSocket& socket, const Endpoint& to, std::span<const uint8_t> datagram) override {
        socket.send(to, datagram);
    }
    std::optional<std::span<const uint8_t>> unwrap(DatagramSocket& socket, Endpoint& from,
                                                   std::span<const uint8_t> datagram) override {
        if (transport::stun::looksLikeStun(datagram)) {
            if (const auto check = transport::stun::parseCheck(datagram, ufrag_, key_)) {
                socket.send(from, transport::stun::answerCheck(*check, from, key_));
            }
            return std::nullopt;
        }
        return datagram;
    }
    size_t overhead() const noexcept override { return 0; }
    std::string describe() const override {
        return "P2P " + local_.toString() + " <-> " + peer_.toString() + " (" + transport::ice::typeName(type_) + ")";
    }
    std::optional<Endpoint> peer() const override { return peer_; }
    void setPeer(const Endpoint& peer) override {
        if (peer != peer_) {
            peer_ = peer;
            type_ = transport::ice::Type::peerReflexive; // Learned from the peer's own datagrams.
        }
    }
    rcs_path_route route() const override {
        return type_ == transport::ice::Type::host ? RCS_ROUTE_DIRECT : RCS_ROUTE_NAT;
    }

private:
    Endpoint peer_;
    std::string ufrag_;
    Bytes key_;
    transport::ice::Type type_;
    Endpoint local_;
};

// The peer is reached at the addresses the allocation has permissions for:
// the one ICE chose, and those its path's restarts add. Once its allocation
// moved to another socket (moveTo, RFC 8016) it carries nothing more.
class RelayLink final : public Link {
public:
    RelayLink(std::unique_ptr<transport::TurnClient> relay, const Endpoint& peer, const std::string& ufrag,
              const std::string& password, transport::ice::Type type, rcs_turn_transport transport)
        : relay_(std::move(relay)), peer_(peer), ufrag_(ufrag), key_(password.begin(), password.end()), type_(type),
          transport_(transport), server_(relay_->server()), relayed_(relay_->relayed()) {}
    // Whether its allocation may move to another socket of this end's (UDP to the server, a mobility ticket).
    bool movable() const noexcept { return relay_ && transport_ == RCS_TURN_UDP && relay_->mobile(); }
    // The allocation, moved to `socket` (its Refresh is sent; the answer
    // comes there): the new link carries the traffic from now on.
    std::unique_ptr<RelayLink> moveTo(DatagramSocket& socket) {
        relay_->move(socket);
        auto moved = std::make_unique<RelayLink>(std::move(relay_), peer_, ufrag_,
                                                 std::string(key_.begin(), key_.end()), type_, transport_);
        return moved;
    }
    bool send(DatagramSocket& socket, std::span<const uint8_t> datagram, uint8_t dscp) override {
        return relay_ && relay_->send(socket, peer_, datagram, dscp);
    }
    void sendTo(DatagramSocket& socket, const Endpoint& to, std::span<const uint8_t> datagram) override {
        if (relay_) {
            relay_->send(socket, to, datagram);
        }
    }
    std::optional<std::span<const uint8_t>> unwrap(DatagramSocket& socket, Endpoint& from,
                                                   std::span<const uint8_t> datagram) override {
        if (!relay_ || from != relay_->server()) {
            return std::nullopt;
        }
        const auto relayed = relay_->receive(socket, datagram);
        if (!relayed || !relay_->permitted(relayed->first)) {
            return std::nullopt;
        }
        from = relayed->first;
        if (transport::stun::looksLikeStun(relayed->second)) {
            if (const auto check = transport::stun::parseCheck(relayed->second, ufrag_, key_)) {
                relay_->send(socket, from, transport::stun::answerCheck(*check, from, key_));
            }
            return std::nullopt;
        }
        return relayed->second;
    }
    size_t overhead() const noexcept override { return relay_ ? relay_->overhead(peer_) : 0; }
    std::string describe() const override {
        return "relay " + relayed_.toString() + " on TURN " + server_.toString() +
               (transport_ == RCS_TURN_TCP   ? " (over TCP)"
                : transport_ == RCS_TURN_TLS ? " (over TLS)"
                                             : "") +
               " <-> " + peer_.toString() + " (" + transport::ice::typeName(type_) + ")";
    }
    std::optional<Endpoint> peer() const override { return peer_; }
    void setPeer(const Endpoint& peer) override {
        if (peer != peer_) {
            peer_ = peer;
            type_ = transport::ice::Type::peerReflexive;
        }
    }
    std::optional<Endpoint> firstHop() const override { return server_; }
    bool reaches(const Endpoint& address) const override { return relay_ && relay_->permitted(address); }
    rcs_path_route route() const override { return RCS_ROUTE_RELAY; }
    rcs_turn_transport relayTransport() const override { return transport_; }
    void tick(DatagramSocket& socket, Clock::time_point now) override {
        if (relay_) {
            relay_->tick(socket, now);
        }
    }
    void close(DatagramSocket& socket) noexcept override {
        try {
            if (relay_) {
                relay_->release(socket);
            }
        } catch (...) {
        }
    }

private:
    std::unique_ptr<transport::TurnClient> relay_; // None once it moved.
    Endpoint peer_;
    std::string ufrag_;
    Bytes key_;
    transport::ice::Type type_;
    rcs_turn_transport transport_;
    Endpoint server_, relayed_;
};

// Packet numbers already seen, up to 1024 behind the highest.
class ReplayWindow {
public:
    bool fresh(uint64_t number) const noexcept {
        if (number == 0) {
            return false;
        }
        if (number > highest_) {
            return true;
        }
        const uint64_t back = highest_ - number;
        return back < seen_.size() && !seen_[size_t(back)];
    }
    void mark(uint64_t number) noexcept {
        if (number > highest_) {
            const uint64_t shift = number - highest_;
            seen_ = shift >= seen_.size() ? std::bitset<1024>() : seen_ << size_t(shift);
            highest_ = number;
            seen_[0] = true;
        } else {
            seen_[size_t(highest_ - number)] = true;
        }
    }
    uint64_t highest() const noexcept { return highest_; }

private:
    uint64_t highest_ = 0;
    std::bitset<1024> seen_; // Bit i: highest_ - i.
};

// A message's number as the wire carries it (its low 32 bits), back to the
// whole number nearest `reference`: those in flight are far less than 2^31
// apart, so numbers go on past 2^32.
uint64_t unwrapped(uint64_t reference, uint32_t wire) noexcept {
    return reference + uint64_t(int64_t(int32_t(wire - uint32_t(reference))));
}

// The sending half of a reliable stream: messages are kept until acknowledged
// and sent again when no acknowledgement came in time, the wait doubling up
// to a second.
struct Outgoing {
    struct Entry {
        Bytes frame;
        int attempts = 0;
        bool ready = false;      // In `ready`, waiting for its next transmission.
        Clock::time_point due{}; // When it goes again unless acknowledged.
    };
    std::map<uint64_t, Entry> unacked;
    std::deque<uint64_t> ready; // To send now: new, or due again.
    std::multimap<Clock::time_point, uint64_t> timers;
    size_t bytes = 0;  // Unacknowledged.
    size_t onWire = 0; // Unacknowledged, and sent at least once.
    uint64_t nextId = 1;

    void add(Bytes frame) {
        bytes += frame.size();
        unacked[nextId] = Entry{std::move(frame), 0, true, {}};
        ready.push_back(nextId++);
    }
    // `id` went out now; `timeout`: the first wait for its acknowledgement.
    void sent(uint64_t id, Entry& entry, Clock::time_point now, Clock::duration timeout) {
        if (!entry.attempts) {
            onWire += entry.frame.size();
        }
        entry.ready = false;
        ++entry.attempts;
        entry.due = now + std::min<Clock::duration>(timeout * (1 << std::min(entry.attempts, 5)), 1s);
        timers.emplace(entry.due, id);
    }
    // What is due again goes first: what is new may wait for room on the
    // path, which only acknowledgements of what is sent already make.
    void expire(Clock::time_point now) {
        std::vector<uint64_t> again;
        while (!timers.empty() && timers.begin()->first <= now) {
            const auto [due, id] = *timers.begin();
            timers.erase(timers.begin());
            const auto found = unacked.find(id);
            if (found != unacked.end() && found->second.due == due && !found->second.ready) {
                found->second.ready = true;
                again.push_back(id);
            }
        }
        ready.insert(ready.begin(), again.begin(), again.end());
    }
    // The path changed: what waits for its acknowledgement goes again now.
    void resendNow() {
        std::vector<uint64_t> again;
        for (auto& [id, entry] : unacked) {
            if (entry.attempts && !entry.ready) {
                entry.ready = true;
                entry.attempts = 1; // Its wait starts over on the new path.
                again.push_back(id);
            }
        }
        ready.insert(ready.begin(), again.begin(), again.end());
    }
    // Bytes the acknowledgement took off the wire.
    size_t acknowledge(uint32_t wire) {
        const auto found = unacked.find(unwrapped(nextId, wire));
        if (found == unacked.end()) {
            return 0;
        }
        const size_t size = found->second.frame.size();
        const bool wasSent = found->second.attempts != 0;
        bytes -= size;
        unacked.erase(found);
        if (!wasSent) {
            return 0;
        }
        onWire -= size;
        return size;
    }
};

// The receiving half: delivers in order, holds what came early.
struct Incoming {
    uint64_t next = 1;
    std::map<uint64_t, Bytes> early; // From `next` on: those arrived, the next ones waiting for room.
    size_t earlyBytes = 0;
    std::vector<uint32_t> acks; // As the wire carries them.
};

// Video kept after it was queued, until a second after it left: fragments
// the receiver may ask for again.
struct SentFragment {
    Bytes frame;              // The VIDEO frame; its send time is stamped as it leaves.
    bool queued = true;       // Waiting in the video queue (first time or again).
    Clock::time_point sent{}; // Its last transmission.
};
struct SentFrame {
    std::vector<SentFragment> fragments; // The video fragments, then the parity.
    uint16_t videoCount = 0;
    size_t bytes = 0;  // Of all its fragments.
    size_t queued = 0; // Fragments waiting in the video queue.
    Clock::time_point lastSent{};
};

// The path's rate as the receiver measures pictures arriving (sender): the
// upper quartile of the last second's rates of pictures the path spaced (one
// may have met other stations' airtime or a receiver's thread that read late,
// and pacing below the path would hold video back), or else the latest of the
// last 10 s (a still screen's pictures are too small to tell). A picture that
// arrived faster without being spaced shows that the path is faster now: the
// rate starts again from it.
class PathRate {
public:
    void spaced(uint64_t bps, Clock::time_point now) {
        samples_.emplace_back(now, bps);
        while (samples_.size() > 1 && now - samples_.front().first > 1s) {
            samples_.pop_front();
        }
        std::vector<uint64_t> rates;
        for (const auto& [time, rate] : samples_) {
            rates.push_back(rate);
        }
        const auto quartile = rates.begin() + ptrdiff_t(rates.size() * 3 / 4);
        std::nth_element(rates.begin(), quartile, rates.end());
        quartile_ = *quartile;
    }
    void unspaced(uint64_t bps, Clock::time_point now) {
        if (const uint64_t known = this->bps(now); known && bps > known) {
            samples_.clear();
            spaced(bps, now);
        }
    }
    uint64_t bps(Clock::time_point now) const {
        return !samples_.empty() && now - samples_.back().first <= 10s ? quartile_ : 0;
    }
    // Another path: what the last one measured says nothing of it.
    void reset() noexcept {
        samples_.clear();
        quartile_ = 0;
    }

private:
    std::deque<std::pair<Clock::time_point, uint64_t>> samples_;
    uint64_t quartile_ = 0;
};

// Sound or unreliable data waiting to go, and since when.
struct Immediate {
    Bytes frame;
    Clock::time_point queued;
};

// The most of a measure over the last one to two seconds: written by the
// network thread, read by any.
class RecentMost {
public:
    void add(uint64_t value, Clock::time_point now) noexcept {
        roll(now);
        current_ = std::max(current_, value);
        most_ = std::max(previous_, current_);
    }
    void roll(Clock::time_point now) noexcept {
        if (now - started_ < 1s) {
            return;
        }
        previous_ = now - started_ < 2s ? current_ : 0;
        current_ = 0;
        started_ = now;
        most_ = previous_;
    }
    uint64_t most() const noexcept { return most_; }

private:
    uint64_t current_ = 0, previous_ = 0;
    Clock::time_point started_{};
    std::atomic<uint64_t> most_{0};
};

// Sound's one-way delay (µs, arbitrary origin, as video's), and its lowest
// of each second over the last 10 s: what is above it queued on the path.
class SoundDelay {
public:
    // The queueing of sound sent at `sent` (the sender's 32-bit microseconds)
    // that arrived at `arrivedNs`.
    uint64_t queueing(uint32_t sent, uint64_t arrivedNs, Clock::time_point now) {
        const uint32_t raw = uint32_t(arrivedNs / 1000) - sent;
        delay_ = previousRaw_ ? delay_ + int32_t(raw - *previousRaw_) : raw;
        previousRaw_ = raw;
        if (lowest_.empty() || now - lowest_.back().first >= 1s) {
            lowest_.emplace_back(now, delay_);
        }
        lowest_.back().second = std::min(lowest_.back().second, delay_);
        while (now - lowest_.front().first > 10s) {
            lowest_.pop_front();
        }
        int64_t lowest = delay_;
        for (const auto& [time, value] : lowest_) {
            lowest = std::min(lowest, value);
        }
        return uint64_t(delay_ - lowest);
    }
    // Another path: its delay starts over (the last one's lowest is no queueing's base).
    void reset() noexcept {
        previousRaw_.reset();
        delay_ = 0;
        lowest_.clear();
    }

private:
    std::optional<uint32_t> previousRaw_;
    int64_t delay_ = 0;
    std::deque<std::pair<Clock::time_point, int64_t>> lowest_;
};

} // namespace

std::unique_ptr<Link> directLink(const Endpoint& peer) {
    return std::make_unique<DirectLink>(peer);
}

std::unique_ptr<Link> iceLink(const Endpoint& peer, const std::string& localUfrag, const std::string& localPassword,
                              transport::ice::Type type, const Endpoint& local) {
    return std::make_unique<IceLink>(peer, localUfrag, localPassword, type, local);
}

std::unique_ptr<Link> relayLink(std::unique_ptr<transport::TurnClient> relay, const Endpoint& peer,
                                const std::string& localUfrag, const std::string& localPassword,
                                transport::ice::Type type, rcs_turn_transport transport) {
    return std::make_unique<RelayLink>(std::move(relay), peer, localUfrag, localPassword, type, transport);
}

std::unique_ptr<Link> hostLink(std::string description) {
    return std::make_unique<HostLink>(std::move(description), transport::HostSocket::placeholder(), RCS_ROUTE_HOST);
}

std::unique_ptr<Link> routeLink(std::string description, const Endpoint& to, rcs_path_route route) {
    return std::make_unique<HostLink>(std::move(description), to, route);
}

std::unique_ptr<Link> tcpLink(const Endpoint& peer) {
    return std::make_unique<TcpLink>(peer);
}

struct UdpConnection::Impl {
    Impl(std::unique_ptr<DatagramSocket> socketIn, std::unique_ptr<Link> linkIn, transport::TrafficKeys keys,
         CancelToken cancelledIn, const UdpOptions& options, Side side)
        : maxInbound(options.maxInbound), sendsVideo(options.videoFrom == side),
          receivesVideo(options.videoFrom != side), side(side), datagramLimit(options.maxDatagram),
          receiveBuffer(options.maxReceived), peerDatagram(options.maxDatagram), idleTimeout(options.idleTimeout),
          pathStall(options.pathStall), hold(options.hold), standbyProbe(options.standbyProbe),
          decideRate(options.decideRate), reopen(options.reopen), sendKeys(std::move(keys.send)),
          receiveKeys(std::move(keys.receive)), sendId(keys.sendId), receiveId(keys.receiveId), exporter(keys.exporter),
          runCancelled(cancelledIn), dataWindow(initialDataWindow()) {
        audioDscp = options.audioDscp;
        migrate = options.migrate;
        for (auto& stream : outgoing) {
            stream.nextId = options.firstMessageId;
        }
        for (auto& stream : incoming) {
            stream.next = options.firstMessageId;
        }
        // The handshake's path is proven: the peer answered over it.
        auto& first = addRoute(std::move(socketIn), std::move(linkIn), Clock::now());
        current = &candidateFor(first, first.link->peer(), Clock::now());
        current->proven = true;
        current->provenAt = current->heard = current->firstHeard = Clock::now();
        updateFragmentSize();
    }

    const size_t maxInbound; // UdpOptions.
    // One end sends video, the other receives it (UdpOptions::videoFrom,
    // setVideoFrom): neither takes what only the other end sends (pictures
    // to hold, requests to resend).
    std::atomic<bool> sendsVideo, receivesVideo;
    const Side side;
    // The largest datagram this end sends; what a datagram is read into (the
    // largest this end takes); the largest the peer sent so far (its
    // fragments' size, for timing). The last two: network thread only.
    const size_t datagramLimit;
    Bytes receiveBuffer;
    size_t peerDatagram;
    const std::chrono::milliseconds idleTimeout, pathStall, hold, standbyProbe;       // UdpOptions.
    const std::function<void(const rcs_rate_report&, rcs_rate_decision&)> decideRate; // UdpOptions.
    const std::function<std::unique_ptr<DatagramSocket>(bool ipv6)> reopen;           // UdpOptions.

    // Reliable data's congestion window: where it starts, the least it keeps.
    size_t initialDataWindow() const noexcept { return 16 * datagramLimit; }
    size_t minDataWindow() const noexcept { return 2 * datagramLimit; }

    // ---- Shared with session threads (mutex) ----------------------------------

    struct QueuedFragment {
        uint8_t stream;
        uint32_t frame;
        uint16_t index;
    };

    std::mutex mutex;
    std::condition_variable inboundReady;
    std::condition_variable acked; // Unacknowledged data went down (or the connection failed).
    std::array<Outgoing, 2> outgoing;
    std::deque<QueuedFragment> video;
    size_t videoBytes = 0;
    size_t largestQueued = 0; // The largest picture queued since the queue was last empty, in bytes.
    // What sends the queued video within the frame spread, from the newest
    // picture on (bits per second); 0 once it all left. Written holding mutex.
    std::atomic<uint64_t> spreadBps{0};
    // Sound and unreliable data: sent at once, ahead of video (between any two
    // of its datagrams) and of everything else; never resent.
    std::deque<Immediate> immediate;
    std::array<std::map<uint32_t, SentFrame>, streams> sentFrames; // By stream.
    std::array<uint32_t, streams> nextFrameId{1, 1, 1, 1};
    // Video while the path is silent goes nowhere: one frame number for all
    // of it, which the receiver finds missing and recovers from.
    std::array<bool, streams> skipped{};
    // Messages for receive(), each with what it takes (deliver()), and all of
    // them together; whether the network thread waits for room (receive()
    // wakes it once half of maxInbound is free).
    struct Delivered {
        Message message;
        size_t bytes;
    };
    std::deque<Delivered> inbound;
    size_t inboundBytes = 0;
    bool wantsRoom = false;
    std::exception_ptr failure;
    bool stopping = false;
    std::atomic<bool> cancelled{false}; // cancel(): receive() ends as with the run's token.
    std::atomic<size_t> maxFragment{0}; // Video bytes a fragment carries over the link as it is now.
    // Parity for lost video (sender): the host's setting, and the parity now,
    // per 100 video fragments (AUTO: from the loss the viewer reports).
    std::atomic<rcs_fec_mode> fecMode{RCS_FEC_AUTO};
    std::atomic<uint32_t> fecLimit{50}, fecPercent{0};
    // What it cost and repaired (sender: parity sent; receiver: fragments
    // rebuilt, fragments that came when asked for again, frames given up on).
    std::atomic<uint64_t> paritySent{0}, fecRecovered{0}, resendRecovered{0}, framesLost{0};

    // Paths: whether this end follows its peer (setMigrate), what the peer
    // said of itself (its pings), the routes addRoute handed over for the
    // network thread to take, what happened to the path for the session
    // (takePathEvents), and whether the path is silent: the video, sound and
    // data wait meanwhile.
    std::atomic<bool> migrate{true}, peerMigrates{true};
    struct AddedRoute {
        std::unique_ptr<DatagramSocket> socket;
        std::unique_ptr<Link> link;
        rcs_path_cause cause;
        bool signaled, standby;
    };
    std::vector<AddedRoute> addedRoutes;
    struct StandbyDrop {
        rcs_path_cause cause;
        rcs_result result;
        std::string why;
    };
    std::optional<StandbyDrop> standbyDropped;                          // dropStandby().
    std::optional<rcs_path_cause> moveRequest;                          // moveRoute()...
    std::optional<std::pair<rcs_path_cause, std::string>> leaveRequest; // ...and leaveRoute(), for the network thread.
    std::atomic<bool> forgotten{false};                                 // peerForgot().
    std::vector<PathEvent> pathEvents;
    std::atomic<bool> silent{false}, pathLost{false};

    // ---- Network thread only -----------------------------------------------

    std::unique_ptr<crypto::Aead> sendKeys, receiveKeys;
    const uint32_t sendId, receiveId; // The connection ids this end's datagrams and the peer's carry.
    const crypto::Key exporter;       // The handshake's (handshake.hpp).
    CancelToken runCancelled;
    Bytes finishedDatagram; // Client: repeated until the server is heard.

    // A way to the peer: a socket of this end's and the link over it, which
    // sends to the peer's address on it (none over the host's channel).
    struct Route {
        std::unique_ptr<DatagramSocket> socket;
        std::unique_ptr<Link> link;
        uint64_t restarts = 0;    // The socket's restarts() as last seen.
        Clock::time_point used{}; // Traffic or a probe over it, last.
        std::string failure;      // Its socket failed (nothing more goes over it): why.
        bool failingSeen = false; // Its sends failing was taken as a change of network.
        // A route this end added: why, and whether the host's signalling found it.
        rcs_path_cause cause = RCS_PATH_CAUSE_NETWORK;
        bool signaled = false;
    };
    // An address the peer may be at over a route, as this end follows it:
    // what came from it, whether it is proven (the peer answered a challenge
    // sent there), the challenges under way and what they are for.
    enum class Purpose : uint8_t { none, peerMoved, oldPath, ownRoute, check, standby };
    struct Candidate {
        Route* route = nullptr;
        std::optional<Endpoint> address; // None over the host's channel.
        bool proven = false;
        Clock::time_point provenAt{}, firstHeard{}, heard{};
        uint64_t received = 0,
                 sent = 0; // Until proven: what came from it, and what went to it (three times that at most).
        Clock::time_point peerProbed{}; // The peer challenged this end over it: it moved knowingly.
        Purpose purpose = Purpose::none;
        std::vector<std::pair<std::array<uint8_t, probeData>, Clock::time_point>> challenges; // Data, when sent.
        Clock::time_point provingSince{}, nextChallenge{};
        int attempts = 0;
    };
    // Guards what status() and peerAddress() read of the paths (the current
    // candidate, its link's peer, routes coming and going), which only this
    // thread changes.
    mutable std::mutex pathMutex;
    std::vector<std::unique_ptr<Route>> routes;
    std::vector<std::unique_ptr<Candidate>> candidates;
    transport::Waiter waiter;     // After the routes: it goes before their sockets.
    Candidate* current = nullptr; // Where the traffic goes.
    Candidate* moving = nullptr;  // The peer's newest traffic came from there: it is being proven.
    // The standby path (addRoute's `standby`): proven every standbyProbe
    // (`standbyReady` while it answers), taken when the current one stalls.
    // Then the path it took over from waits there instead (`standbyHome`),
    // proven every second, and takes the traffic back once it answers.
    Candidate* standby = nullptr;
    bool standbyReady = false, standbyHome = false;
    PathEnd standbyEnd; // The standby path while it is ready (pathMutex).
    Clock::time_point nextStandbyProbe{};
    std::atomic<uint64_t> standbyBytes{0};
    enum class PathState { running, stalled, lost } state = PathState::running;
    Clock::time_point lostAt{};
    uint64_t highestTraffic = 0;       // The newest packet number that was no probe.
    uint64_t pathStartPacket = 1;      // Reports of packets before it are about another path.
    std::deque<uint64_t> probePackets; // Packets sent off the current path lately: no report counts them.
    uint64_t networkSeen = transport::networkGeneration();
    std::optional<Clock::time_point> networkCheck; // The current path is to answer by then.
    int provingStarted = 0;                        // In the current second...
    Clock::time_point provingSecond{};             // ...that began then.
    std::atomic<uint64_t> pathChanges{0}, lastInterruptionNs{0};
    bool confirmed = true;
    uint64_t sendPacket = 0;
    ReplayWindow replay;
    std::array<Incoming, 2> incoming;
    Bytes outgoingFrame; // The frame being sent (stamped), out of the lock.
    // Datagrams sealed for the current route, sent together (flush(): Link::sendBatch):
    // what a round of this thread sends, at most maxBatch at a time.
    struct Batched {
        Bytes datagram;
        uint8_t dscp;
    };
    std::vector<Batched> batch, flushing;
    std::vector<transport::Outgoing> outgoingBatch;
    Route* batchRoute = nullptr;

    // A group of a frame's video fragments and its parity.
    struct Group {
        uint8_t parity = 0;       // Parity fragments the group has; 0 while unknown or none.
        uint16_t lastLength = 0;  // Of its last video fragment, as its parity says.
        std::vector<Bytes> parts; // Its parity; empty entries did not arrive.
        uint8_t received = 0;
        // All its parity left before what arrived since: whatever of it is
        // missing then is lost (unless it is still in reorderDelay).
        Clock::time_point closed{};
    };

    struct Assembly {
        uint16_t count = 0; // 0 while no fragment arrived.
        uint16_t received = 0;
        uint16_t highest = 0; // The highest index that arrived.
        std::vector<Bytes> parts;
        std::vector<uint8_t> nacks;
        // When a missing fragment may be asked for (again); unset for the
        // ones after `highest` before they are.
        std::vector<Clock::time_point> due;
        Clock::time_point detected, first, last;
        Clock::duration pause{}; // The longest wait between its fragments.
        Clock::time_point laterSeen{}; // A later frame's fragment arrived: all of this one had left.
        int wholeNacks = 0;
        Clock::time_point wholeNacked{};
        bool gone = false; // The sender no longer has it: what is missing will not come.
        uint64_t decryptNs = 0;
        std::vector<Group> groups; // One per fecGroup video fragments.
        size_t closedBelow = 0;    // The groups before it are closed.
        bool missingTold = false;  // VideoMissing went for it.
        // How its fragments and parity arrived, for the path's rate: how
        // many, the end of the first bunch and the latest (ns), the bytes
        // after the bunch, and the sender's stamps: the earliest of those
        // after the bunch, the latest of all.
        uint16_t arrivals = 0;
        uint64_t bunchEnd = 0, arrivedLast = 0, afterBunch = 0;
        bool bunched = true; // Still in the first bunch.
        uint32_t sentAfter = 0, sentLast = 0;
    };
    // Each stream's frames, in order on their own.
    struct VideoIn {
        std::map<uint32_t, Assembly> assemblies; // From nextFrame to highestFrame.
        uint32_t nextFrame = 1, highestFrame = 0;
    };
    std::array<VideoIn, streams> videoIn;
    size_t pendingFragments = 0; // Of the assemblies.
    uint64_t gaps = 0;           // Frames given up on since the last summary.
    // The session fell behind (maxInbound): reliable messages wait in
    // `incoming` (`held`), the gap a stream's dropped pictures make is told
    // before its next picture (`gapOwed`); what was dropped meanwhile, for
    // the log, which says when it falls behind and when it caught up.
    bool held = false, behind = false;
    std::array<bool, streams> gapOwed{};
    uint64_t droppedBehind = 0;
    Clock::time_point summarized = Clock::now();

    Clock::time_point nextPing{}, nextFinished{}, tidied{};
    double tokens = 0;
    Clock::time_point refilled = Clock::now();

    // What arrived in the current report window.
    struct Window {
        Clock::time_point started = Clock::now();
        uint64_t firstPacket = 0, highest = 0;
        uint32_t received = 0;
        uint64_t bytes = 0;
        // The lowest one-way delay of the video that arrived: a picture's
        // first datagram, which no datagram of its own queues ahead of, so
        // that a picture sent at once does not count as a queue on the path.
        std::optional<int64_t> delay;
        // The fastest rates pictures completed in it arrived at, of those
        // the path spaced and of the others (pictureRate()).
        uint64_t spacedBps = 0, unspacedBps = 0;
    } window;
    uint64_t reportedHighest = 0;
    double lossSeen = 0;          // Sender: the reported loss, smoothed (a fraction)...
    Clock::time_point lastLoss{}; // ...and when a report last showed any (AUTO parity).
    // One-way delay (µs, arbitrary origin): this end's 32-bit microseconds
    // minus the sender's stamp, unwrapped against the previous sample, so
    // that clocks far apart cannot make it jump.
    std::optional<uint32_t> previousRawDelay;
    int64_t delay = 0;
    double arrivalBps = 0;              // What arrives, smoothed (bits per second).
    Clock::duration recentPause{};      // The latest frames' longest waits between fragments, fading.
    std::optional<RateController> rate; // Sharer.
    bool fixedRate = false;             // The sharer's bitrate does not adapt (rateMutex).
    uint64_t enqueuedBytes = 0;         // Video queued since the last measurement (mutex).
    uint64_t enqueuedImmediateBytes = 0; // Sound and unreliable data queued since then (mutex).
    double encodedBps = 0, immediateBps = 0; // What was queued lately (rateMutex).
    Clock::time_point encodedSince = Clock::now();
    mutable std::mutex rateMutex; // Guards `rate` against status() and setVideoRate().

    std::atomic<uint64_t> roundTripNs{100'000'000}; // Until measured.
    bool rttMeasured = false;
    uint8_t audioDscp = 0;                     // UdpOptions'.
    // Sound: how long what this end sent waited here before it left, and
    // the queueing on the path of what it received (status()).
    RecentMost soundWait, soundQueue;
    SoundDelay soundDelay;
    StallWatch stallWatch;                    // What arrives over the current path.
    StallWatch::Summary stalls;               // Its summary of the last tick (rateMutex).
    Clock::time_point nextStallSummary{};
    std::atomic<uint64_t> backlogDrops{0}, backlogDroppedBytes{0}; // Video dropped unsent.
    std::atomic<uint64_t> markedPackets{0};    // Sound sent with it...
    bool markRefused = false;                  // ...and whether some went without (said once).
    // Video leaves within frameSpreadNs at up to maxPacingBps while the
    // viewer's reports find the path clear, else at pacingBps (currentPace());
    // 0: unpaced (no video goes this way).
    std::atomic<uint64_t> pacingBps{0}, maxPacingBps{0}, frameSpreadNs{0};
    std::atomic<bool> pathClear{true};
    // The path's rate as the viewer's reports measure it (0: unknown), and on
    // a clear path what video leaves at most for it (RateRules: pathPacing
    // percent of it, 0: no limit) after a picture's first pathBurst of it.
    PathRate pathRate;
    std::atomic<uint64_t> pathBps{0}, pathBurstNs{0};
    std::atomic<uint32_t> pathPacing{0};
    // Reliable data never goes ahead of queued video, and a congestion window
    // of its own bounds what of it is on the path. While video runs, the
    // viewer's reports steer it: a quarter more per report on a clear path,
    // half at the first queueing or loss they show (10 ms, 2%; at most once a
    // round trip), long before the video's bitrate reacts (30 ms, 10%). So data
    // takes what the video leaves of the path and pushes the video down
    // neither in pacing nor in bitrate. Without reports (a still screen, or
    // this end receives the video) acknowledgements open it, as TCP's slow
    // start does. It grows only while it holds data back, and starts small
    // again when video resumes after a still screen.
    size_t dataWindow;
    bool dataHeld = false; // Data waited for the window since it last grew.
    Clock::time_point lastReport{}, dataCut{}, videoSent{};
    std::atomic<uint64_t> bytes{0}, receiveNs{0}, decryptNs{0};
    std::atomic<Goodbye::Reason> leaving{Goodbye::stopped}; // Told to the peer when this end closes.
    std::optional<crypto::PublicKey> peerKey; // Set before the thread starts.
    std::thread thread;

    Route& route() const noexcept { return *current->route; }
    Link& link() const noexcept { return *current->route->link; }

    void updateFragmentSize() {
        maxFragment = datagramLimit - link().overhead() - dataHeader - crypto::Aead::tagSize - videoHeader;
    }

    // ---- Sending (network thread) -----------------------------------------------

    // A datagram's bytes over `over`, counted (standbyBytes: over a standby path that waits).
    void counted(const Route& over, size_t size) {
        bytes += size;
        if (standby && standby->route == &over && current->route != &over) {
            standbyBytes += size;
        }
    }

    // To the current route, with what else this round sends there (flush()).
    void sendRaw(Bytes datagram, uint8_t dscp = 0) {
        Route& over = route();
        if (!over.failure.empty()) {
            return; // Its socket is gone: as if lost, until another path takes over.
        }
        if (batchRoute != &over) {
            flush();
            batchRoute = &over;
        }
        bytes += datagram.size() + over.link->overhead();
        batch.push_back({std::move(datagram), dscp});
        if (batch.size() >= maxBatch) {
            flush();
        }
    }

    // Sends what was batched, in order; nothing over a route that failed meanwhile.
    void flush() {
        if (batch.empty()) {
            return;
        }
        flushing.clear(); // A send that threw left it.
        flushing.swap(batch);
        Route& over = *batchRoute;
        if (!over.failure.empty()) {
            return;
        }
        outgoingBatch.clear();
        size_t marks = 0;
        for (const auto& [datagram, dscp] : flushing) {
            outgoingBatch.push_back({nullptr, datagram, dscp});
            marks += dscp != 0;
        }
        const size_t marked = over.link->sendBatch(*over.socket, outgoingBatch);
        flushing.clear();
        markedPackets += marked;
        if (marked < marks && !markRefused) {
            markRefused = true; // Said once.
            log::info("network", "Sound goes unmarked over " + over.link->describe() +
                                     ": the path carries datagrams inside another protocol, or the system refused "
                                     "the mark");
        }
    }

    // `frame` sealed as the next data datagram, under the peer's connection id.
    Bytes seal(std::span<const uint8_t> frame) {
        Bytes datagram;
        datagram.reserve(dataHeader + frame.size() + crypto::Aead::tagSize);
        datagram.push_back(dataKind);
        appendBigEndian(datagram, sendId, 4);
        appendBigEndian(datagram, ++sendPacket, 8);
        const auto sealed = sendKeys->seal(sendPacket, datagram, frame);
        datagram.insert(datagram.end(), sealed.begin(), sealed.end());
        return datagram;
    }

    void sendFrame(std::span<const uint8_t> frame, uint8_t dscp = 0) { sendRaw(seal(frame), dscp); }

    std::chrono::nanoseconds roundTrip() const noexcept { return std::chrono::nanoseconds(roundTripNs.load()); }
    Clock::duration retransmitTimeout() const noexcept { return std::max<Clock::duration>(30ms, roundTrip() * 2); }
    // Whether the path the traffic takes delivers in order and loses nothing (TCP).
    bool ordered() const noexcept { return link().ordered(); }
    // The first wait for a reliable message's acknowledgement.
    Clock::duration resendTimeout() const noexcept {
        return ordered() ? std::max<Clock::duration>(orderedWait, retransmitTimeout()) : retransmitTimeout();
    }

    // ---- Paths (network thread) -------------------------------------------------

    Route& addRoute(std::unique_ptr<DatagramSocket> socket, std::unique_ptr<Link> link, Clock::time_point now) {
        auto added = std::make_unique<Route>();
        added->socket = std::move(socket);
        added->link = std::move(link);
        added->used = now;
        added->restarts = added->socket->restarts();
        waiter.watch(*added->socket);
        std::lock_guard lock(pathMutex);
        routes.push_back(std::move(added));
        return *routes.back();
    }

    // The peer's address `from` over `over` as a candidate (none over the
    // host's channel), followed from now on if it is new.
    Candidate& candidateFor(Route& over, const std::optional<Endpoint>& address, Clock::time_point now) {
        for (auto& candidate : candidates) {
            if (candidate->route == &over && candidate->address == address) {
                return *candidate;
            }
        }
        if (candidates.size() >= maxCandidates) {
            const auto oldest = std::find_if(candidates.begin(), candidates.end(), [&](const auto& c) {
                return !c->proven && c.get() != current && c.get() != moving && c.get() != standby;
            });
            if (oldest != candidates.end()) {
                candidates.erase(oldest);
            }
        }
        auto added = std::make_unique<Candidate>();
        added->route = &over;
        added->address = address;
        added->provenAt = now; // Kept a while from now, as if heard.
        candidates.push_back(std::move(added));
        return *candidates.back();
    }

    // Whether the connection follows its peer to new paths and keeps a lost
    // one: both ends must (pings carry the peer's say).
    bool following() const noexcept { return migrate && peerMigrates; }

    PathEnd endOf(const Candidate& candidate) const {
        PathEnd end;
        end.route = candidate.route->link->route();
        end.relayTransport = candidate.route->link->relayTransport();
        end.remote = candidate.address;
        end.hop =
            candidate.route->link->route() == RCS_ROUTE_RELAY ? candidate.route->link->firstHop() : candidate.address;
        try {
            if (end.route != RCS_ROUTE_HOST && end.route != RCS_ROUTE_FORWARDER) { // Channels with no addresses.
                end.local = candidate.route->socket->local();
            }
        } catch (const Failure&) {
        }
        return end;
    }

    void report(PathEvent event) {
        std::lock_guard lock(mutex);
        if (pathEvents.size() == maxPathEvents) {
            pathEvents.erase(pathEvents.begin()); // Nobody takes them (a forwarder's ends): the latest are kept.
        }
        pathEvents.push_back(std::move(event));
    }

    // A probe (PATH_CHALLENGE or PATH_RESPONSE with `data`) to `to`: padded to
    // the path's datagram, but to an address not proven yet never more than
    // three times what came from it (and bare to one nothing came from), and
    // bare over the host's channel and over a byte stream.
    void sendProbe(Candidate& to, uint8_t type, std::span<const uint8_t> data, Clock::time_point now) {
        Route& over = *to.route;
        if (!over.failure.empty()) {
            return;
        }
        flush(); // What was sealed before it goes before it.
        const size_t bare = dataHeader + probeSize + crypto::Aead::tagSize;
        size_t size = bare;
        if (to.address) {
            uint64_t most = over.link->ordered() ? bare : datagramLimit - over.link->overhead();
            if (!to.proven && to.received) {
                const uint64_t allowed = to.received * 3 > to.sent ? to.received * 3 - to.sent : 0;
                if (allowed < bare) {
                    return;
                }
                most = std::min(most, allowed);
            } else if (!to.proven) {
                most = bare;
            }
            size = size_t(std::max<uint64_t>(bare, most));
        }
        Bytes frame(size - dataHeader - crypto::Aead::tagSize, 0);
        frame[0] = type;
        std::copy(data.begin(), data.end(), frame.begin() + 1);
        const Bytes datagram = seal(frame);
        if (&to != current) {
            probePackets.push_back(sendPacket);
        }
        if (to.address) {
            over.link->sendTo(*over.socket, *to.address, datagram);
        } else {
            over.link->send(*over.socket, datagram);
        }
        over.used = now;
        if (!to.proven) {
            to.sent += datagram.size();
        }
        counted(over, datagram.size() + over.link->overhead());
    }

    // Starts proving `to` (at most provingPerSecond a second); one under way
    // goes on, taking a purpose that moves the traffic.
    void prove(Candidate& to, Purpose purpose, Clock::time_point now) {
        if (to.purpose != Purpose::none) {
            if (purpose == Purpose::peerMoved || purpose == Purpose::ownRoute) {
                to.purpose = purpose;
            }
            return;
        }
        if (now - provingSecond >= 1s) {
            provingSecond = now;
            provingStarted = 0;
        }
        if (provingStarted >= provingPerSecond) {
            return;
        }
        ++provingStarted;
        to.purpose = purpose;
        to.provingSince = now;
        to.attempts = 0;
        to.challenges.clear();
        challenge(to, now);
    }

    void challenge(Candidate& to, Clock::time_point now) {
        std::array<uint8_t, probeData> data{};
        crypto::random(data);
        to.challenges.emplace_back(data, now);
        sendProbe(to, challengeFrame, data, now);
        to.nextChallenge = now + std::max<Clock::duration>(retransmitTimeout(), 50ms) * (1 << std::min(to.attempts, 4));
        ++to.attempts;
    }

    // The peer answered a challenge to `to` (sent at `sent`): the path is proven.
    void answered(Candidate& to, Clock::time_point sent, Clock::time_point now) {
        to.proven = true;
        to.provenAt = now;
        const Purpose purpose = std::exchange(to.purpose, Purpose::none);
        to.challenges.clear();
        if (&to == current) { // A round trip of this path, which works after a change of network.
            const uint64_t sample = nanoseconds(now - sent);
            roundTripNs = rttMeasured ? (roundTripNs * 7 + sample) / 8 : sample;
            rttMeasured = true;
            networkCheck.reset();
        }
        if (purpose == Purpose::peerMoved && moving == &to) {
            switchTo(to, now - to.peerProbed < 10s ? RCS_PATH_CAUSE_PEER : RCS_PATH_CAUSE_REBINDING, now, sent);
        } else if (purpose == Purpose::ownRoute && &to != current) {
            switchTo(to, to.route->cause, now, sent);
        } else if (purpose == Purpose::standby && &to == standby) {
            if (standbyHome) { // The path the standby took over from answers again: back to it.
                Candidate& away = *current;
                switchTo(to, RCS_PATH_CAUSE_STALLED, now, sent);
                standby = &away;
                standbyHome = false;
            }
            standbyIs(true, RCS_PATH_CAUSE_NETWORK, RCS_OK, "");
            nextStandbyProbe = now + standbyProbe;
        }
    }

    // The standby path is ready from now on, or is not (`result` and `why`
    // say why): RCS_PATH_STANDBY when that changed. (`standby`: none, or the
    // path to come back to, while not ready.)
    void standbyIs(bool ready, rcs_path_cause cause, rcs_result result, std::string why) {
        if (ready == standbyReady && (ready || standby)) {
            return;
        }
        standbyReady = ready;
        PathEvent event;
        event.change = RCS_PATH_STANDBY;
        event.cause = cause;
        event.from = endOf(*current);
        if (ready) {
            event.to = endOf(*standby);
            log::info("network", "A standby path is ready: " + standby->route->link->describe());
        } else {
            log::info("network", "No standby path: " + why);
        }
        {
            std::lock_guard lock(pathMutex);
            standbyEnd = event.to;
        }
        event.result = result;
        event.message = std::move(why);
        report(std::move(event));
    }

    // A route kept ready beside the current one (addRoute's `standby`),
    // proven from now on; the one there was goes.
    void setStandby(Route& over, Clock::time_point now) {
        Candidate& candidate = candidateFor(over, over.link->peer(), now);
        if (standby && standby != &candidate && standbyHome) {
            return; // The traffic is on the standby path, and the one it took over from is to come back.
        }
        standby = &candidate;
        standbyHome = false;
        prove(candidate, Purpose::standby, now);
        nextStandbyProbe = now + standbyProbe;
    }

    // The current path stalled: the standby path takes the traffic, and the
    // stalled one waits to come back in its place. Whether it did.
    bool takeStandby(Clock::time_point now) {
        if (!standby || !standbyReady || standbyHome || standby == current || !standby->route->failure.empty()) {
            return false;
        }
        Candidate& home = *current;
        switchTo(*standby, RCS_PATH_CAUSE_STALLED, now);
        standby = &home;
        standbyHome = true;
        standbyIs(false, RCS_PATH_CAUSE_STALLED, RCS_OK, "The standby path took over from the stalled one");
        nextStandbyProbe = now + 1s;
        prove(*current, Purpose::check, now); // The peer hears of it over the new path at once.
        return true;
    }

    // The peer's newest traffic came from `from` over the current route: the
    // peer is there now. A path this end proved lately is taken at once;
    // another is proven first, and so is the old one, whose newer traffic
    // takes the connection back (a path an attacker forwards the peer's
    // datagrams over loses to the real one). What still comes over another
    // route of this end's (one it moved from) moves nothing: this end chose.
    void followPeer(Candidate& from, Clock::time_point now) {
        if (&from == current) {
            moving = nullptr;
            return;
        }
        // The peer came back to the path the standby one took over from, or
        // took the standby path while the current one is silent here too
        // (in flight after this end came back, its datagrams there move nothing).
        if (following() && &from == standby && from.proven && (standbyHome || state != PathState::running)) {
            Candidate& away = *current;
            const bool home = standbyHome; // `from` is the path the standby took over from.
            switchTo(from, RCS_PATH_CAUSE_PEER, now);
            standby = &away;
            standbyHome = !home;
            standbyIs(home, RCS_PATH_CAUSE_PEER, RCS_OK, home ? "" : "The peer took the standby path");
            nextStandbyProbe = now + (standbyHome ? 1s : standbyProbe);
            return;
        }
        if (!following() || from.route != current->route ||
            (from.address && !from.route->link->reaches(*from.address))) {
            return;
        }
        if (from.proven && now - from.provenAt < provenFor) {
            switchTo(from, now - from.peerProbed < 10s ? RCS_PATH_CAUSE_PEER : RCS_PATH_CAUSE_REBINDING, now);
            return;
        }
        moving = &from;
        prove(from, Purpose::peerMoved, now);
        if (state == PathState::running && route().failure.empty()) {
            prove(*current, Purpose::oldPath, now);
        }
    }

    // Traffic goes over a path the rates know nothing of: what was measured
    // of the one before starts over (the adaptive bitrate, the path's rate
    // that paces video, the base of sound's queueing).
    void ratesStartOver() {
        pathRate.reset();
        pathBps = 0;
        soundDelay.reset();
        stallWatch.restart();
        std::lock_guard lock(rateMutex);
        if (rate && !fixedRate) {
            rate->newPath();
            pacingBps = pacing(rate->target());
        }
    }

    // The traffic goes over `to` from now on (`proof`: when the challenge that
    // proved it left, for the round trip).
    void switchTo(Candidate& to, rcs_path_cause cause, Clock::time_point now,
                  std::optional<Clock::time_point> proof = std::nullopt) {
        Candidate& from = *current;
        PathEvent event;
        event.change = RCS_PATH_CHANGED;
        event.cause = cause;
        event.from = endOf(from);
        event.to = endOf(to);
        event.signaled = to.route != from.route && to.route->signaled;
        // From the last datagram over the old path to the first over the new one since.
        const auto resumedAt = to.firstHeard == Clock::time_point{} ? now : to.firstHeard;
        event.interruptionNs = nanoseconds(std::max(Clock::duration::zero(), resumedAt - from.heard));
        // An address that kept its host (only the port changed: a NAT remapped)
        // is the same path to the rate; another starts over.
        const bool sameHost = from.route == &*to.route && from.address && to.address &&
                              from.address->addressBytes() == to.address->addressBytes();
        {
            std::lock_guard lock(pathMutex);
            if (to.address) {
                to.route->link->setPeer(*to.address);
            }
            current = &to;
        }
        moving = nullptr;
        started(now, event);
        if (proof) {
            roundTripNs = nanoseconds(now - *proof);
            rttMeasured = true;
        } else if (!sameHost) {
            roundTripNs = 100'000'000;
            rttMeasured = false;
        }
        if (!sameHost) {
            ratesStartOver();
        }
        updateFragmentSize();
        log::info("network", "The path moved to " + link().describe() + " (" + causeName(cause) + ")");
        report(std::move(event));
    }

    // Traffic goes over the current path again (another one, or the same
    // after it stalled): reports from before say nothing of it, what waits
    // for its acknowledgement goes at once.
    void started(Clock::time_point now, PathEvent& event) {
        state = PathState::running;
        silent = false;
        pathLost = false;
        networkCheck.reset();
        current->heard = now;
        pathStartPacket = sendPacket + 1;
        probePackets.clear();
        ++pathChanges;
        lastInterruptionNs = event.interruptionNs;
        std::lock_guard lock(mutex);
        for (auto& stream : outgoing) {
            stream.resendNow();
        }
    }

    static uint64_t nanoseconds(Clock::duration duration) {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    }

    static const char* causeName(rcs_path_cause cause) {
        switch (cause) {
        case RCS_PATH_CAUSE_NETWORK:
            return "this end's network changed";
        case RCS_PATH_CAUSE_STALLED:
            return "the path stalled";
        case RCS_PATH_CAUSE_PEER:
            return "the peer moved";
        case RCS_PATH_CAUSE_REBINDING:
            return "the peer's NAT remapped it";
        case RCS_PATH_CAUSE_SETTINGS:
            return "the settings";
        default:
            return "the host's channel was replaced";
        }
    }

    // The current path went silent, this end's network changed under it, or
    // its route failed or the peer's connection under it closed (`why`): the
    // video, sound and data wait until it is back or another one takes over,
    // at once a standby path that is ready. Whether that took over.
    bool stalled(rcs_path_cause cause, Clock::time_point now, std::string why = {}) {
        if (state != PathState::running) {
            return false;
        }
        state = PathState::stalled;
        silent = true;
        dropQueuedVideo();
        PathEvent event;
        event.change = RCS_PATH_CHANGING;
        event.cause = cause;
        event.from = endOf(*current);
        event.message = !why.empty()                       ? std::move(why)
                        : cause == RCS_PATH_CAUSE_NETWORK  ? "This end's network changed and the path did not answer"
                        : cause == RCS_PATH_CAUSE_SETTINGS ? "This end may no longer use the path's network"
                                                           : "Nothing came for " + secondsText(pathStall);
        log::info("network", "The path " + link().describe() + " is silent: " + event.message);
        report(std::move(event));
        return takeStandby(now);
    }

    // Nothing came for idleTimeout (or `why`: this end left the path): the
    // connection is kept for `hold` while migration is on at both ends, else
    // it fails as before.
    void lose(Clock::time_point now, rcs_path_cause cause = RCS_PATH_CAUSE_STALLED, std::string why = {}) {
        if (why.empty()) {
            why = "The peer stopped responding (nothing received for " + secondsText(idleTimeout) + " over " +
                  link().describe() + (route().failure.empty() ? "" : "; " + route().failure) + ")";
        }
        if (!following() || hold <= Clock::duration::zero()) {
            fail(RCS_ERROR_NETWORK, why);
        }
        if (stalled(cause, now)) {
            return; // The standby path took over.
        }
        state = PathState::lost;
        pathLost = true;
        lostAt = now;
        PathEvent event;
        event.change = RCS_PATH_LOST;
        event.cause = cause;
        event.from = endOf(*current);
        event.result = RCS_ERROR_NETWORK;
        event.message = why + "; the connection is kept for " + secondsText(hold);
        log::warning("network", event.message);
        report(std::move(event));
    }

    // No network is left for this end (leaveRoute): nothing more goes over
    // the current route, and the path is lost now.
    void leave(rcs_path_cause cause, const std::string& why, Clock::time_point now) {
        Route& over = route();
        if (over.failure.empty()) {
            over.failure = why;
            waiter.unwatch(*over.socket);
        }
        networkCheck.reset();
        if (state != PathState::lost) {
            lose(now, cause, why);
        }
    }

    // The current path brings something again after it stalled, silent since `previous`.
    void resumed(Clock::time_point previous, Clock::time_point now) {
        PathEvent event;
        event.change = RCS_PATH_CHANGED;
        event.cause = RCS_PATH_CAUSE_STALLED;
        event.from = event.to = endOf(*current);
        event.interruptionNs = nanoseconds(now - previous);
        started(now, event);
        log::info("network", "The path " + link().describe() + " is back after " +
                                 std::to_string(event.interruptionNs / 1'000'000) + " ms");
        report(std::move(event));
    }

    // The host replaced its channel with one to the same peer (HOST): the
    // connection goes on over it, as over a new path.
    void hostRestarted(Clock::time_point now) {
        if (!following()) {
            fail(RCS_ERROR_NETWORK, "The host's channel was replaced, and migration is off (rcs_paths_config)");
        }
        PathEvent event;
        event.change = RCS_PATH_CHANGED;
        event.cause = RCS_PATH_CAUSE_HOST;
        event.from = event.to = endOf(*current);
        event.interruptionNs = nanoseconds(now - current->heard);
        started(now, event);
        roundTripNs = 100'000'000;
        rttMeasured = false;
        ratesStartOver();
        log::info("network", "The host's channel was replaced: the connection goes on over the new one");
        report(std::move(event));
    }

    // A route's socket (or its relay) failed: nothing more goes over it. A
    // connection without migration fails with it where the traffic went, as
    // before; otherwise the path is silent until another socket (reopen) or
    // path takes over.
    void routeFailed(Route& over, const Failure& failure, Clock::time_point now) {
        if (&over == &route() &&
            (!following() || over.link->route() == RCS_ROUTE_HOST || over.link->route() == RCS_ROUTE_FORWARDER)) {
            // The host closed its channel for good, or the forwarder its pipe.
            throw;
        }
        log::warning("network", "The path " + over.link->describe() + " failed: " + failure.what());
        over.failure = failure.what();
        waiter.unwatch(*over.socket);
        if (standby && standby->route == &over) {
            // The path the standby one took over from is not coming back
            // either: another standby path is to be found.
            const std::string what = standbyHome ? "The path the standby one took over from failed: "
                                                 : "The standby path's socket failed: ";
            if (standbyHome) {
                standby = nullptr;
                standbyHome = false;
            }
            standbyIs(false, RCS_PATH_CAUSE_NETWORK, RCS_ERROR_NETWORK, what + failure.what());
        }
        if (&over == &route() && !stalled(RCS_PATH_CAUSE_NETWORK, now, failure.what())) {
            reopenRoute(RCS_PATH_CAUSE_NETWORK, now);
        }
    }

    // The peer's own connection to `closed.peer()` over `over` closed (a TCP
    // connection to a sharer's port): nothing reaches it there any more. The
    // path stalls at once where the traffic went, and the peer comes back
    // over another connection; a connection without migration fails, as the
    // TCP connection under it did.
    void peerClosed(Route& over, const transport::PeerClosed& closed, Clock::time_point now) {
        if (current->route != &over || current->address != closed.peer()) {
            std::erase_if(candidates, [&](const auto& c) {
                return c->route == &over && c->address == closed.peer() && c.get() != moving && c.get() != standby;
            });
            return;
        }
        if (!following()) {
            throw closed;
        }
        stalled(RCS_PATH_CAUSE_PEER, now, closed.what());
    }

    std::unique_ptr<DatagramSocket> newSocket(bool ipv6) {
        return reopen ? reopen(ipv6)
                      : std::make_unique<transport::UdpSocket>(Endpoint::parse(ipv6 ? "::" : "0.0.0.0", 0));
    }

    // This end's own socket again, to the peer's address (a direct path with
    // reopen), or to the TURN server with the allocation moved there: proven
    // before the traffic moves there, unless the current route failed.
    void reopenRoute(rcs_path_cause cause, Clock::time_point now) {
        if (auto* relay = dynamic_cast<RelayLink*>(&link()); relay && relay->movable()) {
            try {
                auto socket = newSocket(relay->firstHop()->ipv6());
                log::info("network", "Moving the TURN allocation to a new socket, " + socket->local().toString());
                auto moved = relay->moveTo(*socket);
                Route& added = addRoute(std::move(socket), std::move(moved), now);
                added.cause = cause;
                ownRoute(added, now);
            } catch (const Failure& failure) {
                log::warning("network", std::string("Could not move the TURN allocation: ") + failure.what());
            }
            return;
        }
        if (!reopen || !current->address || !dynamic_cast<DirectLink*>(&link())) {
            return;
        }
        std::unique_ptr<DatagramSocket> socket;
        try {
            socket = reopen(current->address->ipv6());
        } catch (const Failure& failure) {
            log::warning("network", std::string("Could not open another socket for the path: ") + failure.what());
            return;
        }
        log::info("network", "Trying the path from a new socket, " + socket->local().toString());
        Route& added = addRoute(std::move(socket), directLink(*current->address), now);
        added.cause = cause;
        ownRoute(added, now);
    }

    // A route of this end's own to the peer's current address: the peer is
    // known to be there, so nothing limits what goes to it (a TCP connection
    // is a path of its own, proven once the peer answered over it). The
    // traffic moves there at once where the current route failed, unless the
    // path is lost: a lost path is back once the peer answered.
    void ownRoute(Route& added, Clock::time_point now) {
        Candidate& candidate = candidateFor(added, added.link->peer(), now);
        candidate.proven = !added.link->ordered() && current->proven && candidate.address == current->address;
        candidate.provenAt = now;
        if (!route().failure.empty() && state != PathState::lost) {
            switchTo(candidate, added.cause, now);
        }
        prove(candidate, Purpose::ownRoute, now);
    }

    // This end's network changed: the current path is to answer a challenge
    // within a round trip and networkCheckGrace, else it is looked past.
    void networkChanged(Clock::time_point now) {
        if (!confirmed || !current->address || !route().failure.empty()) {
            return;
        }
        prove(*current, Purpose::check, now);
        networkCheck = now + roundTrip() + networkCheckGrace;
    }

    // The peer sent 0xD7 under this end's id: it no longer knows the
    // connection. Anyone on the way could send one, so it counts only while
    // the path is silent, and only from where the traffic goes, or from where
    // this end proves a way of its own to the peer (a new TCP connection
    // beside a silent one).
    void unknown(const Route& over, const Endpoint& from, std::span<const uint8_t> datagram) {
        if (state == PathState::running || uint32_t(get(datagram, connectionIdOffset, 4)) != sendId) {
            return;
        }
        const auto at = [&](const Candidate& c) {
            return c.route == &over && (!c.address || from == *c.address);
        };
        if (!at(*current) && std::none_of(candidates.begin(), candidates.end(), [&](const auto& c) {
                return c->purpose == Purpose::ownRoute && at(*c);
            })) {
            return;
        }
        throw UnknownConnection(RCS_ERROR_NETWORK, "The peer no longer knows this connection (it restarted, or let "
                                                   "it go)");
    }

    // Takes what the session asked for (a standby route no more, the routes
    // addRoute handed over, leaving or moving the path), follows what happens
    // under the routes (a channel replaced, sends failing, the network),
    // retries and gives up proofs, looks for silence, and lets unused routes
    // and addresses go.
    void followPaths(Clock::time_point now) {
        std::vector<AddedRoute> added;
        std::optional<rcs_path_cause> move;
        std::optional<std::pair<rcs_path_cause, std::string>> leaveFor;
        std::optional<StandbyDrop> noStandby;
        {
            std::lock_guard lock(mutex);
            added.swap(addedRoutes);
            move = std::exchange(moveRequest, std::nullopt);
            leaveFor = std::exchange(leaveRequest, std::nullopt);
            noStandby = std::exchange(standbyDropped, std::nullopt);
        }
        if (noStandby && !standbyHome) {
            standby = nullptr;
            standbyReady = true; // Said again, whatever it was.
            standbyIs(false, noStandby->cause, noStandby->result, noStandby->why);
        }
        for (auto& route : added) {
            Route& over = addRoute(std::move(route.socket), std::move(route.link), now);
            over.cause = route.cause;
            over.signaled = route.signaled;
            if (route.standby) {
                setStandby(over, now);
            } else {
                ownRoute(over, now);
            }
        }
        if (leaveFor && following()) {
            leave(leaveFor->first, leaveFor->second, now);
        }
        if (move && following()) {
            reopenRoute(*move, now);
        }
        if (forgotten.exchange(false) && state != PathState::running) {
            throw UnknownConnection(RCS_ERROR_NETWORK, "The peer no longer knows this connection (its signalling "
                                                       "said so: it restarted, or let it go)");
        }
        for (size_t i = 0; i < routes.size(); ++i) { // A route that fails may open another.
            Route& over = *routes[i];
            if (const uint64_t restarts = over.socket->restarts(); restarts != over.restarts) {
                over.restarts = restarts;
                if (&over == &route()) {
                    hostRestarted(now);
                }
            }
            if (over.failure.empty()) {
                try {
                    over.link->tick(*over.socket, now);
                } catch (const Failure& failure) {
                    routeFailed(over, failure, now);
                }
            }
        }
        if (Route& over = route(); over.socket->failing() != over.failingSeen) {
            over.failingSeen = !over.failingSeen;
            if (over.failingSeen) { // Sends fail: the network went from under the path.
                networkChanged(now);
            }
        }
        if (const uint64_t generation = transport::networkGeneration(); generation != networkSeen) {
            networkSeen = generation;
            networkChanged(now);
        }
        if (networkCheck && now >= *networkCheck) {
            networkCheck.reset();
            if (!stalled(RCS_PATH_CAUSE_NETWORK, now)) {
                reopenRoute(RCS_PATH_CAUSE_NETWORK, now);
            }
        }
        const auto giveUp = std::max<Clock::duration>(leastProving, 3 * retransmitTimeout());
        for (auto& candidate : candidates) {
            Candidate& c = *candidate;
            if (c.purpose == Purpose::none) {
                continue;
            }
            if (now - c.provingSince >= giveUp) {
                if (moving == &c) {
                    moving = nullptr;
                }
                if (&c == standby && c.purpose == Purpose::standby && !standbyHome) {
                    standbyIs(false, RCS_PATH_CAUSE_STALLED, RCS_ERROR_NETWORK, "The standby path stopped answering");
                }
                c.purpose = Purpose::none;
                c.challenges.clear();
            } else if (now >= c.nextChallenge) {
                challenge(c, now);
            }
        }
        if (standby && standby->purpose == Purpose::none && now >= nextStandbyProbe &&
            standby->route->failure.empty()) {
            prove(*standby, Purpose::standby, now);
            nextStandbyProbe = now + (standbyHome ? 1s : standbyProbe);
        }
        if (state == PathState::running && now - current->heard > pathStall) {
            stalled(RCS_PATH_CAUSE_STALLED, now);
        }
        if (state != PathState::lost && now - current->heard > idleTimeout) {
            lose(now);
        }
        if (state == PathState::lost && now - lostAt > hold) {
            fail(RCS_ERROR_NETWORK, "No path to the peer for " + secondsText(idleTimeout + hold) + " (the last over " +
                                        link().describe() + ")");
        }
        route().used = now;
        if (now - tidied >= 1s) {
            tidied = now;
            tidy(now);
        }
    }

    // Addresses nothing came from lately, and routes none of them uses, go.
    void tidy(Clock::time_point now) {
        std::erase_if(candidates, [&](const auto& c) {
            return c.get() != current && c.get() != moving && c.get() != standby && c->purpose == Purpose::none &&
                   now - std::max(c->heard, c->provenAt) > (c->proven ? provenFor : unusedRoute);
        });
        std::vector<std::unique_ptr<Route>> gone;
        {
            std::lock_guard lock(pathMutex);
            for (auto it = routes.begin(); it != routes.end();) {
                Route* over = it->get();
                const bool used =
                    std::any_of(candidates.begin(), candidates.end(), [&](const auto& c) { return c->route == over; });
                if (!used && now - over->used > unusedRoute) {
                    gone.push_back(std::move(*it));
                    it = routes.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& over : gone) {
            if (over->failure.empty()) {
                waiter.unwatch(*over->socket);
            }
            over->link->close(*over->socket);
        }
    }

    // ---- Receiving (network thread) ---------------------------------------------

    // Hands `message` (`bytes` as it came) to receive() where there is room
    // (maxInbound; one always fits when none waits); false where not.
    bool deliver(Message message, size_t bytes) {
        bytes += sizeof(Delivered);
        bool roomy;
        {
            std::lock_guard lock(mutex);
            if (!inbound.empty() && inboundBytes + bytes > maxInbound) {
                wantsRoom = true;
                return false;
            }
            roomy = inboundBytes <= maxInbound / 2;
            inboundBytes += bytes;
            inbound.push_back({std::move(message), bytes});
            inboundReady.notify_one();
        }
        if (behind && roomy) {
            behind = false;
            log::info("network", "The session caught up with the peer's messages; " + std::to_string(droppedBehind) +
                                     " pictures, sound packets and unreliable messages were dropped meanwhile");
            droppedBehind = 0;
        }
        return true;
    }

    // What cannot wait found no room: dropped as if lost on the way.
    void fellBehind() {
        ++droppedBehind;
        if (!std::exchange(behind, true)) {
            log::warning("network", "The session takes the peer's messages slower than they arrive (" +
                                        std::to_string(maxInbound >> 20) +
                                        " MiB wait for it; does a callback hold its thread?): pictures, sound and "
                                        "unreliable messages that find no room are dropped, reliable ones wait");
        }
    }

    // A gap the session was not told of goes before anything more of its stream.
    bool gapTold(uint8_t stream) {
        if (gapOwed[stream] && deliver(StreamGap{stream}, 0)) {
            gapOwed[stream] = false;
        }
        return !gapOwed[stream];
    }

    void receiveAll(Clock::time_point now) {
        for (size_t i = 0; i < routes.size(); ++i) {
            if (routes[i]->failure.empty()) {
                receiveOver(*routes[i], now);
            }
        }
    }

    void receiveOver(Route& over, Clock::time_point now) {
        Endpoint from;
        for (int i = 0; i < 512; ++i) {
            std::optional<std::span<const uint8_t>> payload;
            try {
                const auto size = over.socket->receive(receiveBuffer, from);
                if (!size) {
                    return;
                }
                counted(over, *size);
                payload = over.link->unwrap(*over.socket, from, std::span(receiveBuffer).first(*size));
            } catch (const transport::PeerClosed& closed) {
                peerClosed(over, closed, now);
                continue;
            } catch (const Failure& failure) {
                routeFailed(over, failure, now);
                return;
            }
            if (payload) {
                handleDatagram(over, from, *payload, now);
            }
        }
    }

    void handleDatagram(Route& over, const Endpoint& from, std::span<const uint8_t> datagram, Clock::time_point now) {
        if (datagram.size() == unknownSize && datagram[0] == unknownKind) {
            unknown(over, from, datagram);
            return;
        }
        if (datagram.size() < dataHeader + crypto::Aead::tagSize || datagram[0] != dataKind ||
            uint32_t(get(datagram, connectionIdOffset, 4)) != receiveId) {
            return; // Handshake repeats and foreign datagrams.
        }
        const uint64_t number = get(datagram, connectionIdOffset + 4, 8);
        if (!replay.fresh(number)) {
            return;
        }
        const uint64_t started = monotonicNs();
        const auto frame = receiveKeys->open(number, datagram.first(dataHeader), datagram.subspan(dataHeader));
        if (!frame) {
            return; // Forged or corrupted: as if lost.
        }
        if (frame->empty()) {
            invalid("empty");
        }
        replay.mark(number);
        confirmed = true;
        peerDatagram = std::max(peerDatagram, datagram.size()); // The peer's, not whatever reaches the socket.
        window.received += 1;
        window.bytes += datagram.size();
        window.highest = std::max(window.highest, number);
        if (!window.firstPacket) {
            window.firstPacket = number;
        }
        if ((*frame)[0] == videoFrame && frame->size() > videoHeader) {
            const uint32_t raw = uint32_t(started / 1000) - uint32_t(get(*frame, sentOffset, 4));
            delay = previousRawDelay ? delay + int32_t(raw - *previousRawDelay) : raw;
            previousRawDelay = raw;
            window.delay = std::min(window.delay.value_or(delay), delay);
        }
        Candidate& candidate = candidateFor(over, over.link->peer() ? std::optional(from) : std::nullopt, now);
        if (!candidate.proven) {
            candidate.received += datagram.size();
        }
        const auto previous = candidate.heard;
        if (previous == Clock::time_point{} || now - previous > pathStall) {
            candidate.firstHeard = now; // After a silence: where an interruption ends.
        }
        candidate.heard = now;
        if (&candidate == current && state != PathState::running) {
            resumed(previous, now);
        }
        if (&candidate == current) {
            const auto kind = (*frame)[0];
            std::optional<uint32_t> sent;
            if ((kind == videoFrame || kind == parityFrame) && frame->size() > videoHeader) {
                sent = uint32_t(get(*frame, sentOffset, 4));
            } else if (kind == audioFrame && frame->size() > audioHeader) {
                sent = uint32_t(get(*frame, 1, 4));
            }
            stallWatch.arrived(number, sent, started, now);
        }
        const bool probe = (*frame)[0] == challengeFrame || (*frame)[0] == responseFrame;
        if (!probe && number > highestTraffic) {
            highestTraffic = number;
            followPeer(candidate, now);
        }
        handleFrame(candidate, *frame, now, started, monotonicNs() - started);
    }

    // A ping or pong, with this end's flags.
    Bytes timeFrame(uint8_t type, uint64_t time) const {
        Bytes frame{type};
        appendBigEndian(frame, time, 8);
        frame.push_back(migrate ? migrates : 0);
        return frame;
    }

    // `arrivedNs`: when the datagram was read.
    void handleFrame(Candidate& over, std::span<const uint8_t> frame, Clock::time_point now, uint64_t arrivedNs,
                     uint64_t decryptTime) {
        switch (frame[0]) {
        case pingFrame: {
            if (frame.size() != timeFrameSize || frame[9] & ~migrates) {
                invalid("ping");
            }
            peerMigrates = (frame[9] & migrates) != 0;
            sendFrame(timeFrame(pongFrame, get(frame, 1, 8)));
            flush(); // At once: the peer's round trip is not to include the rest of what arrived.
            break;
        }
        case pongFrame: {
            if (frame.size() != timeFrameSize || frame[9] & ~migrates) {
                invalid("pong");
            }
            peerMigrates = (frame[9] & migrates) != 0;
            const uint64_t sent = get(frame, 1, 8), nowNs = monotonicNs();
            if (sent && sent <= nowNs) {
                // The first sample replaces the initial guess; later ones are smoothed.
                const uint64_t sample = nowNs - sent, previous = roundTripNs.load();
                roundTripNs = rttMeasured ? (previous * 7 + sample) / 8 : sample;
                rttMeasured = true;
            }
            break;
        }
        case challengeFrame: // Answered over the path it came by: proves that path to the peer.
            if (frame.size() < probeSize) {
                invalid("path challenge");
            }
            over.peerProbed = now;
            sendProbe(over, responseFrame, frame.subspan(1, probeData), now);
            break;
        case responseFrame: {
            if (frame.size() < probeSize) {
                invalid("path response");
            }
            const auto data = frame.subspan(1, probeData);
            for (auto& candidate : candidates) {
                const auto& sent = candidate->challenges;
                const auto found = std::find_if(sent.begin(), sent.end(), [&](const auto& c) {
                    return std::equal(c.first.begin(), c.first.end(), data.begin());
                });
                if (found != sent.end()) {
                    answered(*candidate, found->second, now);
                    break;
                }
            }
            break;
        }
        case messageFrame:
            receiveMessage(streamOf(frame), uint32_t(get(frame, 2, 4)),
                           frame.subspan(std::min(frame.size(), messageHeader)));
            break;
        case ackFrame: {
            const uint8_t stream = streamOf(frame);
            if ((frame.size() - 2) % 4) {
                invalid("acknowledgement");
            }
            size_t delivered = 0;
            {
                std::lock_guard lock(mutex);
                for (size_t offset = 2; offset < frame.size(); offset += 4) {
                    delivered += outgoing[stream].acknowledge(uint32_t(get(frame, offset, 4)));
                }
                if (stream == dataStream) {
                    acked.notify_all();
                }
            }
            if (stream == dataStream) {
                widenDataWindow(delivered, now);
            }
            break;
        }
        case videoFrame:
            if (!receivesVideo) {
                invalid("video sent to the end that sends it");
            }
            receiveFragment(frame, now, arrivedNs, decryptTime);
            break;
        case parityFrame:
            if (!receivesVideo) {
                invalid("parity sent to the end that sends video");
            }
            receiveParity(frame, now, arrivedNs);
            break;
        case nackFrame:
            if (!sendsVideo || frame.size() < 1 + nackEntry || (frame.size() - 1) % nackEntry) {
                invalid("resend request");
            }
            resend(frame, now);
            break;
        case goneFrame:
            if (!receivesVideo || frame.size() < 1 + goneEntry || (frame.size() - 1) % goneEntry) {
                invalid("gone video");
            }
            for (size_t offset = 1; offset < frame.size(); offset += goneEntry) {
                auto& in = videoIn[streamAt(frame, offset)];
                const uint32_t id = uint32_t(get(frame, offset + 1, 4));
                if (id > in.highestFrame) {
                    invalid("gone video that never came");
                }
                if (const auto found = in.assemblies.find(id); found != in.assemblies.end()) {
                    found->second.gone = true;
                }
            }
            deliverFrames(now);
            break;
        case closeFrame:
            if (frame.size() != 2 || frame[1] > Goodbye::restarting) {
                invalid("close");
            }
            failGoodbye(Goodbye::Reason(frame[1]));
        case reportFrame:
            receiveReport(frame, now);
            break;
        case audioFrame:
            if (frame.size() <= audioHeader) {
                invalid("sound");
            }
            soundQueue.add(soundDelay.queueing(uint32_t(get(frame, 1, 4)), arrivedNs, now), now);
            deliverOnly<AudioPacket>(frame.subspan(audioHeader), "sound");
            break;
        case datagramFrame: { // Unreliable data, a forwarder's pipe, or the sharer's sound through it.
            auto message = decode(frame.subspan(1));
            if (!std::holds_alternative<DataDatagram>(message) && !std::holds_alternative<Pipe>(message) &&
                !std::holds_alternative<SealedAudio>(message)) {
                invalid("not a datagram");
            }
            if (!deliver(std::move(message), frame.size() - 1)) {
                fellBehind();
            }
            break;
        }
        default:
            invalid("unknown type " + std::to_string(frame[0]));
        }
    }

    static uint8_t streamOf(std::span<const uint8_t> frame) {
        const uint8_t stream = uint8_t(get(frame, 1, 1));
        if (stream > dataStream) {
            invalid("unknown stream");
        }
        return stream;
    }

    // The video stream a VIDEO, PARITY, NACK or GONE entry names at `offset`.
    static uint8_t streamAt(std::span<const uint8_t> frame, size_t offset) {
        const uint8_t stream = uint8_t(get(frame, offset, 1));
        if (stream >= streams) {
            invalid("unknown video stream");
        }
        return stream;
    }

    // Some stream's frames wait for fragments.
    bool assembling() const noexcept {
        return std::any_of(videoIn.begin(), videoIn.end(),
                           [](const VideoIn& in) { return in.nextFrame <= in.highestFrame; });
    }

    // A frame type that carries one kind of message, and no other.
    template <class Kind>
    void deliverOnly(std::span<const uint8_t> body, const char* what) {
        auto message = decode(body);
        if (!std::holds_alternative<Kind>(message)) {
            invalid(std::string("not a ") + what);
        }
        if (!deliver(std::move(message), body.size())) {
            fellBehind();
        }
    }

    // The peer's account of what arrived: input to adaptive bitrate.
    void receiveReport(std::span<const uint8_t> frame, Clock::time_point now) {
        if (frame.size() != reportSize) {
            invalid("report");
        }
        RateReport report;
        report.windowUs = uint32_t(get(frame, 1, 4));
        report.expected = uint32_t(get(frame, 5, 4));
        report.received = uint32_t(get(frame, 9, 4));
        report.bytes = get(frame, 13, 8);
        report.delayUs = int64_t(get(frame, 21, 8));
        const uint64_t first = get(frame, 37, 8);
        if (first < pathStartPacket) {
            return; // It began on another path, or before this one stalled.
        }
        if (const uint64_t spaced = get(frame, 29, 4) * 1000) {
            pathRate.spaced(spaced, now);
        }
        if (const uint64_t unspaced = get(frame, 33, 4) * 1000) {
            pathRate.unspaced(unspaced, now);
        }
        // What went off the path (probes) never came this way.
        while (!probePackets.empty() && probePackets.front() < first) {
            probePackets.pop_front();
        }
        const auto off = std::count_if(probePackets.begin(), probePackets.end(),
                                       [&](uint64_t number) { return number < first + report.expected; });
        report.expected -= std::min(report.expected, uint32_t(off));
        adjustParity(report, now);
        const uint64_t backlog = decideRate ? backlogNs() : 0;
        std::optional<uint32_t> target;
        uint64_t received = 0, queueing = 0;
        uint32_t loss = 0;
        bool clear = true;
        {
            std::unique_lock lock(rateMutex);
            if (!rate) {
                return;
            }
            const uint32_t current = rate->target();
            report.sentBps = uint64_t(encodedBps);
            report.roundTripUs = rttMeasured ? roundTripNs / 1000 : 0;
            uint32_t decision = rate->propose(report, now);
            if (decideRate && !fixedRate) {
                rcs_rate_report seen{};
                seen.struct_size = sizeof(seen);
                seen.timestamp_ns = monotonicNs();
                seen.window_us = report.windowUs;
                seen.expected_packets = report.expected;
                seen.received_packets = report.received;
                seen.received_bytes = report.bytes;
                seen.one_way_delay_us = report.delayUs;
                seen.queue_delay_us = uint32_t(std::min<uint64_t>(rate->queueDelayUs(), UINT32_MAX));
                seen.loss_permille = rate->lossPermille();
                seen.received_bps = rate->receivedBps();
                seen.round_trip_ns = roundTripNs;
                seen.path_changes = rate->pathChanges();
                seen.encoder_bps = uint64_t(encodedBps);
                seen.send_backlog_ns = backlog;
                seen.current_bps = current;
                seen.minimum_bps = rate->minimum();
                seen.maximum_bps = rate->maximum();
                rcs_rate_decision decided{sizeof(decided), decision};
                lock.unlock(); // status() and setVideoRate() go on meanwhile.
                decideRate(seen, decided);
                lock.lock();
                decision = decided.bitrate_bps;
            }
            target = rate->settle(decision);
            clear = rate->clear();
            pathClear = clear;
            if (target) {
                pacingBps = pacing(*target);
                received = rate->receivedBps();
                loss = rate->lossPermille();
                queueing = rate->queueDelayUs();
            }
        }
        lastReport = now;
        if (!clear) {
            if (now - dataCut >= roundTrip()) {
                dataWindow = std::max(dataWindow / 2, minDataWindow());
                dataCut = now;
            }
        } else if (dataHeld) {
            dataWindow = std::min(dataWindow + dataWindow / 4, maxDataWindow);
        }
        dataHeld = false;
        if (target) {
            log::debug("network", "Adaptive bitrate " + std::to_string(*target / 1000) + " kbit/s (arrived " +
                                      std::to_string(received / 1000) + " kbit/s, loss " + std::to_string(loss) +
                                      " per mille, queueing " + std::to_string(queueing / 1000) + " ms)");
        }
    }

    // AUTO: parity while the viewer's reports show loss, three times its
    // smoothed rate (10% at least, the host's most at most); none parityHold
    // after the last loss. None on an ordered path: what it loses is what
    // this end's own queue could not hold, which parity would only fill more.
    void adjustParity(const RateReport& report, Clock::time_point now) {
        if (report.expected) {
            const uint32_t lost = report.expected - std::min(report.received, report.expected);
            lossSeen = lossSeen * 0.8 + 0.2 * lost / report.expected;
            if (lost) {
                lastLoss = now;
            }
        }
        if (fecMode == RCS_FEC_AUTO) {
            const bool lossy = !ordered() && lastLoss != Clock::time_point{} && now - lastLoss < parityHold;
            fecPercent = lossy ? std::min(fecLimit.load(), std::max(10u, uint32_t(std::ceil(lossSeen * 300)))) : 0;
        }
    }

    // The host's setting; AUTO keeps its parity within the new most.
    void setParity(const FecSetting& fec) {
        fecMode = fec.mode;
        fecLimit = fec.percent;
        fecPercent = fec.mode == RCS_FEC_FIXED  ? fec.percent
                     : fec.mode == RCS_FEC_AUTO ? std::min(fecPercent.load(), fec.percent)
                                                : 0;
    }

    // Queued video as time at the current pacing rate.
    uint64_t backlogNs() {
        size_t queued;
        {
            std::lock_guard lock(mutex);
            queued = videoBytes;
        }
        const double bytesPerSecond = currentPace().bytesPerSecond;
        return bytesPerSecond > 0 ? uint64_t(double(queued) * 1e9 / bytesPerSecond) : 0;
    }

    // The slower pace: while the viewer's reports show queueing or loss (not
    // clear by the rules' thresholds), and the least video leaves at.
    // Adaptive: keyframes leave at 1.5 times the target, within a few frame
    // intervals, without overrunning the path the target was measured on;
    // never below what the encoder actually produces (content that does not
    // fit the target): the path must see the real load to push back on it.
    // Fixed: at up to four times the bitrate, so keyframes go out within a few
    // frame intervals without bursting at line rate. Sound leaves ahead of
    // video and takes from the same budget, so the rate covers it on top.
    // Never above the host's maximum. Holding rateMutex.
    uint64_t pacing(uint32_t target) const {
        const auto sound = uint64_t(immediateBps * 1.1);
        const uint64_t pace =
            fixedRate ? std::max<uint64_t>(uint64_t(target) * 4, 20'000'000) + sound
                      : std::max({uint64_t(target) * 3 / 2, uint64_t(encodedBps * 1.25), uint64_t(1'000'000)}) + sound;
        return std::min(pace, maxPacingBps.load());
    }

    // The host's pacing (rcs_video_tuning_config). Holding rateMutex.
    void setPacing(const RateRules& rules) {
        maxPacingBps = rules.maxPacingBps;
        frameSpreadNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(rules.frameSpread).count());
        pathPacing = rules.pathPacing;
        pathBurstNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(rules.pathBurst).count());
        pacingBps = pacing(rate->target());
    }

    // The rate video leaves at now, and what of it may leave at once. On a
    // clear path: the queue within the frame spread, at least at the slower
    // pace and at most at the maximum, in batches of a millisecond's worth
    // (what the system's timers resolve), so that a picture the spread allows
    // leaves at once; where the path's rate is known, at most pathPacing
    // percent of it (not below the slower pace) once the batch of pathBurst
    // of it left. Otherwise: the slower pace, in batches of 2 ms.
    struct Pace {
        double bytesPerSecond, burst;
    };
    Pace currentPace() const {
        const double slow = double(pacingBps.load()) / 8;
        const double leastBurst = 4.0 * double(datagramLimit);
        if (slow <= 0 || !pathClear) {
            return {slow, std::max(slow * 0.002, leastBurst)};
        }
        const double most = double(maxPacingBps.load()) / 8;
        const double fast = std::min(most, std::max(slow, frameSpreadNs ? double(spreadBps.load()) / 8 : most));
        const double burst = fast * 0.001 + leastBurst, path = double(pathBps.load()) / 8;
        if (path <= 0 || !pathPacing) {
            return {fast, burst};
        }
        const double paced = std::max(slow, std::min(fast, path * pathPacing / 100));
        return {paced, std::min(burst, std::max(paced * 0.001 + leastBurst, path * double(pathBurstNs.load()) / 1e9))};
    }

    // The encoder's actual output rate, from what send() queued.
    void measureEncoded(Clock::time_point now) {
        if (now - encodedSince < 250ms) {
            return;
        }
        uint64_t queuedVideo, queuedAudio;
        {
            std::lock_guard lock(mutex);
            queuedVideo = std::exchange(enqueuedBytes, 0);
            queuedAudio = std::exchange(enqueuedImmediateBytes, 0);
        }
        const double seconds = std::chrono::duration<double>(now - encodedSince).count();
        const double videoBps = double(queuedVideo) * 8 / seconds;
        encodedSince = now;
        std::lock_guard lock(rateMutex);
        encodedBps = encodedBps ? 0.7 * encodedBps + 0.3 * videoBps : videoBps;
        // What leaves at once (sound, unreliable data), as the pacer counts it.
        immediateBps = 0.7 * immediateBps + 0.3 * double(queuedAudio) * 8 / seconds;
        pacingBps = pacing(rate->target());
    }

    // Every 50 ms in which video arrived: what, how much, how late. Only the
    // side sending video adapts, so the other direction needs no reports.
    // Windows without video still move the count on: the sound and messages
    // they carried were not lost.
    void sendReport(Clock::time_point now) {
        if (now - window.started < reportInterval) {
            return;
        }
        if (window.delay) {
            Bytes report{reportFrame};
            appendBigEndian(
                report, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - window.started).count()),
                4);
            const uint64_t base = reportedHighest ? reportedHighest : window.firstPacket - 1;
            appendBigEndian(report, window.highest > base ? window.highest - base : 0, 4);
            appendBigEndian(report, window.received, 4);
            appendBigEndian(report, window.bytes, 8);
            appendBigEndian(report, uint64_t(*window.delay), 8);
            appendBigEndian(report, std::min<uint64_t>(window.spacedBps / 1000, UINT32_MAX), 4);
            appendBigEndian(report, std::min<uint64_t>(window.unspacedBps / 1000, UINT32_MAX), 4);
            appendBigEndian(report, base + 1, 8);
            sendFrame(report);
        }
        reportedHighest = std::max(reportedHighest, window.highest);
        const double seconds = std::chrono::duration<double>(now - window.started).count();
        const double bps = double(window.bytes) * 8 / seconds;
        arrivalBps = arrivalBps ? 0.8 * arrivalBps + 0.2 * bps : bps;
        window = Window{};
        window.started = now;
    }

    // Acknowledges only what it keeps: a message that finds no room unacknowledged
    // comes again. The next one in order always has room; it drains the others
    // while the session has room for them (handOver).
    void receiveMessage(uint8_t stream, uint32_t wire, std::span<const uint8_t> body) {
        auto& in = incoming[stream];
        const uint64_t id = unwrapped(in.next, wire);
        if (id > in.next && (in.early.size() >= maxOutOfOrder || in.earlyBytes + body.size() > maxEarlyBytes) &&
            !in.early.contains(id)) {
            return;
        }
        in.acks.push_back(wire);
        if (id < in.next) {
            return;
        }
        if (in.early.emplace(id, Bytes(body.begin(), body.end())).second) {
            in.earlyBytes += body.size();
        }
        handOver(stream);
    }

    // Hands what is next in order on `stream` to the session; what finds no
    // room (maxInbound) waits where it is (`held`), as the peer's window does.
    void handOver(uint8_t stream) {
        auto& in = incoming[stream];
        for (auto next = in.early.find(in.next); next != in.early.end(); next = in.early.find(in.next)) {
            auto message = decode(next->second);
            // Data parts have a stream of their own; video, sound and
            // unreliable data have frames of their own.
            const bool part = std::holds_alternative<DataPart>(message);
            const bool elsewhere =
                std::holds_alternative<EncodedPacket>(message) || std::holds_alternative<AudioPacket>(message) ||
                std::holds_alternative<DataDatagram>(message) || std::holds_alternative<Pipe>(message) ||
                std::holds_alternative<SealedVideo>(message) || std::holds_alternative<SealedAudio>(message);
            if (stream == dataStream ? !part : part || elsewhere) {
                invalid("a message on the wrong stream");
            }
            if (!deliver(std::move(message), next->second.size())) {
                held = true;
                return;
            }
            in.earlyBytes -= next->second.size();
            in.early.erase(next);
            ++in.next;
        }
    }

    static Assembly& assembly(VideoIn& in, uint32_t id, Clock::time_point now) {
        auto [found, added] = in.assemblies.try_emplace(id);
        if (added) {
            found->second.detected = now;
        }
        return found->second;
    }

    // The frame of `count` fragments that a fragment or parity of `stream`'s
    // frame `id` belongs to; none for a frame complete or given up on.
    Assembly* frameOf(uint8_t stream, uint32_t id, uint16_t count, Clock::time_point now) {
        if (!count || count > maxFragments) {
            invalid("video fragment numbers");
        }
        auto& in = videoIn[stream];
        if (id < in.nextFrame) {
            return nullptr; // A late copy.
        }
        if (id - in.nextFrame >= frameWindow) {
            invalid("a video frame far ahead of the others");
        }
        // A stream's fragments leave in frame order: whatever of its earlier
        // frames did not arrive by now left before this did.
        for (uint32_t earlier = std::max(in.nextFrame, in.highestFrame); earlier < id; ++earlier) {
            Assembly& a = assembly(in, earlier, now);
            if (a.laterSeen == Clock::time_point{}) {
                a.laterSeen = now;
            }
        }
        in.highestFrame = std::max(in.highestFrame, id);
        Assembly& a = assembly(in, id, now);
        if (!a.count) {
            if (pendingFragments + count > maxPendingFragments) {
                invalid("more video waiting for fragments than any sender keeps");
            }
            pendingFragments += count;
            a.count = count;
            a.parts.resize(count);
            a.nacks.assign(count, 0);
            a.due.assign(count, Clock::time_point{});
            a.groups.resize((count + fecGroup - 1) / fecGroup);
            a.first = now;
        } else if (a.count != count) {
            invalid("a video frame's fragment count changed");
        }
        return &a;
    }

    // A group's parity fragments, as its fragments say (0: none).
    static void groupParity(Group& group, uint8_t parity) {
        if (!parity) {
            return;
        }
        if (group.parity && group.parity != parity) {
            invalid("a group's parity count changed");
        }
        group.parity = parity;
        group.parts.resize(parity);
    }

    // A fragment or parity of `a` (its frame) read at `arrivedNs`.
    static void arrived(Assembly& a, std::span<const uint8_t> frame, uint64_t arrivedNs) {
        const auto sent = uint32_t(get(frame, sentOffset, 4));
        if (!a.arrivals++) {
            a.bunchEnd = arrivedNs;
            a.sentLast = sent;
        } else if (a.bunched && arrivedNs - a.arrivedLast < uint64_t(std::chrono::nanoseconds(bunchGap).count())) {
            a.bunchEnd = arrivedNs;
        } else {
            a.sentAfter = a.bunched || int32_t(sent - a.sentAfter) < 0 ? sent : a.sentAfter;
            a.bunched = false;
            a.afterBunch += frame.size();
        }
        a.sentLast = int32_t(sent - a.sentLast) > 0 ? sent : a.sentLast;
        a.arrivedLast = arrivedNs;
    }

    // The path's rate complete picture `a` shows, and whether the path
    // spaced it (see rateFragments); none where it tells nothing.
    static std::optional<std::pair<uint64_t, bool>> pictureRate(const Assembly& a) {
        if (a.arrivals < rateFragments || a.bunched || a.wholeNacks ||
            std::any_of(a.nacks.begin(), a.nacks.end(), [](uint8_t n) { return n != 0; })) {
            return std::nullopt;
        }
        const uint64_t took = a.arrivedLast - a.bunchEnd, left = uint64_t(uint32_t(a.sentLast - a.sentAfter)) * 1000;
        if (took > left + left / 20 + 200'000) {
            return std::pair(a.afterBunch * 8 * 1'000'000'000 / took, true);
        }
        if (left >= uint64_t(std::chrono::nanoseconds(pacedTime).count())) {
            return std::pair(a.afterBunch * 8 * 1'000'000'000 / left, false);
        }
        return std::nullopt;
    }

    void receiveFragment(std::span<const uint8_t> frame, Clock::time_point now, uint64_t arrivedNs,
                         uint64_t decryptTime) {
        if (frame.size() <= videoHeader) {
            invalid("an empty video fragment");
        }
        const uint8_t stream = streamAt(frame, 1);
        const uint32_t id = uint32_t(get(frame, 2, 4));
        const uint16_t index = uint16_t(get(frame, 6, 2)), count = uint16_t(get(frame, 8, 2));
        if (index >= count) {
            invalid("video fragment numbers");
        }
        Assembly* found = frameOf(stream, id, count, now);
        if (!found) {
            return;
        }
        Assembly& a = *found;
        if (a.received) {
            a.pause = std::max(a.pause, now - a.last);
        }
        a.last = now;
        a.decryptNs += decryptTime;
        arrived(a, frame, arrivedNs);
        const size_t g = index / fecGroup;
        groupParity(a.groups[g], frame[videoHeader - 1]);
        // A later group's fragment: the earlier groups' parity left before it.
        for (; a.closedBelow < g; ++a.closedBelow) {
            if (a.groups[a.closedBelow].closed == Clock::time_point{}) {
                a.groups[a.closedBelow].closed = now;
            }
        }
        if (!a.parts[index].empty()) {
            return; // A copy, or rebuilt from parity before it came.
        }
        resendRecovered += a.nacks[index] || a.wholeNacks; // Asked for again: by itself, or with its frame.
        a.parts[index].assign(frame.begin() + videoHeader, frame.end());
        // Fragments leave in order: the ones passed over were lost, unless
        // they turn up within reorderDelay.
        for (uint32_t skipped = a.received ? a.highest + 1u : 0u; skipped < index; ++skipped) {
            if (a.parts[skipped].empty() && a.due[skipped] == Clock::time_point{}) {
                a.due[skipped] = now + reorderDelay;
            }
        }
        if (!a.received || index > a.highest) {
            a.highest = index;
        }
        ++a.received;
        recover(a, g);
        deliverFrames(now);
    }

    void receiveParity(std::span<const uint8_t> frame, Clock::time_point now, uint64_t arrivedNs) {
        if (frame.size() <= parityHeader) {
            invalid("an empty parity fragment");
        }
        const uint8_t stream = streamAt(frame, 1);
        const uint32_t id = uint32_t(get(frame, 2, 4));
        const size_t g = size_t(get(frame, 6, 2));
        const uint16_t count = uint16_t(get(frame, 8, 2));
        const uint8_t parity = frame[14], which = frame[15];
        const uint16_t last = uint16_t(get(frame, 16, 2));
        Assembly* found = frameOf(stream, id, count, now);
        if (!found) {
            return;
        }
        Assembly& a = *found;
        arrived(a, frame, arrivedNs);
        const auto block = frame.subspan(parityHeader);
        if (g >= a.groups.size() || !parity || which >= parity ||
            std::min(fecGroup, size_t(count) - g * fecGroup) + parity > fec::maxBlocks || !last ||
            last > block.size()) {
            invalid("parity numbers");
        }
        Group& group = a.groups[g];
        groupParity(group, parity);
        if ((group.lastLength && group.lastLength != last) ||
            std::any_of(group.parts.begin(), group.parts.end(),
                        [&](const Bytes& p) { return !p.empty() && p.size() != block.size(); })) {
            invalid("a group's parity changed its lengths");
        }
        group.lastLength = last;
        if (which == parity - 1 && group.closed == Clock::time_point{}) {
            group.closed = now; // Its last parity: all of it passed.
        }
        if (!group.parts[which].empty()) {
            return;
        }
        group.parts[which].assign(block.begin(), block.end());
        ++group.received;
        recover(a, g);
        deliverFrames(now);
    }

    // Rebuilds the missing video fragments of group `g` once at least as
    // many of its parity fragments arrived.
    void recover(Assembly& a, size_t g) {
        Group& group = a.groups[g];
        const size_t first = g * fecGroup, k = std::min(fecGroup, size_t(a.count) - first);
        const auto missing = size_t(
            std::count_if(a.parts.begin() + ptrdiff_t(first), a.parts.begin() + ptrdiff_t(first + k),
                          [](const Bytes& part) { return part.empty(); }));
        if (!missing || missing > group.received) {
            return;
        }
        const size_t size =
            std::find_if(group.parts.begin(), group.parts.end(), [](const Bytes& p) { return !p.empty(); })->size();
        const auto lengthOf = [&](size_t i) { return i + 1 == k ? size_t(group.lastLength) : size; };
        std::vector<Bytes> data(k);
        for (size_t i = 0; i < k; ++i) {
            const Bytes& part = a.parts[first + i];
            if (!part.empty()) {
                if (part.size() != lengthOf(i)) {
                    invalid("a video fragment that does not match its group's parity");
                }
                data[i] = part;
            }
        }
        fec::recover(data, group.parts, size);
        for (size_t i = 0; i < k; ++i) {
            if (a.parts[first + i].empty()) {
                data[i].resize(lengthOf(i));
                a.parts[first + i] = std::move(data[i]);
                a.highest = std::max(a.highest, uint16_t(first + i));
                ++a.received;
                ++fecRecovered;
            }
        }
    }

    // Hands each stream's complete frames over in order; gives up on its
    // oldest when the sender no longer has it, or when it is too old: four
    // round trips, or three times what crossing the path at the rate it
    // delivers takes (a large keyframe on a slow path), on an ordered path
    // twice orderedWait at least. A complete frame the session has no room
    // for (maxInbound) is given up on too.
    void deliverFrames(Clock::time_point now) {
        const double arrival = std::max(arrivalBps, 1e6);
        const Clock::duration least = ordered() ? Clock::duration(2 * orderedWait) : Clock::duration(250ms);
        for (uint8_t stream = 0; stream < streams; ++stream) {
            auto& in = videoIn[stream];
            gapTold(stream);
            while (in.nextFrame <= in.highestFrame) {
                Assembly& a = assembly(in, in.nextFrame, now);
                if (a.count && a.received == a.count) {
                    Bytes body;
                    for (const auto& part : a.parts) {
                        body.insert(body.end(), part.begin(), part.end());
                    }
                    auto message = decode(body);
                    const auto* packet = std::get_if<EncodedPacket>(&message);
                    const auto* sealed = std::get_if<SealedVideo>(&message); // A forwarder's hop.
                    if (packet ? packet->stream != stream : !sealed || sealed->stream != stream) {
                        invalid("video that is not a video packet of its stream");
                    }
                    receiveNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - a.first).count());
                    decryptNs = a.decryptNs;
                    if (const auto measured = pictureRate(a)) {
                        auto& most = measured->second ? window.spacedBps : window.unspacedBps;
                        most = std::max(most, measured->first);
                    }
                    if (!gapTold(stream) || !deliver(std::move(message), body.size())) {
                        gapOwed[stream] = true;
                        ++framesLost;
                        fellBehind();
                    }
                } else if (const auto giveUp = std::max(
                               {least, Clock::duration(roundTrip() * 4),
                                std::chrono::duration_cast<Clock::duration>(
                                    std::chrono::duration<double>(3.0 * a.count * peerDatagram * 8 / arrival))});
                           a.gone || now - a.detected > giveUp) {
                    ++gaps;
                    ++framesLost;
                    gapOwed[stream] = true;
                    gapTold(stream);
                } else {
                    break;
                }
                pendingFragments -= a.count;
                recentPause = std::max(a.pause, recentPause - recentPause / 8);
                in.assemblies.erase(in.nextFrame);
                ++in.nextFrame;
            }
        }
    }

    // How long the rest of a frame may take before it counts as lost when no
    // later frame shows that it left: two fragments' time at the rate video
    // arrives lately, or arrived for this frame (a bottleneck spaces them);
    // at least leastQuiet, and the longest pause within the latest frames.
    // On an ordered path, orderedWait: what is missing there only waits
    // behind its path's own retransmission.
    Clock::duration quietTime(const Assembly& a) const {
        if (ordered()) {
            return orderedWait;
        }
        double spacing = double(peerDatagram) * 8 / (arrivalBps > 0 ? arrivalBps : 1e6);
        if (a.received >= 2) {
            spacing = std::max(spacing, std::chrono::duration<double>(a.last - a.first).count() / (a.received - 1));
        }
        const auto pause = std::min<Clock::duration>(std::max(a.pause, recentPause), mostPause) + reorderDelay;
        return std::max({Clock::duration(leastQuiet), pause,
                         std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(2 * spacing))});
    }

    // Asks for fragments that went missing, and again while they stay missing:
    // the ones below the highest that arrived at once (after reorderDelay);
    // the rest of a frame once it is plausibly lost, not while it may still
    // be on its way (a slow path spaces fragments further than reorderDelay).
    void requestMissing(Clock::time_point now) {
        const auto again = std::max<Clock::duration>(ordered() ? orderedWait : 20ms, roundTrip() * 3 / 2);
        Bytes nack{nackFrame};
        uint8_t stream = 0;
        const auto add = [&](uint32_t id, uint16_t index) {
            nack.push_back(stream);
            appendBigEndian(nack, id, 4);
            appendBigEndian(nack, index, 2);
            if (nack.size() + nackEntry > datagramLimit - dataHeader - crypto::Aead::tagSize - link().overhead()) {
                sendFrame(nack);
                nack.assign(1, nackFrame);
            }
        };
        // The first request for a frame tells the session at once, so that
        // the sharer's encoder can leave the frame out while it still holds
        // what came before (RefLost).
        const auto told = [&](Assembly& a) {
            if (!std::exchange(a.missingTold, true)) {
                deliver(VideoMissing{stream}, 0);
            }
        };
        for (; stream < streams; ++stream) {
            requestMissing(videoIn[stream], add, told, again, now);
        }
        if (nack.size() > 1) {
            sendFrame(nack);
        }
    }

    template <class Add, class Told>
    void requestMissing(VideoIn& in, Add& add, Told& told, Clock::duration again, Clock::time_point now) {
        for (auto& [id, a] : in.assemblies) {
            if (!a.count) { // Nothing of it arrived, and a later frame did.
                if (now - a.detected >= reorderDelay && a.wholeNacks < maxNacks &&
                    (!a.wholeNacks || now - a.wholeNacked >= again)) {
                    add(id, wholeFrame);
                    ++a.wholeNacks;
                    a.wholeNacked = now;
                    told(a);
                }
                continue;
            }
            if (a.received == a.count) {
                continue;
            }
            const auto tail =
                std::min(a.laterSeen == Clock::time_point{} ? Clock::time_point::max() : a.laterSeen + reorderDelay,
                         a.last + quietTime(a));
            for (uint16_t index = 0; index < a.count; ++index) {
                if (!a.parts[index].empty() || a.nacks[index] >= maxNacks) {
                    continue;
                }
                auto due = a.due[index] != Clock::time_point{} ? a.due[index]
                           : index > a.highest                 ? tail
                                                               : Clock::time_point::max();
                // A group with parity: the first request waits until all its
                // parity passed, which may rebuild what is missing.
                if (const Group& group = a.groups[index / fecGroup]; group.parity && !a.nacks[index]) {
                    const auto passed = group.closed != Clock::time_point{} ? group.closed
                                        : a.laterSeen != Clock::time_point{} ? a.laterSeen
                                                                             : a.last + quietTime(a);
                    due = std::max(due, passed + reorderDelay);
                }
                if (now < due) {
                    continue;
                }
                add(id, index);
                ++a.nacks[index];
                a.due[index] = now + again;
                told(a);
            }
        }
    }

    // Queues what the peer asks for, ahead of new video: never what still
    // waits in the queue, and never a fragment that left less than a round
    // trip ago (the request cannot be about that copy). Of frames no longer
    // kept (older than keepSent, or dropped with a backlog) the peer learns
    // that they are gone, so that it gives up on them at once.
    void resend(std::span<const uint8_t> nack, Clock::time_point now) {
        const auto recent = roundTrip();
        Bytes gone{goneFrame};
        {
            std::lock_guard lock(mutex);
            queueAgain(nack, now, recent, gone);
        }
        // In frames this end's datagrams carry: the request came in the peer's, which may be larger.
        const size_t most = (datagramLimit - dataHeader - crypto::Aead::tagSize - link().overhead() - 1) / goneEntry;
        for (size_t offset = 1; offset < gone.size();) {
            const size_t size = std::min(gone.size() - offset, most * goneEntry);
            Bytes frame{goneFrame};
            frame.insert(frame.end(), gone.begin() + ptrdiff_t(offset), gone.begin() + ptrdiff_t(offset + size));
            sendFrame(frame);
            offset += size;
        }
    }

    // Holding mutex.
    void queueAgain(std::span<const uint8_t> nack, Clock::time_point now, Clock::duration recent, Bytes& gone) {
        std::vector<QueuedFragment> again;
        const auto add = [&](uint8_t stream, uint32_t id, SentFrame& frame, uint16_t index) {
            SentFragment& fragment = frame.fragments[index];
            if (fragment.queued || now - fragment.sent < recent) {
                return;
            }
            fragment.queued = true;
            ++frame.queued;
            videoBytes += fragment.frame.size();
            largestQueued = std::max(largestQueued, frame.bytes);
            again.push_back({stream, id, index});
        };
        for (size_t offset = 1; offset < nack.size(); offset += nackEntry) {
            const uint8_t stream = streamAt(nack, offset);
            const uint32_t id = uint32_t(get(nack, offset + 1, 4));
            const uint16_t index = uint16_t(get(nack, offset + 5, 2));
            if (id >= nextFrameId[stream]) {
                invalid("a resend request for video never sent");
            }
            const auto found = sentFrames[stream].find(id);
            if (found == sentFrames[stream].end()) {
                if (gone.size() == 1 || gone[gone.size() - goneEntry] != stream ||
                    loadBigEndian(gone, gone.size() - 4, 4) != id) {
                    gone.push_back(stream);
                    appendBigEndian(gone, id, 4);
                }
                continue;
            }
            auto& frame = found->second;
            if (index == wholeFrame) { // Its video; parity is never sent again.
                for (uint16_t i = 0; i < frame.videoCount; ++i) {
                    add(stream, id, frame, i);
                }
            } else if (index < frame.videoCount) {
                add(stream, id, frame, index);
            } else {
                invalid("a resend request beyond the frame");
            }
        }
        video.insert(video.begin(), again.begin(), again.end()); // The receiver is waiting for these.
    }

    // ---- Timers (network thread) ------------------------------------------------

    void tick(Clock::time_point now) {
        followPaths(now);
        updateFragmentSize();
        pathBps = pathRate.bps(now);
        soundWait.roll(now);
        soundQueue.roll(now);
        if (now >= nextStallSummary) {
            const auto summary = stallWatch.summary(now);
            std::lock_guard lock(rateMutex);
            stalls = summary;
            nextStallSummary = now + 250ms;
        }
        if (now >= nextPing) {
            sendFrame(timeFrame(pingFrame, monotonicNs()));
            nextPing = now + pingInterval;
        }
        if (!confirmed && now >= nextFinished) {
            sendRaw(finishedDatagram);
            nextFinished = now + handshakeInterval;
        }
        for (uint8_t stream = controlStream; stream <= dataStream; ++stream) {
            auto& acks = incoming[stream].acks;
            if (acks.empty()) {
                continue;
            }
            Bytes ack{ackFrame, stream};
            for (const uint32_t id : acks) {
                appendBigEndian(ack, id, 4);
                if (ack.size() + 4 > maxFragment + videoHeader) {
                    sendFrame(ack);
                    ack.resize(2);
                }
            }
            if (ack.size() > 2) {
                sendFrame(ack);
            }
            acks.clear();
        }
        std::deque<Immediate> first;
        std::vector<Bytes> due;
        {
            std::lock_guard lock(mutex);
            first = takeImmediate();
            auto& control = outgoing[controlStream];
            control.expire(now);
            for (; !control.ready.empty(); control.ready.pop_front()) {
                const uint64_t id = control.ready.front();
                if (const auto found = control.unacked.find(id); found != control.unacked.end()) {
                    control.sent(id, found->second, now, resendTimeout());
                    due.push_back(found->second.frame);
                }
            }
            outgoing[dataStream].expire(now); // The pacer sends them.
            for (auto& sent : sentFrames) {
                while (!sent.empty()) {
                    const auto& frame = sent.begin()->second;
                    if (frame.queued || now - frame.lastSent <= keepSent) {
                        break;
                    }
                    sent.erase(sent.begin());
                }
            }
        }
        sendImmediate(first);
        for (const auto& frame : due) {
            sendFrame(frame);
        }
        requestMissing(now);
        deliverFrames(now);
        sendReport(now);
        if (rate) {
            measureEncoded(now);
        }
        if (now - summarized >= summaryInterval) {
            if (gaps) {
                log::debug("network", "Gave up on " + std::to_string(gaps) + " video frames in the last " +
                                          std::to_string(summaryInterval.count()) + " s (fragments did not arrive)");
                gaps = 0;
            }
            summarized = now;
        }
    }

    // What waits to go at once, taking from video's budget (video yields to
    // it, by a burst at most: a flood of it, as a forwarder's pipes carry the
    // end-to-end connections' file transfers and copies, would otherwise hold
    // the pictures back for seconds); nothing while the path is silent (it
    // would be lost anyway). Holding mutex.
    std::deque<Immediate> takeImmediate() {
        if (silent) {
            immediate.clear();
            return {};
        }
        for (const auto& waiting : immediate) {
            tokens -= double(waiting.frame.size() + dataHeader + crypto::Aead::tagSize);
        }
        tokens = std::max(tokens, -currentPace().burst);
        return std::exchange(immediate, {});
    }

    // Sound leaves stamped as video does, and its wait here is measured.
    void sendImmediate(std::deque<Immediate>& waiting) {
        for (auto& [frame, queued] : waiting) {
            if (frame[0] != audioFrame) {
                sendFrame(frame);
                continue;
            }
            const auto now = Clock::now();
            soundWait.add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - queued).count()), now);
            storeBigEndian(frame, 1, uint32_t(monotonicNs() / 1000), 4);
            sendFrame(frame, audioDscp);
        }
    }

    // Data acknowledged: without reports to steer its window, it grows by as much.
    void widenDataWindow(size_t delivered, Clock::time_point now) {
        if (dataHeld && now - lastReport > 4 * reportInterval) {
            dataWindow = std::min(dataWindow + delivered, maxDataWindow);
        }
    }

    // The path went silent: queued video would come late, if at all. Its
    // frames are gone (the receiver gives up on them when it asks).
    void dropQueuedVideo() {
        std::lock_guard lock(mutex);
        for (const auto& queued : video) {
            sentFrames[queued.stream].erase(queued.frame);
        }
        video.clear();
        videoBytes = 0;
        largestQueued = 0;
        spreadBps = 0;
    }

    // Sends queued video within the pacing budget, then reliable data within
    // its window, and what is to go at once ahead of either whenever it comes;
    // returns when to continue. Nothing while the path is silent.
    Clock::time_point pace(Clock::time_point now) {
        const auto [bytesPerSecond, burst] = currentPace();
        if (bytesPerSecond > 0) {
            tokens = std::min(burst, tokens + bytesPerSecond * std::chrono::duration<double>(now - refilled).count());
        }
        refilled = now;
        if (silent) {
            return Clock::time_point::max();
        }
        for (;;) {
            bool fragment = false;
            std::deque<Immediate> ahead;
            {
                std::lock_guard lock(mutex);
                auto& data = outgoing[dataStream];
                if (!immediate.empty()) {
                    ahead = takeImmediate();
                } else if (!video.empty() && bytesPerSecond > 0 && tokens < 0) {
                    return now +
                           std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(-tokens / bytesPerSecond));
                } else if (!video.empty()) {
                    const auto [stream, id, index] = video.front();
                    video.pop_front();
                    auto& frame = sentFrames[stream].at(id);
                    auto& sent = frame.fragments[index];
                    videoBytes -= sent.frame.size();
                    if (!videoBytes) {
                        largestQueued = 0;
                        spreadBps = 0;
                    }
                    sent.queued = false;
                    if (index >= frame.videoCount && sent.sent == Clock::time_point{}) {
                        ++paritySent;
                    }
                    sent.sent = frame.lastSent = now;
                    --frame.queued;
                    outgoingFrame.assign(sent.frame.begin(), sent.frame.end());
                    fragment = true;
                    if (now - videoSent > 4 * reportInterval) {
                        dataWindow = std::min(dataWindow, initialDataWindow());
                    }
                    videoSent = now;
                } else if (!data.ready.empty()) {
                    const uint64_t id = data.ready.front();
                    const auto found = data.unacked.find(id);
                    if (found == data.unacked.end()) {
                        data.ready.pop_front(); // Acknowledged meanwhile.
                        continue;
                    }
                    auto& entry = found->second;
                    if (!entry.attempts && data.onWire && data.onWire + entry.frame.size() > dataWindow) {
                        dataHeld = true;
                        return Clock::time_point::max(); // Until acknowledgements come.
                    }
                    data.ready.pop_front();
                    data.sent(id, entry, now, resendTimeout());
                    outgoingFrame.assign(entry.frame.begin(), entry.frame.end());
                } else {
                    return Clock::time_point::max();
                }
            }
            if (!ahead.empty()) {
                sendImmediate(ahead);
                continue;
            }
            if (fragment) { // The send time, as it leaves; video spends the pacing budget.
                storeBigEndian(outgoingFrame, sentOffset, uint32_t(monotonicNs() / 1000), 4);
                tokens -= double(outgoingFrame.size() + dataHeader + crypto::Aead::tagSize);
            }
            sendFrame(outgoingFrame);
        }
    }

    bool busy() {
        std::lock_guard lock(mutex);
        return !outgoing[controlStream].unacked.empty() || !outgoing[dataStream].unacked.empty() || !video.empty() ||
               !immediate.empty();
    }

    void run() noexcept {
        try {
            for (;;) {
                {
                    std::lock_guard lock(mutex);
                    if (stopping) {
                        return;
                    }
                }
                const auto now = Clock::now();
                if (std::exchange(held, false)) { // Reliable messages wait for room: the session may have made some.
                    handOver(controlStream);
                    handOver(dataStream);
                }
                receiveAll(now);
                flush();
                tick(now);
                const auto paced = pace(now);
                flush();
                const auto timer = now + ((busy() || assembling() || !confirmed) ? 5ms : 50ms);
                waiter.wait(std::min({paced, timer, nextPing}));
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            failure = std::current_exception();
            inboundReady.notify_all();
            acked.notify_all();
        }
    }
};

// ---- Handshake ----------------------------------------------------------------

namespace {

// The first data datagram of a connection, carrying one message: what a
// refused peer gets instead of a session (it opens it as message 1).
Bytes firstMessage(const transport::TrafficKeys& keys, const Message& message) {
    Bytes frame{messageFrame, controlStream};
    appendBigEndian(frame, 1, 4);
    const Bytes body = encode(message);
    frame.insert(frame.end(), body.begin(), body.end());
    Bytes datagram{dataKind};
    appendBigEndian(datagram, keys.sendId, 4);
    appendBigEndian(datagram, 1, 8);
    const auto sealed = keys.send->seal(1, datagram, frame);
    datagram.insert(datagram.end(), sealed.begin(), sealed.end());
    return datagram;
}

// Handshake failures, for the host (rejected_connections, and its log): at
// most maxReports per summaryInterval, the rest summed up in one warning, so
// that a flood of forged hellos neither fills the host's log nor costs it a
// call per datagram. Version notices share the budget.
class Rejections {
public:
    explicit Rejections(const UdpConnection::Rejected& report) : report_(report) {}

    void add(const Endpoint& from, const Failure& failure, Clock::time_point now) {
        roll(now);
        last = failure.what();
        if (reported_ < maxReports) {
            ++reported_;
            if (report_) {
                report_(Failure(failure.result(), from.toString() + ": " + failure.what()));
            }
        } else {
            ++suppressed_;
            latest_ = from.toString() + ": " + failure.what();
        }
    }
    bool mayAnswer(Clock::time_point now) {
        roll(now);
        return answered_++ < maxReports;
    }
    void roll(Clock::time_point now) {
        if (now - started_ < summaryInterval) {
            return;
        }
        if (suppressed_) {
            log::warning("network", std::to_string(suppressed_) + " more handshakes failed in the last " +
                                        std::to_string(summaryInterval.count()) + " s (the last " + latest_ + ")");
        }
        started_ = now;
        reported_ = answered_ = 0;
        suppressed_ = 0;
    }

    std::optional<std::string> last; // The most recent reason.

private:
    const UdpConnection::Rejected& report_;
    Clock::time_point started_ = Clock::now();
    int reported_ = 0, answered_ = 0;
    uint64_t suppressed_ = 0;
    std::string latest_;
};

// A server's answers to data under a connection id it does not know (0xD7):
// at most maxReports per summaryInterval, a budget of their own that a flood
// of other datagrams cannot use up, and smaller than what they answer, so
// they reflect nothing to a forged source.
class UnknownAnswers {
public:
    void answer(std::span<const uint8_t> datagram, const std::function<void(std::span<const uint8_t>)>& reply,
                Clock::time_point now) {
        if (datagram.size() < dataHeader + crypto::Aead::tagSize || datagram[0] != dataKind) {
            return;
        }
        if (now - started_ >= summaryInterval) {
            started_ = now;
            answered_ = 0;
        }
        if (answered_++ < maxReports) {
            Bytes answer(datagram.begin(), datagram.begin() + unknownSize); // The id it came under.
            answer[0] = unknownKind;
            reply(answer);
        }
    }

private:
    Clock::time_point started_ = Clock::now();
    int answered_ = 0;
};

} // namespace

Bytes helloDatagram(std::span<const uint8_t> hello, std::span<const uint8_t> cookie) {
    Bytes datagram = framed(clientHelloKind, hello);
    datagram.insert(datagram.end(), cookie.begin(), cookie.end());
    datagram.resize(1 + helloBody, 0);
    return datagram;
}

// The server's side of the handshakes on one socket, side by side: each
// sender's attempt, the Retry cookie, and the failures for the host.
class ServerHandshakes {
public:
    ServerHandshakes(transport::CredentialsSource credentials, const UdpConnection::Rejected& rejected,
                     std::chrono::milliseconds timeout)
        : credentials_(std::move(credentials)), rejections_(rejected), timeout_(timeout) {
        crypto::random(cookieKey_);
    }

    // Drops the attempts that did not finish within the timeout (reported).
    void expire(Clock::time_point now) {
        std::erase_if(attempts_, [&](const Attempt& a) {
            if (now - a.started <= timeout_) {
                return false;
            }
            rejections_.add(a.from, a.failure ? *a.failure
                                              : Failure(RCS_ERROR_NETWORK,
                                                        "No finished message within " + secondsText(timeout_) +
                                                            " of the hello (the peer holds another key or password, "
                                                            "or left)"),
                            now);
            return true;
        });
        rejections_.roll(now);
    }

    // One datagram's payload from `from`; `reply` answers that sender. The
    // keys when it completed a handshake; failures go to the host.
    std::optional<transport::TrafficKeys> handle(std::span<const uint8_t> payload, const Endpoint& from,
                                                 const std::function<void(std::span<const uint8_t>)>& reply,
                                                 Clock::time_point now) {
        const uint8_t kind = payload[0];
        const auto body = payload.subspan(1);
        const auto attempt =
            std::find_if(attempts_.begin(), attempts_.end(), [&](const Attempt& a) { return a.from == from; });
        try {
            if (kind == clientHelloKind) {
                hello(body, from, attempt, reply, now);
            } else if (kind == finishedKind && attempt != attempts_.end()) {
                transport::TrafficKeys agreed;
                try {
                    attempt->handshake->finish(body, agreed);
                } catch (const transport::UntrustedPeer& refused) {
                    // The real client (its proof verified), which this end does not
                    // trust: tell it why, so it does not take the silence for a broken
                    // path; a few copies, as nothing acknowledges them.
                    const auto notice = firstMessage(agreed, Refused{RCS_ERROR_AUTHENTICATION, refused.what()});
                    for (int copy = 0; copy < 3; ++copy) {
                        reply(notice);
                    }
                    attempts_.erase(attempt);
                    throw;
                } catch (const Failure& failure) {
                    // Anyone can send a finished message that fails: the client's
                    // may still come, until the attempt expires.
                    attempt->failure = failure;
                    return std::nullopt;
                }
                attempts_.erase(attempt);
                return agreed;
            }
        } catch (const Failure& failure) {
            rejections_.add(from, failure, now);
        }
        return std::nullopt;
    }

    // The connection the attempt from `from` came over closed (a TCP
    // connection to a sharer's port): it is dropped, and reported (its own
    // failure, where a finished message failed).
    void closed(const transport::PeerClosed& why, Clock::time_point now) {
        const auto attempt = std::find_if(attempts_.begin(), attempts_.end(),
                                          [&](const Attempt& a) { return a.from == why.peer(); });
        if (attempt == attempts_.end()) {
            return;
        }
        rejections_.add(why.peer(), attempt->failure ? *attempt->failure : static_cast<const Failure&>(why), now);
        attempts_.erase(attempt);
    }

    const std::optional<std::string>& lastFailure() const noexcept { return rejections_.last; }
    // Whether a handshake from `from` is under way: its data may come ahead of its connection.
    bool pending(const Endpoint& from) const {
        return std::any_of(attempts_.begin(), attempts_.end(), [&](const Attempt& a) { return a.from == from; });
    }
    UnknownAnswers unknown;

private:
    struct Attempt {
        Endpoint from;
        std::shared_ptr<const transport::Credentials> credentials; // The handshake's.
        std::unique_ptr<transport::ServerHandshake> handshake;
        Bytes hello;
        Clock::time_point started;
        std::optional<Failure> failure; // A finished message that did not verify.
    };

    // While maxAttempts handshakes are pending, or one from the same address
    // is, a newcomer first proves it receives at its address: Retry hands it
    // a cookie only this server can make, for that address and a 10 s period.
    // With it, it takes the place of its own earlier attempt or the oldest;
    // without it, a hello costs a MAC and leaves no state, so hellos from
    // forged addresses cannot keep a viewer out or replace its handshake, nor
    // cost more than that. A PIN hello always proves its address first: its
    // answer takes one of the PIN's attempts, which hellos from forged
    // addresses must not use up.
    Bytes cookieFor(const Endpoint& from, int64_t period) const {
        Bytes text = from.addressBytes();
        appendBigEndian(text, from.port(), 2);
        appendBigEndian(text, uint64_t(period), 8);
        const auto mac = crypto::hmacSha256(cookieKey_, text);
        return Bytes(mac.begin(), mac.begin() + cookieSize);
    }

    void hello(std::span<const uint8_t> body, const Endpoint& from, std::vector<Attempt>::iterator attempt,
               const std::function<void(std::span<const uint8_t>)>& reply, Clock::time_point now) {
        // Another version learns this end's, so its user learns what to
        // update; the notice is no larger than the hello.
        if (const auto version = transport::otherProtocolVersion(body)) {
            if (rejections_.mayAnswer(now)) {
                reply(framed(versionKind, transport::versionNotice(body)));
            }
            transport::versionMismatch(*version);
        }
        if (body.size() != helloBody) {
            fail(RCS_ERROR_AUTHENTICATION, "Malformed handshake from the peer");
        }
        const auto hello = body.first(transport::clientHelloSize);
        if (attempt != attempts_.end() &&
            std::equal(hello.begin(), hello.end(), attempt->hello.begin(), attempt->hello.end())) {
            reply(framed(serverHelloKind, attempt->handshake->answer()));
            return; // A repeated hello: the same answer, as large as the hello.
        }
        if (attempts_.size() >= maxAttempts || attempt != attempts_.end() ||
            hello[transport::protocolMagicSize] == RCS_SECRET_PIN) {
            const int64_t period =
                std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() / 10;
            const auto cookie = body.subspan(transport::clientHelloSize, cookieSize);
            if (!crypto::equal(cookie, cookieFor(from, period)) && !crypto::equal(cookie, cookieFor(from, period - 1))) {
                reply(framed(retryKind, cookieFor(from, period)));
                return;
            }
        }
        // Checked before it costs a key pair.
        auto credentials = credentials_();
        std::unique_ptr<transport::ServerHandshake> handshake;
        try {
            handshake = std::make_unique<transport::ServerHandshake>(*credentials, hello);
        } catch (const transport::PinLocked&) {
            if (rejections_.mayAnswer(now)) {
                reply(framed(pinKind, transport::pinLockedNotice()));
            }
            throw;
        }
        if (attempt != attempts_.end()) { // The client started over.
            attempts_.erase(attempt);
        } else if (attempts_.size() >= maxAttempts) {
            attempts_.erase(attempts_.begin());
        }
        reply(framed(serverHelloKind, handshake->answer()));
        attempts_.push_back(
            {from, std::move(credentials), std::move(handshake), Bytes(hello.begin(), hello.end()), now, {}});
    }

    const transport::CredentialsSource credentials_;
    Rejections rejections_;
    const std::chrono::milliseconds timeout_;
    crypto::Key cookieKey_{};
    std::vector<Attempt> attempts_; // Oldest first.
};

namespace {

// A connection the handshake made: its thread starts here.
std::unique_ptr<UdpConnection::Impl> startConnection(std::unique_ptr<DatagramSocket> socket, std::unique_ptr<Link> link,
                                                     transport::TrafficKeys keys, CancelToken cancelled,
                                                     const UdpOptions& options, UdpConnection::Side side) {
    log::info("network", "Secure session over " + link->describe());
    const auto peerKey = keys.peerKey;
    auto impl = std::make_unique<UdpConnection::Impl>(std::move(socket), std::move(link), std::move(keys), cancelled,
                                                      options, side);
    impl->peerKey = peerKey;
    if (const auto& rate = options.rate) {
        std::lock_guard lock(impl->rateMutex);
        impl->rate.emplace(rate->minimum, rate->maximum, rate->moveTo, rate->rules);
        impl->fixedRate = !rate->adaptive;
        impl->setPacing(rate->rules);
        impl->setParity(rate->fec);
    }
    return impl;
}

} // namespace

std::unique_ptr<UdpConnection> UdpConnection::establish(Side side, std::unique_ptr<DatagramSocket> socket,
                                                        std::unique_ptr<Link> link,
                                                        const transport::CredentialsSource& credentials,
                                                        const UdpOptions& options, CancelToken cancelled,
                                                        const Rejected& rejected) {
    transport::Waiter waiter;
    waiter.watch(*socket);
    Bytes buffer(options.maxReceived);
    transport::TrafficKeys keys;
    Bytes finishedDatagram;

    if (side == Side::client) {
        const auto own = credentials();
        transport::ClientHandshake client(*own);
        Bytes hello = helloDatagram(client.hello());
        const auto deadline = Clock::now() + options.handshakeTimeout;
        auto nextSend = Clock::now();
        // The first answer that failed (another key, an untrusted device key,
        // another protocol version): it stands unless a valid one follows.
        std::exception_ptr refused;
        Clock::time_point refusedAt;
        const auto failed = [&](Clock::time_point now) {
            keys = {};
            if (!refused) {
                refused = std::current_exception();
                refusedAt = now;
            }
        };
        // The newest answer that costs PBKDF2 and waits for derivationGap.
        std::optional<Bytes> costly;
        std::optional<Clock::time_point> derivedAt;
        const auto answer = [&](std::span<const uint8_t> body, Clock::time_point now) {
            if (client.costly(body)) {
                if (derivedAt && now - *derivedAt < derivationGap) {
                    costly.emplace(body.begin(), body.end());
                    return;
                }
                derivedAt = now;
            }
            finishedDatagram = framed(finishedKind, client.finish(body, keys));
            link->send(*socket, finishedDatagram);
        };
        while (!keys.send) {
            throwIfCancelled(cancelled);
            const auto now = Clock::now();
            if (refused && ((!costly && now - refusedAt >= refusalGrace) || now >= deadline)) {
                std::rethrow_exception(refused);
            }
            if (now >= deadline) {
                fail(RCS_ERROR_NETWORK, "No answer within " + secondsText(options.handshakeTimeout) + " over " +
                                            link->describe() + " (" + link->silence() + ")");
            }
            if (now >= nextSend) {
                link->send(*socket, hello);
                nextSend = now + handshakeInterval;
            }
            link->tick(*socket, now);
            if (costly && now - *derivedAt >= derivationGap) {
                const Bytes body = std::move(*costly);
                costly.reset();
                try {
                    answer(body, now);
                } catch (const Failure&) {
                    failed(now);
                }
                if (keys.send) {
                    break;
                }
            }
            Endpoint from;
            while (const auto size = socket->receive(buffer, from)) {
                const auto payload = link->unwrap(*socket, from, std::span(buffer).first(*size));
                if (!payload || payload->empty() || (link->peer() && from != *link->peer())) {
                    continue;
                }
                const uint8_t kind = (*payload)[0];
                const auto body = payload->subspan(1);
                try {
                    if (kind == serverHelloKind && body.size() == transport::serverHelloSize) {
                        answer(body, now);
                        if (keys.send) {
                            break;
                        }
                        continue;
                    }
                    if (kind == versionKind && body.size() == transport::protocolMagicSize) {
                        if (const auto version = transport::otherProtocolVersion(body)) {
                            transport::versionMismatch(*version);
                        }
                    }
                    if (kind == pinKind) {
                        transport::checkPinLockedNotice(body);
                    }
                } catch (const Failure&) {
                    if (client.spent()) {
                        throw; // The PIN's one test failed: no other answer is tried.
                    }
                    failed(now);
                }
                if (kind == retryKind && body.size() == cookieSize) {
                    // The server asks this end to prove its address: the same hello again, with the cookie.
                    hello = helloDatagram(client.hello(), body);
                    nextSend = Clock::now();
                }
            }
            if (!keys.send) {
                auto until = std::min(nextSend, Clock::now() + 20ms);
                if (costly) {
                    until = std::min(until, *derivedAt + derivationGap);
                }
                waiter.wait(until);
            }
        }
    } else {
        ServerHandshakes handshakes(credentials, rejected, options.handshakeTimeout);
        Endpoint peer;
        const auto deadline = Clock::now() + 2 * options.handshakeTimeout;
        while (!keys.send) {
            throwIfCancelled(cancelled);
            const auto now = Clock::now();
            if (link && !link->awaitsPeer() && now >= deadline) {
                const auto& last = handshakes.lastFailure();
                fail(RCS_ERROR_NETWORK, "The peer did not start the secure session over " + link->describe() +
                                            (last ? " (last: " + *last + ")" : ""));
            }
            if (link) {
                link->tick(*socket, now);
            }
            handshakes.expire(now);
            Endpoint from;
            while (const auto size = socket->receive(buffer, from)) {
                std::optional<std::span<const uint8_t>> payload;
                if (link) {
                    payload = link->unwrap(*socket, from, std::span(buffer).first(*size));
                } else {
                    payload = std::span<const uint8_t>(buffer).first(*size);
                }
                if (!payload || payload->empty() || (link && link->peer() && from != *link->peer())) {
                    continue;
                }
                const auto reply = [&](std::span<const uint8_t> datagram) {
                    link ? (void)link->send(*socket, datagram) : (void)socket->send(from, datagram);
                };
                if (link && link->awaitsPeer() && !handshakes.pending(from)) {
                    // The host's channel: a connection of a viewer this end let go may still send.
                    handshakes.unknown.answer(*payload, reply, now);
                }
                if (auto agreed = handshakes.handle(*payload, from, reply, now)) {
                    keys = std::move(*agreed);
                    peer = from;
                    break;
                }
            }
            if (!keys.send) {
                waiter.wait(Clock::now() + 20ms);
            }
        }
        if (!link) {
            link = directLink(peer);
        }
    }

    auto impl = startConnection(std::move(socket), std::move(link), std::move(keys), cancelled, options, side);
    if (side == Side::client) {
        impl->finishedDatagram = std::move(finishedDatagram);
        impl->confirmed = false;
        impl->nextFinished = Clock::now() + handshakeInterval;
    }
    auto* raw = impl.get();
    raw->thread = log::thread([raw] { raw->run(); });
    return std::unique_ptr<UdpConnection>(new UdpConnection(std::move(impl)));
}

UdpConnection::UdpConnection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

UdpAcceptor::UdpAcceptor(Demux& demux, transport::CredentialsSource credentials, UdpConnection::Rejected rejected,
                         std::chrono::milliseconds handshakeTimeout, LinkFor linkFor)
    : demux_(demux), socket_(demux.handshakes()), rejected_(std::move(rejected)), linkFor_(std::move(linkFor)),
      handshakes_(std::make_unique<ServerHandshakes>(std::move(credentials), rejected_, handshakeTimeout)) {
    waiter_.watch(*socket_);
}

UdpAcceptor::~UdpAcceptor() = default;

std::unique_ptr<UdpConnection> UdpAcceptor::accept(const std::function<UdpOptions()>& options, CancelToken cancelled,
                                                   Endpoint* peer) {
    Bytes buffer(64 * 1024); // The largest a TCP port carries: data under an id it does not know is answered too.
    for (;;) {
        throwIfCancelled(cancelled);
        const auto now = Clock::now();
        handshakes_->expire(now);
        Endpoint from;
        for (;;) {
            std::optional<size_t> size;
            try {
                size = socket_->receive(buffer, from);
            } catch (const transport::PeerClosed& closed) {
                handshakes_->closed(closed, now);
                continue;
            }
            if (!size) {
                break;
            }
            if (!*size) {
                continue;
            }
            const auto reply = [&](std::span<const uint8_t> datagram) {
                socket_->send(from, datagram);
            };
            if (buffer[0] == dataKind && !handshakes_->pending(from)) {
                // Data under an id no connection has: the sharer restarted, or let it go.
                handshakes_->unknown.answer(std::span(buffer).first(*size), reply, now);
                continue;
            }
            if (auto keys = handshakes_->handle(std::span(buffer).first(*size), from, reply, now)) {
                try {
                    const uint32_t id = keys->receiveId;
                    auto impl = startConnection(demux_.connection(id, from), linkFor_(from), std::move(*keys), cancelled,
                                                options(), UdpConnection::Side::server);
                    auto* raw = impl.get();
                    raw->thread = log::thread([raw] { raw->run(); });
                    if (peer) {
                        *peer = from;
                    }
                    return std::unique_ptr<UdpConnection>(new UdpConnection(std::move(impl)));
                } catch (const std::exception& error) { // No descriptor or thread for it: it is dropped.
                    if (rejected_) {
                        rejected_(Failure(RCS_ERROR_BUSY, std::string("No room for the connection: ") + error.what()));
                    }
                }
            }
        }
        waiter_.wait(Clock::now() + 20ms);
    }
}

UdpConnection::~UdpConnection() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->waiter.wake();
    impl_->thread.join();
    // Tell the peer at once instead of letting it time out; best effort. Not
    // over a lost path: a goodbye that came once it is back (a TCP connection
    // that outlived it) would end a connection the peer may come back to.
    if (impl_->state != Impl::PathState::lost) {
        try {
            // What was queued just before and not sent yet goes first (an
            // offer that tells the peer why this end failed, a notice before
            // leaving, the goodbyes of connections that run through this one:
            // a forwarder's pipes).
            std::vector<Bytes> unsent;
            std::deque<Immediate> immediate;
            {
                std::lock_guard lock(impl_->mutex);
                for (const auto& [id, entry] : impl_->outgoing[controlStream].unacked) {
                    if (entry.attempts == 0) {
                        unsent.push_back(entry.frame);
                    }
                }
                immediate = std::exchange(impl_->immediate, {});
            }
            for (const auto& frame : unsent) {
                impl_->sendFrame(frame);
            }
            impl_->sendImmediate(immediate);
            const Bytes close{closeFrame, uint8_t(impl_->leaving.load())};
            impl_->sendFrame(close);
            impl_->sendFrame(close);
            impl_->flush();
        } catch (...) {
        }
    }
    for (const auto& route : impl_->routes) {
        route->link->close(*route->socket);
    }
}

void UdpConnection::leaving(Goodbye::Reason reason) noexcept {
    impl_->leaving = reason;
}

void UdpConnection::cancel() noexcept {
    impl_->cancelled = true;
    std::lock_guard lock(impl_->mutex);
    impl_->inboundReady.notify_all();
    impl_->acked.notify_all();
}

void UdpConnection::sendVideo(uint32_t stream, std::span<const uint8_t> body) {
    auto& impl = *impl_;
    if (stream >= streams) {
        fail(RCS_ERROR_INTERNAL, "Video of stream " + std::to_string(stream) + ", beyond RCS_MAX_STREAMS");
    }
    if (body.size() > transport::maxRecord) {
        fail(RCS_ERROR_INTERNAL,
             "A video packet of " + std::to_string(body.size()) + " bytes is beyond the " +
                 std::to_string(transport::maxRecord >> 20) + " MiB a message carries");
    }
    if (impl.silent) {
        std::lock_guard lock(impl.mutex);
        if (impl.failure) {
            std::rethrow_exception(impl.failure);
        }
        if (!std::exchange(impl.skipped[stream], true)) {
            ++impl.nextFrameId[stream];
        }
        return;
    }
    // The fragments, and each group's parity after it (computed here, out of
    // the lock); the frame's number goes in under it.
    const uint32_t percent = impl.fecPercent;
    const size_t fragmentSize = impl.maxFragment - (percent ? parityExtra : 0);
    const size_t count = (body.size() + fragmentSize - 1) / fragmentSize;
    const auto piece = [&](size_t index) {
        const size_t offset = index * fragmentSize;
        return body.subspan(offset, std::min(fragmentSize, body.size() - offset));
    };
    std::vector<Bytes> fragments(count);
    std::vector<uint16_t> order; // Fragments in sending order: each group's video, then its parity.
    order.reserve(count);
    for (size_t first = 0; first < count; first += fecGroup) {
        const size_t k = std::min(fecGroup, count - first);
        const size_t parity =
            percent ? std::min(std::max<size_t>(1, (k * percent + 99) / 100), fec::maxBlocks - k) : 0;
        for (size_t index = first; index < first + k; ++index) {
            const auto bytes = piece(index);
            Bytes& fragment = fragments[index];
            fragment.reserve(videoHeader + bytes.size());
            fragment.push_back(videoFrame);
            fragment.push_back(uint8_t(stream));
            appendBigEndian(fragment, 0, 4); // The frame's number.
            appendBigEndian(fragment, index, 2);
            appendBigEndian(fragment, count, 2);
            appendBigEndian(fragment, 0, 4); // Send time, stamped when it leaves.
            fragment.push_back(uint8_t(parity));
            fragment.insert(fragment.end(), bytes.begin(), bytes.end());
            order.push_back(uint16_t(index));
        }
        if (!parity) {
            continue;
        }
        std::vector<std::span<const uint8_t>> group;
        for (size_t index = first; index < first + k; ++index) {
            group.push_back(piece(index));
        }
        const size_t size = group.front().size(), last = group.back().size();
        auto blocks = fec::encode(group, parity, size);
        for (size_t j = 0; j < parity; ++j) {
            Bytes fragment;
            fragment.reserve(parityHeader + size);
            fragment.push_back(parityFrame);
            fragment.push_back(uint8_t(stream));
            appendBigEndian(fragment, 0, 4);
            appendBigEndian(fragment, first / fecGroup, 2);
            appendBigEndian(fragment, count, 2);
            appendBigEndian(fragment, 0, 4);
            fragment.push_back(uint8_t(parity));
            fragment.push_back(uint8_t(j));
            appendBigEndian(fragment, last, 2);
            fragment.insert(fragment.end(), blocks[j].begin(), blocks[j].end());
            order.push_back(uint16_t(fragments.size()));
            fragments.push_back(std::move(fragment));
        }
    }
    {
        std::lock_guard lock(impl.mutex);
        if (impl.failure) {
            std::rethrow_exception(impl.failure);
        }
        const uint32_t id = impl.nextFrameId[stream]++;
        impl.skipped[stream] = false;
        SentFrame frame;
        frame.videoCount = uint16_t(count);
        frame.fragments.reserve(fragments.size());
        size_t frameBytes = 0;
        for (auto& fragment : fragments) {
            storeBigEndian(fragment, 2, id, 4);
            frameBytes += fragment.size();
            impl.enqueuedBytes += fragment.size();
            frame.fragments.push_back({std::move(fragment)});
        }
        for (const uint16_t index : order) {
            impl.video.push_back({uint8_t(stream), id, index});
        }
        frame.bytes = frameBytes;
        frame.queued = frame.fragments.size();
        impl.sentFrames[stream].emplace(id, std::move(frame));
        impl.videoBytes += frameBytes;
        impl.largestQueued = std::max(impl.largestQueued, frameBytes);
        if (impl.videoBytes > std::max(maxQueuedVideo, 2 * impl.largestQueued)) {
            // The path is far slower than the video: drop the backlog; the
            // receiver gives up on those pictures and asks for a keyframe.
            // What it asks for of them again is gone too, so that it cannot
            // come back ahead of the new pictures.
            log::warning("network", "Video backlog of " + std::to_string(impl.videoBytes / 1024) +
                                        " KiB dropped: the path cannot carry the configured bitrate");
            ++impl.backlogDrops;
            impl.backlogDroppedBytes += impl.videoBytes;
            for (const auto& queued : impl.video) {
                impl.sentFrames[queued.stream].erase(queued.frame);
            }
            impl.video.clear();
            impl.videoBytes = 0;
            impl.largestQueued = 0;
        }
        // All that is queued leaves within the frame spread from now, as it goes over the wire.
        if (const uint64_t spread = impl.frameSpreadNs) {
            const double wire = double(impl.videoBytes + impl.video.size() * (dataHeader + crypto::Aead::tagSize));
            impl.spreadBps = uint64_t(wire * 8e9 / double(spread));
        }
    }
    impl.waiter.wake();
}

void UdpConnection::discardVideo() {
    auto& impl = *impl_;
    std::lock_guard lock(impl.mutex);
    for (const auto& queued : impl.video) {
        impl.sentFrames[queued.stream].erase(queued.frame); // What is asked for of them again is gone too.
    }
    impl.video.clear();
    impl.videoBytes = 0;
    impl.largestQueued = 0;
}

void UdpConnection::send(const Message& message) {
    auto& impl = *impl_;
    Bytes body = encode(message);
    if (const auto* packet = std::get_if<EncodedPacket>(&message)) {
        sendVideo(packet->stream, body);
        return;
    }
    if (const auto* sealed = std::get_if<SealedVideo>(&message)) {
        sendVideo(sealed->stream, body);
        return;
    }
    if (std::holds_alternative<AudioPacket>(message) || std::holds_alternative<DataDatagram>(message) ||
        std::holds_alternative<Pipe>(message) || std::holds_alternative<SealedAudio>(message)) {
        Bytes frame{std::holds_alternative<AudioPacket>(message) ? audioFrame : datagramFrame};
        if (frame[0] == audioFrame) {
            frame.resize(audioHeader); // The send time, stamped as it leaves.
        }
        frame.insert(frame.end(), body.begin(), body.end());
        if (frame.size() > impl.maxFragment + videoHeader) {
            fail(RCS_ERROR_INTERNAL, "A datagram of " + std::to_string(body.size()) + " bytes is too large");
        }
        std::lock_guard lock(impl.mutex);
        if (impl.failure) {
            std::rethrow_exception(impl.failure);
        }
        impl.enqueuedImmediateBytes += frame.size() + dataHeader + crypto::Aead::tagSize;
        impl.immediate.push_back({std::move(frame), Clock::now()});
    } else {
        if (body.size() + messageHeader > impl.maxFragment + videoHeader) {
            fail(RCS_ERROR_INTERNAL, "A control message of " + std::to_string(body.size()) + " bytes is too large");
        }
        const uint8_t stream = std::holds_alternative<DataPart>(message) ? dataStream : controlStream;
        std::lock_guard lock(impl.mutex);
        if (impl.failure) {
            std::rethrow_exception(impl.failure);
        }
        auto& out = impl.outgoing[stream];
        Bytes frame{messageFrame, stream};
        appendBigEndian(frame, uint32_t(out.nextId), 4);
        frame.insert(frame.end(), body.begin(), body.end());
        out.add(std::move(frame));
    }
    impl.waiter.wake();
}

Message UdpConnection::receive() {
    auto& impl = *impl_;
    std::unique_lock lock(impl.mutex);
    for (;;) {
        if (!impl.inbound.empty()) {
            Message message = std::move(impl.inbound.front().message);
            impl.inboundBytes -= impl.inbound.front().bytes;
            impl.inbound.pop_front();
            const bool room = impl.wantsRoom && impl.inboundBytes <= impl.maxInbound / 2;
            impl.wantsRoom &= !room;
            lock.unlock();
            if (room) {
                impl.waiter.wake(); // What waits for room goes on.
            }
            return message;
        }
        if (impl.failure) {
            std::rethrow_exception(impl.failure);
        }
        throwIfCancelled(impl.runCancelled);
        throwIfCancelled(impl.cancelled);
        impl.inboundReady.wait_for(lock, 20ms);
    }
}

uint64_t UdpConnection::bytesTransferred() const noexcept {
    return impl_->bytes.load();
}
uint64_t UdpConnection::lastReceiveNs() const noexcept {
    return impl_->receiveNs.load();
}
uint64_t UdpConnection::lastDecryptNs() const noexcept {
    return impl_->decryptNs.load();
}
std::optional<crypto::PublicKey> UdpConnection::peerKey() const {
    return impl_->peerKey;
}

std::optional<Endpoint> UdpConnection::peerAddress() const {
    std::lock_guard lock(impl_->pathMutex);
    return impl_->current->address;
}

size_t UdpConnection::unackedBytes() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->outgoing[dataStream].bytes;
}

void UdpConnection::waitForUnackedBelow(size_t bytes, std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    impl_->acked.wait_for(lock, timeout, [&] {
        return impl_->outgoing[dataStream].bytes <= bytes || impl_->failure || impl_->cancelled;
    });
}

uint64_t UdpConnection::sendBacklogNs() const noexcept {
    return impl_->backlogNs();
}

bool UdpConnection::lossy() const noexcept {
    std::lock_guard lock(impl_->pathMutex);
    return !impl_->link().ordered();
}

Connection::PathStatus UdpConnection::status() const {
    PathStatus status;
    status.roundTripNs = impl_->roundTripNs;
    {
        std::lock_guard lock(impl_->pathMutex);
        status.path = impl_->link().describe();
        status.current = impl_->endOf(*impl_->current);
    }
    {
        std::lock_guard lock(impl_->pathMutex);
        status.standby = impl_->standbyEnd;
    }
    status.lost = impl_->pathLost;
    status.pathChanges = impl_->pathChanges;
    status.lastInterruptionNs = impl_->lastInterruptionNs;
    status.bytes = impl_->bytes;
    status.standbyBytes = impl_->standbyBytes;
    std::lock_guard lock(impl_->rateMutex);
    status.fecPercent = impl_->fecPercent;
    status.paritySent = impl_->paritySent;
    status.fecRecovered = impl_->fecRecovered;
    status.resendRecovered = impl_->resendRecovered;
    status.framesLost = impl_->framesLost;
    status.markedPackets = impl_->markedPackets;
    status.pathBps = impl_->pathBps;
    status.soundWaitUs = impl_->soundWait.most();
    status.soundQueueUs = impl_->soundQueue.most();
    status.stalls = impl_->stalls;
    status.backlogDrops = impl_->backlogDrops;
    status.backlogDroppedBytes = impl_->backlogDroppedBytes;
    if (impl_->rate) {
        status.receivedBps = impl_->rate->receivedBps();
        status.lossPermille = impl_->rate->lossPermille();
        status.queueDelayUs = impl_->rate->queueDelayUs();
        // The parity's share of the target is not the encoder's.
        status.targetBitrate = std::max(uint32_t(uint64_t(impl_->rate->target()) * 100 / (100 + status.fecPercent)),
                                        impl_->rate->minimum());
    }
    return status;
}

std::vector<PathEvent> UdpConnection::takePathEvents() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->pathEvents, {});
}

void UdpConnection::addRoute(std::unique_ptr<DatagramSocket> socket, std::unique_ptr<Link> link, rcs_path_cause cause,
                             bool signaled, bool standby) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->addedRoutes.push_back({std::move(socket), std::move(link), cause, signaled, standby});
    }
    impl_->waiter.wake();
}

void UdpConnection::dropStandby(rcs_path_cause cause, rcs_result result, std::string why) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->standbyDropped = Impl::StandbyDrop{cause, result, std::move(why)};
    }
    impl_->waiter.wake();
}

void UdpConnection::moveRoute(rcs_path_cause cause) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->moveRequest = cause;
    }
    impl_->waiter.wake();
}

void UdpConnection::leaveRoute(rcs_path_cause cause, std::string why) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->leaveRequest.emplace(cause, std::move(why));
    }
    impl_->waiter.wake();
}

void UdpConnection::peerForgot() {
    impl_->forgotten = true;
    impl_->waiter.wake();
}

const crypto::Key& UdpConnection::exporter() const noexcept {
    return impl_->exporter;
}

void UdpConnection::setMigrate(bool migrate) {
    impl_->migrate = migrate;
}

void UdpConnection::setVideoFrom(Side from) noexcept {
    impl_->sendsVideo = from == impl_->side;
    impl_->receivesVideo = from != impl_->side;
}

void UdpConnection::setVideoRate(const VideoRate& rate) {
    {
        std::lock_guard lock(impl_->rateMutex);
        if (!impl_->rate) {
            return;
        }
        impl_->fixedRate = !rate.adaptive;
        impl_->rate->setRules(rate.rules);
        impl_->rate->setLimits(rate.minimum, rate.maximum);
        if (rate.moveTo) {
            impl_->rate->moveTo(rate.moveTo);
        }
        impl_->setPacing(rate.rules);
        impl_->setParity(rate.fec);
    }
    impl_->waiter.wake();
}

#ifdef RCSDK_FUZZING
namespace {

// Sends nowhere; what comes is the peer's.
class NowhereLink final : public Link {
public:
    bool send(DatagramSocket&, std::span<const uint8_t>, uint8_t) override { return false; }
    std::optional<std::span<const uint8_t>> unwrap(DatagramSocket&, Endpoint&,
                                                   std::span<const uint8_t> datagram) override {
        return datagram;
    }
    size_t overhead() const noexcept override { return 0; }
    std::string describe() const override { return "nowhere"; }
    rcs_path_route route() const override { return RCS_ROUTE_HOST; }
};

std::atomic<bool> fuzzNever{false};

} // namespace

UdpFuzzEnd::UdpFuzzEnd(bool sendsVideo, transport::TrafficKeys keys, const UdpOptions& options)
    : impl_(std::make_unique<UdpConnection::Impl>(
          std::make_unique<transport::UdpSocket>(Endpoint::parse("127.0.0.1", 0)), std::make_unique<NowhereLink>(),
          std::move(keys), fuzzNever, options, sendsVideo ? transport::Side::server : transport::Side::client)) {}

UdpFuzzEnd::~UdpFuzzEnd() = default;

void UdpFuzzEnd::datagram(std::span<const uint8_t> datagram, Clock::time_point now) {
    impl_->handleDatagram(*impl_->routes.front(), Endpoint{}, datagram, now);
    impl_->flush();
}

void UdpFuzzEnd::tick(Clock::time_point now) {
    if (std::exchange(impl_->held, false)) {
        impl_->handOver(controlStream);
        impl_->handOver(dataStream);
    }
    impl_->requestMissing(now);
    impl_->deliverFrames(now);
    impl_->flush();
}

std::optional<Message> UdpFuzzEnd::take() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->inbound.empty()) {
        return std::nullopt;
    }
    Message message = std::move(impl_->inbound.front().message);
    impl_->inboundBytes -= impl_->inbound.front().bytes;
    impl_->inbound.pop_front();
    return message;
}
#endif

} // namespace rcsdk::protocol
