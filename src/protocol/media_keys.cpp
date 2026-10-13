#include "protocol/media_keys.hpp"

#include "util/bytes.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace rcsdk::protocol {
namespace {

constexpr std::string_view stepLabel = "rcsdk media step", sealLabel = "rcsdk media";

// Step `step + 1`'s key from step `step`'s.
crypto::Key nextStep(const crypto::Key& key) {
    const auto derived =
        crypto::hkdf(key, {}, std::span(reinterpret_cast<const uint8_t*>(stepLabel.data()), stepLabel.size()), 32);
    crypto::Key next{};
    std::copy(derived.begin(), derived.end(), next.begin());
    return next;
}

// What seals and opens under (generation, step).
std::unique_ptr<crypto::Aead> sealOf(const crypto::Key& key, uint32_t generation, uint32_t step) {
    Bytes info(sealLabel.begin(), sealLabel.end());
    appendBigEndian(info, generation, 4);
    appendBigEndian(info, step, 4);
    const auto material = crypto::hkdf(key, {}, info, 64);
    crypto::Key cipher{}, mac{};
    std::copy_n(material.begin(), 32, cipher.begin());
    std::copy_n(material.begin() + 32, 32, mac.begin());
    return std::make_unique<crypto::Aead>(cipher, mac);
}

uint8_t flagsOf(const EncodedPacket& packet) {
    return uint8_t((packet.keyframe ? SealedVideo::keyframeFlag : 0) |
                   (packet.discardable ? SealedVideo::discardableFlag : 0));
}

} // namespace

// ---- The sharer --------------------------------------------------------------------------

MediaSealer::MediaSealer() {
    MediaKey first;
    first.generation = 1;
    crypto::random(first.key);
    use(first);
}

void MediaSealer::use(const MediaKey& key) {
    current_ = key;
    steps_ = {{key.step, key.key}};
    aead_ = sealOf(current_.key, current_.generation, current_.step);
    counter_ = 0;
}

void MediaSealer::stepOn() {
    current_.key = nextStep(current_.key);
    ++current_.step;
    steps_[current_.step] = current_.key;
    while (steps_.size() > keptSteps) {
        steps_.erase(steps_.begin());
    }
    aead_ = sealOf(current_.key, current_.generation, current_.step);
    counter_ = 0;
}

MediaKey MediaSealer::join(std::optional<uint32_t> from) {
    std::lock_guard lock(mutex_);
    if (pending_) { // The newcomer gets no key of the generation the viewers before it are still taking.
        use(*std::exchange(pending_, std::nullopt));
    } else if (const auto held = from ? steps_.find(*from) : steps_.end(); held != steps_.end()) {
        return {current_.generation, held->first, held->second};
    }
    stepOn();
    return current_;
}

MediaKey MediaSealer::rekey() {
    std::lock_guard lock(mutex_);
    MediaKey next;
    next.generation = std::max(current_.generation, pending_ ? pending_->generation : 0u) + 1;
    crypto::random(next.key);
    pending_ = next;
    return next;
}

void MediaSealer::advance() {
    std::lock_guard lock(mutex_);
    if (pending_) {
        use(*std::exchange(pending_, std::nullopt));
    }
}

std::optional<MediaKey> MediaSealer::pending() const {
    std::lock_guard lock(mutex_);
    return pending_;
}

MediaKey MediaSealer::current() const {
    std::lock_guard lock(mutex_);
    return current_;
}

SealedVideo MediaSealer::seal(const EncodedPacket& packet, std::span<const uint8_t> message) {
    SealedVideo sealed;
    std::shared_ptr<const crypto::Aead> aead;
    {
        std::lock_guard lock(mutex_);
        if (packet.keyframe) {
            stepOn();
        }
        sealed.generation = current_.generation;
        sealed.step = current_.step;
        sealed.counter = counter_++;
        aead = aead_;
    }
    sealed.stream = uint8_t(packet.stream);
    sealed.encoding = uint8_t(packet.encoding);
    sealed.temporal = uint8_t(packet.temporalId);
    sealed.flags = flagsOf(packet);
    sealed.sealed = aead->seal(sealed.counter, sealedHeader(sealed), message);
    return sealed;
}

SealedAudio MediaSealer::seal(const AudioPacket&, std::span<const uint8_t> message, uint8_t sound) {
    SealedAudio sealed;
    std::shared_ptr<const crypto::Aead> aead;
    {
        std::lock_guard lock(mutex_);
        sealed.generation = current_.generation;
        sealed.step = current_.step;
        sealed.counter = counter_++;
        aead = aead_;
    }
    sealed.sound = sound;
    sealed.sealed = aead->seal(sealed.counter, sealedHeader(sealed), message);
    return sealed;
}

// ---- A viewer ----------------------------------------------------------------------------

bool MediaOpener::Replay::fresh(uint64_t counter) const {
    if (!any || counter > highest) {
        return true;
    }
    const uint64_t behind = highest - counter;
    return behind < window.size() * 64 && !(window[behind / 64] & (uint64_t(1) << (behind % 64)));
}

void MediaOpener::Replay::mark(uint64_t counter) {
    if (any && counter <= highest) {
        const uint64_t behind = highest - counter;
        window[behind / 64] |= uint64_t(1) << (behind % 64);
        return;
    }
    const uint64_t shift = any ? counter - highest : window.size() * 64;
    if (shift >= window.size() * 64) {
        window.fill(0);
    } else {
        // Move the window up by `shift` bits: bit b (b counters below the old highest) goes to b + shift.
        std::array<uint64_t, 64> moved{};
        for (size_t bit = 0; bit + shift < window.size() * 64; ++bit) {
            if (window[bit / 64] & (uint64_t(1) << (bit % 64))) {
                const size_t to = bit + size_t(shift);
                moved[to / 64] |= uint64_t(1) << (to % 64);
            }
        }
        window = moved;
    }
    window[0] |= 1; // The new highest itself.
    highest = counter;
    any = true;
}

bool MediaOpener::add(const MediaKey& key) {
    auto found = generations_.find(key.generation);
    if (found != generations_.end()) {
        if (key.step <= found->second.step) {
            return false;
        }
        found->second.step = key.step;
        found->second.key = key.key;
        return true;
    }
    if (!generations_.empty() && key.generation < generations_.rbegin()->first) {
        return false; // Older than the newest held.
    }
    Generation generation;
    generation.step = key.step;
    generation.key = key.key;
    generations_.emplace(key.generation, std::move(generation));
    while (generations_.size() > 2) { // The newest and the one before.
        generations_.erase(generations_.begin());
    }
    return true;
}

MediaOpener::Outcome MediaOpener::open(uint32_t generationNumber, uint32_t stepNumber, uint64_t counter,
                                       std::span<const uint8_t> header, std::span<const uint8_t> sealed,
                                       Clock::time_point now, const std::function<bool(Bytes&)>& take) {
    // A previous generation goes 2 s after a newer one opened something.
    std::erase_if(generations_,
                  [&](const auto& g) { return g.second.retired && now - *g.second.retired > previousKept; });
    const auto found = generations_.find(generationNumber);
    if (found == generations_.end()) {
        return Outcome::unkeyed;
    }
    auto& generation = found->second;
    // The step's seal: one that opened something; else from the key held, or
    // one it went past, or derived on from the newest, which moves there only
    // once something under it opened (a header made up on the way moves
    // nothing). A step older than every kept one stays shut: its counters
    // are no longer known.
    Step derived;
    Step* step = nullptr;
    std::vector<std::pair<uint32_t, crypto::Key>> passed; // Deriving on: the steps on the way.
    crypto::Key derivedKey{};
    if (const auto opened = generation.steps.find(stepNumber); opened != generation.steps.end()) {
        step = &opened->second;
    } else if (generation.steps.size() >= keptSteps && stepNumber < generation.steps.begin()->first) {
        return Outcome::unkeyed;
    } else if (stepNumber == generation.step) {
        derived.aead = sealOf(generation.key, generationNumber, stepNumber);
    } else if (const auto before = generation.before.find(stepNumber); before != generation.before.end()) {
        derived.aead = sealOf(before->second, generationNumber, stepNumber);
    } else if (stepNumber > generation.step && stepNumber - generation.step <= maxStepsAhead) {
        derivedKey = generation.key;
        for (uint32_t s = generation.step; s < stepNumber; ++s) {
            if (stepNumber - s <= keptSteps) {
                passed.emplace_back(s, derivedKey);
            }
            derivedKey = nextStep(derivedKey);
        }
        derived.aead = sealOf(derivedKey, generationNumber, stepNumber);
    } else {
        return Outcome::unkeyed; // Sealed before this end came, or implausibly far on.
    }
    if (!step) {
        step = &derived;
    }
    if (!step->replay.fresh(counter)) {
        return Outcome::replayed;
    }
    auto plain = step->aead->open(counter, header, sealed);
    if (!plain || !take(*plain)) {
        return Outcome::rejected;
    }
    if (stepNumber > generation.step) {
        generation.before.insert(passed.begin(), passed.end());
        generation.key = derivedKey;
        generation.step = stepNumber;
        while (generation.before.size() > keptSteps) {
            generation.before.erase(generation.before.begin());
        }
    }
    if (step == &derived) {
        step = &(generation.steps[stepNumber] = std::move(derived));
        while (generation.steps.size() > keptSteps) { // A step that goes can no longer be opened.
            generation.before.erase(generation.steps.begin()->first);
            generation.steps.erase(generation.steps.begin());
        }
    }
    step->replay.mark(counter);
    if (generationNumber != opened_.generation) {
        for (auto& [number, older] : generations_) {
            if (number < generationNumber && !older.retired) {
                older.retired = now;
            }
        }
    }
    opened_ = {generationNumber, stepNumber};
    return Outcome::opened;
}

MediaOpener::Outcome MediaOpener::open(const SealedVideo& sealed, Clock::time_point now, EncodedPacket& packet) {
    return open(sealed.generation, sealed.step, sealed.counter, sealedHeader(sealed), sealed.sealed, now,
                [&](Bytes& plain) {
                    try {
                        auto message = decode(plain);
                        auto* opened = std::get_if<EncodedPacket>(&message);
                        if (!opened || opened->stream != sealed.stream || opened->encoding != sealed.encoding ||
                            opened->temporalId != sealed.temporal || flagsOf(*opened) != sealed.flags) {
                            return false;
                        }
                        packet = std::move(*opened);
                        return true;
                    } catch (const Failure&) {
                        return false; // Sealed, but no picture this end takes: as if changed on the way.
                    }
                });
}

MediaOpener::Outcome MediaOpener::open(const SealedAudio& sealed, Clock::time_point now, AudioPacket& packet) {
    return open(sealed.generation, sealed.step, sealed.counter, sealedHeader(sealed), sealed.sealed, now,
                [&](Bytes& plain) {
                    try {
                        auto message = decode(plain);
                        auto* opened = std::get_if<AudioPacket>(&message);
                        if (!opened) {
                            return false;
                        }
                        packet = std::move(*opened);
                        return true;
                    } catch (const Failure&) {
                        return false;
                    }
                });
}

MediaKeyAck MediaOpener::newest() const {
    if (generations_.empty()) {
        return {};
    }
    const auto& [number, generation] = *generations_.rbegin();
    return {number, generation.step};
}

} // namespace rcsdk::protocol
