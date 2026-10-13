// View side: bounded queue between the network reader and the decoder.
//
// Compressed pictures depend on earlier ones, so the queue never drops a single
// packet. When it overflows or its oldest packet is too old it drops the whole
// backlog, discards packets until the next keyframe, asks the sharer for one
// and marks that keyframe to reset the decoder. A picture the network lost
// (lost()) leaves what came before it in place: the queue discards what
// follows until a keyframe or a picture that refers to nothing after the last
// one it kept (EncodedPacket::recoversAfter), and asks the sharer for that.
// Not thread-safe.
#pragma once

#include "media/video.hpp"

#include <algorithm>
#include <deque>
#include <optional>
#include <utility>

namespace rcsdk::session {

// rcs_decoder_config's queue limits.
struct ReceiveLimits {
    uint32_t frames = 64;
    uint32_t maxAgeMs = 80; // 0: no age limit.
};

class ReceiveQueue {
public:
    struct Item {
        EncodedPacket packet;
        uint64_t arrivalNs = 0;
        bool resetDecoder = false;
        bool afterGap = false; // Packets before it were lost or dropped: decoding goes on here.
    };
    // What to ask the sharer for: a picture that refers to nothing after
    // `after` (protocol::RefLost), or without it a keyframe.
    struct Request {
        std::optional<uint64_t> after;
    };
    // A lost picture got over: how, and how long the picture stood still
    // (from the arrival of the last picture before the loss).
    struct Resumed {
        rcs_recovery_method method = RCS_RECOVERY_KEYFRAME;
        uint64_t frozenNs = 0;
    };

    explicit ReceiveQueue(const ReceiveLimits& limits) : limits_(limits) {}

    // New limits apply from the next push or pop.
    void setLimits(const ReceiveLimits& limits) noexcept { limits_ = limits; }

    void push(EncodedPacket packet, uint64_t nowNs) {
        if (items_.size() >= limits_.frames) {
            overflow(nowNs);
        }
        if (waiting_ != Waiting::nothing) {
            const bool leavesOut = waiting_ == Waiting::recovery && packet.recoversAfter && kept_ &&
                                   *packet.recoversAfter <= *kept_;
            if (!packet.keyframe && !leavesOut) {
                ++dropped_;
                ask(nowNs, false);
                return;
            }
            if (lostNs_) {
                resumed_ = Resumed{rcs_recovery_method(packet.keyframe ? RCS_RECOVERY_KEYFRAME : RCS_RECOVERY_REFERENCES),
                                   nowNs - *lostNs_};
                lostNs_.reset();
            }
            const bool reset = waiting_ == Waiting::keyframe;
            waiting_ = Waiting::nothing;
            kept_ = packet.sequence;
            keptNs_ = nowNs;
            items_.push_back({std::move(packet), nowNs, reset, true});
            return;
        }
        kept_ = packet.sequence;
        keptNs_ = nowNs;
        items_.push_back({std::move(packet), nowNs, false, false});
    }

    std::optional<Item> pop(uint64_t nowNs) {
        if (items_.empty()) {
            return std::nullopt;
        }
        const uint64_t since = std::max(items_.front().arrivalNs, ageOrigin_);
        if (limits_.maxAgeMs && nowNs - since > uint64_t(limits_.maxAgeMs) * 1'000'000) {
            overflow(nowNs);
            return std::nullopt;
        }
        Item item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    // The network lost a picture after the last one pushed.
    void lost(uint64_t nowNs) {
        if (!lostNs_) {
            lostNs_ = kept_ ? keptNs_ : nowNs;
        }
        const bool fresh = waiting_ == Waiting::nothing;
        if (fresh) {
            waiting_ = kept_ ? Waiting::recovery : Waiting::keyframe;
        }
        ask(nowNs, fresh);
    }

    // Drops the backlog and waits for the next keyframe, as on overflow; used
    // when the decoder had to be replaced.
    void restartAtKeyframe(uint64_t nowNs) { dropBacklog(nowNs); }

    // What waited until now is not late for the age limit: the decoder took
    // the time (a phone's opens its codec on the first packet and takes over
    // 100 ms for its first picture), not the path; the backlog decodes in a
    // few milliseconds.
    void forgiveWaiting(uint64_t nowNs) noexcept { ageOrigin_ = nowNs; }

    // The stream ended: what waits goes, and nothing is asked for; a stream
    // that starts again begins with a keyframe. The counts stay.
    void clear() noexcept {
        items_.clear();
        waiting_ = Waiting::nothing;
        kept_.reset();
        lostNs_.reset();
        resumed_.reset();
        requested_ = false;
    }

    // Once after each request (at most every 200 ms while it is not answered).
    std::optional<Request> takeRequest() noexcept {
        if (!std::exchange(requested_, false) || waiting_ == Waiting::nothing) {
            return std::nullopt;
        }
        return Request{waiting_ == Waiting::recovery ? kept_ : std::nullopt};
    }
    // Once after a lost picture was got over.
    std::optional<Resumed> takeResumed() noexcept { return std::exchange(resumed_, std::nullopt); }

    size_t size() const noexcept { return items_.size(); }
    uint64_t dropped() const noexcept { return dropped_; }
    uint64_t recoveries() const noexcept { return recoveries_; } // Requests sent.
    uint64_t overflows() const noexcept { return overflows_; }   // Backlogs dropped for size or age.
    bool empty() const noexcept { return items_.empty(); }

private:
    enum class Waiting { nothing, recovery, keyframe };

    void overflow(uint64_t nowNs) {
        ++overflows_;
        dropBacklog(nowNs);
    }

    void dropBacklog(uint64_t nowNs) {
        dropped_ += items_.size();
        items_.clear();
        waiting_ = Waiting::keyframe;
        ask(nowNs, false);
    }

    void ask(uint64_t nowNs, bool now) {
        if (now || nowNs - lastRequestNs_ >= 200'000'000) {
            lastRequestNs_ = nowNs;
            requested_ = true;
            ++recoveries_;
        }
    }

    ReceiveLimits limits_;
    std::deque<Item> items_;
    Waiting waiting_ = Waiting::nothing;
    std::optional<uint64_t> kept_;   // The sequence of the last packet kept...
    uint64_t keptNs_ = 0;            // ...and its arrival.
    std::optional<uint64_t> lostNs_; // Where the picture stood still since a loss, until it is got over.
    std::optional<Resumed> resumed_;
    bool requested_ = false;
    uint64_t lastRequestNs_ = 0;
    uint64_t dropped_ = 0;
    uint64_t recoveries_ = 0;
    uint64_t overflows_ = 0;
    uint64_t ageOrigin_ = 0; // Waiting before it does not count (forgiveWaiting).
};

} // namespace rcsdk::session
