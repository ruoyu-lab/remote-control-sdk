#include "session/checks.hpp"

#include "session/negotiation.hpp"
#include "session/stream_plan.hpp"
#include "seams/seams.hpp"

#include <algorithm>
#include <iterator>

namespace rcsdk::session {
namespace {

template <class Codec>
bool common(const std::vector<Codec>& offer, uint32_t peer) noexcept {
    return !peer || std::any_of(offer.begin(), offer.end(), [&](Codec c) { return peer & (1u << c); });
}

// Something listed is offered by the peer (unknown before it connected).
Refusal checkPeerCodecs(const char* extension, std::span<const rcs_codec> codecs, uint32_t peer) {
    std::vector<rcs_codec> listed;
    std::copy_if(codecs.begin(), codecs.end(), std::back_inserter(listed), [](rcs_codec c) { return c != 0; });
    if (!common(listed, peer)) {
        return {RCS_ERROR_UNSUPPORTED, std::string(extension) + ": the peer offers none of " + codecList(listed) +
                                           " (it offers " + codecList(codecsOf(peer)) + ")"};
    }
    return {};
}

// The picture formats the peer takes in `codec` (rcs_peer_info.codec_limits);
// all of them where that is not known.
uint32_t peerFormats(const PeerOffers& peer, rcs_codec codec) noexcept {
    const auto limit = std::find_if(peer.codecLimits.begin(), peer.codecLimits.end(),
                                    [&](const rcs_codec_limit& l) { return l.codec == codec; });
    return limit != peer.codecLimits.end() && limit->picture_formats ? limit->picture_formats : allPictureFormats;
}

// What a host's codec takes of the formats: (1u << format) bits.
struct HostCodec {
    uint32_t codecs = 0, formats = 0;
};

// A codec implementation: the backend exists, something listed is
// implemented or handled by the host's own codec (`host`), in one of the
// listed picture formats (within `produced`: what a sharer's capture
// produces), and the peer, once known, takes one of them.
Refusal checkCodecs(const char* extension, std::span<const rcs_codec> codecs,
                    std::span<const rcs_picture_format> formats, rcs_acceleration acceleration, rcs_backend backend,
                    rcs_codec_direction direction, const PeerOffers& peer,
                    std::span<const platform::CodecImplementation> implementations, HostCodec host,
                    uint32_t produced = allPictureFormats) {
    if (backend != RCS_BACKEND_AUTO &&
        std::none_of(implementations.begin(), implementations.end(), [&](const platform::CodecImplementation& i) {
            return i.backend == backend && i.direction == direction;
        })) {
        return {RCS_ERROR_UNSUPPORTED, std::string(extension) + ": the " + backendName(backend) + " codec backend " +
                                           (direction == RCS_CODEC_ENCODER ? "encodes" : "decodes") +
                                           " nothing on this system"};
    }
    const char* verb = direction == RCS_CODEC_ENCODER ? "encode" : "decode";
    const char* how = acceleration == RCS_ACCELERATION_HARDWARE   ? "hardware"
                      : acceleration == RCS_ACCELERATION_SOFTWARE ? "software"
                                                                  : "hardware or software";
    const auto offer = codecOffer(codecs, acceleration, backend, direction, implementations, host.codecs);
    if (offer.empty()) {
        std::vector<rcs_codec> listed;
        std::copy_if(codecs.begin(), codecs.end(), std::back_inserter(listed), [](rcs_codec c) { return c != 0; });
        return {RCS_ERROR_UNSUPPORTED, std::string(extension) + ": this system cannot " + verb + " " +
                                           codecList(listed) + " with " + how + " acceleration"};
    }
    const auto taken = pictureFormats(offer, formats, acceleration, backend, direction, implementations, host.codecs,
                                      host.formats, produced);
    if (std::all_of(taken.begin(), taken.end(), [](uint32_t f) { return !f; })) {
        // What it does take, in each codec.
        static constexpr rcs_picture_format every[] = {RCS_PICTURE_420_8,     RCS_PICTURE_420_10,
                                                       RCS_PICTURE_444_8,     RCS_PICTURE_444_10,
                                                       RCS_PICTURE_HDR10_420, RCS_PICTURE_HDR10_444};
        const auto possible = pictureFormats(offer, every, acceleration, backend, direction, implementations,
                                             host.codecs, host.formats, produced);
        uint32_t listed = 0;
        for (const rcs_picture_format format : formats) {
            listed |= format ? 1u << format : 0;
        }
        std::string can;
        for (size_t i = 0; i < offer.size(); ++i) {
            can += (can.empty() ? "" : "; ") + std::string(codecName(offer[i])) + " " + pictureFormatList(possible[i]);
        }
        return {RCS_ERROR_UNSUPPORTED, std::string(extension) + ": this system cannot " + verb + " " +
                                           pictureFormatList(listed) + " pictures (formats[]) with " + how +
                                           " acceleration in " + codecList(offer) +
                                           (direction == RCS_CODEC_ENCODER ? " (encoder and capture)" : "") +
                                           "; it can: " + can};
    }
    if (!common(offer, peer.codecs)) {
        return {RCS_ERROR_UNSUPPORTED, std::string(extension) + ": the peer offers none of " + codecList(offer) +
                                           " (it offers " + codecList(codecsOf(peer.codecs)) + ")"};
    }
    bool shared = !peer.codecs;
    for (size_t i = 0; i < offer.size() && !shared; ++i) {
        shared = (peer.codecs & (1u << offer[i])) && (taken[i] & peerFormats(peer, offer[i]));
    }
    if (!shared) {
        return {RCS_ERROR_UNSUPPORTED, std::string(extension) +
                                           ": the peer takes none of these picture formats (formats[]) in the "
                                           "codecs both offer (rcs_peer_info.codec_limits says which it takes)"};
    }
    return {};
}

// The sharer's own size is never made smaller: some codec this end offers
// must take it with this machine's encoder and, once a viewer is connected,
// that viewer's decoder. A connection uses the first codec both offer that
// takes it (planStream); with none, that connection ends.
Refusal checkStreamSize(const SessionConfig& c, const agent::Machine& machine, const PeerOffers& peer) {
    const auto& v = c.shareVideo;
    const auto encoders = offerOf(c, machine);
    const auto& offer = encoders.codecs;
    std::string first;
    for (size_t i = 0; i < offer.size(); ++i) {
        PictureSize decoder;
        if (peer.codecs) {
            const auto limit = std::find_if(peer.codecLimits.begin(), peer.codecLimits.end(),
                                            [&](const rcs_codec_limit& l) { return l.codec == offer[i]; });
            if (limit == peer.codecLimits.end() || !(limit->picture_formats & encoders.formats[i])) {
                continue; // The viewer does not offer it, or in none of these formats.
            }
            decoder = {limit->max_width, limit->max_height};
        }
        const auto problem = sizeProblem(offer[i], v.width, v.height, encoders.largest[i], decoder);
        if (problem.empty()) {
            return {};
        }
        if (first.empty()) {
            first = problem;
        }
    }
    if (first.empty()) {
        return {}; // No codec in common: the codec checks say so.
    }
    return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_SHARE_VIDEO: " + first +
                                       (offer.size() > 1 ? ", and no other codec offered takes it either" : "") +
                                       "; offer one that does (rcs_encoder_config.codecs) or share a smaller size"};
}

// Display 0 is the main display; others must be listed by rcs_list_displays.
bool displayAvailable(const agent::Machine& machine, uint32_t displayId) {
    if (!displayId || seams::captureOverridden()) {
        return true;
    }
    const auto& list = machine.displays;
    return std::any_of(list.begin(), list.end(), [&](const rcs_display_info& d) { return d.display_id == displayId; });
}

// The capture sources the machine itself captures, by name.
const char* screenSource(rcs_capture_source source) {
    switch (source) {
    case RCS_CAPTURE_DISPLAY:
        return "a display";
    case RCS_CAPTURE_REGION:
        return "a region of a display";
    case RCS_CAPTURE_WINDOW:
        return "single windows (Windows 10 1903 or later)";
    case RCS_CAPTURE_APPLICATION:
        return "an application's windows";
    default:
        return nullptr;
    }
}

} // namespace

platform::InputOptions inputOptions(const SessionConfig& config) {
    return {config.capture.secure_desktop != 0, config.control.caps_lock,
            config.control.caps_lock_hold_ms ? config.control.caps_lock_hold_ms : 500u};
}

platform::CaptureTarget captureTarget(const rcs_capture_config& c) {
    platform::CaptureTarget target;
    target.source = c.source;
    switch (c.source) {
    case RCS_CAPTURE_REGION:
        target.region = {c.region_x, c.region_y, c.region_width, c.region_height};
        target.displayId = c.display_id;
        break;
    case RCS_CAPTURE_WINDOW:
        target.windowId = c.window_id;
        break;
    case RCS_CAPTURE_APPLICATION:
        target.processId = c.process_id;
        target.displayId = c.display_id;
        break;
    default: // A display; with the host's own pictures, the display input goes to.
        target.source = RCS_CAPTURE_DISPLAY;
        target.displayId = c.display_id;
        break;
    }
    return target;
}

// The displays of streams 1, 2, ...: connected, and none of them stream 0's.
Refusal checkStreams(const SessionConfig& c, const agent::Machine& machine) {
    uint32_t first = c.capture.display_id;
    for (const auto& d : machine.displays) {
        first = first ? first : (d.is_main ? d.display_id : 0);
    }
    for (const uint32_t display : streamDisplays(c)) {
        if (!displayAvailable(machine, display)) {
            return {RCS_ERROR_INVALID_ARGUMENT,
                    "RCS_EXT_STREAMS: display " + std::to_string(display) + " is not available; see rcs_list_displays"};
        }
        if (display == first) {
            return {RCS_ERROR_INVALID_ARGUMENT, "RCS_EXT_STREAMS: display " + std::to_string(display) +
                                                    " is the one stream 0 shares (RCS_EXT_CAPTURE)"};
        }
    }
    return {};
}

Refusal checkCapture(const rcs_capture_config& capture, const agent::Machine& machine) {
    const char* source = screenSource(capture.source);
    if (capture.secure_desktop && source && !machine.capabilities.secure_desktop) {
        return {RCS_ERROR_UNSUPPORTED,
                machine.capabilities.platform == RCS_PLATFORM_WINDOWS
                    ? "RCS_EXT_CAPTURE: secure_desktop needs a process running as LocalSystem in a user session; "
                      "run the sharing process (or its agent) as the helper of an rcs_supervisor with account SYSTEM"
                    : "RCS_EXT_CAPTURE: secure_desktop applies to Windows only"};
    }
    if (!source || seams::captureOverridden()) { // The host's pictures.
        return {};
    }
    if (!(machine.capabilities.capture_sources & (1u << capture.source))) {
        return {RCS_ERROR_UNSUPPORTED, std::string("RCS_EXT_CAPTURE: this system cannot share ") + source};
    }
    if (machine.screenPermission != RCS_PERMISSION_GRANTED) {
        return {RCS_ERROR_PERMISSION_REQUIRED, "RCS_EXT_CAPTURE: screen recording permission is required"};
    }
    if (capture.source == RCS_CAPTURE_WINDOW) { // A window that is gone pauses capture instead.
        return {};
    }
    if (!displayAvailable(machine, capture.display_id)) {
        return {RCS_ERROR_INVALID_ARGUMENT, "RCS_EXT_CAPTURE: display " + std::to_string(capture.display_id) +
                                                " is not available; see rcs_list_displays"};
    }
    if (capture.source == RCS_CAPTURE_REGION) {
        for (const auto& d : machine.displays) {
            const auto& r = capture;
            if ((r.display_id ? d.display_id == r.display_id : d.is_main) &&
                (uint64_t(r.region_x) + r.region_width > d.pixel_width ||
                 uint64_t(r.region_y) + r.region_height > d.pixel_height)) {
                return {RCS_ERROR_INVALID_ARGUMENT,
                        "RCS_EXT_CAPTURE: the region is not inside display " + std::to_string(d.display_id) + " (" +
                            std::to_string(d.pixel_width) + "x" + std::to_string(d.pixel_height) + " pixels)"};
            }
        }
    }
    return {};
}

namespace {

// The audio codecs of `bits` ((1u << rcs_audio_codec)), named.
std::string audioCodecsIn(uint32_t bits) {
    static constexpr rcs_audio_codec every[] = {RCS_AUDIO_OPUS, RCS_AUDIO_AAC, RCS_AUDIO_PCM};
    return audioCodecList(audioOffer(every, bits));
}

// `peers`: what each peer receives (decodes).
Refusal checkSendAudio(const SessionConfig& c, const std::vector<uint32_t>& peers, MachineRef machine) {
    const auto& a = c.sendAudio;
    const bool share = c.role == RCS_ROLE_SHARE;
    const auto offer = audioOffer(a.codecs, capabilities().audio_encoders);
    if (a.enabled && offer.empty()) {
        return {RCS_ERROR_UNSUPPORTED,
                "RCS_EXT_SEND_AUDIO: this system encodes none of these codecs[]; it encodes " +
                    audioCodecsIn(capabilities().audio_encoders)};
    }
    if (!std::all_of(peers.begin(), peers.end(), [&](uint32_t peer) { return common(offer, peer); })) {
        return {RCS_ERROR_UNSUPPORTED, !share            ? "RCS_EXT_SEND_AUDIO: the sharer decodes none of these codecs[]"
                                       : peers.size() > 1 ? "RCS_EXT_SEND_AUDIO: a viewer decodes none of these codecs[]"
                                                          : "RCS_EXT_SEND_AUDIO: the viewer decodes none of these codecs[]"};
    }
    if (!share && a.enabled && a.source == RCS_AUDIO_SYSTEM) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_SEND_AUDIO: a viewer sends a microphone or the host's sound (source "
                                       "MICROPHONE or HOST); what its machine plays holds the sharer's sound"};
    }
    // Playing the viewers' sound while sending what the machine plays would
    // send them their own voices back, unless this process's sound is left
    // out of it, here (an agent of another process records its own process's).
    const auto& r = c.receiveAudio;
    if (share && a.enabled && a.source == RCS_AUDIO_SYSTEM && r.enabled && r.playback &&
        (!a.exclude_current_process || (c.chained & (1u << RCS_EXT_AGENT_LINK)))) {
        return {RCS_ERROR_UNSUPPORTED,
                "RCS_EXT_RECEIVE_AUDIO: playing the viewers' sound while sending what this machine plays would send "
                "them their own voices back; set exclude_current_process (RCS_EXT_SEND_AUDIO, with the agent in "
                "this process), send the host's sound (source HOST), or play it yourself (playback 0)"};
    }
    if (!a.enabled) {
        return {};
    }
    if (a.source == RCS_AUDIO_SYSTEM && machine && !seams::audioCaptureOverridden()) {
        const auto& t = c.audioTuning;
        const auto backend = t.capture_backend ? t.capture_backend : machine->defaultAudioBackend;
        if (!(machine->capabilities.audio_capture_backends & (1u << backend))) {
            return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_AUDIO_TUNING: capture_backend " + std::to_string(backend) +
                                               " is not available on this system"};
        }
        if (t.capture_period_frames && !(machine->audioPeriodBackends & (1u << backend))) {
            return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_AUDIO_TUNING: this capture backend delivers the system's periods; "
                                           "leave capture_period_frames 0 or use RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP"};
        }
    }
    return {};
}

Refusal checkControl(const SessionConfig& c, const rcs_session_callbacks& callbacks, MachineRef machine) {
    // Control may also come from on_viewer_join, which grants it per viewer.
    if (!c.control.allowed && !callbacks.on_viewer_join) {
        return {};
    }
    if (c.control.target == RCS_INPUT_HOST) {
        if (!callbacks.on_input) {
            return {RCS_ERROR_INVALID_ARGUMENT, "RCS_EXT_CONTROL: target RCS_INPUT_HOST needs on_input"};
        }
        return {};
    }
    if (c.control.host_input_types && !callbacks.on_input) {
        return {RCS_ERROR_INVALID_ARGUMENT, "RCS_EXT_CONTROL: host_input_types needs on_input, where the host "
                                            "executes those types"};
    }
    if (!c.control.allowed) {
        return {}; // The system's checks are made when control is allowed.
    }
    // What follows is the system's: it needs nothing when the host executes every type.
    if (!machine || !systemInputTypes(c.control)) {
        return {};
    }
    if (!machine->capabilities.input_types) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_CONTROL: this system cannot execute remote input"};
    }
    // The permission only: a lock screen or a secure desktop at this moment
    // pauses the grant, it does not refuse the setting.
    if (machine->inputPermission == RCS_PERMISSION_UNAVAILABLE) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_CONTROL: this process cannot inject input (on Windows a process in "
                                       "session 0, such as a service, has no desktop; an agent in the user's session "
                                       "can, see rcs_agent_link_config)"};
    }
    if (machine->inputPermission != RCS_PERMISSION_GRANTED) {
        return {RCS_ERROR_PERMISSION_REQUIRED, "RCS_EXT_CONTROL: input permission is required (see "
                                               "rcs_request_input_permission)"};
    }
    return {};
}

// The formats in a list, as (1u << rcs_picture_format) bits.
uint32_t formatMask(std::span<const rcs_picture_format> formats) noexcept {
    uint32_t mask = 0;
    for (const rcs_picture_format format : formats) {
        mask |= format ? 1u << format : 0;
    }
    return mask;
}

// HDR pictures reach no CPU image (BGRA8 is SDR) and no host encoder (their
// layout for it is not defined): a sharer's preview and encoder.
Refusal checkPreviewFormats(const SessionConfig& c, const rcs_session_callbacks& callbacks) {
    const uint32_t high = allPictureFormats & ~sdrPictureFormats;
    if (c.role != RCS_ROLE_SHARE || c.capture.source == RCS_CAPTURE_ENCODED) {
        return {};
    }
    if (callbacks.on_frame && (formatMask(c.encoder.formats) & high)) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_ENCODER: formats[] lists HDR10, whose pictures the local preview "
                                       "on_frame (BGRA8, SDR) cannot show; use on_gpu_frame or no preview"};
    }
    if (c.hostEncoder.codecs[0] && (c.hostEncoder.picture_formats & high)) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_HOST_ENCODER: HDR10 pictures do not reach the host's encoder yet; "
                                       "list SDR formats in picture_formats"};
    }
    return {};
}

Refusal checkDecoder(const SessionConfig& c, const rcs_session_callbacks& callbacks, const PeerOffers& peer) {
    const auto& d = c.decoder;
    if (!c.encodedOutput.decode) { // Nothing decodes: what is listed only has to suit the peer.
        return checkCodecs("RCS_EXT_DECODER", d.codecs, d.formats, d.acceleration, RCS_BACKEND_AUTO,
                           RCS_CODEC_DECODER, peer, codecImplementations(), {~0u, allPictureFormats});
    }
    const HostCodec host{codecMask(c.hostDecoder.codecs), c.hostDecoder.picture_formats};
    if (auto refusal = checkCodecs("RCS_EXT_DECODER", d.codecs, d.formats, d.acceleration, d.backend,
                                   RCS_CODEC_DECODER, peer, codecImplementations(), host)) {
        return refusal;
    }
    // CPU images are BGRA8, SDR: HDR10 needs GPU frames, or the host's decoder
    // for every codec (it shows the pictures itself).
    const bool hostDecodesAll = std::all_of(std::begin(d.codecs), std::end(d.codecs),
                                            [&](rcs_codec codec) { return !codec || handles(c.hostDecoder, codec); });
    if (callbacks.on_frame && !hostDecodesAll && (formatMask(d.formats) & ~sdrPictureFormats)) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_DECODER: formats[] lists HDR10, whose pictures CPU frames (on_frame, "
                                       "BGRA8, SDR) cannot carry; use on_gpu_frame, or leave HDR10 out"};
    }
    const bool hardware = d.acceleration == RCS_ACCELERATION_HARDWARE;
    if (void* device = c.gpuDevice.d3d11_device) {
        if (capabilities().platform != RCS_PLATFORM_WINDOWS) {
            return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_GPU_DEVICE: d3d11_device applies on Windows only"};
        }
        // Hardware decoding needs its GPU to decode the codecs; software
        // decoding only uploads its pictures there.
        uint32_t decoded = 0; // What the SDK's decoder, not the host's, decodes on the GPU.
        for (const rcs_codec codec : d.codecs) {
            if (hardware && codec && !hostCodec(codec) && !handles(c.hostDecoder, codec)) {
                decoded |= 1u << codec;
            }
        }
        try {
            platform::checkHostDevice(device, decoded);
        } catch (const Failure& failure) {
            return {failure.result(), std::string("RCS_EXT_GPU_DEVICE: ") + failure.what()};
        }
    }
    if (callbacks.on_gpu_frame && !hardware && !capabilities().software_decoding_gpu_frames) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_DECODER: software decoding yields CPU images on this system; use "
                                       "on_frame"};
    }
    return {};
}

// `peers`: what each peer sends (encodes).
Refusal checkReceiveAudio(const SessionConfig& c, const std::vector<uint32_t>& peers) {
    if (c.audioTuning.playback_queue_frames && capabilities().platform != RCS_PLATFORM_WINDOWS) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_AUDIO_TUNING: playback_queue_frames applies on Windows only; leave it 0"};
    }
    const auto offer = audioOffer(c.receiveAudio.codecs, receivedAudioCodecs(c));
    if (c.receiveAudio.enabled && offer.empty()) {
        return {RCS_ERROR_UNSUPPORTED,
                "RCS_EXT_RECEIVE_AUDIO: this system decodes none of these codecs[]; it decodes " +
                    audioCodecsIn(capabilities().audio_decoders)};
    }
    if (!std::all_of(peers.begin(), peers.end(), [&](uint32_t peer) { return common(offer, peer); })) {
        return {RCS_ERROR_UNSUPPORTED, c.role == RCS_ROLE_VIEW
                                           ? "RCS_EXT_RECEIVE_AUDIO: the sharer encodes none of these codecs[]"
                                           : "RCS_EXT_RECEIVE_AUDIO: a viewer encodes none of these codecs[]"};
    }
    return {};
}

Refusal checkEncodings(const SessionConfig& c) {
    if (c.capture.source == RCS_CAPTURE_ENCODED && c.encodings.mode == RCS_ENCODINGS_FIXED && c.encodings.count > 1) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_ENCODINGS: the host's own bitstream (capture source ENCODED) has one "
                                       "encoding; FIXED with count above 1 cannot run"};
    }
    return {};
}

// What chooses networks has sockets of this end's own to choose them for (rcs_paths_config).
Refusal checkPaths(const SessionConfig& c) {
    const auto& p = c.paths;
    const auto transport = c.connect.transport;
    const bool listens = c.role == RCS_ROLE_SHARE && (transport == RCS_TRANSPORT_UDP || transport == RCS_TRANSPORT_TCP);
    if (p.excluded_networks) {
        if (c.networkTuning.local_interface[0]) {
            return {RCS_ERROR_INVALID_ARGUMENT, "RCS_EXT_PATHS: excluded_networks and local_interface "
                                                "(RCS_EXT_NETWORK_TUNING) both choose the network: set one"};
        }
        if (usesHostChannel(c)) {
            return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_PATHS: excluded_networks: this end goes over the host's channel "
                                           "(HOST, or a forwarder's link HOST), whose networks the host chooses"};
        }
        if (listens) {
            return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_PATHS: excluded_networks: a sharer over UDP or TCP is reached "
                                           "where it listens (rcs_connect.address)"};
        }
        if (!(seams::networkFacts() & RCS_NETWORK_FACT_KIND)) {
            return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_PATHS: excluded_networks: this system does not say which "
                                           "network is which (rcs_capabilities.network_facts; Android says it "
                                           "with its Java VM: rcs_android_initialize)"};
        }
    }
    if (p.standby && (transport == RCS_TRANSPORT_HOST || transport == RCS_TRANSPORT_FORWARDER || listens)) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_PATHS: standby keeps a path of this end's own: a viewer's UDP or "
                                       "TCP, or P2P and RELAY"};
    }
    return {};
}

Refusal checkIceServers(const SessionConfig& c) {
    if (c.connect.transport == RCS_TRANSPORT_RELAY && !c.iceServers.turn_server[0]) {
        return {RCS_ERROR_INVALID_ARGUMENT, "RCS_EXT_ICE_SERVERS: RELAY needs turn_server"};
    }
    return {};
}

} // namespace

const rcs_capabilities& capabilities() {
    static const rcs_capabilities value = [] {
        rcs_capabilities caps = agent::localMachine().capabilities;
        // Opus is built in, PCM needs no codec; AAC comes from the system.
        caps.audio_encoders = caps.audio_decoders = (1u << RCS_AUDIO_OPUS) | (1u << RCS_AUDIO_PCM);
#ifdef __EMSCRIPTEN__
        // A browser has no sockets of its own: the page's channel it is.
        caps.transports = 1u << RCS_TRANSPORT_HOST | 1u << RCS_TRANSPORT_FORWARDER;
        caps.forwarder_links = 1u << RCS_FORWARDER_HOST;
#else
        caps.transports = (1u << (RCS_TRANSPORT_FORWARDER + 1)) - 1;
        caps.forwarder_links = (1u << (RCS_FORWARDER_HOST + 1)) - 1;
#endif
        crypto::Bytes config(std::begin(aacConfig), std::end(aacConfig)); // AAC-LC, 48 kHz, stereo.
        try {
            config = platform::createAacEncoder(128000)->config();
            caps.audio_encoders |= 1u << RCS_AUDIO_AAC;
        } catch (const Failure&) {
        }
        try {
            platform::createAacDecoder(config);
            caps.audio_decoders |= 1u << RCS_AUDIO_AAC;
        } catch (const Failure&) {
        }
        return caps;
    }();
    return value;
}

namespace {

Refusal checkAgentLink(const SessionConfig& c, const rcs_session_callbacks& callbacks) {
    if (hasRemoteAgent(c) && (callbacks.on_frame || callbacks.on_gpu_frame)) {
        return {RCS_ERROR_INVALID_ARGUMENT, "RCS_EXT_AGENT_LINK: a session with an agent has no local preview (the picture "
                                            "does not leave the agent's process); leave on_frame and on_gpu_frame "
                                            "unset"};
    }
    return {};
}

Refusal checkEncodedOutput(const SessionConfig& c, const rcs_session_callbacks& callbacks) {
    if (c.role == RCS_ROLE_VIEW && !c.encodedOutput.decode &&
        (callbacks.on_frame || callbacks.on_gpu_frame || callbacks.on_audio)) {
        return {RCS_ERROR_INVALID_ARGUMENT, "RCS_EXT_ENCODED_OUTPUT: decode 0 decodes neither the picture nor the "
                                            "sound; leave on_frame, on_gpu_frame and on_audio unset"};
    }
    return {};
}

} // namespace

Refusal checkStart(const SessionConfig& c, const rcs_session_callbacks& callbacks, MachineRef machine) {
    if (auto refusal = checkAgentLink(c, callbacks)) {
        return refusal;
    }
    if (auto refusal = checkEncodedOutput(c, callbacks)) {
        return refusal;
    }
    if (!(capabilities().transports & (1u << c.connect.transport))) {
        return {RCS_ERROR_UNSUPPORTED, "This system carries no session over its own sockets "
                                       "(rcs_capabilities.transports): in a browser it goes over the host's "
                                       "channel, RCS_TRANSPORT_HOST, or through a forwarder over it"};
    }
    if (c.connect.transport == RCS_TRANSPORT_FORWARDER &&
        !(capabilities().forwarder_links & (1u << c.forwarder.link))) {
        return {RCS_ERROR_UNSUPPORTED, "RCS_EXT_FORWARDER: this system reaches no forwarder over its own sockets "
                                       "(rcs_capabilities.forwarder_links): in a browser link RCS_FORWARDER_HOST, "
                                       "the page's channel to it (e.g. a WebSocket to its WebSocket entry)"};
    }
    if (usesIce(c.connect.transport) && !callbacks.on_signal) {
        return {RCS_ERROR_INVALID_ARGUMENT, "P2P and RELAY need on_signal to hand this end's signal to the host"};
    }
    const uint32_t all = ~fixedExtensions;
    return checkChange(c, all, {}, callbacks, machine); // The peer is not known yet.
}

Refusal checkChange(const SessionConfig& c, uint32_t changed, const PeerOffers& peer,
                    const rcs_session_callbacks& callbacks, MachineRef machine) {
    changed &= extensionsOf(c.role);
    const auto has = [&](rcs_extension_type type) { return (changed & (1u << type)) != 0; };
    if (has(RCS_EXT_ICE_SERVERS)) {
        if (auto refusal = checkIceServers(c)) {
            return refusal;
        }
    }
    if (has(RCS_EXT_PATHS) || has(RCS_EXT_NETWORK_TUNING)) {
        if (auto refusal = checkPaths(c)) {
            return refusal;
        }
    }
    if (c.role == RCS_ROLE_SHARE && (has(RCS_EXT_ENCODINGS) || has(RCS_EXT_CAPTURE))) {
        if (auto refusal = checkEncodings(c)) {
            return refusal;
        }
    }
    // What HDR pictures cannot reach, before what makes them.
    if (has(RCS_EXT_ENCODER) || has(RCS_EXT_HOST_ENCODER)) {
        if (auto refusal = checkPreviewFormats(c, callbacks)) {
            return refusal;
        }
    }
    if (has(RCS_EXT_ENCODER) || has(RCS_EXT_CAPTURE)) {
        // The machine decides what is implemented and what its capture
        // produces; the viewer what it can decode, which is known even while
        // the remote agent is away. The host's own bitstream: it encodes
        // whatever it listed.
        const bool encoded = c.capture.source == RCS_CAPTURE_ENCODED;
        const HostCodec host = encoded    ? HostCodec{~0u, allPictureFormats}
                               : machine ? HostCodec{machine->hostEncoders, machine->hostEncoderFormats}
                                         : HostCodec{};
        const auto refusal =
            machine ? checkCodecs("RCS_EXT_ENCODER", c.encoder.codecs, c.encoder.formats, c.encoder.acceleration,
                                  c.encoder.backend, RCS_CODEC_ENCODER, peer, machine->implementations, host,
                                  producedFormats(c, *machine))
                    : checkPeerCodecs("RCS_EXT_ENCODER", c.encoder.codecs, peer.codecs);
        if (refusal) {
            return refusal;
        }
    }
    if (has(RCS_EXT_CAPTURE) && machine) {
        if (auto refusal = checkCapture(c.capture, *machine)) {
            return refusal;
        }
    }
    if ((has(RCS_EXT_STREAMS) || has(RCS_EXT_CAPTURE)) && machine) {
        if (auto refusal = checkStreams(c, *machine)) {
            return refusal;
        }
    }
    if ((has(RCS_EXT_SHARE_VIDEO) || has(RCS_EXT_ENCODER) || has(RCS_EXT_CAPTURE)) && machine) {
        if (auto refusal = checkStreamSize(c, *machine, peer)) {
            return refusal;
        }
    }
    if (has(RCS_EXT_SEND_AUDIO) || has(RCS_EXT_RECEIVE_AUDIO) || has(RCS_EXT_AUDIO_TUNING) || has(RCS_EXT_AGENT_LINK)) {
        if (auto refusal = checkSendAudio(c, peer.receiveAudio, machine)) {
            return refusal;
        }
    }
    if (has(RCS_EXT_CONTROL)) {
        if (auto refusal = checkControl(c, callbacks, machine)) {
            return refusal;
        }
    }
    if (has(RCS_EXT_DECODER)) {
        if (auto refusal = checkDecoder(c, callbacks, peer)) {
            return refusal;
        }
    }
    if (has(RCS_EXT_RECEIVE_AUDIO) || has(RCS_EXT_AUDIO_TUNING)) {
        if (auto refusal = checkReceiveAudio(c, peer.sendAudio)) {
            return refusal;
        }
    }
    return {};
}

} // namespace rcsdk::session
