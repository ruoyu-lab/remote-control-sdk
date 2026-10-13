// The separate pointer stream: each image keeps its id for the run, and both
// ends hold the same last images, so one that returns after it was evicted is
// sent again under its id while one still held is not.
#include "session/config.hpp"
#include "session/cursor_stream.hpp"
#include "session/session.hpp"
#include "support/check.hpp"

#include <rcsdk/rcsdk.h>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <vector>

using namespace rcsdk;
using namespace rcsdk::session;

namespace {

// Keeps what is sent, for the receiving end.
struct Recorder {
    void send(protocol::Message message) { sent.push_back(std::move(message)); }
    std::vector<protocol::Message> sent;
};

std::shared_ptr<const CursorImage> image(uint8_t shade) {
    auto value = std::make_shared<CursorImage>();
    value->width = value->height = 8;
    value->pixels.assign(8 * 8 * 4, shade);
    return value;
}

} // namespace

int main() {
    return runTest("cursor_stream", [] {
        // The viewer's session reports the pointer as events.
        struct Seen {
            uint32_t shapeId;
            bool withImage;
        };
        std::vector<Seen> seen;
        rcs_session_callbacks callbacks = RCS_SESSION_CALLBACKS_INIT;
        callbacks.user_data = &seen;
        callbacks.on_event = [](const rcs_event* event, void* data) {
            if (event->type == RCS_EVENT_CURSOR) {
                static_cast<std::vector<Seen>*>(data)->push_back({event->cursor.shape_id, event->cursor.shape != nullptr});
            }
        };
        rcs_session_config core = RCS_SESSION_CONFIG_INIT;
        core.identity.key[0] = 1;
        Session view(parseConfig(core), callbacks, log::makeSource(RCS_LOG_SOURCE_SESSION, nullptr, nullptr, 0));

        Recorder link;
        CursorSender sender([&](protocol::Message message) { link.send(std::move(message)); });
        CursorReceiver receiver(view);
        sender.setActive(true);
        uint64_t serial = 0;
        std::map<uint8_t, uint32_t> ids; // Each image's id as the viewer saw it first.
        size_t parts = 0;
        const auto show = [&](uint8_t shade) {
            ++serial;
            sender.update(0, {true, uint32_t(serial), 200, image(shade), serial}); // It moves: a position goes each time.
            size_t sentParts = 0;
            for (const auto& message : link.sent) {
                if (const auto* part = std::get_if<protocol::CursorShapePart>(&message)) {
                    receiver.part(*part);
                    ++sentParts;
                } else if (const auto* position = std::get_if<protocol::CursorPosition>(&message)) {
                    receiver.position(*position);
                }
            }
            link.sent.clear();
            parts += sentParts;
            CHECK(!seen.empty());
            ids.try_emplace(shade, seen.back().shapeId);
            CHECK(seen.back().shapeId == ids[shade]); // Its id for the run.
            return sentParts;
        };

        // More distinct images than both ends hold.
        constexpr int distinct = int(CursorSender::heldImages) + 8;
        for (int shade = 0; shade < distinct; ++shade) {
            CHECK(show(uint8_t(shade)) > 0);
            CHECK(seen.back().withImage); // The host gets each image once.
        }
        CHECK(ids.size() == size_t(distinct));
        // Still held: no image goes again, and the host is not given it again.
        CHECK(show(uint8_t(distinct - 1)) == 0);
        CHECK(!seen.back().withImage);
        // Evicted: sent again, under the same id, and the host is given it
        // again: it keeps only the last RCS_MAX_CURSOR_SHAPES it was given.
        CHECK(show(0) > 0);
        CHECK(seen.back().shapeId == ids[0] && seen.back().withImage);
        // The same pixels in another allocation are the same image.
        CHECK(show(0) == 0 && seen.back().shapeId == ids[0] && !seen.back().withImage);

        // A host that keeps the last RCS_MAX_CURSOR_SHAPES images, dropping the
        // oldest given first, always holds the one shown, however many there
        // are and in whatever order they come back (SA-14).
        std::deque<uint32_t> held;
        const auto hold = [&](const Seen& event) {
            if (event.withImage) {
                CHECK(std::find(held.begin(), held.end(), event.shapeId) == held.end());
                held.push_back(event.shapeId);
                if (held.size() > RCS_MAX_CURSOR_SHAPES) {
                    held.pop_front();
                }
            }
            CHECK(std::find(held.begin(), held.end(), event.shapeId) != held.end());
        };
        for (const Seen& event : seen) {
            hold(event);
        }
        uint32_t state = 1;
        for (int step = 0; step < 2000; ++step) {
            state = state * 1103515245u + 12345u;
            show(uint8_t((state >> 16) % (step < 1000 ? 40 : 200)));
            hold(seen.back());
        }
    });
}
