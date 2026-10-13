#include "agent/agent.hpp"

#include "codecs/codecs.hpp"

#include "seams/seams.hpp"

namespace rcsdk::agent {

// Hardware does not change while the process runs; probing it is slow on Windows.
const std::vector<platform::CodecImplementation>& codecImplementations() {
    static const std::vector<platform::CodecImplementation> value = codecs::implementations();
    return value;
}

Machine localMachine() {
    Machine machine;
    machine.capabilities = platform::capabilities();
    seams::applyCapabilities(machine.capabilities);
    machine.implementations = codecImplementations();
    for (const auto& i : machine.implementations) {
        const bool hardware = i.acceleration == RCS_ACCELERATION_HARDWARE;
        uint32_t& mask = i.direction == RCS_CODEC_ENCODER
                             ? (hardware ? machine.capabilities.hardware_encoders : machine.capabilities.software_encoders)
                             : (hardware ? machine.capabilities.hardware_decoders : machine.capabilities.software_decoders);
        mask |= 1u << i.codec;
    }
    machine.capabilities.audio_capture_backends = platform::audioCaptureBackends();
    machine.capabilities.audio_exclusion = platform::audioExclusionAvailable();
    machine.defaultAudioBackend = platform::defaultAudioCaptureBackend();
    for (const auto backend : {RCS_AUDIO_CAPTURE_SCREENCAPTUREKIT, RCS_AUDIO_CAPTURE_CORE_AUDIO_TAP,
                               RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK}) {
        if (platform::audioCapturePeriodSelectable(backend)) {
            machine.audioPeriodBackends |= 1u << backend;
        }
    }
    machine.displays = platform::displays();
    machine.screenPermission = platform::screenPermission();
    machine.inputPermission = seams::inputPermission();
    machine.process = transport::thisProcess();
    return machine;
}

rcs_agent_info agentInfo(const Machine& machine, bool attached, rcs_result result, const std::string& message) {
    rcs_agent_info info = RCS_AGENT_INFO_INIT;
    info.attached = attached;
    info.result = result;
    copyText(info.message, message);
    info.process_id = machine.process.processId;
    info.os_session = machine.process.osSession;
    copyText(info.account, machine.process.account);
    if (machine.process.program.size() < sizeof(info.program)) { // A path cut short could name another program.
        copyText(info.program, machine.process.program);
    }
    info.platform = machine.capabilities.platform;
    info.capture_sources = machine.capabilities.capture_sources;
    info.picture_formats = machine.capabilities.picture_formats;
    info.input_types = machine.capabilities.input_types;
    info.hardware_encoders = machine.capabilities.hardware_encoders;
    info.software_encoders = machine.capabilities.software_encoders;
    info.screen_permission = machine.screenPermission;
    info.input_permission = machine.inputPermission;
    info.displays = uint32_t(machine.displays.size());
    info.secure_desktop = machine.capabilities.secure_desktop;
    return info;
}

} // namespace rcsdk::agent
