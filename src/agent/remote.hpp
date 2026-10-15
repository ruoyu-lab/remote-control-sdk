// A remote agent, as the session sees it: the agent runs in another process
// of this machine (in the user's login session, beside a service that holds
// the connection) and reaches the session over a local link: an endpoint the
// session listens on (transport/local_stream.hpp), the link's prefix and
// secure channel, and the messages of agent/link_messages.hpp.
//
// RemoteAgent is the session's Port. Agents that connect are checked at once,
// each on a thread of its own: the system says which process and account each
// is, the key proves it may serve, and the Observer (the host) decides whether
// it attaches. The one that attaches replaces the one before, which is told so
// and ends. It gets what the session asked for, and every agent that attaches
// later gets it again: a stream, the sound and the input setup carry over to
// the next agent, and the session needs no case for an agent that came or
// went. While none is attached, the session hears that the picture is paused,
// the sound off and input unavailable.
#pragma once

#include "agent/agent.hpp"
#include "agent/link_messages.hpp"
#include "transport/credentials.hpp"
#include "transport/local_stream.hpp"
#include "transport/secure_channel.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace rcsdk::agent {

class RemoteAgent final : public Port {
public:
    // Agents asking to attach, attaching and leaving, on the link's threads,
    // never while a lock of the RemoteAgent is held.
    class Observer {
    public:
        virtual ~Observer() = default;
        // An agent passed the handshake: its machine, and its process as the
        // system reports it. Throws Failure to refuse it (the agent is told
        // why); it then replaces nobody.
        virtual void admit(const Machine& machine) = 0;
        // It attached and has the session's asks again.
        virtual void attached(const Machine& machine) = 0;
        // The attached agent left: why (RCS_OK: it closed the link). Not called
        // for one that another replaced.
        virtual void detached(rcs_result result, const std::string& message) = 0;
    };

    struct Options {
        std::string endpoint;
        crypto::Key key{};
        transport::LocalAccess access = transport::LocalAccess::privileged;
    };

    // Creates the endpoint and listens. Throws Failure when it cannot.
    RemoteAgent(Events& events, Observer& observer, const Options& options);
    ~RemoteAgent() override;

    // Whether an agent is attached now.
    bool attached() const;

    Machine machine() const override;
    void startVideo(const VideoStart& start) override;
    void stopVideo(uint32_t index, uint32_t encoding) override;
    void setRate(uint32_t index, uint32_t encoding, uint32_t bitrate, bool budgeted) override;
    void setCursorInVideo(bool inVideo) override;
    void setInputBoost(uint32_t ms) override;
    void requestKeyframe(uint32_t index, uint32_t encoding) override;
    void invalidateAfter(uint32_t index, uint32_t encoding, uint64_t sequence) override;
    void setBacklogSource(std::function<uint64_t(uint32_t)> backlogNs) override;
    void startAudio(const AudioStart& start) override;
    void stopAudio() override;
    void configureInput(const platform::CaptureTarget& target, const std::vector<uint32_t>& streamDisplays,
                        const platform::InputOptions& options) override;
    rcs_result applyInput(const rcs_input_event& event) override;
    void releaseInput() override;
    void startModule(const std::string& id, uint32_t version) override;
    void stopModule(const std::string& id) override;
    void moduleData(const std::string& id, Bytes bytes) override;
    std::vector<rcs_window_info> windows() override;

private:
    // One agent's connection: checked on its reader thread, which then reads
    // its events; once attached, a writer thread sends the session's asks.
    struct Attachment {
        transport::LocalPeer peer;
        std::shared_ptr<transport::SecureChannel> channel;
        std::atomic<bool> closed{false}; // Ends its stream's transfers.
        std::atomic<int> running{0};     // Threads still going.
        bool replaced = false;           // Its events are not heard any more (under deliverMutex_).
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<crypto::Bytes> queue; // Encoded commands, in order.
        bool retiring = false;           // Replaced: the queue holds its last message.
        // Why it is closed, as first said (an agent's failure, a stuck agent).
        std::optional<std::pair<rcs_result, std::string>> departure;
        std::thread reader, writer;
        // The reader's, by stream: the pointer image the agent last sent (later
        // positions reuse it), and the size of the picture it started.
        struct Stream {
            std::shared_ptr<const CursorImage> cursorImage;
            uint64_t cursorSerial = 0;
            std::optional<std::pair<uint32_t, uint32_t>> picture;
        };
        std::array<Stream, RCS_MAX_STREAMS> streams;
        link::ModuleAssembly modulesIn; // Its modules' messages, as their parts come.
    };

    // What the session last asked for: asked again of the next agent.
    struct Desired {
        // By stream, and each stream's by encoding.
        std::array<std::array<std::optional<VideoStart>, RCS_MAX_ENCODINGS>, RCS_MAX_STREAMS> video;
        std::optional<AudioStart> audio;
        std::optional<link::ConfigureInput> input;
        std::map<std::string, uint32_t> modules; // The machine's halves, and the session's versions.
    };

    // Connections not attached yet, per account (LocalPeer::user): how many
    // are being checked, and after failed ones, until when the next waits.
    struct Source {
        uint32_t pending = 0;
        uint32_t failures = 0;
        Clock::time_point waitUntil{};
    };

    // Refused connections in the log: the first of a burst at once, the rest
    // counted and summed up every few seconds.
    class RefusalLog {
    public:
        ~RefusalLog();
        void add(const transport::LocalPeer* peer, const std::string& reason);

    private:
        std::mutex mutex_;
        Clock::time_point windowEnd_{};
        uint64_t counted_ = 0;
        std::string last_;
    };

    void accept();
    // Why a client may not try the handshake now; empty when it may.
    std::string admission(const transport::LocalPeer& peer);
    // `confirm`: the connection's (transport::LocalConnection), checked once the agent spoke.
    void serve(const std::shared_ptr<Attachment>& attachment, std::unique_ptr<transport::Stream> stream,
               const std::function<void()>& confirm);
    Machine handshake(Attachment& attachment, std::unique_ptr<transport::Stream> stream);
    // A connection that did not attach: counted against its account.
    void refused(Attachment& attachment, const std::string& reason);
    bool attach(const std::shared_ptr<Attachment>& next, const Machine& machine);
    void retire(Attachment& old);
    void read(Attachment& attachment);
    void write(Attachment& attachment);
    void dispatch(Attachment& attachment, link::Message&& message);
    // The agent's connection ended (its reader thread): the session hears what that means.
    void lost(Attachment& attachment, rcs_result result, const std::string& reason);
    void close(Attachment& attachment) noexcept;
    // Closes it with this reason, unless it was given one before.
    void depart(Attachment& attachment, rcs_result result, const std::string& reason);
    void send(const link::Message& message); // To the attached agent, if any.
    // What the session asked for is updated with the ask, atomically with
    // respect to an agent attaching, and sent.
    template <class Update>
    void command(Update&& update, const link::Message& message);
    // Queues the message for the attached agent, holding mutex_. One that does
    // not take what is sent is closed; its reader thread reports it.
    void enqueue(const link::Message& message);
    void replay(Attachment& attachment); // Holding mutex_.
    void reap();                          // Holding mutex_.
    // What was asked of an agent whose link ended fails: no answer comes.
    void abandonAsks(const Attachment& attachment);

    Events& events_;
    Observer& observer_;
    transport::Credentials credentials_;
    transport::LocalListener listener_;
    RefusalLog refusals_;

    mutable std::mutex mutex_; // current_, desired_, machine_, attachments_, sources_, pending_
    std::shared_ptr<Attachment> current_;
    std::vector<std::shared_ptr<Attachment>> attachments_;
    std::map<std::string, Source> sources_;
    uint32_t pending_ = 0;
    Desired desired_;
    Machine machine_;
    std::array<CaptureStatus, RCS_MAX_STREAMS> lastCapture_;

    // Window lists asked of the attached agent, by the number of the ask,
    // until the asker takes the answer or gives up: who was asked, and the answer.
    struct WindowsAsk {
        const Attachment* agent = nullptr;
        std::optional<std::vector<rcs_window_info>> windows;
        std::optional<Failure> failure;
    };
    std::mutex windowsMutex_; // After mutex_, never before it.
    std::condition_variable windowsAnswered_;
    uint32_t windowsAsked_ = 0;
    std::map<uint32_t, WindowsAsk> windowAsks_;

    std::mutex deliverMutex_; // One agent's events at a time, in order; its arrival and leaving among them.
    std::mutex backlogMutex_;
    std::function<uint64_t(uint32_t)> backlogSource_;
    std::atomic<bool> quit_{false};
    std::thread accepter_; // Last: it starts once everything above exists.
};

} // namespace rcsdk::agent
