// The module's session half: the transfers of a session, with each peer
// (messages.hpp, "To the other end") and with what reads and writes the
// bytes (the machine's half for SYSTEM, the host's callbacks for HOST:
// "Between the halves").
//
// Sending, items go in their order, each in pieces of 256 KiB read ahead
// while at most 4 MiB of the transfer are read, queued, on their way or
// not yet written at the other end (its ACKs say what is written); a piece
// that comes back from the store goes to the peer in the order asked
// (steps), then the item's ITEM_END. Receiving, a piece must be the next of
// the item expected, in the answer's epoch: anything else breaks the rules.
// When the peer or the store leaves and comes back, the receiver answers
// again with a new epoch and where each item is, and the sender goes on
// from there; chunks of older epochs are dropped.
//
// What either end handed its store stays counted until the store answers
// it, whatever the peer says meanwhile: a sender's reads (an ANSWER drops
// them, they still count against its window until they come back), a
// receiver's writes (it answers an OFFER again, or a piece past the window,
// with a new epoch only once its store wrote all it had, and drops what
// comes before that answer). So neither end holds more than about a window
// of a transfer in memory, however its peer behaves.
//
// A transfer that ends tells its store, which had something of it, what the
// end asks (closing its files; receiving with SYSTEM, completing what it
// wrote or discarding it), after all asked before; the host learns that it
// ended (RCS_FILE_DONE, FINISHED) once the store is through with it, or is
// gone (the machine's half left, the run stopped).
//
// Publications: what this end lets a peer fetch later (FETCH) is a sending
// transfer then, listed anew and OFFERED with the fetcher's number. An INTO
// fetch goes on as any transfer, accepted at the fetching end as its offer
// comes; a READ fetch goes at once: the fetcher asks for pieces (READ), the
// sender reads them from the store in the order asked and answers each
// (DATA), at most 16 MiB of them not answered, until the fetcher closes it.
//
// Every function runs as one of the module's calls (the SDK's callbacks,
// rcs_module_run for the host's functions and the module's own threads),
// so none overlaps another. What the hosts' and other modules' callbacks of
// publications and fetches are told goes to a Notifier: they come on its
// thread, outside the module's calls, and may call its functions.
#pragma once

#include "messages.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace rcsdk::file_transfer {

using Clock = std::chrono::steady_clock;
class Hub;

// Calls Hub::tick (rcs_module_run) every 50 ms while the hub wants it: offers
// whose time ran out, peers that did not come back, the rate limit, progress.
class Ticker {
public:
    Ticker(rcs_module module, Hub& hub);
    ~Ticker();
    void want(bool wanted);
    // Its thread ends (a tick under way finishes first).
    void stop();

private:
    void run();

    const rcs_module module_;
    Hub& hub_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool wanted_ = false, quit_ = false;
    std::thread thread_; // Last: it starts once the rest exists.
};

// Runs what callbacks of publications and fetches are told, on a thread of
// its own, one at a time and in order, until it goes (what is left runs
// first).
class Notifier {
public:
    Notifier();
    ~Notifier();
    void post(std::function<void()> call);

private:
    void run();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> calls_;
    bool quit_ = false;
    std::thread thread_; // Last.
};

// target HOST: a transfer's callbacks, and what they are told of it.
struct HostContext {
    rcs_file_transfer_settings settings = RCS_FILE_TRANSFER_SETTINGS_INIT;
    uint64_t transfer = 0;
    Token token{};
    std::vector<ItemSpec> items;
};

// target HOST: the host's callbacks do what the machine's half does with
// files, one request at a time on a thread of the module's; their answers go
// to Hub::hostReply (rcs_module_run).
class HostStore {
public:
    HostStore(rcs_module module, Hub& hub);
    ~HostStore();
    void post(uint64_t run, Request request, std::shared_ptr<const HostContext> context, rcs_viewer_id viewer);

private:
    struct Job {
        uint64_t run = 0;
        Request request;
        std::shared_ptr<const HostContext> context;
        rcs_viewer_id viewer = 0;
    };
    void work();
    static std::optional<Reply> handle(const Job& job);

    const rcs_module module_;
    Hub& hub_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool quit_ = false;
    std::thread thread_; // Last.
};

class Hub {
public:
    explicit Hub(rcs_module module) : module_(module) {}
    ~Hub();
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    // The module's callbacks.
    void start(rcs_role role, const rcs_file_transfer_settings& settings);
    void update(const rcs_file_transfer_settings& settings);
    void peerJoined(rcs_viewer_id peer, std::span<const uint8_t> hello);
    void peerHello(rcs_viewer_id peer, std::span<const uint8_t> hello);
    void peerLeft(rcs_viewer_id peer);
    void received(rcs_viewer_id peer, std::span<const uint8_t> data);
    void room(rcs_viewer_id peer);
    void machineJoined();
    void machineLeft();
    void fromMachine(std::span<const uint8_t> data);
    void stop();
    // The module's threads.
    void tick();
    void hostReply(uint64_t run, Reply reply);

    // The host's functions; a failure's reason goes through rcs_set_last_call_error.
    rcs_result send(const rcs_file_offer& offer, uint64_t& transfer);
    rcs_result accept(uint64_t transfer, const char* directory, rcs_file_conflict conflict);
    rcs_result reject(uint64_t transfer, const char* reason);
    rcs_result cancel(uint64_t transfer);
    rcs_result pause(uint64_t transfer, uint32_t paused);
    rcs_result info(uint64_t transfer, rcs_file_transfer_info& out) const;
    rcs_result items(uint64_t transfer, uint32_t first, rcs_file_item_info* out, uint32_t capacity,
                     uint32_t& count) const;
    rcs_result peer(rcs_viewer_id viewer, rcs_file_peer& out) const;
    rcs_result discard(const char* directory, const uint8_t* token);
    rcs_result publish(const rcs_file_publication& publication, uint8_t* id);
    rcs_result unpublish(const uint8_t* id, uint32_t flags);
    rcs_result fetch(const rcs_file_fetch_request& request, uint64_t& transfer);
    rcs_result read(uint64_t transfer, uint32_t item, uint64_t offset, uint32_t size, uint64_t tag);
    rcs_result close(uint64_t transfer, rcs_result outcome, const char* reason);

private:
    enum class Phase { preparing, offered, going, finished };
    struct Item {
        ItemSpec spec;
        // Send, SYSTEM: where it is on the machine (bases[entry], then these names).
        uint32_t entry = 0;
        std::string relative;
        rcs_file_item_state state = RCS_FILE_ITEM_PENDING;
        rcs_result result = RCS_OK;
        std::string message, saved;
        uint64_t done = 0;     // Written (send: as the other end acknowledged), an earlier run's included.
        uint64_t start = 0;    // Send: where the receiver has it go on from. Receive: what an earlier run wrote.
        uint64_t received = 0; // Receive: handed to the store.
        bool begun = false;    // Receive: a piece of it went to a store (its partial file is this transfer's).
        bool ended = false;    // Receive: its ITEM_END came, it is being finished.
        bool skip = false;     // Send: the receiver needs nothing more of it.
        bool endSent = false;  // Send: its ITEM_END is on its way (this epoch).
        bool failedHere = false; // Send: it could not be read; the receiver learns it from its ITEM_END.
        bool earlier = false;    // Receive: an earlier run completed it.
        bool finished() const noexcept { return state >= RCS_FILE_ITEM_COMPLETED; }
    };
    // Send: a piece asked of the store, or an item's end, in the order they go.
    struct Step {
        uint32_t item = 0;
        uint64_t offset = 0;
        uint32_t length = 0; // 0: the item's ITEM_END.
        rcs_result result = RCS_OK;
        std::string message;
        bool dropped = false; // Its item ended, or an answer came, meanwhile: it does not go (but is waited for).
    };
    struct Unacked {
        uint32_t item = 0;
        uint64_t end = 0;
        uint32_t length = 0;
    };
    // A read of a READ fetch, not answered yet: receive, asked of the peer;
    // send, given to the store (in the order asked).
    struct Piece {
        uint64_t serial = 0;
        uint64_t tag = 0; // Receive: the host's.
        uint32_t item = 0;
        uint64_t offset = 0;
        uint32_t length = 0;
        bool asked = false; // Receive: sent to the peer; send: given to the store.
    };
    // What a fetching host is told (rcs_file_fetch_request).
    struct Fetcher {
        void (*onListed)(uint64_t transfer, void* userData) = nullptr;
        void (*onRead)(uint64_t transfer, uint64_t tag, rcs_result result, const uint8_t* data, uint32_t size,
                       void* userData) = nullptr;
        void (*onEnded)(uint64_t transfer, rcs_result result, const char* message, void* userData) = nullptr;
        void* userData = nullptr;
    };
    struct Transfer {
        uint64_t id = 0;   // This end's number.
        uint64_t wire = 0; // The sender's (send: id).
        bool sending = false;
        rcs_viewer_id peer = 0;
        Token peerEnd{}; // The peer's run (its hello).
        bool present = false;
        Clock::time_point absentSince;
        Token token{};
        bool resuming = false;
        rcs_file_target target = RCS_FILE_SYSTEM;
        std::shared_ptr<const HostContext> host;
        Phase phase = Phase::preparing;
        bool pausedHere = false, pausedThere = false;
        std::vector<Item> items;
        uint32_t finishedItems = 0; // Completed, skipped or failed.
        uint64_t total = 0;
        std::optional<ListRequest> listing; // Send, SYSTEM, preparing.
        std::vector<std::string> bases;     // Send, SYSTEM: where its entries' files are read.
        // A fetch of a publication: send, a peer's of this end's; receive, this end's.
        bool fetched = false;
        rcs_file_fetch_mode mode = 0;
        Token publication{};
        uint64_t fetchId = 0;    // The fetcher's number.
        bool awaiting = false;   // Receive: its OFFER has not come (wire is not known).
        bool offered = false;    // Send: its OFFER went.
        Fetcher fetcher;         // Receive.
        std::deque<Piece> serving;        // Send, READ.
        std::map<uint64_t, Piece> pieces; // Receive, READ: by serial.
        uint64_t pieceBytes = 0;          // Of serving or pieces.
        uint64_t pieceSerial = 0;
        // Receive.
        std::string directory;
        rcs_file_conflict conflict = RCS_FILE_CONFLICT_RENAME;
        bool keep = false;
        Clock::time_point deadline; // Offered: the host decides by then.
        uint32_t epoch = 0;         // Of the newest answer; send: the one chunks carry.
        uint32_t expected = 0;      // Receive: the item whose bytes come next.
        // Receive: the pieces handed to the store and not answered yet (it
        // answers each in order), and their bytes.
        std::deque<uint32_t> pending;
        uint64_t unwritten = 0;
        bool answerDue = false; // Receive: the sender learns where to go on from, once the store wrote what it had.
        // Send.
        bool synced = false; // An answer of this connection came.
        uint32_t cursor = 0;
        uint64_t cursorOffset = 0;
        std::deque<Step> steps;
        std::deque<Unacked> unacked;
        uint64_t outstanding = 0; // Bytes in unacked.
        uint32_t storeEpoch = 0;  // The store's answers to older asks are dropped.
        double allowance = 0;     // Bytes the rate limit lets go now.
        // How it ended.
        rcs_file_end end = 0;
        bool byPeer = false;
        rcs_result result = RCS_OK;
        std::string message;
        bool partialKept = false;
        bool peerTold = true; // The peer knows it ended (else it learns when it comes back).
        bool settling = false; // Its store is not through with the end yet: the host learns of it then.
        bool tidying = false;  // Receive, SYSTEM: the end sets directories' times or deletes a record.
        // Numbers.
        uint64_t resumed = 0, transferred = 0;
        std::deque<std::pair<Clock::time_point, uint64_t>> samples;
        uint64_t speed = 0;
        Clock::time_point progressAt;
        uint64_t progressDone = ~uint64_t(0);
        rcs_file_state reported = 0;
        uint32_t reportedPaused = 0;
    };
    struct Peer {
        std::optional<Hello> hello;
        struct Waiting {
            uint64_t transfer = 0;
            bool data = false; // A chunk or an item's end: dropped when the transfer starts over.
            Bytes bytes;
        };
        std::deque<Waiting> waiting; // Refused by a full queue: they go, in order, on room().
        uint64_t waitingBytes = 0;   // Of `waiting`.
    };
    // What one peer may fetch (rcs_file_publish), until it is withdrawn.
    struct Publication {
        rcs_viewer_id peer = 0;
        Token peerEnd{}; // The peer's run: the same end coming back may fetch it too.
        bool present = true;
        Clock::time_point absentSince;
        std::vector<std::pair<std::string, std::string>> entries; // SYSTEM: path and name, listed at each fetch.
        std::vector<ItemSpec> items;                              // HOST.
        uint64_t total = 0;                                       // HOST.
        void (*onFetch)(const uint8_t* publication, uint64_t transfer, void* userData) = nullptr;
        void (*onUnpublished)(const uint8_t* publication, rcs_result result, const char* message,
                              void* userData) = nullptr;
        void* userData = nullptr;
    };

    // ---- Messages ----
    void sendTo(rcs_viewer_id peer, const PeerMessage& message, uint64_t transfer = 0, bool data = false);
    void dropData(Transfer& t);
    void onOffer(rcs_viewer_id peer, Offer offer);
    void onAnswer(rcs_viewer_id peer, const Answer& answer);
    void onChunk(rcs_viewer_id peer, const Chunk& chunk);
    void onItemEnd(rcs_viewer_id peer, const ItemEnd& end);
    void onAck(rcs_viewer_id peer, const Ack& ack);
    void onItemDone(rcs_viewer_id peer, const ItemDone& done);
    void onCancel(rcs_viewer_id peer, const Cancel& cancel);
    void onPause(rcs_viewer_id peer, const Pause& pause);
    void onFetch(rcs_viewer_id peer, const Fetch& fetch);
    void onFetchOffer(rcs_viewer_id peer, Offer offer);
    void onRead(rcs_viewer_id peer, const Read& read);
    void onReadData(rcs_viewer_id peer, const ReadData& data);
    // The store's answers.
    void onReply(Reply reply);
    void onListed(Listed listed);
    void onPrepared(const Prepared& prepared);
    void onData(Data data);
    void onReadFailed(const ReadFailed& failed);
    void onWritten(const Written& written);
    void onWriteFailed(const WriteFailed& failed);
    void onFinished(const Finished& finished);
    void onEnded(const Ended& ended);
    void onPurged(const Purged& purged);

    // ---- Transfers ----
    Transfer* find(uint64_t id);
    const Transfer* find(uint64_t id) const;
    // A transfer by its sender's number at that peer: received, or this end's own.
    Transfer* incoming(rcs_viewer_id peer, uint64_t wire);
    Transfer* outgoing(rcs_viewer_id peer, uint64_t id);
    // The request goes to the transfer's store; false where none takes it
    // now, or (`refused`) why the machine's half did not take it.
    bool submit(Transfer& t, Request request, rcs_result* refused = nullptr);
    // A request of a transfer that prepares: one the machine's half does not
    // take (larger than a message) ends it, as no answer to it comes.
    void prepare(Transfer& t, Request request);
    void offer(Transfer& t, bool again);
    // Send: what the store reads of an item.
    ReadRequest readOf(const Transfer& t, uint32_t item, uint64_t offset, uint32_t length) const;
    // A fetch's items are listed: offered (READ: it goes at once).
    void offerFetched(Transfer& t);
    // What tells the peer that `t` ended, as it ended: by the fetcher's number
    // where the peer never heard (or this end never heard) the sender's.
    Cancel cancelOf(const Transfer& t) const;
    // What a host gives for items (rcs_file_send, rcs_file_publish), checked:
    // SYSTEM, paths and names to list; HOST, the items.
    rcs_result checkItems(const rcs_file_item* given, uint32_t count, const char* what,
                          std::vector<std::pair<std::string, std::string>>& entries,
                          std::vector<ItemSpec>& specs) const;
    // HOST: the bytes of `specs`, within both ends' limits and one message.
    rcs_result hostTotal(const std::vector<ItemSpec>& specs, const Hello& there, const char* what,
                         uint64_t& total) const;
    // HOST: a transfer's items, and what its callbacks are told of it (its id and token set).
    void hostItems(Transfer& t, std::vector<ItemSpec> specs);
    // A publication is withdrawn: its host learns why, fetches of it are refused.
    void revoke(const Token& id, rcs_result result, const std::string& why);
    void revokeAll(rcs_result result, const std::string& why);
    // READ: the fetcher asks the peer what it has not asked; the sender gives
    // the store what it has not given.
    void sendPieces(Transfer& t);
    void servePieces(Transfer& t);
    // READ: `length` bytes of an item at `offset` went; read to its end from
    // the start, it is COMPLETED.
    void advance(Transfer& t, uint32_t item, uint64_t offset, uint32_t length);
    // READ, closed: what was read whole is COMPLETED, directories too, the rest SKIPPED.
    void concludeReads(Transfer& t);
    // Runs on the Notifier's thread.
    void notify(std::function<void()> call);
    void answer(Transfer& t);
    // Receive: the sender goes on from what the store took, in a new epoch
    // (what comes before the answer is dropped), answered once due: the
    // store wrote all it was handed, and is there, as is the peer.
    void again(Transfer& t);
    void answerWhenDue(Transfer& t);
    // Receive: the store answered the oldest piece handed to it.
    void took(Transfer& t);
    void pump(Transfer& t);
    void flushSteps(Transfer& t);
    void pushEnd(Transfer& t, rcs_result result, const std::string& message);
    // Send: what was asked of the store and not back goes again from where it was.
    void rewind(Transfer& t);
    // Send: the item went nowhere further (its outcome came, or its read failed).
    void abandon(Transfer& t, uint32_t item);
    // Receive: what goes next; with `from` the first to look at.
    uint32_t nextExpected(const Transfer& t, uint32_t from) const;
    bool needs(const Item& item) const noexcept { return item.spec.kind != RCS_FILE_OTHER && !item.finished() && !item.ended; }
    // An item's outcome (it counts as finished).
    void conclude(Transfer& t, uint32_t item, rcs_file_item_state state, rcs_result result,
                  const std::string& message);
    // ...and reported.
    void settle(Transfer& t, uint32_t item, rcs_file_item_state state, rcs_result result, const std::string& message,
                bool tellPeer);
    void checkComplete(Transfer& t);
    void finish(Transfer& t, rcs_file_end end, rcs_result result, const std::string& message, bool byPeer,
                bool tellPeer);
    // The host learns that it ended (its store is through, or gone): RCS_FILE_DONE.
    void announce(Transfer& t);
    // What the end asked of the machine's half that it did not get to.
    void untidy(const Transfer& t);
    // The other end broke the module's rules with this transfer.
    void broken(Transfer& t, const std::string& why);
    bool needsMachine(const Transfer& t) const noexcept;
    rcs_file_state stateOf(const Transfer& t) const noexcept;
    void refresh(Transfer& t);
    void wantTicks();
    uint64_t doneBytes(const Transfer& t) const noexcept;
    uint32_t current(const Transfer& t) const noexcept;
    Hello hello() const;

    // ---- Events ----
    void post(rcs_viewer_id peer, uint32_t kind, const void* payload, size_t size);
    void emitOffer(const Transfer& t);
    void emitProgress(Transfer& t);
    void emitItem(const Transfer& t, uint32_t item);
    void emitDone(const Transfer& t);
    void emitPeer(rcs_viewer_id peer, const Hello& hello);

    const rcs_module module_;
    rcs_role role_ = RCS_ROLE_VIEW;
    rcs_file_transfer_settings settings_ = RCS_FILE_TRANSFER_SETTINGS_INIT;
    bool running_ = false;
    uint64_t run_ = 0; // Each run's: the host store's answers of older runs are dropped.
    Token end_{};      // This end's run, in its hello.
    std::map<rcs_viewer_id, Peer> peers_;
    bool machine_ = false; // The machine's half runs.
    Clock::time_point machineAwaySince_;
    Clock::time_point lastTick_;
    uint64_t serial_ = 0;
    std::map<uint64_t, Transfer> transfers_;
    std::map<std::pair<Token, uint64_t>, uint64_t> incoming_; // (the peer's run, its number): this end's.
    std::deque<uint64_t> finished_;                           // Kept, oldest first...
    size_t finishedItems_ = 0;                                // ...and their items.
    std::map<uint64_t, Token> purges_;                        // rcs_file_discard's, by job.
    std::map<Token, Publication> publications_;
    std::map<std::pair<Token, uint64_t>, uint64_t> fetches_; // (the fetcher's run, its number): this end's.
    std::unique_ptr<Notifier> notifier_;
    std::unique_ptr<Ticker> ticker_;
    std::unique_ptr<HostStore> hostStore_;
};

} // namespace rcsdk::file_transfer
