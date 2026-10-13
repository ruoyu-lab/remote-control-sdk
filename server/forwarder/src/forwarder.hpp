// The forwarder (rcsfwd.h): entries the ends' hops come in at (the SDK's UDP
// connection under the magic RCSFWD: transport/handshake.hpp,
// protocol/udp_connection.hpp; over UDP, or a TCP or WebSocket connection
// each: entry.hpp), rooms of one sharer and its viewers, and what each
// viewer gets.
//
// Threads: one per entry takes the handshakes (UdpAcceptor), one per end
// reads its hop (its first message is its token; the rest it sends moves
// through the room on that thread: a viewer's pipe to the sharer, the
// sharer's pipes, pictures, sound and subscriptions to the viewers), and one
// turns every 100 ms: each viewer's path to the sharer (PathReport), viewers
// whose path fell far behind moved down at once, ends that never said who
// they are dropped, and ended ones joined. Each viewer's hop paces, resends
// and adds parity for that viewer alone; its adaptive bitrate measures that
// viewer's path.
//
// What a viewer starts on: a stream it newly takes, or another encoding,
// starts at a keyframe. The room keeps each encoding's newest keyframe and
// the pictures after it (cache_ms, cache_bytes, max_cache_bytes), sealed as
// they came; a viewer whose stream key opens them (Subscribe's key: not
// before its step) gets them at once, others the next keyframe, which the
// sharer is asked for. Pictures go to a viewer from its sharer's thread
// only, so that they keep their order: a viewer the turn moved starts on
// what was kept at the sharer's next message.
//
// Bounds against what an end may make it hold: the handshakes each acceptor
// runs (16 at once, Retry cookies); ends, rooms and viewers (max_ends,
// max_rooms, max_viewers_per_room; ends without a token yet: maxUnnamed,
// each dropped 5 s after its handshake); TCP and WebSocket connections
// (entry.hpp); a hop's own queues (a viewer's video backlog is dropped
// beyond what its hop holds); pipes, each way at max_pipe_bps; pictures from
// the room's sharer only (any other end's first piece of one drops it), at
// max_publish_bitrate_bps with its sound; what is kept; and messages
// each type validated (protocol/messages.cpp): an end that breaks the hop's
// rules is dropped.
#pragma once

#include "entry.hpp"
#include "protocol/udp_connection.hpp"
#include "token.hpp"

#include <rcsfwd.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace rcsdk::forwarder {

struct Settings {
    std::optional<transport::Endpoint> listenUdp, listenTcp, listenWebSocket;
    std::array<uint8_t, RCSFWD_IDENTITY_KEY_SIZE> identity{};
    std::vector<rcsfwd_secret> secrets;
    int32_t (*verifyToken)(const char*, rcsfwd_grant*, void*) = nullptr;
    uint32_t maxRooms = 64, maxViewers = 64, maxEnds = 1024;
    uint32_t maxViewerBitrate = 100'000'000, maxPipeBps = 50'000'000, maxPublishBps = 400'000'000;
    uint32_t cacheMs = 3000;
    uint64_t cacheBytes = 16u << 20, maxCacheBytes = 256u << 20;
    void (*onEvent)(const rcsfwd_event*, void*) = nullptr;
    void* userData = nullptr;
};

class Forwarder {
public:
    static constexpr size_t maxUnnamed = 32;       // Ends past the handshake that did not send their token yet.
    static constexpr auto helloWait = 5s;          // How long such an end may take.
    static constexpr auto turn = 100ms;            // Path reports, moves, upkeep.
    static constexpr auto farBehind = 1s;          // A viewer's backlog, or short path, that moves it down at once.
    static constexpr auto restartWait = 2s;        // One far behind on its own encoding starts again this often.

    explicit Forwarder(Settings settings);
    ~Forwarder();
    Forwarder(const Forwarder&) = delete;
    Forwarder& operator=(const Forwarder&) = delete;

    void start(); // Throws NETWORK when an entry cannot be bound; INVALID_STATE while serving.
    void stop();
    // The port of `entry`; throws INVALID_STATE unless serving, INVALID_ARGUMENT for one it has not.
    uint16_t port(rcsfwd_entry entry = RCSFWD_ENTRY_UDP) const;

    rcsfwd_stats stats() const;
    // Throws INVALID_ARGUMENT for a room it does not have.
    rcsfwd_room_stats roomStats(const std::string& room) const;
    std::vector<rcsfwd_viewer_stats> viewers(const std::string& room) const;
    void closeRoom(const std::string& room);

    // Tests: each sealed picture as it passes, which it may change (the
    // forwarder never sees inside; a viewer must refuse what was changed).
    void setMediaHook(std::function<void(protocol::SealedVideo&)> hook);

    struct End;
    struct Room;
    struct Door;

private:
    void acceptLoop(Door& door) noexcept;
    void serve(const std::shared_ptr<End>& end) noexcept;
    void turnLoop() noexcept;
    void welcome(const std::shared_ptr<End>& end, const protocol::ForwarderHello& hello);
    void fromSharer(End& sharer, protocol::Message& message);
    void fromViewer(End& viewer, protocol::Message& message);
    // From the room's sharer, on its thread.
    void publish(End& sharer, protocol::SealedVideo& video);
    void publishSound(End& sharer, const protocol::SealedAudio& sound);
    void subscribe(End& sharer, const protocol::Subscribe& subscription);
    // Viewers the turn moved: what was kept for them, or a keyframe asked.
    void startMoved(End& sharer);
    void lostOnTheWay(End& sharer, uint8_t stream);
    void leave(const std::shared_ptr<End>& end, rcs_result result, const std::string& why);
    void reportPaths(Clock::time_point now);
    void event(rcsfwd_event_type type, const End* end, const std::string& room, rcs_result result = RCS_OK,
               const std::string& message = {});
    void refuse(End& end, rcs_result result, const std::string& why);

    const Settings settings_;
    std::array<uint8_t, 32> audience_{}; // The SHA-256 of its public key: what its tokens are for.
    std::atomic<bool> stopping_{false};
    std::vector<std::unique_ptr<Door>> doors_;
    std::thread turnThread_;

    mutable std::mutex mutex_; // Below, and each End's and Room's state.
    std::condition_variable turned_;
    std::vector<std::shared_ptr<End>> ends_;
    std::map<std::string, std::shared_ptr<Room>> rooms_;
    std::function<void(protocol::SealedVideo&)> mediaHook_;
    uint64_t refused_ = 0, rejected_ = 0, helloTimeouts_ = 0;
    uint64_t cacheBytes_ = 0; // Kept in every room.
};

} // namespace rcsdk::forwarder
