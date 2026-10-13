#include "forwarder.hpp"

#include "crypto/crypto.hpp"
#include "util/util.hpp"

#include <algorithm>
#include <cstring>

namespace rcsdk::forwarder {

using protocol::Message;

namespace {

constexpr uint32_t minimumBitrate = 300'000;   // A viewer's path is measured from here up...
constexpr uint32_t startBitrate = 8'000'000;   // ...starting here.
constexpr auto running = 2s;                   // An encoding whose picture passed this recently runs.
constexpr size_t minimumPipeCost = 256;        // What a pipe datagram costs at least.

// A budget, one way: bytes per second, a quarter second's worth at once.
struct Bucket {
    double tokens = 0;
    Clock::time_point at{};
    bool take(size_t bytes, uint32_t bps, Clock::time_point now, size_t minimumCost = 0) {
        const double rate = double(bps) / 8, burst = std::max(rate / 4, 65536.0);
        tokens = at == Clock::time_point{} ? burst
                                           : std::min(burst, tokens + rate * std::chrono::duration<double>(now - at).count());
        at = now;
        const double cost = double(std::max(bytes, minimumCost));
        if (tokens < cost) {
            return false;
        }
        tokens -= cost;
        return true;
    }
};

// Sends without letting an end that just failed fail the sender (its reader finds out).
template <class Send>
void quietly(Send&& send) noexcept {
    try {
        send();
    } catch (...) {
    }
}

std::string addressOf(const transport::Endpoint& address) {
    return address.valid() ? address.address() : std::string();
}

const char* entryName(rcsfwd_entry entry) {
    return entry == RCSFWD_ENTRY_TCP ? "TCP" : entry == RCSFWD_ENTRY_WEBSOCKET ? "WebSocket" : "UDP";
}

} // namespace

struct Forwarder::Door {
    rcsfwd_entry entry = RCSFWD_ENTRY_UDP;
    std::unique_ptr<protocol::UdpDemux> udp;
    std::unique_ptr<Entry> stream; // TCP or WebSocket.
    std::unique_ptr<protocol::UdpAcceptor> acceptor;
    std::thread thread;
    protocol::Demux& demux() { return udp ? static_cast<protocol::Demux&>(*udp) : stream->routes(); }
};

struct Forwarder::End {
    std::unique_ptr<protocol::UdpConnection> connection;
    std::shared_ptr<std::atomic<uint64_t>> reports; // Its hop's adaptive bitrate measured the path this often.
    Door* door = nullptr;
    uint64_t connectionRoute = 0; // TCP, WebSocket: its connection's route in the entry.
    transport::Endpoint address;
    Clock::time_point connected = Clock::now();
    std::thread thread;
    std::atomic<bool> done{false};

    // The forwarder's mutex.
    rcsfwd_role role = 0; // 0 until its token was taken.
    std::shared_ptr<Room> room;
    uint64_t peer = 0, route = 0;
    bool cancelled = false; // Dropped: its reader ends.
    // A viewer's.
    uint32_t streams = 0;      // Its subscription...
    uint8_t encoding = 0;      // ...of this encoding (or the one it was moved down to),
    uint8_t sound = 0;         // ...this sound,
    protocol::MediaKeyAck key; // ...and the stream key it holds, from this step on.
    uint32_t waiting = 0;      // Streams waiting for a keyframe of `encoding`.
    bool moved = false;        // The turn moved it: it starts on what was kept at the sharer's next message.
    // Far behind on its own encoding: it starts again (moved) then, restartWait after it last did.
    std::optional<Clock::time_point> restartAt;
    Clock::time_point restarted{};
    uint64_t sentFrames = 0, skippedFrames = 0, sentBytes = 0, movesDown = 0, dropsSeen = 0;
    Clock::time_point shortSince{}; // Its path has carried less than half of what its encoding sends since.
    Bucket toSharer, fromSharer;
    // A sharer's.
    Bucket published;
};

struct Forwarder::Room {
    std::string name;
    std::shared_ptr<End> sharer;
    std::map<uint64_t, std::shared_ptr<End>> viewers;
    uint64_t nextPeer = 1;
    std::array<std::array<Clock::time_point, RCS_MAX_ENCODINGS>, RCS_MAX_STREAMS> seen{}; // A picture passed then.
    // What each stream's encodings sent: bytes so far in this second, bits per second in the last one.
    std::array<std::array<uint64_t, RCS_MAX_ENCODINGS>, RCS_MAX_STREAMS> bytes{}, rates{};
    Clock::time_point measured = Clock::now();
    std::set<std::pair<uint8_t, uint8_t>> keyframes; // (stream, encoding) a moved viewer waits for.
    // Each stream's encodings' newest keyframe and the pictures after it, as they came.
    struct Kept {
        Clock::time_point keyframeAt{};
        uint32_t generation = 0, step = 0; // The keyframe's key.
        std::vector<Bytes> pictures;
        uint64_t bytes = 0;
    };
    std::array<std::array<Kept, RCS_MAX_ENCODINGS>, RCS_MAX_STREAMS> kept;
    rcsfwd_room_stats stats = RCSFWD_ROOM_STATS_INIT;
};

Forwarder::Forwarder(Settings settings) : settings_(std::move(settings)) {
    const crypto::DeviceKey pair(settings_.identity);
    audience_ = crypto::sha256(pair.publicKey());
}

Forwarder::~Forwarder() {
    stop();
}

void Forwarder::start() {
    if (!doors_.empty()) {
        fail(RCS_ERROR_INVALID_STATE, "The forwarder serves already");
    }
    stopping_ = false;
    transport::Credentials credentials;
    credentials.secret = RCS_SECRET_NONE; // The ends check the forwarder's key; the forwarder checks their tokens.
    credentials.device = std::make_shared<const crypto::DeviceKey>(settings_.identity);
    credentials.forwarder = true;
    auto source = std::make_shared<const transport::Credentials>(std::move(credentials));
    const auto rejected = [this](const Failure&) {
        std::lock_guard lock(mutex_);
        ++rejected_;
    };
    try {
        for (const auto& [entry, listen] :
             {std::pair(RCSFWD_ENTRY_UDP, settings_.listenUdp), std::pair(RCSFWD_ENTRY_TCP, settings_.listenTcp),
              std::pair(RCSFWD_ENTRY_WEBSOCKET, settings_.listenWebSocket)}) {
            if (!listen) {
                continue;
            }
            auto door = std::make_unique<Door>();
            door->entry = entry;
            if (entry == RCSFWD_ENTRY_UDP) {
                door->udp = std::make_unique<protocol::UdpDemux>(*listen);
                door->acceptor =
                    std::make_unique<protocol::UdpAcceptor>(door->demux(), [source] { return source; }, rejected, 5s);
            } else {
                door->stream = std::make_unique<Entry>(
                    entry == RCSFWD_ENTRY_TCP ? Entry::Kind::tcp : Entry::Kind::websocket, *listen, settings_.maxEnds);
                // A channel's route, as a host's: a hop ends with its connection.
                const std::string name = std::string("the forwarder's ") + entryName(entry) + " entry";
                door->acceptor = std::make_unique<protocol::UdpAcceptor>(
                    door->demux(), [source] { return source; }, rejected, 5s,
                    [name](const transport::Endpoint& from) {
                        return protocol::routeLink(name, from, RCS_ROUTE_HOST);
                    });
            }
            doors_.push_back(std::move(door));
        }
    } catch (...) {
        doors_.clear();
        throw;
    }
    for (auto& door : doors_) {
        door->thread = std::thread([this, d = door.get()] { acceptLoop(*d); });
    }
    turnThread_ = std::thread([this] { turnLoop(); });
}

void Forwarder::stop() {
    if (doors_.empty()) {
        return;
    }
    stopping_ = true;
    for (auto& door : doors_) {
        door->thread.join();
    }
    {
        std::lock_guard lock(mutex_);
        turned_.notify_all();
    }
    turnThread_.join();
    std::vector<std::shared_ptr<End>> ends;
    {
        std::lock_guard lock(mutex_);
        ends.swap(ends_);
    }
    for (const auto& end : ends) { // Each hears that the forwarder goes, and comes back as its settings say.
        end->connection->leaving(protocol::Goodbye::restarting);
        end->connection->cancel();
    }
    for (const auto& end : ends) {
        end->thread.join();
    }
    ends.clear(); // Their goodbyes leave through the entries, which close after them.
    {
        std::lock_guard lock(mutex_);
        rooms_.clear();
        cacheBytes_ = 0;
    }
    doors_.clear();
}

uint16_t Forwarder::port(rcsfwd_entry entry) const {
    if (doors_.empty()) {
        fail(RCS_ERROR_INVALID_STATE, "The forwarder does not serve");
    }
    for (const auto& door : doors_) {
        if (door->entry == entry) {
            return door->udp ? door->udp->local().port() : door->stream->port();
        }
    }
    fail(RCS_ERROR_INVALID_ARGUMENT, std::string("The forwarder has no ") + entryName(entry) + " entry");
}

void Forwarder::setMediaHook(std::function<void(protocol::SealedVideo&)> hook) {
    std::lock_guard lock(mutex_);
    mediaHook_ = std::move(hook);
}

// ---- Ends ----------------------------------------------------------------------------------

void Forwarder::acceptLoop(Door& door) noexcept {
    for (;;) {
        try {
            auto reports = std::make_shared<std::atomic<uint64_t>>(0);
            const auto options = [&] {
                protocol::UdpOptions o;
                // Viewers get pictures from the forwarder; an end sends it some
                // only once it said it is the room's sharer (welcome).
                o.videoFrom = transport::Side::server;
                o.rate = protocol::Connection::VideoRate{true, minimumBitrate, settings_.maxViewerBitrate,
                                                         startBitrate, {}, {}};
                o.decideRate = [reports](const rcs_rate_report&, rcs_rate_decision&) { ++*reports; };
                return o;
            };
            transport::Endpoint from;
            auto connection = door.acceptor->accept(options, stopping_, &from);
            auto end = std::make_shared<End>();
            end->connection = std::move(connection);
            end->reports = std::move(reports);
            end->door = &door;
            end->address = from;
            if (door.stream) { // The end's address is its connection's; the route stands for that.
                end->connectionRoute = protocol::RouteDemux::routeOf(from).value_or(0);
                door.stream->claim(end->connectionRoute);
                end->address = door.stream->peer(end->connectionRoute).value_or(transport::Endpoint{});
            }
            std::lock_guard lock(mutex_);
            const size_t unnamed =
                size_t(std::count_if(ends_.begin(), ends_.end(), [](const auto& e) { return !e->role; }));
            if (unnamed >= maxUnnamed || ends_.size() >= settings_.maxEnds) {
                ++refused_;
                quietly([&] {
                    end->connection->send(protocol::Refused{
                        RCS_ERROR_BUSY, unnamed >= maxUnnamed ? "The forwarder is busy"
                                                              : "The forwarder is full (" +
                                                                    std::to_string(settings_.maxEnds) + " ends)"});
                });
                if (door.stream) {
                    door.stream->close(end->connectionRoute);
                }
                continue; // Destroyed here: the refusal and the close go out.
            }
            end->thread = std::thread([this, end] { serve(end); });
            ends_.push_back(std::move(end));
        } catch (const Failure& failure) {
            if (failure.result() == RCS_ERROR_CANCELLED) {
                return;
            }
        } catch (...) {
            // A thread the system would not start: that end is dropped.
        }
    }
}

void Forwarder::refuse(End& end, rcs_result result, const std::string& why) {
    {
        std::lock_guard lock(mutex_);
        ++refused_;
    }
    quietly([&] { end.connection->send(protocol::Refused{result, why}); });
    event(RCSFWD_EVENT_REFUSED, &end, {}, result, why);
}

void Forwarder::serve(const std::shared_ptr<End>& end) noexcept {
    rcs_result result = RCS_ERROR_DISCONNECTED;
    std::string why = "It left";
    try {
        auto first = end->connection->receive(); // The turn cancels one that says nothing.
        const auto* hello = std::get_if<protocol::ForwarderHello>(&first);
        if (!hello) {
            fail(RCS_ERROR_PROTOCOL, "Its first message was not its token");
        }
        welcome(end, *hello);
        for (;;) {
            auto message = end->connection->receive();
            if (end->role == RCSFWD_ROLE_PUBLISH) {
                fromSharer(*end, message);
            } else {
                fromViewer(*end, message);
            }
        }
    } catch (const Failure& failure) {
        result = failure.result();
        why = failure.what();
        if (result == RCS_ERROR_PROTOCOL) {
            std::lock_guard lock(mutex_);
            ++rejected_;
        }
    } catch (const std::exception& error) {
        result = RCS_ERROR_INTERNAL;
        why = error.what();
    }
    leave(end, result, why);
    if (end->door->stream) {
        end->door->stream->close(end->connectionRoute); // Once its hop's goodbye went.
    }
    end->done = true;
}

// The end's token: it gets into its room, or is refused (and dropped).
void Forwarder::welcome(const std::shared_ptr<End>& end, const protocol::ForwarderHello& hello) {
    std::variant<Grant, std::string> checked;
    if (settings_.verifyToken) {
        rcsfwd_grant grant{};
        grant.struct_size = sizeof(grant);
        checked = settings_.verifyToken(hello.token.c_str(), &grant, settings_.userData)
                      ? forwarder::checked(grant, unixNow())
                      : std::variant<Grant, std::string>(std::string("The host's server refused the token"));
    } else {
        checked = verify(hello.token, settings_.secrets, audience_, unixNow());
    }
    if (const auto* why = std::get_if<std::string>(&checked)) {
        refuse(*end, RCS_ERROR_REFUSED, *why);
        fail(RCS_ERROR_REFUSED, *why);
    }
    const auto& grant = std::get<Grant>(checked);
    const auto role = hello.role == protocol::ForwarderHello::publish ? RCSFWD_ROLE_PUBLISH : RCSFWD_ROLE_SUBSCRIBE;
    if (grant.role != role) {
        const std::string why = role == RCSFWD_ROLE_PUBLISH ? "The token is a viewer's: it does not publish"
                                                            : "The token is a sharer's: it does not subscribe";
        refuse(*end, RCS_ERROR_REFUSED, why);
        fail(RCS_ERROR_REFUSED, why);
    }
    std::shared_ptr<End> sharer;
    std::vector<protocol::PeerJoined> present;
    bool opened = false;
    {
        std::unique_lock lock(mutex_);
        auto found = rooms_.find(grant.room);
        if (found == rooms_.end()) {
            if (rooms_.size() >= settings_.maxRooms) {
                lock.unlock();
                const std::string why =
                    "The forwarder serves as many rooms as it may (" + std::to_string(settings_.maxRooms) + ")";
                refuse(*end, RCS_ERROR_BUSY, why);
                fail(RCS_ERROR_BUSY, why);
            }
            auto room = std::make_shared<Room>();
            room->name = grant.room;
            found = rooms_.emplace(grant.room, std::move(room)).first;
            opened = true;
        }
        auto& room = *found->second;
        if (role == RCSFWD_ROLE_PUBLISH) {
            if (room.sharer) {
                lock.unlock();
                refuse(*end, RCS_ERROR_REFUSED, "The room has its sharer");
                fail(RCS_ERROR_REFUSED, "The room has its sharer");
            }
            room.sharer = end;
            for (const auto& [peer, viewer] : room.viewers) {
                present.push_back({peer, viewer->route});
            }
        } else {
            if (room.viewers.size() >= settings_.maxViewers) {
                lock.unlock();
                const std::string why = "The room is full (" + std::to_string(settings_.maxViewers) + " viewers)";
                refuse(*end, RCS_ERROR_BUSY, why);
                fail(RCS_ERROR_BUSY, why);
            }
            end->peer = room.nextPeer++;
            room.viewers.emplace(end->peer, end);
            sharer = room.sharer;
        }
        end->role = role;
        end->route = grant.route;
        end->room = found->second;
    }
    if (opened) {
        event(RCSFWD_EVENT_ROOM_OPENED, end.get(), grant.room);
    }
    if (role == RCSFWD_ROLE_SUBSCRIBE) {
        const uint32_t cap = grant.maxBitrate ? std::min(grant.maxBitrate, settings_.maxViewerBitrate)
                                              : settings_.maxViewerBitrate;
        end->connection->setVideoRate({true, std::min(minimumBitrate, cap), cap, std::min(startBitrate, cap), {}, {}});
    } else {
        end->connection->setVideoFrom(transport::Side::client); // Its pictures, from the welcome on.
    }
    end->connection->send(
        protocol::ForwarderWelcome{end->peer, role == RCSFWD_ROLE_PUBLISH || sharer, settings_.cacheMs});
    if (role == RCSFWD_ROLE_PUBLISH) {
        for (const auto& joined : present) {
            end->connection->send(joined);
        }
    } else if (sharer) {
        quietly([&] { sharer->connection->send(protocol::PeerJoined{end->peer, end->route}); });
    }
    event(RCSFWD_EVENT_JOINED, end.get(), grant.room);
}

// The sharer's: pipes to its viewers, pictures, sound, subscriptions, viewers
// it drops. Viewers the turn moved start first.
void Forwarder::fromSharer(End& sharer, Message& message) {
    auto& room = *sharer.room;
    startMoved(sharer);
    if (auto* pipe = std::get_if<protocol::Pipe>(&message)) {
        std::shared_ptr<End> viewer;
        bool passes = false;
        {
            std::lock_guard lock(mutex_);
            if (const auto found = room.viewers.find(pipe->peer); found != room.viewers.end()) {
                viewer = found->second;
                passes =
                    viewer->fromSharer.take(pipe->bytes.size(), settings_.maxPipeBps, Clock::now(), minimumPipeCost);
                room.stats.pipe_bytes += passes ? pipe->bytes.size() : 0;
                room.stats.pipe_dropped += !passes;
            }
        }
        if (passes) {
            quietly([&] { viewer->connection->send(protocol::Pipe{0, std::move(pipe->bytes)}); });
        }
    } else if (auto* video = std::get_if<protocol::SealedVideo>(&message)) {
        publish(sharer, *video);
    } else if (const auto* sound = std::get_if<protocol::SealedAudio>(&message)) {
        publishSound(sharer, *sound);
    } else if (const auto* subscription = std::get_if<protocol::Subscribe>(&message)) {
        subscribe(sharer, *subscription);
    } else if (const auto* left = std::get_if<protocol::PeerLeft>(&message)) {
        std::lock_guard lock(mutex_);
        if (const auto found = room.viewers.find(left->peer); found != room.viewers.end() && !found->second->cancelled) {
            found->second->cancelled = true; // The sharer sent it away: its hop closes.
            found->second->connection->cancel();
        }
    } else if (const auto* gap = std::get_if<protocol::StreamGap>(&message)) {
        lostOnTheWay(sharer, gap->stream);
    } else if (!std::holds_alternative<protocol::VideoMissing>(message)) { // Missing pieces may still come.
        fail(RCS_ERROR_PROTOCOL, "The sharer sent what a sharer does not send the forwarder");
    }
}

// A viewer's: its pipe to the sharer, and nothing else.
void Forwarder::fromViewer(End& viewer, Message& message) {
    if (auto* pipe = std::get_if<protocol::Pipe>(&message)) {
        if (pipe->peer) {
            fail(RCS_ERROR_PROTOCOL, "A viewer's pipe names another peer");
        }
        std::shared_ptr<End> sharer;
        bool passes = false;
        {
            std::lock_guard lock(mutex_);
            auto& room = *viewer.room;
            sharer = room.sharer;
            if (sharer) {
                passes = viewer.toSharer.take(pipe->bytes.size(), settings_.maxPipeBps, Clock::now(), minimumPipeCost);
                room.stats.pipe_bytes += passes ? pipe->bytes.size() : 0;
                room.stats.pipe_dropped += !passes;
            }
        }
        if (passes) { // Without a sharer, dropped: the viewer's handshake repeats, and times out.
            quietly([&] { sharer->connection->send(protocol::Pipe{viewer.peer, std::move(pipe->bytes)}); });
        }
    } else {
        fail(RCS_ERROR_PROTOCOL, "A viewer sent what only a sharer sends");
    }
}

namespace {

// Holding the forwarder's mutex: what `kept` holds goes.
void drop(Forwarder::Room& room, uint64_t& total, Forwarder::Room::Kept& kept) {
    room.stats.cache_bytes -= kept.bytes;
    total -= kept.bytes;
    kept = {};
}

} // namespace

// A sealed picture: to each viewer that takes its stream and encoding, from
// a keyframe on (sent once for all of them by the sharer); kept for viewers
// that come later.
void Forwarder::publish(End& sharer, protocol::SealedVideo& video) {
    auto& room = *sharer.room;
    const auto now = Clock::now();
    std::vector<std::shared_ptr<End>> to;
    Bytes encoded;
    {
        std::lock_guard lock(mutex_);
        if (!sharer.published.take(video.sealed.size(), settings_.maxPublishBps, now)) {
            ++room.stats.publish_dropped;
            return;
        }
        if (mediaHook_) {
            mediaHook_(video);
        }
        encoded = protocol::encode(video);
        ++room.stats.media_frames;
        room.stats.media_bytes += video.sealed.size();
        room.seen[video.stream][video.encoding] = now;
        room.bytes[video.stream][video.encoding] += video.sealed.size();
        const uint32_t bit = 1u << video.stream;
        const bool keyframe = video.flags & protocol::SealedVideo::keyframeFlag;
        auto& kept = room.kept[video.stream][video.encoding];
        if (keyframe) {
            drop(room, cacheBytes_, kept);
            kept.keyframeAt = now;
            kept.generation = video.generation;
            kept.step = video.step;
        }
        if (kept.pictures.empty() && !keyframe) {
            // Nothing kept to go on from.
        } else if (now - kept.keyframeAt > std::chrono::milliseconds(settings_.cacheMs) ||
                   room.stats.cache_bytes + encoded.size() > settings_.cacheBytes ||
                   cacheBytes_ + encoded.size() > settings_.maxCacheBytes) {
            drop(room, cacheBytes_, kept); // Too old, or too much: until the next keyframe.
        } else {
            kept.pictures.push_back(encoded);
            kept.bytes += encoded.size();
            room.stats.cache_bytes += encoded.size();
            cacheBytes_ += encoded.size();
        }
        for (const auto& [peer, viewer] : room.viewers) {
            if (!(viewer->streams & bit) || viewer->encoding != video.encoding || viewer->cancelled) {
                continue;
            }
            if (viewer->waiting & bit) {
                if (!keyframe) {
                    ++viewer->skippedFrames;
                    continue;
                }
                viewer->waiting &= ~bit;
            }
            ++viewer->sentFrames;
            viewer->sentBytes += video.sealed.size();
            ++room.stats.sent_frames;
            room.stats.sent_bytes += video.sealed.size();
            to.push_back(viewer);
        }
    }
    for (const auto& viewer : to) {
        quietly([&] { viewer->connection->sendVideo(video.stream, encoded); });
    }
}

// The sharer's sound: to each viewer subscribed to it, as it came (no resends).
void Forwarder::publishSound(End& sharer, const protocol::SealedAudio& sound) {
    auto& room = *sharer.room;
    std::vector<std::shared_ptr<End>> to;
    {
        std::lock_guard lock(mutex_);
        if (!sharer.published.take(sound.sealed.size(), settings_.maxPublishBps, Clock::now())) {
            ++room.stats.publish_dropped;
            return;
        }
        ++room.stats.sound_packets;
        room.stats.sound_bytes += sound.sealed.size();
        for (const auto& [peer, viewer] : room.viewers) {
            if (viewer->sound == sound.sound && !viewer->cancelled) {
                to.push_back(viewer);
            }
        }
    }
    for (const auto& viewer : to) {
        quietly([&] { viewer->connection->send(sound); });
    }
}

namespace {

// Holding the forwarder's mutex: of `streams`, which `viewer` began to wait
// for, those whose keyframe was kept under a key it holds, and what was kept
// after it fits its path in half a second (more would leave it far behind at
// once), start on what was kept (into `pictures`); the others wait for the
// next keyframe, asked of the sharer (into `asked`).
void startOnKept(Forwarder::Room& room, Forwarder::End& viewer, uint32_t streams, std::chrono::milliseconds cacheFor,
                 Clock::time_point now, std::vector<std::pair<uint8_t, const Bytes*>>& pictures,
                 std::vector<protocol::NeedKeyframe>& asked) {
    uint64_t fits = uint64_t(viewer.connection->status().targetBitrate) / 16; // Bytes in half a second.
    for (uint8_t s = 0; s < RCS_MAX_STREAMS; ++s) {
        const uint32_t bit = 1u << s;
        if (!(viewer.waiting & streams & bit)) {
            continue;
        }
        const auto& kept = room.kept[s][viewer.encoding];
        const bool opens = kept.generation > viewer.key.generation ||
                           (kept.generation == viewer.key.generation && kept.step >= viewer.key.step);
        if (kept.pictures.empty() || now - kept.keyframeAt > cacheFor || !opens || kept.bytes > fits) {
            asked.push_back({s, viewer.encoding});
            ++room.stats.keyframes_asked;
            continue;
        }
        for (const auto& picture : kept.pictures) {
            pictures.emplace_back(s, &picture);
            viewer.sentBytes += picture.size();
            room.stats.sent_bytes += picture.size();
        }
        viewer.sentFrames += kept.pictures.size();
        room.stats.sent_frames += kept.pictures.size();
        ++room.stats.cache_starts;
        viewer.waiting &= ~bit;
        fits -= kept.bytes;
    }
}

} // namespace

// What a viewer gets from now on: a stream it newly takes, or another
// encoding, from that encoding's newest kept keyframe or its next, which the
// sharer is asked for; what waited of the one it had goes.
void Forwarder::subscribe(End& sharer, const protocol::Subscribe& subscription) {
    auto& room = *sharer.room;
    std::shared_ptr<End> viewer;
    std::vector<std::pair<uint8_t, Bytes>> pictures;
    std::vector<protocol::NeedKeyframe> asked;
    bool moved = false;
    {
        std::lock_guard lock(mutex_);
        const auto found = room.viewers.find(subscription.peer);
        if (found == room.viewers.end()) {
            return; // It left meanwhile.
        }
        viewer = found->second;
        auto& v = *viewer;
        moved = subscription.encoding != v.encoding;
        const uint32_t began = moved ? subscription.streams : subscription.streams & ~v.streams & ~v.waiting;
        v.waiting = moved ? subscription.streams : (v.waiting | (subscription.streams & ~v.streams));
        v.waiting &= subscription.streams;
        v.streams = subscription.streams;
        v.encoding = subscription.encoding;
        v.sound = subscription.sound;
        v.key = {subscription.keyGeneration, subscription.keyStep};
        v.moved = false;
        std::vector<std::pair<uint8_t, const Bytes*>> kept;
        startOnKept(room, v, began, std::chrono::milliseconds(settings_.cacheMs), Clock::now(), kept, asked);
        for (const auto& [stream, picture] : kept) {
            pictures.emplace_back(stream, *picture);
        }
    }
    if (moved) {
        viewer->connection->discardVideo();
    }
    for (const auto& [stream, picture] : pictures) {
        quietly([&] { viewer->connection->sendVideo(stream, picture); });
    }
    for (const auto& need : asked) {
        quietly([&] { sharer.connection->send(need); });
    }
}

// A picture of `stream` the hop gave up on (the sharer's uplink lost it):
// what follows refers to it, so the stream's viewers start again at their
// encoding's next keyframe, which the sharer is asked for, and nothing kept
// of the stream goes on from it.
void Forwarder::lostOnTheWay(End& sharer, uint8_t stream) {
    auto& room = *sharer.room;
    std::set<std::pair<uint8_t, uint8_t>> asked;
    {
        std::lock_guard lock(mutex_);
        ++room.stats.media_lost;
        for (auto& kept : room.kept[stream]) {
            drop(room, cacheBytes_, kept);
        }
        const uint32_t bit = 1u << stream;
        for (const auto& [peer, viewer] : room.viewers) {
            if (viewer->streams & bit) {
                viewer->waiting |= bit;
                asked.insert({stream, viewer->encoding});
            }
        }
        room.stats.keyframes_asked += asked.size();
    }
    for (const auto& [s, e] : asked) {
        quietly([&] { sharer.connection->send(protocol::NeedKeyframe{s, e}); });
    }
}

void Forwarder::startMoved(End& sharer) {
    auto& room = *sharer.room;
    std::vector<std::pair<std::shared_ptr<End>, std::vector<std::pair<uint8_t, Bytes>>>> starts;
    std::vector<protocol::NeedKeyframe> asked;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [peer, viewer] : room.viewers) {
            if (!viewer->moved) {
                continue;
            }
            viewer->moved = false;
            std::vector<std::pair<uint8_t, const Bytes*>> kept;
            startOnKept(room, *viewer, viewer->waiting, std::chrono::milliseconds(settings_.cacheMs), Clock::now(),
                        kept, asked);
            auto& pictures = starts.emplace_back(viewer, std::vector<std::pair<uint8_t, Bytes>>{}).second;
            for (const auto& [stream, picture] : kept) {
                pictures.emplace_back(stream, *picture);
            }
        }
    }
    for (const auto& [viewer, pictures] : starts) {
        for (const auto& [stream, picture] : pictures) {
            quietly([&] { viewer->connection->sendVideo(stream, picture); });
        }
    }
    std::sort(asked.begin(), asked.end(), [](const auto& a, const auto& b) {
        return std::pair(a.stream, a.encoding) < std::pair(b.stream, b.encoding);
    });
    asked.erase(std::unique(asked.begin(), asked.end()), asked.end());
    for (const auto& need : asked) {
        quietly([&] { sharer.connection->send(need); });
    }
}

void Forwarder::leave(const std::shared_ptr<End>& end, rcs_result result, const std::string& why) {
    std::shared_ptr<End> sharer;
    std::string name;
    bool closed = false;
    {
        std::lock_guard lock(mutex_);
        const auto room = end->room;
        if (!room) {
            return; // Never got in (refused, or silent).
        }
        name = room->name;
        if (end->role == RCSFWD_ROLE_PUBLISH) {
            if (room->sharer == end) {
                room->sharer.reset();
                for (auto& stream : room->kept) { // Of its stream key: no one that comes opens it.
                    for (auto& kept : stream) {
                        drop(*room, cacheBytes_, kept);
                    }
                }
            }
        } else if (const auto found = room->viewers.find(end->peer); found != room->viewers.end() && found->second == end) {
            room->viewers.erase(found);
            sharer = room->sharer;
        }
        if (!room->sharer && room->viewers.empty()) {
            if (const auto found = rooms_.find(room->name); found != rooms_.end() && found->second == room) {
                rooms_.erase(found);
                closed = true;
            }
        }
    }
    if (sharer) {
        quietly([&] { sharer->connection->send(protocol::PeerLeft{end->peer}); });
    }
    event(RCSFWD_EVENT_LEFT, end.get(), name, result, why);
    if (closed) {
        event(RCSFWD_EVENT_ROOM_CLOSED, end.get(), name);
    }
}

// ---- The turn ------------------------------------------------------------------------------

void Forwarder::turnLoop() noexcept {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        turned_.wait_for(lock, turn);
        if (stopping_) {
            return;
        }
        const auto now = Clock::now();
        // Ends that never said who they are go; ended ones are joined.
        std::vector<std::shared_ptr<End>> ended;
        for (auto it = ends_.begin(); it != ends_.end();) {
            auto& end = **it;
            if (!end.role && !end.cancelled && now - end.connected > helloWait) {
                end.cancelled = true;
                ++helloTimeouts_;
                end.connection->cancel();
            }
            if (end.done) {
                ended.push_back(std::move(*it));
                it = ends_.erase(it);
            } else {
                ++it;
            }
        }
        lock.unlock();
        for (auto& end : ended) {
            end->thread.join();
        }
        ended.clear(); // Their hops close.
        try {
            reportPaths(now);
        } catch (...) {
            // Out of memory for a report: the next turn tries again.
        }
        lock.lock();
    }
}

// Each room's viewers: their paths to the sharer, and any whose path fell
// far behind moved down to a lower encoding that runs, at once (its backlog
// goes; it starts again on that encoding's kept keyframe, or its next).
// Far behind: its hop measured less than half of what its encoding actually
// sends for a second (the hop is paced no slower than the pictures come, so
// a slow path shows as loss and a low measure, not as backlog), its backlog
// passed a second, or the hop dropped its backlog. Without a lower one, one
// far behind in backlog starts again on its own encoding as one moved does
// (what was kept, or the next keyframe, which the forwarder asks for: once
// its backlog went, nothing reaches the viewer to show it what it missed,
// so it asks for nothing itself), at most once per restartWait, which bounds
// what its keyframes cost the others on its encoding (until then a keyframe
// that comes for them starts it); one whose path is short waits for the
// sharer's plan (a lower bitrate, or a lower encoding, which it moves to as
// soon as that runs). Its backlog goes before the sharer's thread may start
// it again.
void Forwarder::reportPaths(Clock::time_point now) {
    struct Mail {
        std::shared_ptr<End> sharer;
        std::vector<Message> messages;
    };
    std::vector<Mail> mail;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, room] : rooms_) {
            if (const auto elapsed = now - room->measured; elapsed >= 1s) {
                const double seconds = std::chrono::duration<double>(elapsed).count();
                for (size_t s = 0; s < RCS_MAX_STREAMS; ++s) {
                    for (size_t e = 0; e < RCS_MAX_ENCODINGS; ++e) {
                        room->rates[s][e] = uint64_t(double(room->bytes[s][e]) * 8 / seconds);
                        room->bytes[s][e] = 0;
                    }
                }
                room->measured = now;
            }
            Mail out{room->sharer, {}};
            protocol::PathReport report;
            for (const auto& [peer, viewer] : room->viewers) {
                const auto status = viewer->connection->status();
                const uint64_t backlog = viewer->connection->sendBacklogNs();
                const bool dropped = status.backlogDrops > viewer->dropsSeen;
                viewer->dropsSeen = status.backlogDrops;
                uint64_t sends = 0;
                for (uint32_t s = 0; s < RCS_MAX_STREAMS; ++s) {
                    sends += viewer->streams & (1u << s) ? room->rates[s][viewer->encoding] : 0;
                }
                const bool shortPath = *viewer->reports && uint64_t(status.targetBitrate) * 2 < sends;
                if (!shortPath) {
                    viewer->shortSince = {};
                } else if (viewer->shortSince == Clock::time_point{}) {
                    viewer->shortSince = now;
                }
                const bool fallen = shortPath && now - viewer->shortSince >= farBehind;
                const bool behind = dropped || backlog > uint64_t(std::chrono::nanoseconds(farBehind).count());
                if ((fallen || behind) && viewer->streams) {
                    // The next step down that runs on every stream it takes that runs now (it may be
                    // let have streams the sharer does not send).
                    std::optional<uint8_t> lower;
                    for (uint8_t e = uint8_t(viewer->encoding + 1); e < RCS_MAX_ENCODINGS && !lower; ++e) {
                        bool any = false, all = true;
                        for (uint32_t s = 0; s < RCS_MAX_STREAMS; ++s) {
                            if ((viewer->streams & (1u << s)) && now - room->seen[s][viewer->encoding] < running) {
                                any = true;
                                all = all && now - room->seen[s][e] < running;
                            }
                        }
                        if (any && all) {
                            lower = e;
                        }
                    }
                    if (lower || behind) {
                        viewer->connection->discardVideo();
                        viewer->shortSince = {};
                        viewer->waiting = viewer->streams;
                    }
                    if (lower) {
                        viewer->encoding = *lower;
                        viewer->moved = true; // It starts on what was kept, at the sharer's next message.
                        viewer->restartAt.reset();
                        ++viewer->movesDown;
                        ++room->stats.moves_down;
                        out.messages.push_back(protocol::PeerMoved{peer, *lower});
                    } else if (behind) {
                        viewer->restartAt = std::max(now, viewer->restarted + restartWait);
                    }
                }
                if (viewer->restartAt && now >= *viewer->restartAt) { // Meanwhile a keyframe may have come.
                    viewer->restartAt.reset();
                    viewer->restarted = now;
                    viewer->moved = true;
                }
                report.peers.push_back({peer, *viewer->reports ? status.targetBitrate : 0, status.lossPermille,
                                        uint32_t(std::min<uint64_t>(status.queueDelayUs, UINT32_MAX)),
                                        uint32_t(std::min<uint64_t>(status.roundTripNs / 1000, UINT32_MAX)),
                                        uint32_t(std::min<uint64_t>(backlog / 1000, UINT32_MAX)),
                                        uint32_t(status.backlogDrops), viewer->sentBytes});
                if (report.peers.size() == protocol::maxPathReportPeers) {
                    out.messages.push_back(std::move(report));
                    report = {};
                }
            }
            if (!report.peers.empty()) {
                out.messages.push_back(std::move(report));
            }
            if (out.sharer && !out.messages.empty()) {
                mail.push_back(std::move(out));
            }
        }
    }
    for (auto& [sharer, messages] : mail) {
        for (const auto& message : messages) {
            quietly([&] { sharer->connection->send(message); });
        }
    }
}

// ---- What the host sees --------------------------------------------------------------------

void Forwarder::event(rcsfwd_event_type type, const End* end, const std::string& room, rcs_result result,
                      const std::string& message) {
    if (!settings_.onEvent) {
        return;
    }
    rcsfwd_event e{};
    e.struct_size = sizeof(e);
    e.type = type;
    copyText(e.room, sizeof(e.room), room);
    if (end) {
        e.role = end->role;
        e.peer = end->peer;
        e.route = end->route;
        copyText(e.address, sizeof(e.address), addressOf(end->address));
        e.port = end->address.valid() ? end->address.port() : 0;
        e.entry = end->door->entry;
    }
    e.result = result;
    copyText(e.message, sizeof(e.message), message);
    settings_.onEvent(&e, settings_.userData);
}

rcsfwd_stats Forwarder::stats() const {
    std::lock_guard lock(mutex_);
    rcsfwd_stats s{sizeof(rcsfwd_stats),
                   uint32_t(rooms_.size()),
                   uint32_t(ends_.size()),
                   refused_,
                   rejected_,
                   helloTimeouts_,
                   0,
                   0,
                   cacheBytes_};
    for (const auto& door : doors_) {
        if (door->stream) {
            s.connections += uint32_t(door->stream->connections());
            s.connections_refused += door->stream->refused();
        }
    }
    return s;
}

rcsfwd_room_stats Forwarder::roomStats(const std::string& name) const {
    std::lock_guard lock(mutex_);
    const auto found = rooms_.find(name);
    if (found == rooms_.end()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The forwarder has no room \"" + name + "\"");
    }
    auto stats = found->second->stats;
    stats.sharer = found->second->sharer ? 1 : 0;
    stats.viewers = uint32_t(found->second->viewers.size());
    return stats;
}

std::vector<rcsfwd_viewer_stats> Forwarder::viewers(const std::string& name) const {
    std::lock_guard lock(mutex_);
    const auto found = rooms_.find(name);
    if (found == rooms_.end()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The forwarder has no room \"" + name + "\"");
    }
    std::vector<rcsfwd_viewer_stats> list;
    for (const auto& [peer, viewer] : found->second->viewers) {
        const auto status = viewer->connection->status();
        rcsfwd_viewer_stats v{};
        v.struct_size = sizeof(v);
        v.peer = peer;
        v.route = viewer->route;
        v.streams = viewer->streams;
        v.encoding = viewer->encoding;
        v.target_bps = *viewer->reports ? status.targetBitrate : 0;
        v.loss_permille = status.lossPermille;
        v.round_trip_ns = status.roundTripNs;
        v.backlog_ns = viewer->connection->sendBacklogNs();
        v.sent_frames = viewer->sentFrames;
        v.skipped_frames = viewer->skippedFrames;
        v.sent_bytes = viewer->sentBytes;
        v.moves_down = viewer->movesDown;
        copyText(v.address, sizeof(v.address), addressOf(viewer->address));
        v.port = viewer->address.valid() ? viewer->address.port() : 0;
        v.entry = viewer->door->entry;
        v.sound = viewer->sound;
        list.push_back(v);
    }
    return list;
}

void Forwarder::closeRoom(const std::string& name) {
    std::lock_guard lock(mutex_);
    const auto found = rooms_.find(name);
    if (found == rooms_.end()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The forwarder has no room \"" + name + "\"");
    }
    auto& room = *found->second;
    std::vector<std::shared_ptr<End>> members;
    if (room.sharer) {
        members.push_back(room.sharer);
    }
    for (const auto& [peer, viewer] : room.viewers) {
        members.push_back(viewer);
    }
    for (const auto& end : members) { // Their readers end, and each leaves the room.
        end->cancelled = true;
        end->connection->leaving(protocol::Goodbye::restarting);
        end->connection->cancel();
    }
}

} // namespace rcsdk::forwarder
