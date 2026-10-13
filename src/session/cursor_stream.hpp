// The pointer sent apart from the picture (RCS_CURSOR_SEPARATE).
// Share: CursorSender gives each distinct image an id that stays its own for
// the run, sends the image (packed, in parts that fit a control message) when
// the viewer does not hold it, then the position whenever the pointer moves,
// changes or hides. Both ends hold the last `heldImages` images sent, evicting
// in the order they were sent, so the sharer knows what the viewer holds
// without being told; an image that returns after it was evicted is sent again
// under its id.
// View: CursorReceiver assembles images and reports the pointer
// (RCS_EVENT_CURSOR), each image with the first event that shows it while
// the host does not hold it: the host keeps the last RCS_MAX_CURSOR_SHAPES
// images it was given, so one that returns after that many others is given
// again.
#pragma once

#include "protocol/connection.hpp"
#include "session/session.hpp"

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace rcsdk::session {

// Thread-safe: the agent reports the pointer on its own thread while the
// share loop turns the mode on and off. What it sends goes to `send` (one
// viewer's, in order).
class CursorSender {
public:
    using Send = std::function<void(protocol::Message)>;
    explicit CursorSender(Send send) : send_(std::move(send)) {}
    // On: sends the pointer as the agent last reported it, its image too, and
    // after that whenever it changes. Off: nothing.
    void setActive(bool active);
    // The pointer as it is now, on `stream`'s display (each stream's capture
    // numbers its images apart); kept even while off, for the next time it
    // turns on.
    void update(uint32_t stream, const CursorState& state);

    // Images both ends hold.
    static constexpr size_t heldImages = 32;

private:
    void send(); // Holding mutex_.
    // The id of `image`, sending it first if the viewer does not hold it.
    uint32_t idOf(const std::shared_ptr<const CursorImage>& image);

    const Send send_;
    std::mutex mutex_;
    CursorState state_;
    uint32_t stream_ = 0;
    bool active_ = false;
    std::optional<std::pair<uint32_t, uint64_t>> image_; // The stream and serial of the image sent last.
    uint32_t imageId_ = 0;
    std::map<uint64_t, uint32_t> ids_; // Every image of the run, by its content's digest.
    std::deque<std::pair<std::shared_ptr<const CursorImage>, uint32_t>> held_; // The viewer holds these, oldest first.
    uint32_t nextId_ = 1;
    std::optional<protocol::CursorPosition> sent_;
};

class CursorReceiver {
public:
    explicit CursorReceiver(Session& session) : session_(session) {}
    void part(const protocol::CursorShapePart& part);
    void position(const protocol::CursorPosition& position);

private:
    struct Pending {
        protocol::CursorShapePart header;
        std::vector<uint8_t> packed;
    };
    Session& session_;
    std::map<uint32_t, Pending> pending_;
    std::deque<std::pair<uint32_t, CursorImage>> images_; // As the sharer holds them, oldest first.
    std::deque<uint32_t> reported_; // The images the host holds, as given to it, oldest first.
};

} // namespace rcsdk::session
