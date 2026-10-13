// The forwarder (server/forwarder, RCS_TRANSPORT_FORWARDER) and what the ends
// share with it: tokens (signed, checked, expired, of another key, room or
// forwarder), the stream key (sealed once, opened only by who holds the key,
// a step on at each keyframe, a newcomer from the step of what was kept for
// it, a generation on for each that left; changed pictures and sound and
// repeated counters refused), the hop's messages, and the forwarder over
// loopback, over UDP, TCP and WebSocket, against ends that break its rules:
// a token that does not hold, a session's hello, an end that says nothing,
// more ends, rooms or connections than it takes, a viewer that sends what
// only a sharer sends, a pipe and a sharer flooded beyond their budgets,
// openings and frames that break HTTP or RFC 6455, pings whose pongs are
// never read, connections that never carry a hop. What it keeps for viewers that come later, and the sound; a
// viewer far behind on its only encoding starts again at a keyframe it asks for.
#include "../server/forwarder/src/forwarder.hpp"
#include "../server/forwarder/src/token.hpp"
#include "media/nal.hpp"
#include "protocol/media_keys.hpp"
#include "protocol/messages.hpp"
#include "protocol/udp_connection.hpp"
#include "support/check.hpp"
#include "support/shaping_proxy.hpp"
#include "transport/framed_socket.hpp"
#include "transport/websocket.hpp"
#include "util/bytes.hpp"

#include <rcsfwd.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <set>
#include <thread>

using namespace rcsdk;
using namespace rcsdk::protocol;

namespace {

EncodedPacket picture(uint32_t stream, uint32_t encoding, bool keyframe, uint64_t sequence, size_t size = 400) {
    EncodedPacket p;
    p.codec = RCS_CODEC_H264;
    p.width = 640;
    p.height = 360;
    p.stream = stream;
    p.encoding = encoding;
    p.keyframe = keyframe;
    p.sequence = sequence;
    nal::appendUnit(p.config, std::vector<uint8_t>{0x67, 1, 2, 3});
    nal::appendUnit(p.config, std::vector<uint8_t>{0x68, 4});
    nal::appendUnit(p.data, std::vector<uint8_t>(size, uint8_t(sequence)));
    p.data[4] = keyframe ? 0x65 : 0x41;
    return p;
}

AudioPacket sound(uint32_t sequence) {
    return AudioPacket{sequence, 960, uint64_t(sequence) * 20'000'000, std::vector<uint8_t>(120, uint8_t(sequence)),
                       0,        {}};
}

template <class Condition>
bool waitUntil(Condition condition, std::chrono::milliseconds wait) {
    for (const auto until = Clock::now() + wait; Clock::now() < until; std::this_thread::sleep_for(20ms)) {
        if (condition()) {
            return true;
        }
    }
    return condition();
}

SealedVideo seal(MediaSealer& sealer, const EncodedPacket& p) {
    return sealer.seal(p, encode(p));
}

SealedAudio seal(MediaSealer& sealer, const AudioPacket& p, uint8_t number) {
    return sealer.seal(p, encode(p), number);
}

MediaOpener::Outcome open(MediaOpener& opener, const SealedVideo& sealed, Clock::time_point now = Clock::now()) {
    EncodedPacket out;
    return opener.open(sealed, now, out);
}

MediaOpener::Outcome open(MediaOpener& opener, const SealedAudio& sealed, Clock::time_point now = Clock::now()) {
    AudioPacket out;
    return opener.open(sealed, now, out);
}

std::array<uint8_t, 32> audienceOf(const std::array<uint8_t, RCSFWD_IDENTITY_KEY_SIZE>& identity) {
    const crypto::DeviceKey pair(identity);
    return crypto::sha256(pair.publicKey());
}

// ---- Tokens -----------------------------------------------------------------------------------

void tokens() {
    rcsfwd_secret one{1, {}}, two{2, {}};
    crypto::random(one.secret);
    crypto::random(two.secret);
    const std::vector<rcsfwd_secret> held{one, two};
    std::array<uint8_t, 32> audience{};
    crypto::random(audience);
    forwarder::Grant grant{"room", RCSFWD_ROLE_SUBSCRIBE, 1234, 2'000'000'000, 5'000'000, audience};
    const uint64_t now = 1'900'000'000;
    const auto token = forwarder::sign(one, grant);
    CHECK(token.rfind("rcsf1.", 0) == 0 && token.size() < RCS_FORWARDER_TOKEN_SIZE);
    const auto checked = std::get<forwarder::Grant>(forwarder::verify(token, held, audience, now));
    CHECK(checked.room == "room" && checked.role == RCSFWD_ROLE_SUBSCRIBE && checked.route == 1234 &&
          checked.expires == grant.expires && checked.maxBitrate == 5'000'000 && checked.audience == audience);
    // Either key the forwarder holds (the host's server moves to a new one).
    CHECK(
        std::holds_alternative<forwarder::Grant>(forwarder::verify(forwarder::sign(two, grant), held, audience, now)));
    // Two tokens of one grant differ (the nonce).
    CHECK(forwarder::sign(one, grant) != token);
    const auto refused = [&](std::string_view text, uint64_t at, const std::string& reason,
                             const std::array<uint8_t, 32>& forwarder = {}) {
        const auto result =
            forwarder::verify(text, held, forwarder == std::array<uint8_t, 32>{} ? audience : forwarder, at);
        const auto* why = std::get_if<std::string>(&result);
        return why && why->find(reason) != std::string::npos;
    };
    CHECK(refused(token, grant.expires, "expired"));
    // Another forwarder that holds the same secret takes none of it.
    auto elsewhere = audience;
    elsewhere[0] ^= 1;
    CHECK(refused(token, now, "another forwarder", elsewhere));
    rcsfwd_secret other{3, {}};
    crypto::random(other.secret);
    CHECK(refused(forwarder::sign(other, grant), now, "does not hold"));
    rcsfwd_secret impostor = one; // The right number, another secret.
    impostor.secret[0] ^= 1;
    CHECK(refused(forwarder::sign(impostor, grant), now, "signature"));
    // Any character changed, in the grant or the signature: refused.
    for (size_t i = 6; i < token.size(); i += 7) {
        auto changed = token;
        changed[i] = changed[i] == 'A' ? 'B' : 'A';
        CHECK(!std::holds_alternative<forwarder::Grant>(forwarder::verify(changed, held, audience, now)));
    }
    CHECK(refused("rcsf9." + token.substr(6), now, "Malformed")); // A format no version defines.
    CHECK(refused(token.substr(0, token.find('.', 6)), now, "Malformed"));
    CHECK(refused("rcsf1.!!!." + token.substr(token.find('.', 6) + 1), now, "Malformed"));
    CHECK(refused(token + "A", now, "Malformed"));
    CHECK(refused("", now, "Malformed"));
    // A grant no token holds: no room, too long a room, no such role, a NUL, no audience.
    CHECK(throws<Failure>([&] { forwarder::sign(one, {"", RCSFWD_ROLE_SUBSCRIBE, 0, 1, 0, audience}); }));
    CHECK(throws<Failure>(
        [&] { forwarder::sign(one, {std::string(65, 'r'), RCSFWD_ROLE_SUBSCRIBE, 0, 1, 0, audience}); }));
    CHECK(throws<Failure>([&] { forwarder::sign(one, {"room", 3, 0, 1, 0, audience}); }));
    CHECK(throws<Failure>(
        [&] { forwarder::sign(one, {std::string("ro\0m", 4), RCSFWD_ROLE_SUBSCRIBE, 0, 1, 0, audience}); }));
    CHECK(throws<Failure>([&] { forwarder::sign(one, {"room", RCSFWD_ROLE_SUBSCRIBE, 0, 1, 0, {}}); }));
    // The C interface signs the same, into a buffer it says is too small when it is.
    rcsfwd_grant c = RCSFWD_GRANT_INIT;
    std::strcpy(c.room, "room");
    c.expires = 2'000'000'000;
    char text[RCS_FORWARDER_TOKEN_SIZE];
    CHECK(rcsfwd_sign_token(&one, &c, text, sizeof(text)) == RCS_ERROR_INVALID_ARGUMENT); // No audience.
    std::copy(audience.begin(), audience.end(), c.audience);
    CHECK(rcsfwd_sign_token(&one, &c, text, sizeof(text)) == RCS_OK);
    CHECK(std::holds_alternative<forwarder::Grant>(forwarder::verify(text, held, audience, now)));
    CHECK(rcsfwd_sign_token(&one, &c, text, 20) == RCS_ERROR_BUFFER_TOO_SMALL);
}

// ---- The stream key ---------------------------------------------------------------------------

void streamKeys() {
    using Outcome = MediaOpener::Outcome;
    MediaSealer sealer;
    MediaOpener first, second, late;
    // Viewer 1 is let in: a step on (1). A keyframe steps on (2), which it derives itself.
    CHECK(first.add(sealer.join()));
    const auto stepOne = seal(sealer, picture(0, 0, false, 1));
    CHECK(stepOne.step == 1 && open(first, stepOne) == Outcome::opened);
    const auto keyframe = seal(sealer, picture(0, 0, true, 2));
    const auto after = seal(sealer, picture(0, 0, false, 3));
    CHECK(keyframe.step == 2 && after.step == 2);
    CHECK(open(first, keyframe) == Outcome::opened && first.opened().step == 2 &&
          open(first, after) == Outcome::opened);
    // Viewer 2 is let in from that keyframe's step (what a forwarder kept for it): it opens the keyframe and
    // what followed, nothing before it.
    const auto fromKept = sealer.join(2);
    CHECK(fromKept.generation == 1 && fromKept.step == 2 && sealer.current().step == 2);
    CHECK(second.add(fromKept));
    CHECK(open(second, keyframe) == Outcome::opened && open(second, after) == Outcome::opened);
    CHECK(open(second, stepOne) == Outcome::unkeyed);
    // A step it no longer holds: a step on instead (what was kept is not for that viewer).
    const auto onward = sealer.join(1'000'000);
    CHECK(onward.step == 3 && sealer.current().step == 3);
    // Each counter once: a picture that comes again is refused.
    CHECK(open(first, after) == Outcome::replayed);
    // A changed byte anywhere, in the picture or the header the forwarder reads: refused.
    auto changed = seal(sealer, picture(0, 0, false, 4));
    for (const size_t at : {size_t(0), changed.sealed.size() / 2, changed.sealed.size() - 1}) {
        auto copy = changed;
        copy.sealed[at] ^= 1;
        CHECK(open(first, copy) == Outcome::rejected);
    }
    for (const auto change : {+[](SealedVideo& v) { v.encoding = 1; }, +[](SealedVideo& v) { v.flags ^= 1; },
                              +[](SealedVideo& v) { v.temporal = 1; }, +[](SealedVideo& v) { ++v.counter; }}) {
        auto copy = changed;
        change(copy);
        CHECK(open(first, copy) != Outcome::opened);
    }
    CHECK(open(first, changed) == Outcome::opened); // Untouched, it opens.
    // One counter for every stream, encoding and the sound under a key: none sealed twice. Keyframes of
    // other streams step on; what other streams sealed under a step already gone past still opens.
    std::set<std::pair<uint32_t, uint64_t>> counters;
    std::vector<SealedVideo> pictures;
    for (uint32_t i = 0; i < 60; ++i) {
        pictures.push_back(seal(sealer, picture(i % 2, (i / 2) % RCS_MAX_ENCODINGS, i % 7 == 0, 10 + i)));
        const auto voice = seal(sealer, sound(i), 1);
        CHECK(counters.insert({pictures.back().step, pictures.back().counter}).second);
        CHECK(counters.insert({voice.step, voice.counter}).second);
        CHECK(open(second, voice) == Outcome::opened);
    }
    for (auto it = pictures.rbegin(); it != pictures.rend(); ++it) { // Newest first: later steps before earlier.
        CHECK(open(first, *it) == Outcome::opened);
    }
    CHECK(open(first, pictures[5]) == Outcome::replayed);
    // Sound: its header (which sound) is sealed too; a picture's seal is no sound's.
    auto voice = seal(sealer, sound(100), 2);
    auto renumbered = voice;
    renumbered.sound = 3;
    CHECK(open(first, renumbered) == Outcome::rejected);
    CHECK(open(first, voice) == Outcome::opened && open(first, voice) == Outcome::replayed);
    const auto aPicture = seal(sealer, picture(0, 0, false, 101));
    SealedAudio posing{aPicture.generation, aPicture.step, aPicture.counter, 1, aPicture.sealed};
    CHECK(open(first, posing) == Outcome::rejected);
    // Steps far behind the kept ones stay shut.
    MediaOpener behind;
    CHECK(behind.add(sealer.current()));
    const auto old = seal(sealer, picture(1, 0, false, 102));
    for (uint32_t i = 0; i < MediaOpener::keptSteps + 2; ++i) {
        CHECK(open(behind, seal(sealer, picture(0, 0, true, 103 + i))) == Outcome::opened);
    }
    CHECK(open(behind, old) == Outcome::unkeyed);
    // Viewer 2 leaves: a new generation for viewer 1 alone; once used, viewer 2 opens nothing more.
    const auto next = sealer.rekey();
    CHECK(next.generation == 2 && sealer.pending() && sealer.current().generation == 1);
    const auto beforeChange = seal(sealer, picture(0, 0, false, 200)); // Still the old one until advance().
    CHECK(beforeChange.generation == 1 && open(second, beforeChange) == Outcome::opened);
    CHECK(first.add(next));
    sealer.advance();
    const auto changedTo = seal(sealer, picture(0, 0, false, 201));
    CHECK(changedTo.generation == 2 && changedTo.step == 0 && !sealer.pending());
    CHECK(open(first, changedTo) == Outcome::opened);
    CHECK(open(second, changedTo) == Outcome::unkeyed);
    // The generation before stays 2 s after the new one opened something, for what was on its way.
    const auto t0 = Clock::now();
    CHECK(open(first, beforeChange, t0) == Outcome::opened);
    const auto straggler = seal(sealer, picture(0, 0, false, 202)); // generation 2
    CHECK(open(first, straggler, t0 + 3s) == Outcome::opened);
    auto stale = beforeChange;
    stale.counter += 1000; // Another picture of generation 1, long after.
    CHECK(open(first, stale, t0 + 3s) == Outcome::unkeyed);
    // A viewer let in while a generation waits gets the new one, a step on, whatever was kept.
    const auto pending = sealer.rekey();
    const auto joined = sealer.join(0);
    CHECK(joined.generation == pending.generation && joined.step == 1 && !sealer.pending());
    CHECK(late.add(joined));
    CHECK(open(late, seal(sealer, picture(0, 0, false, 300))) == Outcome::opened);
    CHECK(open(late, changedTo) == Outcome::unkeyed);
    // A header that names a step far on (made up on the way) moves nothing: the next real picture opens.
    auto distant = seal(sealer, picture(0, 0, false, 301));
    distant.step += 500;
    CHECK(open(late, distant) == Outcome::rejected);
    distant.step += MediaOpener::maxStepsAhead;
    CHECK(open(late, distant) == Outcome::unkeyed);
    CHECK(open(late, seal(sealer, picture(0, 0, false, 302))) == Outcome::opened);
    // Older keys are no news.
    CHECK(!late.add(joined) && !late.add(next));
    CHECK(late.newest().generation == joined.generation && late.newest().step == 1);
}

// ---- The hop's messages -----------------------------------------------------------------------

template <class M>
M roundTrip(const M& m) {
    return std::get<M>(decode(encode(m)));
}

void hopMessages() {
    CHECK(roundTrip(ForwarderHello{ForwarderHello::publish, "rcsf1.x.y"}) ==
          (ForwarderHello{ForwarderHello::publish, "rcsf1.x.y"}));
    CHECK(roundTrip(ForwarderWelcome{7, true, 3000}) == (ForwarderWelcome{7, true, 3000}));
    CHECK(roundTrip(Pipe{9, Bytes(1136, 3)}) == (Pipe{9, Bytes(1136, 3)}));
    CHECK(roundTrip(PeerJoined{4, 101}) == (PeerJoined{4, 101}));
    CHECK(roundTrip(PeerLeft{4}) == (PeerLeft{4}));
    CHECK(roundTrip(Subscribe{4, 3, 2, 1, 5, 6}) == (Subscribe{4, 3, 2, 1, 5, 6}));
    PathReport report;
    for (uint64_t i = 0; i < maxPathReportPeers; ++i) {
        report.peers.push_back({i, 1'000'000, 12, 3000, 20'000, 5000, 2, 77});
    }
    CHECK(roundTrip(report) == report);
    CHECK(encode(report).size() + 6 <= 1107); // One reliable message over a 1200-byte datagram.
    CHECK(roundTrip(NeedKeyframe{1, 2}) == (NeedKeyframe{1, 2}));
    CHECK(roundTrip(PeerMoved{4, 1}) == (PeerMoved{4, 1}));
    MediaSealer sealer;
    const auto sealed = seal(sealer, picture(1, 2, true, 5));
    CHECK(roundTrip(sealed) == sealed && sealed.flags == SealedVideo::keyframeFlag);
    auto loud = sound(7);
    loud.data.resize(maxAudioPacketBytes);
    const auto voice = seal(sealer, loud, 9);
    CHECK(roundTrip(voice) == voice && voice.sound == 9);
    CHECK(1 + encode(voice).size() <= 1200 - 13 - crypto::Aead::tagSize); // One datagram of the hop, however large.
    CHECK(roundTrip(MediaKey{3, 4, crypto::Key{1, 2, 3}}) == (MediaKey{3, 4, crypto::Key{1, 2, 3}}));
    CHECK(roundTrip(MediaKeyAck{3, 4}) == (MediaKeyAck{3, 4}));
    // What no version sends is refused.
    const auto refusedAfter = [](Bytes bytes, size_t at, uint8_t value) {
        bytes[at] = value;
        return throws<Failure>([&] { decode(bytes); });
    };
    CHECK(refusedAfter(encode(ForwarderHello{ForwarderHello::subscribe, "t"}), 1, 3));            // Role.
    CHECK(throws<Failure>([] { decode(encode(ForwarderHello{ForwarderHello::subscribe, ""})); })); // No token.
    CHECK(throws<Failure>([] { decode(encode(Pipe{0, {}})); }));
    CHECK(refusedAfter(encode(ForwarderWelcome{1, true}), 9, 2));
    CHECK(refusedAfter(encode(Subscribe{1, 1, 0}), 13, RCS_MAX_ENCODINGS));
    CHECK(refusedAfter(encode(NeedKeyframe{0, 0}), 1, RCS_MAX_STREAMS));
    CHECK(refusedAfter(encode(sealed), 1 + 16, RCS_MAX_STREAMS));       // Stream.
    CHECK(refusedAfter(encode(sealed), 1 + 19, 4));                     // Flags.
    CHECK(refusedAfter(encode(voice), 1 + 16, 0));                      // Sound 0 is none.
    CHECK(throws<Failure>([] { decode(encode(MediaKey{0, 0, {}})); })); // Generation 0 is none.
    auto empty = report;
    empty.peers.clear();
    CHECK(throws<Failure>([&] { decode(encode(empty)); }));
    auto many = report;
    many.peers.push_back({});
    CHECK(throws<Failure>([&] { decode(encode(many)); }));
    auto shortSeal = encode(sealed);
    shortSeal.resize(sealedHeaderSize + crypto::Aead::tagSize); // A tag and nothing it seals.
    CHECK(throws<Failure>([&] { decode(shortSeal); }));
    auto longVoice = encode(voice);
    longVoice.resize(longVoice.size() + 100);
    CHECK(throws<Failure>([&] { decode(longVoice); }));
}

// ---- The forwarder against ends that break its rules -----------------------------------------

// An end's hop, read on a thread of its own: what came, waited for with a timeout.
class End {
public:
    explicit End(std::unique_ptr<UdpConnection> hop) : hop_(std::move(hop)), thread_([this] { run(); }) {}
    ~End() {
        hop_->cancel();
        thread_.join();
    }
    UdpConnection& hop() { return *hop_; }
    // The next message other than a heartbeat within `wait`; none when it did not come or the hop ended.
    std::optional<Message> next(std::chrono::milliseconds wait) {
        std::unique_lock lock(mutex_);
        if (!arrived_.wait_for(lock, wait, [&] { return !inbound_.empty() || ended_; }) || inbound_.empty()) {
            return std::nullopt;
        }
        auto message = std::move(inbound_.front());
        inbound_.pop_front();
        return message;
    }
    // The next of type M within `wait`, skipping others (path reports).
    template <class M>
    std::optional<M> nextOf(std::chrono::milliseconds wait) {
        for (const auto until = Clock::now() + wait; Clock::now() < until;) {
            const auto got = next(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()));
            if (got && std::holds_alternative<M>(*got)) {
                return std::get<M>(*got);
            }
        }
        return std::nullopt;
    }
    bool ended() {
        std::lock_guard lock(mutex_);
        return ended_;
    }
    // Its hello, and the forwarder's first answer.
    Message answer(const ForwarderHello& hello) {
        hop_->send(hello);
        auto got = next(3s);
        CHECK(got.has_value());
        return *got;
    }

private:
    void run() {
        try {
            for (;;) {
                auto message = hop_->receive();
                std::lock_guard lock(mutex_);
                inbound_.push_back(std::move(message));
                arrived_.notify_all();
            }
        } catch (const Failure&) {
            std::lock_guard lock(mutex_);
            ended_ = true;
            arrived_.notify_all();
        }
    }

    std::unique_ptr<UdpConnection> hop_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<Message> inbound_;
    bool ended_ = false;
    std::thread thread_; // Last.
};

// Until `stream` is readable, or `most` passed.
void waitReadable(transport::ByteStream& stream, std::chrono::milliseconds most) {
    static const std::atomic<bool> never{false};
    try {
        transport::waitReady(stream.handle(), false, never, Clock::now() + most);
    } catch (const Failure&) {
    }
}

// A connection to an entry and a WebSocket's opening over it: what came after the answer.
Bytes openWebSocket(transport::ByteStream& stream, CancelToken cancelled) {
    const auto opening = transport::websocket::open("127.0.0.1", "/rcs");
    const auto request = std::span(reinterpret_cast<const uint8_t*>(opening.request.data()), opening.request.size());
    CHECK(stream.writeSome(request) == request.size());
    Bytes answer;
    uint8_t chunk[4096];
    for (const auto until = Clock::now() + 3s; Clock::now() < until;) {
        if (const size_t taken = stream.readSome(chunk)) {
            answer.insert(answer.end(), chunk, chunk + taken);
            if (const auto consumed = transport::websocket::opened(answer, opening.key)) {
                return Bytes(answer.begin() + ptrdiff_t(*consumed), answer.end());
            }
        } else {
            waitReadable(stream, 50ms);
        }
    }
    (void)cancelled;
    CHECK(false);
    return {};
}

struct Fixture {
    rcsfwd_secret secret{1, {}};
    std::array<uint8_t, RCSFWD_IDENTITY_KEY_SIZE> identity = crypto::DeviceKey::generate();
    std::array<uint8_t, 32> audience = audienceOf(identity);
    std::unique_ptr<forwarder::Forwarder> forwarder;
    std::atomic<bool> cancelled{false};

    explicit Fixture(forwarder::Settings s = {}) {
        crypto::random(secret.secret);
        const auto loopback = transport::Endpoint::parse("127.0.0.1", 0);
        s.listenUdp = loopback;
        s.listenTcp = loopback;
        s.listenWebSocket = loopback;
        s.identity = identity;
        s.secrets = {secret};
        forwarder = std::make_unique<forwarder::Forwarder>(std::move(s));
        forwarder->start();
    }
    std::string token(rcsfwd_role role, const std::string& room, int64_t expiresIn = 3600) const {
        return forwarder::sign(secret,
                               {room, role, 9, uint64_t(int64_t(forwarder::unixNow()) + expiresIn), 0, audience});
    }
    transport::Endpoint at(rcsfwd_entry entry) const {
        return transport::Endpoint::parse("127.0.0.1", forwarder->port(entry));
    }
    // A hop as an end makes one, its handshake done, over `entry` (UDP: by way of `via`, where given);
    // `forwarderMagic` false: a session's hello.
    std::unique_ptr<UdpConnection> hop(bool forwarderMagic = true, bool sendsVideo = false,
                                       rcsfwd_entry entry = RCSFWD_ENTRY_UDP,
                                       std::optional<transport::Endpoint> via = std::nullopt) {
        transport::Credentials credentials;
        credentials.secret = RCS_SECRET_NONE;
        credentials.forwarder = forwarderMagic;
        UdpOptions options;
        options.handshakeTimeout = 2s;
        if (sendsVideo) {
            options.videoFrom = transport::Side::client;
            options.rate = Connection::VideoRate{false, 20'000'000, 20'000'000, 20'000'000, {}, {}};
        }
        const auto to = via.value_or(at(entry));
        std::unique_ptr<transport::DatagramSocket> socket;
        if (entry == RCSFWD_ENTRY_UDP) {
            socket = std::make_unique<transport::UdpSocket>(transport::Endpoint::parse("127.0.0.1", 0));
        } else {
            auto stream = transport::tcpBytes(transport::connectStreamTcp(to, "the forwarder", cancelled, 2s, 0),
                                              "the forwarder");
            const auto arrived = entry == RCSFWD_ENTRY_WEBSOCKET ? openWebSocket(*stream, cancelled) : Bytes{};
            socket = std::make_unique<transport::FramedSocket>(
                std::move(stream), to,
                entry == RCSFWD_ENTRY_TCP ? transport::lengthFraming() : transport::webSocketFraming(), arrived);
        }
        return UdpConnection::establish(UdpConnection::Side::client, std::move(socket), directLink(to), credentials,
                                        options, cancelled);
    }
    std::unique_ptr<End> end(bool sendsVideo = false, rcsfwd_entry entry = RCSFWD_ENTRY_UDP,
                             std::optional<transport::Endpoint> via = std::nullopt) {
        return std::make_unique<End>(hop(true, sendsVideo, entry, via));
    }
    // A sharer in `room`, and a viewer there over `entry`, with its number.
    std::unique_ptr<End> sharer(const std::string& room) {
        auto made = end(true);
        CHECK(std::holds_alternative<ForwarderWelcome>(
            made->answer({ForwarderHello::publish, token(RCSFWD_ROLE_PUBLISH, room)})));
        return made;
    }
    std::pair<std::unique_ptr<End>, uint64_t> viewer(const std::string& room, rcsfwd_entry entry = RCSFWD_ENTRY_UDP,
                                                     std::optional<transport::Endpoint> via = std::nullopt) {
        auto made = end(false, entry, via);
        const auto answer = made->answer({ForwarderHello::subscribe, token(RCSFWD_ROLE_SUBSCRIBE, room)});
        CHECK(std::holds_alternative<ForwarderWelcome>(answer));
        return {std::move(made), std::get<ForwarderWelcome>(answer).peer};
    }
};

bool refusedWith(const Message& m, rcs_result result, const std::string& reason) {
    const auto* r = std::get_if<Refused>(&m);
    return r && r->result == result && r->reason.find(reason) != std::string::npos;
}

void refusals() {
    forwarder::Settings one;
    one.maxRooms = 1;
    Fixture f(one);
    CHECK(refusedWith(f.end()->answer({ForwarderHello::subscribe, "rcsf1.bad.token"}), RCS_ERROR_REFUSED, "Malformed"));
    CHECK(refusedWith(f.end()->answer({ForwarderHello::subscribe, f.token(RCSFWD_ROLE_SUBSCRIBE, "a", -1)}),
                      RCS_ERROR_REFUSED, "expired"));
    CHECK(refusedWith(f.end()->answer({ForwarderHello::publish, f.token(RCSFWD_ROLE_SUBSCRIBE, "a")}),
                      RCS_ERROR_REFUSED, "viewer's"));
    // A token of the same secret for another forwarder.
    auto elsewhere = f.audience;
    elsewhere[31] ^= 1;
    const auto stranger =
        forwarder::sign(f.secret, {"a", RCSFWD_ROLE_SUBSCRIBE, 9, forwarder::unixNow() + 60, 0, elsewhere});
    CHECK(refusedWith(f.end()->answer({ForwarderHello::subscribe, stranger}), RCS_ERROR_REFUSED, "another forwarder"));
    auto sharer = f.end(true);
    CHECK(std::holds_alternative<ForwarderWelcome>(
        sharer->answer({ForwarderHello::publish, f.token(RCSFWD_ROLE_PUBLISH, "a")})));
    CHECK(refusedWith(f.end(true)->answer({ForwarderHello::publish, f.token(RCSFWD_ROLE_PUBLISH, "a")}),
                      RCS_ERROR_REFUSED, "has its sharer"));
    CHECK(refusedWith(f.end()->answer({ForwarderHello::subscribe, f.token(RCSFWD_ROLE_SUBSCRIBE, "b")}), RCS_ERROR_BUSY,
                      "as many rooms"));
    CHECK(f.forwarder->stats().refused == 6);
    // A refused end's hop is closed after its refusal.
    {
        auto refused = f.end();
        refused->answer({ForwarderHello::subscribe, "rcsf1.x.y"});
        CHECK(waitUntil([&] { return refused->ended(); }, 3s));
    }
    // A session's hello (another magic) gets no handshake: the end times out, the forwarder counts it.
    const uint64_t rejected = f.forwarder->stats().rejected;
    CHECK(throws<Failure>([&] { f.hop(false); }));
    CHECK(waitUntil([&] { return f.forwarder->stats().rejected > rejected; }, 8s));
    // A viewer that sends what only a sharer sends is dropped (its hop closes), and the sharer hears it left.
    {
        auto viewer = f.end();
        CHECK(std::holds_alternative<ForwarderWelcome>(
            viewer->answer({ForwarderHello::subscribe, f.token(RCSFWD_ROLE_SUBSCRIBE, "a")})));
        CHECK(sharer->nextOf<PeerJoined>(2s));
        viewer->hop().send(Subscribe{1, 1, 0});
        CHECK(waitUntil([&] { return viewer->ended(); }, 3s));
        CHECK(sharer->nextOf<PeerLeft>(2s));
    }
}

// Ends that say nothing after their handshake take no room for long, and no
// more than 32 at once wait to say who they are.
void silentEnds() {
    Fixture f;
    std::vector<std::unique_ptr<UdpConnection>> silent;
    for (int i = 0; i < 40; ++i) {
        silent.push_back(f.hop());
    }
    std::this_thread::sleep_for(200ms);
    CHECK(f.forwarder->stats().ends <= forwarder::Forwarder::maxUnnamed);
    CHECK(f.forwarder->stats().refused >= 8);
    CHECK(waitUntil([&] { return f.forwarder->stats().ends == 0; }, forwarder::Forwarder::helloWait + 3s));
    CHECK(f.forwarder->stats().hello_timeouts >= 30);
    // Room again for an end that says who it is.
    CHECK(std::holds_alternative<ForwarderWelcome>(
        f.end()->answer({ForwarderHello::subscribe, f.token(RCSFWD_ROLE_SUBSCRIBE, "a")})));
}

// A full room; a pipe flooded far beyond its budget is cut to it, and the
// sharer gets no more than that; a sharer's pictures beyond its own budget
// likewise; pictures reach only viewers that subscribed, from a keyframe on.
void floodsAndPictures() {
    forwarder::Settings s;
    s.maxViewers = 2;
    s.maxPipeBps = 1'000'000;
    s.maxPublishBps = 1'000'000;
    Fixture f(s);
    auto sharer = f.sharer("a");
    std::vector<std::unique_ptr<End>> viewers;
    std::vector<uint64_t> peers;
    for (int i = 0; i < 2; ++i) {
        auto [viewer, peer] = f.viewer("a");
        viewers.push_back(std::move(viewer));
        peers.push_back(peer);
        CHECK(sharer->nextOf<PeerJoined>(2s)->peer == peer);
    }
    CHECK(refusedWith(f.end()->answer({ForwarderHello::subscribe, f.token(RCSFWD_ROLE_SUBSCRIBE, "a")}),
                      RCS_ERROR_BUSY, "full"));
    // 1 Mbit/s a pipe: 2 MB within a second are cut to a quarter second's burst and the second's budget.
    const Bytes datagram(1000, 7);
    const auto started = Clock::now();
    for (int i = 0; i < 2000; ++i) {
        viewers[0]->hop().send(Pipe{0, datagram});
        if (i % 100 == 99) {
            std::this_thread::sleep_for(50ms);
        }
    }
    size_t through = 0;
    for (auto m = sharer->next(500ms); m && Clock::now() - started < 3s; m = sharer->next(500ms)) {
        if (const auto* pipe = std::get_if<Pipe>(&*m)) {
            CHECK(pipe->peer == peers[0]);
            through += pipe->bytes.size();
        }
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const auto room = f.forwarder->roomStats("a");
    std::printf("flood: %zu of %d bytes through in %.1f s, %llu datagrams dropped\n", through, 2000 * 1000, seconds,
                (unsigned long long)room.pipe_dropped);
    CHECK(through <= 65536 + size_t(125'000 * seconds) + 2000 && room.pipe_dropped >= 1000);
    // Pictures: none before a subscription; from a keyframe on after it (here, with a key that opens none of
    // what was kept: the forwarder asks the sharer for a keyframe).
    MediaSealer sealer;
    sharer->hop().send(seal(sealer, picture(0, 0, true, 1)));
    CHECK(!viewers[1]->next(500ms));
    const auto current = sealer.current();
    sharer->hop().send(Subscribe{peers[1], 1, 0, 0, current.generation, current.step + 1});
    CHECK(sharer->nextOf<NeedKeyframe>(2s) == (NeedKeyframe{0, 0}));
    sharer->hop().send(seal(sealer, picture(0, 0, false, 2))); // Not a keyframe: it waits.
    sharer->hop().send(seal(sealer, picture(0, 0, true, 3)));
    const auto first = viewers[1]->next(2s);
    CHECK(first && std::get<SealedVideo>(*first).flags == SealedVideo::keyframeFlag);
    auto list = f.forwarder->viewers("a");
    CHECK(list.size() == 2 && list[1].skipped_frames == 1 && list[1].sent_frames == 1 && list[0].sent_frames == 0);
    // The sharer's budget, 1 Mbit/s: 2 MB of pictures within half a second are mostly dropped.
    for (int i = 0; i < 40; ++i) {
        sharer->hop().send(seal(sealer, picture(0, 0, false, 10 + i, 50'000)));
    }
    CHECK(waitUntil([&] { return f.forwarder->roomStats("a").publish_dropped >= 20; }, 5s));
    std::printf("flood: %llu pictures over the sharer's budget dropped\n",
                (unsigned long long)f.forwarder->roomStats("a").publish_dropped);
    // The room closes: every end hears the forwarder goes (as restarting).
    f.forwarder->closeRoom("a");
    CHECK(waitUntil([&] { return sharer->ended() && viewers[0]->ended() && viewers[1]->ended(); }, 3s));
    CHECK(throws<Failure>([&] { f.forwarder->roomStats("a"); }));
}

// What the forwarder keeps: a viewer whose key opens the newest keyframe
// starts on it and the pictures after it at once, without a keyframe asked
// of the sharer; one moved to another encoding likewise. Beyond the room's
// bytes, nothing is kept until the next keyframe. The sound reaches the
// viewers of its number only.
void keptAndSound() {
    forwarder::Settings s;
    s.cacheBytes = 1u << 20;
    Fixture f(s);
    auto sharer = f.sharer("k");
    MediaSealer sealer;
    const auto key = sealer.join();
    sharer->hop().send(seal(sealer, picture(0, 0, true, 1)));
    for (uint32_t i = 2; i <= 5; ++i) {
        sharer->hop().send(seal(sealer, picture(0, 0, false, i)));
    }
    sharer->hop().send(seal(sealer, picture(0, 1, true, 6))); // The lower encoding's keyframe, a step on.
    CHECK(waitUntil([&] { return f.forwarder->roomStats("k").media_frames == 6; }, 2s));
    auto [late, peer] = f.viewer("k");
    CHECK(sharer->nextOf<PeerJoined>(2s));
    // Its key is of the keyframe's step: the kept pictures, in order, at once.
    sharer->hop().send(Subscribe{peer, 1, 0, 1, key.generation, key.step + 1});
    for (uint64_t sequence = 1; sequence <= 5; ++sequence) {
        const auto got = late->nextOf<SealedVideo>(2s);
        CHECK(got && got->counter == sequence - 1 && got->encoding == 0);
    }
    CHECK(!sharer->nextOf<NeedKeyframe>(300ms));
    auto room = f.forwarder->roomStats("k");
    CHECK(room.cache_starts == 1 && room.keyframes_asked == 0 && room.cache_bytes > 0);
    // Moved to the lower encoding: its kept keyframe.
    sharer->hop().send(Subscribe{peer, 1, 1, 1, key.generation, key.step + 1});
    const auto lower = late->nextOf<SealedVideo>(2s);
    CHECK(lower && lower->encoding == 1 && (lower->flags & SealedVideo::keyframeFlag));
    CHECK(f.forwarder->roomStats("k").cache_starts == 2);
    // The sound of its number reaches it, another's does not.
    sharer->hop().send(seal(sealer, sound(1), 2));
    sharer->hop().send(seal(sealer, sound(2), 1));
    const auto heard = late->nextOf<SealedAudio>(2s);
    CHECK(heard && heard->sound == 1);
    CHECK(f.forwarder->roomStats("k").sound_packets == 2);
    // More than the room keeps: dropped until the next keyframe, and a newcomer then waits for one.
    sharer->hop().send(seal(sealer, picture(0, 0, true, 20, 200'000)));
    for (uint32_t i = 0; i < 8; ++i) {
        sharer->hop().send(seal(sealer, picture(0, 0, false, 21 + i, 200'000)));
    }
    CHECK(waitUntil([&] { return f.forwarder->roomStats("k").media_frames == 15; }, 3s));
    CHECK(f.forwarder->roomStats("k").cache_bytes < (1u << 20));
    auto [another, second] = f.viewer("k");
    sharer->hop().send(Subscribe{second, 1, 0, 0, key.generation, 0});
    CHECK(sharer->nextOf<NeedKeyframe>(2s));
    CHECK(f.forwarder->roomStats("k").keyframes_asked == 1);
    CHECK(f.forwarder->stats().cache_bytes == f.forwarder->roomStats("k").cache_bytes);
}

// A viewer far behind on the one encoding there is (its path carries 1
// Mbit/s; 12 Mbit/s come for it): what waited for it goes, and it starts
// again at a keyframe, which the forwarder asks the sharer for, as nothing
// after what went reaches the viewer to show it a gap; behind again at once,
// it is asked for no sooner than restartWait after the last.
void behindAlone() {
    Fixture f;
    auto sharer = f.sharer("b");
    test::ShapingProxy narrow(f.at(RCSFWD_ENTRY_UDP));
    narrow.bitsPerSecond = 1'000'000;
    auto [viewer, peer] = f.viewer("b", RCSFWD_ENTRY_UDP, narrow.address());
    CHECK(sharer->nextOf<PeerJoined>(2s));
    MediaSealer sealer;
    const auto key = sealer.join();
    sharer->hop().send(Subscribe{peer, 1, 0, 1, key.generation, key.step});
    CHECK(sharer->nextOf<NeedKeyframe>(2s)); // Nothing kept for it yet.
    sharer->hop().send(seal(sealer, picture(0, 0, true, 1)));
    CHECK(viewer->nextOf<SealedVideo>(3s));
    uint64_t sequence = 1;
    const auto fallBehind = [&] { // 12 Mbit/s of pictures until the forwarder asks for a keyframe.
        for (const auto until = Clock::now() + 8s; Clock::now() < until;) {
            sharer->hop().send(seal(sealer, picture(0, 0, false, ++sequence, 30'000)));
            if (const auto asked = sharer->nextOf<NeedKeyframe>(20ms)) {
                return *asked == NeedKeyframe{0, 0};
            }
        }
        return false;
    };
    CHECK(fallBehind());
    const auto first = Clock::now();
    // It waits for a keyframe (what comes before it is skipped), and starts on the one asked for.
    const auto waiting = f.forwarder->viewers("b").at(0);
    sharer->hop().send(seal(sealer, picture(0, 0, false, ++sequence)));
    sharer->hop().send(seal(sealer, picture(0, 0, true, ++sequence)));
    sharer->hop().send(seal(sealer, picture(0, 0, false, ++sequence)));
    CHECK(waitUntil([&] { return f.forwarder->viewers("b").at(0).sent_frames == waiting.sent_frames + 2; }, 3s));
    CHECK(f.forwarder->viewers("b").at(0).skipped_frames == waiting.skipped_frames + 1);
    CHECK(fallBehind());
    const double again = std::chrono::duration<double>(Clock::now() - first).count();
    std::printf("behind: started again on the keyframe asked; asked again %.1f s after\n", again);
    CHECK(again >= 1.5);
}

// Many rooms at once, each its own: pictures stay in their room.
void rooms() {
    Fixture f;
    std::vector<std::unique_ptr<End>> sharers, viewers;
    std::vector<MediaSealer> sealers(3);
    for (int r = 0; r < 3; ++r) {
        const std::string name = "room " + std::to_string(r);
        sharers.push_back(f.sharer(name));
        auto [viewer, peer] = f.viewer(name);
        viewers.push_back(std::move(viewer));
        CHECK(sharers[r]->nextOf<PeerJoined>(2s));
        sharers[r]->hop().send(Subscribe{peer, 1, 0, 0, 0, 0});
    }
    CHECK(f.forwarder->stats().rooms == 3);
    for (int r = 0; r < 3; ++r) {
        sharers[r]->hop().send(seal(sealers[r], picture(0, 0, true, uint64_t(100 * r))));
    }
    for (int r = 0; r < 3; ++r) {
        const auto got = viewers[r]->nextOf<SealedVideo>(2s);
        CHECK(got && got->generation == sealers[r].current().generation);
        CHECK(!viewers[r]->nextOf<SealedVideo>(200ms)); // One picture each: its own room's.
        CHECK(f.forwarder->roomStats("room " + std::to_string(r)).sent_frames == 1);
    }
}

// ---- TCP and WebSocket entries -----------------------------------------------------------------

// A raw TCP connection to an entry: what it sends, and what comes back until it closes.
struct Raw {
    std::atomic<bool> never{false};
    std::unique_ptr<transport::ByteStream> stream;
    explicit Raw(const transport::Endpoint& to)
        : stream(transport::tcpBytes(transport::connectStreamTcp(to, "the entry", never, 2s, 0), "the entry")) {}
    void send(std::span<const uint8_t> bytes) {
        while (!bytes.empty()) {
            bytes = bytes.subspan(stream->writeSome(bytes));
        }
    }
    void send(const std::string& text) { send(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())); }
    // What came within `wait`, and whether the entry closed the connection.
    std::pair<Bytes, bool> read(std::chrono::milliseconds wait) {
        Bytes got;
        uint8_t chunk[4096];
        for (const auto until = Clock::now() + wait; Clock::now() < until;) {
            try {
                if (const size_t taken = stream->readSome(chunk)) {
                    got.insert(got.end(), chunk, chunk + taken);
                    continue;
                }
            } catch (const Failure&) {
                return {got, true};
            }
            waitReadable(*stream, 20ms);
        }
        return {got, false};
    }
    std::string text(const Bytes& bytes) { return std::string(bytes.begin(), bytes.end()); }
};

// Hops over TCP and WebSocket: in the room as over UDP, pictures to them;
// and what each entry refuses, and closes.
void entries() {
    forwarder::Settings s;
    s.maxEnds = 12;
    Fixture f(s);
    auto sharer = f.sharer("e");
    MediaSealer sealer;
    std::vector<std::unique_ptr<End>> viewers;
    for (const auto entry : {RCSFWD_ENTRY_TCP, RCSFWD_ENTRY_WEBSOCKET}) {
        auto [viewer, peer] = f.viewer("e", entry);
        CHECK(sharer->nextOf<PeerJoined>(2s));
        sharer->hop().send(Subscribe{peer, 1, 0, 0, 0, 0});
        viewers.push_back(std::move(viewer));
    }
    std::this_thread::sleep_for(100ms);
    for (uint32_t i = 0; i < 30; ++i) { // Large ones too: many datagrams each.
        sharer->hop().send(seal(sealer, picture(0, 0, i == 0, i, i % 5 ? 400 : 60'000)));
    }
    for (auto& viewer : viewers) {
        for (uint32_t i = 0; i < 30; ++i) {
            const auto got = viewer->nextOf<SealedVideo>(3s);
            CHECK(got && got->counter == i);
        }
    }
    const auto list = f.forwarder->viewers("e");
    CHECK(list.size() == 2 && list[0].entry == RCSFWD_ENTRY_TCP && list[1].entry == RCSFWD_ENTRY_WEBSOCKET);
    CHECK(f.forwarder->stats().connections == 2);
    // An end whose connection closes leaves at once, over either entry.
    viewers.clear();
    CHECK(waitUntil([&] { return f.forwarder->roomStats("e").viewers == 0; }, 2s));
    CHECK(waitUntil([&] { return f.forwarder->stats().connections == 0; }, 2s));

    // The WebSocket entry's refusals: an answer that says why, and the connection closed.
    const auto ws = f.at(RCSFWD_ENTRY_WEBSOCKET);
    const auto answered = [&](const std::string& request) {
        Raw raw(ws);
        raw.send(request);
        auto [got, closed] = raw.read(2s);
        return closed ? raw.text(got) : std::string("(open)");
    };
    CHECK(answered("GET / HTTP/1.1\r\nHost: x\r\n\r\n").rfind("HTTP/1.1 426", 0) == 0);
    CHECK(answered("POST / HTTP/1.1\r\nHost: x\r\n\r\n").rfind("HTTP/1.1 405", 0) == 0);
    CHECK(answered("BREW coffee\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
    CHECK(answered(std::string(transport::websocket::maxHandshake, 'G')).rfind("HTTP/1.1 431", 0) == 0);
    // Frames that break RFC 6455 after a good opening: the close code that says why, then closed.
    const auto closedWith = [&](const Bytes& frame) {
        Raw raw(ws);
        raw.send(transport::websocket::open("x", "/").request);
        const auto opened = raw.read(300ms);
        CHECK(!opened.second && raw.text(opened.first).rfind("HTTP/1.1 101", 0) == 0);
        raw.send(frame);
        const auto [got, closed] = raw.read(2s);
        transport::websocket::Decoder server(false);
        server.push(got);
        const auto event = server.next();
        return closed && event && event->kind == transport::websocket::close && event->payload.size() >= 2
                   ? uint16_t(loadBigEndian(event->payload, 0, 2))
                   : uint16_t(0);
    };
    CHECK(closedWith(transport::websocket::frame(transport::websocket::binary, Bytes(8), false)) ==
          transport::websocket::protocolError); // Unmasked.
    CHECK(closedWith(transport::websocket::frame(transport::websocket::text, Bytes(8), true)) ==
          transport::websocket::unsupportedData);
    Bytes huge{0x82, 0xFF};
    appendBigEndian(huge, 1u << 20, 8);
    huge.insert(huge.end(), {1, 2, 3, 4});
    CHECK(closedWith(huge) == transport::websocket::tooBig);
    CHECK(closedWith(Bytes{0x82 | 0x40, 0x80, 1, 2, 3, 4}) == transport::websocket::protocolError); // RSV1.
    // The TCP entry: a datagram of no bytes breaks the framing; the connection closes.
    {
        Raw raw(f.at(RCSFWD_ENTRY_TCP));
        raw.send(Bytes{0, 0});
        CHECK(raw.read(2s).second);
    }
    // Connections that never open, or never carry a hop, are closed in time; the opening's within 5 s, a
    // hop's within 10 s.
    const uint64_t refusedBefore = f.forwarder->stats().connections_refused;
    Raw slow(ws), idle(f.at(RCSFWD_ENTRY_TCP));
    slow.send("GET / HTTP/1.1\r\n"); // And then nothing.
    CHECK(slow.read(forwarder::Entry::openWait + 2s).second);
    CHECK(idle.read(forwarder::Entry::claimWait - forwarder::Entry::openWait + 2s).second);
    CHECK(f.forwarder->stats().connections_refused >= refusedBefore + 2);
    // Beyond max_ends connections at once, one is closed at once.
    std::vector<std::unique_ptr<Raw>> crowd;
    for (int i = 0; i < 13; ++i) {
        crowd.push_back(std::make_unique<Raw>(f.at(RCSFWD_ENTRY_TCP)));
    }
    size_t closed = 0;
    for (auto& raw : crowd) {
        closed += raw->read(100ms).second;
    }
    CHECK(closed >= 1);
    std::printf("entries: %llu connections refused, %zu of a crowd of 13 closed at once\n",
                (unsigned long long)f.forwarder->stats().connections_refused, closed);
}

// An end that sends pictures though it is not the room's sharer (before its
// token, or a viewer) is dropped at its first fragment: the forwarder holds
// nothing of them (a picture may be 64 MiB, and its missing pieces are
// waited for many seconds).
void picturesOnlyFromSharers() {
    Fixture f;
    auto sharer = f.sharer("p");
    MediaSealer sealer;
    const auto large = seal(sealer, picture(0, 0, true, 1, 8u << 20));
    for (const bool viewer : {false, true}) {
        transport::Credentials credentials;
        credentials.secret = RCS_SECRET_NONE;
        credentials.forwarder = true;
        UdpOptions options;
        options.videoFrom = transport::Side::client;
        RateRules slow;
        slow.maxPacingBps = 16'000'000; // The picture takes seconds on the way.
        options.rate = Connection::VideoRate{false, 4'000'000, 4'000'000, 4'000'000, slow, {}};
        End end(UdpConnection::establish(
            UdpConnection::Side::client, std::make_unique<transport::UdpSocket>(transport::Endpoint::parse("127.0.0.1", 0)),
            directLink(f.at(RCSFWD_ENTRY_UDP)), credentials, options, f.cancelled));
        if (viewer) {
            CHECK(std::holds_alternative<ForwarderWelcome>(
                end.answer({ForwarderHello::subscribe, f.token(RCSFWD_ROLE_SUBSCRIBE, "p")})));
        }
        const auto started = Clock::now();
        end.hop().send(large);
        CHECK(waitUntil([&] { return end.ended(); }, 1s));
        std::printf("pictures: %s that sent one was dropped after %lld ms\n", viewer ? "a viewer" : "an end without a token",
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count());
    }
    CHECK(f.forwarder->roomStats("p").media_frames == 0 && !sharer->ended());
}

// A WebSocket peer that pings and never reads the pongs (one that proved
// nothing yet, too) makes the entry hold no more than maxOutbound for it.
void unreadPongs() {
    forwarder::EntryReader reader(forwarder::EntryKind::websocket);
    std::vector<Bytes> datagrams;
    Bytes outbound;
    const auto opening = transport::websocket::open("x", "/").request;
    CHECK(reader.take(std::span(reinterpret_cast<const uint8_t*>(opening.data()), opening.size()), datagrams,
                      outbound));
    CHECK(reader.upgraded());
    Bytes pings; // A read's worth, as the entry reads.
    while (pings.size() < 64 * 1024) {
        const auto ping = transport::websocket::frame(transport::websocket::ping, Bytes(125, 7), true);
        pings.insert(pings.end(), ping.begin(), ping.end());
    }
    for (int read = 0; read < 64; ++read) { // 4 MiB of pings, nothing taken out.
        CHECK(reader.take(pings, datagrams, outbound));
    }
    CHECK(datagrams.empty());
    CHECK(outbound.size() <= forwarder::Entry::maxOutbound);
    std::printf("unread pongs: %zu bytes wait for the peer after 4 MiB of pings\n", outbound.size());
}

// A hop's batch goes to its connection in one write, each datagram its own
// frame, in order. A peer that reads nothing makes the entry drop what would
// wait beyond maxOutbound, whole datagrams only: a large one that does not
// fit is dropped while later small ones that fit go on; what it took arrives
// once the peer reads again.
void entryBatches() {
    using Kind = forwarder::Entry::Kind;
    for (const auto kind : {Kind::tcp, Kind::websocket}) {
        forwarder::Entry entry(kind, transport::Endpoint::parse("127.0.0.1", 0), 4);
        Raw peer(transport::Endpoint::parse("127.0.0.1", entry.port()));
        if (kind == Kind::websocket) {
            peer.send(transport::websocket::open("x", "/").request);
            const auto opened = peer.read(300ms);
            CHECK(!opened.second && peer.text(opened.first).rfind("HTTP/1.1 101", 0) == 0);
        }
        CHECK(waitUntil([&] { return entry.connections() == 1; }, 2s));
        entry.claim(1); // The first connection's route.
        const auto to = RouteDemux::endpoint(1);
        auto socket = entry.routes().connection(0, to);
        // Numbered datagrams, every other one large; about 32 MB while the peer reads nothing.
        const auto datagram = [](uint32_t n) {
            Bytes d(n % 2 ? 100 : 16000, uint8_t(n));
            storeBigEndian(d, 0, n, 4);
            return d;
        };
        constexpr uint32_t sent = 63 * 64;
        for (uint32_t first = 0; first < sent; first += 64) {
            std::vector<Bytes> datagrams;
            std::vector<transport::Outgoing> batch;
            for (uint32_t n = first; n < first + 64; ++n) {
                datagrams.push_back(datagram(n));
            }
            for (const auto& d : datagrams) {
                batch.push_back({&to, d});
            }
            socket->sendBatch(batch);
        }
        const auto [wire, closed] = peer.read(2s);
        CHECK(!closed);
        std::vector<Bytes> got;
        if (kind == Kind::tcp) {
            auto lengths = transport::lengthFraming();
            lengths->push(wire);
            Bytes none;
            while (auto d = lengths->pop(none)) {
                got.push_back(std::move(*d));
            }
        } else {
            transport::websocket::Decoder frames(false);
            frames.push(wire);
            while (auto event = frames.next()) {
                CHECK(event->kind == transport::websocket::binary);
                got.push_back(std::move(event->payload));
            }
        }
        bool largeDropped = false, smallAfter = false;
        for (uint32_t expected = 0; const auto& d : got) {
            const auto n = uint32_t(loadBigEndian(d, 0, 4));
            CHECK(n >= expected && n < sent && d == datagram(n)); // In order, whole.
            largeDropped = largeDropped || n > expected + expected % 2; // Missing: an even (large) one.
            smallAfter = smallAfter || (largeDropped && n % 2);
            expected = n + 1;
        }
        const uint64_t refused = entry.routes().refused();
        CHECK(got.size() + refused == sent && refused > 0 && smallAfter);
        // The connection goes on: one more, alone.
        const Bytes last = datagram(sent);
        CHECK(socket->send(to, last));
        const auto [more, ended] = peer.read(300ms);
        CHECK(!ended && more.size() == last.size() + (kind == Kind::tcp ? 2 : 4));
        std::printf("entry batches (%s): %zu of %u datagrams arrived, %llu dropped while the peer read nothing\n",
                    kind == Kind::tcp ? "TCP" : "WebSocket", got.size(), sent, (unsigned long long)refused);
    }
}

// The C interface's settings: what no version defines is refused.
void settings() {
    rcsfwd_config c = RCSFWD_CONFIG_INIT;
    rcsfwd forwarder = nullptr;
    char why[256];
    const auto refused = [&](const char* field) {
        if (rcsfwd_create(&c, &forwarder) != RCS_ERROR_INVALID_ARGUMENT && !forwarder) {
            return false;
        }
        rcsfwd_get_last_error(why, sizeof(why));
        return std::string(why).find(field) != std::string::npos;
    };
    CHECK(refused("listen_udp"));
    std::strcpy(c.listen_websocket, "localhost:80");
    CHECK(refused("listen_websocket"));
    std::strcpy(c.listen_websocket, "127.0.0.1:0");
    std::strcpy(c.listen_tcp, "127.0.0.1:0");
    CHECK(refused("identity_key"));
    CHECK(rcsfwd_generate_identity(c.identity_key) == RCS_OK);
    CHECK(refused("secrets"));
    rcsfwd_secret secret{1, {}};
    c.secrets = &secret;
    c.secret_count = 1;
    c.max_viewers_per_room = RCS_MAX_VIEWERS + 1;
    CHECK(refused("max_viewers_per_room"));
    c.max_viewers_per_room = 0;
    c.max_rooms = 70'000;
    CHECK(refused("max_rooms"));
    c.max_rooms = 2;
    c.cache_bytes = 1000;
    CHECK(refused("cache_bytes"));
    c.cache_bytes = 2u << 20;
    c.max_cache_bytes = 1u << 20;
    CHECK(refused("max_cache_bytes"));
    c.max_cache_bytes = 0;
    c.cache_ms = 99;
    CHECK(refused("cache_ms"));
    c.cache_ms = 0;
    c.max_publish_bitrate_bps = 1000;
    CHECK(refused("max_publish_bitrate_bps"));
    c.max_publish_bitrate_bps = 0;
    c.version = RCSDK_VERSION + 1;
    CHECK(refused("version"));
    c.version = RCSDK_VERSION;
    CHECK(rcsfwd_create(&c, &forwarder) == RCS_OK && forwarder);
    uint32_t port = 0;
    CHECK(rcsfwd_get_port(forwarder, RCSFWD_ENTRY_TCP, &port) == RCS_ERROR_INVALID_STATE);
    CHECK(rcsfwd_start(forwarder) == RCS_OK);
    CHECK(rcsfwd_get_port(forwarder, RCSFWD_ENTRY_TCP, &port) == RCS_OK && port);
    CHECK(rcsfwd_get_port(forwarder, RCSFWD_ENTRY_WEBSOCKET, &port) == RCS_OK && port);
    CHECK(rcsfwd_get_port(forwarder, RCSFWD_ENTRY_UDP, &port) == RCS_ERROR_INVALID_ARGUMENT); // None.
    CHECK(rcsfwd_get_port(forwarder, 7, &port) == RCS_ERROR_INVALID_ARGUMENT);
    rcsfwd_room_stats room = RCSFWD_ROOM_STATS_INIT;
    CHECK(rcsfwd_get_room_stats(forwarder, "none", &room) == RCS_ERROR_INVALID_ARGUMENT);
    uint8_t sha[32];
    CHECK(rcsfwd_public_key_sha256(c.identity_key, sha) == RCS_OK);
    CHECK(rcsfwd_stop(forwarder) == RCS_OK);
    rcsfwd_destroy(forwarder);
}

} // namespace

int main() {
    return runTest("forwarder", [] {
        tokens();
        streamKeys();
        hopMessages();
        unreadPongs();
        settings();
        picturesOnlyFromSharers();
        refusals();
        floodsAndPictures();
        keptAndSound();
        behindAlone();
        rooms();
        entries();
        entryBatches();
        silentEnds();
    });
}
