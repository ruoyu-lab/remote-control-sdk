// The stream key of a run through a forwarder (RCS_TRANSPORT_FORWARDER): the
// sharer seals each picture and each packet of its sound once for every
// viewer it let in, the viewers open them, and the forwarder reads only the
// headers (messages.hpp, SealedVideo and SealedAudio), which the seal covers.
//
// Keys: a generation's key is 32 random bytes; step s + 1's is
// HKDF(step s's, info "rcsdk media step"), so whoever holds a step derives
// every later one of its generation and no earlier one. What is sealed under
// (generation, step): cipher and MAC keys HKDF(key, info "rcsdk media" ||
// generation || step), crypto::Aead with the counter as its sequence and the
// header (sealedHeader, its message type first) as associated data, over the
// picture's EncodedPacket message or the packet's AudioPacket message. One
// counter per key, whatever the stream, encoding, picture or sound: no (key,
// counter) seals twice.
//
// The sharer (MediaSealer) steps on at each keyframe, of any stream and
// encoding, so that a step starts with a keyframe. A viewer let in gets the
// key of the step of the newest keyframes of what it takes, when the
// forwarder still keeps them (it starts at once from them, and opens what was
// sealed from there: at most the forwarder's cache time before it came), or
// else of a step on from then (it opens nothing sealed before it came); the
// others derive the steps from the headers. When a viewer leaves (or the
// rotation is due) the sharer starts a generation, hands the key to the
// viewers that stay and seals with it once they all acknowledged it, or a
// second passed; the one that left holds no key of it. A viewer (MediaOpener)
// keeps the newest generation it holds and the one before, the latter for 2 s
// once the newer opened something (what was sealed before the change still
// on its way), the keys of the latest steps of each (what other streams
// sealed under a step it already went past may still come), and takes each
// counter of a key once.
#pragma once

#include "protocol/messages.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace rcsdk::protocol {

class MediaSealer {
public:
    static constexpr uint32_t keptSteps = 128; // Of the generation sealing now, what join() may give.

    MediaSealer(); // Generation 1 at step 0, a key at random.

    // A viewer is let in: it gets the key of step `from` of the generation
    // sealing now where that is one of the last keptSteps, or else of a step
    // on from now (completing a generation change that waited first).
    MediaKey join(std::optional<uint32_t> from = std::nullopt);
    // A new generation for the viewers that stay, sealed with once advance()
    // says so; one that waited for that is replaced.
    MediaKey rekey();
    // The generation rekey() made seals from now on.
    void advance();
    // The generation waiting for its viewers' acknowledgements, if any.
    std::optional<MediaKey> pending() const;
    // The key sealing now, for a viewer that missed it.
    MediaKey current() const;
    // `packet` as `message` (its encode()), sealed; a keyframe a step on.
    // `sound`: which of the sharer's sounds a packet of sound is of. Any thread.
    SealedVideo seal(const EncodedPacket& packet, std::span<const uint8_t> message);
    SealedAudio seal(const AudioPacket& packet, std::span<const uint8_t> message, uint8_t sound);

private:
    void stepOn();                 // Holding the mutex.
    void use(const MediaKey& key); // Holding the mutex: seals with `key` from now on.

    mutable std::mutex mutex_;
    MediaKey current_;
    std::optional<MediaKey> pending_;
    std::map<uint32_t, crypto::Key> steps_;    // The current generation's latest steps' keys.
    std::shared_ptr<const crypto::Aead> aead_; // current_'s.
    uint64_t counter_ = 0;                     // The next under current_.
};

class MediaOpener {
public:
    static constexpr uint32_t maxStepsAhead = 1024; // What a header may name past the newest step held.
    // Of each generation, below the newest: what other streams and encodings
    // sealed under steps gone past, and what a forwarder kept for a move
    // (keyframes of every stream and encoding step on, a few a second).
    static constexpr uint32_t keptSteps = 128;
    static constexpr auto previousKept = 2s; // A generation's keys once a newer one opened something.

    enum class Outcome { opened, rejected, replayed, unkeyed };
    // A key the sharer sent; one older than what it has is no news (false).
    bool add(const MediaKey& key);
    // `sealed` opened into `packet` (opened), or why not: it does not verify
    // or its header does not say what it seals (rejected), its counter came
    // before (replayed), no key of its generation, or of a step before the
    // ones held (unkeyed).
    Outcome open(const SealedVideo& sealed, Clock::time_point now, EncodedPacket& packet);
    Outcome open(const SealedAudio& sealed, Clock::time_point now, AudioPacket& packet);
    // The newest key held (generation 0: none yet), what a MediaKeyAck says.
    MediaKeyAck newest() const;
    // The key that last opened something.
    MediaKeyAck opened() const { return opened_; }

private:
    // Counters taken under one key: the highest, and a window below it.
    struct Replay {
        uint64_t highest = 0;
        bool any = false;
        std::array<uint64_t, 64> window{}; // 4096 counters below the highest, a bit each.
        bool fresh(uint64_t counter) const;
        void mark(uint64_t counter);
    };
    struct Step {
        std::unique_ptr<crypto::Aead> aead;
        Replay replay;
    };
    struct Generation {
        uint32_t step = 0;                        // The latest step held...
        crypto::Key key{};                        // ...and its key.
        std::map<uint32_t, crypto::Key> before;   // Keys of steps below it it went past.
        std::map<uint32_t, Step> steps;           // The seals of steps that opened something.
        std::optional<Clock::time_point> retired; // A newer generation opened something then.
    };
    // Opens `sealed` under (generation, step) and hands the plaintext to
    // `take`, which says whether it is what the header says it is.
    Outcome open(uint32_t generation, uint32_t step, uint64_t counter, std::span<const uint8_t> header,
                 std::span<const uint8_t> sealed, Clock::time_point now, const std::function<bool(Bytes&)>& take);

    std::map<uint32_t, Generation> generations_;
    MediaKeyAck opened_;
};

} // namespace rcsdk::protocol
