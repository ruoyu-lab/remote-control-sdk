// Group sharing's pure parts (GROUP-SHARING-DESIGN): the policy that places
// viewers on a stream's encodings and their temporal layers and sets their
// bitrates (session/group_plan.hpp), the temporal layer a bitstream says
// (media/layers.hpp), and pictures made smaller for a lower encoding
// (media/scale.hpp, and the platform's surfaces where it has them).
#include "media/layers.hpp"
#include "media/scale.hpp"
#include "platform/platform.hpp"
#include "session/group_plan.hpp"
#include "support/check.hpp"

#include <numeric>

using namespace rcsdk;
using namespace rcsdk::session;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr uint32_t M = 1'000'000;

GroupViewer viewer(rcs_viewer_id id, uint32_t target, uint32_t encoding = 0) {
    GroupViewer v;
    v.id = id;
    v.target = target;
    v.encoding = encoding;
    v.decodesLower = true;
    return v;
}

// The main encoding sending `sent` at `bitrate`, the lower one `lowerSent`.
std::array<GroupEncoding, RCS_MAX_ENCODINGS> sending(uint64_t sent, uint32_t bitrate, uint64_t lowerSent = 0,
                                                     uint32_t lowerBitrate = 0) {
    return {GroupEncoding{bitrate, sent}, GroupEncoding{lowerBitrate, lowerSent}, GroupEncoding{}};
}

GroupSettings settings(rcs_bitrate_from from = RCS_BITRATE_FROM_QUORUM) {
    GroupSettings s;
    s.from = from;
    s.count = 2;
    s.available = 0b11;
    s.minimum = 500'000;
    s.maximum = 20 * M;
    return s;
}

// The design's examples (section 4): who moves, and each encoding's bitrate.
void quorum() {
    CHECK(quorumTarget({20 * M, 8 * M}, 50) == 20 * M);
    std::vector<uint32_t> nine(9, 10 * M);
    auto slow = nine;
    slow.push_back(5 * M);
    CHECK(quorumTarget(slow, 50) == 10 * M);
    auto fast = nine;
    fast.push_back(20 * M);
    CHECK(quorumTarget(fast, 50) == 10 * M);
    std::vector<uint32_t> halves{4 * M, 4 * M, 4 * M, 4 * M, 4 * M, 20 * M, 20 * M, 20 * M, 20 * M, 20 * M};
    CHECK(quorumTarget(halves, 50) == 20 * M && quorumTarget(halves, 60) == 4 * M);
    CHECK(quorumTarget(halves, 100) == 4 * M && quorumTarget(halves, 1) == 20 * M && !quorumTarget({}, 50));

    const auto t0 = Clock::now();
    // 20 and 8 Mbit/s, content that uses the bitrate: the 8 falls short and,
    // after move_down_ms, gets the lower encoding, whose bitrate is its own.
    {
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 8 * M)};
        auto turn = plan.turn(group, sending(20 * M, 20 * M), settings(), t0);
        CHECK(turn.moves.empty() && turn.bitrate[0] == 20 * M && turn.limiting[0] == 1 && !turn.uplinkLimited);
        CHECK(plan.turn(group, sending(20 * M, 20 * M), settings(), t0 + 1900ms).moves.empty());
        turn = plan.turn(group, sending(20 * M, 20 * M), settings(), t0 + 2s);
        CHECK(turn.moves.size() == 1 && turn.moves[0].viewer == 2 && turn.moves[0].to == 1 &&
              turn.moves[0].reason == RCS_VIEWER_VIDEO_PATH);
        CHECK(turn.bitrate[0] == 20 * M && turn.bitrate[1] == 8 * M && turn.limiting[1] == 2 && turn.wanted == 0b11);
    }
    // The same paths with a desktop that sends 3 Mbit/s: both carry it, no one moves.
    {
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 8 * M)};
        for (auto at = t0; at < t0 + 5s; at += 100ms) {
            const auto turn = plan.turn(group, sending(3 * M, 20 * M), settings(), at);
            CHECK(turn.moves.empty() && turn.wanted == 1);
        }
    }
    // Nine at 10 and one at 5: the nine keep 10, the one moves.
    {
        GroupPlan plan;
        std::vector<GroupViewer> group;
        for (rcs_viewer_id id = 1; id <= 9; ++id) {
            group.push_back(viewer(id, 10 * M));
        }
        group.push_back(viewer(10, 5 * M));
        plan.turn(group, sending(10 * M, 10 * M), settings(), t0);
        const auto turn = plan.turn(group, sending(10 * M, 10 * M), settings(), t0 + 2s);
        CHECK(turn.bitrate[0] == 10 * M && turn.moves.size() == 1 && turn.moves[0].viewer == 10);
    }
    // Nine at 10 and one at 20: all at 10, no one moves (the 20 is the few).
    {
        GroupPlan plan;
        std::vector<GroupViewer> group;
        for (rcs_viewer_id id = 1; id <= 9; ++id) {
            group.push_back(viewer(id, 10 * M));
        }
        group.push_back(viewer(10, 20 * M));
        plan.turn(group, sending(10 * M, 10 * M), settings(), t0);
        const auto turn = plan.turn(group, sending(10 * M, 10 * M), settings(), t0 + 3s);
        CHECK(turn.bitrate[0] == 10 * M && turn.moves.empty());
    }
    // Five at 4 and five at 20: the main encoding at 20 and the five move; at
    // 60% all stay at 4.
    {
        std::vector<GroupViewer> group;
        for (rcs_viewer_id id = 1; id <= 10; ++id) {
            group.push_back(viewer(id, id <= 5 ? 4 * M : 20 * M));
        }
        GroupPlan plan;
        plan.turn(group, sending(20 * M, 20 * M), settings(), t0);
        auto turn = plan.turn(group, sending(20 * M, 20 * M), settings(), t0 + 2s);
        CHECK(turn.bitrate[0] == 20 * M && turn.bitrate[1] == 4 * M && turn.moves.size() == 5);
        auto sixty = settings();
        sixty.quorumPercent = 60;
        GroupPlan stays;
        stays.turn(group, sending(4 * M, 4 * M), sixty, t0);
        turn = stays.turn(group, sending(4 * M, 4 * M), sixty, t0 + 3s);
        CHECK(turn.bitrate[0] == 4 * M && turn.moves.empty());
    }
}

// MINIMUM and MAIN, and QUORUM where no lower encoding can run.
void modes() {
    const auto t0 = Clock::now();
    std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 8 * M)};
    {
        GroupPlan plan;
        plan.turn(group, sending(20 * M, 8 * M), settings(RCS_BITRATE_FROM_MINIMUM), t0);
        const auto turn = plan.turn(group, sending(20 * M, 8 * M), settings(RCS_BITRATE_FROM_MINIMUM), t0 + 3s);
        CHECK(turn.bitrate[0] == 8 * M && turn.limiting[0] == 2 && turn.moves.empty());
    }
    {
        auto main = group;
        main[0].main = true;
        GroupPlan plan;
        plan.turn(main, sending(20 * M, 20 * M), settings(RCS_BITRATE_FROM_MAIN), t0);
        const auto turn = plan.turn(main, sending(20 * M, 20 * M), settings(RCS_BITRATE_FROM_MAIN), t0 + 2s);
        CHECK(turn.bitrate[0] == 20 * M && turn.limiting[0] == 1 && turn.moves.size() == 1);
    }
    for (const uint32_t available : {1u, 0b01u}) { // SINGLE (count 1), or the lower one refused.
        auto single = settings();
        single.count = available == 1 ? 1 : 2;
        single.available = available;
        GroupPlan plan;
        plan.turn(group, sending(20 * M, 20 * M), single, t0);
        const auto turn = plan.turn(group, sending(20 * M, 20 * M), single, t0 + 3s);
        CHECK(turn.bitrate[0] == 8 * M && turn.moves.empty()); // As MINIMUM.
    }
    // Bitrates stay within the limits.
    std::vector<GroupViewer> slow{viewer(1, 100'000)};
    CHECK(GroupPlan().turn(slow, sending(0, 0), settings(), t0).bitrate[0] == 500'000);
}

// Most of an encoding's viewers short of it together: the sharer's uplink,
// no one moves; a drop moves at once; back up after move_up_ms.
void moves() {
    const auto t0 = Clock::now();
    {
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 3 * M), viewer(2, 3 * M), viewer(3, 3 * M), viewer(4, 12 * M)};
        for (auto at = t0; at <= t0 + 4s; at += 100ms) {
            const auto turn = plan.turn(group, sending(10 * M, 10 * M), settings(), at);
            CHECK(turn.moves.empty() && turn.uplinkLimited);
        }
    }
    {
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 18 * M)};
        group[1].dropped = true;
        const auto turn = plan.turn(group, sending(5 * M, 20 * M), settings(), t0);
        CHECK(turn.moves.size() == 1 && turn.moves[0].viewer == 2 && turn.moves[0].reason == RCS_VIEWER_VIDEO_PATH);
    }
    {
        // Back up: its path carries what the main encoding sends with a quarter
        // to spare (or the main one's target), for move_up_ms on end.
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 6 * M, 1)};
        auto s = settings();
        CHECK(plan.turn(group, sending(4 * M, 20 * M, 2 * M, 6 * M), s, t0).moves.empty());
        CHECK(plan.turn(group, sending(4 * M, 20 * M, 2 * M, 6 * M), s, t0 + 9900ms).moves.empty());
        const auto up = plan.turn(group, sending(4 * M, 20 * M, 2 * M, 6 * M), s, t0 + 10s);
        CHECK(up.moves.size() == 1 && up.moves[0].to == 0 && up.moves[0].reason == RCS_VIEWER_VIDEO_PATH);
        GroupPlan short_;
        std::vector<GroupViewer> heavy{viewer(1, 20 * M), viewer(2, 6 * M, 1)};
        for (auto at = t0; at <= t0 + 12s; at += 1s) { // The main one sends 18: 6 is not enough.
            CHECK(short_.turn(heavy, sending(18 * M, 20 * M, 2 * M, 6 * M), s, at).moves.empty());
        }
    }
    {
        // Alone it drags no one: it stays, falling short or dropping; alone
        // on a lower encoding with no one on the main one, it goes back at once.
        GroupPlan plan;
        std::vector<GroupViewer> alone{viewer(1, 2 * M)};
        alone[0].dropped = true;
        for (auto at = t0; at <= t0 + 3s; at += 1s) {
            CHECK(plan.turn(alone, sending(10 * M, 10 * M), settings(), at).moves.empty());
        }
        std::vector<GroupViewer> below{viewer(2, 2 * M, 1)};
        const auto turn = plan.turn(below, sending(10 * M, 10 * M), settings(), t0);
        CHECK(turn.moves.size() == 1 && turn.moves[0].to == 0 && turn.moves[0].reason == RCS_VIEWER_VIDEO_PATH);
    }
    {
        // Its decoder: off the main encoding at once, and only where it decodes a lower one.
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 0), viewer(3, 0)};
        group[1].decodesMain = false;
        group[2].decodesMain = false;
        group[2].decodesLower = false;
        const auto turn = plan.turn(group, sending(5 * M, 20 * M), settings(), t0);
        CHECK(turn.moves.size() == 1 && turn.moves[0].viewer == 2 && turn.moves[0].to == 1 &&
              turn.moves[0].reason == RCS_VIEWER_VIDEO_DECODER);
    }
    {
        // The host's choice, losses, an encoding no longer available.
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 20 * M), viewer(3, 20 * M), viewer(4, 6 * M, 1)};
        group[0].pinned = 1;
        group[1].lossKeyframes = 3;
        auto turn = plan.turn(group, sending(5 * M, 20 * M), settings(), t0);
        CHECK(turn.moves.size() == 2 && turn.moves[0].reason == RCS_VIEWER_VIDEO_HOST &&
              turn.moves[1].reason == RCS_VIEWER_VIDEO_LOSS && turn.moves[1].to == 1);
        auto refused = settings();
        refused.available = 1;
        turn = plan.turn(group, sending(5 * M, 20 * M), refused, t0);
        CHECK(turn.moves.size() == 1 && turn.moves[0].viewer == 4 && turn.moves[0].to == 0 &&
              turn.moves[0].reason == RCS_VIEWER_VIDEO_ENCODING);
    }
    {
        // FIXED runs its encodings unused; a viewer that holds no one back
        // sets no main bitrate, but a lower one's alone on it.
        auto fixed = settings();
        fixed.fixed = true;
        std::vector<GroupViewer> group{viewer(1, 10 * M), viewer(2, 2 * M), viewer(3, 3 * M, 1)};
        group[1].counts = false;
        group[2].counts = false;
        const auto turn = GroupPlan().turn(group, sending(1 * M, 10 * M, 1 * M, 3 * M), fixed, Clock::now());
        CHECK(turn.wanted == 0b11 && turn.bitrate[0] == 10 * M && turn.bitrate[1] == 3 * M);
    }
}

// The main encoding in two temporal layers, `base` of what it sends in layer 0.
std::array<GroupEncoding, RCS_MAX_ENCODINGS> layered(uint64_t sent, uint32_t bitrate, double base,
                                                     uint64_t lowerSent = 0, uint32_t lowerBitrate = 0) {
    auto encodings = sending(sent, bitrate, lowerSent, lowerBitrate);
    encodings[0].layers = 2;
    encodings[0].layerBps = {uint64_t(double(sent) * base), sent, sent};
    return encodings;
}

// A viewer that falls short of a layered encoding gets its lower layer first
// (at once, the same encoding), then the encoding below; up again the same
// steps; the layers it gets hold the bitrate to what they need of it.
void layeredSteps() {
    const auto t0 = Clock::now();
    const auto is = [](const GroupMove& m, rcs_viewer_id id, uint32_t to, uint32_t toLayers,
                       rcs_viewer_video_reason reason) {
        return m.viewer == id && m.to == to && m.toLayers == toLayers && m.reason == reason;
    };
    {
        // 20, 20 and 4 Mbit/s; the main encoding sends 8, half of it in layer 0.
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 20 * M), viewer(3, 4 * M)};
        for (auto& v : group) {
            v.layers = 2;
        }
        CHECK(plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(), t0).moves.empty());
        auto turn = plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(), t0 + 2s);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 3, 0, 1, RCS_VIEWER_VIDEO_PATH) &&
              turn.moves[0].from == 0 && turn.moves[0].fromLayers == 2 && turn.wanted == 1);
        group[2].layers = 1; // Layer 0 alone: 4 of 8 Mbit/s, which it carries.
        for (auto at = t0 + 2s; at <= t0 + 5s; at += 500ms) {
            CHECK(plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(), at).moves.empty());
        }
        // Heavier content (16, 8 in layer 0): the encoding below.
        CHECK(plan.turn(group, layered(16 * M, 20 * M, 0.5), settings(), t0 + 6s).moves.empty());
        turn = plan.turn(group, layered(16 * M, 20 * M, 0.5), settings(), t0 + 8s);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 3, 1, 1, RCS_VIEWER_VIDEO_PATH) && turn.wanted == 0b11);
        group[2].encoding = 1;
        // Its path clears: layer 0 of the main encoding, then both.
        group[2].target = 20 * M;
        CHECK(plan.turn(group, layered(16 * M, 20 * M, 0.5, 2 * M, 4 * M), settings(), t0 + 10s).moves.empty());
        turn = plan.turn(group, layered(16 * M, 20 * M, 0.5, 2 * M, 4 * M), settings(), t0 + 20s);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 3, 0, 1, RCS_VIEWER_VIDEO_PATH));
        group[2].encoding = 0;
        CHECK(plan.turn(group, layered(16 * M, 20 * M, 0.5), settings(), t0 + 21s).moves.empty());
        turn = plan.turn(group, layered(16 * M, 20 * M, 0.5), settings(), t0 + 31s);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 3, 0, 2, RCS_VIEWER_VIDEO_PATH));
    }
    {
        // MINIMUM: no path moves; one that gets layer 0 alone (the host's
        // say) holds the bitrate at what makes its share fit its path: twice
        // its 4 Mbit/s where layer 0 takes half.
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 4 * M)};
        group[0].layers = 2;
        group[1].layers = 1;
        group[1].pinnedLayers = 1;
        const auto turn = plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(RCS_BITRATE_FROM_MINIMUM), t0);
        CHECK(turn.moves.empty() && turn.bitrate[0] == 8 * M && turn.limiting[0] == 2);
        // Without layers, its own 4.
        group[1].layers = 2;
        group[1].pinnedLayers = 0;
        CHECK(plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(RCS_BITRATE_FROM_MINIMUM), t0).bitrate[0] ==
              4 * M);
    }
    {
        // The host's layers: one at once (HOST); all an encoding has where it has fewer.
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 20 * M)};
        group[0].layers = group[1].layers = 2;
        group[1].pinnedLayers = 1;
        auto turn = plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(), t0);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 2, 0, 1, RCS_VIEWER_VIDEO_HOST));
        group[1].pinnedLayers = 3;
        group[1].layers = 1;
        turn = plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(), t0);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 2, 0, 2, RCS_VIEWER_VIDEO_HOST));
        group[1].layers = 2;
        // One layer: nothing to leave out.
        CHECK(plan.turn(group, sending(8 * M, 20 * M), settings(), t0).moves.empty());
        // Its layers fixed, its encoding still follows its path.
        GroupPlan path;
        std::vector<GroupViewer> pinned{viewer(1, 20 * M), viewer(2, 20 * M), viewer(3, 3 * M)};
        for (auto& v : pinned) {
            v.layers = 2;
        }
        pinned[2].layers = 1;
        pinned[2].pinnedLayers = 1;
        path.turn(pinned, layered(16 * M, 20 * M, 0.5), settings(), t0);
        turn = path.turn(pinned, layered(16 * M, 20 * M, 0.5), settings(), t0 + 2s);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 3, 1, 1, RCS_VIEWER_VIDEO_PATH));
    }
    {
        // Alone, it gets all of its encoding at once; losses move it to the
        // encoding below, not to fewer layers (its keyframes would still be
        // the others'); the sharer's uplink moves no one's layers either.
        GroupPlan plan;
        std::vector<GroupViewer> alone{viewer(1, 2 * M)};
        alone[0].layers = 1;
        auto turn = plan.turn(alone, layered(8 * M, 20 * M, 0.5), settings(), t0);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 1, 0, 2, RCS_VIEWER_VIDEO_PATH));
        std::vector<GroupViewer> lossy{viewer(1, 20 * M), viewer(2, 20 * M)};
        lossy[0].layers = lossy[1].layers = 2;
        lossy[1].lossKeyframes = 3;
        turn = plan.turn(lossy, layered(8 * M, 20 * M, 0.5), settings(), t0);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 2, 1, 1, RCS_VIEWER_VIDEO_LOSS));
        GroupPlan uplink;
        std::vector<GroupViewer> group{viewer(1, 3 * M), viewer(2, 3 * M), viewer(3, 3 * M), viewer(4, 12 * M)};
        for (auto& v : group) {
            v.layers = 2;
        }
        for (auto at = t0; at <= t0 + 4s; at += 100ms) {
            turn = uplink.turn(group, layered(10 * M, 10 * M, 0.5), settings(), at);
            CHECK(turn.moves.empty() && turn.uplinkLimited);
        }
    }
    {
        // Pinned to its encoding, its layers still follow its path.
        GroupPlan plan;
        std::vector<GroupViewer> group{viewer(1, 20 * M), viewer(2, 3 * M)};
        group[0].layers = group[1].layers = 2;
        group[1].pinned = 0;
        plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(), t0);
        const auto turn = plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(), t0 + 2s);
        CHECK(turn.moves.size() == 1 && is(turn.moves[0], 2, 0, 1, RCS_VIEWER_VIDEO_PATH));
        group[1].layers = 1; // Still short at layer 0 (4 of 8): it stays, pinned.
        for (auto at = t0 + 2s; at <= t0 + 6s; at += 1s) {
            CHECK(plan.turn(group, layered(8 * M, 20 * M, 0.5), settings(), at).moves.empty());
        }
    }
}

Bytes units(std::initializer_list<Bytes> list) {
    Bytes out;
    for (const auto& unit : list) {
        nal::appendUnit(out, unit);
    }
    return out;
}

// The temporal layer and whether a picture is discardable, as far as its
// bitstream says; the encoder says the rest.
void layers() {
    using Layer = TemporalLayer;
    EncodedPacket h264;
    h264.codec = RCS_CODEC_H264;
    h264.data = units({{0x65, 1, 2}}); // An IDR slice, nal_ref_idc 3.
    CHECK((temporalLayer(h264) == Layer{std::nullopt, false}));
    h264.data = units({{0x01, 1, 2}}); // A slice nothing refers to.
    CHECK((temporalLayer(h264) == Layer{std::nullopt, true}));
    h264.data = units({{0x01, 1, 2}, {0x21, 1, 2}}); // One of its slices referred to.
    CHECK(!*temporalLayer(h264).discardable);
    h264.data = units({{0x6E, 0x80, 0, 2 << 5}, {0x01, 1}}); // An SVC prefix: temporal_id 2.
    CHECK((temporalLayer(h264) == Layer{2, true}));
    h264.data = units({{0x6E, 0x00, 0, 2 << 5}, {0x41, 1}}); // Not an SVC extension: no layer.
    CHECK((temporalLayer(h264) == Layer{std::nullopt, false}));

    EncodedPacket hevc;
    hevc.codec = RCS_CODEC_HEVC;
    hevc.data = units({{0 << 1, 2, 9}}); // TRAIL_N, layer 1.
    CHECK((temporalLayer(hevc) == Layer{1, true}));
    hevc.data = units({{1 << 1, 1, 9}}); // TRAIL_R, layer 0.
    CHECK((temporalLayer(hevc) == Layer{0, false}));
    hevc.data = units({{3 << 1, 2, 9}}); // TSA_R, layer 1 (VideoToolbox's upper layer).
    CHECK((temporalLayer(hevc) == Layer{1, false}));
    hevc.data = units({{19 << 1, 1, 9}}); // IDR_W_RADL.
    CHECK((temporalLayer(hevc) == Layer{0, false}));
    hevc.data = units({{1 << 1, 0, 9}}); // nuh_temporal_id_plus1 0.
    CHECK(throws<Failure>([&] { temporalLayer(hevc); }));

    EncodedPacket av1;
    av1.codec = RCS_CODEC_AV1;
    av1.data = {(6 << 3) | 0x06, 1 << 5, 1, 0}; // A frame OBU with an extension: temporal_id 1.
    CHECK((temporalLayer(av1) == Layer{1, std::nullopt}));
    av1.data = {(6 << 3) | 0x02, 1, 0};
    CHECK((temporalLayer(av1) == Layer{0, std::nullopt}));

    // VP9: whether the frame refreshes a slot (refresh_frame_flags); a
    // keyframe always does. The frame marker, profile 0, an inter frame
    // shown, error resilient, then refresh_frame_flags.
    EncodedPacket vp9;
    vp9.codec = RCS_CODEC_VP9;
    vp9.data = {0x87, 0x00, 0x00}; // 10 0 0 0 1 1 1, refresh_frame_flags 0.
    CHECK((temporalLayer(vp9) == Layer{std::nullopt, true}));
    vp9.data = {0x87, 0x02, 0x00}; // refresh_frame_flags 0x02.
    CHECK((temporalLayer(vp9) == Layer{std::nullopt, false}));

    EncodedPacket vp8;
    vp8.codec = RCS_CODEC_VP8;
    vp8.data = {1, 2, 3};
    CHECK((temporalLayer(vp8) == Layer{}));

    // What a packet said is replaced by what the bitstream says (standard
    // codecs), and that is told; what it does not say stays.
    h264.data = units({{0x01, 1, 2}});
    h264.temporalId = 1;
    h264.discardable = false;
    CHECK(readTemporalLayer(h264) && h264.temporalId == 1 && h264.discardable);
    CHECK(!readTemporalLayer(h264));
    hevc.data = units({{0 << 1, 2, 9}});
    hevc.temporalId = 3;
    CHECK(readTemporalLayer(hevc) && hevc.temporalId == 1 && hevc.discardable);
    vp8.temporalId = 2;
    vp8.discardable = true;
    CHECK(!readTemporalLayer(vp8) && vp8.temporalId == 2 && vp8.discardable);
    EncodedPacket own;
    own.codec = RCS_CODEC_HOST_FIRST;
    own.temporalId = 2;
    own.discardable = true;
    CHECK(!readTemporalLayer(own) && own.temporalId == 2 && own.discardable);
    CHECK(temporalLayerProblem(7, 1).empty() && !temporalLayerProblem(8, 0).empty() &&
          !temporalLayerProblem(0, 2).empty());

    // A recovery in an upper layer: the next picture of each layer below it says so too.
    LayeredRecovery recovery;
    const auto next = [&](uint32_t layer, std::optional<uint64_t> recovers = std::nullopt, bool keyframe = false) {
        EncodedPacket p;
        p.temporalId = layer;
        p.recoversAfter = recovers;
        p.keyframe = keyframe;
        recovery.apply(p);
        return p.recoversAfter;
    };
    CHECK(!next(0, std::nullopt, true) && next(2, 52) == 52u && !next(2) && next(1) == 52u && !next(2) && !next(1) &&
          next(0) == 52u && !next(0) && !next(1));
    CHECK(next(1, 60) == 60u && !next(0, std::nullopt, true) && !next(0)); // A keyframe serves all.
    CHECK(next(0, 70) == 70u && !next(0)); // One in layer 0 serves all.
}

// CPU pictures made smaller: each pixel the mean of those it covers.
void scaling() {
    VideoFrame frame;
    frame.width = 4;
    frame.height = 2;
    frame.stride = 20; // Rows with padding.
    auto pixels = std::make_shared<std::vector<uint8_t>>(40, 0);
    for (uint32_t y = 0; y < 2; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            uint8_t* p = pixels->data() + y * 20 + x * 4;
            p[0] = uint8_t(x * 10 + y * 100); // Blue.
            p[1] = 200;
            p[2] = uint8_t(x < 2 ? 0 : 255);
            p[3] = 255;
        }
    }
    frame.pixels = pixels;
    frame.sequence = 9;
    const auto scaled = scaleBgra(frame, 2, 1);
    CHECK(scaled.width == 2 && scaled.height == 1 && scaled.stride == 8 && scaled.sequence == 9);
    const auto& out = *scaled.pixels;
    CHECK(out[0] == 55 && out[1] == 200 && out[2] == 0 && out[3] == 255);    // (0 + 10 + 100 + 110) / 4
    CHECK(out[4] == 75 && out[5] == 200 && out[6] == 255 && out[7] == 255);  // (20 + 30 + 120 + 130) / 4
    const auto odd = scaleBgra(frame, 3, 2); // Not a whole divisor: each takes at least one.
    CHECK(odd.width == 3 && odd.pixels->size() == 3 * 2 * 4);
}

} // namespace

int main() {
    return runTest("group", [] {
        quorum();
        modes();
        moves();
        layeredSteps();
        layers();
        scaling();
    });
}
