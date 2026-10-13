// Codec negotiation. Right after the handshake the viewer sends its CodecOffer
// and settings, and the sharer, once it took the viewer, its own:
// the codecs their host listed that this system implements with the configured
// acceleration and backend, or the host's own codec handles, most preferred
// first (a sharer whose host pushes its own bitstream, and a viewer whose
// host takes the packets without decoding them, offer every listed one),
// each with the picture formats of the host's list it takes there; a
// codec that takes none of them is left out. Both then derive the same codec:
// the first of the sharer's codecs that the viewer also offers with a picture
// format in common (with several viewers: that every viewer offers), and the
// sharer the format (session/stream_plan.hpp). The sharer decides from the
// viewer's offer whether it can serve it next to the viewers it serves. An
// end sends its offer again when its host changes it while running.
#pragma once

#include "agent/agent.hpp"
#include "platform/platform.hpp"
#include "protocol/connection.hpp"
#include "session/config.hpp"
#include "util/api.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rcsdk::session {

// This end's offer: the listed codecs, in the host's order, that one of
// `implementations` encodes or decodes with the acceleration and backend, or
// the host's own codec handles (`hostCodecs`, as (1u << codec) bits).
// RCS_ACCELERATION_HARDWARE_FIRST takes the first hardware and the first
// software implementation of each codec together, here and below.
std::vector<rcs_codec> codecOffer(std::span<const rcs_codec> codecs, rcs_acceleration acceleration,
                                  rcs_backend backend, rcs_codec_direction direction,
                                  std::span<const platform::CodecImplementation> implementations,
                                  uint32_t hostCodecs = 0);
std::vector<rcs_codec> encoderOffer(const rcs_encoder_config& encoder,
                                    std::span<const platform::CodecImplementation> implementations,
                                    uint32_t hostCodecs = 0);
std::vector<rcs_codec> decoderOffer(const rcs_decoder_config& decoder,
                                    std::span<const platform::CodecImplementation> implementations,
                                    uint32_t hostCodecs = 0);
// The largest picture each of `codecs` takes with these settings: the codec's
// own limit for the host's own codec (`hostCodecs`), which handles it before
// the system's; otherwise the first implementation's that encodes or decodes
// it with the acceleration and backend (the one RCS_BACKEND_AUTO takes on the
// first GPU); with HARDWARE_FIRST the smaller of the two where the software
// one takes formats the hardware one does not.
std::vector<PictureSize> largestPictures(std::span<const rcs_codec> codecs, rcs_acceleration acceleration,
                                         rcs_backend backend, rcs_codec_direction direction,
                                         std::span<const platform::CodecImplementation> implementations,
                                         uint32_t hostCodecs = 0);
// The picture formats of `listed` (a host's formats[]) that each of `codecs`
// takes with these settings, as (1u << rcs_picture_format) bits: those of the
// host's own codec (`hostFormats`) where it handles the codec (`hostCodecs`),
// which it does before the system's; otherwise those of the first
// implementation that encodes or decodes it with the acceleration and backend
// (the one largestPictures takes); and within `produced`, what a sharer's
// capture produces for its encoder.
std::vector<uint32_t> pictureFormats(std::span<const rcs_codec> codecs, std::span<const rcs_picture_format> listed,
                                     rcs_acceleration acceleration, rcs_backend backend,
                                     rcs_codec_direction direction,
                                     std::span<const platform::CodecImplementation> implementations,
                                     uint32_t hostCodecs = 0, uint32_t hostFormats = 0,
                                     uint32_t produced = allPictureFormats);
// The most pixels a second a sharer's encoder of `codec` takes in `format`
// (CodecImplementation::maxPixelRates of the implementation largestPictures
// takes, or with HARDWARE_FIRST the software one where only it takes the
// format); 0: no limit known, the host's own encoder or bitstream.
uint64_t encoderPixelRate(const SessionConfig& config, const agent::Machine& machine, rcs_codec codec,
                          rcs_picture_format format);
// The largest picture `offer` takes in `codec`; none (0x0) for a codec it does not offer.
PictureSize largestOffered(const protocol::CodecOffer& offer, rcs_codec codec) noexcept;
// The picture formats `offer` takes in `codec`; none for a codec it does not offer.
uint32_t formatsOffered(const protocol::CodecOffer& offer, rcs_codec codec) noexcept;
// What every one of a sharer's viewers decodes (one stream serves them all):
// the codecs all of them offer with a picture format all of them take, in the
// first one's order, each with the smallest of their largest pictures
// (unknown where one did not say) and the formats all of them take.
protocol::CodecOffer commonOffer(std::span<const protocol::CodecOffer> offers);
// Video: the first of the sharer's codecs that the viewer offers too, with a
// picture format both take; 0 when there is none.
rcs_codec negotiatedCodec(const protocol::CodecOffer& sharer, const protocol::CodecOffer& viewer) noexcept;
// Why two offers that have codecs in common have no picture format in common
// in any of them: what each end takes in each common codec.
std::string formatMismatch(const protocol::CodecOffer& sharer, const protocol::CodecOffer& viewer);

// This end's audio codecs, in the host's order, that this system encodes (to
// send) or decodes (to receive): `available` as (1u << rcs_audio_codec) bits.
// Offered with sound off too, so it can be turned on during the run.
std::vector<rcs_audio_codec> audioOffer(std::span<const rcs_audio_codec> codecs, uint32_t available);
// The audio codecs this end receives, as (1u << rcs_audio_codec) bits: those
// this system decodes, or any on a viewer with decode 0 (RCS_EXT_ENCODED_OUTPUT).
uint32_t receivedAudioCodecs(const SessionConfig& config);

// The first of `shareOffer` that `viewOffer` holds too, or 0 when the lists
// have nothing in common: audio codecs, and video codecs before picture
// formats are considered.
rcs_codec negotiatedCodec(std::span<const rcs_codec> shareOffer, std::span<const rcs_codec> viewOffer) noexcept;

// The input a sharer takes (rcs_peer_info.input_types): every type when its
// host executes input; else what the agent's machine executes, Ctrl+Alt+Del
// where this process sends it, and the types its host executes itself.
uint32_t takenInputTypes(const SessionConfig& config, const agent::Machine& machine);

// The picture formats a sharer's pictures can be, as (1u << rcs_picture_format)
// bits: what its machine's capture produces, SDR for the host's own BGRA8
// pictures (and a test's capture), everything for the host's own bitstream
// (it says what it is).
uint32_t producedFormats(const SessionConfig& config, const agent::Machine& machine) noexcept;

// This end's offer, from the role's latest settings: the encoder's or the
// decoder's codecs, the sound's, whether sound is on, and the platform; a
// sharer's input and whether it tells where text goes. A sharer's encoders
// are its agent's: `machine` is the machine the agent runs on (this system's,
// without a machine).
protocol::CodecOffer offerOf(const SessionConfig& config);
// This system as the machine of a sharer's own agent: with the codecs its
// host's encoder handles.
agent::Machine ownMachine(const SessionConfig& config);
protocol::CodecOffer offerOf(const SessionConfig& config, const agent::Machine& machine);
// The peer as the host sees it (rcs_peer_info): its offer, and the device key
// it proved in the handshake.
rcs_peer_info peerOf(const protocol::CodecOffer& offer, const std::optional<crypto::PublicKey>& key) noexcept;

// View: sends this end's offer and settings, and returns the sharer's offer,
// which must be its first message after the admission's (protocol::Admission,
// each answered; `waiting` hears of the first, with the time the sharer's host
// has, and of the end of the wait); a sharer that refused this end (an
// untrusted device key, its host's decision, no room: protocol::Refused)
// fails it with its result.
struct AdmissionWatch {
    std::function<void(uint32_t timeoutMs)> waiting;
    std::function<void()> admitted;
    std::function<void(rcs_result result, const std::string& reason)> refused;
};
protocol::CodecOffer exchangeOffers(protocol::Connection& connection, const protocol::CodecOffer& offer,
                                    const protocol::ViewerSettings& settings, const AdmissionWatch& watch);

// (1u << codec) bits of `codecs`, and the codecs of such a mask.
uint32_t codecMask(std::span<const rcs_codec> codecs) noexcept;
std::vector<rcs_codec> codecsOf(uint32_t mask);
// "HEVC, H.264" for error messages; "none" when empty.
std::string codecList(std::span<const rcs_codec> codecs);
std::string audioCodecList(std::span<const rcs_audio_codec> codecs);

} // namespace rcsdk::session
