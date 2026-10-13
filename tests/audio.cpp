// Audio codecs (Opus, AAC, PCM) round trip a tone; concealment fills gaps;
// packets fit a datagram at any bitrate; the playback buffer absorbs jitter
// and clock drift without underruns and keeps its delay bounded, also with
// device pulls larger than its limit; audio messages encode and validate.
#include "audio/codec.hpp"
#include "audio/playout.hpp"
#include "platform/platform.hpp"
#include "protocol/messages.hpp"
#include "support/check.hpp"
#include "support/test_tone.hpp"

#include <rcsdk/rcsdk.hpp>

#include <algorithm>
#include <cmath>
#include <random>

using namespace rcsdk;

namespace {

std::vector<float> tone(uint64_t first, uint32_t frames) {
    std::vector<float> samples(size_t(frames) * audioChannels);
    for (uint32_t i = 0; i < frames; ++i) {
        const auto value = float(test::toneAmplitude * std::sin(2 * 3.14159265358979 * test::toneHz *
                                                                 double(first + i) / audioRate));
        samples[i * 2] = samples[i * 2 + 1] = value;
    }
    return samples;
}

std::vector<float> left(const std::vector<float>& stereo, size_t from) {
    std::vector<float> mono;
    for (size_t i = from * audioChannels; i < stereo.size(); i += audioChannels) {
        mono.push_back(stereo[i]);
    }
    return mono;
}

// One second of tone through the codec: the same tone comes out.
void roundTrip(rcs_audio_codec codec, uint32_t bitrate) {
    auto encoder = audio::createEncoder(codec, bitrate);
    auto decoder = audio::createDecoder({codec, bitrate, encoder->frames(), encoder->config()});
    std::vector<float> decoded;
    size_t packets = 0, bytes = 0;
    for (uint64_t frame = 0; frame < audioRate; frame += encoder->frames()) {
        for (const auto& packet : encoder->encode(tone(frame, encoder->frames()))) {
            CHECK(!packet.empty() && packet.size() <= 1024);
            ++packets;
            bytes += packet.size();
            decoder->decode(packet, decoded);
        }
    }
    CHECK(packets >= audioRate / encoder->frames() - 3); // Codec look-ahead may hold back a few.
    const auto measured = test::measureTone(left(decoded, audioRate / 4)); // After the codec's warm-up.
    std::printf("%s (%s): %zu packets, %zu kbit/s, %.1f Hz, RMS %.3f\n", audioCodecName(codec),
                encoder->name().c_str(), packets, bytes * 8 / 1000, measured.hz, measured.rms);
    CHECK(std::abs(measured.hz - test::toneHz) < 10);
    CHECK(std::abs(measured.rms - test::toneAmplitude / std::sqrt(2.0)) < 0.05);
    // Concealment fills exactly what was asked.
    const size_t before = decoded.size();
    decoder->conceal(2 * encoder->frames(), decoded);
    CHECK(decoded.size() - before == size_t(2) * encoder->frames() * audioChannels);
    // Opus packets follow what capture delivers: 2.5 to 20 ms each.
    if (codec == RCS_AUDIO_OPUS) {
        for (const uint32_t frames : {480u, 240u, 120u}) {
            CHECK(encoder->next(frames) == frames && encoder->next(frames + 50) == frames);
            const size_t start = decoded.size();
            for (const auto& packet : encoder->encode(tone(0, frames))) {
                decoder->decode(packet, decoded);
            }
            CHECK(decoded.size() - start == size_t(frames) * audioChannels);
        }
        CHECK(encoder->next(100) == 0);
    }
    // A corrupt packet is refused rather than played. (PCM accepts any even length.)
    if (codec == RCS_AUDIO_PCM) {
        CHECK(throws<Failure>([&] { decoder->decode(std::vector<uint8_t>{1, 2, 3}, decoded); }));
    }
}

// Simulated time: packets of `packetFrames` arrive with up to `jitterMs` of
// delay; the device pulls 512 frames at its own clock, `ppm` faster than the
// sharer's. A receiver whose radio naps `napMs` every `napEveryMs` gets what
// came meanwhile when it wakes.
struct PlayoutRun {
    uint64_t underruns = 0, maxBuffered = 0, dropped = 0, concealed = 0, adjustments = 0;
    uint64_t peakBuffered = 0; // From the first pull on.
    uint32_t target = 0, peakTarget = 0;
};
PlayoutRun simulate(uint32_t packetFrames, double jitterMs, double ppm, double seconds, uint32_t maxDelayMs = 120,
                    uint32_t devicePeriod = 512, double napEveryMs = 0, double napMs = 0) {
    audio::Playout playout(maxDelayMs, 2000);
    std::mt19937 random(7);
    std::uniform_real_distribution<double> jitter(0, jitterMs * 1e6);
    const double packetNs = packetFrames * 1e9 / audioRate;
    const double pullNs = devicePeriod * 1e9 / audioRate / (1 + ppm * 1e-6);
    std::vector<float> out(devicePeriod * audioChannels);
    uint64_t sent = 0, pulls = 0;
    double nextPacket = 0, nextPull = 50e6;
    std::vector<std::pair<double, uint64_t>> inFlight; // Arrival time, first frame.
    PlayoutRun run;
    while (nextPull < seconds * 1e9) {
        while (nextPacket <= nextPull) {
            double arrival = nextPacket + packetNs + jitter(random);
            if (napEveryMs > 0) {
                const double into = std::fmod(arrival, napEveryMs * 1e6);
                arrival += into < napMs * 1e6 ? napMs * 1e6 - into : 0;
            }
            inFlight.emplace_back(arrival, sent);
            sent += packetFrames;
            nextPacket += packetNs;
        }
        std::sort(inFlight.begin(), inFlight.end());
        while (!inFlight.empty() && inFlight.front().first <= nextPull) {
            const auto [arrival, first] = inFlight.front();
            inFlight.erase(inFlight.begin());
            playout.push(tone(first, packetFrames), uint64_t(first * 1e9 / audioRate), uint64_t(arrival));
        }
        playout.pull(out.data(), devicePeriod);
        ++pulls;
        nextPull += pullNs;
        const auto c = playout.counters();
        if (nextPull > 4e9) { // After the first seconds (the buffer assumes 40 ms of jitter for three).
            run.maxBuffered = std::max<uint64_t>(run.maxBuffered, c.bufferedFrames);
        }
        run.peakBuffered = std::max<uint64_t>(run.peakBuffered, c.bufferedFrames);
        run.peakTarget = std::max(run.peakTarget, c.targetFrames);
    }
    const auto c = playout.counters();
    run.underruns = c.underruns;
    run.dropped = c.droppedFrames;
    run.concealed = c.concealedFrames;
    run.target = c.targetFrames;
    run.adjustments = c.adjustments;
    return run;
}

void playout() {
    // Wi-Fi saving power at the receiver: 150 ms naps every 3 s. The buffer
    // remembers the naps between them: it runs dry at the first one only.
    auto naps = simulate(480, 2, 0, 40, 200, 512, 3000, 150);
    std::printf("150 ms naps every 3 s: target %u ms, %llu underruns, %llu ms dropped\n",
                naps.target * 1000 / audioRate, (unsigned long long)naps.underruns,
                (unsigned long long)(naps.dropped * 1000 / audioRate));
    CHECK(naps.underruns <= 2 && naps.target * 1000 / audioRate >= 150);
    // Longer naps further apart than the buffers remember (120 ms every 12
    // s): the jitter the first showed is held, the others find the sound there.
    auto rare = simulate(480, 2, 0, 90, 200, 512, 12000, 120);
    std::printf("120 ms naps every 12 s: target %u ms, %llu underruns, %llu ms dropped\n",
                rare.target * 1000 / audioRate, (unsigned long long)rare.underruns,
                (unsigned long long)(rare.dropped * 1000 / audioRate));
    CHECK(rare.underruns <= 2);
    // A steady LAN: a small buffer, no underruns.
    auto lan = simulate(480, 1, 0, 30);
    std::printf("LAN: target %u ms, max %llu ms, %llu underruns\n", lan.target * 1000 / audioRate,
                (unsigned long long)(lan.maxBuffered * 1000 / audioRate), (unsigned long long)lan.underruns);
    CHECK(lan.underruns == 0 && lan.maxBuffered * 1000 / audioRate <= 22);
    // Wi-Fi-like jitter: the target follows it, still no underruns.
    auto wifi = simulate(480, 25, 0, 30);
    std::printf("25 ms jitter: target %u ms, %llu underruns\n", wifi.target * 1000 / audioRate,
                (unsigned long long)wifi.underruns);
    CHECK(wifi.underruns <= 1 && wifi.target * 1000 / audioRate >= 30);
    // AAC's 21 ms packets.
    CHECK(simulate(1024, 2, 0, 30).underruns == 0);
    // Clock drift either way over ten minutes: corrected without underruns,
    // the delay stays bounded.
    for (const double ppm : {300.0, -300.0}) {
        const auto drift = simulate(480, 2, ppm, 600);
        std::printf("%+.0f ppm: %llu underruns, max %llu ms, dropped %llu, repeated %llu frames\n", ppm,
                    (unsigned long long)drift.underruns, (unsigned long long)(drift.maxBuffered * 1000 / audioRate),
                    (unsigned long long)drift.dropped, (unsigned long long)drift.concealed);
        CHECK(drift.underruns == 0);
        CHECK(drift.maxBuffered * 1000 / audioRate <= 25);
    }
    // Drift corrections stay rare at any device period and packet size:
    // each one is a short cross-fade, audible as a warble when frequent.
    for (const uint32_t period : {144u, 256u, 480u, 512u}) {
        for (const uint32_t packet : {240u, 480u, 960u}) {
            for (const double ppm : {0.0, 100.0, -100.0}) {
                const auto run = simulate(packet, 2, ppm, 120, 120, period);
                const double perMinute = double(run.adjustments) / 2;
                std::printf("period %3u, packet %3u, %+4.0f ppm: %.1f adjustments per minute, %llu underruns, max %llu ms\n",
                            period, packet, ppm, perMinute, (unsigned long long)run.underruns,
                            (unsigned long long)(run.maxBuffered * 1000 / audioRate));
                CHECK(perMinute <= (ppm == 0 ? 2 : 8) && run.underruns <= 2); // At 0 ppm: settling after the start.
            }
        }
    }
    // A device taking more at a time than max_delay_ms holds (21 and 85 ms
    // pulls, 20 ms allowed): the buffer holds one pull and a packet, and no
    // pull reads sound that has not arrived (an earlier buffer did: its level
    // wrapped around to billions of frames and playback stayed garbage).
    for (const uint32_t pull : {1024u, 4096u}) {
        const auto large = simulate(480, 1, 0, 30, 20, pull);
        std::printf("%u-frame pulls, 20 ms max: target %u, peak %llu frames, %llu underruns\n", pull, large.peakTarget,
                    (unsigned long long)large.peakBuffered, (unsigned long long)large.underruns);
        CHECK(large.peakTarget <= pull + 480 && large.peakBuffered <= pull + 480);
        CHECK(large.underruns <= 2);
    }
    // Sound that piled up before playback started is not played late.
    audio::Playout late(120, 2000);
    std::vector<float> pulled(512 * audioChannels);
    for (uint64_t i = 0; i < 10; ++i) { // 100 ms waiting before the device's first pull.
        late.push(tone(i * 480, 480), i * 10'000'000, 100'000'000);
    }
    late.pull(pulled.data(), 512);
    CHECK(late.counters().bufferedFrames <= late.counters().targetFrames);
    // A burst far above max_delay_ms is cut back to the target at once.
    audio::Playout bounded(60, 2000);
    std::vector<float> out(512 * audioChannels);
    for (uint64_t i = 0; i < 100; ++i) { // One second arrives at once.
        bounded.push(tone(i * 480, 480), i * 10'000'000, 1'000'000'000);
    }
    bounded.pull(out.data(), 512);
    bounded.pull(out.data(), 512);
    CHECK(bounded.counters().bufferedFrames * 1000 / audioRate <= 60);
    CHECK(bounded.counters().droppedFrames > audioRate / 2);
}

// AAC is the system's (not in a browser, nor on Linux).
bool hasAac() {
    const auto caps = rcsdk::capabilities();
    return (caps.audio_encoders & caps.audio_decoders & (1u << RCS_AUDIO_AAC)) != 0;
}

// Loud noise at each codec's highest bitrate: every packet fits the one
// datagram it travels in (Opus 20 ms packets at 510 kbit/s would take 1275 bytes).
void packetLimit() {
    std::mt19937 random(5);
    std::uniform_real_distribution<float> noise(-1, 1);
    const auto aac = platform::aacBitrates();
    const struct {
        rcs_audio_codec codec;
        uint32_t bitrate, frames;
    } cases[] = {{RCS_AUDIO_OPUS, 510000, 960},
                 {RCS_AUDIO_OPUS, 510000, 480},
                 {RCS_AUDIO_AAC, aac.empty() ? 320000u : aac.back(), aacFrames},
                 {RCS_AUDIO_PCM, 0, pcmFrames}};
    for (const auto& c : cases) {
        if (c.codec == RCS_AUDIO_AAC && !hasAac()) {
            continue;
        }
        auto encoder = audio::createEncoder(c.codec, c.bitrate);
        std::vector<float> samples(size_t(c.frames) * audioChannels);
        size_t largest = 0;
        for (int i = 0; i < 250; ++i) {
            for (auto& sample : samples) {
                sample = noise(random);
            }
            for (const auto& packet : encoder->encode(samples)) {
                largest = std::max(largest, packet.size());
            }
        }
        std::printf("%s at %u bit/s, %u frames per packet: largest %zu bytes\n", audioCodecName(c.codec), c.bitrate,
                    c.frames, largest);
        CHECK(largest > 0 && largest <= maxAudioPacketBytes);
    }
}

// AAC runs at the bitrate asked for, or refuses it: where any rate in range
// is taken (macOS), an uncommon one is applied exactly, and one beyond the
// encoder's range fails as UNSUPPORTED.
void aacBitrate() {
    if (!platform::aacBitrates().empty()) {
        return; // Windows: fixed rates, checked by the session's configuration.
    }
    std::mt19937 random(11);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    auto encoder = audio::createEncoder(RCS_AUDIO_AAC, 100'000);
    std::vector<float> samples(size_t(aacFrames) * audioChannels);
    size_t bytes = 0, packets = 0;
    for (int i = 0; i < 300; ++i) {
        for (auto& sample : samples) {
            sample = noise(random);
        }
        for (const auto& packet : encoder->encode(samples)) {
            bytes += packet.size();
            ++packets;
        }
    }
    const double bitrate = double(bytes) * 8 * audioRate / aacFrames / double(packets);
    std::printf("AAC asked for 100 kbit/s: %.1f kbit/s\n", bitrate / 1000);
    CHECK(std::abs(bitrate - 100'000) < 5'000);
    try {
        audio::createEncoder(RCS_AUDIO_AAC, 330'000);
        CHECK(false);
    } catch (const Failure& failure) {
        CHECK(failure.result() == RCS_ERROR_UNSUPPORTED);
    }
}

void messages() {
    using namespace rcsdk::protocol;
    // Each way its own codecs, preferred first, and whether it flows.
    const auto offer = std::get<CodecOffer>(decode(encode(CodecOffer{{RCS_CODEC_HEVC},
                                                                     {{3840, 2160}},
                                                                     {defaultPictureFormats},
                                                                     {RCS_AUDIO_OPUS, RCS_AUDIO_PCM},
                                                                     {RCS_AUDIO_PCM},
                                                                     true,
                                                                     false})));
    CHECK(offer.codecs.size() == 1 && offer.sendAudio.size() == 2 && offer.sendAudio[1] == RCS_AUDIO_PCM);
    CHECK(offer.receiveAudio == std::vector<rcs_audio_codec>{RCS_AUDIO_PCM} && offer.sendsAudio &&
          !offer.receivesAudio);
    CHECK(std::get<CodecOffer>(decode(encode(CodecOffer{{RCS_CODEC_H264}, {{1920, 1080}}, {defaultPictureFormats}})))
              .sendAudio.empty()); // No codecs.
    const auto format =
        std::get<AudioFormat>(decode(encode(AudioFormat{RCS_AUDIO_AAC, 128000, aacFrames, {0x11, 0x90}})));
    CHECK(format.codec == RCS_AUDIO_AAC && format.framesPerPacket == aacFrames && format.config.size() == 2);
    const auto packet = std::get<AudioPacket>(decode(encode(AudioPacket{7, opusFrames, 99, {1, 2, 3}})));
    CHECK(packet.sequence == 7 && packet.frames == opusFrames && packet.timestampNs == 99 && packet.data.size() == 3 &&
          packet.previous.empty() && !packet.previousFrames);
    // With the one before once more (redundancy).
    const auto both = std::get<AudioPacket>(decode(encode(AudioPacket{8, opusFrames, 99, {4, 5}, 480, {1, 2, 3}})));
    CHECK((both.data == std::vector<uint8_t>{4, 5} && both.previousFrames == 480 &&
           both.previous == std::vector<uint8_t>{1, 2, 3}));
    // Malformed copies: frames without data, data without frames, both beyond a datagram.
    CHECK(throws<Failure>([] { decode(encode(AudioPacket{8, 480, 0, {1}, 480, {}})); }));
    CHECK(throws<Failure>([] { decode(encode(AudioPacket{8, 480, 0, {1}, 0, {2}})); }));
    CHECK(throws<Failure>([] {
        decode(encode(AudioPacket{8, 480, 0, std::vector<uint8_t>(600), 480, std::vector<uint8_t>(600)}));
    }));
    // Malformed: unknown codec, wrong packet duration, empty or oversized data.
    CHECK(throws<Failure>([] { decode(encode(AudioFormat{9, 128000, 480, {}})); }));
    CHECK(throws<Failure>([] { decode(encode(AudioFormat{RCS_AUDIO_OPUS, 128000, 1024, {}})); }));
    CHECK(throws<Failure>([] { decode(encode(AudioPacket{1, 480, 0, {}})); }));
    CHECK(throws<Failure>([] { decode(encode(AudioPacket{1, 480, 0, std::vector<uint8_t>(2000)})); }));
    CHECK(throws<Failure>([] {
        decode(encode(
            CodecOffer{{RCS_CODEC_H264}, {{1920, 1080}}, {defaultPictureFormats}, {RCS_AUDIO_OPUS, RCS_AUDIO_OPUS}}));
    }));
}

} // namespace

int main() {
    return runTest("audio", [] {
        roundTrip(RCS_AUDIO_OPUS, 128000);
        roundTrip(RCS_AUDIO_OPUS, 32000);
        roundTrip(RCS_AUDIO_PCM, 0);
        if (hasAac()) {
            roundTrip(RCS_AUDIO_AAC, 128000);
            aacBitrate();
        } else {
            std::printf("No AAC on this system: its round trip skipped\n");
        }
        packetLimit();
        playout();
        messages();
    });
}
