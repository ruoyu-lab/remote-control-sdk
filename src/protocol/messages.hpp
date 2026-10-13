// Messages exchanged inside the secure channel, and their wire encoding.
// Decoding validates every field, so the rest of the SDK can trust messages.
#pragma once

#include "crypto/crypto.hpp"
#include "media/audio.hpp"
#include "media/cursor.hpp"
#include "media/picture_size.hpp"
#include "media/video.hpp"
#include "transport/secure_channel.hpp"
#include "util/api.hpp"

#include <rcsdk/module.h>

#include <span>
#include <variant>
#include <string>
#include <vector>

namespace rcsdk::protocol {

// Both directions, first message after the handshake (a sharer's: once it
// admitted the viewer, which sent its offer and settings first) and again
// whenever the host changes them: the codecs this end can and will use, most
// preferred first (see session/negotiation.hpp), with the largest picture it
// takes in each (a sharer's encoder, a viewer's decoder) and the picture
// formats it takes there, whether it has sound on, and its platform.
struct CodecOffer {
    std::vector<rcs_codec> codecs;
    std::vector<PictureSize> largest; // Of each of `codecs`, in the same order...
    std::vector<uint32_t> formats;    // ...and its picture formats, as (1u << rcs_picture_format) bits.
    // Sound, each way: the codecs this end encodes (to send) and decodes (to
    // receive), preferred first, and whether it sends and receives now.
    std::vector<rcs_audio_codec> sendAudio, receiveAudio;
    bool sendsAudio = false, receivesAudio = false;
    rcs_platform platform = RCS_PLATFORM_UNKNOWN;
    uint32_t inputTypes = 0; // A sharer: the input it takes (rcs_peer_info.input_types).
    bool operator==(const CodecOffer&) const = default;
};

// View -> share, right after its offer and whenever the viewer's settings
// change: the picture and sound it asks for (rcs_view_request_config, 0
// fields leave the choice to the sharer), how it wants the pointer, the
// streams it takes ((1u << stream) bits; 0: all), and what it paused
// (RCS_PAUSE_* bits: it takes no stream, or no sound, meanwhile).
struct StreamRequest {
    uint32_t width = 0, height = 0, framesPerSecond = 0, bitrate = 0;
    rcs_codec codec = 0;
    rcs_picture_format format = 0;
    rcs_audio_codec audioCodec = 0;
    uint32_t audioBitrate = 0;
    bool operator==(const StreamRequest&) const = default;
};
struct ViewerSettings {
    StreamRequest request;
    rcs_cursor_mode cursor = RCS_CURSOR_IN_VIDEO;
    uint32_t streams = 0;
    uint32_t paused = 0;
    bool operator==(const ViewerSettings&) const = default;
};

// Share -> view, before a stream's first video packet and whenever the
// stream changes (size, frame rate, bitrate, codec, picture format, display,
// letterboxing, whether the pointer is in the picture, the encoding the
// viewer gets or how many the sharer runs); info.stream says which, and
// info.encoding which encoding of it: the viewer's own (each viewer hears
// the one it gets). Its video packets say their encoding too (EncodedPacket),
// as a datagram path may bring them before the announcement.
struct StreamAnnouncement {
    rcs_stream_info info{};
};

// Share -> view: `stream` stopped (rcs_streams_config changed, or the viewer
// no longer takes it); its packets stop.
struct StreamEnded {
    uint8_t stream = 0;
    bool operator==(const StreamEnded&) const = default;
};

// Share -> view with RCS_CURSOR_SEPARATE: a cursor image, its packed pixels
// (media/cursor.hpp) split into parts that each fit a control message, sent
// in order once per image; then CursorPosition whenever the pointer moves,
// changes or hides.
struct CursorShapePart {
    uint32_t id = 0;
    rcs_cursor_type type = RCS_CURSOR_CUSTOM;
    uint32_t width = 0, height = 0, hotspotX = 0, hotspotY = 0;
    uint32_t packedSize = 0, offset = 0; // Of the whole packed image; this part's place in it.
    crypto::Bytes bytes;
};
constexpr size_t maxCursorPart = 900;
struct CursorPosition {
    bool visible = false;
    uint8_t stream = 0;    // The stream whose display it is on...
    uint32_t x = 0, y = 0; // ...0..65535 across it.
    uint32_t shapeId = 0;
    bool operator==(const CursorPosition&) const = default;
};

// Share -> view, whenever control is granted or revoked. Input carries the
// epoch it was produced under; input from an older epoch is discarded.
struct ControlGrant {
    bool allowed = false;
    uint64_t epoch = 0;
};

// View -> share.
struct Input {
    rcs_input_event event{};
    uint64_t epoch = 0;
};

// View -> share, after the receiver dropped packets of `stream`; `encoding`
// is the one its packets were of (RCS_EXT_ENCODINGS).
struct KeyframeRequest {
    uint8_t stream = 0;
    uint8_t encoding = 0;
    bool operator==(const KeyframeRequest&) const = default;
};

// View -> share: a picture after `after`, the last one this end has, is
// missing: asked for again (gaveUp false: it may still come; told at once, so
// that the encoder still holds what came before) or given up on (true). The
// sharer's encoder drops the pictures after it from its references, and the
// packet after that says so (EncodedPacket::recoversAfter); or the sharer
// sends a keyframe once it was given up on.
struct RefLost {
    uint8_t stream = 0;
    uint8_t encoding = 0; // Of the picture `after`; one the viewer no longer gets is ignored.
    uint64_t after = 0;
    bool gaveUp = false;
    bool operator==(const RefLost&) const = default;
};

// Local to the viewer, never sent: the transport gave up on a video packet
// of `stream`, so pictures that depend on it cannot be decoded until the
// next keyframe (or the picture after their recovery: RefLost).
struct StreamGap {
    uint8_t stream = 0;
};

// Local to the viewer, never sent: a video packet of `stream` misses pieces
// that parity did not rebuild, and they were asked for again: they may still
// come, or the transport gives up on it (StreamGap). Once per packet.
struct VideoMissing {
    uint8_t stream = 0;
};

// Sound, either way (the sharer's to a viewer, a viewer's to the sharer): the
// sender's AudioFormat before the first AudioPacket, again for each restart
// of the sound (new settings); AudioOff when it stops (an end turned that
// direction off). Over UDP, audio packets are datagrams of their own: never
// resent, possibly lost.
struct AudioOff {
    enum Reason : uint8_t { senderOff = 0, receiverOff = 1, noCommonCodec = 2 } reason = senderOff;
};

// Both directions, on the data stream (UDP's own; in order with each other,
// beside the rest): a reliable message in parts that each fit a control
// message, sent in order; `count` parts make one message, and nothing else of
// the data stream interleaves. What it is:
//   data           The hosts' (rcs_session_send_data) on `channel`.
//   moduleMessage  A module's (rcsdk/module.h): the sender's module `module`.
//   moduleList     The sender's modules (ModuleList), once per connection:
//                  the viewer's first, then the sharer's in answer, each
//                  before any module's message or hello.
//   moduleHello    Module `module`'s new hello (rcs_module_set_hello); may be empty.
// `module` numbers the sender's modules as its list does, from 1; 0 otherwise.
struct DataPart {
    enum Kind : uint8_t { data = 0, moduleMessage = 1, moduleList = 2, moduleHello = 3 };
    Kind kind = data;
    uint8_t module = 0;
    uint32_t channel = 0;
    uint32_t index = 0, count = 1;
    crypto::Bytes bytes;
    bool operator==(const DataPart&) const = default;
};

// Both directions: an unreliable message, the hosts' (module 0, on `channel`)
// or a module's (`module` as in DataPart). Over UDP a datagram of its own,
// never resent; TCP carries it like the rest.
struct DataDatagram {
    uint8_t module = 0;
    uint32_t channel = 0;
    crypto::Bytes bytes;
    bool operator==(const DataDatagram&) const = default;
};

// A DataPart moduleList's message: the modules this end uses that work with
// their kind at the other end (RCS_MODULE_PEER), each with its version and
// hello, numbered from 1 in this order.
struct ModuleList {
    struct Entry {
        std::string id;
        uint32_t version = 0;
        crypto::Bytes hello;
        bool operator==(const Entry&) const = default;
    };
    std::vector<Entry> modules;
    bool operator==(const ModuleList&) const = default;
};
crypto::Bytes encodeModuleList(const ModuleList& list);
// Throws Failure(RCS_ERROR_PROTOCOL) for more than RCS_MODULES_MAX modules, an
// id that is none (rcsdk/module.h) or there twice, a hello too long.
ModuleList decodeModuleList(std::span<const uint8_t> bytes);
// The most bytes a ModuleList's message holds.
inline constexpr size_t maxModuleListBytes = 1 + RCS_MODULES_MAX * (1 + 63 + 4 + 2 + RCS_MODULE_HELLO_MAX);

// Share -> view, and the connection closes after it: this end does not serve
// the viewer, and why. Instead of the codec offer, once the handshake
// authenticated the viewer: it does not trust the device key the viewer
// proved (AUTHENTICATION, under the keys the handshake just agreed), its host
// refused it (REFUSED) or did not decide in time (TIMEOUT), or it cannot
// serve it next to the viewers it serves (BUSY). After the offers: the stream
// cannot be what its settings say with what both ends offer, e.g. a size the
// negotiated codec does not take (UNSUPPORTED). The viewer's run ends with
// `result`.
struct Refused {
    rcs_result result = RCS_ERROR_AUTHENTICATION;
    std::string reason;
    bool operator==(const Refused&) const = default;
};

// Both directions, before the codec offer, while the sharer's host decides
// whether the viewer gets in (RCS_ADMIT_LATER): the sharer sends one at once
// and then once a second, with the time left for the decision (0: no limit);
// the viewer answers each at once with one of its own (remaining 0), so each
// end knows the other is still there. The codec offer or Refused ends it.
struct Admission {
    uint32_t remainingMs = 0;
    bool operator==(const Admission&) const = default;
};

// Share -> view, right after the offers and whenever it changes: what the
// shared machine shows (rcs_desktop_info).
struct DesktopState {
    rcs_desktop_kind kind = RCS_DESKTOP_UNKNOWN;
    bool userLoggedOn = false;
    bool operator==(const DesktopState&) const = default;
};

// Share -> view, after the offers while anything is paused and whenever that
// changes: what the sharer paused for all its viewers (RCS_PAUSE_* bits).
struct SharerPaused {
    uint32_t paused = 0;
    bool operator==(const SharerPaused&) const = default;
};

// How an end leaves a connection on purpose, which the peer learns from the
// connection's CLOSE frame (udp_connection.hpp): it stopped for good, or will
// be back (it stopped with RCS_STOP_RESTART).
struct Goodbye {
    enum Reason : uint8_t { stopped = 0, restarting = 1 };
};

// ---- Through a forwarder (RCS_TRANSPORT_FORWARDER) ------------------------------------
//
// The sharer and each viewer reach the forwarder over a UDP connection of
// their own, the hop (this framing, the handshake's magic RCSFWD:
// transport/handshake.hpp; the forwarder: server/forwarder). An end's first
// message is ForwarderHello; the forwarder answers ForwarderWelcome, or
// Refused (REFUSED: the token; BUSY: no room) and closes. Then:
//   Pipe         each viewer's end-to-end connection with the sharer, datagram
//                by datagram as it is: the forwarder reads none of it
//   SealedVideo  the sharer's pictures, sealed once with the stream key
//                (protocol/media_keys.hpp), the hop's video: the forwarder
//                sends each viewer those of the encodings it takes
//   SealedAudio  the sharer's sound, each packet sealed once, a datagram:
//                the forwarder sends each viewer the packets of its sound
// and the room's news between the forwarder and the sharer (PeerJoined,
// PeerLeft, Subscribe, PathReport, NeedKeyframe, PeerMoved). MediaKey and
// MediaKeyAck go end to end, inside a viewer's pipe. A session's connection
// takes none of these, and the hop none of the session's others but Refused.

// End -> forwarder, the hop's first message: what it is there for, and the
// host's server's token for it (rcs_forwarder_config.token, opaque here).
struct ForwarderHello {
    enum Role : uint8_t { publish = 1, subscribe = 2 } role = subscribe;
    std::string token;
    bool operator==(const ForwarderHello&) const = default;
};
inline constexpr size_t maxForwarderToken = RCS_FORWARDER_TOKEN_SIZE - 1;

// Forwarder -> end: it is in the room. A viewer's number there (its pipe's
// route at the sharer) and whether the room has its sharer now; the sharer's
// 0, and how long the forwarder keeps each encoding's newest keyframe (and
// the pictures after it) for viewers that come later (0: it keeps none).
struct ForwarderWelcome {
    uint64_t peer = 0;
    bool sharer = false;
    uint32_t cacheMs = 0;
    bool operator==(const ForwarderWelcome&) const = default;
};

// Both ways: a datagram of a viewer's end-to-end connection. To and from the
// sharer `peer` names the viewer; a viewer's is 0. Over UDP a datagram of its
// own, never resent (the connection inside resends).
struct Pipe {
    uint64_t peer = 0;
    crypto::Bytes bytes;
    bool operator==(const Pipe&) const = default;
};

// Forwarder -> sharer: a viewer came into the room, with the route its token
// names (the host's: a hint, asserted by the forwarder, never proof of who it
// is); its pipe's datagrams follow.
struct PeerJoined {
    uint64_t peer = 0, route = 0;
    bool operator==(const PeerJoined&) const = default;
};

// Forwarder -> sharer: the viewer left the room (its pipe is closed).
// Sharer -> forwarder: drop it (it left the session, or was sent away).
struct PeerLeft {
    uint64_t peer = 0;
    bool operator==(const PeerLeft&) const = default;
};

// Sharer -> forwarder: what viewer `peer` gets from now on: its streams
// ((1u << stream) bits; 0: none) of `encoding`, each from a keyframe (the
// newest the forwarder kept, when the viewer holds its key; else the next,
// which it asks the sharer for), and the packets of sound `sound` (0: none).
// The stream key the viewer was given is (keyGeneration, keyStep): it opens
// what was sealed under that key and later ones only. Only a viewer the
// sharer let in and gave the stream key gets anything.
struct Subscribe {
    uint64_t peer = 0;
    uint32_t streams = 0;
    uint8_t encoding = 0;
    uint8_t sound = 0;
    uint32_t keyGeneration = 0, keyStep = 0;
    bool operator==(const Subscribe&) const = default;
};

// Forwarder -> sharer, every 100 ms: each viewer's path from the forwarder
// as its hop measures it, for the group's policy (session/group_plan.hpp).
struct PathReport {
    struct Peer {
        uint64_t peer = 0;
        uint32_t targetBps = 0;   // Its adaptive target; 0 while not measured.
        uint32_t lossPermille = 0;
        uint32_t queueUs = 0;     // Queueing on the path.
        uint32_t roundTripUs = 0;
        uint32_t backlogUs = 0;   // Pictures waiting to go to it.
        uint32_t drops = 0;       // Times its backlog was dropped, so far.
        uint64_t bytes = 0;       // Sent to it so far.
        bool operator==(const Peer&) const = default;
    };
    std::vector<Peer> peers;
    bool operator==(const PathReport&) const = default;
};
inline constexpr size_t maxPathReportPeers = 24; // What one message holds.

// Forwarder -> sharer: a viewer waits for a keyframe of `encoding` of
// `stream` (it moved there); asked once for all that wait.
struct NeedKeyframe {
    uint8_t stream = 0, encoding = 0;
    bool operator==(const NeedKeyframe&) const = default;
};

// Forwarder -> sharer: the forwarder moved viewer `peer` down to `encoding`
// on the spot (its path fell far behind); the sharer's policy takes it from there.
struct PeerMoved {
    uint64_t peer = 0;
    uint8_t encoding = 0;
    bool operator==(const PeerMoved&) const = default;
};

// Sharer -> forwarder -> viewer: a picture (its EncodedPacket's message),
// sealed with the stream key of `generation` and `step`
// (protocol/media_keys.hpp). The forwarder reads the header, which the seal
// covers: which stream and encoding, the temporal layer, keyframe and
// discardable (flags); `counter` numbers the seals under that key.
struct SealedVideo {
    static constexpr uint8_t keyframeFlag = 1, discardableFlag = 2;
    uint32_t generation = 0, step = 0;
    uint64_t counter = 0;
    uint8_t stream = 0, encoding = 0, temporal = 0, flags = 0;
    crypto::Bytes sealed;
    bool operator==(const SealedVideo&) const = default;
};
// The header as it is sealed with: the message's bytes before the sealed ones.
inline constexpr size_t sealedHeaderSize = 1 + 4 + 4 + 8 + 4;
crypto::Bytes sealedHeader(const SealedVideo& video);

// Sharer -> forwarder -> viewer: a packet of the sharer's sound (its
// AudioPacket's message), sealed with the stream key as a picture is, under
// the same counter. `sound` names the sharer's sound it is of (one encoding
// of the sound for every viewer that takes that format): the forwarder sends
// each viewer the packets of the sound it was subscribed to. A datagram,
// never resent.
struct SealedAudio {
    uint32_t generation = 0, step = 0;
    uint64_t counter = 0;
    uint8_t sound = 0;
    crypto::Bytes sealed;
    bool operator==(const SealedAudio&) const = default;
};
crypto::Bytes sealedHeader(const SealedAudio& audio);

// Sharer -> viewer, end to end once the host let it in, and whenever the
// stream key changes: the key of `generation` at `step` (pictures under a
// later step of the generation: the viewer ratchets it on).
struct MediaKey {
    uint32_t generation = 0, step = 0;
    crypto::Key key{};
    bool operator==(const MediaKey&) const = default;
};

// Viewer -> sharer: the newest stream key it has; sent again when pictures
// come under one it lacks, which the sharer answers with the current one.
struct MediaKeyAck {
    uint32_t generation = 0, step = 0;
    bool operator==(const MediaKeyAck&) const = default;
};

// Either end -> the other, end to end: the short stalls of what this end
// receives (stall_watch.hpp) began to come at a steady pace, changed it, or
// no longer come so (periodMs 0); their median length and how many in the
// last 10 s.
struct PathStalls {
    uint32_t periodMs = 0, stallMs = 0, stalls = 0;
    bool operator==(const PathStalls&) const = default;
};

using Message =
    std::variant<CodecOffer, ViewerSettings, StreamAnnouncement, EncodedPacket, ControlGrant, Input,
                 KeyframeRequest, StreamGap, VideoMissing, AudioFormat, AudioPacket, AudioOff, CursorShapePart,
                 CursorPosition, DataPart, DataDatagram, Refused, DesktopState, Admission, RefLost,
                 StreamEnded, SharerPaused, ForwarderHello, ForwarderWelcome, Pipe, PeerJoined, PeerLeft, Subscribe,
                 PathReport, NeedKeyframe, PeerMoved, SealedVideo, MediaKey, MediaKeyAck, SealedAudio, PathStalls>;

crypto::Bytes encode(const Message& message);
// Throws Failure(RCS_ERROR_PROTOCOL) for malformed or invalid messages.
Message decode(std::span<const uint8_t> bytes);

class Writer;
class Reader;

bool validInput(const rcs_input_event& event) noexcept;
// Every input type some version defines, as (1u << rcs_input_type) bits.
inline constexpr uint32_t allInputTypes = ((1u << (RCS_INPUT_SECURE_ATTENTION + 1)) - 1) & ~1u;
// An input event's fields, as the network and the agent link carry them (the
// reader leaves validation to validInput).
void putInputEvent(Writer& writer, const rcs_input_event& event);
rcs_input_event readInputEvent(Reader& reader);
// A stream of `codec` may be width x height: a valid picture size its codec takes.
bool validStreamSize(rcs_codec codec, uint32_t width, uint32_t height) noexcept;
// The most bytes of data and configuration one video packet carries: what a
// message holds, less room for the fields around them (here and on the agent link).
inline constexpr size_t maxVideoBytes = transport::maxRecord - 1024;
// What is wrong with a video packet, empty when nothing is: a codec this
// version defines, a picture some stream may be (validPicture), a size its
// codec takes, data, no more than maxVideoBytes; standard codecs a
// configuration and the boundaries of their format (NAL units, OBUs; VP8 and
// VP9: the configuration record, the frames, a keyframe's header). Host
// codecs are opaque.
std::string videoProblem(const EncodedPacket& packet);

} // namespace rcsdk::protocol
