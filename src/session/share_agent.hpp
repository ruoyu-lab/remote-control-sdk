// The sharer's agent for a run (agent/agent.hpp), as the session uses it. The
// agent runs for the whole run and serves one set of viewers after another;
// what it reports goes to whoever is attached: the run's stream takes the
// picture and the pointer for every viewer, the sound goes to the viewers'
// senders (AudioHub), and the control grant follows whether the system
// accepts input.
#pragma once

#include "agent/agent.hpp"
#include "agent/media_agent.hpp"
#include "agent/remote.hpp"
#include "platform/platform.hpp"
#include "session/session.hpp"
#include "seams/seams.hpp"

#include <atomic>
#include <optional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace rcsdk::session {

// What a share loop hears of the pictures: each stream's in order, on its
// pipeline's thread (agent::Events).
class VideoSink {
public:
    virtual ~VideoSink() = default;
    virtual void videoStarted(const agent::VideoStarted& started) = 0;
    virtual void videoPacket(EncodedPacket&& packet) = 0;
    virtual void videoContent(uint32_t index, const Rect& content, const platform::DisplayMode& display) = 0;
    virtual void encoderMode(uint32_t index, uint32_t encoding, rcs_codec codec, bool lowLatency,
                             bool refInvalidation, uint32_t temporalLayers, const std::string& name) = 0;
    virtual void encodingFailed(uint32_t index, uint32_t encoding, rcs_result result, const std::string& message) = 0;
    virtual void captureStatus(uint32_t index, const agent::CaptureStatus& status) = 0;
    virtual void cursor(uint32_t index, const CursorState& state) = 0;
    // What the machine shows (the viewer is told).
    virtual void desktop(const platform::DesktopState& state) = 0;
    // An agent attached, or its displays changed: the viewer is offered what
    // its machine does, and the streams say where their displays sit.
    virtual void machineChanged() = 0;
};

class ShareAgent final : public agent::Events, public agent::RemoteAgent::Observer {
public:
    // The agent of this session: in this process, or the one that attaches
    // through the session's agent link (rcs_agent_link_config).
    explicit ShareAgent(Session& session);
    ~ShareAgent() override;

    agent::Port& port() noexcept { return *port_; }
    agent::Machine machine() const { return port_->machine(); }

    // Returns once an agent has attached in this run and the settings were
    // checked against its machine (at once for an agent in this process; an
    // agent that left later is waited for by the stream, not here); throws
    // Failure when they cannot be carried out there, or when `cancelled` is set.
    void waitAttached(CancelToken cancelled);

    // The picture and the pointer go to `sink` until detachVideo(), which
    // returns once no handler of the sink is running. A failure of the
    // previous sink's stream that came after it ended is forgotten here.
    void attachVideo(VideoSink* sink);
    void detachVideo();

    // The sound captured by the agent goes to `deliver` and `status` until
    // detachAudio() (which, too, waits for handlers); periodFrames() is the
    // frames the capture delivers at a time.
    void attachAudio(platform::AudioCapture::Deliver deliver, platform::AudioCapture::Status status);
    void detachAudio();
    uint32_t audioPeriodFrames() const noexcept { return audioPeriod_; }

    // Whether the system accepts injected input, as the agent last said;
    // nothing until the attached agent has said.
    std::optional<bool> inputAvailable() const noexcept {
        const int said = inputAvailable_;
        return said < 0 ? std::nullopt : std::optional<bool>(said != 0);
    }

    // Throws what ends the run: an attached agent's machine that cannot carry
    // out the settings (also while the run waits for a viewer), and (an agent
    // in this process) its stream's failure.
    void rethrowFailure();

    // agent::RemoteAgent::Observer
    void admit(const agent::Machine& machine) override;
    void attached(const agent::Machine& machine) override;
    void detached(rcs_result result, const std::string& message) override;

    // agent::Events
    void videoStarted(const agent::VideoStarted& started) override;
    void videoPacket(EncodedPacket&& packet) override;
    void videoContent(uint32_t index, const Rect& content, const platform::DisplayMode& display) override;
    void encoderMode(uint32_t index, uint32_t encoding, rcs_codec codec, bool lowLatency, bool refInvalidation,
                     uint32_t temporalLayers, const std::string& name) override;
    void encodingFailed(uint32_t index, uint32_t encoding, rcs_result result, const std::string& message) override;
    void captureStatus(uint32_t index, const agent::CaptureStatus& status) override;
    void cursor(uint32_t index, const CursorState& state) override;
    void displays(const std::vector<rcs_display_info>& list) override;
    void desktopState(const platform::DesktopState& state) override;
    void inputAvailability(bool available) override;
    void inputResult(rcs_result last, uint64_t applied) override;
    void moduleStatus(const std::string& id, rcs_result result, const std::string& message) override;
    void moduleData(const std::string& id, Bytes bytes) override;
    void audioPcm(std::span<const float> samples, uint64_t timestampNs, uint32_t periodFrames) override;
    void audioStatus(const platform::AudioStatus& status) override;
    void failed(rcs_result result, const std::string& message) override;

private:
    // The run ends with this.
    void fatal(const FailureInfo& failure);

    Session& session_;

    std::shared_mutex videoMutex_;
    VideoSink* video_ = nullptr;
    std::shared_mutex audioMutex_;
    platform::AudioCapture::Deliver audioDeliver_;
    platform::AudioCapture::Status audioStatus_;
    std::atomic<uint32_t> audioPeriod_{0};
    std::atomic<int> inputAvailable_{-1}; // -1: not said yet.

    std::mutex failureMutex_;
    std::optional<FailureInfo> fatal_;  // Never forgotten: the run ends with it.
    std::optional<FailureInfo> stream_; // The current sink's stream's.
    std::mutex attachMutex_;
    std::condition_variable attachWake_;
    agent::RemoteAgent* remote_ = nullptr; // port_, when the agent is in another process.
    bool everAttached_ = false;            // Under attachMutex_.

    std::unique_ptr<agent::Port> port_; // Last: its threads call the handlers above.
};

// Remote input through the agent: what the viewer sends while control is
// granted reaches the system the agent runs on.
class AgentInjector final : public platform::InputInjector {
public:
    // Sets the agent's injector up for `target` and the displays of streams 1, 2, ...
    AgentInjector(agent::Port& port, const platform::CaptureTarget& target,
                  const std::vector<uint32_t>& streamDisplays, bool secureDesktop)
        : port_(port) {
        port.configureInput(target, streamDisplays, secureDesktop);
    }
    // Ctrl+Alt+Del is sent by the session's own process (Windows: the service
    // in session 0 can, the user's session cannot), to the agent's session;
    // the rest the agent executes.
    rcs_result apply(const rcs_input_event& event) override {
        return event.type == RCS_INPUT_SECURE_ATTENTION ? seams::secureAttention(port_.machine().process.processId)
                                                        : port_.applyInput(event);
    }
    void releaseAll() noexcept override { port_.releaseInput(); }

private:
    agent::Port& port_;
};

// The sound the agent captures, as the sound sender's capture.
class AgentAudioCapture final : public platform::AudioCapture {
public:
    // `start`: what to capture (its period and exclusion come with start()).
    AgentAudioCapture(ShareAgent& agent, const agent::AudioStart& start) : agent_(agent), start_(start) {}
    ~AgentAudioCapture() override { stop(); }
    void start(const platform::AudioCaptureConfig& config, Deliver deliver, Status status) override;
    void stop() noexcept override;
    uint32_t periodFrames() const noexcept override { return agent_.audioPeriodFrames(); }

private:
    ShareAgent& agent_;
    agent::AudioStart start_;
    bool started_ = false;
};

} // namespace rcsdk::session
