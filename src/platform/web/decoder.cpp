// Video decoding in a browser: WebCodecs' VideoDecoder, on the SDK's media
// thread (a worker of the page's that keeps its event loop, web.hpp). The
// SDK's decoding thread hands each packet over and waits for its picture; the
// picture stays a VideoFrame in that worker, named by a number, closed when
// the SDK lets go. The worker draws it there (rcsdk.mjs's VideoView) or hands
// it to the page (docs/guide/23-browser.md). Which pictures are
// kept and shown: decoded.hpp.
#include "platform/web/decoded.hpp"
#include "platform/web/web.hpp"

#include "media/web_codec_config.hpp"

#include <emscripten.h>

#include <map>

namespace rcsdk::platform {
namespace {

// The media thread's side (web/pre.js keeps the decoders and frames in its Module.rcsdk).
EM_JS(void, jsDecoderOpen, (int id, int hardware), { Module.rcsdk.decoderOpen(id, hardware); });
EM_JS(void, jsDecoderClose, (int id), { Module.rcsdk.decoderClose(id); });
EM_JS(void, jsDecode,
      (int id, const char* codec, const uint8_t* description, int descriptionSize, const uint8_t* data, int size,
       int keyframe, double chunk),
      {
          Module.rcsdk.decode(id, codec ? UTF8ToString(codec) : null,
                              descriptionSize ? HEAPU8.slice(description, description + descriptionSize) : null,
                              HEAPU8.slice(data, data + size), keyframe, chunk);
      });
EM_JS(void, jsFrameClose, (int frame), { Module.rcsdk.frameClose(frame); });

// Decoders by number, for the page's callbacks.
std::mutex slotsMutex;
std::map<int, std::shared_ptr<DecodedSlot>> slots;
int nextDecoder = 1;

class WebDecoder final : public VideoDecoder {
public:
    ~WebDecoder() override {
        if (id_) {
            onMedia([id = id_] { jsDecoderClose(id); });
            std::lock_guard lock(slotsMutex);
            slots.erase(id_);
        }
    }

    void open(const DecoderConfig& config) override {
        const bool hardware = config.acceleration == RCS_ACCELERATION_HARDWARE;
        if (!(webDecoderFormats(config.codec, hardware) & (1u << config.picture.format))) {
            fail(RCS_ERROR_UNSUPPORTED, "This browser has no " + std::string(hardware ? "hardware" : "software") +
                                            " decoder for this stream (VideoDecoder.isConfigSupported)");
        }
        codec_ = config.codec;
        hardware_ = hardware;
        slot_ = std::make_shared<DecodedSlot>();
        {
            std::lock_guard lock(slotsMutex);
            id_ = nextDecoder++;
            slots[id_] = slot_;
        }
        onMedia([id = id_, hardware] { jsDecoderOpen(id, hardware); });
    }

    bool decode(const EncodedPacket& packet, VideoFrame& frame, CancelToken cancelled) override {
        // The decoder is configured from each keyframe's parameter sets (a
        // new stream, or the same one again after a reset).
        std::string codec;
        std::vector<uint8_t> description;
        if (packet.keyframe && !packet.config.empty() && packet.config != configured_) {
            const auto setup = web::decoderSetup(codec_, packet.config);
            if (!setup) {
                fail(RCS_ERROR_CODEC, "The keyframe's configuration is incomplete (parameter sets, sequence header)");
            }
            configured_ = packet.config;
            codec = setup->codec;
            description = setup->description;
        } else if (configured_.empty()) {
            return false; // Nothing to decode with until a keyframe.
        }
        // AV1 keyframes carry the sequence header too: WebCodecs takes a key
        // chunk only with it (Chromium checks).
        std::vector<uint8_t> data;
        if (codec_ == RCS_CODEC_AV1 && packet.keyframe) {
            data = packet.config;
        }
        data.insert(data.end(), packet.data.begin(), packet.data.end());
        const double chunk = double(++chunks_);
        {
            std::lock_guard waiting(slot_->mutex);
            slot_->waiting = chunk;
        }
        onMedia([id = id_, codec, description, data = std::move(data), keyframe = packet.keyframe, chunk] {
            jsDecode(id, codec.empty() ? nullptr : codec.c_str(), description.data(), int(description.size()),
                     data.data(), int(data.size()), keyframe, chunk);
        });
        // WebCodecs gives each packet its picture (optimizeForLatency); one
        // that does not within half a second is a decoder that holds them.
        std::unique_lock lock(slot_->mutex);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (slot_->outcomes.empty()) {
            if (cancelled) {
                slot_->waiting = 0;
                fail(RCS_ERROR_CANCELLED, "Cancelled");
            }
            if (slot_->arrived.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() +
                                                                        std::chrono::milliseconds(20))) ==
                    std::cv_status::timeout &&
                std::chrono::steady_clock::now() >= deadline) {
                slot_->waiting = 0;
                return false;
            }
        }
        const Decoded outcome = std::move(slot_->outcomes.front());
        slot_->outcomes.pop_front();
        slot_->waiting = 0;
        lock.unlock();
        if (!outcome.frame) {
            configured_.clear();
            fail(RCS_ERROR_CODEC, "The browser's decoder failed: " + outcome.error);
        }
        if (const auto problem = sizeProblem(outcome.width, outcome.height, packet.width, packet.height);
            !problem.empty()) {
            onMedia([frame = outcome.frame] { jsFrameClose(frame); });
            fail(RCS_ERROR_CODEC, problem);
        }
        frame.width = outcome.width;
        frame.height = outcome.height;
        frame.surface = std::shared_ptr<void>(reinterpret_cast<void*>(intptr_t(outcome.frame)), [](void* p) {
            onMedia([frame = int(reinterpret_cast<intptr_t>(p))] { jsFrameClose(frame); });
        });
        frame.color = {RCS_PRIMARIES_BT709, RCS_TRANSFER_BT709, RCS_MATRIX_BT709, RCS_RANGE_LIMITED};
        return true;
    }

    void reset() override {
        configured_.clear(); // The next keyframe configures it again.
        std::lock_guard lock(slot_->mutex);
        slot_->waiting = 0;
        for (const auto& outcome : slot_->outcomes) {
            if (outcome.frame) {
                onMedia([frame = outcome.frame] { jsFrameClose(frame); });
            }
        }
        slot_->outcomes.clear();
    }

    std::string name() const override { return webDecoderName(codec_, hardware_); }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_WEBCODECS; }

private:
    int id_ = 0;
    rcs_codec codec_ = RCS_CODEC_H264;
    bool hardware_ = true;
    std::shared_ptr<DecodedSlot> slot_;
    std::vector<uint8_t> configured_; // The parameter sets it was configured with.
    uint64_t chunks_ = 0;             // The packets handed over so far, which number them.
};

} // namespace

std::unique_ptr<VideoDecoder> createVideoDecoder(const DecoderConfig&) {
    return std::make_unique<WebDecoder>();
}

} // namespace rcsdk::platform

// The decoders report here (on the media thread): a picture (frame, its size,
// the chunk it is of) or an error (frame 0, the browser's message).
extern "C" EMSCRIPTEN_KEEPALIVE void rcsdk_web_decoded(int decoder, int frame, uint32_t width, uint32_t height,
                                                       double chunk, const char* error) {
    using namespace rcsdk::platform;
    std::shared_ptr<DecodedSlot> slot;
    {
        std::lock_guard lock(slotsMutex);
        const auto found = slots.find(decoder);
        if (found != slots.end()) {
            slot = found->second;
        }
    }
    if (!slot) {
        if (frame) {
            jsFrameClose(frame); // Its decoder is gone.
        }
        return;
    }
    if (!slot->keep({frame, width, height, chunk, error ? error : ""})) {
        jsFrameClose(frame); // No decode() waits for it: a second picture of one chunk, or a late one.
    }
}
