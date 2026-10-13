#include "session/negotiation.hpp"

#include "seams/seams.hpp"
#include "session/checks.hpp"
#include "util/log.hpp"

#include <algorithm>

namespace rcsdk::session {

namespace {

// The implementations an acceleration takes for `codec`, the first listed
// of each kind with the backend (RCS_BACKEND_AUTO: any): one, or with
// HARDWARE_FIRST a hardware one first and a software one second.
struct Chosen {
    const platform::CodecImplementation* first = nullptr;
    const platform::CodecImplementation* second = nullptr;
};
Chosen chosen(rcs_codec codec, rcs_acceleration acceleration, rcs_backend backend, rcs_codec_direction direction,
              std::span<const platform::CodecImplementation> implementations) {
    const auto find = [&](rcs_acceleration kind) -> const platform::CodecImplementation* {
        const auto i =
            std::find_if(implementations.begin(), implementations.end(), [&](const platform::CodecImplementation& i) {
                return i.codec == codec && i.direction == direction && i.acceleration == kind &&
                       (backend == RCS_BACKEND_AUTO || i.backend == backend);
            });
        return i == implementations.end() ? nullptr : &*i;
    };
    if (acceleration != RCS_ACCELERATION_HARDWARE_FIRST) {
        return {find(acceleration), nullptr};
    }
    return {find(RCS_ACCELERATION_HARDWARE), find(RCS_ACCELERATION_SOFTWARE)};
}

} // namespace

std::vector<rcs_codec> codecOffer(std::span<const rcs_codec> codecs, rcs_acceleration acceleration,
                                  rcs_backend backend, rcs_codec_direction direction,
                                  std::span<const platform::CodecImplementation> implementations,
                                  uint32_t hostCodecs) {
    std::vector<rcs_codec> offer;
    for (const rcs_codec codec : codecs) {
        const auto [first, second] = chosen(codec, acceleration, backend, direction, implementations);
        if (codec && ((hostCodecs & (1u << codec)) || first || second)) {
            offer.push_back(codec);
        }
    }
    return offer;
}

std::vector<rcs_codec> encoderOffer(const rcs_encoder_config& encoder,
                                    std::span<const platform::CodecImplementation> implementations,
                                    uint32_t hostCodecs) {
    return codecOffer(encoder.codecs, encoder.acceleration, encoder.backend, RCS_CODEC_ENCODER, implementations,
                      hostCodecs);
}

std::vector<rcs_codec> decoderOffer(const rcs_decoder_config& decoder,
                                    std::span<const platform::CodecImplementation> implementations,
                                    uint32_t hostCodecs) {
    return codecOffer(decoder.codecs, decoder.acceleration, decoder.backend, RCS_CODEC_DECODER, implementations,
                      hostCodecs);
}

std::vector<PictureSize> largestPictures(std::span<const rcs_codec> codecs, rcs_acceleration acceleration,
                                         rcs_backend backend, rcs_codec_direction direction,
                                         std::span<const platform::CodecImplementation> implementations,
                                         uint32_t hostCodecs) {
    std::vector<PictureSize> largest;
    for (const rcs_codec codec : codecs) {
        const auto limit = codecLimit(codec);
        PictureSize size{limit.width, limit.height};
        const auto within = [&](const platform::CodecImplementation* i) {
            if (i) {
                size = {std::min(size.width, i->maxWidth), std::min(size.height, i->maxHeight)};
            }
        };
        if (!(hostCodecs & (1u << codec))) {
            const auto [first, second] = chosen(codec, acceleration, backend, direction, implementations);
            within(first ? first : second);
            if (first && second && (second->pictureFormats & ~first->pictureFormats)) {
                within(second); // A stream of a format only the second takes is the second's.
            }
        }
        largest.push_back(size);
    }
    return largest;
}

std::vector<uint32_t> pictureFormats(std::span<const rcs_codec> codecs, std::span<const rcs_picture_format> listed,
                                     rcs_acceleration acceleration, rcs_backend backend,
                                     rcs_codec_direction direction,
                                     std::span<const platform::CodecImplementation> implementations,
                                     uint32_t hostCodecs, uint32_t hostFormats, uint32_t produced) {
    uint32_t wanted = 0;
    for (const rcs_picture_format format : listed) {
        wanted |= format ? 1u << format : 0; // Unused entries are 0.
    }
    std::vector<uint32_t> formats;
    for (const rcs_codec codec : codecs) {
        uint32_t taken = 0;
        if (hostCodecs & (1u << codec)) {
            taken = hostFormats;
        } else {
            const auto [first, second] = chosen(codec, acceleration, backend, direction, implementations);
            taken = (first ? first->pictureFormats : 0) | (second ? second->pictureFormats : 0);
        }
        formats.push_back(wanted & taken & produced);
    }
    return formats;
}

uint64_t encoderPixelRate(const SessionConfig& config, const agent::Machine& machine, rcs_codec codec,
                          rcs_picture_format format) {
    if (config.capture.source == RCS_CAPTURE_ENCODED || (machine.hostEncoders & (1u << codec))) {
        return 0;
    }
    const auto& e = config.encoder;
    const auto [first, second] = chosen(codec, e.acceleration, e.backend, RCS_CODEC_ENCODER, machine.implementations);
    const auto* used = first && (first->pictureFormats & (1u << format)) ? first : second ? second : first;
    return used && format < used->maxPixelRates.size() ? used->maxPixelRates[format] : 0;
}

PictureSize largestOffered(const protocol::CodecOffer& offer, rcs_codec codec) noexcept {
    for (size_t i = 0; i < offer.codecs.size() && i < offer.largest.size(); ++i) {
        if (offer.codecs[i] == codec) {
            return offer.largest[i];
        }
    }
    return {};
}

uint32_t formatsOffered(const protocol::CodecOffer& offer, rcs_codec codec) noexcept {
    for (size_t i = 0; i < offer.codecs.size() && i < offer.formats.size(); ++i) {
        if (offer.codecs[i] == codec) {
            return offer.formats[i];
        }
    }
    return 0;
}

protocol::CodecOffer commonOffer(std::span<const protocol::CodecOffer> offers) {
    protocol::CodecOffer common;
    if (offers.empty()) {
        return common;
    }
    for (size_t i = 0; i < offers.front().codecs.size(); ++i) {
        const rcs_codec codec = offers.front().codecs[i];
        PictureSize smallest = largestOffered(offers.front(), codec);
        uint32_t formats = formatsOffered(offers.front(), codec);
        bool everyone = true;
        for (const auto& offer : offers.subspan(1)) {
            if (std::find(offer.codecs.begin(), offer.codecs.end(), codec) == offer.codecs.end()) {
                everyone = false;
                break;
            }
            const PictureSize size = largestOffered(offer, codec);
            if (size.width && (!smallest.width || size.width < smallest.width)) {
                smallest.width = size.width;
            }
            if (size.height && (!smallest.height || size.height < smallest.height)) {
                smallest.height = size.height;
            }
            formats &= formatsOffered(offer, codec);
        }
        if (everyone && formats) {
            common.codecs.push_back(codec);
            common.largest.push_back(smallest);
            common.formats.push_back(formats);
        }
    }
    return common;
}

rcs_codec negotiatedCodec(std::span<const rcs_codec> shareOffer, std::span<const rcs_codec> viewOffer) noexcept {
    for (const rcs_codec codec : shareOffer) {
        if (std::find(viewOffer.begin(), viewOffer.end(), codec) != viewOffer.end()) {
            return codec;
        }
    }
    return 0;
}

rcs_codec negotiatedCodec(const protocol::CodecOffer& sharer, const protocol::CodecOffer& viewer) noexcept {
    for (const rcs_codec codec : sharer.codecs) {
        if (formatsOffered(sharer, codec) & formatsOffered(viewer, codec)) {
            return codec;
        }
    }
    return 0;
}

std::string formatMismatch(const protocol::CodecOffer& sharer, const protocol::CodecOffer& viewer) {
    std::string each;
    for (const rcs_codec codec : sharer.codecs) {
        if (const uint32_t taken = formatsOffered(viewer, codec)) {
            each += (each.empty() ? "in " : "; in ") + std::string(codecName(codec)) + " the sharer sends " +
                    pictureFormatList(formatsOffered(sharer, codec)) + " and the viewer takes " +
                    pictureFormatList(taken);
        }
    }
    return "No picture format in common: " + each;
}

uint32_t receivedAudioCodecs(const SessionConfig& config) {
    return config.role == RCS_ROLE_VIEW && !config.encodedOutput.decode ? ~0u : capabilities().audio_decoders;
}

std::vector<rcs_audio_codec> audioOffer(std::span<const rcs_audio_codec> codecs, uint32_t available) {
    std::vector<rcs_audio_codec> offer;
    for (const rcs_audio_codec codec : codecs) {
        if (codec && (available & (1u << codec))) {
            offer.push_back(codec);
        }
    }
    return offer;
}

uint32_t takenInputTypes(const SessionConfig& config, const agent::Machine& machine) {
    if (config.control.target == RCS_INPUT_HOST) {
        return protocol::allInputTypes;
    }
    constexpr uint32_t attention = 1u << RCS_INPUT_SECURE_ATTENTION;
    return (machine.capabilities.input_types & ~attention) | (capabilities().secure_attention ? attention : 0u) |
           config.control.host_input_types;
}

uint32_t producedFormats(const SessionConfig& config, const agent::Machine& machine) noexcept {
    switch (config.capture.source) {
    case RCS_CAPTURE_ENCODED:
        return allPictureFormats;
    case RCS_CAPTURE_HOST:
        return sdrPictureFormats; // BGRA8.
    default: // A test's capture (seams) gives BGRA8 too.
        return seams::captureOverridden() ? sdrPictureFormats : machine.capabilities.picture_formats;
    }
}

namespace {

// Leaves out the codecs that take none of the listed picture formats.
void dropUnformatted(protocol::CodecOffer& offer) {
    for (size_t i = offer.codecs.size(); i-- > 0;) {
        if (!offer.formats[i]) {
            offer.codecs.erase(offer.codecs.begin() + std::ptrdiff_t(i));
            offer.largest.erase(offer.largest.begin() + std::ptrdiff_t(i));
            offer.formats.erase(offer.formats.begin() + std::ptrdiff_t(i));
        }
    }
}

} // namespace

protocol::CodecOffer offerOf(const SessionConfig& config, const agent::Machine& machine) {
    const auto& caps = capabilities();
    protocol::CodecOffer offer;
    if (config.role == RCS_ROLE_SHARE) {
        // The host's own bitstream: it encodes whatever it listed.
        const bool encoded = config.capture.source == RCS_CAPTURE_ENCODED;
        const uint32_t host = encoded ? ~0u : machine.hostEncoders;
        const uint32_t hostFormats = encoded ? allPictureFormats : machine.hostEncoderFormats;
        const auto& e = config.encoder;
        offer.codecs = encoderOffer(e, machine.implementations, host);
        offer.largest =
            largestPictures(offer.codecs, e.acceleration, e.backend, RCS_CODEC_ENCODER, machine.implementations, host);
        offer.formats = pictureFormats(offer.codecs, e.formats, e.acceleration, e.backend, RCS_CODEC_ENCODER,
                                       machine.implementations, host, hostFormats, producedFormats(config, machine));
        dropUnformatted(offer);
        offer.inputTypes = takenInputTypes(config, machine);
    } else {
        // decode 0: the host stores whatever it listed, as the host's own codec would take it.
        const bool decoding = config.encodedOutput.decode != 0;
        const auto& d = config.decoder;
        const uint32_t host = decoding ? codecMask(config.hostDecoder.codecs) : ~0u;
        const uint32_t hostFormats = decoding ? config.hostDecoder.picture_formats : allPictureFormats;
        offer.codecs = decoderOffer(d, codecImplementations(), host);
        offer.largest =
            largestPictures(offer.codecs, d.acceleration, d.backend, RCS_CODEC_DECODER, codecImplementations(), host);
        offer.formats = pictureFormats(offer.codecs, d.formats, d.acceleration, d.backend, RCS_CODEC_DECODER,
                                       codecImplementations(), host, hostFormats);
        dropUnformatted(offer);
    }
    offer.sendAudio = audioOffer(config.sendAudio.codecs, caps.audio_encoders);
    offer.receiveAudio = audioOffer(config.receiveAudio.codecs, receivedAudioCodecs(config));
    offer.sendsAudio = config.sendAudio.enabled != 0;
    offer.receivesAudio = config.receiveAudio.enabled != 0;
    offer.platform = caps.platform;
    return offer;
}

protocol::CodecOffer offerOf(const SessionConfig& config) {
    return offerOf(config, config.role == RCS_ROLE_SHARE ? ownMachine(config) : agent::Machine{});
}

agent::Machine ownMachine(const SessionConfig& config) {
    agent::Machine machine = agent::localMachine();
    machine.hostEncoders = codecMask(config.hostEncoder.codecs);
    machine.hostEncoderFormats = config.hostEncoder.picture_formats;
    return machine;
}

rcs_peer_info peerOf(const protocol::CodecOffer& offer, const std::optional<crypto::PublicKey>& key) noexcept {
    rcs_peer_info peer = RCS_PEER_INFO_INIT;
    peer.platform = offer.platform;
    peer.codecs = codecMask(offer.codecs);
    for (size_t i = 0; i < offer.codecs.size() && i < offer.largest.size() && i < offer.formats.size() &&
                       i < RCS_MAX_CODECS;
         ++i) {
        peer.codec_limits[i] = {offer.codecs[i], offer.largest[i].width, offer.largest[i].height, offer.formats[i]};
    }
    peer.send_audio_codecs = codecMask(offer.sendAudio);
    peer.receive_audio_codecs = codecMask(offer.receiveAudio);
    peer.sends_audio = offer.sendsAudio;
    peer.receives_audio = offer.receivesAudio;
    peer.input_types = offer.inputTypes;
    if (key) {
        peer.has_public_key = 1;
        std::copy(key->begin(), key->end(), peer.public_key);
    }
    return peer;
}

protocol::CodecOffer exchangeOffers(protocol::Connection& connection, const protocol::CodecOffer& offer,
                                    const protocol::ViewerSettings& settings, const AdmissionWatch& watch) {
    connection.send(offer);
    connection.send(settings);
    auto message = connection.receive();
    bool waited = false;
    while (const auto* admission = std::get_if<protocol::Admission>(&message)) {
        if (!waited) {
            waited = true;
            log::info("session", "The sharer's host decides whether this end gets in");
            watch.waiting(admission->remainingMs);
        }
        connection.send(protocol::Admission{});
        message = connection.receive();
    }
    if (const auto* refused = std::get_if<protocol::Refused>(&message)) {
        if (waited) {
            watch.refused(refused->result, refused->reason);
        }
        fail(refused->result, "The sharer refused this end: " + refused->reason);
    }
    if (waited) {
        watch.admitted();
    }
    auto* peer = std::get_if<protocol::CodecOffer>(&message);
    if (!peer) {
        fail(RCS_ERROR_PROTOCOL, "The sharer did not start with a codec offer");
    }
    return std::move(*peer);
}

std::vector<rcs_codec> codecsOf(uint32_t mask) {
    std::vector<rcs_codec> codecs;
    for (const rcs_codec codec : standardCodecs) {
        if (mask & (1u << codec)) {
            codecs.push_back(codec);
        }
    }
    return codecs;
}

uint32_t codecMask(std::span<const rcs_codec> codecs) noexcept {
    uint32_t mask = 0;
    for (const rcs_codec codec : codecs) {
        mask |= codec ? 1u << codec : 0; // Unused entries are 0.
    }
    return mask;
}

std::string codecList(std::span<const rcs_codec> codecs) {
    std::string list;
    for (const rcs_codec codec : codecs) {
        list += (list.empty() ? "" : ", ") + std::string(codecName(codec));
    }
    return list.empty() ? "none" : list;
}

std::string audioCodecList(std::span<const rcs_audio_codec> codecs) {
    std::string list;
    for (const rcs_audio_codec codec : codecs) {
        list += (list.empty() ? "" : ", ") + std::string(audioCodecName(codec));
    }
    return list.empty() ? "none (audio off)" : list;
}

} // namespace rcsdk::session
