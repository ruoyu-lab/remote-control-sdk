#include "session/host_decoder.hpp"

namespace rcsdk::session {
namespace {

// "The host's decoder X ... (result)".
std::string what(const rcs_host_decoder_config& config, const char* happened, rcs_result result) {
    return std::string("The host's decoder ") + config.name + " " + happened + " (" + rcs_result_message(result) +
           (codecFailure(result) != result ? ", " + std::to_string(result) : std::string()) + ")";
}

} // namespace

HostDecoder::~HostDecoder() {
    if (opened_) {
        config_.close(stream_, config_.user_data);
    }
}

bool HostDecoder::decode(const EncodedPacket& packet, VideoFrame&, CancelToken) {
    if (!opened_) {
        if (!packet.keyframe) {
            return false; // A stream starts at a keyframe (the receive queue sees to it).
        }
        rcs_host_decoder_settings settings{};
        settings.struct_size = sizeof(settings);
        settings.stream = stream_;
        settings.codec = codec_;
        settings.format = packet.picture.format;
        settings.color = colorOf(packet.picture);
        settings.width = packet.width;
        settings.height = packet.height;
        if (const rcs_result result = config_.open(&settings, config_.user_data); result != RCS_OK) {
            fail(codecFailure(result), what(config_, "did not open", result));
        }
        opened_ = true;
    }
    const auto view = publicPacket(packet);
    if (const rcs_result result = config_.decode(&view, config_.user_data); result != RCS_OK) {
        fail(codecFailure(result), what(config_, "failed to decode", result));
    }
    return true;
}

void HostDecoder::reset() {
    if (opened_) {
        config_.reset(stream_, config_.user_data);
    }
}

} // namespace rcsdk::session
