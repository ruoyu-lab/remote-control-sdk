#include "agent/agent_server.hpp"

#include "agent/link_messages.hpp"
#include "modules/registry.hpp"
#include "transport/secure_channel.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace rcsdk::agent {
namespace {

// How a run ends for good, whatever `reconnect` says: the session is another
// version (FAILED), or another agent replaced this one (STOPPED).
class End final : public Failure {
public:
    End(rcs_state state, rcs_result result, const std::string& message) : Failure(result, message), state_(state) {}
    rcs_state state() const noexcept { return state_; }

private:
    rcs_state state_;
};

} // namespace

// What the MediaAgent reports, sent over the link. Calls come from the agent's
// threads; none throws: a link that breaks ends the attachment (`cancel`). The
// agent's own failure is its last words: the session closes the link on them,
// which ends the attachment too (or this end does, if the session does not).
class AgentServer::LinkEvents final : public Events {
public:
    LinkEvents(transport::SecureChannel& channel, std::atomic<bool>& cancel) : channel_(channel), cancel_(cancel) {}

    void send(const link::Message& message) noexcept {
        try {
            channel_.send(link::encode(message));
        } catch (...) {
            cancel_ = true;
        }
    }

    // What the commands did, for the periodic reports.
    void inputApplied(rcs_result result) {
        std::lock_guard lock(mutex_);
        lastInput_ = result;
        applied_ += result == RCS_OK;
        inputDirty_ = true;
    }

    // The reports that wait for their time; after a failure, the end of the
    // link when the session has not closed it in time.
    void flush() {
        std::lock_guard lock(mutex_);
        if (failure_) {
            if (Clock::now() >= closeBy_) {
                cancel_ = true;
            }
            return;
        }
        for (uint32_t index = 0; index < capture_.size(); ++index) {
            if (auto& c = capture_[index]; c.pending && Clock::now() >= c.next) {
                sendCaptureLocked(index, *c.pending);
            }
        }
        if (inputDirty_) {
            send(link::InputResults{lastInput_, applied_});
            applied_ = 0;
            inputDirty_ = false;
        }
    }

    // The agent's failure, once it reported one: the attachment ends with it.
    std::optional<Failure> failure() {
        std::lock_guard lock(mutex_);
        return failure_;
    }

    void videoStarted(const VideoStarted& started) override { send(link::Started{started}); }
    void videoPacket(EncodedPacket&& packet) override { send(link::Packet{std::move(packet)}); }
    void videoContent(uint32_t index, const Rect& content, const platform::DisplayMode& display) override {
        send(link::Content{uint8_t(index), content, display});
    }
    void encoderMode(uint32_t index, uint32_t encoding, rcs_codec codec, bool lowLatency, bool refInvalidation,
                     uint32_t temporalLayers, const std::string& name) override {
        send(link::Mode{uint8_t(index), uint8_t(encoding), codec, lowLatency, refInvalidation, uint8_t(temporalLayers),
                        name});
    }
    void encodingFailed(uint32_t index, uint32_t encoding, rcs_result result, const std::string& message) override {
        send(link::EncodingFailed{uint8_t(index), uint8_t(encoding), result, message});
    }
    // A few times a second, and at once when the capture pauses or resumes.
    void captureStatus(uint32_t index, const CaptureStatus& status) override {
        std::lock_guard lock(mutex_);
        auto& c = capture_.at(index);
        if (!c.sent || status.paused != c.sent->paused || Clock::now() >= c.next) {
            sendCaptureLocked(index, status);
        } else {
            c.pending = status;
        }
    }
    // Each stream's pipeline on its own thread: their images are their own.
    void cursor(uint32_t index, const CursorState& state) override {
        CursorState copy = state;
        {
            std::lock_guard lock(mutex_);
            auto& image = images_.at(index);
            if (image && *image == state.imageSerial) {
                copy.image = nullptr; // The session has this image.
            }
            image = state.imageSerial;
        }
        send(link::Pointer{uint8_t(index), std::move(copy)});
    }
    void displays(const std::vector<rcs_display_info>& list) override { send(link::DisplayList{list}); }
    void desktopState(const platform::DesktopState& state) override { send(link::Desktop{state}); }
    void inputAvailability(bool available) override { send(link::Availability{available}); }
    void inputResult(rcs_result, uint64_t) override {} // This end executes input: it reports with inputApplied.
    void moduleStatus(const std::string& id, rcs_result result, const std::string& message) override {
        send(link::ModuleStatus{id, result, message});
    }
    // In one turn: another message's parts do not come between.
    void moduleData(const std::string& id, Bytes bytes) override {
        std::lock_guard lock(moduleMutex_);
        link::sendModuleData([this](const link::Message& part) { send(part); }, id, bytes);
    }
    void audioPcm(std::span<const float> samples, uint64_t timestampNs, uint32_t periodFrames) override {
        send(link::Pcm{timestampNs, periodFrames, std::vector<float>(samples.begin(), samples.end())});
    }
    void audioStatus(const platform::AudioStatus& status) override { send(link::AudioState{status}); }
    // The session hears why and closes the link, and this attachment ends: the
    // agent connects again as it was told to, with all of it set up anew.
    void failed(rcs_result result, const std::string& message) override {
        {
            std::lock_guard lock(mutex_);
            if (failure_) {
                return;
            }
            failure_.emplace(result, message);
            closeBy_ = Clock::now() + link::lastWordsWait;
        }
        log::error("agent", "The agent failed (" + std::string(rcs_result_message(result)) + "): " + message);
        send(link::Failed{result, message});
    }

private:
    void sendCaptureLocked(uint32_t index, const CaptureStatus& status) {
        auto& c = capture_[index];
        c.sent = status;
        c.pending.reset();
        c.next = Clock::now() + 100ms;
        send(link::Capture{uint8_t(index), status});
    }

    transport::SecureChannel& channel_;
    std::atomic<bool>& cancel_;
    std::mutex moduleMutex_;
    std::mutex mutex_;
    struct CaptureReports {
        std::optional<CaptureStatus> sent, pending;
        Clock::time_point next{};
    };
    std::array<CaptureReports, RCS_MAX_STREAMS> capture_; // By stream.
    rcs_result lastInput_ = RCS_OK;
    uint64_t applied_ = 0;
    bool inputDirty_ = false;
    std::array<std::optional<uint64_t>, RCS_MAX_STREAMS> images_; // The pointer image each stream sent last.
    std::optional<Failure> failure_;
    Clock::time_point closeBy_{};
};

AgentServer::AgentServer(Options options, StateHandler handler)
    : options_(std::move(options)), handler_(std::move(handler)) {}

AgentServer::~AgentServer() {
    stop();
}

bool AgentServer::start() {
    std::unique_lock lock(mutex_);
    if (thread_.joinable()) {
        if (!ended()) {
            return false;
        }
        lock.unlock();
        thread_.join(); // The run that ended.
        lock.lock();
    }
    stopRequested_ = false;
    state_ = RCS_STATE_STARTING;
    result_ = RCS_OK;
    ended_ = false;
    message_.clear();
    thread_ = log::thread([this] { run(); });
    return true;
}

void AgentServer::stop() {
    {
        std::lock_guard lock(mutex_);
        stopRequested_ = true;
        if (linkCancel_) {
            *linkCancel_ = true;
        }
    }
    changed_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

rcs_state AgentServer::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

std::pair<rcs_result, std::string> AgentServer::failure() const {
    std::lock_guard lock(mutex_);
    return {result_, message_};
}

bool AgentServer::ended() const {
    return ended_;
}

bool AgentServer::wait(std::optional<std::chrono::milliseconds> timeout) {
    std::unique_lock lock(mutex_);
    if (!timeout) {
        changed_.wait(lock, [&] { return ended(); });
        return true;
    }
    return changed_.wait_for(lock, *timeout, [&] { return ended(); });
}

void AgentServer::setState(rcs_state state, rcs_result result) {
    {
        std::lock_guard lock(mutex_);
        if (state == state_ && result == result_) {
            return;
        }
        state_ = state;
        result_ = result;
    }
    if (handler_) {
        handler_(state, result);
    }
    if (state == RCS_STATE_STOPPED || state == RCS_STATE_FAILED) { // A waiter hears the end after the handler did.
        std::lock_guard lock(mutex_);
        ended_ = true;
    }
    changed_.notify_all();
}

// ---- The run -----------------------------------------------------------------------------------

void AgentServer::run() {
    setState(RCS_STATE_CONNECTING, RCS_OK);
    bool attached = false; // A link has served: what follows is a reconnection.
    std::string told;      // Why the last try failed, as logged: the same again is not logged again.
    for (;;) {
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard lock(mutex_);
            if (stopRequested_) {
                break;
            }
            linkCancel_ = cancel;
        }
        rcs_result result = RCS_ERROR_INTERNAL;
        try {
            attachment(cancel, attached);
        } catch (const End& end) {
            (end.state() == RCS_STATE_FAILED ? log::error : log::info)("agent", end.what());
            {
                std::lock_guard lock(mutex_);
                linkCancel_.reset();
                message_ = end.what();
            }
            setState(end.state(), end.result());
            return;
        } catch (const Failure& failure) {
            result = failure.result();
            std::lock_guard lock(mutex_);
            message_ = failure.what();
        }
        {
            std::lock_guard lock(mutex_);
            linkCancel_.reset();
            if (stopRequested_) {
                break;
            }
            if (attached) {
                log::info("agent", "The link ended (" + message_ + ")");
            } else if (message_ != told) { // A session that is not there, or refuses, is retried quietly.
                log::warning("agent", "Could not attach to the session (" + message_ + ")");
            }
            told = message_;
        }
        if (!options_.reconnect) {
            setState(result == RCS_ERROR_DISCONNECTED ? RCS_STATE_STOPPED : RCS_STATE_FAILED, result);
            return;
        }
        setState(attached ? RCS_STATE_RECONNECTING : RCS_STATE_CONNECTING, result);
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, options_.reconnectInterval, [&] { return stopRequested_; });
    }
    setState(RCS_STATE_STOPPED, RCS_OK);
}

void AgentServer::checkSession(const transport::LocalPeer& session) const {
    const auto& self = transport::thisProcess();
    if (options_.access == transport::LocalAccess::anyUser || session.privileged || session.user == self.user) {
        return;
    }
    fail(RCS_ERROR_AUTHENTICATION,
         "The process serving " + options_.endpoint + " (process " + std::to_string(session.processId) + ") runs as " +
             session.account + "; this agent serves a session of its own account (" + self.account +
             "), LocalSystem or an elevated administrator (rcs_agent_config.access)");
}

void AgentServer::attachment(const std::shared_ptr<std::atomic<bool>>& cancel, bool& attached) {
    Machine machine = localMachine(); // Before connecting: probing the codecs takes a while.
    if (const auto& host = options_.hostEncoder) {
        for (const rcs_codec codec : host->codecs) {
            machine.hostEncoders |= codec ? 1u << codec : 0;
        }
        machine.hostEncoderFormats = host->picture_formats;
    }
    auto connection = transport::connectLocal(options_.endpoint, *cancel, 2s);
    checkSession(connection.peer);
    // The prefixes first: a session of another version is told apart before anything else.
    connection.stream->write(link::prefix(), 2s);
    crypto::Bytes theirs(link::prefixSize);
    connection.stream->read(theirs, 5s);
    try {
        link::checkPrefix(theirs, "session");
    } catch (const Failure& failure) {
        if (failure.result() == RCS_ERROR_UNSUPPORTED) { // Connecting again changes nothing.
            throw End(RCS_STATE_FAILED, failure.result(), failure.what());
        }
        throw;
    }
    transport::SecureChannel channel(std::move(connection.stream), transport::SecureChannel::Side::client,
                                     link::linkCredentials(options_.key));
    channel.send(link::encode(link::AgentHello{machine}));
    const auto reply = link::decode(channel.receive());
    if (const auto* refused = std::get_if<link::Refused>(&reply)) {
        fail(refused->result, "The session refused this agent: " + refused->reason);
    }
    if (!std::holds_alternative<link::Accepted>(reply)) {
        fail(RCS_ERROR_NETWORK, "The session did not answer the agent's hello");
    }
    attached = true;
    setState(RCS_STATE_RUNNING, RCS_OK);
    log::info("agent", "Attached to the session on " + options_.endpoint + " (process " +
                           std::to_string(connection.peer.processId) + ", " + connection.peer.account + ")");

    LinkEvents events(channel, *cancel);
    std::array<std::atomic<uint64_t>, RCS_MAX_ENCODINGS> backlogNs{};
    MediaAgent media(events);
    link::ModuleAssembly modulesIn; // The modules' messages, as their parts come.
    if (options_.hostEncoder) {
        media.setHostEncoder(*options_.hostEncoder);
    }
    media.setBacklogSource([&backlogNs](uint32_t encoding) { return backlogNs[encoding].load(); });

    // Reports that wait for their time, and the heartbeat that tells the
    // session this end is alive.
    std::mutex tickMutex;
    std::condition_variable tickWake;
    bool done = false;
    std::thread ticker = log::thread([&] {
        std::unique_lock lock(tickMutex);
        while (!done) {
            tickWake.wait_for(lock, 50ms, [&] { return done; });
            lock.unlock();
            events.flush();
            if (Clock::now() - channel.lastSend() >= 1s) {
                events.send(link::Heartbeat{});
            }
            lock.lock();
        }
    });
    const auto stopTicker = [&] {
        {
            std::lock_guard lock(tickMutex);
            done = true;
        }
        tickWake.notify_all();
        ticker.join();
    };

    try {
        for (;;) {
            auto message = link::decode(channel.receive());
            if (events.failure() && !std::holds_alternative<link::Replaced>(message)) {
                continue; // It failed and said so: nothing more is done until the session closes the link.
            }
            std::visit(
                Overloaded{[&](link::StartVideo&& m) { media.startVideo(m.start); },
                           [&](link::StopVideo&& m) { media.stopVideo(m.index, m.encoding); },
                           [&](link::SetRate&& m) { media.setRate(m.index, m.encoding, m.bitrate, m.budgeted); },
                           [&](link::SetCursorInVideo&& m) { media.setCursorInVideo(m.inVideo); },
                           [&](link::SetInputBoost&& m) { media.setInputBoost(m.ms); },
                           [&](link::RequestKeyframe&& m) { media.requestKeyframe(m.index, m.encoding); },
                           [&](link::InvalidateAfter&& m) {
                               media.invalidateAfter(m.index, m.encoding, m.sequence);
                           },
                           [&](link::Backlog&& m) {
                               for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                                   backlogNs[encoding] = m.ns[encoding];
                               }
                           },
                           [&](link::StartAudio&& m) { media.startAudio(m.start); },
                           [&](link::StopAudio&&) { media.stopAudio(); },
                           [&](link::ConfigureInput&& m) {
                               media.configureInput(m.target, m.streamDisplays, m.options);
                           },
                           [&](link::ApplyInput&& m) { events.inputApplied(media.applyInput(m.event)); },
                           [&](link::ReleaseInput&&) { media.releaseInput(); },
                           [&](link::ModuleStart&& m) { media.startModule(m.id, m.version); },
                           [&](link::ModuleStop&& m) { media.stopModule(m.id); },
                           [&](link::ModuleData&& m) {
                               // A module this process has not registered takes nothing (its start
                               // said so); one it has, up to what it takes.
                               const auto* module = modules::find(m.id);
                               const std::string id = m.id;
                               if (auto whole = modulesIn.add(std::move(m), module ? module->table.max_message_bytes
                                                                                   : RCS_MODULE_MAX_MESSAGE)) {
                                   media.moduleData(id, std::move(*whole));
                               }
                           },
                           [&](link::ListWindows&& m) { events.send(link::WindowList{m.serial, media.windows()}); },
                           [](link::Heartbeat&&) {},
                           [](link::Replaced&&) {
                               throw End(RCS_STATE_STOPPED, RCS_ERROR_REPLACED,
                                         "Another agent took this one's place at the session");
                           },
                           [](auto&&) { fail(RCS_ERROR_NETWORK, "The session sent a message that is not a command"); }},
                std::move(message));
        }
    } catch (const End&) {
        stopTicker();
        throw; // Replaced, it ends, also after a failure of its own.
    } catch (...) {
        stopTicker();
        if (auto failure = events.failure()) {
            throw *failure; // The agent's own failure ended it, not the link.
        }
        throw;
    }
}

} // namespace rcsdk::agent
