// Share role: which encoding each viewer gets and how many of its temporal
// layers, and what bitrate each encoding's encoder follows
// (rcs_viewers_config, RCS_EXT_ENCODINGS). The policy alone, apart from the
// stream that carries it out (share.cpp); pure logic, run by the share loop
// every 100 ms.
//
// A viewer falls short of what it gets when its path carries less than that
// actually sends (not its target: a desktop's picture mostly uses a fraction
// of it), or its send backlog drops video; it moves a step down after
// move_down_ms of that (at once after a drop), and a step up again once its
// path carried what the step above sends, with a quarter to spare, or that
// step's share of its encoding's target, for move_up_ms (at once where no
// one gets the encoding above). The steps: the layers of an encoding, the
// upper ones left out one at a time (at once, without a keyframe), then the
// encoding below with all its layers; up again a layer at a time, and from
// all of an encoding to layer 0 of the one above. Most of an encoding's
// viewers falling short together is the sharer's own uplink: no one moves;
// and a viewer alone on its encoding drags no one (it gets all its layers).
// Only QUORUM and MAIN move viewers for their paths; any mode moves one that
// causes too many keyframes (to the encoding below: fewer layers would not
// keep its keyframes from the others), one that cannot decode its encoding
// and one the host placed.
#pragma once

#include "util/api.hpp"
#include "util/util.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace rcsdk::session {

// A viewer as the policy sees it.
struct GroupViewer {
    rcs_viewer_id id = 0;
    uint32_t encoding = 0;              // Where it is (or is on its way to)...
    uint32_t layers = 1;                // ...and how many of its temporal layers it gets.
    uint32_t pinned = RCS_ENCODING_ANY; // rcs_viewer_video.encoding...
    uint32_t pinnedLayers = 0;          // ...layers...
    bool counts = true;                 // ...and counts_for_bitrate.
    bool main = false;
    uint32_t target = 0; // Its path's adaptive target; 0 while not measured (or over TCP).
    bool behind = false;  // Its video waits half a second to go out now...
    bool dropped = false; // ...or it dropped video a second behind since the last turn.
    uint32_t lossKeyframes = 0; // Keyframes it caused within the last minute, where others watch.
    bool decodesMain = true;    // The main encoding's codec, format and size...
    bool decodesLower = true;   // ...and a codec a lower encoding can be in.
};

// An encoding as the policy sees it: its encoder's target, what it sent over
// the last seconds (all streams together), its temporal layers and what the
// lowest of them sent (layerBps[k - 1]: layers 0..k - 1).
struct GroupEncoding {
    uint32_t bitrate = 0;
    uint64_t sentBps = 0;
    uint32_t layers = 1;
    std::array<uint64_t, 3> layerBps{};
};

struct GroupSettings {
    rcs_bitrate_from from = RCS_BITRATE_FROM_QUORUM;
    uint32_t quorumPercent = 50;
    uint32_t count = 2;           // Encodings a stream may have; 1 with SINGLE.
    uint32_t available = 1;       // Those a viewer may move to now, as (1u << encoding) bits (0 always).
    bool fixed = false;           // FIXED: count encodings run, used or not.
    uint32_t moveDownMs = 2000, moveUpMs = 10'000, lossKeyframesMove = 3;
    uint32_t minimum = 0, maximum = 0; // The bitrate's limits.
};

struct GroupMove {
    rcs_viewer_id viewer = 0;
    uint32_t from = 0, to = 0;             // Its encoding...
    uint32_t fromLayers = 1, toLayers = 1; // ...and the layers of it it gets.
    rcs_viewer_video_reason reason = RCS_VIEWER_VIDEO_PATH;
};

struct GroupTurn {
    std::vector<GroupMove> moves;
    // Each encoding's bitrate (0: as it is) and the viewer whose path holds it there.
    std::array<uint32_t, RCS_MAX_ENCODINGS> bitrate{};
    std::array<rcs_viewer_id, RCS_MAX_ENCODINGS> limiting{};
    uint32_t wanted = 1;        // The encodings to run, as (1u << encoding) bits.
    bool uplinkLimited = false; // Most of an encoding's viewers fell short together.
};

class GroupPlan {
public:
    // `viewers` with their encodings and layers as they are; the moves are
    // applied to them by the caller, who also forgets the viewers that left.
    GroupTurn turn(std::span<const GroupViewer> viewers, const std::array<GroupEncoding, RCS_MAX_ENCODINGS>& encodings,
                   const GroupSettings& settings, Clock::time_point now);
    void forget(rcs_viewer_id viewer) { tracks_.erase(viewer); }

private:
    // Since when a viewer falls short of what it gets, and since when it
    // would keep up with the step above.
    struct Track {
        std::optional<Clock::time_point> shortSince, upSince;
    };
    std::map<rcs_viewer_id, Track> tracks_;
};

// QUORUM's target: the highest of `targets` that at least `percent` of them
// reach; 0 for none.
uint32_t quorumTarget(std::vector<uint32_t> targets, uint32_t percent);

} // namespace rcsdk::session
