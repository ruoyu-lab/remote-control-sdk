// What this system can do of a session's configuration: checked for every
// extension when a session starts, and for the changed ones when the host
// updates a running session. A refusal names the setting and the reason; the
// SDK never replaces what the host asked for.
#pragma once

#include "agent/agent.hpp"
#include "platform/platform.hpp"
#include "session/config.hpp"
#include "util/api.hpp"

#include <array>
#include <string>
#include <vector>

namespace rcsdk::session {

// This system's codec implementations and capabilities, probed once per process.
using agent::codecImplementations;
const rcs_capabilities& capabilities();

struct Refusal {
    rcs_result result = RCS_OK;
    std::string reason;
    explicit operator bool() const noexcept { return result != RCS_OK; }
};

// The peers' offers, as (1u << codec) bits: the sharer's for a viewer, the
// viewers' for a sharer. Video: what every peer decodes (one stream serves
// them all), with the largest picture each codec takes there (rcs_peer_info;
// the smallest of the viewers'); sound: each peer's (encoded for each). 0 and
// none before a peer is known.
struct PeerOffers {
    uint32_t codecs = 0;
    // Each peer's sound codecs (each viewer's; a viewer's sharer), as
    // (1u << rcs_audio_codec) bits: what it receives (decodes), what it sends.
    std::vector<uint32_t> receiveAudio, sendAudio;
    std::array<rcs_codec_limit, RCS_MAX_CODECS> codecLimits{};
};

// What a sharer's settings are checked against: the machine its agent runs on
// (this system, or a remote agent's once it attached). Null where it is not
// known yet: the checks that depend on the machine wait for the agent.
using MachineRef = const agent::Machine*;

// What the machine captures for these settings, as the agent takes it.
platform::CaptureTarget captureTarget(const rcs_capture_config& capture);
// How the machine executes the viewer's input for these settings.
platform::InputOptions inputOptions(const SessionConfig& config);
// A capture's settings against the machine that captures (a session's, and
// rcs_capture_picture's `what`): a source it does not offer, a display it
// does not list, a region not inside its display, missing permission.
Refusal checkCapture(const rcs_capture_config& capture, const agent::Machine& machine);

// Everything start() needs, before any thread or connection exists.
Refusal checkStart(const SessionConfig& config, const rcs_session_callbacks& callbacks, MachineRef machine);
// The extensions in `changed` ((1u << type) bits) of a running session.
Refusal checkChange(const SessionConfig& next, uint32_t changed, const PeerOffers& peer,
                    const rcs_session_callbacks& callbacks, MachineRef machine);

} // namespace rcsdk::session
