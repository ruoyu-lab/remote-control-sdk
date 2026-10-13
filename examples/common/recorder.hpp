// Records what a viewer receives from the encoded packets the SDK hands over
// (RCS_EXT_ENCODED_OUTPUT), without decoding or encoding anything: the
// picture as an elementary stream and the sound in a plain file of its codec,
// each on one timeline that starts with the first picture written.
//
//   PREFIX.h264, PREFIX.h265  H.264, HEVC: Annex B (start codes), the
//                             parameter sets before each keyframe
//   PREFIX.obu                AV1: an OBU stream (a temporal delimiter before
//                             each picture, the sequence header before each
//                             keyframe)
//   <picture file>.timestamps each picture's time in ms from the first, one
//                             per line (mkvmerge's "timestamp format v2")
//   PREFIX.ivf                VP8, VP9: IVF (a 32-byte file header, then each
//                             frame with its size and its time in ms), which
//                             carries the times itself
//   PREFIX.ogg                Opus in Ogg (RFC 7845)
//   PREFIX.aac                AAC as ADTS
//   PREFIX.wav                PCM, 16-bit stereo, 48 kHz
//
// A file starts at a keyframe; after pictures were lost it goes on at the
// packet marked after_gap (a keyframe, or a picture the encoder made without
// the lost ones: it refers only to pictures written before them). The sound is aligned with the pictures at its
// start, and where packets were lost (Opus: packets of no data, which a
// decoder conceals; PCM: silence; AAC: the previous packet again) or the
// sound is more than 100 ms behind its timestamps. To play the two together:
//   mkvmerge -o rec.mkv --timestamps 0:rec.h264.timestamps rec.h264 rec.ogg
// Small on purpose: it shows that the packets are complete and playable. A
// product writes MP4 or MKV with a library of its choice; the SDK writes no
// files. Test software, not part of the SDK.
#pragma once

#include <rcsdk/host_codec.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace recorder {

constexpr uint32_t soundRate = 48000;

inline void put16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
}
inline void put32(std::vector<uint8_t>& out, uint32_t value) {
    put16(out, value & 0xFFFF);
    put16(out, value >> 16);
}
inline void put64(std::vector<uint8_t>& out, uint64_t value) {
    put32(out, uint32_t(value));
    put32(out, uint32_t(value >> 32));
}

inline void write(std::ofstream& out, const uint8_t* data, size_t size) {
    out.write(reinterpret_cast<const char*>(data), std::streamsize(size));
}

// ---- The picture ----------------------------------------------------------------------------

// One codec's file and its timestamps.
class PictureFile {
public:
    PictureFile(const std::string& path, rcs_codec codec)
        : path_(path), codec_(codec), out_(path, std::ios::binary) {
        if (ivf()) {
            std::cout << "record: " << path << std::endl;
            return;
        }
        times_.open(path + ".timestamps");
        times_ << "# timestamp format v2\n";
        std::cout << "record: " << path << " and " << path << ".timestamps" << std::endl;
    }
    ~PictureFile() {
        if (ivf() && pictures_) { // The frame count in the header, now that it is known.
            std::vector<uint8_t> count;
            put32(count, uint32_t(pictures_));
            out_.seekp(24);
            recorder::write(out_, count.data(), count.size());
        }
    }

    void write(const rcs_encoded_packet& p, double ms) {
        static const uint8_t start[] = {0, 0, 0, 1};
        if (ivf()) {
            std::vector<uint8_t> header;
            if (!pictures_) { // The file's header, from the first picture (a keyframe).
                header = {'D', 'K', 'I', 'F'};
                put16(header, 0);  // Version.
                put16(header, 32); // Header size.
                const char* fourcc = codec_ == RCS_CODEC_VP8 ? "VP80" : "VP90";
                header.insert(header.end(), fourcc, fourcc + 4);
                put16(header, p.width);
                put16(header, p.height);
                put32(header, 1000); // Times in ms: a rate of 1000...
                put32(header, 1);    // ...per unit.
                put32(header, 0);    // Frames: written at the end.
                put32(header, 0);
            }
            put32(header, uint32_t(p.data_size));
            put64(header, uint64_t(std::llround(std::max(0.0, ms))));
            recorder::write(out_, header.data(), header.size());
            recorder::write(out_, p.data, size_t(p.data_size));
            ++pictures_;
            return;
        }
        if (codec_ == RCS_CODEC_AV1) {
            static const uint8_t delimiter[] = {0x12, 0x00}; // A temporal unit starts.
            recorder::write(out_, delimiter, sizeof(delimiter));
            if (p.keyframe) {
                recorder::write(out_, p.config, p.config_size);
            }
            recorder::write(out_, p.data, size_t(p.data_size));
        } else {
            // NAL units with 4-byte big-endian lengths, written with start codes.
            const auto units = [&](const uint8_t* bytes, size_t size) {
                for (size_t at = 0; at + 4 <= size;) {
                    const size_t length = size_t(bytes[at]) << 24 | size_t(bytes[at + 1]) << 16 |
                                          size_t(bytes[at + 2]) << 8 | bytes[at + 3];
                    at += 4;
                    if (length > size - at) {
                        return; // The SDK checked every packet: never.
                    }
                    recorder::write(out_, start, sizeof(start));
                    recorder::write(out_, bytes + at, length);
                    at += length;
                }
            };
            if (p.keyframe) {
                units(p.config, p.config_size);
            }
            units(p.data, size_t(p.data_size));
        }
        char line[32];
        std::snprintf(line, sizeof(line), "%.3f\n", ms);
        times_ << line;
        ++pictures_;
    }
    uint64_t pictures() const noexcept { return pictures_; }
    const std::string& path() const noexcept { return path_; }

private:
    bool ivf() const noexcept { return codec_ == RCS_CODEC_VP8 || codec_ == RCS_CODEC_VP9; }

    std::string path_;
    rcs_codec codec_;
    std::ofstream out_, times_;
    uint64_t pictures_ = 0;
};

// ---- The sound ------------------------------------------------------------------------------

// One audio codec's file. silence() writes about `frames` sample frames of
// silence before `next` and returns how many it wrote.
class SoundFile {
public:
    virtual ~SoundFile() = default;
    virtual void write(const rcs_encoded_audio& p) = 0;
    virtual uint64_t silence(uint64_t frames, const rcs_encoded_audio& next) = 0;
};

// The CRC of Ogg pages: polynomial 0x04C11DB7, no reflection, starting at 0.
inline uint32_t oggCrc(const std::vector<uint8_t>& bytes) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int bit = 0; bit < 8; ++bit) {
                r = r & 0x80000000u ? (r << 1) ^ 0x04C11DB7u : r << 1;
            }
            t[i] = r;
        }
        return t;
    }();
    uint32_t crc = 0;
    for (const uint8_t b : bytes) {
        crc = (crc << 8) ^ table[((crc >> 24) ^ b) & 0xFF];
    }
    return crc;
}

// Opus in Ogg: one packet per page; a page's granule position counts the
// 48 kHz sample frames up to the end of its packet. The last page waits for
// the next packet, or for the end, which marks it the stream's last.
class OggOpus final : public SoundFile {
public:
    explicit OggOpus(const std::string& path) : out_(path, std::ios::binary) {
        std::cout << "record: " << path << std::endl;
        std::vector<uint8_t> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2};
        put16(head, 120); // Pre-skip: the SDK's encoder looks 2.5 ms ahead (restricted low delay).
        put32(head, soundRate);
        put16(head, 0); // Output gain.
        head.push_back(0); // Channel mapping family 0: mono or stereo.
        page(head, 0, 0x02); // The stream's first page.
        static const char vendor[] = "rcsdk_cli recorder";
        std::vector<uint8_t> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
        put32(tags, uint32_t(sizeof(vendor) - 1));
        tags.insert(tags.end(), vendor, vendor + sizeof(vendor) - 1);
        put32(tags, 0); // No comments.
        page(tags, 0, 0);
    }
    ~OggOpus() override {
        if (held_) {
            page(*held_, granule_, 0x04); // The stream's last page.
        }
    }
    void write(const rcs_encoded_audio& p) override { add({p.data, p.data + p.data_size}, p.frames); }
    // Packets of one frame and no data (RFC 6716, 3.2.1) in the mode of the
    // next: a decoder conceals them as lost.
    uint64_t silence(uint64_t frames, const rcs_encoded_audio& next) override {
        const uint8_t toc = next.data[0];
        const uint32_t config = toc >> 3; // Its frames' mode and length.
        static const uint32_t silk[] = {480, 960, 1920, 2880}, celt[] = {120, 240, 480, 960};
        const uint32_t each = config < 12 ? silk[config & 3] : config < 16 ? (config & 1 ? 960 : 480) : celt[config & 3];
        uint64_t written = 0;
        for (; written + each / 2 < frames; written += each) {
            add({uint8_t(toc & 0xFC)}, each); // Code 0: one frame, here of no bytes.
        }
        return written;
    }

private:
    void add(std::vector<uint8_t> packet, uint32_t frames) {
        if (held_) {
            page(*held_, granule_, 0);
        }
        held_ = std::move(packet);
        granule_ += frames;
    }
    void page(const std::vector<uint8_t>& packet, uint64_t granule, uint8_t flags) {
        std::vector<uint8_t> page = {'O', 'g', 'g', 'S', 0, flags};
        put64(page, granule);
        put32(page, serial_);
        put32(page, sequence_++);
        put32(page, 0); // The CRC, below.
        page.push_back(uint8_t(packet.size() / 255 + 1)); // Lacing: 255s, then the rest (maybe 0).
        page.insert(page.end(), packet.size() / 255, 255);
        page.push_back(uint8_t(packet.size() % 255));
        page.insert(page.end(), packet.begin(), packet.end());
        const uint32_t crc = oggCrc(page);
        for (int i = 0; i < 4; ++i) {
            page[22 + i] = uint8_t(crc >> (8 * i));
        }
        recorder::write(out_, page.data(), page.size());
    }

    std::ofstream out_;
    const uint32_t serial_ = 0x52435344; // Any number: the file holds one stream.
    uint32_t sequence_ = 0;
    uint64_t granule_ = 0;
    std::optional<std::vector<uint8_t>> held_;
};

// AAC as ADTS: a 7-byte header from the AudioSpecificConfig before each
// access unit. Lost sound is the previous unit again (the file has no
// timestamps to leave a gap with).
class Adts final : public SoundFile {
public:
    explicit Adts(const std::string& path) : out_(path, std::ios::binary) {
        std::cout << "record: " << path << std::endl;
    }
    void write(const rcs_encoded_audio& p) override {
        if (p.config_size < 2) {
            return; // The SDK always gives the AudioSpecificConfig.
        }
        const uint32_t object = p.config[0] >> 3;
        const uint32_t rate = (p.config[0] & 7u) << 1 | p.config[1] >> 7;
        const uint32_t channels = (p.config[1] >> 3) & 15u;
        const uint32_t length = p.data_size + 7;
        last_ = {0xFF,
                 0xF1, // MPEG-4, no CRC.
                 uint8_t((object - 1) << 6 | rate << 2 | channels >> 2),
                 uint8_t((channels & 3) << 6 | length >> 11),
                 uint8_t(length >> 3),
                 uint8_t((length & 7) << 5 | 0x1F),
                 0xFC};
        last_.insert(last_.end(), p.data, p.data + p.data_size);
        recorder::write(out_, last_.data(), last_.size());
    }
    uint64_t silence(uint64_t frames, const rcs_encoded_audio&) override {
        uint64_t written = 0;
        for (; !last_.empty() && written + 512 < frames; written += 1024) {
            recorder::write(out_, last_.data(), last_.size());
        }
        return written;
    }

private:
    std::ofstream out_;
    std::vector<uint8_t> last_; // The latest unit with its header.
};

// PCM as WAV; the sizes are written when the file closes.
class Wav final : public SoundFile {
public:
    explicit Wav(const std::string& path) : out_(path, std::ios::binary) {
        std::cout << "record: " << path << std::endl;
        std::vector<uint8_t> header = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
        put32(header, 16);
        put16(header, 1); // PCM.
        put16(header, 2);
        put32(header, soundRate);
        put32(header, soundRate * 4);
        put16(header, 4);
        put16(header, 16);
        header.insert(header.end(), {'d', 'a', 't', 'a', 0, 0, 0, 0});
        recorder::write(out_, header.data(), header.size());
    }
    ~Wav() override {
        std::vector<uint8_t> size;
        put32(size, uint32_t(36 + bytes_));
        out_.seekp(4);
        recorder::write(out_, size.data(), 4);
        size.clear();
        put32(size, uint32_t(bytes_));
        out_.seekp(40);
        recorder::write(out_, size.data(), 4);
    }
    void write(const rcs_encoded_audio& p) override {
        recorder::write(out_, p.data, p.data_size);
        bytes_ += p.data_size;
    }
    uint64_t silence(uint64_t frames, const rcs_encoded_audio&) override {
        const std::vector<uint8_t> zeros(size_t(frames) * 4);
        recorder::write(out_, zeros.data(), zeros.size());
        bytes_ += zeros.size();
        return frames;
    }

private:
    std::ofstream out_;
    uint64_t bytes_ = 0;
};

// ---- The recorder ---------------------------------------------------------------------------

class Recorder final : public rcsdk::EncodedOutput {
public:
    explicit Recorder(std::string prefix) : prefix_(std::move(prefix)) {}

    // The decoding thread, in decoding order.
    void video(const rcs_encoded_packet& p) override {
        std::lock_guard lock(mutex_);
        if (p.after_gap) {
            std::cout << "record: pictures were lost; going on from " << (p.keyframe ? "a keyframe" : "older pictures")
                      << std::endl;
        }
        if (p.codec != codec_) {
            codec_ = p.codec;
            waiting_ = true; // A stream starts at a keyframe anyway.
        }
        const char* extension = p.codec == RCS_CODEC_H264                               ? "h264"
                                : p.codec == RCS_CODEC_HEVC                             ? "h265"
                                : p.codec == RCS_CODEC_VP8 || p.codec == RCS_CODEC_VP9 ? "ivf"
                                : p.codec == RCS_CODEC_AV1                              ? "obu"
                                                                                        : nullptr;
        if (!extension) {
            if (waiting_) {
                std::cout << "record: host codec " << p.codec << " is the hosts' own format; not written" << std::endl;
            }
            waiting_ = false;
            return;
        }
        if (waiting_ && !p.keyframe) {
            ++skipped_;
            return;
        }
        waiting_ = false;
        auto& file = pictures_[p.codec];
        if (!file) {
            file = std::make_unique<PictureFile>(prefix_ + "." + extension, p.codec);
        }
        if (!start_) {
            start_ = p.timestamp_ns; // The timeline's 0.
        }
        file->write(p, double(int64_t(p.timestamp_ns - *start_)) / 1e6);
    }

    // The sound thread, in order.
    void audio(const rcs_encoded_audio& p) override {
        std::lock_guard lock(mutex_);
        const uint64_t duration = uint64_t(p.frames) * 1'000'000'000 / soundRate;
        if (!start_ || p.timestamp_ns + duration <= *start_) {
            return; // Before the first picture.
        }
        auto& sound = sounds_[p.codec];
        if (!sound.file) {
            sound.path = prefix_ + (p.codec == RCS_AUDIO_OPUS  ? ".ogg"
                                    : p.codec == RCS_AUDIO_AAC ? ".aac"
                                                               : ".wav");
            if (p.codec == RCS_AUDIO_OPUS) {
                sound.file = std::make_unique<OggOpus>(sound.path);
            } else if (p.codec == RCS_AUDIO_AAC) {
                sound.file = std::make_unique<Adts>(sound.path);
            } else {
                sound.file = std::make_unique<Wav>(sound.path);
            }
        }
        // Where this packet belongs on the timeline, in sample frames.
        const int64_t due = (int64_t(p.timestamp_ns) - int64_t(*start_)) * soundRate / 1'000'000'000;
        uint64_t missing = 0;
        if (!sound.written) {
            missing = uint64_t(std::max<int64_t>(due, 0)); // The sound starts after the picture.
        } else if (sound.sequence && p.sequence > *sound.sequence + 1) {
            missing = (p.sequence - *sound.sequence - 1) * sound.frames; // Lost on the way.
        }
        if (due > int64_t(sound.written + missing) + soundRate / 10) {
            missing = uint64_t(due) - sound.written; // Behind by more than 100 ms: dropped at the sharer.
        }
        if (missing) {
            const uint64_t filled = sound.file->silence(missing, p);
            if (filled + soundRate / 100 < missing) { // AAC before its first packet.
                std::cout << "record: " << (missing - filled) * 1000 / soundRate << " ms of sound missing in "
                          << sound.path << ", not filled in" << std::endl;
            }
            sound.written += filled;
            sound.filled += filled;
        }
        sound.file->write(p);
        sound.written += p.frames;
        sound.sequence = p.sequence;
        sound.frames = p.frames;
        ++sound.packets;
    }

    // Waiting for a keyframe: the host asks for one (rcs_session_request_keyframe).
    bool waitingForKeyframe() {
        std::lock_guard lock(mutex_);
        return waiting_;
    }

    // What was written; after the session stopped.
    void report(std::ostream& out) {
        std::lock_guard lock(mutex_);
        for (const auto& [codec, file] : pictures_) {
            out << "record: " << file->pictures() << " pictures in " << file->path() << std::endl;
        }
        if (skipped_) {
            out << "record: " << skipped_ << " pictures skipped while waiting for a keyframe" << std::endl;
        }
        for (const auto& [codec, sound] : sounds_) {
            out << "record: " << sound.packets << " sound packets in " << sound.path << ", "
                << sound.written * 1000 / soundRate << " ms (" << sound.filled * 1000 / soundRate
                << " ms of it filled in)" << std::endl;
        }
    }

private:
    struct Sound {
        std::string path;
        std::unique_ptr<SoundFile> file;
        uint64_t written = 0, filled = 0; // Sample frames.
        std::optional<uint64_t> sequence; // The latest packet's...
        uint32_t frames = 0;              // ...and its length.
        uint64_t packets = 0;
    };

    std::mutex mutex_;
    const std::string prefix_;
    rcs_codec codec_ = 0;
    bool waiting_ = true; // For a keyframe.
    uint64_t skipped_ = 0;
    std::optional<uint64_t> start_; // The first picture written: the timeline's 0, on the sharer's clock.
    std::map<rcs_codec, std::unique_ptr<PictureFile>> pictures_;
    std::map<rcs_audio_codec, Sound> sounds_;
};

} // namespace recorder
