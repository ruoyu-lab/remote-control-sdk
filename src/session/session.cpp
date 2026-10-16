#include "session/session.hpp"

#include "media/gpu_frame.hpp"
#include "protocol/messages.hpp"
#include "session/connect.hpp"
#include "session/negotiation.hpp"
#include "session/path_info.hpp"
#include "session/share_agent.hpp"
#include "session/share_viewer.hpp"
#include "transport/ice.hpp"
#include "transport/network_changes.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <shared_mutex>
#include <thread>

namespace rcsdk::session {
namespace {

thread_local const Session* callbackSession = nullptr;

std::string pauseText(uint32_t paused) {
    if (!paused) {
        return "nothing";
    }
    std::string text = paused & RCS_PAUSE_VIDEO ? "video" : "";
    if (paused & RCS_PAUSE_AUDIO) {
        text += text.empty() ? "audio" : " and audio";
    }
    return text;
}

const char* stateName(rcs_state state) noexcept {
    static const char* names[] = {"idle",    "starting",     "listening", "connecting",
                                  "running", "reconnecting", "stopped",   "failed"};
    return state <= RCS_STATE_FAILED ? names[state] : "unknown";
}

} // namespace

Session::CallbackThread::CallbackThread(const Session& session) : previous_(callbackSession) {
    callbackSession = &session;
}
Session::CallbackThread::~CallbackThread() {
    callbackSession = previous_;
}

Session::Session(SessionConfig config, const rcs_session_callbacks& callbacks, log::SourceRef logSource)
    : config_(std::move(config)), callbacks_(callbacks), logSource_(std::move(logSource)),
      live_(std::make_shared<const SessionConfig>(config_)), viewers_(std::make_unique<ViewerSet>()),
      modules_(std::make_unique<ModuleSet>(*this)) {
    logSource_->level = config_.log.max_level;
    clearStreams();
    background_ = seams::appInBackground();
    pauseChanged();
    appState_ = seams::watchAppState([this](bool background) {
        background_ = background;
        pauseChanged();
    });
}

Session::~Session() {
    appState_.reset(); // No more calls from the application's state, then the run ends.
    stop();
}

bool Session::onCallbackThread() const noexcept {
    return callbackSession == this;
}

bool Session::running() const noexcept {
    const rcs_state current = state_.load();
    return current != RCS_STATE_IDLE && current != RCS_STATE_STOPPED && current != RCS_STATE_FAILED;
}

// ---- Lifecycle ---------------------------------------------------------------------------

rcs_result Session::start() {
    std::lock_guard lock(lifecycleMutex_);
    if (running()) {
        fail(RCS_ERROR_INVALID_STATE, "The session runs already; stop it first");
    }
    join(); // The previous run has ended; its worker may still be returning.
    {
        // What the previous run knew is gone before this one is checked: a
        // new run's agent attaches afresh, and its peer is not known yet.
        std::lock_guard data(dataMutex_);
        stats_ = {};
        departed_.clear();
        clearStreams();
        peer_ = RCS_PEER_INFO_INIT;
        desktop_ = RCS_DESKTOP_INFO_INIT;
        agentMachine_.reset();
        agentAttached_ = false;
        agentLeft_ = {};
        failure_ = {};
    }
    {
        // Viewer ids start at 1 each run.
        std::unique_lock viewers(viewers_->mutex);
        viewers_->nextId = 1;
        viewers_->chosenMain = 0;
        viewers_->joining = 0;
        viewers_->streamCodec = 0;
    }
    {
        // Checked and marked started under liveMutex_: update() sees either a
        // stopped session it may reconfigure or a running one.
        std::lock_guard live(liveMutex_);
        const auto machine = machineForChecks();
        if (auto refusal = checkStart(config_, callbacks_, machine ? &*machine : nullptr)) {
            log::warning("session",
                         std::string("Start refused (") + rcs_result_message(refusal.result) + "): " + refusal.reason);
            std::lock_guard data(dataMutex_);
            failure_ = {refusal.result, std::move(refusal.reason)};
            return failure_.result;
        }
        state_ = RCS_STATE_STARTING; // Reported through on_event by the worker.
        applyControl();
        pinAttempts_ = std::make_shared<transport::PinAttempts>(
            config_.identity.pin_attempts, [this](uint32_t failures, uint32_t left) { pinFailed(failures, left); });
    }
    {
        std::lock_guard signals(signalMutex_); // A first connection waits for the sharer's answer as long as it takes.
        signalDeadline_.reset();
    }
    if (usesHostChannel(config_)) {
        const auto& link = config_.hostTransport; // Fixed for the run.
        const bool stream = link.host_channel == RCS_HOST_CHANNEL_STREAM;
        std::lock_guard host(hostMutex_);
        if (routedChannel()) {
            hostRoutes_ = std::make_shared<protocol::RouteDemux>(
                stream ? protocol::RouteDemux::Kind::stream : protocol::RouteDemux::Kind::datagram,
                [this, &link](uint64_t route, std::span<const uint8_t> bytes) {
                    return callHostWith(link.user_data, link.on_transport_send, route, bytes.data(),
                                        uint32_t(bytes.size())) != 0;
                });
        } else {
            hostChannel_ = std::make_shared<transport::HostChannel>(
                stream ? transport::HostKind::stream : transport::HostKind::datagram,
                [this, &link](std::span<const uint8_t> bytes) {
                    return callHostWith(link.user_data, link.on_transport_send, uint64_t(0), bytes.data(),
                                        uint32_t(bytes.size())) != 0;
                });
        }
    }
    {
        std::lock_guard reported(reportedMutex_);
        reportedControl_ = RCS_CONTROL_INFO_INIT;
    }
    passwordKeys_ = std::make_shared<transport::PasswordKeys>(); // A new salt per run.
    stopRequested_ = false;
    leave_ = RCS_STOP_NORMAL;
    cancelled_ = false;
    keyframeRequested_ = false;
    viewControl_.granted = false;
    viewControl_.inputs.clear();
    worker_ = std::thread([this] {
        const log::Scope scope(logSource_); // Every thread of the run takes it from here.
        run();
    });
    return RCS_OK;
}

void Session::stop(rcs_stop_reason reason) {
    std::lock_guard lock(lifecycleMutex_);
    leave_ = reason;
    stopRequested_ = true;
    cancelled_ = true;
    viewControl_.inputs.wake();
    join();
}

void Session::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Session::run() noexcept {
    setState(RCS_STATE_STARTING);
    pauseChanged(true);
    modules_->start();
    if (config_.role == RCS_ROLE_SHARE && usesIce(config_.connect.transport)) {
        // Viewers' signals go to the run's listener from now on; those that
        // came before start are the first it answers.
        auto listener = std::make_unique<IceListener>(*this);
        std::lock_guard lock(signalMutex_);
        for (auto& early : earlySignals_) {
            listener->signal(std::move(early));
        }
        earlySignals_.clear();
        iceListener_ = std::move(listener);
    }
    FailureInfo failed;
    for (;;) {
        connected_ = false;
        Next next;
        try {
            if (config_.role == RCS_ROLE_SHARE) {
                if (!shareAgent_) {
                    auto agent = std::make_unique<ShareAgent>(*this);
                    {
                        std::lock_guard lock(agentMutex_);
                        shareAgent_ = std::move(agent);
                    }
                    modules_->agentReady(); // The machine's halves start there.
                }
                runShare(*this);
            } else {
                runView(*this);
            }
        } catch (const Failure& failure) {
            // Whatever ends a connection while stop() tears it down is part of stopping.
            if (!stopRequested_) {
                next = resumeAfter(failure);
                if (!next.resume) {
                    failed = {failure.result(), failure.what()};
                }
            }
        } catch (const std::exception& exception) {
            failed = {RCS_ERROR_INTERNAL, exception.what()};
        } catch (...) {
            failed = {RCS_ERROR_INTERNAL, "Unexpected internal error"};
        }
        releaseConnection();
        if (!next.resume) {
            break;
        }
        {
            // What was known of the peer and its stream; a run that ended keeps it.
            std::lock_guard lock(dataMutex_);
            clearStreams();
            peer_ = RCS_PEER_INFO_INIT;
            if (config_.role == RCS_ROLE_VIEW) {
                desktop_ = RCS_DESKTOP_INFO_INIT; // The sharer says again once connected.
            }
        }
        if (next.report) { // A host that asks on this event hears nothing of the connection that ended.
            setState(*next.report, next.result);
        }
        cancelled_ = false; // Set by the helper threads of the connection that ended.
        if (stopRequested_) {
            break;
        }
        // A sharer that is restarting is coming back at once; otherwise the
        // viewer waits its interval between attempts.
        if (config_.role == RCS_ROLE_VIEW && reconnectWait_) {
            const auto interval = std::chrono::milliseconds(liveConfig()->networkTuning.reconnect_interval_ms);
            for (const auto until = Clock::now() + interval; Clock::now() < until && !cancelled_;) {
                std::this_thread::sleep_for(20ms);
            }
        }
    }
    cancelled_ = true;
    modules_->stop(); // Every connection ended; the agent's halves stop before it goes.
    {
        std::lock_guard lock(agentMutex_);
        shareAgent_.reset(); // Its threads end with the run.
    }
    {
        std::lock_guard lock(dataMutex_); // The run's agent is gone with it.
        agentAttached_ = false;
    }
    std::unique_ptr<IceListener> listener; // Its attempts end with the run.
    {
        std::lock_guard lock(signalMutex_); // Answers belong to one run; viewers' signals come for the next.
        listener = std::move(iceListener_);
        answers_.clear();
    }
    if (listener) {
        // The viewers' signals this run did not answer, the first the next
        // run answers: e.g. viewers that reconnect while a sharer restarts.
        auto unanswered = listener->finish();
        listener.reset();
        std::lock_guard lock(signalMutex_);
        earlySignals_.insert(earlySignals_.begin(), std::make_move_iterator(unanswered.begin()),
                             std::make_move_iterator(unanswered.end()));
        while (earlySignals_.size() > maxEarlySignals) {
            earlySignals_.pop_front();
        }
    }
    if (const auto channel = hostChannel()) { // The host's channel belongs to one run; its count stays.
        updateStats([&](Stats& s) { s.network.host_refused = channel->refused(); });
    }
    if (const auto routes = hostRoutes()) {
        updateStats([&](Stats& s) { s.network.host_refused = routes->refused(); });
    }
    {
        std::lock_guard host(hostMutex_);
        hostChannel_.reset();
        hostRoutes_.reset();
    }
    if (failed.result != RCS_OK && !stopRequested_) {
        log::error("session",
                   std::string("Session failed (") + rcs_result_message(failed.result) + "): " + failed.message);
        {
            std::lock_guard lock(dataMutex_);
            failure_ = std::move(failed);
        }
        setState(RCS_STATE_FAILED);
    } else {
        log::info("session", "Session stopped");
        setState(RCS_STATE_STOPPED);
    }
}

Session::Next Session::resumeAfter(const Failure& failure) {
    const rcs_result result = failure.result();
    const auto live = liveConfig();
    const auto& reconnect = live->reconnect;
    const auto window = std::chrono::milliseconds(live->networkTuning.reconnect_window_ms);
    if (config_.role == RCS_ROLE_SHARE) {
        // The viewers, or the paths to them, ended the run (its last viewer
        // left, or one that came could not be served): a sharer that keeps
        // listening waits for the next one. Failures of this end (capture,
        // codecs, its agent, the port) end the run.
        if (!dynamic_cast<const PeerFailure*>(&failure) || !reconnect.keep_listening) {
            return {};
        }
        log::info("session", std::string("No viewer is left (") + failure.what() + "); listening for the next one");
        return {true, RCS_STATE_LISTENING, result};
    }
    // View: the path broke while a connection ran (or the sharer said it is
    // restarting), or an attempt to get it back failed; a sharer that closed
    // the connection for good is gone, and one that broke the protocol is not
    // tried again. A sharer that has no room yet may still hold this end's
    // lost connection: busy is tried again while reconnecting.
    const bool busy = result == RCS_ERROR_BUSY;
    if ((result != RCS_ERROR_NETWORK && result != RCS_ERROR_RESTARTING && !busy) || !reconnect.reconnect) {
        return {};
    }
    const auto now = Clock::now();
    // A sharer that restarted, or no longer knows the connection, is there to answer at once.
    reconnectWait_ = result != RCS_ERROR_RESTARTING && !dynamic_cast<const protocol::UnknownConnection*>(&failure);
    Next next{true, std::nullopt, result};
    if (connected_ && !busy) {
        // A connection kept while its path was lost broke then, and said so.
        lostAt_ = pathLostAt_.value_or(now);
        log::warning("session", std::string("The connection was lost (") + failure.what() + "); reconnecting");
        if (state_ != RCS_STATE_RECONNECTING) {
            next.report = RCS_STATE_RECONNECTING;
        }
        if (window.count() && now - lostAt_ >= window) {
            log::warning("session", "Could not reconnect within " + secondsText(window) + "; giving up");
            return {};
        }
    } else if (state_ != RCS_STATE_RECONNECTING) {
        return {}; // The first connection failed, or the sharer was busy for it.
    } else if (window.count() && now - lostAt_ >= window) {
        log::warning("session", "Could not reconnect within " + secondsText(window) + "; giving up");
        return {};
    } else {
        log::info("session", std::string("Reconnecting failed (") + failure.what() + "); trying again");
    }
    std::lock_guard lock(signalMutex_);
    if (window.count()) {
        signalDeadline_ = lostAt_ + window;
    }
    return next;
}

void Session::releaseConnection() {
    {
        std::lock_guard lock(shareControl_.mutex);
        shareControl_.release(0);
        shareControl_.injector.reset();
    }
    deliverHostRelease();
    viewControl_.granted = false;
    viewControl_.inputs.clear();
    keyframeRequested_ = false;
    if (pausedByPeer_.exchange(0)) { // A viewer's sharer says it again on the next connection.
        pauseChanged();
    }
    if (dataQueue_.clear()) { // What the connection did not get to is dropped, as documented.
        dataRoom(0);          // A host waiting for room has it now.
    }
    if (config_.role == RCS_ROLE_VIEW) {
        controlChanged(); // The grant ends with the connection.
    }
}

void Session::updateStats(rcs_viewer_id viewer, const std::function<void(Stats&)>& update) {
    if (!viewer) {
        updateStats(update);
        return;
    }
    std::shared_lock lock(viewers_->mutex);
    if (Viewer* found = viewers_->find(viewer)) {
        found->updateStats(update);
    }
}

void Session::connectionEstablished() {
    connected_ = true;
    pathLostAt_.reset();
    dataQueue_.open();
    {
        std::lock_guard lock(signalMutex_);
        signalDeadline_.reset();
    }
    updateStats([](Stats& s) { ++s.network.connections; });
}

// ---- Settings ----------------------------------------------------------------------------

// These take only liveMutex_, never lifecycleMutex_: callbacks may change
// settings while stop() holds that and waits for them.
void Session::update(const rcs_extension* changes) {
    std::unique_lock live(liveMutex_);
    SessionConfig next = config_;
    const uint32_t changed = applyExtensions(next, changes);
    if (next.modules.size() != config_.modules.size()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "RCS_EXT_MODULE: " + next.modules.back().module->id +
                                             " is not one of the session's modules; a session uses those it was "
                                             "created with");
    }
    commit(live, next, changed);
    if (changed & (1u << RCS_EXT_BACKGROUND)) {
        if (live.owns_lock()) {
            live.unlock();
        }
        pauseChanged();
    }
}

void Session::setPreset(rcs_preset preset) {
    std::unique_lock live(liveMutex_);
    SessionConfig next = config_;
    const uint32_t changed = applyPreset(next, preset);
    commit(live, next, changed);
}

void Session::setIdentity(const rcs_identity& identity) {
    std::unique_lock live(liveMutex_);
    SessionConfig next = config_;
    applyIdentity(next, identity);
    if (pinAttempts_) { // The count starts again; nothing below refuses an identity that passed.
        pinAttempts_->reset(next.identity.pin_attempts);
    }
    commit(live, next, 0);
}

SessionConfig Session::settings() const {
    std::lock_guard live(liveMutex_);
    return config_;
}

void Session::commit(std::unique_lock<std::mutex>& live, const SessionConfig& next, uint32_t changed) {
    if (!running()) { // A finished run reads no settings after reporting its state; start() checks them.
        copyExtensionSettings(config_, next);
        live_ = std::make_shared<const SessionConfig>(config_);
        logSource_->level = config_.log.max_level;
        return;
    }
    if (const uint32_t fixed = changed & fixedExtensions) {
        fail(RCS_ERROR_INVALID_STATE, extensionList(fixed) +
                                          " cannot change while the session runs; stop it first (every other "
                                          "extension can)");
    }
    if (next.iceServers.turn_transport != config_.iceServers.turn_transport) {
        fail(RCS_ERROR_INVALID_STATE, "RCS_EXT_ICE_SERVERS: turn_transport is fixed for a run; servers and "
                                      "credentials (the TURN server's certificate and name too) may change (they "
                                      "apply from the next connection)");
    }
    if (next.forwarder.link != config_.forwarder.link) {
        fail(RCS_ERROR_INVALID_STATE, "RCS_EXT_FORWARDER: link is fixed for a run; the token, the forwarder's key "
                                      "and its WebSocket URL may change (they apply from the next connection)");
    }
    const auto machine = machineForChecks();
    if (auto refusal = checkChange(next, changed, peerOffers(), callbacks_, machine ? &*machine : nullptr)) {
        fail(refusal.result, refusal.reason);
    }
    copyRuntimeSettings(config_, next); // The run reads the other settings without a lock.
    const LiveConfig before = std::exchange(live_, std::make_shared<const SessionConfig>(config_));
    const LiveConfig after = live_;
    logSource_->level = config_.log.max_level;
    if (changed & (1u << RCS_EXT_CONTROL)) {
        applyControl();
    }
    ++settingsVersion_;
    live.unlock(); // The modules hear of it without the lock: they may read the settings.
    if (changed & (1u << RCS_EXT_MODULE)) {
        modules_->update(*before, *after);
    }
    if (changed & (1u << RCS_EXT_NETWORK_TUNING)) {
        holdSystem(state_);
    }
}

// Every viewer's grant follows the host's setting, which wins over what was
// decided for one viewer; viewers that join get it too.
void Session::applyControl() {
    const auto& c = config_.control;
    std::shared_lock viewers(viewers_->mutex);
    std::lock_guard lock(shareControl_.mutex);
    const bool moved = shareControl_.target != c.target || shareControl_.hostTypes != c.host_input_types;
    bool revoked = false;
    for (const auto& viewer : viewers_->list) {
        if (viewer->control != bool(c.allowed) || moved) {
            revoked |= viewer->control && shareControl_.granted(true);
            viewer->control = c.allowed != 0;
            // Input produced under the old grant or for the old executors is discarded.
            ++viewer->epoch;
        }
    }
    if (moved || revoked) {
        shareControl_.release(0);
    }
    shareControl_.allowed = c.allowed != 0;
    shareControl_.target = c.target;
    shareControl_.hostTypes = c.host_input_types;
    shareControl_.systemExecutes = systemInputTypes(c) != 0;
}

PeerOffers Session::peerOffers() const {
    if (config_.role == RCS_ROLE_VIEW) {
        std::lock_guard data(dataMutex_);
        const auto known = [&](uint32_t mask) { return peer_.codecs ? std::vector<uint32_t>{mask} : std::vector<uint32_t>{}; };
        return {peer_.codecs, known(peer_.receive_audio_codecs), known(peer_.send_audio_codecs),
                std::to_array(peer_.codec_limits)};
    }
    // One stream for every viewer; sound for each.
    PeerOffers offers;
    std::vector<protocol::CodecOffer> each;
    {
        std::shared_lock viewers(viewers_->mutex);
        for (const auto& viewer : viewers_->list) {
            each.push_back(viewer->hello().offer);
            offers.receiveAudio.push_back(codecMask(each.back().receiveAudio));
            offers.sendAudio.push_back(codecMask(each.back().sendAudio));
        }
    }
    if (each.empty()) {
        return offers;
    }
    const auto common = commonOffer(each);
    offers.codecs = codecMask(common.codecs);
    for (size_t i = 0; i < common.codecs.size() && i < offers.codecLimits.size(); ++i) {
        offers.codecLimits[i] = {common.codecs[i], common.largest[i].width, common.largest[i].height,
                                 common.formats[i]};
    }
    if (!offers.codecs) {
        offers.codecs = ~0u; // No codec all of them decode: none is refused for that (the stream keeps its own).
    }
    return offers;
}

bool Session::callOnInput(const rcs_input_event& event) {
    if (!callbacks_.on_input) {
        return false;
    }
    std::lock_guard lock(inputCallbackMutex_);
    if (shareControl_.hostRelease.exchange(false)) {
        rcs_input_event release = RCS_INPUT_EVENT_INIT;
        release.type = RCS_INPUT_RELEASE_ALL;
        release.viewer_id = shareControl_.releaseFor;
        callHost(callbacks_.on_input, &release);
    }
    return callHost(callbacks_.on_input, &event) != 0;
}

void Session::deliverHostRelease() {
    if (!shareControl_.hostRelease || !callbacks_.on_input) {
        return;
    }
    std::lock_guard lock(inputCallbackMutex_);
    if (shareControl_.hostRelease.exchange(false)) {
        rcs_input_event release = RCS_INPUT_EVENT_INIT;
        release.type = RCS_INPUT_RELEASE_ALL;
        release.viewer_id = shareControl_.releaseFor;
        callHost(callbacks_.on_input, &release);
    }
}

LiveConfig Session::liveConfig() const {
    std::lock_guard lock(liveMutex_);
    return live_;
}

ReceiveLimits Session::receiveLimits() const {
    std::lock_guard lock(liveMutex_);
    return {config_.videoTuning.queue_frames, config_.videoTuning.max_queue_age_ms};
}

// ---- Events ------------------------------------------------------------------------------

// on_event calls never overlap. An event raised on the thread that is inside
// on_event (the host called the SDK from there) is copied and comes on that
// thread once on_event returned, instead of waiting for itself.
void Session::emit(rcs_event& event) {
    if (!callbacks_.on_event) {
        return;
    }
    event.struct_size = sizeof(event);
    event.timestamp_ns = monotonicNs();
    if (eventThread_.load() == std::this_thread::get_id()) {
        HeldEvent& held = heldEvents_.emplace_back();
        held.event = event;
        if (event.type == RCS_EVENT_MODULE) {
            held.module = event.module.module;
            const auto* bytes = static_cast<const uint8_t*>(event.module.payload);
            held.payload.assign(bytes, bytes + event.module.size);
        } else if (event.type == RCS_EVENT_CURSOR && event.cursor.shape) {
            held.shape = *event.cursor.shape;
            held.pixels.assign(held.shape.pixels, held.shape.pixels + size_t(held.shape.stride_bytes) * held.shape.height);
        }
        return;
    }
    std::lock_guard lock(eventMutex_);
    eventThread_ = std::this_thread::get_id();
    callHost(callbacks_.on_event, &event);
    while (!heldEvents_.empty()) {
        HeldEvent held = std::move(heldEvents_.front());
        heldEvents_.pop_front();
        if (held.event.type == RCS_EVENT_MODULE) {
            held.event.module.module = held.module.c_str();
            held.event.module.payload = held.payload.empty() ? nullptr : held.payload.data();
        } else if (held.event.type == RCS_EVENT_CURSOR && held.event.cursor.shape) {
            held.shape.pixels = held.pixels.data();
            held.event.cursor.shape = &held.shape;
        }
        callHost(callbacks_.on_event, &held.event);
    }
    eventThread_ = std::thread::id();
}

void Session::setState(rcs_state state, rcs_result reason) {
    // Reconnection attempts are part of RECONNECTING, not CONNECTING; the port
    // opening again after "listening again" was reported (with its reason) is no news.
    if ((state == RCS_STATE_CONNECTING && state_ == RCS_STATE_RECONNECTING) ||
        (state == RCS_STATE_LISTENING && state_ == RCS_STATE_LISTENING && reason == RCS_OK)) {
        return;
    }
    state_ = state;
    holdSystem(state);
    log::debug("session",
               std::string(config_.role == RCS_ROLE_SHARE ? "Share" : "View") + " session " + stateName(state));
    rcs_event event{};
    event.type = RCS_EVENT_STATE;
    event.state.value = state;
    event.state.result = state == RCS_STATE_FAILED ? failure().result : reason;
    emit(event);
}

// What the system gives a run: the process as prompt as one in front while
// it runs (platform::stayResponsive), the system's changes of its networks
// seen while it runs over sockets (rcs_paths_config), and Wi-Fi out of power
// saving while connected (rcs_network_tuning_config.wifi_low_latency).
void Session::holdSystem(rcs_state state) {
    std::lock_guard lock(systemMutex_);
    const bool runs = state != RCS_STATE_IDLE && state != RCS_STATE_STOPPED && state != RCS_STATE_FAILED;
    if (!runs) {
        responsive_.reset();
    } else if (!responsive_) {
        responsive_ = platform::stayResponsive();
    }
    if (!runs || usesHostChannel(config_)) {
        networks_.reset();
    } else if (!networks_) {
        try {
            networks_ = platform::watchNetworks();
        } catch (const Failure& failure) {
            log::warning("network", std::string("The system's network changes go unseen (a broken path is still "
                                                "found by its silence): ") +
                                        failure.what());
        }
    }
    if (state != RCS_STATE_RUNNING || !liveConfig()->networkTuning.wifi_low_latency) {
        wifi_.reset();
    } else if (!wifi_) {
        try {
            wifi_ = platform::lowLatencyWifi();
        } catch (const Failure& failure) {
            if (failure.result() != RCS_ERROR_UNSUPPORTED || capabilities().wifi_low_latency) {
                log::warning("network", std::string("Wi-Fi stays in power saving: ") + failure.what());
            }
        }
    }
    updateStats([&](Stats& s) { s.network.wifi_low_latency = wifi_ != nullptr; });
}

void Session::codecChanged(const CodecInfo& info) {
    const bool share = config_.role == RCS_ROLE_SHARE;
    const bool lowLatency = info.lowLatency;
    const std::string& name = info.name;
    rcs_event event{};
    event.type = RCS_EVENT_CODEC;
    event.stream_index = info.stream;
    event.codec.direction = share ? RCS_CODEC_ENCODER : RCS_CODEC_DECODER;
    event.codec.codec = info.codec;
    event.codec.acceleration = info.acceleration;
    event.codec.backend = info.backend;
    event.codec.low_latency = lowLatency;
    event.codec.ref_invalidation = info.refInvalidation;
    event.codec.temporal_layers = share ? info.temporalLayers : 0;
    copyText(event.codec.name, name);
    log::info(share ? "encoder" : "decoder",
              (info.stream ? "Stream " + std::to_string(info.stream) + ": " : std::string()) + codecName(info.codec) +
                  (info.acceleration == RCS_ACCELERATION_HARDWARE ? " hardware " : " software ") +
                  (info.backend == RCS_BACKEND_HOST ? "host " : "") + (share ? "encoder: " : "decoder: ") + name +
                  (share ? (lowLatency ? " (one frame at a time)" : " (pipelined)") : ""));
    updateStats([&](Stats& s) {
        auto& video = s.video[info.stream];
        if (share) {
            video.send.low_latency = lowLatency;
            video.send.temporal_layers = info.temporalLayers;
            copyText(video.send.encoder, name);
        } else {
            copyText(video.receive.decoder, name);
        }
    });
    emit(event);
}

void Session::encodingChanged(uint32_t stream, const rcs_encoding_event& encoding) {
    const std::string which =
        "Stream " + std::to_string(stream) + ", encoding " + std::to_string(encoding.encoding) + ": ";
    if (encoding.result != RCS_OK) {
        log::warning("session", which + (encoding.running ? "stays as it was, its change could not start ("
                                                          : "could not run (") +
                                    rcs_result_message(encoding.result) + "): " + std::string(encoding.message));
    } else if (encoding.running) {
        log::debug("session", which + "started");
    }
    rcs_event event{};
    event.type = RCS_EVENT_ENCODING;
    event.stream_index = stream;
    event.encoding = encoding;
    emit(event);
}

void Session::viewerVideoChanged(rcs_viewer_id viewer, const rcs_viewer_video_event& video) {
    static const char* why[] = {"",       "it joined so",    "its path",          "its lost pictures",
                                "its decoder", "the host's choice", "its encoding ended"};
    log::info("session", "Viewer " + std::to_string(viewer) + " gets encoding " + std::to_string(video.encoding) +
                             ", " + std::to_string(video.layers) +
                             (video.layers == 1 ? " temporal layer" : " temporal layers") +
                             " (" + why[video.reason < std::size(why) ? video.reason : 0] +
                             (video.target_bps ? ", its path carries " + std::to_string(video.target_bps / 1000) +
                                                     " kbit/s)"
                                               : std::string(")")));
    rcs_event event{};
    event.type = RCS_EVENT_VIEWER_VIDEO;
    event.viewer_id = viewer;
    event.viewer_video = video;
    emit(event);
}

void Session::captureChanged(uint32_t stream, rcs_capture_pause paused) {
    if (stream) {
        log::info("capture", "Stream " + std::to_string(stream) + ":");
    }
    switch (paused) {
    case RCS_CAPTURE_PAUSE_NONE:
        log::info("capture", "Capture resumed");
        break;
    case RCS_CAPTURE_PAUSE_DESKTOP:
        log::warning("capture", "Capture paused: the desktop cannot be captured (lock screen, secure desktop or "
                                "display mode change); the viewer keeps the last picture");
        break;
    case RCS_CAPTURE_PAUSE_HIDDEN:
        log::warning("capture", "Capture paused: what is shared does not show (minimized or hidden); the viewer "
                                "keeps the last picture");
        break;
    case RCS_CAPTURE_PAUSE_GONE:
        log::warning("capture", "Capture paused: the shared window was closed or its application quit; the "
                                "viewer keeps the last picture until the host shares something else");
        break;
    default:
        log::warning("capture", "Capture paused: no agent is attached; the viewer keeps the last picture");
        break;
    }
    rcs_event event{};
    event.type = RCS_EVENT_CAPTURE;
    event.stream_index = stream;
    event.capture.paused = paused;
    emit(event);
}

void Session::displaysChanged(uint32_t count) {
    log::info("capture", "Displays changed: " + std::to_string(count) + " now");
    rcs_event event{};
    event.type = RCS_EVENT_DISPLAYS;
    event.displays.count = count;
    emit(event);
}

void Session::controlChanged() {
    rcs_event event{};
    event.type = RCS_EVENT_CONTROL;
    event.control = controlInfo();
    {
        std::lock_guard lock(reportedMutex_);
        if (event.control.allowed == reportedControl_.allowed && event.control.granted == reportedControl_.granted) {
            return; // A new grant epoch alone is no news for the host.
        }
        reportedControl_ = event.control;
    }
    log::info("control", std::string("Remote control ") + (event.control.granted ? "granted" : "not granted"));
    emit(event);
}

void Session::viewerControlChanged(rcs_viewer_id viewer, const rcs_control_info& info) {
    std::string why;
    if (info.allowed && !info.granted) {
        std::lock_guard lock(shareControl_.mutex);
        why = !shareControl_.available ? " (the system does not accept injected input right now)" : "";
    }
    log::info("control", "Remote control " + std::string(info.granted ? "granted" : "not granted") + " to viewer " +
                             std::to_string(viewer) + why);
    rcs_event event{};
    event.type = RCS_EVENT_CONTROL;
    event.viewer_id = viewer;
    event.control = info;
    emit(event);
    modules_->peerControl(viewer, info.granted != 0);
}

void Session::admission(rcs_admission_state state, rcs_viewer_id viewer, uint32_t timeoutMs, rcs_result result,
                        const std::string& message) {
    rcs_event event{};
    event.type = RCS_EVENT_ADMISSION;
    event.viewer_id = viewer;
    event.admission.state = state;
    event.admission.timeout_ms = timeoutMs;
    event.admission.result = result;
    copyText(event.admission.message, message);
    emit(event);
}

void Session::pinFailed(uint32_t failures, uint32_t left) {
    if (cancelled_) {
        return; // The run ends: it cut its handshakes short.
    }
    log::warning("network", "A PIN handshake failed (" + std::to_string(failures) + " so far): " +
                                (left ? std::to_string(left) + " more allowed"
                                      : std::string("PIN handshakes are refused until the PIN is set again")));
    updateStats([](Stats& s) { ++s.network.pin_failures; });
    rcs_event event{};
    event.type = RCS_EVENT_PIN;
    event.pin.failures = failures;
    event.pin.attempts_left = left;
    emit(event);
}

void Session::viewerChanged(const rcs_viewer_info& info) {
    rcs_event event{};
    event.type = RCS_EVENT_VIEWER;
    event.viewer_id = info.viewer_id;
    event.viewer = info;
    emit(event);
}

void Session::rateChanged(const protocol::Connection::PathStatus& status, rcs_viewer_id viewer) {
    rcs_event event{};
    event.type = RCS_EVENT_RATE;
    event.viewer_id = viewer;
    event.rate.bitrate_bps = status.targetBitrate;
    event.rate.loss_permille = status.lossPermille;
    event.rate.received_bps = status.receivedBps;
    event.rate.queue_delay_ms = uint32_t(status.queueDelayUs / 1000);
    event.rate.round_trip_ms = uint32_t(status.roundTripNs / 1'000'000);
    log::info("encoder", "Bitrate " + std::to_string(status.targetBitrate / 1000) + " kbit/s (path delivers " +
                             std::to_string(status.receivedBps / 1000) + " kbit/s, loss " +
                             std::to_string(status.lossPermille) + " per mille, queueing " +
                             std::to_string(status.queueDelayUs / 1000) + " ms)");
    emit(event);
}

void Session::rateReport(const rcs_rate_report& report, rcs_rate_decision& decision) {
    const uint32_t proposal = decision.bitrate_bps;
    if (const auto& decider = config_.rateDecider; decider.on_rate_report) { // Fixed for the run.
        std::lock_guard lock(rateCallbackMutex_);
        callHostWith(decider.user_data, decider.on_rate_report, &report, &decision);
    }
    updateStats(report.viewer_id, [&](Stats& s) {
        ++s.network.rate_reports;
        s.network.rate_overrides += decision.bitrate_bps != proposal;
    });
}

std::optional<protocol::PathStalls> Session::publishPath(const protocol::Connection::PathStatus& status,
                                                         rcs_viewer_id viewer) {
    updateStats(viewer, [&](Stats& s) {
        s.network.round_trip_ns = status.roundTripNs;
        s.network.received_bps = status.receivedBps;
        s.network.loss_permille = status.lossPermille;
        s.network.queue_delay_us = uint32_t(std::min<uint64_t>(status.queueDelayUs, UINT32_MAX));
        s.network.fec_percent = status.fecPercent;
        s.network.fec_sent = status.paritySent;
        s.network.fec_recovered = status.fecRecovered;
        s.network.resend_recovered = status.resendRecovered;
        s.network.frames_lost = status.framesLost;
        s.network.marked_packets = status.markedPackets;
        s.network.path_bps = status.pathBps;
        s.network.sound_wait_us = uint32_t(std::min<uint64_t>(status.soundWaitUs, UINT32_MAX));
        s.network.sound_queue_us = uint32_t(std::min<uint64_t>(status.soundQueueUs, UINT32_MAX));
        s.network.backlog_drops = status.backlogDrops;
        s.network.backlog_dropped_bytes = status.backlogDroppedBytes;
        copyText(s.network.path, status.path);
        s.network.path_lost = status.lost;
        s.network.path_changes = status.pathChanges;
        s.network.last_interruption_ns = status.lastInterruptionNs;
    });
    // Its facts only when it or the networks changed: they take a look at the system's.
    const uint64_t networks = transport::networkGeneration();
    bool changed = false;
    updateStats(viewer, [&](Stats& s) {
        changed = s.pathEnd != status.current || s.standbyEnd != status.standby || !s.path.route ||
                  s.pathNetworks != networks;
    });
    if (changed && status.current.route != RCS_ROUTE_NONE) {
        const rcs_path_info info = describePath(status.current);
        const rcs_path_info standby = status.standby.route ? describePath(status.standby) : rcs_path_info{};
        updateStats(viewer, [&](Stats& s) {
            s.pathEnd = status.current;
            s.standbyEnd = status.standby;
            s.pathNetworks = networks;
            s.path = info;
            s.standbyPath = standby;
            s.network.route = info.route;
            s.network.network = info.network;
            s.network.network_flags = info.network_flags;
            s.network.standby = standby.route != RCS_ROUTE_NONE;
            s.network.standby_network = standby.network;
        });
    }
    // Bytes since the last look, over the networks the paths took then (a new connection counts from 0).
    updateStats(viewer, [&](Stats& s) {
        const uint64_t traffic = status.bytes - status.standbyBytes;
        if (traffic < s.trafficSeen || status.standbyBytes < s.standbySeen) {
            s.trafficSeen = s.standbySeen = 0;
        }
        if (s.path.network_flags & RCS_NETWORK_FLAG_METERED) {
            s.network.metered_bytes += traffic - s.trafficSeen;
        }
        if (s.standbyPath.network_flags & RCS_NETWORK_FLAG_METERED) {
            s.network.metered_bytes += status.standbyBytes - s.standbySeen;
        }
        s.trafficSeen = traffic;
        s.standbySeen = status.standbyBytes;
    });
    // Short stalls: reported when their pace began, changed by a tenth or stopped.
    const auto& stalls = status.stalls;
    std::optional<protocol::PathStalls> said;
    rcs_path_info path{};
    updateStats(viewer, [&](Stats& s) {
        s.network.stalls = stalls.count;
        s.network.stall_ms = stalls.medianMs;
        s.network.longest_stall_ms = stalls.longestMs;
        s.network.stall_period_ms = stalls.periodMs;
        const uint32_t before = s.stallPace, now = stalls.periodMs;
        if (!before != !now || (before && uint64_t(before > now ? before - now : now - before) * 10 >= before)) {
            s.stallPace = now;
            said = protocol::PathStalls{now, stalls.medianMs, stalls.count};
            path = s.path;
        }
    });
    if (said) {
        rcs_event event{};
        event.type = RCS_EVENT_STALLS;
        event.viewer_id = viewer;
        event.stalls = {0, said->periodMs, said->stallMs, said->stalls, path, wifiUnder(path)};
        emit(event);
    }
    return said;
}

void Session::peerStalls(const protocol::PathStalls& stalls, rcs_viewer_id viewer) {
    rcs_path_info path{};
    updateStats(viewer, [&](Stats& s) {
        s.network.peer_stall_period_ms = stalls.periodMs;
        s.network.peer_stall_ms = stalls.stallMs;
        path = s.path;
    });
    rcs_event event{};
    event.type = RCS_EVENT_STALLS;
    event.viewer_id = viewer;
    event.stalls = {1, stalls.periodMs, stalls.stallMs, stalls.stalls, path, wifiUnder(path)};
    emit(event);
}

void Session::pathChanged(const protocol::PathEvent& change, rcs_viewer_id viewer) {
    rcs_event event{};
    event.type = RCS_EVENT_PATH;
    event.viewer_id = viewer;
    auto& path = event.path;
    path.change = change.change;
    path.cause = change.cause;
    path.from = describePath(change.from);
    path.to = change.change == RCS_PATH_CHANGED || change.change == RCS_PATH_STANDBY ? describePath(change.to)
                                                                                     : rcs_path_info{};
    path.interruption_ns = change.interruptionNs;
    path.signaled = change.signaled;
    path.result = change.result;
    copyText(path.message, change.message);
    if (change.change == RCS_PATH_CHANGED) {
        updateStats(viewer, [&](Stats& s) {
            s.pathEnd = change.to;
            s.path = path.to;
            s.network.route = path.to.route;
            s.network.network = path.to.network;
            s.network.network_flags = path.to.network_flags;
        });
    } else if (change.change == RCS_PATH_STANDBY) {
        updateStats(viewer, [&](Stats& s) {
            s.standbyEnd = change.to;
            s.standbyPath = path.to;
            s.network.standby = path.to.route != RCS_ROUTE_NONE;
            s.network.standby_network = path.to.network;
        });
    }
    emit(event);
}

std::pair<rcs_path_info, rcs_path_info> Session::pathInfo(rcs_viewer_id viewer) const {
    if (config_.role == RCS_ROLE_VIEW) {
        if (viewer) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "A viewing session has no viewers: pass 0");
        }
        const rcs_state now = state();
        std::lock_guard lock(dataMutex_);
        if ((now != RCS_STATE_RUNNING && now != RCS_STATE_RECONNECTING) || !stats_.path.route) {
            fail(RCS_ERROR_INVALID_STATE, "No connection: the session is not connected");
        }
        return {stats_.path, stats_.standbyPath};
    }
    std::shared_lock lock(viewers_->mutex);
    const Viewer* found = viewer ? &viewers_->present(viewer) : viewers_->main();
    if (!found) {
        fail(RCS_ERROR_INVALID_STATE, "No viewer is connected");
    }
    const auto stats = found->stats();
    if (!stats.path.route) {
        fail(RCS_ERROR_INVALID_STATE, "The viewer's path is not known yet");
    }
    return {stats.path, stats.standbyPath};
}

void Session::pathLost(Clock::time_point brokeAt) {
    pathLostAt_ = brokeAt;
    setState(RCS_STATE_RECONNECTING, RCS_ERROR_NETWORK);
    viewControl_.granted = false;
    controlChanged(); // The sharer revokes or pauses it meanwhile, and says so once back.
}

void Session::pathBack() {
    pathLostAt_.reset();
    setState(RCS_STATE_RUNNING);
}

NetworkChoice::Choice Session::networkChoice() {
    return networkChoice_.look(*liveConfig(), settingsVersion());
}

uint32_t Session::boundInterface() {
    const auto choice = networkChoice();
    if (choice.lost) {
        fail(RCS_ERROR_NETWORK, choice.why);
    }
    return seams::networksOverridden() ? 0 : choice.index;
}

uint32_t Session::standbyInterface() {
    const auto choice = networkChoice();
    if (!choice.standbyIndex) {
        fail(RCS_ERROR_NETWORK, choice.standbyWhy.empty() ? "Standby is off (rcs_paths_config)" : choice.standbyWhy);
    }
    return seams::networksOverridden() ? 0 : choice.standbyIndex;
}

void Session::cursorChanged(const rcs_cursor_event& cursor) {
    rcs_event event{};
    event.type = RCS_EVENT_CURSOR;
    event.stream_index = cursor.stream;
    event.cursor = cursor;
    emit(event);
}

void Session::keyframeDue(rcs_keyframe_reason reason, rcs_viewer_id viewer) {
    static const char* why[] = {"", "the stream starts", "it was asked for", "the interval passed"};
    log::debug("encoder", std::string("The host's next packet is to be a keyframe: ") + why[reason]);
    rcs_event event{};
    event.type = RCS_EVENT_KEYFRAME;
    event.viewer_id = viewer;
    event.keyframe.reason = reason;
    emit(event);
}

void Session::recovering(uint32_t stream, rcs_recovery_reason reason, rcs_viewer_id viewer,
                         rcs_recovery_method method, uint64_t frozenNs) {
    const bool references = method == RCS_RECOVERY_REFERENCES;
    switch (reason) {
    case RCS_RECOVERY_BACKLOG:
        log::warning("decoder", "Decode backlog over its limit: dropped it, waiting for a keyframe");
        break;
    case RCS_RECOVERY_LOSS:
        if (config_.role == RCS_ROLE_VIEW) {
            log::info("decoder", std::string("A picture was lost on the way; went on after ") +
                                     std::to_string(frozenNs / 1'000'000) + " ms from " +
                                     (references ? "a picture that refers to older ones" : "a keyframe"));
        } else {
            log::info("encoder", "Viewer " + std::to_string(viewer) + " lost a picture; " +
                                     (references ? "the encoder drops it from its references" : "a keyframe follows"));
        }
        break;
    case RCS_RECOVERY_DECODER:
        log::warning("decoder", "Decoder failed; replacing it and restarting from the next keyframe");
        break;
    default:
        log::debug("encoder", "A keyframe was asked for");
        break;
    }
    rcs_event event{};
    event.type = RCS_EVENT_RECOVERY;
    event.viewer_id = viewer;
    event.stream_index = stream;
    event.recovery.reason = reason;
    event.recovery.method = method;
    event.recovery.frozen_ns = frozenNs;
    emit(event);
}

void Session::audioChanged(const platform::AudioStatus& status, rcs_audio_direction direction, rcs_viewer_id viewer,
                           rcs_audio_codec codec) {
    rcs_event event{};
    event.type = RCS_EVENT_AUDIO;
    event.viewer_id = viewer;
    event.audio.direction = direction;
    event.audio.active = status.active;
    event.audio.codec = status.active ? codec : 0;
    event.audio.result = status.result;
    event.audio.voice_processing = status.active && status.voiceProcessing;
    copyText(event.audio.message, status.message);
    updateStats(viewer, [&](Stats& s) {
        if (direction == RCS_AUDIO_SENDING) {
            s.audioSend.interruptions += s.audioSend.active && !status.active;
            s.audioSend.active = status.active;
        } else {
            s.audioReceive.active = status.active;
        }
    });
    const char* way = direction == RCS_AUDIO_SENDING ? "Sending" : "Receiving";
    if (status.active) {
        log::info("audio", std::string(way) + " " + audioCodecName(codec) + " audio" +
                               (viewer ? " (viewer " + std::to_string(viewer) + ")" : std::string()));
    } else {
        log::info("audio", std::string(way) + " no audio: " + status.message);
    }
    emit(event);
}

// ---- Stream, peer and control ------------------------------------------------------------

FailureInfo Session::failure() const {
    std::lock_guard lock(dataMutex_);
    return failure_;
}

namespace {

// The counters of a viewer's connection, added to the run's.
void addCounters(Stats& total, const Stats& viewer) noexcept {
    total.network.rate_reports += viewer.network.rate_reports;
    total.network.rate_overrides += viewer.network.rate_overrides;
    total.network.fec_sent += viewer.network.fec_sent;
    total.network.marked_packets += viewer.network.marked_packets;
    total.network.backlog_drops += viewer.network.backlog_drops;
    total.network.backlog_dropped_bytes += viewer.network.backlog_dropped_bytes;
    total.network.path_changes += viewer.network.path_changes;
    total.network.metered_bytes += viewer.network.metered_bytes;
    total.network.bytes += viewer.network.bytes;
    total.input.received += viewer.input.received;
    total.input.applied += viewer.input.applied;
    total.audioSend.encoded_packets += viewer.audioSend.encoded_packets;
    total.audioSend.encoded_bytes += viewer.audioSend.encoded_bytes;
    total.audioSend.interruptions += viewer.audioSend.interruptions;
    total.audioReceive.received_packets += viewer.audioReceive.received_packets;
    total.audioReceive.lost_packets += viewer.audioReceive.lost_packets;
    total.audioReceive.recovered_packets += viewer.audioReceive.recovered_packets;
    total.data.queued_bytes += viewer.data.queued_bytes;
    total.data.sent_messages += viewer.data.sent_messages;
    total.data.sent_bytes += viewer.data.sent_bytes;
    total.data.received_messages += viewer.data.received_messages;
    total.data.received_bytes += viewer.data.received_bytes;
    if (total.modules.size() < viewer.modules.size()) {
        total.modules.resize(viewer.modules.size(), sizedStruct<rcs_module_stats>());
    }
    for (size_t i = 0; i < viewer.modules.size(); ++i) {
        auto& m = total.modules[i];
        const auto& v = viewer.modules[i];
        m.queued_bytes += v.queued_bytes;
        m.sent_messages += v.sent_messages;
        m.sent_bytes += v.sent_bytes;
        m.received_messages += v.received_messages;
        m.received_bytes += v.received_bytes;
    }
}

// The main viewer's measurements and state, as the run's.
void takeGauges(Stats& total, const Stats& main) noexcept {
    auto& n = total.network;
    std::copy(std::begin(main.network.path), std::end(main.network.path), n.path);
    n.round_trip_ns = main.network.round_trip_ns;
    n.received_bps = main.network.received_bps;
    n.loss_permille = main.network.loss_permille;
    n.queue_delay_us = main.network.queue_delay_us;
    n.path_bps = main.network.path_bps;
    n.fec_percent = main.network.fec_percent;
    n.sound_wait_us = main.network.sound_wait_us;
    n.sound_queue_us = main.network.sound_queue_us;
    n.route = main.network.route;
    n.network = main.network.network;
    n.network_flags = main.network.network_flags;
    n.path_lost = main.network.path_lost;
    n.standby = main.network.standby;
    n.standby_network = main.network.standby_network;
    n.last_interruption_ns = main.network.last_interruption_ns;
    if (main.input.last_result != RCS_OK) { // A remote agent reports results for every viewer together.
        total.input.last_result = main.input.last_result;
    }
    const uint64_t captured = total.audioSend.captured_frames; // The one capture's.
    const auto counters = total.audioSend;
    total.audioSend = main.audioSend;
    total.audioSend.captured_frames = captured;
    total.audioSend.encoded_packets = counters.encoded_packets;
    total.audioSend.encoded_bytes = counters.encoded_bytes;
    total.audioSend.interruptions = counters.interruptions;
}

} // namespace

Stats Session::stats() const {
    const auto channel = hostChannel();
    const auto routes = hostRoutes();
    // The viewers' first: one that leaves meanwhile is added to the run's under this lock.
    std::shared_lock viewers(viewers_->mutex);
    Stats copy;
    {
        std::lock_guard lock(dataMutex_);
        copy = stats_;
    }
    if (config_.role == RCS_ROLE_VIEW) {
        copy.data.queued_bytes = dataQueue_.bytes();
        modules_->addQueued(copy.modules, dataQueue_);
    }
    for (const auto& viewer : viewers_->list) {
        addCounters(copy, viewer->stats());
    }
    if (const Viewer* main = viewers_->main()) {
        takeGauges(copy, main->stats());
    }
    if (channel) {
        copy.network.host_refused = channel->refused();
    } else if (routes) {
        copy.network.host_refused = routes->refused();
    }
    return copy;
}

void Session::viewerLeft(rcs_viewer_id viewer, const Stats& stats, bool main) {
    std::lock_guard lock(dataMutex_);
    Stats counted = stats;
    counted.data.queued_bytes = 0; // A level, not a count: what it had queued went with it.
    addCounters(stats_, counted);
    if (main) {
        takeGauges(stats_, stats);
    }
    if (departed_.size() == departedKept) {
        departed_.pop_front();
    }
    departed_.emplace_back(viewer, counted);
}

rcs_module_stats Session::moduleStats(rcs_viewer_id viewer, int module) const {
    const Stats all = viewer ? viewerStats(viewer) : stats();
    return size_t(module) < all.modules.size() ? all.modules[size_t(module)] : sizedStruct<rcs_module_stats>();
}

Stats Session::viewerStats(rcs_viewer_id viewer) const {
    std::shared_lock viewers(viewers_->mutex);
    if (const Viewer* present = viewers_->find(viewer)) {
        return present->stats();
    }
    {
        std::lock_guard lock(dataMutex_);
        for (const auto& [id, stats] : departed_) {
            if (id == viewer) {
                return stats; // Its final statistics.
            }
        }
    }
    return viewers_->present(viewer).stats(); // Refused, saying why.
}

std::vector<rcs_viewer_info> Session::viewers() const {
    std::shared_lock viewers(viewers_->mutex);
    std::lock_guard lock(shareControl_.mutex);
    const Viewer* main = viewers_->main();
    std::vector<rcs_viewer_info> list;
    for (const auto& viewer : viewers_->list) {
        auto info = viewer->info(viewer.get() == main);
        info.control.allowed = viewer->control;
        info.control.granted = shareControl_.granted(viewer->control);
        list.push_back(info);
    }
    return list;
}

void Session::disconnectViewer(rcs_viewer_id viewer) {
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "A viewing session has no viewers");
    }
    std::shared_lock viewers(viewers_->mutex);
    viewers_->present(viewer).leave(RCS_ERROR_DISCONNECTED, "The host disconnected the viewer");
}

void Session::setViewerControl(rcs_viewer_id viewer, bool allowed) {
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "A viewing session has no viewers");
    }
    if (allowed) { // Refused where the host's setting would be.
        std::lock_guard live(liveMutex_);
        SessionConfig next = config_;
        next.control.allowed = 1;
        const auto machine = machineForChecks();
        if (auto refusal = checkChange(next, 1u << RCS_EXT_CONTROL, {}, callbacks_, machine ? &*machine : nullptr)) {
            fail(refusal.result, refusal.reason);
        }
    }
    std::shared_lock viewers(viewers_->mutex);
    Viewer& found = viewers_->present(viewer);
    std::lock_guard lock(shareControl_.mutex);
    if (found.control == allowed) {
        return;
    }
    if (found.control && shareControl_.granted(true)) {
        shareControl_.release(viewer); // What it holds; what others hold goes too.
    }
    found.control = allowed;
    ++found.epoch;
}

void Session::admitViewer(rcs_viewer_id viewer, const rcs_viewer_decision& decision) {
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "A viewing session has no viewers");
    }
    {
        std::unique_lock viewers(viewers_->mutex);
        if (!viewer || viewer >= viewers_->nextId) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "Viewer " + std::to_string(viewer) + " was never given in this run");
        }
        const auto waiting = viewers_->waiting.find(viewer);
        if (waiting == viewers_->waiting.end() || waiting->second) {
            fail(RCS_ERROR_INVALID_STATE, "Viewer " + std::to_string(viewer) + " does not wait for a decision");
        }
        waiting->second = decision;
    }
    viewers_->decided.notify_all();
}

void Session::setViewerVideo(rcs_viewer_id viewer, const rcs_viewer_video& video) {
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "A viewing session has no viewers");
    }
    if (video.encoding != RCS_ENCODING_ANY) {
        const auto live = liveConfig();
        const uint32_t count = live->encodings.mode == RCS_ENCODINGS_SINGLE ? 1 : live->encodings.count;
        if (video.encoding >= count) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "encoding " + std::to_string(video.encoding) +
                                                 " is beyond the encodings a stream may have (RCS_EXT_ENCODINGS: " +
                                                 std::to_string(count) + ")");
        }
    }
    std::shared_lock viewers(viewers_->mutex);
    Viewer& found = viewers_->present(viewer);
    found.pinned = video.encoding;
    found.pinnedLayers = video.layers;
    found.counts = video.counts_for_bitrate != 0;
}

void Session::setMainViewer(rcs_viewer_id viewer) {
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "A viewing session has no viewers");
    }
    {
        std::unique_lock viewers(viewers_->mutex);
        if (viewer) {
            viewers_->present(viewer);
        }
        if (viewers_->chosenMain == viewer) {
            return;
        }
        viewers_->chosenMain = viewer;
    }
    ++viewers_->version;
    mainViewerChanged();
}

void Session::mainViewerChanged() {
    std::shared_lock viewers(viewers_->mutex);
    const Viewer* main = viewers_->main();
    if (!main) {
        return;
    }
    const auto info = main->info(true);
    const auto [codec, bitrate] = main->audioStream();
    std::lock_guard lock(dataMutex_);
    peer_ = info.peer;
    streams_[0].audio_codec = codec;
    streams_[0].audio_bitrate_bps = bitrate;
}

rcs_stream_info Session::streamInfo(uint32_t stream) const {
    std::lock_guard lock(dataMutex_);
    rcs_stream_info info = streams_.at(stream);
    info.audio_codec = streams_[0].audio_codec;
    info.audio_bitrate_bps = streams_[0].audio_bitrate_bps;
    return info;
}

rcs_peer_info Session::peerInfo() const {
    std::lock_guard lock(dataMutex_);
    return peer_;
}

void Session::clearStreams() noexcept {
    for (uint32_t stream = 0; stream < streams_.size(); ++stream) {
        streams_[stream] = RCS_STREAM_INFO_INIT;
        streams_[stream].stream = stream;
    }
}

rcs_event Session::streamEvent(uint32_t stream, uint32_t changes) const {
    rcs_event event{};
    event.type = RCS_EVENT_STREAM;
    event.stream_index = stream;
    event.stream.changes = changes;
    event.stream.info = streams_[stream];
    event.stream.info.audio_codec = streams_[0].audio_codec;
    event.stream.info.audio_bitrate_bps = streams_[0].audio_bitrate_bps;
    return event;
}

void Session::setVideoStream(const rcs_stream_info& info) {
    std::unique_lock lock(dataMutex_);
    const rcs_stream_info& o = streams_.at(info.stream);
    const bool first = !o.codec;
    const bool firstOfRun =
        std::none_of(streams_.begin(), streams_.end(), [](const rcs_stream_info& s) { return s.codec != 0; });
    uint32_t changes = 0;
    changes |=
        (first || info.codec != o.codec || info.encoder_acceleration != o.encoder_acceleration) ? RCS_STREAM_CODEC : 0;
    changes |= (first || info.format != o.format || !sameColor(info.color, o.color) || !sameHdr(info.hdr, o.hdr))
                   ? RCS_STREAM_FORMAT
                   : 0;
    changes |= (first || info.width != o.width || info.height != o.height) ? RCS_STREAM_SIZE : 0;
    changes |= (first || info.frames_per_second != o.frames_per_second) ? RCS_STREAM_FRAME_RATE : 0;
    changes |= (first || info.bitrate_bps != o.bitrate_bps) ? RCS_STREAM_BITRATE : 0;
    changes |= (first || info.content_x != o.content_x || info.content_y != o.content_y ||
                info.content_width != o.content_width || info.content_height != o.content_height)
                   ? RCS_STREAM_CONTENT
                   : 0;
    changes |= (first || info.display_width != o.display_width || info.display_height != o.display_height ||
                info.display_refresh_hz != o.display_refresh_hz || info.desktop_x != o.desktop_x ||
                info.desktop_y != o.desktop_y || info.desktop_width != o.desktop_width ||
                info.desktop_height != o.desktop_height)
                   ? RCS_STREAM_DISPLAY
                   : 0;
    changes |= (first || info.cursor != o.cursor) ? RCS_STREAM_CURSOR : 0;
    changes |= (first || info.encoding != o.encoding || info.encodings != o.encodings || info.layers != o.layers ||
                info.received_layers != o.received_layers)
                   ? RCS_STREAM_ENCODING
                   : 0;
    auto& stored = streams_[info.stream];
    const rcs_audio_codec audioCodec = stored.audio_codec;
    const uint32_t audioBitrate = stored.audio_bitrate_bps;
    stored = info;
    stored.struct_size = sizeof(stored);
    stored.audio_codec = audioCodec;
    stored.audio_bitrate_bps = audioBitrate;
    if (changes) {
        auto event = streamEvent(info.stream, firstOfRun ? changes | RCS_STREAM_AUDIO : changes);
        lock.unlock(); // The host's callback may call getters.
        emit(event);
    }
}

void Session::videoStreamEnded(uint32_t stream) {
    std::unique_lock lock(dataMutex_);
    auto& stored = streams_.at(stream);
    if (!stored.codec) {
        return; // Never announced, or ended already.
    }
    const rcs_audio_codec audioCodec = stored.audio_codec;
    const uint32_t audioBitrate = stored.audio_bitrate_bps;
    stored = RCS_STREAM_INFO_INIT;
    stored.stream = stream;
    stored.audio_codec = audioCodec;
    stored.audio_bitrate_bps = audioBitrate;
    auto event = streamEvent(stream, RCS_STREAM_ENDED);
    lock.unlock();
    emit(event);
}

void Session::setAudioStream(rcs_audio_codec codec, uint32_t bitrate, rcs_viewer_id viewer) {
    bool main = true; // The view side's sound, or the main viewer's: the stream's.
    if (viewer) {
        std::shared_lock viewers(viewers_->mutex);
        Viewer* found = viewers_->find(viewer);
        if (!found) {
            return;
        }
        if (found->audioStream() == std::pair(codec, bitrate)) {
            return;
        }
        found->setAudio(codec, bitrate);
        main = found == viewers_->main();
    }
    std::unique_lock lock(dataMutex_);
    if (!viewer) {
        stats_.audioReceive.codec = codec;
    }
    const bool pictures =
        std::any_of(streams_.begin(), streams_.end(), [](const rcs_stream_info& info) { return info.codec != 0; });
    if (!pictures) { // Before the first picture: reported with it.
        if (main) {
            streams_[0].audio_codec = codec;
            streams_[0].audio_bitrate_bps = bitrate;
        }
        return;
    }
    if (main && codec == streams_[0].audio_codec && bitrate == streams_[0].audio_bitrate_bps) {
        return;
    }
    if (main) {
        streams_[0].audio_codec = codec;
        streams_[0].audio_bitrate_bps = bitrate;
    }
    auto event = streamEvent(0, RCS_STREAM_AUDIO);
    event.viewer_id = viewer;
    event.stream.info.audio_codec = codec;
    event.stream.info.audio_bitrate_bps = bitrate;
    lock.unlock();
    emit(event);
}

void Session::setPeer(const rcs_peer_info& peer, rcs_viewer_id viewer) {
    rcs_event event{};
    event.type = RCS_EVENT_PEER;
    event.viewer_id = viewer;
    event.peer = peer;
    event.peer.struct_size = sizeof(event.peer);
    bool main = true;
    if (viewer) {
        std::shared_lock viewers(viewers_->mutex);
        const Viewer* found = viewers_->find(viewer);
        main = found && found == viewers_->main();
    }
    if (main) {
        std::lock_guard lock(dataMutex_);
        peer_ = event.peer;
    }
    emit(event);
}

void Session::moduleEvent(rcs_viewer_id viewer, const rcs_module_event& module) {
    rcs_event event{};
    event.type = RCS_EVENT_MODULE;
    event.viewer_id = viewer;
    event.module = module;
    emit(event);
}

void Session::setRefusedPeerKey(const crypto::PublicKey& key) {
    std::lock_guard lock(dataMutex_);
    peer_.has_public_key = 1;
    std::copy(key.begin(), key.end(), peer_.public_key);
}

// ---- The agent ---------------------------------------------------------------------------

rcs_agent_info Session::agentInfoOf(const agent::Machine& machine, bool attached, rcs_result result,
                                    const std::string& message) const {
    rcs_agent_info info = agent::agentInfo(machine, attached, result, message);
    info.input_types |= liveConfig()->control.host_input_types;
    return info;
}

rcs_agent_info Session::agentInfo() const {
    if (!hasRemoteAgent(*liveConfig())) {
        return agentInfoOf(agent::localMachine(), true); // Probed outside the locks.
    }
    std::optional<agent::Machine> machine;
    FailureInfo left;
    bool attached;
    {
        std::lock_guard lock(dataMutex_);
        machine = agentMachine_;
        left = agentLeft_;
        attached = agentAttached_;
    }
    return machine ? agentInfoOf(*machine, attached, left.result, left.message) : sizedStruct<rcs_agent_info>();
}

std::vector<rcs_display_info> Session::displays() const {
    if (!hasRemoteAgent(*liveConfig())) {
        return agent::localMachine().displays;
    }
    std::lock_guard lock(dataMutex_);
    return agentMachine_ ? agentMachine_->displays : std::vector<rcs_display_info>{};
}

std::vector<rcs_window_info> Session::windows() {
    if (!hasRemoteAgent(*liveConfig())) {
        return seams::windows();
    }
    std::lock_guard lock(agentMutex_);
    if (!shareAgent_) {
        fail(RCS_ERROR_INVALID_STATE, "No agent is attached to list its windows; the session is not running");
    }
    return shareAgent_->port().windows();
}

std::optional<agent::Machine> Session::machineForChecks() const {
    if (config_.role != RCS_ROLE_SHARE) {
        return std::nullopt;
    }
    if (!hasRemoteAgent(config_)) {
        return ownMachine(config_);
    }
    std::lock_guard lock(dataMutex_);
    return agentAttached_ ? agentMachine_ : std::nullopt; // Before an agent attached nothing is known.
}

void Session::agentAttached(const agent::Machine& machine) {
    rcs_event event{};
    event.type = RCS_EVENT_AGENT;
    event.agent = agentInfoOf(machine, true);
    {
        std::lock_guard lock(dataMutex_);
        agentMachine_ = machine;
        agentAttached_ = true;
        agentLeft_ = {};
    }
    log::info("agent", "Agent attached: process " + std::to_string(machine.process.processId) + " (" +
                           machine.process.account + "), " + std::to_string(machine.displays.size()) + " display(s)");
    emit(event);
}

void Session::agentDetached(rcs_result result, const std::string& message) {
    rcs_event event{};
    event.type = RCS_EVENT_AGENT;
    std::optional<agent::Machine> machine;
    {
        std::lock_guard lock(dataMutex_);
        agentAttached_ = false;
        agentLeft_ = {result, message};
        machine = agentMachine_;
    }
    event.agent = machine ? agentInfoOf(*machine, false, result, message) : sizedStruct<rcs_agent_info>();
    emit(event);
}

rcs_desktop_info Session::desktopInfo() const {
    std::lock_guard lock(dataMutex_);
    return desktop_;
}

void Session::desktopChanged(const platform::DesktopState& state) {
    rcs_event event{};
    event.type = RCS_EVENT_DESKTOP;
    {
        std::lock_guard lock(dataMutex_);
        if (desktop_.kind == state.kind && desktop_.user_logged_on == (state.userLoggedOn ? 1u : 0u)) {
            return;
        }
        desktop_.kind = state.kind;
        desktop_.user_logged_on = state.userLoggedOn ? 1u : 0u;
        event.desktop = desktop_;
    }
    log::info("capture",
              std::string("The machine shows ") +
                  (state.kind == RCS_DESKTOP_USER     ? "the user's desktop"
                   : state.kind == RCS_DESKTOP_SECURE ? (state.userLoggedOn ? "a secure desktop (lock screen or prompt)"
                                                                            : "a secure desktop (sign-in screen)")
                                                      : "an unknown desktop"));
    emit(event);
}

void Session::agentDisplays(const std::vector<rcs_display_info>& list) {
    {
        std::lock_guard lock(dataMutex_);
        if (agentMachine_) {
            agentMachine_->displays = list;
        }
    }
    displaysChanged(uint32_t(list.size()));
}

void Session::checkMachine(const agent::Machine& machine) {
    std::lock_guard live(liveMutex_);
    if (auto refusal = checkChange(config_, ~fixedExtensions, peerOffers(), callbacks_, &machine)) {
        fail(refusal.result, refusal.reason);
    }
}

rcs_control_info Session::controlInfo() const {
    rcs_control_info info = RCS_CONTROL_INFO_INIT;
    if (config_.role == RCS_ROLE_SHARE) {
        std::shared_lock viewers(viewers_->mutex);
        const Viewer* main = viewers_->main();
        std::lock_guard lock(shareControl_.mutex);
        info.allowed = main ? main->control : shareControl_.allowed;
        info.granted = main && shareControl_.granted(main->control);
    } else {
        info.granted = viewControl_.granted;
    }
    return info;
}

// ---- Actions -----------------------------------------------------------------------------

void Session::addRestart(PathRestart& restart) {
    std::lock_guard lock(signalMutex_);
    restarts_.push_back(&restart);
}

void Session::removeRestart(PathRestart& restart) {
    std::lock_guard lock(signalMutex_);
    std::erase(restarts_, &restart);
}

void Session::signal(const std::string& text, uint64_t route) {
    if (!usesIce(config_.connect.transport)) {
        fail(RCS_ERROR_UNSUPPORTED, "Signals are exchanged by P2P and RELAY sessions only");
    }
    if (const auto kind = transport::ice::signalKind(text); kind != transport::ice::SignalKind::connect) {
        // About a running connection: its restart takes it. One this end has
        // no connection of is answered so: the peer connects anew.
        const auto tag = transport::ice::signalTag(text);
        {
            std::lock_guard lock(signalMutex_);
            const auto found = std::find_if(restarts_.begin(), restarts_.end(),
                                            [&](const PathRestart* restart) { return restart->tag() == tag; });
            if (found != restarts_.end()) {
                if (kind == transport::ice::SignalKind::unknown) {
                    (*found)->unknown();
                    return;
                }
                const auto restart = transport::ice::decodeRestart(text, (*found)->key());
                if (restart.description.role == config_.role) {
                    fail(RCS_ERROR_INVALID_ARGUMENT, "The signal comes from the same role as this session; pass the "
                                                     "peer's");
                }
                (*found)->signal(restart);
                return;
            }
        }
        if (kind == transport::ice::SignalKind::restart) {
            log::info("network", "A restart signal for a connection this end does not have; the peer hears so");
            emitSignal(transport::ice::encodeUnknown(tag), route);
        }
        return;
    }
    auto description = transport::ice::decode(text);
    if (description.role == config_.role) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The signal comes from the same role as this session; pass the peer's");
    }
    std::lock_guard lock(signalMutex_);
    if (config_.role == RCS_ROLE_VIEW) {
        answers_.add(std::move(description));
        signalArrived_.notify_all();
    } else if (iceListener_) {
        iceListener_->signal({std::move(description), route});
    } else {
        earlySignals_.push_back({std::move(description), route});
        if (earlySignals_.size() > maxEarlySignals) {
            earlySignals_.pop_front();
        }
    }
}

std::shared_ptr<transport::HostChannel> Session::hostChannel() const {
    std::lock_guard lock(hostMutex_);
    return hostChannel_;
}

std::shared_ptr<protocol::RouteDemux> Session::hostRoutes() const {
    std::lock_guard lock(hostMutex_);
    return hostRoutes_;
}

namespace {

// HOST, or FORWARDER's link HOST: the run's channel, or a failure naming why there is none.
template <class Channel>
std::shared_ptr<Channel> runningChannel(const SessionConfig& config, std::shared_ptr<Channel> channel) {
    if (!usesHostChannel(config)) {
        fail(RCS_ERROR_UNSUPPORTED, "Only a session of transport HOST, or FORWARDER with link RCS_FORWARDER_HOST, runs "
                                    "over the host's channel");
    }
    if (!channel) {
        fail(RCS_ERROR_INVALID_STATE, "The session does not run; start it first");
    }
    return channel;
}

// A viewer's channel leads to the sharer alone, a forwarder's hop's to the forwarder.
void checkOneRoute(uint64_t route) {
    if (route) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "This end's channel has route 0 only (a viewer's leads to the sharer alone, "
                                         "a forwarder's hop to the forwarder)");
    }
}

} // namespace

void Session::transportReceive(uint64_t route, std::span<const uint8_t> bytes) {
    if (routedChannel()) {
        runningChannel(config_, hostRoutes())->receive(route, bytes);
        return;
    }
    const auto channel = runningChannel(config_, hostChannel());
    checkOneRoute(route);
    channel->receive(bytes);
}

void Session::transportClosed(uint64_t route) {
    if (routedChannel()) {
        runningChannel(config_, hostRoutes())->closed(route);
        return;
    }
    const auto channel = runningChannel(config_, hostChannel());
    checkOneRoute(route);
    channel->closed();
}

void Session::transportRestarted(uint64_t route) {
    if (routedChannel()) {
        runningChannel(config_, hostRoutes())->restarted(route);
        return;
    }
    const auto channel = runningChannel(config_, hostChannel());
    checkOneRoute(route);
    channel->restarted();
}

void Session::emitSignal(const std::string& text, uint64_t route) {
    log::debug("network", "Signal for the peer (route " + std::to_string(route) + "): " +
                              std::to_string(text.size()) + " characters");
    std::lock_guard lock(signalCallbackMutex_);
    callHost(callbacks_.on_signal, text.c_str(), route);
}

transport::ice::Description Session::waitForAnswer(const std::string& ufrag) {
    std::unique_lock lock(signalMutex_);
    for (;;) {
        if (auto answer = answers_.take(ufrag)) {
            return std::move(*answer);
        }
        throwIfCancelled(cancelled_);
        if (signalDeadline_ && Clock::now() >= *signalDeadline_) {
            fail(RCS_ERROR_NETWORK, "The sharer's answer did not arrive within the reconnect window");
        }
        signalArrived_.wait_for(lock, 20ms);
    }
}

std::optional<transport::ice::Description> Session::takeAnswer(const std::string& ufrag) {
    std::lock_guard lock(signalMutex_);
    return answers_.take(ufrag);
}

rcs_result Session::sendInput(const rcs_input_event& event) {
    if (!protocol::validInput(event)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "Not a valid input event (type, button, key or wheel out of range)");
    }
    if (event.viewer_id) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "viewer_id is the sharer's (on_input); send input with 0");
    }
    if (config_.role != RCS_ROLE_VIEW) {
        fail(RCS_ERROR_UNSUPPORTED, "Input is sent by a viewing session");
    }
    if (state_ != RCS_STATE_RUNNING) {
        fail(RCS_ERROR_INVALID_STATE, "Input is sent while the session runs");
    }
    if (!viewControl_.granted) {
        fail(RCS_ERROR_INVALID_STATE, "The sharer has not granted control (RCS_EVENT_CONTROL says when it does)");
    }
    if (const rcs_result queued = viewControl_.inputs.push(event, viewControl_.epoch); queued != RCS_OK) {
        fail(queued, "The input queue is full: what waited became a release-all; stop capturing input meanwhile");
    }
    return RCS_OK;
}

void Session::pause(uint32_t what, bool paused) {
    if (paused) {
        pausedByHost_ |= what;
    } else {
        pausedByHost_ &= ~what;
    }
    pauseChanged();
}

void Session::peerPaused(uint32_t what) {
    pausedByPeer_ = what;
    pauseChanged();
}

void Session::pauseChanged(bool starting) {
    std::lock_guard lock(pauseMutex_);
    const uint32_t host = pausedByHost_.load(), peer = pausedByPeer_.load();
    const uint32_t background = background_ ? liveConfig()->background.pause : 0;
    const rcs_pause_event now{host | background | peer, host, background, peer};
    const rcs_pause_event& was = pauseReported_;
    const bool changed = now.paused != was.paused || now.by_host != was.by_host ||
                         now.by_background != was.by_background || now.by_peer != was.by_peer;
    if (changed) {
        pauseReported_ = now;
        if (paused_.exchange(host | background) != (host | background)) {
            ++settingsVersion_; // The run's threads pause or resume, and a viewer's tell its sharer.
        }
        log::info("session", "Paused: " + pauseText(now.paused) + " (the host: " + pauseText(host) +
                                 ", the background: " + pauseText(background) +
                                 (config_.role == RCS_ROLE_VIEW ? ", the sharer: " + pauseText(peer) : "") + ")");
    }
    if (starting ? now.paused != 0 : changed && running()) {
        rcs_event event{};
        event.type = RCS_EVENT_PAUSE;
        event.pause = now;
        emit(event);
    }
}

rcs_result Session::requestKeyframe() {
    if (state_ != RCS_STATE_RUNNING) {
        fail(RCS_ERROR_INVALID_STATE, "A keyframe is asked for while the session runs");
    }
    keyframeRequested_ = true;
    viewControl_.inputs.wake(); // The viewer's sender thread asks the sharer.
    return RCS_OK;
}

void Session::pushVideoFrame(const rcs_frame& frame) {
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "Frames are pushed by a sharing session");
    }
    if (liveConfig()->capture.source != RCS_CAPTURE_HOST) {
        fail(RCS_ERROR_INVALID_STATE, "Frames are pushed with capture source RCS_CAPTURE_HOST");
    }
    if (frame.stream) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The host's pictures are stream 0's: stream must be 0");
    }
    hostVideo_.push(frame);
}

void Session::pushGpuFrame(const rcs_gpu_frame_info& frame) {
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "Frames are pushed by a sharing session");
    }
    if (liveConfig()->capture.source != RCS_CAPTURE_HOST) {
        fail(RCS_ERROR_INVALID_STATE, "Frames are pushed with capture source RCS_CAPTURE_HOST");
    }
    hostVideo_.push(frame);
}

void Session::pushSampleBuffer(const void* sampleBuffer, const rcs_sample_buffer_info& info) {
    if (platform::hostImages() != platform::HostImages::sampleBuffers) {
        fail(RCS_ERROR_UNSUPPORTED, platform::hostImages() == platform::HostImages::hardwareBuffers
                                        ? "Android has no Core Media sample buffers: push AHardwareBuffers with "
                                          "rcs_session_push_hardware_buffer"
                                        : "This system has no Core Media sample buffers: push pictures with "
                                          "rcs_session_push_video_frame");
    }
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "Sample buffers are pushed by a sharing session");
    }
    if (info.type == RCS_SAMPLE_BUFFER_VIDEO) {
        hostVideo_.prepareSamples();
        if (liveConfig()->capture.source != RCS_CAPTURE_HOST) {
            fail(RCS_ERROR_INVALID_STATE, "Pictures are pushed with capture source RCS_CAPTURE_HOST");
        }
        hostVideo_.pushSample(sampleBuffer, info.orientation);
    } else {
        hostAudio_.prepareSamples();
        const auto live = liveConfig();
        if (live->sendAudio.source != RCS_AUDIO_HOST) {
            fail(RCS_ERROR_INVALID_STATE, "Sound is pushed with send audio source RCS_AUDIO_HOST");
        }
        hostAudio_.pushSample(sampleBuffer, info.type, live->sendAudio);
    }
}

void Session::pushHardwareBuffer(const void* buffer, uint32_t rotation) {
    if (platform::hostImages() != platform::HostImages::hardwareBuffers) {
        fail(RCS_ERROR_UNSUPPORTED, "Only Android hands over AHardwareBuffers (rcs_session_push_hardware_buffer)");
    }
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "Hardware buffers are pushed by a sharing session");
    }
    hostVideo_.prepareSamples();
    if (liveConfig()->capture.source != RCS_CAPTURE_HOST) {
        fail(RCS_ERROR_INVALID_STATE, "Pictures are pushed with capture source RCS_CAPTURE_HOST");
    }
    hostVideo_.pushSample(buffer, rotation);
}

// The host's own bitstream (capture source ENCODED).
void Session::pushEncodedVideo(const rcs_encoded_packet& packet) {
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "Encoded video is pushed by a sharing session");
    }
    if (liveConfig()->capture.source != RCS_CAPTURE_ENCODED) {
        fail(RCS_ERROR_INVALID_STATE, "Encoded video is pushed with capture source RCS_CAPTURE_ENCODED");
    }
    if (packet.stream || packet.encoding) {
        fail(RCS_ERROR_INVALID_ARGUMENT,
             "The host's bitstream is stream 0's, of one encoding: stream and encoding must be 0");
    }
    encodedVideo_.push(packet);
}

rcs_result Session::videoReady() {
    if (config_.role != RCS_ROLE_SHARE) {
        fail(RCS_ERROR_UNSUPPORTED, "A viewing session sends no video");
    }
    if (liveConfig()->capture.source != RCS_CAPTURE_ENCODED) {
        fail(RCS_ERROR_INVALID_STATE, "rcs_session_video_ready is for capture source RCS_CAPTURE_ENCODED");
    }
    return encodedVideo_.ready();
}

void Session::pushAudio(const rcs_audio_frame& frame) {
    if (liveConfig()->sendAudio.source != RCS_AUDIO_HOST) {
        fail(RCS_ERROR_INVALID_STATE, "Sound is pushed with audio source RCS_AUDIO_HOST");
    }
    hostAudio_.push(frame);
}

void Session::sendData(const rcs_data_message& message) {
    if (!message.data || !message.size || message.size > RCS_DATA_MAX_MESSAGE) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "A data message holds 1..16 MiB");
    }
    if (message.flags & ~uint32_t(RCS_DATA_UNRELIABLE)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "Unknown data flags");
    }
    if ((message.flags & RCS_DATA_UNRELIABLE) && message.size > RCS_DATA_MAX_DATAGRAM) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "An unreliable data message holds at most RCS_DATA_MAX_DATAGRAM bytes");
    }
    if (config_.role == RCS_ROLE_VIEW && message.viewer_id) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "viewer_id names one of a sharer's viewers; a viewing session sends with 0");
    }
    if (state_ != RCS_STATE_RUNNING) {
        fail(RCS_ERROR_INVALID_STATE, "Data is sent while the session runs");
    }
    const size_t limit = liveConfig()->networkTuning.send_queue_bytes;
    const auto item = [&] {
        DataQueue::Item item;
        item.channel = message.channel;
        item.unreliable = message.flags & RCS_DATA_UNRELIABLE;
        item.bytes.assign(message.data, message.data + message.size);
        return item;
    };
    // What a queue said: queued, or why not.
    const auto check = [&](DataQueue::Push pushed) {
        if (pushed == DataQueue::Push::full) {
            fail(RCS_ERROR_QUEUE_FULL, "The data send queue (" + std::to_string(limit / (1u << 20)) +
                                           " MiB) cannot take the message; wait for RCS_EVENT_DATA");
        }
        if (pushed == DataQueue::Push::closed) {
            fail(RCS_ERROR_INVALID_STATE, "No connection takes data now (the peer left; the run is listening or "
                                          "reconnecting)");
        }
    };
    if (config_.role == RCS_ROLE_VIEW) {
        check(dataQueue_.push(item(), limit));
        return;
    }
    std::shared_lock viewers(viewers_->mutex);
    if (message.viewer_id) {
        check(viewers_->present(message.viewer_id).data.push(item(), limit));
        return;
    }
    // Every viewer, or none: one message at a time, so that the room found is
    // there to take it (the queues' senders only take from them).
    std::lock_guard sending(dataSendMutex_);
    std::vector<DataQueue*> queues;
    for (const auto& viewer : viewers_->list) {
        if (viewer->leaving()) {
            continue;
        }
        const auto room = viewer->data.room(message.size, limit);
        if (room == DataQueue::Push::queued) {
            queues.push_back(&viewer->data);
        } else if (room == DataQueue::Push::full) {
            check(room);
        }
    }
    if (queues.empty()) {
        check(DataQueue::Push::closed);
    }
    for (auto* queue : queues) {
        queue->push(item(), limit);
    }
}

void Session::deliverData(uint32_t channel, rcs_data_flags flags, const crypto::Bytes& bytes, rcs_viewer_id viewer) {
    updateStats(viewer, [&](Stats& s) {
        ++s.data.received_messages;
        s.data.received_bytes += bytes.size();
    });
    if (!callbacks_.on_data) {
        return;
    }
    rcs_data_message message = RCS_DATA_MESSAGE_INIT;
    message.viewer_id = viewer;
    message.channel = channel;
    message.flags = flags;
    message.size = uint32_t(bytes.size());
    message.data = bytes.data();
    std::lock_guard lock(dataCallbackMutex_);
    callHost(callbacks_.on_data, &message);
}

void Session::dataRoom(uint64_t queuedBytes, rcs_viewer_id viewer) {
    rcs_event event{};
    event.type = RCS_EVENT_DATA;
    event.viewer_id = viewer;
    event.data.queued_bytes = queuedBytes;
    emit(event);
}

// ---- Frames ------------------------------------------------------------------------------

void Session::countDelivered(uint32_t stream) {
    updateStats([&](Stats& s) {
        auto& video = s.video[stream];
        ++(config_.role == RCS_ROLE_SHARE ? video.send.preview_frames : video.receive.delivered_frames);
    });
}

void Session::deliver(const VideoFrame& frame) {
    if (callbacks_.on_gpu_frame) {
        // start() refuses configurations that cannot produce native surfaces.
        auto* handle = new rcs_gpu_frame_impl;
        handle->frame = frame;
        handle->info.struct_size = sizeof(handle->info);
        if (!platform::describeGpuFrame(frame, handle->info)) {
            rcs_gpu_frame_release(handle);
            fail(RCS_ERROR_INTERNAL, "A frame without a native surface reached on_gpu_frame");
        }
        countDelivered(frame.stream);
        callHost(callbacks_.on_gpu_frame, static_cast<rcs_gpu_frame>(handle));
        rcs_gpu_frame_release(handle);
        return;
    }
    if (!callbacks_.on_frame) {
        return;
    }
    countDelivered(frame.stream);
    if (frame.pixels) {
        rcs_frame view{};
        view.struct_size = sizeof(view);
        view.stream = frame.stream;
        view.width = frame.width;
        view.height = frame.height;
        view.stride_bytes = frame.stride;
        view.format = RCS_PIXEL_BGRA8;
        view.sequence = frame.sequence;
        view.timestamp_ns = frame.timestampNs;
        view.data = frame.pixels->data();
        view.data_size = frame.pixels->size();
        callHost(callbacks_.on_frame, &view);
    } else {
        platform::withCpuImage(frame, [&](const rcs_frame& view) { callHost(callbacks_.on_frame, &view); });
    }
}

} // namespace rcsdk::session
