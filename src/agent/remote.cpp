#include "agent/remote.hpp"

#include "modules/registry.hpp"
#include "util/log.hpp"

#include <algorithm>

namespace rcsdk::agent {
namespace {

// An agent that lets this many commands wait is stuck, not busy.
constexpr size_t maxQueued = 8192;
// Connections checked at once: in all, and of one account. More wait in the
// endpoint's queue (they are not read), so a process that connects and stays
// silent holds a few of these for seconds at most, never the endpoint.
constexpr uint32_t maxPending = 16, maxPendingPerAccount = 2;

// After a connection of an account failed, the next waits this long, doubling.
std::chrono::milliseconds backoff(uint32_t failures) {
    return std::min<std::chrono::milliseconds>(250ms * (1u << std::min(failures - 1, 6u)), 10s);
}

std::string who(const transport::LocalPeer& peer) {
    return "process " + std::to_string(peer.processId) + ", " + peer.account;
}

// The last of an attachment's threads to end lets go of the channel: that
// closes the agent's end.
void finish(std::shared_ptr<transport::SecureChannel>& channel, std::atomic<int>& running) {
    if (--running == 0) {
        channel.reset();
    }
}

} // namespace

RemoteAgent::RefusalLog::~RefusalLog() {
    if (counted_) {
        log::warning("agent",
                     std::to_string(counted_) + " more connections to the agent link were refused, the last: " + last_);
    }
}

void RemoteAgent::RefusalLog::add(const transport::LocalPeer* peer, const std::string& reason) {
    const std::string text = (peer ? who(*peer) : std::string("a client")) + ": " + reason;
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    if (now < windowEnd_) {
        ++counted_;
        last_ = text;
        return;
    }
    if (counted_) {
        log::warning("agent", std::to_string(counted_) +
                                  " more connections to the agent link were refused within 10 s, the last: " + last_);
    }
    counted_ = 0;
    windowEnd_ = now + 10s;
    log::warning("agent", "Refused a connection to the agent link (" + text + ")");
}

RemoteAgent::RemoteAgent(Events& events, Observer& observer, const Options& options)
    : events_(events), observer_(observer), credentials_(link::linkCredentials(options.key)),
      listener_(options.endpoint, options.access) {
    log::info("agent", "Waiting for an agent on " + options.endpoint);
    accepter_ = log::thread([this] { accept(); });
}

RemoteAgent::~RemoteAgent() {
    quit_ = true;
    {
        std::lock_guard lock(mutex_);
        for (const auto& attachment : attachments_) {
            close(*attachment);
        }
    }
    accepter_.join();
    std::vector<std::shared_ptr<Attachment>> all;
    {
        std::lock_guard lock(mutex_);
        all = attachments_;
    }
    for (const auto& attachment : all) {
        close(*attachment);
        if (attachment->reader.joinable()) {
            attachment->reader.join(); // First: it may still be starting the writer.
        }
        if (attachment->writer.joinable()) {
            attachment->writer.join();
        }
    }
}

bool RemoteAgent::attached() const {
    std::lock_guard lock(mutex_);
    return current_ != nullptr;
}

Machine RemoteAgent::machine() const {
    std::lock_guard lock(mutex_);
    return machine_;
}

// ---- Agents that connect -----------------------------------------------------------------------

void RemoteAgent::accept() {
    while (!quit_) {
        auto next = std::make_shared<Attachment>();
        transport::LocalConnection connection;
        try {
            connection = listener_.accept(quit_, next->closed);
        } catch (const Failure& failure) {
            if (quit_) {
                break;
            }
            refusals_.add(nullptr, failure.what());
            if (failure.result() == RCS_ERROR_NETWORK) {
                std::this_thread::sleep_for(100ms); // The listener itself may be failing.
            }
            continue;
        }
        std::string refusal;
        {
            std::lock_guard lock(mutex_);
            if (quit_) {
                break;
            }
            refusal = admission(connection.peer);
            if (refusal.empty()) {
                reap();
                next->peer = connection.peer;
                next->running = 1;
                attachments_.push_back(next);
                next->reader = log::thread([this, next, stream = std::move(connection.stream),
                                            confirm = std::move(connection.confirm)]() mutable {
                    serve(next, std::move(stream), confirm);
                    finish(next->channel, next->running);
                });
            }
        }
        if (!refusal.empty()) {
            refusals_.add(&connection.peer, refusal); // Its stream closes unread.
        }
    }
}

std::string RemoteAgent::admission(const transport::LocalPeer& peer) {
    auto& source = sources_[peer.user];
    if (Clock::now() < source.waitUntil) {
        return "its account's last try failed moments ago";
    }
    if (source.pending >= maxPendingPerAccount) {
        return "connections of its account are being checked already";
    }
    if (pending_ >= maxPending) {
        return std::to_string(maxPending) + " connections are being checked already";
    }
    ++source.pending;
    ++pending_;
    return {};
}

// A connection's reader thread: the checks, then the agent's events once it attached.
void RemoteAgent::serve(const std::shared_ptr<Attachment>& attachment, std::unique_ptr<transport::Stream> stream,
                        const std::function<void()>& confirm) {
    auto& a = *attachment;
    Machine machine;
    try {
        machine = handshake(a, std::move(stream));
        confirm(); // The process that spoke the handshake is the one the system named.
        machine.process = a.peer;
        try {
            observer_.admit(machine);
        } catch (const Failure& refusal) {
            try {
                a.channel->send(link::encode(link::Refused{refusal.result(), refusal.what()}));
            } catch (const Failure&) { // It may be gone; why it was refused is what is reported.
            }
            throw;
        }
        a.channel->send(link::encode(link::Accepted{}));
    } catch (const Failure& failure) {
        refused(a, failure.what());
        return;
    }
    if (attach(attachment, machine)) {
        read(a);
    }
}

// The prefixes first, in the clear: an agent of another version learns that
// it is (and this end says so in its log) before either tries to understand
// the other. Then the secure channel, and the agent's hello.
Machine RemoteAgent::handshake(Attachment& attachment, std::unique_ptr<transport::Stream> stream) {
    crypto::Bytes theirs(link::prefixSize);
    stream->read(theirs, 2s); // An agent sends it as it connects.
    stream->write(link::prefix(), 2s);
    link::checkPrefix(theirs, "agent");
    attachment.channel = std::make_shared<transport::SecureChannel>(
        std::move(stream), transport::SecureChannel::Side::server, credentials_);
    const auto first = link::decode(attachment.channel->receive());
    const auto* hello = std::get_if<link::AgentHello>(&first);
    if (!hello) {
        fail(RCS_ERROR_NETWORK, "The first message of the agent is not its hello");
    }
    return hello->machine;
}

void RemoteAgent::refused(Attachment& attachment, const std::string& reason) {
    {
        std::lock_guard lock(mutex_);
        auto& source = sources_[attachment.peer.user];
        --source.pending;
        --pending_;
        if (quit_) {
            return;
        }
        ++source.failures;
        source.waitUntil = Clock::now() + backoff(source.failures);
    }
    refusals_.add(&attachment.peer, reason);
}

bool RemoteAgent::attach(const std::shared_ptr<Attachment>& next, const Machine& machine) {
    std::shared_ptr<Attachment> old;
    {
        std::lock_guard lock(mutex_);
        auto& source = sources_[next->peer.user];
        --source.pending;
        --pending_;
        if (quit_) {
            return false;
        }
        source.failures = 0;
        source.waitUntil = {};
        machine_ = machine;
        replay(*next);
        old = std::move(current_);
        current_ = next;
        reap();
    }
    if (old) {
        retire(*old);
    }
    // Reported before any of its events: what it asks is queued meanwhile;
    // and in order with what the agent before it left behind (lost()).
    log::info("agent", "An agent attached (" + who(next->peer) + ", session " + std::to_string(next->peer.osSession) +
                           ")");
    {
        std::lock_guard deliver(deliverMutex_);
        observer_.attached(machine);
    }
    ++next->running;
    next->writer = log::thread([this, next] {
        write(*next);
        finish(next->channel, next->running);
    });
    return true;
}

// The agent before is replaced: its events are not heard from here on, and
// its last message tells it to end (it closes the link then; its reader reads
// on until it did).
void RemoteAgent::retire(Attachment& old) {
    {
        std::lock_guard deliver(deliverMutex_);
        old.replaced = true;
    }
    {
        std::lock_guard lock(old.mutex);
        old.queue.clear();
        old.queue.push_back(link::encode(link::Replaced{}));
        old.retiring = true;
    }
    old.wake.notify_all();
    log::info("agent", "The agent before (" + who(old.peer) + ") is told it was replaced");
}

void RemoteAgent::read(Attachment& attachment) {
    rcs_result result = RCS_OK;
    std::string reason = "it closed the link";
    try {
        while (!attachment.closed) {
            dispatch(attachment, link::decode(attachment.channel->receive()));
        }
    } catch (const Failure& failure) {
        if (failure.result() != RCS_ERROR_DISCONNECTED) {
            result = failure.result();
            reason = failure.what();
        }
    } catch (const std::exception& exception) {
        result = RCS_ERROR_INTERNAL;
        reason = exception.what();
    }
    lost(attachment, result, reason);
}

void RemoteAgent::write(Attachment& attachment) {
    std::array<uint64_t, RCS_MAX_ENCODINGS> sentBacklog{};
    try {
        while (!attachment.closed) {
            const bool polling = [&] {
                std::lock_guard lock(backlogMutex_);
                return bool(backlogSource_);
            }();
            std::deque<crypto::Bytes> work;
            bool last = false;
            {
                std::unique_lock lock(attachment.mutex);
                attachment.wake.wait_for(lock, polling ? 2ms : 200ms,
                                         [&] { return !attachment.queue.empty() || attachment.closed.load(); });
                work.swap(attachment.queue);
                last = attachment.retiring;
            }
            for (const auto& bytes : work) {
                attachment.channel->send(bytes);
            }
            if (last) { // It has been told it was replaced: it closes the link, which its reader hears.
                std::unique_lock lock(attachment.mutex);
                attachment.wake.wait_for(lock, link::lastWordsWait, [&] { return attachment.closed.load(); });
                lock.unlock();
                close(attachment);
                return;
            }
            auto backlog = sentBacklog;
            {
                std::lock_guard lock(backlogMutex_);
                if (backlogSource_) {
                    for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                        backlog[encoding] = backlogSource_(encoding);
                    }
                }
            }
            bool changed = false;
            for (uint32_t encoding = 0; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                const uint64_t now = backlog[encoding], sent = sentBacklog[encoding];
                changed = changed || (now > sent ? now - sent : sent - now) >= 500'000 || (now == 0) != (sent == 0);
            }
            if (changed) {
                attachment.channel->send(link::encode(link::Backlog{backlog}));
                sentBacklog = backlog;
            }
            if (Clock::now() - attachment.channel->lastSend() >= 1s) {
                attachment.channel->send(link::encode(link::Heartbeat{}));
            }
        }
    } catch (const Failure& failure) {
        depart(attachment, RCS_ERROR_NETWORK, failure.what());
    }
}

void RemoteAgent::dispatch(Attachment& attachment, link::Message&& message) {
    std::lock_guard deliver(deliverMutex_);
    if (attachment.closed || attachment.replaced) { // What it still sends is not heard.
        return;
    }
    std::visit(Overloaded{[&](link::Started&& m) {
                              if (!m.started.encoding) { // The capture's frames, as its content is checked.
                                  attachment.streams[m.started.index].picture =
                                      std::pair{m.started.stream.width, m.started.stream.height};
                              }
                              events_.videoStarted(m.started);
                          },
                          [&](link::Packet&& m) { events_.videoPacket(std::move(m.packet)); },
                          [&](link::Content&& m) {
                              const auto& picture = attachment.streams[m.index].picture;
                              if (!picture || !link::fits(m.content, picture->first, picture->second)) {
                                  fail(RCS_ERROR_PROTOCOL, "Invalid message: content outside the picture");
                              }
                              events_.videoContent(m.index, m.content, m.display);
                          },
                          [&](link::Mode&& m) {
                              events_.encoderMode(m.index, m.encoding, m.codec, m.lowLatency, m.refInvalidation,
                                                  m.temporalLayers, m.name);
                          },
                          [&](link::EncodingFailed&& m) {
                              events_.encodingFailed(m.index, m.encoding, m.result, m.message);
                          },
                          [&](link::Capture&& m) {
                              {
                                  std::lock_guard lock(mutex_);
                                  lastCapture_[m.index] = m.status;
                              }
                              events_.captureStatus(m.index, m.status);
                          },
                          [&](link::Pointer&& m) {
                              auto& stream = attachment.streams[m.index];
                              if (m.state.image) {
                                  stream.cursorImage = m.state.image;
                                  stream.cursorSerial = m.state.imageSerial;
                              } else if (stream.cursorImage && stream.cursorSerial == m.state.imageSerial) {
                                  m.state.image = stream.cursorImage; // The position of an image sent before.
                              }
                              events_.cursor(m.index, m.state);
                          },
                          [&](link::DisplayList&& m) {
                              {
                                  std::lock_guard lock(mutex_);
                                  machine_.displays = m.list;
                              }
                              events_.displays(m.list);
                          },
                          [&](link::Availability&& m) { events_.inputAvailability(m.available); },
                          [&](link::Desktop&& m) { events_.desktopState(m.state); },
                          [&](link::InputResults&& m) { events_.inputResult(m.last, m.applied); },
                          [&](link::ModuleData&& m) {
                              const auto* module = modules::find(m.id);
                              if (!module) {
                                  fail(RCS_ERROR_PROTOCOL, "The agent sent a message of a module the session "
                                                           "does not have");
                              }
                              const std::string id = m.id;
                              if (auto whole = attachment.modulesIn.add(std::move(m), module->table.max_message_bytes)) {
                                  events_.moduleData(id, std::move(*whole));
                              }
                          },
                          [&](link::ModuleStatus&& m) { events_.moduleStatus(m.id, m.result, m.message); },
                          [&](link::Pcm&& m) { events_.audioPcm(m.samples, m.timestampNs, m.periodFrames); },
                          [&](link::AudioState&& m) { events_.audioStatus(m.status); },
                          // Answers to no ask still waiting, or to another agent's, are dropped.
                          [&](link::WindowList&& m) {
                              std::lock_guard lock(windowsMutex_);
                              const auto ask = windowAsks_.find(m.serial);
                              if (ask != windowAsks_.end() && ask->second.agent == &attachment) {
                                  ask->second.windows = std::move(m.windows);
                                  windowsAnswered_.notify_all();
                              }
                          },
                          // It cannot go on: it leaves with why, and connects again as it was told to.
                          [&](link::Failed&& m) { depart(attachment, m.result, m.message); }, [](link::Heartbeat&&) {},
                          [](auto&&) { fail(RCS_ERROR_NETWORK, "The agent sent a message that is not an event"); }},
               std::move(message));
}

void RemoteAgent::lost(Attachment& attachment, rcs_result result, const std::string& reason) {
    close(attachment);
    std::pair<rcs_result, std::string> why{result, reason};
    {
        std::lock_guard lock(attachment.mutex);
        if (attachment.departure) {
            why = *attachment.departure;
        }
    }
    bool attached = false, audio = false;
    std::array<std::optional<CaptureStatus>, RCS_MAX_STREAMS> capture; // Of the streams asked for.
    {
        std::lock_guard lock(mutex_);
        attached = current_.get() == &attachment && !quit_; // Else replaced, or this side ends.
        if (attached) {
            current_.reset();
            audio = desired_.audio.has_value();
            for (size_t index = 0; index < capture.size(); ++index) {
                if (desired_.video[index][0]) {
                    capture[index] = lastCapture_[index];
                }
            }
        }
    }
    // Once it is no longer the one asked (no ask of it can follow); also one
    // that was replaced, as its answers are not heard.
    abandonAsks(attachment);
    if (!attached) {
        return;
    }
    if (why.first == RCS_OK) {
        log::info("agent", "The agent left (" + why.second + ")");
    } else {
        log::warning("agent",
                     "The agent left (" + std::string(rcs_result_message(why.first)) + ": " + why.second + ")");
    }
    std::lock_guard deliver(deliverMutex_);
    {
        std::lock_guard lock(mutex_);
        if (current_) { // The next agent attached meanwhile: what it said stands.
            return;
        }
    }
    for (uint32_t index = 0; index < capture.size(); ++index) {
        if (auto& status = capture[index]) {
            status->paused = RCS_CAPTURE_PAUSE_AGENT; // The picture waits for the next agent.
            events_.captureStatus(index, *status);
        }
    }
    events_.inputAvailability(false);
    events_.desktopState({}); // Nothing is known of the machine's desktop now.
    if (audio) {
        events_.audioStatus({false, RCS_ERROR_DEVICE, "The agent left"});
    }
    observer_.detached(why.first, why.second);
}

void RemoteAgent::close(Attachment& attachment) noexcept {
    {
        std::lock_guard lock(attachment.mutex); // Its writer does not miss it between looking and waiting.
        attachment.closed = true;
    }
    attachment.wake.notify_all();
}

void RemoteAgent::depart(Attachment& attachment, rcs_result result, const std::string& reason) {
    {
        std::lock_guard lock(attachment.mutex);
        if (!attachment.departure) {
            attachment.departure.emplace(result, reason);
        }
    }
    close(attachment); // Its reader thread wakes and reports it.
}

// What the session asked for, in the order that brings an agent up: input, sound, picture.
void RemoteAgent::replay(Attachment& attachment) {
    std::lock_guard lock(attachment.mutex);
    const auto push = [&](const link::Message& message) { attachment.queue.push_back(link::encode(message)); };
    if (desired_.input) {
        push(*desired_.input);
    }
    for (const auto& [id, version] : desired_.modules) {
        push(link::ModuleStart{id, version});
    }
    if (desired_.audio) {
        push(link::StartAudio{*desired_.audio});
    }
    for (const auto& stream : desired_.video) {
        for (const auto& video : stream) { // The main encoding first: the others take its pictures.
            if (video) {
                push(link::StartVideo{*video});
            }
        }
    }
}

// Finished attachments are joined and forgotten.
void RemoteAgent::reap() {
    attachments_.erase(std::remove_if(attachments_.begin(), attachments_.end(),
                                      [&](const std::shared_ptr<Attachment>& attachment) {
                                          if (attachment->running != 0 || attachment == current_) {
                                              return false;
                                          }
                                          if (attachment->reader.joinable()) {
                                              attachment->reader.join();
                                          }
                                          if (attachment->writer.joinable()) {
                                              attachment->writer.join();
                                          }
                                          return true;
                                      }),
                       attachments_.end());
}

void RemoteAgent::enqueue(const link::Message& message) {
    if (!current_) {
        return;
    }
    auto& attachment = *current_;
    auto bytes = link::encode(message);
    {
        std::lock_guard lock(attachment.mutex);
        if (attachment.queue.size() < maxQueued) {
            attachment.queue.push_back(std::move(bytes));
            attachment.wake.notify_one();
            return;
        }
    }
    depart(attachment, RCS_ERROR_NETWORK, "it does not take what the session sends");
}

void RemoteAgent::send(const link::Message& message) {
    std::lock_guard lock(mutex_);
    enqueue(message);
}

// ---- The Port ------------------------------------------------------------------------------------

template <class Update>
void RemoteAgent::command(Update&& update, const link::Message& message) {
    std::lock_guard lock(mutex_);
    update();
    enqueue(message);
}

void RemoteAgent::startVideo(const VideoStart& start) {
    command(
        [&] {
            auto& stream = desired_.video.at(start.index);
            if (!start.encoding) {
                stream = {}; // Its lower encodings start again after it.
            }
            stream.at(start.encoding) = start;
        },
        link::StartVideo{start});
}

void RemoteAgent::stopVideo(uint32_t index, uint32_t encoding) {
    command(
        [&] {
            auto& stream = desired_.video.at(index);
            if (encoding) {
                stream.at(encoding).reset();
            } else {
                stream = {};
            }
        },
        link::StopVideo{uint8_t(index), uint8_t(encoding)});
}

void RemoteAgent::setRate(uint32_t index, uint32_t encoding, uint32_t bitrate, bool budgeted) {
    command(
        [&] {
            if (auto& video = desired_.video.at(index).at(encoding)) {
                video->bitrate = bitrate;
                video->budgeted = budgeted;
            }
        },
        link::SetRate{uint8_t(index), uint8_t(encoding), bitrate, budgeted});
}

void RemoteAgent::setCursorInVideo(bool inVideo) {
    command(
        [&] {
            for (auto& stream : desired_.video) {
                if (auto& video = stream[0]) {
                    video->cursorInVideo = inVideo;
                }
            }
        },
        link::SetCursorInVideo{inVideo});
}

void RemoteAgent::setInputBoost(uint32_t ms) {
    command(
        [&] {
            for (auto& stream : desired_.video) {
                if (auto& video = stream[0]) {
                    video->inputBoostMs = ms;
                }
            }
        },
        link::SetInputBoost{ms});
}

void RemoteAgent::requestKeyframe(uint32_t index, uint32_t encoding) {
    send(link::RequestKeyframe{uint8_t(index), uint8_t(encoding)});
}

void RemoteAgent::invalidateAfter(uint32_t index, uint32_t encoding, uint64_t sequence) {
    send(link::InvalidateAfter{uint8_t(index), uint8_t(encoding), sequence});
}

void RemoteAgent::setBacklogSource(std::function<uint64_t(uint32_t)> backlogNs) {
    const bool cleared = !backlogNs;
    {
        std::lock_guard lock(backlogMutex_);
        backlogSource_ = std::move(backlogNs);
    }
    if (cleared) {
        send(link::Backlog{}); // Nothing waits any more.
    }
}

void RemoteAgent::startAudio(const AudioStart& start) {
    command([&] { desired_.audio = start; }, link::StartAudio{start});
}

void RemoteAgent::stopAudio() {
    command([&] { desired_.audio.reset(); }, link::StopAudio{});
}

void RemoteAgent::configureInput(const platform::CaptureTarget& target, const std::vector<uint32_t>& streamDisplays,
                                 bool secureDesktop) {
    const link::ConfigureInput ask{target, streamDisplays, secureDesktop};
    command([&] { desired_.input = ask; }, ask);
}

void RemoteAgent::startModule(const std::string& id, uint32_t version) {
    command([&] { desired_.modules[id] = version; }, link::ModuleStart{id, version});
}

void RemoteAgent::stopModule(const std::string& id) {
    command([&] { desired_.modules.erase(id); }, link::ModuleStop{id});
}

// Its parts go in one turn, so that another message's do not come between
// them; with no agent attached, it goes nowhere (the module heard
// machine_left).
void RemoteAgent::moduleData(const std::string& id, Bytes bytes) {
    std::lock_guard lock(mutex_);
    link::sendModuleData([&](const link::Message& part) { enqueue(part); }, id, bytes);
}

std::vector<rcs_window_info> RemoteAgent::windows() {
    uint32_t serial;
    {
        std::lock_guard lock(mutex_);
        if (!current_) {
            fail(RCS_ERROR_INVALID_STATE, "No agent is attached to list its windows");
        }
        {
            std::lock_guard asks(windowsMutex_);
            serial = ++windowsAsked_;
            windowAsks_[serial].agent = current_.get();
        }
        enqueue(link::ListWindows{serial});
    }
    std::unique_lock lock(windowsMutex_);
    const struct Forget { // Whatever comes for it later is dropped.
        RemoteAgent& agent;
        uint32_t serial;
        ~Forget() { agent.windowAsks_.erase(serial); }
    } forget{*this, serial};
    auto& ask = windowAsks_[serial];
    if (!windowsAnswered_.wait_for(lock, 1s, [&] { return ask.windows || ask.failure; })) {
        fail(RCS_ERROR_TIMEOUT, "The agent did not list its windows within a second");
    }
    if (ask.failure) {
        throw *ask.failure;
    }
    return std::move(*ask.windows);
}

void RemoteAgent::abandonAsks(const Attachment& attachment) {
    std::lock_guard lock(windowsMutex_);
    for (auto& [serial, ask] : windowAsks_) {
        if (ask.agent == &attachment && !ask.windows) {
            ask.failure.emplace(RCS_ERROR_INVALID_STATE, "The agent left before it listed its windows");
        }
    }
    windowsAnswered_.notify_all();
}

rcs_result RemoteAgent::applyInput(const rcs_input_event& event) {
    std::lock_guard lock(mutex_);
    if (!current_) {
        return RCS_ERROR_INVALID_STATE;
    }
    enqueue(link::ApplyInput{event});
    return kInputDeferred;
}

void RemoteAgent::releaseInput() {
    send(link::ReleaseInput{});
}

} // namespace rcsdk::agent
