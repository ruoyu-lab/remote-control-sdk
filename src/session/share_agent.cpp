#include "session/share_agent.hpp"

#include "util/log.hpp"

namespace rcsdk::session {

ShareAgent::ShareAgent(Session& session) : session_(session) {
    const auto& link = session.config().agentLink;
    if (hasRemoteAgent(session.config())) {
        agent::RemoteAgent::Options options;
        options.endpoint = link.endpoint;
        std::copy(std::begin(link.key), std::end(link.key), options.key.begin());
        options.access = link.access == RCS_AGENT_ACCESS_ANY_USER ? transport::LocalAccess::anyUser
                                                                  : transport::LocalAccess::privileged;
        auto remote = std::make_unique<agent::RemoteAgent>(*this, *this, options);
        remote_ = remote.get();
        port_ = std::move(remote);
        return;
    }
    auto embedded = std::make_unique<agent::MediaAgent>(*this);
    embedded->setHostCapture([&session] { return session.hostVideo().capture(); });
    if (const auto& host = session.config().hostEncoder; host.codecs[0]) {
        embedded->setHostEncoder(host);
    }
    const auto& callbacks = session.callbacks();
    if (callbacks.on_frame || callbacks.on_gpu_frame) {
        embedded->setPreview([&session](const VideoFrame& frame) {
            Session::CallbackThread mark(session);
            session.deliver(frame);
        });
    }
    port_ = std::move(embedded);
    session.agentAttached(port_->machine()); // Reported once: the session's own agent.
}

ShareAgent::~ShareAgent() = default;

void ShareAgent::attachVideo(VideoSink* sink) {
    std::unique_lock lock(videoMutex_);
    video_ = sink;
    std::lock_guard failure(failureMutex_);
    stream_.reset();
}

void ShareAgent::detachVideo() {
    std::unique_lock lock(videoMutex_);
    video_ = nullptr;
}

void ShareAgent::attachAudio(platform::AudioCapture::Deliver deliver, platform::AudioCapture::Status status) {
    std::unique_lock lock(audioMutex_);
    audioDeliver_ = std::move(deliver);
    audioStatus_ = std::move(status);
}

void ShareAgent::detachAudio() {
    std::unique_lock lock(audioMutex_);
    audioDeliver_ = {};
    audioStatus_ = {};
}

void ShareAgent::waitAttached(CancelToken cancelled) {
    if (!remote_) {
        return;
    }
    std::unique_lock lock(attachMutex_);
    for (;;) {
        rethrowFailure();
        if (everAttached_) {
            return;
        }
        throwIfCancelled(cancelled);
        attachWake_.wait_for(lock, 20ms);
    }
}

// An agent that passed the handshake asks to attach: the host decides
// (on_agent_attach; without it, every agent holding the key is taken).
void ShareAgent::admit(const agent::Machine& machine) {
    const auto& link = session_.config().agentLink;
    if (!link.on_agent_attach) {
        return;
    }
    const rcs_agent_info info = session_.agentInfoOf(machine, false);
    if (!session_.callHostWith(link.user_data, link.on_agent_attach, &info)) {
        fail(RCS_ERROR_AUTHENTICATION, "The host refused it (on_agent_attach)");
    }
}

// An agent attached: it is reported, and the settings are checked against its
// machine. A machine that cannot carry them out ends the run, also while it
// waits for a viewer.
void ShareAgent::attached(const agent::Machine& machine) {
    inputAvailable_ = -1; // This agent says.
    Session::CallbackThread mark(session_);
    session_.modules().machineGone(); // Those of an agent it replaced; its own say they run.
    session_.agentAttached(machine);
    try {
        session_.checkMachine(machine);
    } catch (const Failure& failure) {
        log::warning("agent", std::string("The agent's machine cannot carry out the settings (") +
                                  rcs_result_message(failure.result()) + "): " + failure.what());
        fatal({failure.result(), failure.what()});
    }
    {
        std::lock_guard lock(attachMutex_);
        everAttached_ = true;
        attachWake_.notify_all();
    }
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->machineChanged(); // The viewer is offered what this agent's machine does.
    }
}

void ShareAgent::detached(rcs_result result, const std::string& message) {
    Session::CallbackThread mark(session_);
    session_.modules().machineGone();
    session_.agentDetached(result, message);
}

void ShareAgent::fatal(const FailureInfo& failure) {
    {
        std::lock_guard lock(failureMutex_);
        if (!fatal_) {
            fatal_ = failure;
        }
    }
    std::lock_guard lock(attachMutex_);
    attachWake_.notify_all();
}

void ShareAgent::rethrowFailure() {
    std::lock_guard lock(failureMutex_);
    if (fatal_) {
        fail(fatal_->result, fatal_->message);
    }
    if (stream_) {
        fail(stream_->result, stream_->message);
    }
}

// ---- Events ------------------------------------------------------------------------------------

void ShareAgent::videoStarted(const agent::VideoStarted& started) {
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->videoStarted(started);
    }
}

void ShareAgent::videoPacket(EncodedPacket&& packet) {
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->videoPacket(std::move(packet));
    }
}

void ShareAgent::videoContent(uint32_t index, const Rect& content, const platform::DisplayMode& display) {
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->videoContent(index, content, display);
    }
}

void ShareAgent::encoderMode(uint32_t index, uint32_t encoding, rcs_codec codec, bool lowLatency,
                             bool refInvalidation, uint32_t temporalLayers, const std::string& name) {
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->encoderMode(index, encoding, codec, lowLatency, refInvalidation, temporalLayers, name);
    }
}

void ShareAgent::encodingFailed(uint32_t index, uint32_t encoding, rcs_result result, const std::string& message) {
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->encodingFailed(index, encoding, result, message);
    }
}

void ShareAgent::captureStatus(uint32_t index, const agent::CaptureStatus& status) {
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->captureStatus(index, status);
    }
}

void ShareAgent::cursor(uint32_t index, const CursorState& state) {
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->cursor(index, state);
    }
}

void ShareAgent::displays(const std::vector<rcs_display_info>& list) {
    {
        Session::CallbackThread mark(session_);
        session_.agentDisplays(list);
    }
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->machineChanged(); // Where the shared displays sit may have changed.
    }
}

void ShareAgent::desktopState(const platform::DesktopState& state) {
    Session::CallbackThread mark(session_);
    session_.desktopChanged(state); // The host hears it whether or not a viewer is connected...
    std::shared_lock lock(videoMutex_);
    if (video_) {
        video_->desktop(state); // ...and the viewer that is.
    }
}

void ShareAgent::inputAvailability(bool available) {
    inputAvailable_ = available ? 1 : 0;
}

void ShareAgent::moduleStatus(const std::string& id, rcs_result result, const std::string& message) {
    session_.modules().machineStatus(id, result, message);
}

void ShareAgent::moduleData(const std::string& id, Bytes bytes) {
    session_.modules().fromMachine(id, bytes);
}

void ShareAgent::inputResult(rcs_result last, uint64_t applied) {
    session_.updateStats([&](Stats& s) {
        s.input.last_result = last;
        s.input.applied += applied;
    });
}

void ShareAgent::audioPcm(std::span<const float> samples, uint64_t timestampNs, uint32_t periodFrames) {
    std::shared_lock lock(audioMutex_);
    audioPeriod_ = periodFrames;
    if (audioDeliver_) {
        audioDeliver_(samples, timestampNs);
    }
}

void ShareAgent::audioStatus(const platform::AudioStatus& status) {
    std::shared_lock lock(audioMutex_);
    if (audioStatus_) {
        audioStatus_(status);
    }
}

// The agent in this process failed: the stream of the share loop that runs
// ends the run. One that comes after its stream ended (the viewer left as it
// failed) has nothing left to end.
void ShareAgent::failed(rcs_result result, const std::string& message) {
    std::shared_lock video(videoMutex_);
    if (!video_) {
        log::info("agent", "The agent failed after its stream ended (" + std::string(rcs_result_message(result)) +
                               "): " + message);
        return;
    }
    log::warning("agent", "The agent failed (" + std::string(rcs_result_message(result)) + "): " + message);
    std::lock_guard lock(failureMutex_);
    if (!stream_) {
        stream_ = FailureInfo{result, message};
    }
}

// ---- The sound ---------------------------------------------------------------------------------

void AgentAudioCapture::start(const platform::AudioCaptureConfig& config, Deliver deliver, Status status) {
    agent_.attachAudio(std::move(deliver), std::move(status));
    started_ = true;
    auto start = start_;
    start.excludeCurrentProcess = config.excludeCurrentProcess;
    start.periodFrames = config.periodFrames;
    agent_.port().startAudio(start);
}

void AgentAudioCapture::stop() noexcept {
    if (!started_) {
        return;
    }
    started_ = false;
    agent_.detachAudio();
    agent_.port().stopAudio();
}

} // namespace rcsdk::session
