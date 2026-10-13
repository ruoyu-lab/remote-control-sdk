// What every codec library's encoder and decoder share: pictures in and out
// (YuvImage), one picture at a time, the requests the pipeline makes
// (keyframes, the bitrate, reference invalidation). Each library fills in
// how it encodes or decodes one picture.
#pragma once

#include "codecs/codecs.hpp"
#include "media/yuv_image.hpp"
#include "media/picture_size.hpp"
#include "util/log.hpp"

#include <atomic>
#include <deque>
#include <optional>

namespace rcsdk::codecs {

// A library's implementation, as rcs_codec_implementation lists it.
inline platform::CodecImplementation libraryImplementation(rcs_codec codec, rcs_codec_direction direction,
                                                           rcs_backend backend, uint32_t pictureFormats,
                                                           std::string name, bool refInvalidation,
                                                           uint32_t temporalLayers = 1) {
    platform::CodecImplementation i;
    i.codec = codec;
    i.direction = direction;
    i.acceleration = RCS_ACCELERATION_SOFTWARE;
    i.backend = backend;
    const auto limit = codecLimit(codec);
    i.maxWidth = limit.width;
    i.maxHeight = limit.height;
    i.pictureFormats = pictureFormats;
    i.name = std::move(name);
    i.refInvalidation = refInvalidation;
    i.temporalLayers = temporalLayers;
    return i;
}


// An encoder that encodes each picture as it is submitted, on the caller's
// thread (the library's own threads inside): lowLatency, one packet ready
// after each submit.
class SoftwareEncoder : public platform::VideoEncoder {
public:
    void open(const platform::EncoderConfig& config) final {
        config_ = config;
        bitrate_ = config.bitrate;
        start(config);
    }

    void submit(const VideoFrame& frame) final {
        const uint64_t started = monotonicNs();
        Request request;
        request.keyframe = keyframe_.exchange(false);
        if (const uint64_t after = invalidate_.exchange(none); after != none) {
            request.invalidateAfter = after;
        }
        if (const uint32_t rate = wantedBitrate_.exchange(0); rate && rate != bitrate_) {
            request.bitrate = rate;
        }
        request.sequence = frame.sequence;
        std::optional<Output> out;
        if (!frame.surface && frame.pixels) { // The host's BGRA pictures: converted here.
            fromBgra(frame.pixels->data(), frame.stride, frame.width, frame.height, config_.picture, scratch_);
            out = encode(scratch_.view(), request);
        } else {
            if (!reader_) {
                reader_ = platform::createSurfaceReader();
            }
            reader_->read(frame, config_.picture, [&](const YuvImage& image) { out = encode(image, request); });
        }
        if (request.bitrate) {
            bitrate_ = request.bitrate;
        }
        if (!out) {
            return; // The library kept the picture to itself (never in real-time mode).
        }
        EncodedPacket packet;
        packet.codec = config_.codec;
        packet.picture = config_.picture;
        packet.width = config_.width;
        packet.height = config_.height;
        packet.keyframe = out->keyframe;
        packet.sequence = frame.sequence;
        packet.timestampNs = frame.timestampNs;
        packet.recoversAfter = out->recoversAfter;
        packet.temporalId = out->temporalId;
        packet.discardable = out->discardable;
        packet.data = std::move(out->data);
        packet.config = std::move(out->config);
        packet.encodeNs = monotonicNs() - started;
        ready_.push_back(std::move(packet));
    }

    bool receive(EncodedPacket& packet, std::chrono::milliseconds) final {
        if (ready_.empty()) {
            return false;
        }
        packet = std::move(ready_.front());
        ready_.pop_front();
        return true;
    }
    size_t inFlight() const noexcept final { return ready_.size(); }
    void requestKeyframe() noexcept final { keyframe_ = true; }
    void invalidateAfter(uint64_t sequence) noexcept final { invalidate_ = sequence; }
    bool refInvalidation() const noexcept override { return true; } // Where a library has none, false.
    uint32_t temporalLayers() const noexcept final { return layers_; }
    void setBitrate(uint32_t bitsPerSecond) noexcept final { wantedBitrate_ = bitsPerSecond; }
    uint32_t bitrate() const noexcept final { return bitrate_; }
    bool lowLatency() const noexcept final { return true; }

protected:
    struct Request {
        bool keyframe = false;
        std::optional<uint64_t> invalidateAfter;
        uint32_t bitrate = 0; // A new target, or 0.
        uint64_t sequence = 0;
    };
    struct Output {
        std::vector<uint8_t> data, config;
        bool keyframe = false;
        std::optional<uint64_t> recoversAfter;
        uint32_t temporalId = 0; // Its layer, and whether nothing refers to it.
        bool discardable = false;
    };
    virtual void start(const platform::EncoderConfig& config) = 0;
    // One picture, as `request` says; nothing for a picture the library holds back.
    virtual std::optional<Output> encode(const YuvImage& image, const Request& request) = 0;

    platform::EncoderConfig config_;
    uint32_t layers_ = 1; // The temporal layers it makes (start() decides).

private:
    static constexpr uint64_t none = ~0ull;
    std::atomic<bool> keyframe_{false};
    std::atomic<uint64_t> invalidate_{none};
    std::atomic<uint32_t> wantedBitrate_{0};
    uint32_t bitrate_ = 0;
    YuvBuffer scratch_;
    std::unique_ptr<platform::SurfaceReader> reader_;
    std::deque<EncodedPacket> ready_;
};

// A decoder that decodes each packet as it comes, on the caller's thread.
class SoftwareDecoder : public platform::VideoDecoder {
public:
    void open(const platform::DecoderConfig& config) final {
        config_ = config;
        if (!config.cpuImages) {
            writer_ = platform::createSurfaceWriter(config);
        }
        start(config);
    }

    bool decode(const EncodedPacket& packet, VideoFrame& frame, CancelToken) final {
        const YuvImage* image = decodePicture(packet);
        if (!image) {
            return false;
        }
        if (writer_) {
            frame = writer_->write(*image, packet.picture);
        } else {
            toBgra(*image, packet.picture, frame);
        }
        return true;
    }

protected:
    virtual void start(const platform::DecoderConfig& config) = 0;
    // The packet's picture, valid until the next call; null while there is none.
    virtual const YuvImage* decodePicture(const EncodedPacket& packet) = 0;

    platform::DecoderConfig config_;

private:
    std::unique_ptr<platform::SurfaceWriter> writer_;
};

} // namespace rcsdk::codecs
