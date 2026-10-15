// Remote Control SDK - header-only C++ wrapper over the C interface: the entry
// and feature layers. The tuning and replacing layers have their own headers,
// as in C: rcsdk/tuning.hpp, rcsdk/host_codec.hpp, rcsdk/rate_control.hpp,
// rcsdk/host_transport.hpp, rcsdk/gpu.hpp, rcsdk/agent.hpp.
//
// Naming: a noun returns what it names (capabilities(), session.streamInfo(),
// config.encoder() - the whole C structure, for fields without a setter);
// set* changes a setting; verbs act (start, update, requestKeyframe).
//
// Errors:
// - Actions (start, stop, update, sendInput, ...) return rcs_result, the
//   reason in lastCallError().
// - Construction, the config setters and the nouns that produce something
//   new (capabilities(), displays(), generateConnectionKey(), failure(), ...)
//   throw Error, which carries the result and the reason.
// - A session's state and information getters cannot fail on a session that
//   exists and are noexcept; the two whose section a role may not have,
//   stats<T>() and agentInfo(), return the zeroed structure there (the C
//   call's UNSUPPORTED), as their comments say.
#pragma once

#include <rcsdk/gpu.h>
#include <rcsdk/module.h>
#include <rcsdk/rcsdk.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace rcsdk {

// A failed call: its result and, where the SDK gave one, the reason.
class Error : public std::runtime_error {
public:
    explicit Error(rcs_result result) : std::runtime_error(describe(result)), result_(result) {}
    Error(rcs_result result, const std::string& reason)
        : std::runtime_error(std::string(rcs_result_message(result)) + ": " + reason), result_(result) {}
    rcs_result result() const noexcept { return result_; }

private:
    static std::string describe(rcs_result result) {
        char reason[512] = {};
        rcs_get_last_call_error(reason, sizeof(reason));
        return reason[0] ? std::string(rcs_result_message(result)) + ": " + reason : rcs_result_message(result);
    }
    rcs_result result_;
};

inline void check(rcs_result result) {
    if (result != RCS_OK) {
        throw Error(result);
    }
}

// Why the previous SDK call on this thread failed (rcs_get_last_call_error).
inline std::string lastCallError() {
    char buffer[512] = {};
    rcs_get_last_call_error(buffer, sizeof(buffer));
    return buffer;
}

// ---- Library and system ----------------------------------------------------

inline std::string version() {
    return rcs_version();
}

inline rcs_capabilities capabilities() {
    rcs_capabilities value = RCS_CAPABILITIES_INIT;
    check(rcs_get_capabilities(&value));
    return value;
}

// ---- Modules (rcsdk/module.h) ------------------------------------------------

// For this process's sessions and agents, once (see rcs_register_module).
inline void registerModule(const rcs_module_definition& definition) {
    check(rcs_register_module(&definition));
}
// Those registered, in order.
inline std::vector<rcs_module_info> registeredModules() {
    uint32_t count = 0;
    check(rcs_list_modules(nullptr, sizeof(rcs_module_info), 0, &count));
    std::vector<rcs_module_info> list(count);
    check(rcs_list_modules(list.data(), sizeof(rcs_module_info), count, &count));
    list.resize(count);
    return list;
}

// Whether a (1u << value) mask holds `value`: a codec in rcs_capabilities or
// rcs_peer_info, an audio codec, an audio capture backend.
inline constexpr bool supports(uint32_t mask, uint32_t value) noexcept {
    return value < 32 && (mask & (1u << value)) != 0;
}

namespace detail {
// A list that grew between the passes (windows come and go) is asked for again.
template <class T, class List>
std::vector<T> list(List call) {
    for (;;) {
        uint32_t count = 0;
        check(call(nullptr, 0, &count));
        std::vector<T> items(count + 8);
        const rcs_result result = call(items.data(), uint32_t(items.size()), &count);
        if (result != RCS_ERROR_BUFFER_TOO_SMALL) {
            check(result);
            items.resize(count);
            return items;
        }
    }
}
} // namespace detail

inline std::vector<rcs_codec_implementation> codecImplementations() {
    return detail::list<rcs_codec_implementation>([](rcs_codec_implementation* items, uint32_t capacity,
                                                     uint32_t* count) {
        return rcs_list_codec_implementations(items, sizeof(rcs_codec_implementation), capacity, count);
    });
}

inline std::vector<rcs_display_info> displays() {
    return detail::list<rcs_display_info>([](rcs_display_info* items, uint32_t capacity, uint32_t* count) {
        return rcs_list_displays(items, sizeof(rcs_display_info), capacity, count);
    });
}

// The windows a user would pick from to share, front to back (rcs_list_windows).
inline std::vector<rcs_window_info> windows() {
    return detail::list<rcs_window_info>([](rcs_window_info* items, uint32_t capacity, uint32_t* count) {
        return rcs_list_windows(items, sizeof(rcs_window_info), capacity, count);
    });
}

// This machine's networks, the default first (rcs_list_networks).
inline std::vector<rcs_network_info> networks() {
    return detail::list<rcs_network_info>([](rcs_network_info* items, uint32_t capacity, uint32_t* count) {
        return rcs_list_networks(items, sizeof(rcs_network_info), capacity, count);
    });
}

// This machine's Wi-Fi networks that it is connected to (rcs_list_wifi).
inline std::vector<rcs_wifi_info> wifi() {
    return detail::list<rcs_wifi_info>([](rcs_wifi_info* items, uint32_t capacity, uint32_t* count) {
        return rcs_list_wifi(items, sizeof(rcs_wifi_info), capacity, count);
    });
}

// The host's own watcher saw the network change (rcs_notify_network_change).
inline void notifyNetworkChange() noexcept {
    rcs_notify_network_change();
}

// The sound devices, inputs then outputs (rcs_list_audio_devices).
inline std::vector<rcs_audio_device_info> audioDevices() {
    return detail::list<rcs_audio_device_info>([](rcs_audio_device_info* items, uint32_t capacity, uint32_t* count) {
        return rcs_list_audio_devices(items, sizeof(rcs_audio_device_info), capacity, count);
    });
}

// A picture of what a capture would share (rcs_capture_picture): BGRA8 rows
// of width * 4 bytes, opaque.
struct CapturedPicture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

namespace detail {
// One picture call (rcs_capture_picture, or a module's that takes one, as
// rcs_thumbnail_capture), into a buffer that takes any answer within the
// limits (memory not written to is not used).
template <class Call>
CapturedPicture picture(const rcs_capture_config& what, uint32_t maxWidth, uint32_t maxHeight, Call call) {
    rcs_capture_config alone = what; // E.g. SessionConfig::capture(), chained once get() was called.
    alone.header.next = nullptr;
    rcs_picture out = RCS_PICTURE_INIT;
    out.capacity = maxWidth <= 8192 && maxHeight <= 8192 ? uint64_t(maxWidth) * maxHeight * 4 : 0;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[size_t(out.capacity)]);
    out.data = buffer.get();
    check(call(alone, out));
    return {out.width, out.height, std::vector<uint8_t>(buffer.get(), buffer.get() + out.data_size)};
}
} // namespace detail

// A picture of what `what` shares on this machine now, fitted into maxWidth x
// maxHeight (rcs_capture_picture; throws Error, e.g. INVALID_STATE for a
// minimized window). `what` may be chained: it is taken alone.
inline CapturedPicture capturePicture(const rcs_capture_config& what, uint32_t maxWidth, uint32_t maxHeight) {
    return detail::picture(what, maxWidth, maxHeight, [&](const rcs_capture_config& alone, rcs_picture& out) {
        return rcs_capture_picture(&alone, maxWidth, maxHeight, &out);
    });
}

using ConnectionKey = std::array<uint8_t, 32>;
inline ConnectionKey generateConnectionKey() {
    ConnectionKey key{};
    check(rcs_generate_connection_key(key.data(), uint32_t(key.size())));
    return key;
}

// A PIN of `digits` decimal digits, 4..32 (RCS_SECRET_PIN; rcs_generate_pin).
inline std::string generatePin(uint32_t digits = 6) {
    char pin[33] = {};
    check(rcs_generate_pin(pin, uint32_t(sizeof(pin)), digits));
    return pin;
}

// Device keys (see rcs_device_identity_config): the private key pair, and its public half.
using DeviceKey = std::array<uint8_t, RCS_DEVICE_KEY_SIZE>;
using PublicKey = std::array<uint8_t, RCS_PUBLIC_KEY_SIZE>;
inline DeviceKey generateDeviceKey() {
    DeviceKey key{};
    check(rcs_generate_device_key(key.data(), uint32_t(key.size())));
    return key;
}
inline PublicKey publicKeyOf(const DeviceKey& key) {
    PublicKey value{};
    check(rcs_device_public_key(key.data(), uint32_t(key.size()), value.data(), uint32_t(value.size())));
    return value;
}

// The SHA-256 of a certificate's DER (rcs_ice_servers_config.turn_certificate_sha256).
using CertificateSha256 = std::array<uint8_t, 32>;

inline rcs_permission screenCapturePermission() noexcept {
    return rcs_screen_capture_permission();
}
inline rcs_permission inputPermission() noexcept {
    return rcs_input_permission();
}
// Show the OS permission prompt; call on the application's main thread.
inline rcs_result requestScreenCapturePermission() noexcept {
    return rcs_request_screen_capture_permission();
}
inline rcs_result requestInputPermission() noexcept {
    return rcs_request_input_permission();
}
inline rcs_permission microphonePermission() noexcept {
    return rcs_microphone_permission();
}
inline rcs_result requestMicrophonePermission() noexcept {
    return rcs_request_microphone_permission();
}
// iOS: who sets up the application's audio session (rcs_audio_session_config);
// UNSUPPORTED elsewhere.
inline rcs_result setAudioSessionConfig(const rcs_audio_session_config& config) noexcept {
    return rcs_set_audio_session_config(&config);
}
inline rcs_audio_session_config audioSessionConfig() noexcept {
    rcs_audio_session_config config = RCS_AUDIO_SESSION_CONFIG_INIT;
    rcs_get_audio_session_config(&config);
    return config;
}

// Process-wide log callback (rcs_set_log_callback); an empty function turns
// logging off. Runs on SDK threads, one call at a time; must not throw. A
// record of a Session or Agent of this wrapper names it by handle
// (record.session == session.handle()); its user_data is the wrapper's own.
using LogCallback = std::function<void(const rcs_log_record& record)>;
inline rcs_result setLogCallback(rcs_log_level maxLevel, LogCallback callback) {
    static LogCallback stored;
    // Detach first: once that returns, the SDK no longer calls the old function.
    if (const auto result = rcs_set_log_callback(maxLevel, nullptr, nullptr); result != RCS_OK) {
        return result;
    }
    stored = std::move(callback);
    if (!stored) {
        return RCS_OK;
    }
    return rcs_set_log_callback(
        maxLevel,
        [](const rcs_log_record* record, void*) noexcept { stored(*record); },
        nullptr);
}

// ---- Configuration ---------------------------------------------------------

// Each extension's defaults (its *_INIT, which are BALANCED's), for building
// one to pass to Session::update:
//   auto video = rcsdk::defaults<rcs_share_video_config>();
// The headers of the other layers add theirs.
template <class Extension>
Extension defaults() noexcept;
template <>
inline rcs_share_video_config defaults<rcs_share_video_config>() noexcept {
    return RCS_SHARE_VIDEO_CONFIG_INIT;
}
template <>
inline rcs_encoder_config defaults<rcs_encoder_config>() noexcept {
    return RCS_ENCODER_CONFIG_INIT;
}
template <>
inline rcs_capture_config defaults<rcs_capture_config>() noexcept {
    return RCS_CAPTURE_CONFIG_INIT;
}
template <>
inline rcs_streams_config defaults<rcs_streams_config>() noexcept {
    return RCS_STREAMS_CONFIG_INIT;
}
template <>
inline rcs_send_audio_config defaults<rcs_send_audio_config>() noexcept {
    return RCS_SEND_AUDIO_CONFIG_INIT;
}
template <>
inline rcs_control_config defaults<rcs_control_config>() noexcept {
    return RCS_CONTROL_CONFIG_INIT;
}
template <>
inline rcs_viewers_config defaults<rcs_viewers_config>() noexcept {
    return RCS_VIEWERS_CONFIG_INIT;
}
template <>
inline rcs_encodings_config defaults<rcs_encodings_config>() noexcept {
    return RCS_ENCODINGS_CONFIG_INIT;
}
template <>
inline rcs_view_request_config defaults<rcs_view_request_config>() noexcept {
    return RCS_VIEW_REQUEST_CONFIG_INIT;
}
template <>
inline rcs_decoder_config defaults<rcs_decoder_config>() noexcept {
    return RCS_DECODER_CONFIG_INIT;
}
template <>
inline rcs_background_config defaults<rcs_background_config>() noexcept {
    return RCS_BACKGROUND_CONFIG_INIT;
}
template <>
inline rcs_receive_audio_config defaults<rcs_receive_audio_config>() noexcept {
    return RCS_RECEIVE_AUDIO_CONFIG_INIT;
}
template <>
inline rcs_ice_servers_config defaults<rcs_ice_servers_config>() noexcept {
    return RCS_ICE_SERVERS_CONFIG_INIT;
}
template <>
inline rcs_reconnect_config defaults<rcs_reconnect_config>() noexcept {
    return RCS_RECONNECT_CONFIG_INIT;
}
template <>
inline rcs_device_identity_config defaults<rcs_device_identity_config>() noexcept {
    return RCS_DEVICE_IDENTITY_CONFIG_INIT;
}
template <>
inline rcs_log_config defaults<rcs_log_config>() noexcept {
    return RCS_LOG_CONFIG_INIT;
}
template <>
inline rcs_paths_config defaults<rcs_paths_config>() noexcept {
    return RCS_PATHS_CONFIG_INIT;
}
template <>
inline rcs_forwarder_config defaults<rcs_forwarder_config>() noexcept {
    return RCS_FORWARDER_CONFIG_INIT;
}

// A preset's settings of an extension it has (rcs_preset_defaults), e.g. to
// change one field of LOW_LATENCY's video tuning; throws Error for one it has
// not.
template <class Extension>
Extension defaults(rcs_preset preset) {
    Extension value = defaults<Extension>();
    check(rcs_preset_defaults(preset, &value.header));
    return value;
}

namespace detail {
// Fills a fixed codec list: at most N entries, the rest 0.
template <class T, size_t N>
void setList(T (&field)[N], std::initializer_list<T> values, const char* name) {
    if (values.size() > N) {
        throw Error(RCS_ERROR_INVALID_ARGUMENT, std::string(name) + ": at most " + std::to_string(N));
    }
    std::fill(std::begin(field), std::end(field), T{});
    std::copy(values.begin(), values.end(), field);
}
// Copies a string with its terminator into a fixed C field.
template <size_t N>
void setText(char (&field)[N], const std::string& value, const char* name) {
    if (value.size() >= N || value.find('\0') != std::string::npos) {
        throw Error(RCS_ERROR_INVALID_ARGUMENT,
                    std::string(name) + ": at most " + std::to_string(N - 1) + " bytes, no NUL");
    }
    std::memcpy(field, value.c_str(), value.size() + 1);
}
// The extension types rcs_preset_defaults has settings for.
inline constexpr bool presetHas(rcs_extension_type type) noexcept {
    return type == RCS_EXT_SHARE_VIDEO || type == RCS_EXT_ENCODER || type == RCS_EXT_SEND_AUDIO ||
           type == RCS_EXT_VIEW_REQUEST || type == RCS_EXT_VIDEO_TUNING || type == RCS_EXT_AUDIO_TUNING ||
           type == RCS_EXT_NETWORK_TUNING;
}
// The replacing layer's extensions: fixed for a run, so Session::update(const
// SessionConfig&) leaves them out.
inline constexpr bool fixedForRun(rcs_extension_type type) noexcept {
    return type == RCS_EXT_HOST_ENCODER || type == RCS_EXT_HOST_DECODER || type == RCS_EXT_ENCODED_OUTPUT ||
           type == RCS_EXT_RATE_DECIDER || type == RCS_EXT_HOST_TRANSPORT || type == RCS_EXT_GPU_DEVICE ||
           type == RCS_EXT_AGENT_LINK;
}
} // namespace detail

// Builds an rcs_session_config and its extension chain. The role and the
// preset come first; the connection and the identity are the core's. Only the
// extensions touched are chained: each starts from the preset's settings
// where it has some (else the defaults) and is the host's from then on;
// everything else keeps the preset's. Each extension is reachable as a whole
// by its name (shareVideo(), encoder(), ...) or by its type
// (extension<rcs_video_tuning_config>(), for the other layers' headers); the
// setters cover the common fields. A value the C structures cannot hold (a
// string too long for its field, more codecs or STUN servers than there are
// slots) throws Error(INVALID_ARGUMENT) rather than being cut short. Settings
// of the other role throw nothing here; rcs_session_create refuses them.
//
//   auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE, RCS_PRESET_LOW_LATENCY)
//                     .setAddress("0.0.0.0", 47990).setKey(key)
//                     .setVideo(3840, 2160, 120, 80'000'000)
//                     .setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264});
class SessionConfig {
public:
    explicit SessionConfig(rcs_role role, rcs_preset preset = RCS_PRESET_BALANCED) noexcept {
        config_.role = role;
        config_.preset = preset;
    }
    SessionConfig(const SessionConfig& other) : config_(other.config_), trusted_(other.trusted_) {
        for (const auto& extension : other.extensions_) {
            extensions_.push_back(extension->clone());
        }
        for (const auto& module : other.modules_) {
            modules_.push_back({module.id, module.settings, RCS_MODULE_CONFIG_INIT});
        }
    }
    SessionConfig& operator=(const SessionConfig&) = delete;

    rcs_role role() const noexcept { return config_.role; }
    rcs_preset preset() const noexcept { return config_.preset; }

    // ---- How to connect and who this end is (the core) ---------------------

    // UDP, TCP: the address and port (share: listens on them; view: connects).
    // P2P and RELAY: the port is the local UDP port (0: any).
    SessionConfig& setAddress(const std::string& address, uint32_t port) {
        detail::setText(config_.connect.address, address, "address");
        config_.connect.port = port;
        return *this;
    }
    SessionConfig& setTransport(rcs_transport transport) noexcept {
        config_.connect.transport = transport;
        return *this;
    }
    SessionConfig& setKey(const uint8_t (&key)[32]) noexcept {
        std::copy(std::begin(key), std::end(key), config_.identity.key);
        return *this;
    }
    SessionConfig& setKey(const ConnectionKey& key) noexcept {
        std::copy(key.begin(), key.end(), config_.identity.key);
        return *this;
    }
    // A password or a short PIN instead of the connection key (rcs_identity).
    SessionConfig& setPassword(const std::string& password) {
        config_.identity.secret = RCS_SECRET_PASSWORD;
        detail::setText(config_.identity.password, password, "password");
        return *this;
    }
    // A sharer allows `attempts` failed PIN handshakes (see rcs_identity).
    SessionConfig& setPin(const std::string& pin, uint32_t attempts = 5) {
        config_.identity.secret = RCS_SECRET_PIN;
        detail::setText(config_.identity.password, pin, "pin");
        config_.identity.pin_attempts = attempts;
        return *this;
    }
    SessionConfig& setSecret(rcs_secret secret) noexcept {
        config_.identity.secret = secret;
        return *this;
    }

    // ---- Both roles --------------------------------------------------------

    // This end's device key, and the peers' public keys it trusts
    // (rcs_device_identity_config).
    SessionConfig& setDeviceKey(const DeviceKey& key) {
        auto& i = deviceIdentity();
        i.has_device_key = 1;
        std::copy(key.begin(), key.end(), i.device_key);
        return *this;
    }
    SessionConfig& setTrustedKeys(std::vector<PublicKey> keys) {
        deviceIdentity();
        trusted_ = std::move(keys);
        return *this;
    }
    // The video codecs this end offers, most preferred first (the encoder's
    // when sharing, the decoder's when viewing); at most RCS_MAX_CODECS.
    SessionConfig& setCodecs(std::initializer_list<rcs_codec> codecs) {
        detail::setList(sharing() ? encoder().codecs : decoder().codecs, codecs, "codecs");
        return *this;
    }
    SessionConfig& setAcceleration(rcs_acceleration acceleration) {
        (sharing() ? encoder().acceleration : decoder().acceleration) = acceleration;
        return *this;
    }
    SessionConfig& setBackend(rcs_backend backend) {
        (sharing() ? encoder().backend : decoder().backend) = backend;
        return *this;
    }
    // The picture formats this end sends or takes (rcs_picture_format), most
    // preferred first; at most RCS_MAX_PICTURE_FORMATS.
    SessionConfig& setPictureFormats(std::initializer_list<rcs_picture_format> formats) {
        detail::setList(sharing() ? encoder().formats : decoder().formats, formats, "picture formats");
        return *this;
    }
    // The sharer's sound, which is on by default (it flows while both ends
    // have it on): a sharer sends it, a viewer receives it. false turns this
    // end's sound off both ways.
    SessionConfig& setAudioEnabled(bool enabled) {
        if (enabled) {
            (sharing() ? sendAudio().enabled : receiveAudio().enabled) = 1u;
        } else {
            sendAudio().enabled = 0u;
            receiveAudio().enabled = 0u;
        }
        return *this;
    }
    // Each direction on its own: this end's sound to the other (a viewer:
    // setMicrophone, or setAudioSource(RCS_AUDIO_HOST) and pushAudio), and
    // the other end's to this one (a sharer: each viewer's).
    SessionConfig& setSendAudio(bool enabled) {
        sendAudio().enabled = enabled ? 1u : 0u;
        return *this;
    }
    SessionConfig& setReceiveAudio(bool enabled) {
        receiveAudio().enabled = enabled ? 1u : 0u;
        return *this;
    }
    // Sends a microphone: 0 for the system's default input, else an input of
    // audioDevices(); voice processing cancels the echo of what this
    // machine plays (see rcs_send_audio_config).
    SessionConfig& setMicrophone(uint32_t device = 0, bool voiceProcessing = true) {
        auto& send = sendAudio();
        send.enabled = 1u;
        send.source = RCS_AUDIO_MICROPHONE;
        send.device = device;
        send.voice_processing = voiceProcessing ? 1u : 0u;
        return *this;
    }
    // The audio codecs this end offers both ways, most preferred first.
    SessionConfig& setAudioCodecs(std::initializer_list<rcs_audio_codec> codecs) {
        detail::setList(sendAudio().codecs, codecs, "audio codecs");
        detail::setList(receiveAudio().codecs, codecs, "audio codecs");
        return *this;
    }
    // The sound this end sends: its Opus/AAC bitrate and where it comes from
    // (a viewer: RCS_AUDIO_MICROPHONE or RCS_AUDIO_HOST).
    SessionConfig& setAudioBitrate(uint32_t bitsPerSecond) {
        sendAudio().bitrate_bps = bitsPerSecond;
        return *this;
    }
    SessionConfig& setAudioSource(rcs_audio_source source) {
        sendAudio().source = source;
        return *this;
    }
    // Leave this application's own sounds out (shared by default).
    SessionConfig& setExcludeOwnSound(bool exclude) {
        sendAudio().exclude_current_process = exclude ? 1u : 0u;
        return *this;
    }
    // The host's sound as Core Media sample buffers (rcsdk/apple.h): the
    // volume of what the device or the applications play and of the
    // microphone in the mix, in percent (0..400, both 100 by default); 0
    // leaves one out.
    SessionConfig& setSampleBufferVolumes(uint32_t audioPercent, uint32_t microphonePercent) {
        auto& send = sendAudio();
        send.sample_buffer_audio_percent = audioPercent;
        send.sample_buffer_microphone_percent = microphonePercent;
        return *this;
    }
    // The sound this end receives: false leaves playback to the host
    // (Callbacks::onAudio).
    SessionConfig& setAudioPlayback(bool sdkPlays) {
        receiveAudio().playback = sdkPlays ? 1u : 0u;
        return *this;
    }
    // RELAY: the TURN server ("host:port"), its credentials and how to reach
    // it (RCS_TURN_TCP where only TCP passes, RCS_TURN_TLS where little but
    // HTTPS does).
    SessionConfig& setRelay(const std::string& server, const std::string& username, const std::string& password,
                            rcs_turn_transport turnTransport = RCS_TURN_UDP) {
        auto& t = iceServers();
        config_.connect.transport = RCS_TRANSPORT_RELAY;
        t.turn_transport = turnTransport;
        detail::setText(t.turn_server, server, "turn_server");
        detail::setText(t.turn_username, username, "turn_username");
        detail::setText(t.turn_password, password, "turn_password");
        return *this;
    }
    // RCS_TURN_TLS: accept exactly the TURN server's certificate with this
    // SHA-256 (e.g. a self-signed one) instead of the system's trust; all zero
    // goes back to the system's trust (see rcs_ice_servers_config).
    SessionConfig& setRelayCertificate(const CertificateSha256& sha256) {
        std::copy(sha256.begin(), sha256.end(), iceServers().turn_certificate_sha256);
        return *this;
    }
    // RCS_TURN_TLS: the TURN server's name, sent to it and (without a pinned
    // certificate) checked; empty: the host part of the server's address.
    SessionConfig& setRelayServerName(const std::string& name) {
        detail::setText(iceServers().turn_tls_server_name, name, "turn_tls_server_name");
        return *this;
    }
    // FORWARDER: through the forwarder at `address`:`port` (an address or a
    // name; over UDP, or `link` TCP: its TCP entry), whose public key has the
    // SHA-256 `keySha256`, into the room `token` names (rcs_forwarder_config).
    SessionConfig& setForwarder(const std::string& address, uint32_t port, const std::array<uint8_t, 32>& keySha256,
                                const std::string& token, rcs_forwarder_link link = RCS_FORWARDER_UDP) {
        auto& f = extension<rcs_forwarder_config>();
        setAddress(address, port);
        config_.connect.transport = RCS_TRANSPORT_FORWARDER;
        f.link = link;
        f.websocket_url[0] = 0;
        std::fill(std::begin(f.tls_certificate_sha256), std::end(f.tls_certificate_sha256), uint8_t(0));
        std::copy(keySha256.begin(), keySha256.end(), f.forwarder_key_sha256);
        detail::setText(f.token, token, "token");
        return *this;
    }
    // FORWARDER over a WebSocket: `url` "ws://host[:port]/path" (its
    // WebSocket entry) or "wss://..." (a TLS proxy in front of it), the
    // proxy's certificate checked by the system's trust store and the URL's
    // host, or `tlsSha256` (all zero: none) for one it does not trust.
    SessionConfig& setForwarderWebSocket(const std::string& url, const std::array<uint8_t, 32>& keySha256,
                                         const std::string& token, const std::array<uint8_t, 32>& tlsSha256 = {}) {
        auto& f = extension<rcs_forwarder_config>();
        setAddress("", 0);
        config_.connect.transport = RCS_TRANSPORT_FORWARDER;
        f.link = RCS_FORWARDER_WEBSOCKET;
        detail::setText(f.websocket_url, url, "websocket_url");
        std::copy(tlsSha256.begin(), tlsSha256.end(), f.tls_certificate_sha256);
        std::copy(keySha256.begin(), keySha256.end(), f.forwarder_key_sha256);
        detail::setText(f.token, token, "token");
        return *this;
    }
    // FORWARDER over the host's own channel to it (RCS_FORWARDER_HOST): the
    // channel goes in with rcsdk::hostTransport (rcsdk/host_transport.hpp),
    // DATAGRAM to its WebSocket entry or UDP port, STREAM to its TCP entry.
    SessionConfig& setForwarderHost(const std::array<uint8_t, 32>& keySha256, const std::string& token) {
        auto& f = extension<rcs_forwarder_config>();
        setAddress("", 0);
        config_.connect.transport = RCS_TRANSPORT_FORWARDER;
        f.link = RCS_FORWARDER_HOST;
        f.websocket_url[0] = 0;
        std::fill(std::begin(f.tls_certificate_sha256), std::end(f.tls_certificate_sha256), uint8_t(0));
        std::copy(keySha256.begin(), keySha256.end(), f.forwarder_key_sha256);
        detail::setText(f.token, token, "token");
        return *this;
    }
    // P2P: "host:port" STUN servers; at most RCS_MAX_STUN_SERVERS.
    SessionConfig& setStunServers(const std::vector<std::string>& servers) {
        if (servers.size() > RCS_MAX_STUN_SERVERS) {
            throw Error(RCS_ERROR_INVALID_ARGUMENT, "stun_servers: at most RCS_MAX_STUN_SERVERS (4)");
        }
        auto& t = iceServers();
        for (size_t i = 0; i < RCS_MAX_STUN_SERVERS; ++i) {
            detail::setText(t.stun_servers[i], i < servers.size() ? servers[i] : std::string(), "stun_servers");
        }
        return *this;
    }
    // Share: listen for the next viewer when one leaves (on by default).
    SessionConfig& setKeepListening(bool keep) {
        reconnect().keep_listening = keep ? 1u : 0u;
        return *this;
    }
    // View: reconnect when the path breaks (on by default; how long and how
    // often: rcs_network_tuning_config).
    SessionConfig& setReconnect(bool enabled) {
        reconnect().reconnect = enabled ? 1u : 0u;
        return *this;
    }
    // Keep the connection when the network changes (rcs_paths_config; on by default).
    SessionConfig& setMigrate(bool enabled) {
        paths().migrate = enabled ? 1u : 0u;
        return *this;
    }

    // A module this session uses (registerModule first), with its settings,
    // copied (see rcs_module_config); the same id again replaces them, e.g.
    // for Session::update. A module's own header has a typed way to it.
    SessionConfig& useModule(const std::string& id, const void* settings, uint32_t size) {
        const auto* bytes = static_cast<const uint8_t*>(settings);
        for (auto& module : modules_) {
            if (module.id == id) {
                module.settings.assign(bytes, bytes + size);
                return *this;
            }
        }
        modules_.push_back({id, std::vector<uint8_t>(bytes, bytes + size), RCS_MODULE_CONFIG_INIT});
        return *this;
    }
    template <class Settings>
    SessionConfig& useModule(const std::string& id, const Settings& settings) {
        static_assert(std::is_trivially_copyable_v<Settings>, "a module's settings are copied byte for byte");
        return useModule(id, &settings, uint32_t(sizeof(settings)));
    }

    // How much this session logs: 0 as the process (setLogCallback), or its
    // own level (see rcs_log_config).
    SessionConfig& setLogLevel(rcs_log_level level) {
        log().max_level = level;
        return *this;
    }

    // ---- Sharing -----------------------------------------------------------

    // The stream sent unless the viewer asks for another.
    SessionConfig& setVideo(uint32_t width, uint32_t height, uint32_t framesPerSecond, uint32_t bitrateBps) {
        auto& v = shareVideo();
        v.width = width;
        v.height = height;
        v.frames_per_second = framesPerSecond;
        v.bitrate_bps = bitrateBps;
        return *this;
    }
    // How the bitrate follows the network; 0 keeps a default.
    SessionConfig& setRateMode(rcs_rate_mode mode, uint32_t minimumBps = 0) {
        auto& v = shareVideo();
        v.rate_mode = mode;
        v.min_bitrate_bps = minimumBps;
        return *this;
    }
    // Whether the viewer's requests are followed, and their limits; 0 limits
    // are the display's own (see rcs_share_video_config).
    SessionConfig& setRequestPolicy(bool followRequests, uint32_t maxWidth = 0, uint32_t maxHeight = 0,
                                    uint32_t maxFramesPerSecond = 0, uint32_t maxBitrateBps = 0) {
        auto& v = shareVideo();
        v.follow_requests = followRequests ? 1u : 0u;
        v.max_width = maxWidth;
        v.max_height = maxHeight;
        v.max_frames_per_second = maxFramesPerSecond;
        v.max_bitrate_bps = maxBitrateBps;
        return *this;
    }
    // How pictures are written as YCbCr: SDR pictures' matrix (BT709, BT601),
    // and the range of every format.
    SessionConfig& setColor(rcs_color_matrix matrix, rcs_color_range range) {
        encoder().matrix = matrix;
        encoder().range = range;
        return *this;
    }
    // What is shared (see rcs_capture_config); HOST and ENCODED need nothing more.
    SessionConfig& setCaptureSource(rcs_capture_source source) {
        capture().source = source;
        return *this;
    }
    // A display; 0 is the main display.
    SessionConfig& setCaptureDisplay(uint32_t displayId) {
        capture().source = RCS_CAPTURE_DISPLAY;
        capture().display_id = displayId;
        return *this;
    }
    // A rectangle of a display, in the display's pixels from its top left.
    SessionConfig& setCaptureRegion(uint32_t displayId, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        auto& c = capture();
        c.source = RCS_CAPTURE_REGION;
        c.display_id = displayId;
        c.region_x = x;
        c.region_y = y;
        c.region_width = width;
        c.region_height = height;
        return *this;
    }
    // One window: rcs_window_info.window_id (see windows()).
    SessionConfig& setCaptureWindow(uint64_t windowId) {
        capture().source = RCS_CAPTURE_WINDOW;
        capture().window_id = windowId;
        return *this;
    }
    // Every window of one process (rcs_window_info.process_id) on a display.
    SessionConfig& setCaptureApplication(uint32_t processId, uint32_t displayId = 0) {
        capture().source = RCS_CAPTURE_APPLICATION;
        capture().process_id = processId;
        capture().display_id = displayId;
        return *this;
    }
    // Hide this application's windows from the viewer (shown by default).
    SessionConfig& setExcludeOwnWindows(bool exclude) {
        capture().exclude_current_process = exclude ? 1u : 0u;
        return *this;
    }
    // Windows: capture and remote input follow secure desktops (sign-in and
    // lock screens, UAC prompts); needs a LocalSystem process (see rcs_capture_config).
    SessionConfig& setSecureDesktop(bool follow) {
        capture().secure_desktop = follow ? 1u : 0u;
        return *this;
    }
    // More streams, each a whole display (rcs_streams_config): streams 1, 2,
    // ... share `displays` in order, beside stream 0 (setCaptureDisplay);
    // `weights` share the bitrate (stream 0 first; empty: by pixels).
    SessionConfig& setStreams(std::initializer_list<uint32_t> displays,
                              std::initializer_list<uint32_t> weights = {}) {
        auto& s = streams();
        detail::setList(s.displays, displays, "displays");
        detail::setList(s.weights, weights, "weights");
        return *this;
    }
    // How many viewers at once (0: the default, 64; up to RCS_MAX_VIEWERS),
    // and whose path the bitrate follows (see rcs_viewers_config).
    SessionConfig& setViewers(uint32_t maxViewers, rcs_bitrate_from bitrateFrom = RCS_BITRATE_FROM_QUORUM) {
        auto& v = viewers();
        v.max_viewers = maxViewers;
        v.bitrate_from = bitrateFrom;
        return *this;
    }
    // Remote control: whether a viewer may control (each that joins, unless
    // Callbacks::onViewerJoin decides), and who executes its input.
    SessionConfig& setControl(bool allowed, rcs_input_target target = RCS_INPUT_SYSTEM) {
        auto& c = control();
        c.allowed = allowed ? 1u : 0u;
        c.target = target;
        return *this;
    }
    // Target SYSTEM: the input types the host executes itself (Callbacks::onInput),
    // as (1u << rcs_input_type) bits, e.g. 1u << RCS_INPUT_PEN; the system
    // executes the rest.
    SessionConfig& setHostInputTypes(uint32_t types) {
        control().host_input_types = types;
        return *this;
    }
    // What the viewer's Caps Lock does (rcs_caps_lock), and with INPUT_SOURCE
    // how long it is held to toggle Caps Lock instead (0: 500 ms).
    SessionConfig& setCapsLock(rcs_caps_lock capsLock, uint32_t holdMs = 0) {
        auto& c = control();
        c.caps_lock = capsLock;
        c.caps_lock_hold_ms = holdMs;
        return *this;
    }

    // ---- Viewing -----------------------------------------------------------

    // The picture to ask the sharer for; 0 leaves a choice to it.
    SessionConfig& setVideoRequest(uint32_t width, uint32_t height, uint32_t framesPerSecond = 0,
                                   uint32_t bitrateBps = 0, rcs_codec codec = 0, rcs_picture_format format = 0) {
        auto& r = viewRequest();
        r.width = width;
        r.height = height;
        r.frames_per_second = framesPerSecond;
        r.bitrate_bps = bitrateBps;
        r.codec = codec;
        r.format = format;
        return *this;
    }
    // The sound to ask the sharer for; 0 leaves a choice to it.
    SessionConfig& setAudioRequest(rcs_audio_codec codec, uint32_t bitrateBps = 0) {
        auto& r = viewRequest();
        r.audio_codec = codec;
        r.audio_bitrate_bps = bitrateBps;
        return *this;
    }
    // The streams to receive (rcs_streams_config), as (1u << stream) bits; 0: all.
    SessionConfig& setReceiveStreams(uint32_t streams) {
        viewRequest().streams = streams;
        return *this;
    }
    // RCS_CURSOR_SEPARATE leaves the pointer out of the picture and reports
    // it (RCS_EVENT_CURSOR) for the host to draw.
    SessionConfig& setCursorMode(rcs_cursor_mode mode) {
        viewRequest().cursor = mode;
        return *this;
    }

    // ---- The structures ----------------------------------------------------

    rcs_session_config& core() noexcept { return config_; }
    rcs_share_video_config& shareVideo() { return extension<rcs_share_video_config>(); }
    rcs_encoder_config& encoder() { return extension<rcs_encoder_config>(); }
    rcs_capture_config& capture() { return extension<rcs_capture_config>(); }
    rcs_streams_config& streams() { return extension<rcs_streams_config>(); }
    rcs_send_audio_config& sendAudio() { return extension<rcs_send_audio_config>(); }
    rcs_control_config& control() { return extension<rcs_control_config>(); }
    rcs_viewers_config& viewers() { return extension<rcs_viewers_config>(); }
    rcs_encodings_config& encodings() { return extension<rcs_encodings_config>(); }
    rcs_view_request_config& viewRequest() { return extension<rcs_view_request_config>(); }
    rcs_decoder_config& decoder() { return extension<rcs_decoder_config>(); }
    rcs_background_config& background() { return extension<rcs_background_config>(); }
    rcs_receive_audio_config& receiveAudio() { return extension<rcs_receive_audio_config>(); }
    rcs_ice_servers_config& iceServers() { return extension<rcs_ice_servers_config>(); }
    rcs_reconnect_config& reconnect() { return extension<rcs_reconnect_config>(); }
    rcs_paths_config& paths() { return extension<rcs_paths_config>(); }
    // trusted_keys is set from setTrustedKeys when the chain is built.
    rcs_device_identity_config& deviceIdentity() { return extension<rcs_device_identity_config>(); }
    rcs_log_config& log() { return extension<rcs_log_config>(); }

    // Any extension by its structure, chained from now on: the one touched
    // before, or a new one with the preset's settings where it has some, else
    // its defaults<Extension>(); the sound a role has off without it (a
    // viewer's sending, a sharer's receiving) starts off. E.g. with
    // rcsdk/tuning.hpp:
    //   config.extension<rcs_video_tuning_config>().input_boost_ms = 100;
    template <class Extension>
    Extension& extension() {
        const Extension initial = defaults<Extension>();
        for (const auto& held : extensions_) {
            if (held->header()->type == initial.header.type) {
                return static_cast<Held<Extension>&>(*held).value;
            }
        }
        auto held = std::make_unique<Held<Extension>>(initial);
        if (detail::presetHas(initial.header.type)) {
            check(rcs_preset_defaults(config_.preset, &held->value.header));
        }
        auto& value = held->value;
        if constexpr (std::is_same_v<Extension, rcs_send_audio_config>) {
            value.enabled = sharing() ? 1u : 0u;
        } else if constexpr (std::is_same_v<Extension, rcs_receive_audio_config>) {
            value.enabled = sharing() ? 0u : 1u;
        }
        extensions_.push_back(std::move(held));
        return value;
    }
    // Chains `extension` as it is, in place of one of its type touched before.
    template <class Extension>
    SessionConfig& use(const Extension& extension) {
        this->extension<Extension>() = extension;
        return *this;
    }

    // The configuration with its chain, pointing into this object: valid until
    // it is changed, copied from or destroyed. Session::update(const
    // SessionConfig&) passes the settings of a running session without it.
    const rcs_session_config& get() const noexcept {
        config_.extensions = chain(false);
        return config_;
    }

private:
    friend class Session;
    bool sharing() const noexcept { return config_.role == RCS_ROLE_SHARE; }

    // One extension of any type, held where the chain can point at it.
    struct Stored {
        virtual ~Stored() = default;
        virtual rcs_extension* header() noexcept = 0;
        virtual std::unique_ptr<Stored> clone() const = 0;
    };
    template <class T>
    struct Held final : Stored {
        explicit Held(const T& initial) : value(initial) {}
        rcs_extension* header() noexcept override { return &value.header; }
        std::unique_ptr<Stored> clone() const override { return std::make_unique<Held>(value); }
        T value;
    };

    const rcs_extension* chain(bool runtimeOnly) const noexcept {
        const rcs_extension* head = nullptr;
        for (const auto& held : extensions_) {
            rcs_extension* header = held->header();
            if (runtimeOnly && detail::fixedForRun(header->type)) {
                continue;
            }
            if (header->type == RCS_EXT_DEVICE_IDENTITY) {
                auto& identity = reinterpret_cast<rcs_device_identity_config&>(*header);
                identity.trusted_key_count = uint32_t(trusted_.size());
                identity.trusted_keys = trusted_.empty() ? nullptr : trusted_.front().data();
            }
            header->next = head;
            head = header;
        }
        for (auto& module : modules_) { // Each module's settings change while running too.
            module.config.module = module.id.c_str();
            module.config.settings = module.settings.empty() ? nullptr : module.settings.data();
            module.config.size = uint32_t(module.settings.size());
            module.config.header.next = head;
            head = &module.config.header;
        }
        return head;
    }

    mutable rcs_session_config config_ = RCS_SESSION_CONFIG_INIT;
    std::vector<std::unique_ptr<Stored>> extensions_; // In the order touched.
    std::vector<PublicKey> trusted_;
    struct Module {
        std::string id;
        std::vector<uint8_t> settings;
        rcs_module_config config; // Pointing into the above when the chain is built.
    };
    mutable std::vector<Module> modules_;
};

// ---- Frames ----------------------------------------------------------------

// Owning reference to a native GPU frame.
class GpuFrame final {
public:
    GpuFrame() = default;
    // Takes a new reference to a frame borrowed from a callback.
    explicit GpuFrame(rcs_gpu_frame borrowed) : handle_(borrowed) {
        if (handle_) {
            rcs_gpu_frame_retain(handle_);
        }
    }
    GpuFrame(const GpuFrame& other) : GpuFrame(other.handle_) {}
    GpuFrame(GpuFrame&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    GpuFrame& operator=(GpuFrame other) noexcept {
        std::swap(handle_, other.handle_);
        return *this;
    }
    ~GpuFrame() { rcs_gpu_frame_release(handle_); }

    explicit operator bool() const noexcept { return handle_ != nullptr; }
    rcs_gpu_frame handle() const noexcept { return handle_; }
    rcs_gpu_frame_info info() const noexcept {
        rcs_gpu_frame_info value = RCS_GPU_FRAME_INFO_INIT;
        rcs_gpu_frame_get_info(handle_, &value);
        return value;
    }
    rcs_result wait(uint32_t timeoutMs) const noexcept { return rcs_gpu_frame_wait(handle_, timeoutMs); }

private:
    rcs_gpu_frame handle_ = nullptr;
};

// ---- Session ---------------------------------------------------------------

// Session callbacks. Leave a callback empty to disable it. Set at most one of
// onFrame and onGpuFrame; with neither, no images are delivered. Callbacks run
// on SDK threads and must not throw: an escaping exception terminates the
// process. onEvent calls never overlap. The replacing layer's callbacks are in
// their extensions (rcsdk/host_codec.hpp, rcsdk/rate_control.hpp,
// rcsdk/host_transport.hpp, rcsdk/agent.hpp).
struct Callbacks {
    std::function<void(const rcs_event&)> onEvent;
    std::function<void(const rcs_frame&)> onFrame;
    std::function<void(const GpuFrame&)> onGpuFrame;
    // The other end's decoded sound; a sharer's names the viewer (see
    // rcs_session_callbacks.on_audio).
    std::function<void(const rcs_audio_frame&)> onAudio;
    // Share: every remote input event. Target SYSTEM: true executes it, except
    // the types of host_input_types, which the host executes (the result is
    // ignored); target HOST: the host executes it, the result is ignored.
    std::function<bool(const rcs_input_event&)> onInput;
    // A data message from the other end (see rcs_session_callbacks.on_data).
    std::function<void(const rcs_data_message&)> onData;
    // P2P and RELAY: deliver the text to the other end (its Session::signal):
    // a sharer's answer goes back by `route`, the one the host passed with
    // the viewer's signal; a viewer's route is 0 (see
    // rcs_session_callbacks.on_signal).
    std::function<void(const std::string& text, uint64_t route)> onSignal;
    // Share: a viewer is about to join; change the decision to refuse it, to
    // decide its control or to decide later with Session::admitViewer (see
    // rcs_session_callbacks.on_viewer_join).
    std::function<void(const rcs_viewer_info&, rcs_viewer_decision&)> onViewerJoin;
};

// The statistics section of each structure, for Session::stats<T>().
template <class T>
struct StatsSection;
template <>
struct StatsSection<rcs_capture_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_CAPTURE;
};
template <>
struct StatsSection<rcs_video_send_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_VIDEO_SEND;
};
template <>
struct StatsSection<rcs_audio_send_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_AUDIO_SEND;
};
template <>
struct StatsSection<rcs_video_receive_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_VIDEO_RECEIVE;
};
template <>
struct StatsSection<rcs_audio_receive_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_AUDIO_RECEIVE;
};
template <>
struct StatsSection<rcs_network_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_NETWORK;
};
template <>
struct StatsSection<rcs_input_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_INPUT;
};
template <>
struct StatsSection<rcs_data_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_DATA;
};
template <>
struct StatsSection<rcs_group_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_GROUP;
};
template <>
struct StatsSection<rcs_viewer_video_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_VIEWER_VIDEO;
};
template <>
struct StatsSection<rcs_forwarder_stats> {
    static constexpr rcs_stats_section value = RCS_STATS_FORWARDER;
};

// Why the most recent start() or run failed (rcs_session_get_failure).
struct SessionFailure {
    rcs_result result = RCS_OK;
    std::string message;
    explicit operator bool() const noexcept { return result != RCS_OK; }
};

// Owns a session. Not copyable or movable: callbacks capture its address.
class Session final {
public:
    explicit Session(const SessionConfig& config, Callbacks callbacks = {})
        : Session(config.get(), std::move(callbacks)) {}
    explicit Session(const rcs_session_config& config, Callbacks callbacks = {})
        : callbacks_(std::make_unique<Callbacks>(std::move(callbacks))) {
        rcs_session_callbacks table = RCS_SESSION_CALLBACKS_INIT;
        table.user_data = callbacks_.get();
        if (callbacks_->onEvent) {
            table.on_event = [](const rcs_event* event, void* data) noexcept { self(data).onEvent(*event); };
        }
        if (callbacks_->onFrame) {
            table.on_frame = [](const rcs_frame* frame, void* data) noexcept { self(data).onFrame(*frame); };
        }
        if (callbacks_->onGpuFrame) {
            table.on_gpu_frame = [](rcs_gpu_frame frame, void* data) noexcept {
                self(data).onGpuFrame(GpuFrame(frame));
            };
        }
        if (callbacks_->onAudio) {
            table.on_audio = [](const rcs_audio_frame* frame, void* data) noexcept { self(data).onAudio(*frame); };
        }
        if (callbacks_->onInput) {
            table.on_input = [](const rcs_input_event* event, void* data) noexcept -> int32_t {
                return self(data).onInput(*event) ? 1 : 0;
            };
        }
        if (callbacks_->onData) {
            table.on_data = [](const rcs_data_message* message, void* data) noexcept {
                self(data).onData(*message);
            };
        }
        if (callbacks_->onSignal) {
            table.on_signal = [](const char* signal, uint64_t route, void* data) noexcept {
                self(data).onSignal(signal, route);
            };
        }
        if (callbacks_->onViewerJoin) {
            table.on_viewer_join = [](const rcs_viewer_info* viewer, rcs_viewer_decision* decision,
                                      void* data) noexcept {
                self(data).onViewerJoin(*viewer, *decision);
            };
        }
        check(rcs_session_create(&config, &table, &handle_));
    }
    // Destroying a session from its own callback is a programming error.
    ~Session() noexcept {
        if (rcs_session_destroy(handle_) != RCS_OK) {
            std::terminate();
        }
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- Lifecycle ---------------------------------------------------------

    rcs_result start() noexcept { return rcs_session_start(handle_); }
    // RCS_STOP_RESTART tells the peer this end will be back (it reports
    // RCS_ERROR_RESTARTING and a viewer reconnects by itself).
    rcs_result stop(rcs_stop_reason reason = RCS_STOP_NORMAL) noexcept {
        return rcs_session_stop_with_reason(handle_, reason);
    }

    // rcs_session_update: all of the given extensions or none, e.g.
    //   session.update(video, audio);
    rcs_result update(const rcs_extension* changes) noexcept { return rcs_session_update(handle_, changes); }
    // Every extension `config` touched that a running session may change (all
    // but the replacing layer's), e.g. after changing the settings of the
    // SessionConfig the session was created with. Works on a copy: pointers
    // `config.get()` handed out stay valid.
    rcs_result update(const SessionConfig& config) {
        SessionConfig copy(config);
        return update(copy.chain(true));
    }
    template <class... Extensions>
    rcs_result update(const Extensions&... extensions) noexcept {
        std::tuple<Extensions...> copies(extensions...);
        const rcs_extension* head = nullptr;
        std::apply(
            [&](auto&... copy) {
                ((copy.header.next = head, head = &copy.header), ...);
            },
            copies);
        return update(head);
    }
    // Another preset for the extensions the host has not chained (rcs_session_set_preset).
    rcs_result setPreset(rcs_preset preset) noexcept { return rcs_session_set_preset(handle_, preset); }
    // Another identity, from the next handshake (rcs_session_set_identity),
    // e.g. a new PIN; the core's identity of `config`.
    rcs_result setIdentity(const rcs_identity& identity) noexcept {
        return rcs_session_set_identity(handle_, &identity);
    }

    // ---- Queries -----------------------------------------------------------

    rcs_state state() const noexcept {
        rcs_state value = RCS_STATE_FAILED;
        rcs_session_get_state(handle_, &value);
        return value;
    }
    SessionFailure failure() const {
        SessionFailure value;
        char message[1024] = {};
        rcs_session_get_failure(handle_, &value.result, message, sizeof(message));
        value.message = message;
        return value;
    }
    // The settings of one extension the session uses now, the preset's where
    // the host chained none (rcs_session_get_settings; throws Error), e.g.
    // settings<rcs_share_video_config>().
    template <class Extension>
    Extension settings() const {
        Extension value = defaults<Extension>();
        check(rcs_session_get_settings(handle_, &value.header));
        return value;
    }
    // One statistics section, e.g. stats<rcs_video_receive_stats>(); all zero
    // (beyond struct_size) when the session's role has no such section.
    template <class T>
    T stats() const noexcept {
        T value{};
        value.struct_size = sizeof(T);
        rcs_session_get_stats(handle_, StatsSection<T>::value, &value);
        return value;
    }
    // One stream's (rcs_streams_config): rcs_capture_stats, rcs_video_send_stats, rcs_video_receive_stats.
    template <class T>
    T streamStats(uint32_t stream) const noexcept {
        T value{};
        value.struct_size = sizeof(T);
        rcs_session_get_stream_stats(handle_, stream, StatsSection<T>::value, &value);
        return value;
    }
    // Share: one encoding's encoder statistics (RCS_EXT_ENCODINGS); all zero
    // where the call fails (an encoding that does not run).
    rcs_video_send_stats encodingStats(uint32_t stream, uint32_t encoding) const noexcept {
        rcs_video_send_stats value = RCS_VIDEO_SEND_STATS_INIT;
        rcs_session_get_encoding_stats(handle_, stream, encoding, &value);
        return value;
    }
    // A module's messages (see rcs_session_get_module_stats): the session's, or one viewer's.
    rcs_module_stats moduleStats(const std::string& module, rcs_viewer_id viewer = 0) const noexcept {
        rcs_module_stats value = RCS_MODULE_STATS_INIT;
        rcs_session_get_module_stats(handle_, viewer, module.c_str(), &value);
        return value;
    }
    // A stream as last announced (rcs_streams_config; 0 without it).
    rcs_stream_info streamInfo(uint32_t stream = 0) const noexcept {
        rcs_stream_info value = RCS_STREAM_INFO_INIT;
        rcs_session_get_stream_info(handle_, stream, &value);
        return value;
    }
    rcs_peer_info peerInfo() const noexcept {
        rcs_peer_info value = RCS_PEER_INFO_INIT;
        rcs_session_get_peer_info(handle_, &value);
        return value;
    }
    rcs_control_info controlInfo() const noexcept {
        rcs_control_info value = RCS_CONTROL_INFO_INIT;
        rcs_session_get_control_info(handle_, &value);
        return value;
    }
    // What the shared machine shows (Windows: the user's desktop or a secure
    // one); UNKNOWN where the system does not say, and for a viewer until the
    // sharer has.
    rcs_desktop_info desktopInfo() const noexcept {
        rcs_desktop_info value = RCS_DESKTOP_INFO_INIT;
        rcs_session_get_desktop_info(handle_, &value);
        return value;
    }
    // Share: the displays of the machine the session captures (empty before
    // a remote agent attached).
    std::vector<rcs_display_info> displays() const {
        return detail::list<rcs_display_info>([this](rcs_display_info* items, uint32_t capacity, uint32_t* count) {
            return rcs_session_list_displays(handle_, items, sizeof(rcs_display_info), capacity, count);
        });
    }
    // Share: the viewers connected now, in the order they joined.
    std::vector<rcs_viewer_info> viewers() const {
        return detail::list<rcs_viewer_info>([this](rcs_viewer_info* items, uint32_t capacity, uint32_t* count) {
            return rcs_session_list_viewers(handle_, items, sizeof(rcs_viewer_info), capacity, count);
        });
    }
    // Share: one viewer's statistics, of the sections of its connection
    // (NETWORK, AUDIO_SEND, AUDIO_RECEIVE, DATA, INPUT) and VIEWER_VIDEO; all
    // zero where the call fails (an id never given, a viewer that left before
    // the last 256 that left).
    template <class T>
    T viewerStats(rcs_viewer_id viewer) const noexcept {
        T value{};
        value.struct_size = sizeof(T);
        rcs_session_get_viewer_stats(handle_, viewer, StatsSection<T>::value, &value);
        return value;
    }
    // The connection's path now (rcs_session_get_path_info: a viewer's, or a
    // sharer's to `viewer`, 0 the main one); route NONE where the call fails.
    rcs_path_info pathInfo(rcs_viewer_id viewer = 0) const noexcept {
        rcs_path_info active{};
        rcs_session_get_path_info(handle_, viewer, &active, nullptr);
        return active;
    }
    // Share: the windows of the machine the session captures (throws Error:
    // INVALID_STATE while a remote agent is not attached, TIMEOUT).
    std::vector<rcs_window_info> windows() const {
        return detail::list<rcs_window_info>([this](rcs_window_info* items, uint32_t capacity, uint32_t* count) {
            return rcs_session_list_windows(handle_, items, sizeof(rcs_window_info), capacity, count);
        });
    }

    // ---- Actions -----------------------------------------------------------

    // P2P and RELAY: the other end's signal. Share: `route` is the host's for
    // where it came from, and comes back with the answer (onSignal) and in the
    // viewer's rcs_viewer_info.route; view: not read.
    rcs_result signal(const std::string& text, uint64_t route = 0) noexcept {
        return rcs_session_signal(handle_, text.c_str(), route);
    }
    rcs_result sendInput(const rcs_input_event& event) noexcept { return rcs_session_send_input(handle_, &event); }
    rcs_result requestKeyframe() noexcept { return rcs_session_request_keyframe(handle_); }
    // The host's pause of `what` (RCS_PAUSE_* bits), and its end: a viewer's
    // own flows, or a sharer's for every viewer (see rcs_session_pause).
    rcs_result pause(uint32_t what) noexcept { return rcs_session_pause(handle_, what); }
    rcs_result resume(uint32_t what) noexcept { return rcs_session_resume(handle_, what); }
    // Share with capture source HOST: the next picture, BGRA8 of the stream's size.
    rcs_result pushVideoFrame(const rcs_frame& frame) noexcept { return rcs_session_push_video_frame(handle_, &frame); }
    rcs_result pushVideoFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t strideBytes) noexcept {
        rcs_frame frame = RCS_FRAME_INIT;
        frame.width = width;
        frame.height = height;
        frame.stride_bytes = strideBytes;
        frame.data = bgra;
        frame.data_size = uint64_t(strideBytes) * height;
        return pushVideoFrame(frame);
    }
    // Sending with audio source HOST: 48 kHz stereo interleaved float32.
    rcs_result pushAudio(const rcs_audio_frame& frame) noexcept { return rcs_session_push_audio(handle_, &frame); }
    rcs_result pushAudio(const float* samples, uint32_t frames) noexcept {
        rcs_audio_frame frame = RCS_AUDIO_FRAME_INIT;
        frame.frames = frames;
        frame.samples = samples;
        return pushAudio(frame);
    }

    // A data message for the other end's onData (see rcs_session_send_data).
    rcs_result sendData(const rcs_data_message& message) noexcept { return rcs_session_send_data(handle_, &message); }
    rcs_result sendData(uint32_t channel, const void* data, uint32_t size, rcs_data_flags flags = 0) noexcept {
        rcs_data_message message = RCS_DATA_MESSAGE_INIT;
        message.channel = channel;
        message.flags = flags;
        message.size = size;
        message.data = static_cast<const uint8_t*>(data);
        return sendData(message);
    }
    rcs_result sendData(uint32_t channel, std::string_view text, rcs_data_flags flags = 0) noexcept {
        return sendData(channel, text.data(), uint32_t(text.size()), flags);
    }
    // Share: to one viewer only (rcs_data_message.viewer_id).
    rcs_result sendDataTo(rcs_viewer_id viewer, uint32_t channel, const void* data, uint32_t size,
                          rcs_data_flags flags = 0) noexcept {
        rcs_data_message message = RCS_DATA_MESSAGE_INIT;
        message.viewer_id = viewer;
        message.channel = channel;
        message.flags = flags;
        message.size = size;
        message.data = static_cast<const uint8_t*>(data);
        return sendData(message);
    }
    rcs_result sendDataTo(rcs_viewer_id viewer, uint32_t channel, std::string_view text,
                          rcs_data_flags flags = 0) noexcept {
        return sendDataTo(viewer, channel, text.data(), uint32_t(text.size()), flags);
    }

    // Share: one viewer (see rcs_session_disconnect_viewer and the functions after it).
    rcs_result disconnectViewer(rcs_viewer_id viewer) noexcept {
        return rcs_session_disconnect_viewer(handle_, viewer);
    }
    rcs_result setViewerControl(rcs_viewer_id viewer, bool allowed) noexcept {
        return rcs_session_set_viewer_control(handle_, viewer, allowed ? 1u : 0u);
    }
    rcs_result setMainViewer(rcs_viewer_id viewer) noexcept { return rcs_session_set_main_viewer(handle_, viewer); }
    rcs_result setViewerVideo(rcs_viewer_id viewer, const rcs_viewer_video& video) noexcept {
        return rcs_session_set_viewer_video(handle_, viewer, &video);
    }
    // The decision for a viewer that waits (RCS_ADMIT_LATER; see rcs_session_admit_viewer).
    rcs_result admitViewer(rcs_viewer_id viewer, const rcs_viewer_decision& decision) noexcept {
        return rcs_session_admit_viewer(handle_, viewer, &decision);
    }

    rcs_session handle() const noexcept { return handle_; }

private:
    static Callbacks& self(void* data) noexcept { return *static_cast<Callbacks*>(data); }

    std::unique_ptr<Callbacks> callbacks_;
    rcs_session handle_ = nullptr;
};

} // namespace rcsdk
