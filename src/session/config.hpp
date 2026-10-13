// A session's configuration, copied out of the host's rcs_session_config and
// extension chain. Every extension is present here, with its defaults where the
// host did not chain it; `next` pointers are cleared.
#pragma once

#include "crypto/crypto.hpp"
#include "media/video.hpp"
#include "modules/registry.hpp"
#include "protocol/rate_control.hpp"
#include "util/api.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rcsdk::session {

// A module the session uses (RCS_EXT_MODULE), and its settings as the host gave them.
struct ModuleUse {
    const modules::Definition* module = nullptr;
    crypto::Bytes settings;
    bool operator==(const ModuleUse&) const = default;
};

struct SessionConfig {
    rcs_role role = RCS_ROLE_VIEW;
    rcs_preset preset = RCS_PRESET_BALANCED;
    // The extension types the host chained, as (1u << type) bits: the host's
    // from then on. The preset sets the others it has (presetExtensions).
    uint32_t chained = 0;
    rcs_connect connect{};
    rcs_identity identity{};
    // Sharing.
    rcs_share_video_config shareVideo = RCS_SHARE_VIDEO_CONFIG_INIT;
    rcs_encoder_config encoder = RCS_ENCODER_CONFIG_INIT;
    rcs_capture_config capture = RCS_CAPTURE_CONFIG_INIT;
    rcs_streams_config streams = RCS_STREAMS_CONFIG_INIT;
    rcs_send_audio_config sendAudio = RCS_SEND_AUDIO_CONFIG_INIT;
    rcs_control_config control = RCS_CONTROL_CONFIG_INIT;
    rcs_viewers_config viewers = RCS_VIEWERS_CONFIG_INIT;
    rcs_encodings_config encodings = RCS_ENCODINGS_CONFIG_INIT;
    // Viewing.
    rcs_view_request_config viewRequest = RCS_VIEW_REQUEST_CONFIG_INIT;
    rcs_decoder_config decoder = RCS_DECODER_CONFIG_INIT;
    rcs_receive_audio_config receiveAudio = RCS_RECEIVE_AUDIO_CONFIG_INIT;
    // Both.
    rcs_ice_servers_config iceServers = RCS_ICE_SERVERS_CONFIG_INIT;
    rcs_reconnect_config reconnect = RCS_RECONNECT_CONFIG_INIT;
    rcs_background_config background = RCS_BACKGROUND_CONFIG_INIT;
    // deviceIdentity.trusted_keys is cleared: the keys are copied into trustedKeys.
    rcs_device_identity_config deviceIdentity = RCS_DEVICE_IDENTITY_CONFIG_INIT;
    std::vector<crypto::PublicKey> trustedKeys;
    rcs_log_config log = RCS_LOG_CONFIG_INIT;
    std::vector<ModuleUse> modules; // In the order chained; the same modules for the session's life.
    rcs_paths_config paths = RCS_PATHS_CONFIG_INIT;
    rcs_forwarder_config forwarder = RCS_FORWARDER_CONFIG_INIT; // None while token[0] is 0.
    // Tuning.
    rcs_video_tuning_config videoTuning = RCS_VIDEO_TUNING_CONFIG_INIT;
    rcs_audio_tuning_config audioTuning = RCS_AUDIO_TUNING_CONFIG_INIT;
    rcs_network_tuning_config networkTuning = RCS_NETWORK_TUNING_CONFIG_INIT;
    // The host replaces a part.
    rcs_host_encoder_config hostEncoder = RCS_HOST_ENCODER_CONFIG_INIT; // None while codecs[0] is 0.
    rcs_host_decoder_config hostDecoder = RCS_HOST_DECODER_CONFIG_INIT; // None while codecs[0] is 0.
    rcs_encoded_output_config encodedOutput = RCS_ENCODED_OUTPUT_CONFIG_INIT;
    rcs_rate_decider_config rateDecider = RCS_RATE_DECIDER_CONFIG_INIT;
    rcs_host_transport_config hostTransport = RCS_HOST_TRANSPORT_CONFIG_INIT;
    rcs_gpu_device_config gpuDevice = RCS_GPU_DEVICE_CONFIG_INIT;
    rcs_agent_link_config agentLink = RCS_AGENT_LINK_CONFIG_INIT;
};

// Validates and copies the host's configuration. Throws
// Failure(INVALID_ARGUMENT) naming the offending field.
SessionConfig parseConfig(const rcs_session_config& config);

// The displays of streams 1, 2, ... (rcs_streams_config), in order.
std::vector<uint32_t> streamDisplays(const SessionConfig& config);

// An agent's chain (rcs_agent_config.extensions): RCS_EXT_LOG and
// RCS_EXT_HOST_ENCODER, each at most once, validated as a session's. Throws
// Failure(INVALID_ARGUMENT) naming the problem.
struct AgentExtensions {
    rcs_log_level logLevel = 0;
    std::optional<rcs_host_encoder_config> hostEncoder;
};
AgentExtensions parseAgentExtensions(const rcs_extension* chain);

// rcs_capture_picture's `what`: an RCS_EXT_CAPTURE extension alone, valid
// as a session's, of a source the SDK captures (not HOST, ENCODED). Throws
// Failure(INVALID_ARGUMENT) naming the problem.
rcs_capture_config parseThumbnailCapture(const rcs_capture_config* what);

// Validates every extension in `chain` for config.role and, only if all are
// valid, replaces those extensions' settings in `config` and counts them as
// chained. Returns the extension types present, as (1u << type) bits.
uint32_t applyExtensions(SessionConfig& config, const rcs_extension* chain);

// The core configuration's identity; throws Failure(INVALID_ARGUMENT) naming
// the problem.
rcs_identity parseIdentity(const rcs_identity& identity);
// rcs_session_set_identity: checks `identity` with the rest of `config` and
// replaces config.identity with it. Throws as parseIdentity.
void applyIdentity(SessionConfig& config, const rcs_identity& identity);

// The extension types a preset gives settings to, as (1u << type) bits.
uint32_t presetExtensions() noexcept;
// Makes `preset` the session's and gives its settings to every extension of
// presetExtensions() the host has not chained; checks the result as
// applyExtensions does. Returns the types whose settings changed.
uint32_t applyPreset(SessionConfig& config, rcs_preset preset);
// rcs_preset_defaults: `preset`'s settings for `extension`'s type, written
// into it. Throws Failure(INVALID_ARGUMENT) naming the problem.
void presetDefaults(rcs_preset preset, rcs_extension& extension);
// rcs_session_get_settings: the settings `config` holds for `extension`'s
// type, written into it. Throws Failure(INVALID_ARGUMENT) naming the problem.
void getSettings(const SessionConfig& config, rcs_extension& extension);

// Extensions fixed for a run, as (1u << type) bits: the replacing layer's.
// Every other one changes while running.
constexpr uint32_t fixedExtensions = (1u << RCS_EXT_HOST_ENCODER) | (1u << RCS_EXT_HOST_DECODER) |
                                     (1u << RCS_EXT_ENCODED_OUTPUT) | (1u << RCS_EXT_RATE_DECIDER) |
                                     (1u << RCS_EXT_HOST_TRANSPORT) | (1u << RCS_EXT_GPU_DEVICE) |
                                     (1u << RCS_EXT_AGENT_LINK);
// Their names, for messages: "RCS_EXT_SHARE_VIDEO, ... and RCS_EXT_CONTROL".
std::string extensionList(uint32_t types);
// The extensions a session of `role` takes, as (1u << type) bits.
uint32_t extensionsOf(rcs_role role) noexcept;
// Copies the identity, the preset and every extension's settings, or (while
// running) all but the fixed extensions'.
void copyExtensionSettings(SessionConfig& to, const SessionConfig& from) noexcept;
void copyRuntimeSettings(SessionConfig& to, const SessionConfig& from) noexcept;

// Whether a codec list (unused entries 0) holds `codec`.
template <size_t N>
bool lists(const uint32_t (&codecs)[N], uint32_t codec) noexcept {
    for (const uint32_t listed : codecs) {
        if (listed == codec) {
            return true;
        }
    }
    return false;
}

// ADAPTIVE's floor: min_bitrate_bps, or 500 kbit/s (at most bitrate_bps) for 0.
inline uint32_t minimumBitrate(const rcs_share_video_config& video) noexcept {
    return video.min_bitrate_bps ? video.min_bitrate_bps : std::min(500'000u, video.bitrate_bps);
}

// The sharer's video bitrate for a stream of `bitrate` (the plan's, see
// session/stream_plan.hpp): adaptive between limits by the rules, or fixed at it.
struct RateLimits {
    bool adaptive = false;
    uint32_t minimum = 0, maximum = 0, start = 0;
    protocol::RateRules rules;
    protocol::FecSetting fec;
    bool operator==(const RateLimits&) const = default;
};
RateLimits rateLimits(const SessionConfig& config, uint32_t bitrate) noexcept;

// Whether the transport finds its path with ICE over the host's signals.
inline bool usesIce(rcs_transport transport) noexcept {
    return transport == RCS_TRANSPORT_P2P || transport == RCS_TRANSPORT_RELAY;
}

// Whether what this end sends goes over the host's own channel
// (RCS_EXT_HOST_TRANSPORT): the session's connections (HOST), or its hop to a
// forwarder (FORWARDER, link HOST), whose one route is 0.
inline bool usesHostChannel(const SessionConfig& config) noexcept {
    return config.connect.transport == RCS_TRANSPORT_HOST ||
           (config.connect.transport == RCS_TRANSPORT_FORWARDER && config.forwarder.link == RCS_FORWARDER_HOST);
}

// How many viewers a sharer takes at once: rcs_viewers_config.max_viewers, 0 being 64.
inline uint32_t maxViewers(const SessionConfig& config) noexcept {
    return config.viewers.max_viewers ? config.viewers.max_viewers : 64;
}

// Whether the sharer's machine-bound work runs in an agent of another process.
inline bool hasRemoteAgent(const SessionConfig& config) noexcept {
    return config.agentLink.endpoint[0] != '\0';
}

// The input types the system executes, as (1u << rcs_input_type) bits: none
// with target HOST, else those not handed to the host (host_input_types).
// RCS_INPUT_RELEASE_ALL goes to both and is no type of its own here.
uint32_t systemInputTypes(const rcs_control_config& control) noexcept;

} // namespace rcsdk::session
