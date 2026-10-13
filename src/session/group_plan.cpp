#include "session/group_plan.hpp"

#include <algorithm>
#include <functional>
#include <tuple>

namespace rcsdk::session {

uint32_t quorumTarget(std::vector<uint32_t> targets, uint32_t percent) {
    if (targets.empty()) {
        return 0;
    }
    std::sort(targets.begin(), targets.end(), std::greater<>());
    const size_t reaching = (targets.size() * percent + 99) / 100; // At least this many reach it.
    return targets[std::clamp<size_t>(reaching, 1, targets.size()) - 1];
}

GroupTurn GroupPlan::turn(std::span<const GroupViewer> viewers,
                          const std::array<GroupEncoding, RCS_MAX_ENCODINGS>& encodings, const GroupSettings& s,
                          Clock::time_point now) {
    GroupTurn turn;
    const auto usable = [&](uint32_t encoding) { return !encoding || (s.available & (1u << encoding)); };
    // The next encoding below `encoding` a viewer may move to, and the next above.
    const auto below = [&](uint32_t encoding) -> std::optional<uint32_t> {
        for (uint32_t next = encoding + 1; next < std::min(s.count, RCS_MAX_ENCODINGS); ++next) {
            if (usable(next)) {
                return next;
            }
        }
        return std::nullopt;
    };
    const auto above = [&](uint32_t encoding) {
        uint32_t next = encoding;
        while (next && !usable(--next)) {
        }
        return next;
    };
    const auto layersOf = [&](uint32_t encoding) { return std::clamp(encodings[encoding].layers, 1u, 3u); };
    // The share of an encoding's bitrate its lowest `layers` take, as they
    // sent it (all of it before anything was measured), at least a quarter.
    const auto share = [&](uint32_t encoding, uint32_t layers) {
        const auto& e = encodings[encoding];
        if (layers >= layersOf(encoding) || !e.sentBps || !e.layerBps[layers - 1]) {
            return 1.0;
        }
        return std::clamp(double(e.layerBps[layers - 1]) / double(e.sentBps), 0.25, 1.0);
    };
    // What a path must carry to keep up with `layers` of an encoding: what
    // they sent, never more than their share of its target (the budget lets
    // it overshoot a little).
    const auto load = [&](uint32_t encoding, uint32_t layers) {
        const auto& e = encodings[encoding];
        const uint64_t all = e.bitrate ? std::min<uint64_t>(e.sentBps, e.bitrate) : e.sentBps;
        return uint64_t(double(all) * share(encoding, layers));
    };
    const auto shortOfLoad = [&](const GroupViewer& v, uint32_t layers) {
        return v.target && uint64_t(v.target) * 10 < load(v.encoding, layers) * 9;
    };

    // The layers each gets of its encoding as it is now.
    std::vector<uint32_t> got(viewers.size());
    for (size_t i = 0; i < viewers.size(); ++i) {
        got[i] = std::clamp(viewers[i].layers, 1u, layersOf(std::min(viewers[i].encoding, RCS_MAX_ENCODINGS - 1)));
    }

    // The sharer's uplink: most of an encoding's measured viewers short of it together.
    std::array<uint32_t, RCS_MAX_ENCODINGS> measured{}, shortOf{}, on{};
    for (size_t i = 0; i < viewers.size(); ++i) {
        const auto& v = viewers[i];
        ++on[std::min(v.encoding, RCS_MAX_ENCODINGS - 1)];
        if (v.counts && v.target && v.encoding < RCS_MAX_ENCODINGS) {
            ++measured[v.encoding];
            shortOf[v.encoding] += shortOfLoad(v, got[i]);
        }
    }
    std::array<bool, RCS_MAX_ENCODINGS> uplink{};
    for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
        uplink[encoding] = shortOf[encoding] >= 2 && shortOf[encoding] * 2 > measured[encoding];
        turn.uplinkLimited = turn.uplinkLimited || uplink[encoding];
    }

    const bool paths = s.from != RCS_BITRATE_FROM_MINIMUM;
    std::vector<std::pair<uint32_t, uint32_t>> placed(viewers.size()); // Encoding, layers.
    for (size_t i = 0; i < viewers.size(); ++i) {
        const auto& v = viewers[i];
        auto& track = tracks_[v.id];
        const bool encodingMoves = v.pinned == RCS_ENCODING_ANY, layerMoves = !v.pinnedLayers;
        // All the layers of an encoding it may get.
        const auto full = [&](uint32_t encoding) {
            return layerMoves ? layersOf(encoding) : std::min(v.pinnedLayers, layersOf(encoding));
        };
        uint32_t to = v.encoding, toLayers = got[i];
        rcs_viewer_video_reason reason = RCS_VIEWER_VIDEO_PATH;
        const auto lower = v.decodesLower ? below(0) : std::nullopt;
        if (!usable(v.encoding)) {
            to = above(v.encoding);
            reason = RCS_VIEWER_VIDEO_ENCODING;
        } else if (!encodingMoves) {
            to = std::min(v.pinned, std::min(s.count, RCS_MAX_ENCODINGS) - 1);
            to = usable(to) ? to : v.encoding;
            reason = RCS_VIEWER_VIDEO_HOST;
            if (!to && !v.decodesMain && lower) {
                to = *lower;
                reason = RCS_VIEWER_VIDEO_DECODER;
            } else if (to && !v.decodesLower && v.decodesMain) {
                to = 0;
                reason = RCS_VIEWER_VIDEO_DECODER;
            }
        } else if (!v.encoding && !v.decodesMain) {
            to = lower.value_or(0);
            reason = RCS_VIEWER_VIDEO_DECODER;
        } else if (v.encoding && !v.decodesLower && v.decodesMain) {
            to = 0;
            reason = RCS_VIEWER_VIDEO_DECODER;
        }
        if (to != v.encoding) {
            toLayers = full(to);
        } else if (got[i] != full(to) && !layerMoves) {
            toLayers = full(to); // The host's layers.
            reason = RCS_VIEWER_VIDEO_HOST;
        } else {
            // Its path: a step down while it falls short (a layer fewer, else
            // the encoding below), a step up once it carries the step above;
            // the host's layers stay. Alone on its encoding it drags no one:
            // its path sets that one's bitrate, and it gets all of it.
            reason = RCS_VIEWER_VIDEO_PATH;
            const bool alone = on[v.encoding] <= 1;
            std::optional<std::pair<uint32_t, uint32_t>> down;
            const auto encodingBelow = encodingMoves && v.decodesLower && !alone ? below(v.encoding) : std::nullopt;
            if (!alone && layerMoves && got[i] > 1) {
                down = std::pair{v.encoding, got[i] - 1};
            } else if (encodingBelow) {
                down = std::pair{*encodingBelow, full(*encodingBelow)};
            }
            const bool falling = v.behind || v.dropped || shortOfLoad(v, got[i]);
            if (s.lossKeyframesMove && v.lossKeyframes >= s.lossKeyframesMove && encodingBelow) {
                to = *encodingBelow;
                toLayers = full(to);
                reason = RCS_VIEWER_VIDEO_LOSS;
            } else if (paths && falling && !uplink[v.encoding]) {
                track.shortSince = track.shortSince.value_or(now);
                if (down && (v.dropped || now - *track.shortSince >= std::chrono::milliseconds(s.moveDownMs))) {
                    std::tie(to, toLayers) = *down;
                }
            } else {
                track.shortSince.reset();
            }
            if (to == v.encoding && toLayers == got[i]) {
                std::optional<std::pair<uint32_t, uint32_t>> up;
                if (got[i] < full(v.encoding)) {
                    up = std::pair{v.encoding, alone ? full(v.encoding) : got[i] + 1};
                } else if (encodingMoves && v.encoding) {
                    const uint32_t next = above(v.encoding);
                    up = std::pair{next, on[next] && layerMoves ? 1u : full(next)};
                }
                const bool decodes = up && (up->first || v.decodesMain);
                const bool empty = up && up->first != v.encoding && !on[up->first];
                if (decodes && ((alone && up->first == v.encoding) || empty)) {
                    std::tie(to, toLayers) = *up; // At once: no one else gets what it goes to.
                } else if (decodes) {
                    const uint64_t target = v.target;
                    const auto& e = encodings[up->first];
                    const double nominal = e.bitrate * share(up->first, up->second);
                    const bool carries = target && !falling &&
                                         (target * 4 >= load(up->first, up->second) * 5 ||
                                          (e.bitrate && double(target) * 20 >= nominal * 19));
                    if (!carries) {
                        track.upSince.reset();
                    } else if (track.upSince = track.upSince.value_or(now);
                               now - *track.upSince >= std::chrono::milliseconds(s.moveUpMs)) {
                        std::tie(to, toLayers) = *up;
                    }
                }
            }
        }
        if (to != v.encoding || toLayers != got[i]) {
            turn.moves.push_back({v.id, v.encoding, to, got[i], toLayers, reason});
            track = {};
        }
        placed[i] = {to, toLayers};
    }

    // Each encoding's bitrate: the main one's by bitrate_from (QUORUM as
    // MINIMUM where no lower encoding can take who falls short), the lower
    // ones' their slowest viewer's. A viewer that gets some of the layers
    // holds the encoding to what makes those fit its path.
    for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
        std::vector<std::pair<uint32_t, rcs_viewer_id>> counted;
        uint32_t lowest = 0, main = 0, others = 0;
        rcs_viewer_id lowestBy = 0, mainBy = 0, othersBy = 0;
        for (size_t i = 0; i < viewers.size(); ++i) {
            const auto& v = viewers[i];
            if (placed[i].first != encoding || !v.target) {
                continue;
            }
            const uint32_t target =
                uint32_t(std::min<double>(UINT32_MAX, v.target / share(encoding, placed[i].second)));
            if (!v.counts) {
                if (!othersBy || target < others) {
                    others = target;
                    othersBy = v.id;
                }
                continue;
            }
            counted.push_back({target, v.id});
            if (!lowestBy || target < lowest) {
                lowest = target;
                lowestBy = v.id;
            }
            if (v.main) {
                main = target;
                mainBy = v.id;
            }
        }
        uint32_t bitrate = lowest;
        rcs_viewer_id by = lowestBy;
        if (!encoding && s.from == RCS_BITRATE_FROM_MAIN && mainBy) {
            bitrate = main;
            by = mainBy;
        } else if (!encoding && s.from == RCS_BITRATE_FROM_QUORUM && below(0)) {
            std::vector<uint32_t> targets;
            for (const auto& [target, id] : counted) {
                targets.push_back(target);
            }
            bitrate = quorumTarget(targets, s.quorumPercent);
            for (const auto& [target, id] : counted) {
                if (target == bitrate) {
                    by = id;
                    break;
                }
            }
        } else if (encoding && !lowestBy) { // Viewers that hold no one back still set a lower encoding's.
            bitrate = others;
            by = othersBy;
        }
        if (bitrate) {
            turn.bitrate[encoding] = std::clamp(bitrate, s.minimum, std::max(s.minimum, s.maximum));
            turn.limiting[encoding] = by;
        }
    }
    for (const auto& [encoding, layers] : placed) {
        turn.wanted |= 1u << encoding;
    }
    if (s.fixed) {
        turn.wanted |= ((1u << std::min(s.count, RCS_MAX_ENCODINGS)) - 1) & (s.available | 1u);
    }
    return turn;
}

} // namespace rcsdk::session
