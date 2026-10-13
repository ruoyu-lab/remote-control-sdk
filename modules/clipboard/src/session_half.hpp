// The module's session half: this end's newest copy goes to each peer that
// takes copies, fitted to what that peer takes and seen first by the host
// (on_clipboard); a copy that comes is seen by the host and, with target
// SYSTEM, placed on this machine's clipboard by the machine half (a viewer's
// own, a sharer's agent's machine's). A copy placed here never goes back
// where it came from; a sharer passes it on to its other viewers (not its
// files: their publication is the viewer's to this end). The machine's
// clipboard is watched while a peer takes part.
//
// A copy's files (FILES) go through the file transfer module's public
// functions: published to each peer that takes them, withdrawn once a newer
// copy replaces them (the peer hears it, WITHDRAWN); a system paste here of
// a peer's files is a fetch of its publication, asked for by the machine half
// and answered with what the fetch's callbacks are told. Those callbacks come
// on file transfer's thread, outside both modules' calls, and enter this
// half's calls through a Link (rcs_module_run) while it lives.
//
// Every function runs as one of the module's calls (the SDK's callbacks, or
// rcs_module_run for the host's functions), so none overlaps another.
#pragma once

#include "messages.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace rcsdk::clipboard {

class Hub;

// How file transfer's callbacks reach a hub: as one of its module's calls,
// while the hub lives (its destructor waits for those under way).
class Link {
public:
    Link(rcs_module module, Hub& hub) : module_(module), hub_(&hub) {}
    void run(std::function<void(Hub&)> task);
    void close();

private:
    const rcs_module module_;
    std::mutex mutex_;
    std::condition_variable idle_;
    Hub* hub_;
    int busy_ = 0;
};

class Hub {
public:
    explicit Hub(rcs_module module) : module_(module), link_(std::make_shared<Link>(module, *this)) {}
    ~Hub();
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    // The module's callbacks.
    void start(rcs_role role, const rcs_clipboard_settings& settings);
    void update(const rcs_clipboard_settings& settings);
    void peerJoined(rcs_viewer_id peer, std::span<const uint8_t> hello);
    void peerHello(rcs_viewer_id peer, std::span<const uint8_t> hello);
    void peerLeft(rcs_viewer_id peer);
    void received(rcs_viewer_id peer, std::span<const uint8_t> data);
    void sent(rcs_viewer_id peer, uint64_t tag);
    void machineJoined();
    void machineLeft();
    void fromMachine(std::span<const uint8_t> data);
    void stop();

    // The host's functions; a failure's reason goes through rcs_set_last_call_error.
    rcs_result send(const rcs_clipboard_content& content);
    rcs_result peer(rcs_viewer_id viewer, rcs_clipboard_peer& out) const;
    rcs_result stats(rcs_viewer_id viewer, rcs_clipboard_stats& out) const;

    // What file transfer's callbacks tell (through the Link): this end's
    // files fetched by a peer, or withdrawn; a fetch the machine asked for
    // listed, a piece read, its end.
    void fetched(const Publication& publication, uint64_t transfer);
    void unpublished(const Publication& publication);
    void listed(uint64_t request, uint64_t transfer);
    void answered(uint64_t request, uint64_t tag, rcs_result result, Bytes data);
    void ended(uint64_t request, uint64_t transfer, rcs_result result, const std::string& message);

private:
    // This end's newest copy, and where it came from.
    struct Copy {
        uint64_t serial = 0;
        Content content;
        bool fromPeer = false;  // Placed from the other end (view): it goes nowhere.
        rcs_viewer_id from = 0; // Placed from this viewer (share): it goes to the others.
    };
    // A copy on its way to a peer, until sent() (tagged with its serial).
    struct Sending {
        uint32_t formats = 0, leftOut = 0;
        uint64_t bytes = 0;
        std::string why; // FILES left out: why.
    };
    struct Peer {
        Takes takes;
        // The copy last decided on for it (sent, kept back, too large), as it took then.
        std::optional<std::pair<uint64_t, Takes>> decided;
        std::map<uint64_t, Sending> sending;
    };
    struct Placing {
        Content content;
        rcs_viewer_id from = 0;
    };
    // A copy of this end's whose files a peer may fetch.
    struct Published {
        rcs_viewer_id peer = 0;
        uint64_t serial = 0;
        bool cut = false;
    };
    // A copy of a peer's placed here with its files: what pastes of it fetch.
    struct StandIn {
        rcs_viewer_id from = 0;
        Publication publication{};
        bool cut = false;
    };
    // A fetch the machine half asked for.
    struct Fetching {
        uint64_t transfer = 0;
        uint64_t placing = 0;
        rcs_file_fetch_mode mode = RCS_FILE_FETCH_READ;
        bool ahead = false; // Nobody pastes: the copy's files fetched as they come (files_prefetch_bytes).
    };

    void copied(Content content, bool existing);
    void readFailed(rcs_result result, const std::string& message);
    void placed(uint64_t id, rcs_result result, const std::string& message);
    void arrived(Content content, rcs_viewer_id peer);
    void withdrawn(rcs_viewer_id peer, const Publication& publication);
    // This end's newest copy is `copy` now: what older ones published goes.
    void latest(const Copy& copy);
    // Offers `copy` to the peers that take copies (`only`: that one), asking
    // the host first (`ask`); one that has it as it takes it now, or for
    // which it was decided so, is left out.
    void offer(const Copy& copy, rcs_viewer_id only, bool ask);
    // Publishes `files` of `copy` to `peer`; nullopt, with why, where they cannot go.
    std::optional<Publication> publish(rcs_viewer_id peer, const Copy& copy, const Files& files, std::string& why);
    // Withdraws what copies of this end published (all, or those not of `keep`).
    void withdraw(std::optional<uint64_t> keep = std::nullopt);
    // A system paste asks for a fetch of a copy placed here.
    void want(const WantMessage& want);
    // on_clipboard; true without one.
    bool askHost(const Content& content, rcs_clipboard_flow flow, rcs_viewer_id viewer);
    // Reports a copy that went, or could not go or be placed (RCS_CLIPBOARD_COPY).
    void report(rcs_clipboard_flow flow, rcs_result result, rcs_viewer_id viewer, uint32_t formats, uint32_t leftOut,
                uint64_t bytes, const std::string& message);
    // Reports a fetch of a copy's files (RCS_CLIPBOARD_PASTE).
    void reportPaste(rcs_clipboard_flow flow, rcs_viewer_id viewer, rcs_result result, uint64_t transfer, bool cut,
                     bool ahead, const std::string& message);
    void peerTakes(rcs_viewer_id peer, std::span<const uint8_t> hello);
    // What this end takes now (its hello): FILES only where it can take them.
    Takes takes() const;
    void tellTakes();
    // The machine half watches and places as the settings and the peers say now.
    void useMachine(bool always = false);
    void toMachine(const ToMachine& message);
    rcs_clipboard_stats& count(rcs_viewer_id peer);
    rcs_session session() const { return rcs_module_session(module_); }

    const rcs_module module_;
    const std::shared_ptr<Link> link_;
    rcs_role role_ = RCS_ROLE_VIEW;
    rcs_clipboard_settings settings_ = RCS_CLIPBOARD_SETTINGS_INIT;
    bool running_ = false;
    bool fileTransfer_ = false; // The session uses rcsdk.file-transfer.
    std::optional<Takes> told_;  // The hello set.
    std::map<rcs_viewer_id, Peer> peers_; // Those it is active with.
    std::optional<Copy> latest_;
    uint64_t serial_ = 0; // Of copies, and of copies placed.
    std::map<uint64_t, Placing> placing_;
    bool machine_ = false;   // The machine half runs...
    std::optional<Use> used_; // ...and was told this...
    uint32_t machineFiles_ = 0; // ...and told what its clipboard does with files.
    std::map<Publication, Published> published_;
    std::map<uint64_t, StandIn> standIns_; // By placing; the newest kept.
    std::map<uint64_t, Fetching> fetches_; // By the machine's request.
    // The run's counters: a viewer's own (a sharer's, by id; those that
    // left, the newest departedKept), and the session's (0).
    static constexpr size_t departedKept = 256;
    std::map<rcs_viewer_id, rcs_clipboard_stats> stats_;
    std::deque<rcs_viewer_id> departed_;
};

} // namespace rcsdk::clipboard
