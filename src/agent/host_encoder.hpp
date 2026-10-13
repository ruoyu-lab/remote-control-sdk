// The host's own encoder (RCS_EXT_HOST_ENCODER) as the pipeline's encoder: each
// picture goes to its encode callback, as the capture made it (rcs_gpu_frame)
// or as BGRA8 on the CPU; its packets come back through
// rcs_encoder_output_push, during encode or later from any thread.
#pragma once

#include "platform/platform.hpp"
#include "util/api.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace rcsdk::agent {
class HostEncoder;
}

// What the host pushes its packets to (rcs_encoder_output): its encoder.
struct rcs_encoder_output_impl {
    rcsdk::agent::HostEncoder* encoder;
};

namespace rcsdk::agent {

class HostEncoder final : public platform::VideoEncoder {
public:
    // Encoding `encoding` of stream `stream` (rcs_streams_config,
    // RCS_EXT_ENCODINGS): its calls name both.
    HostEncoder(const rcs_host_encoder_config& config, uint32_t stream, uint32_t encoding)
        : config_(config), stream_(stream), encoding_(encoding) {}
    ~HostEncoder() override;

    // Throws Failure with the host's result when its open fails.
    void open(const platform::EncoderConfig& config) override;
    // Throws Failure with the host's result when its encode fails.
    void submit(const VideoFrame& frame) override;
    bool receive(EncodedPacket& packet, std::chrono::milliseconds timeout) override;
    size_t inFlight() const noexcept override;
    void requestKeyframe() noexcept override { keyframe_ = true; }
    void setBitrate(uint32_t bitsPerSecond) noexcept override;
    uint32_t bitrate() const noexcept override { return bitrate_; }
    bool lowLatency() const noexcept override { return config_.low_latency != 0; }
    uint32_t temporalLayers() const noexcept override {
        return std::min(opened_.temporalLayers, config_.temporal_layers);
    }
    std::string name() const override { return config_.name; }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_HOST; }
    void onOutputReady(std::function<void()> callback) override;

    // rcs_encoder_output_push: throws Failure, INVALID_ARGUMENT naming what is
    // wrong with the packet, INVALID_STATE once the encoder closed.
    void push(const rcs_encoded_packet& packet);

private:
    const rcs_host_encoder_config config_;
    const uint32_t stream_, encoding_;
    rcs_encoder_output_impl output_{this};
    platform::EncoderConfig opened_;
    std::atomic<bool> keyframe_{true}; // The next picture is to be a keyframe.
    std::atomic<uint32_t> bitrate_{0};
    bool refused_ = false; // A bitrate it did not take was logged (the pipeline's thread).

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    bool open_ = false;
    uint64_t lastSubmitted_ = 0;
    std::deque<uint64_t> pending_; // Pictures given to encode and not answered by a packet yet.
    std::deque<EncodedPacket> packets_;
    std::vector<uint8_t> streamConfig_; // Standard codecs: the latest keyframe's.
    std::function<void()> outputReady_;
};

} // namespace rcsdk::agent
