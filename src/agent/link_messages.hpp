// The link between a session and its agent (agent/remote.hpp).
//
// It opens with a prefix in the clear, before anything else: each end sends
// the link's magic and its RCSDK_VERSION, the agent first. Ends of different
// versions tell each other apart there, before they try to understand anything
// else (the secure channel's handshake, the messages); later versions keep the
// prefix as it is. Then comes the secure channel, keyed with the link's key
// bound to the link (linkCredentials), so it is never mistaken for a network
// connection holding the same key, and inside it the messages below: the
// Port's calls going one way and the Events coming the other. Both ends are
// the same SDK version on one machine, so fields have the layout of the
// machine they run on; decoding still checks every field, because the agent
// may be a process that has been compromised.
#pragma once

#include "agent/agent.hpp"
#include "crypto/crypto.hpp"
#include "transport/credentials.hpp"

#include <map>
#include <optional>
#include <variant>

namespace rcsdk::agent::link {

// ---- The prefix --------------------------------------------------------------------------------

inline constexpr size_t prefixSize = 12;
// "RCSLINK", the link's format, and `version`, big-endian.
crypto::Bytes prefix(uint32_t version = RCSDK_VERSION);
// Checks the other end's prefix against this end's. Throws Failure:
// UNSUPPORTED with both versions for another version or format, NETWORK for
// bytes that are no link's prefix at all.
void checkPrefix(std::span<const uint8_t> bytes, const char* peer);
// RCSDK_VERSION as "major.minor.patch".
std::string versionText(uint32_t version);

// What the secure channel of a link with this key authenticates with.
transport::Credentials linkCredentials(const crypto::Key& key);

// ---- The messages ------------------------------------------------------------------------------

// The agent's first message: what its machine offers. The session answers
// with Accepted, or Refused.
struct AgentHello {
    Machine machine;
};
struct Accepted {};
// Instead of Accepted: the session does not take this agent (its host refused
// it, RCS_ERROR_AUTHENTICATION), and closes the link.
struct Refused {
    rcs_result result = RCS_ERROR_AUTHENTICATION;
    std::string reason;
};
// The session's last message to an agent another one replaced: it ends.
struct Replaced {};
struct Heartbeat {};

// Session to agent: the Port. `index`: the stream's (rcs_streams_config);
// `encoding`: which of its encodings (RCS_EXT_ENCODINGS).
struct StartVideo {
    VideoStart start;
};
struct StopVideo {
    uint8_t index = 0;
    uint8_t encoding = 0;
};
struct SetRate {
    uint8_t index = 0;
    uint8_t encoding = 0;
    uint32_t bitrate = 0;
    bool budgeted = false;
};
struct SetCursorInVideo {
    bool inVideo = true;
};
struct SetInputBoost {
    uint32_t ms = 0;
};
struct RequestKeyframe {
    uint8_t index = 0;
    uint8_t encoding = 0;
};
// Drop the pictures after `sequence` from the encoder's references (Port::invalidateAfter).
struct InvalidateAfter {
    uint8_t index = 0;
    uint8_t encoding = 0;
    uint64_t sequence = 0;
};
// The session's send backlog of each encoding, when it changed.
struct Backlog {
    std::array<uint64_t, RCS_MAX_ENCODINGS> ns{};
};
struct StartAudio {
    AudioStart start;
};
struct StopAudio {};
struct ConfigureInput {
    platform::CaptureTarget target;
    std::vector<uint32_t> streamDisplays;
    bool secureDesktop = false;
};
struct ApplyInput {
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
};
struct ReleaseInput {};
// A module's machine's half (Port::startModule): it runs from now on, and
// ends; ModuleStatus answers a start.
struct ModuleStart {
    std::string id;
    uint32_t version = 0;
};
struct ModuleStop {
    std::string id;
};
// The machine's windows, asked for by number: the answer (WindowList) says
// which ask it answers.
struct ListWindows {
    uint32_t serial = 0;
};

// Agent to session: the Events. `index`: the stream's.
struct Started {
    VideoStarted started;
};
struct Packet {
    EncodedPacket packet;
};
// The content rectangle is checked against the stream's size by the receiver
// (RemoteAgent), which knows the stream.
struct Content {
    uint8_t index = 0;
    Rect content;
    platform::DisplayMode display;
};
struct Mode {
    uint8_t index = 0;
    uint8_t encoding = 0;
    rcs_codec codec = RCS_CODEC_H264;
    bool lowLatency = false;
    bool refInvalidation = false;
    uint8_t temporalLayers = 1;
    std::string name;
};
struct Capture {
    uint8_t index = 0;
    CaptureStatus status;
};
// The pointer as a stream's capture sees it; the image comes with the first
// message and whenever it changes.
struct Pointer {
    uint8_t index = 0;
    CursorState state;
};
struct DisplayList {
    std::vector<rcs_display_info> list;
};
struct Availability {
    bool available = false;
};
struct Desktop {
    platform::DesktopState state;
};
struct InputResults {
    rcs_result last = RCS_OK;
    uint64_t applied = 0;
};
struct Pcm {
    uint64_t timestampNs = 0;
    uint32_t periodFrames = 0;
    std::vector<float> samples;
};
struct AudioState {
    platform::AudioStatus status;
};
struct WindowList {
    uint32_t serial = 0;
    std::vector<rcs_window_info> windows;
};
// Both directions, a module's message (Port::moduleData, Events::moduleData)
// in parts of at most modulePart bytes, those of one message one after
// another (rcs_module_definition.max_message_bytes may well be more than one
// record of the secure channel).
inline constexpr size_t modulePart = 4u << 20;
struct ModuleData {
    std::string id;
    uint32_t index = 0, count = 1;
    crypto::Bytes bytes;
};
// Sends `bytes` as ModuleData parts through `send`, in order; the caller
// keeps another message of the module from coming between.
template <class Send>
void sendModuleData(Send&& send, const std::string& id, const crypto::Bytes& bytes) {
    const size_t count = (bytes.size() + modulePart - 1) / modulePart;
    for (size_t i = 0; i < count; ++i) {
        const auto begin = bytes.begin() + ptrdiff_t(i * modulePart);
        const auto end = begin + ptrdiff_t(std::min(modulePart, bytes.size() - i * modulePart));
        send(ModuleData{id, uint32_t(i), uint32_t(count), crypto::Bytes(begin, end)});
    }
}
// Each module's message put back together from its parts.
class ModuleAssembly {
public:
    // The message once its last part came. Throws Failure(PROTOCOL) for a
    // part out of order, or more than `limit` bytes.
    std::optional<crypto::Bytes> add(ModuleData&& part, size_t limit);

private:
    struct Partial {
        uint32_t count = 0, next = 0;
        crypto::Bytes bytes;
    };
    std::map<std::string, Partial> partial_;
};
// A lower encoding could not start, or failed (Events::encodingFailed).
struct EncodingFailed {
    uint8_t index = 0;
    uint8_t encoding = 0;
    rcs_result result = RCS_ERROR_CODEC;
    std::string message;
};
// From the agent: whether a module's half runs, as Events::moduleStatus says.
struct ModuleStatus {
    std::string id;
    rcs_result result = RCS_OK;
    std::string message;
};
// The agent cannot go on: it leaves the session with this error, which is
// never RCS_OK.
struct Failed {
    rcs_result result = RCS_ERROR_INTERNAL;
    std::string message;
};
// After its last words (Replaced, Failed) an end waits this long at most for
// the other to close the link, reading on. Closing first, it would make the
// other end's sends fail, and that end could give up on the link before it
// read why.
inline constexpr auto lastWordsWait = 2s;

using Message =
    std::variant<AgentHello, Accepted, Refused, Replaced, Heartbeat, StartVideo, StopVideo, SetRate, SetCursorInVideo,
                 SetInputBoost, RequestKeyframe, InvalidateAfter, Backlog, StartAudio, StopAudio, ConfigureInput, ApplyInput,
                 ReleaseInput, ModuleStart, ModuleStop, ListWindows, Started, Packet, Content, Mode, Capture, Pointer,
                 DisplayList, Availability, Desktop, InputResults, Pcm, AudioState, WindowList, ModuleData,
                 ModuleStatus, Failed, EncodingFailed>;

crypto::Bytes encode(const Message& message);
// Throws Failure(PROTOCOL) for malformed or invalid messages.
Message decode(std::span<const uint8_t> bytes);

// Whether a content rectangle lies within a picture of this size.
bool fits(const Rect& content, uint32_t width, uint32_t height) noexcept;

} // namespace rcsdk::agent::link
