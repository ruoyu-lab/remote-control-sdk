// The host's own decoder (RCS_EXT_HOST_DECODER) as the view's decoder: each
// packet goes to its decode callback; the pictures are the host's to show, so
// it yields none to deliver.
#pragma once

#include "platform/platform.hpp"
#include "util/api.hpp"

namespace rcsdk::session {

class HostDecoder final : public platform::VideoDecoder {
public:
    // Stream `stream`'s (rcs_streams_config): its calls name it.
    HostDecoder(const rcs_host_decoder_config& config, uint32_t stream) : config_(config), stream_(stream) {}
    ~HostDecoder() override;

    // The host's open waits for the stream's first keyframe, which says its size.
    void open(const platform::DecoderConfig& config) override { codec_ = config.codec; }
    // Hands the packet over: true once the host took it, though there is
    // never a picture for the SDK to deliver. Throws Failure with the host's
    // result when its open or decode fails.
    bool decode(const EncodedPacket& packet, VideoFrame& frame, CancelToken cancelled) override;
    void reset() override;
    std::string name() const override { return config_.name; }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_HOST; }

private:
    const rcs_host_decoder_config config_;
    const uint32_t stream_;
    rcs_codec codec_ = 0;
    bool opened_ = false;
};

} // namespace rcsdk::session
