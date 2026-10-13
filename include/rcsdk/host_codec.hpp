// Remote Control SDK - C++ wrapper of the host's own codecs and the encoded
// packets (rcsdk/host_codec.h).
#pragma once

#include <rcsdk/host_codec.h>
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/tuning.hpp>

namespace rcsdk {

template <>
inline rcs_host_encoder_config defaults<rcs_host_encoder_config>() noexcept {
    return RCS_HOST_ENCODER_CONFIG_INIT;
}
template <>
inline rcs_host_decoder_config defaults<rcs_host_decoder_config>() noexcept {
    return RCS_HOST_DECODER_CONFIG_INIT;
}
template <>
inline rcs_encoded_output_config defaults<rcs_encoded_output_config>() noexcept {
    return RCS_ENCODED_OUTPUT_CONFIG_INIT;
}

// The host's own encoder (see rcs_host_encoder_config): hostEncoder() hands it
// to a session or an agent (AgentConfig::setHostEncoder), which calls it from
// its encoding thread (each stream's and each of its encodings' its own,
// rcs_streams_config, RCS_EXT_ENCODINGS: the calls name both); it must
// outlive them. Push each packet with
// pushEncoded(settings.output, packet). An exception escaping a call counts
// as RCS_ERROR_CODEC.
class HostEncoder {
public:
    virtual ~HostEncoder() = default;
    virtual rcs_result open(const rcs_host_encoder_settings& settings) = 0;
    virtual rcs_result encode(const rcs_host_encoder_frame& frame) = 0;
    // Not overridden: the bitrate it opened with stays.
    virtual rcs_result setBitrate(uint32_t stream, uint32_t encoding, uint32_t bitsPerSecond) {
        (void)stream;
        (void)encoding;
        (void)bitsPerSecond;
        return RCS_ERROR_UNSUPPORTED;
    }
    virtual void close(uint32_t stream, uint32_t encoding) = 0;
};

inline rcs_result pushEncoded(rcs_encoder_output output, const rcs_encoded_packet& packet) noexcept {
    return rcs_encoder_output_push(output, &packet);
}

// The host's own decoder (see rcs_host_decoder_config), likewise; it shows
// the pictures itself.
class HostDecoder {
public:
    virtual ~HostDecoder() = default;
    virtual rcs_result open(const rcs_host_decoder_settings& settings) = 0;
    virtual rcs_result decode(const rcs_encoded_packet& packet) = 0;
    virtual void reset(uint32_t stream) = 0;
    virtual void close(uint32_t stream) = 0;
};

namespace detail {
template <class Call>
rcs_result shielded(Call&& call) noexcept {
    try {
        return call();
    } catch (...) {
        return RCS_ERROR_CODEC;
    }
}
} // namespace detail

// The extension of the host's encoder `encoder` for `codecs` (list them in
// SessionConfig::setCodecs too, in the order of preference), in
// `pictureFormats` ((1u << rcs_picture_format) bits):
//   config.use(rcsdk::hostEncoder(encoder, {RCS_CODEC_H264}, "my encoder"));
inline rcs_host_encoder_config hostEncoder(HostEncoder& encoder, std::initializer_list<rcs_codec> codecs,
                                           const std::string& name, bool cpuImages = false,
                                           rcs_acceleration acceleration = RCS_ACCELERATION_HARDWARE,
                                           bool lowLatency = true,
                                           uint32_t pictureFormats = 1u << RCS_PICTURE_420_8) {
    rcs_host_encoder_config c = RCS_HOST_ENCODER_CONFIG_INIT;
    detail::setList(c.codecs, codecs, "codecs");
    c.picture_formats = pictureFormats;
    detail::setText(c.name, name, "name");
    c.cpu_images = cpuImages ? 1u : 0u;
    c.acceleration = acceleration;
    c.low_latency = lowLatency ? 1u : 0u;
    c.user_data = &encoder;
    c.open = [](const rcs_host_encoder_settings* settings, void* data) noexcept {
        return detail::shielded([&] { return static_cast<HostEncoder*>(data)->open(*settings); });
    };
    c.encode = [](const rcs_host_encoder_frame* frame, void* data) noexcept {
        return detail::shielded([&] { return static_cast<HostEncoder*>(data)->encode(*frame); });
    };
    c.set_bitrate = [](uint32_t stream, uint32_t encoding, uint32_t bitrate, void* data) noexcept {
        return detail::shielded(
            [&] { return static_cast<HostEncoder*>(data)->setBitrate(stream, encoding, bitrate); });
    };
    c.close = [](uint32_t stream, uint32_t encoding, void* data) noexcept {
        detail::shielded([&] {
            static_cast<HostEncoder*>(data)->close(stream, encoding);
            return RCS_OK;
        });
    };
    return c;
}

// The extension of the host's decoder `decoder` for `codecs` (list them in
// SessionConfig::setCodecs too), in `pictureFormats`.
inline rcs_host_decoder_config hostDecoder(HostDecoder& decoder, std::initializer_list<rcs_codec> codecs,
                                           const std::string& name,
                                           rcs_acceleration acceleration = RCS_ACCELERATION_HARDWARE,
                                           uint32_t pictureFormats = 1u << RCS_PICTURE_420_8) {
    rcs_host_decoder_config c = RCS_HOST_DECODER_CONFIG_INIT;
    detail::setList(c.codecs, codecs, "codecs");
    c.picture_formats = pictureFormats;
    detail::setText(c.name, name, "name");
    c.acceleration = acceleration;
    c.user_data = &decoder;
    c.open = [](const rcs_host_decoder_settings* settings, void* data) noexcept {
        return detail::shielded([&] { return static_cast<HostDecoder*>(data)->open(*settings); });
    };
    c.decode = [](const rcs_encoded_packet* packet, void* data) noexcept {
        return detail::shielded([&] { return static_cast<HostDecoder*>(data)->decode(*packet); });
    };
    c.reset = [](uint32_t stream, void* data) noexcept {
        detail::shielded([&] {
            static_cast<HostDecoder*>(data)->reset(stream);
            return RCS_OK;
        });
    };
    c.close = [](uint32_t stream, void* data) noexcept {
        detail::shielded([&] {
            static_cast<HostDecoder*>(data)->close(stream);
            return RCS_OK;
        });
    };
    return c;
}

// The encoded packets (see rcs_encoded_output_config): a sharer's before
// they are sent, a viewer's as received, before they are decoded. It must
// outlive the session; neither call may throw (an escaping exception
// terminates the process).
class EncodedOutput {
public:
    virtual ~EncodedOutput() = default;
    virtual void video(const rcs_encoded_packet& packet) = 0;
    // Not overridden: the sound's packets are not wanted.
    virtual void audio(const rcs_encoded_audio& packet) { (void)packet; }
};

//   config.use(rcsdk::encodedOutput(recorder));        // view: and decode as usual
//   config.use(rcsdk::encodedOutput(recorder, false)); // view: the packets only
inline rcs_encoded_output_config encodedOutput(EncodedOutput& output, bool decode = true) noexcept {
    rcs_encoded_output_config c = RCS_ENCODED_OUTPUT_CONFIG_INIT;
    c.decode = decode ? 1u : 0u;
    c.user_data = &output;
    c.on_video = [](const rcs_encoded_packet* packet, void* data) noexcept {
        static_cast<EncodedOutput*>(data)->video(*packet);
    };
    c.on_audio = [](const rcs_encoded_audio* packet, void* data) noexcept {
        static_cast<EncodedOutput*>(data)->audio(*packet);
    };
    return c;
}

// Share with capture source ENCODED: the host's next packet, and whether a
// new picture may go now (RCS_OK) or should be skipped (QUEUE_FULL).
inline rcs_result pushEncodedVideo(Session& session, const rcs_encoded_packet& packet) noexcept {
    return rcs_session_push_encoded_video(session.handle(), &packet);
}
inline rcs_result videoReady(Session& session) noexcept {
    return rcs_session_video_ready(session.handle());
}

} // namespace rcsdk
