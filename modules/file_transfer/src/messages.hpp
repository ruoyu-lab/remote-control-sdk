// The module's own messages (version 3 of "rcsdk.file-transfer"),
// little-endian. Whatever comes is checked: the other end, and a sharer's
// agent of another process, may be anyone's. Decoding throws
// Error(PROTOCOL).
//
// Every message names its transfer by the number its sending end gave it
// (the sender's rcs_file_send number, or the one it gave a fetch of its
// publication); CANCEL and PAUSE, which either end sends, say whose number
// it is. Before a fetch's OFFER came, its fetcher's number names it (FETCH,
// and a CANCEL of it either way).
#pragma once

#include "common.hpp"

#include <utility>
#include <variant>

namespace rcsdk::file_transfer {

// An item as offered.
struct ItemSpec {
    rcs_file_kind kind = RCS_FILE_REGULAR;
    uint64_t size = 0;
    int64_t modified = 0; // Nanoseconds since 1970 UTC; 0: none.
    std::string path;     // Relative (paths.hpp).
    bool operator==(const ItemSpec&) const = default;
};

// The bytes of `items`' files; throws Error(PROTOCOL) where they overflow.
uint64_t totalBytes(const std::vector<ItemSpec>& items);

// ---- To the other end -------------------------------------------------------------

// The hello: what this end allows, the run it is (the same end coming back
// within a run says the same), and its limits.
struct Hello {
    rcs_file_flow flow = 0;
    Token end{};
    uint32_t maxItems = 0;
    uint64_t maxOfferBytes = 0;
    bool operator==(const Hello&) const = default;
};
Bytes encode(const Hello& hello);
Hello decodeHello(std::span<const uint8_t> bytes);

// The sender's: its items. Again (after the peer came back): the receiver
// answers with where the transfer is, or treats it as new where it never
// had it; `known` is the first item whose outcome the sender has not heard.
// A fetch's (`fetch`: the fetcher's number, with its mode): INTO is accepted
// as it comes; READ goes at once, with no answer: the fetcher reads.
struct Offer {
    uint64_t id = 0;
    bool again = false;
    bool resuming = false;
    bool paused = false; // The sender paused it.
    Token token{};
    uint32_t known = 0;
    uint64_t fetch = 0;
    rcs_file_fetch_mode mode = 0; // A fetch's: INTO or READ.
    std::vector<ItemSpec> items;
};
// The fetcher's: a transfer of publication `publication`, its number `id`.
struct Fetch {
    uint64_t id = 0;
    Token publication{};
    rcs_file_fetch_mode mode = RCS_FILE_FETCH_INTO;
};
// The fetcher's, of a READ fetch: `length` bytes (1..RCS_FILE_READ_MAX, not
// past its end) of a file at `offset`; `serial` names the read.
struct Read {
    uint64_t id = 0;
    uint64_t serial = 0;
    uint32_t item = 0;
    uint64_t offset = 0;
    uint32_t length = 0;
};
// The sender's answer to a read: all its bytes (borrowed from the message),
// or why not.
struct ReadData {
    uint64_t id = 0;
    uint64_t serial = 0;
    rcs_result result = RCS_OK;
    std::string message;
    std::span<const uint8_t> data;
};
// The receiver's, as it accepts and whenever the sender must go on from
// elsewhere (the peer or the receiving machine came back): what to send,
// from the `epoch` on (older chunks are dropped). Items below `first` need
// nothing; of the others, those in `starts` begin at its offset (skipAll:
// nothing to send), the rest at 0.
struct Answer {
    static constexpr uint64_t skipAll = ~uint64_t(0);
    uint64_t id = 0;
    uint32_t epoch = 0;
    bool paused = false; // The receiver paused it.
    uint32_t first = 0;
    std::vector<std::pair<uint32_t, uint64_t>> starts; // Items in increasing order.
};
// The sender's: bytes of a file at `offset` (borrowed from the message).
struct Chunk {
    uint64_t id = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    uint64_t offset = 0;
    std::span<const uint8_t> data;
};
// The sender's: all of an item went (RCS_OK), or it could not be read.
struct ItemEnd {
    uint64_t id = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    rcs_result result = RCS_OK;
    std::string message;
};
// The receiver's: the item is written up to `written`.
struct Ack {
    uint64_t id = 0;
    uint32_t item = 0;
    uint64_t written = 0;
};
// The receiver's: an item's outcome (COMPLETED, SKIPPED or FAILED);
// `earlier`: an earlier run of the transfer completed it.
struct ItemDone {
    uint64_t id = 0;
    uint32_t item = 0;
    rcs_file_item_state state = RCS_FILE_ITEM_COMPLETED;
    rcs_result result = RCS_OK;
    std::string message;
    bool earlier = false;
};
// Either end's: the transfer ended there (REJECTED by the receiver,
// CANCELLED, FAILED), or the receiver's answer to an offer of one it
// completed before the sender heard, and of a READ fetch it closed
// (COMPLETED, RCS_OK). `sending`: the message's sender sends the files (the
// id is its own). `fetch`: the id is the fetcher's number of a FETCH,
// refused or ended by the sender before it offered it (`sending`), or
// ended by the fetcher before its OFFER came.
struct Cancel {
    bool sending = false;
    bool fetch = false;
    uint64_t id = 0;
    rcs_file_end end = RCS_FILE_CANCELLED;
    rcs_result result = RCS_ERROR_CANCELLED;
    std::string message;
};
struct Pause {
    bool sending = false;
    uint64_t id = 0;
    bool paused = false;
};
using PeerMessage = std::variant<Offer, Answer, Chunk, ItemEnd, Ack, ItemDone, Cancel, Pause, Fetch, Read, ReadData>;
Bytes encode(const PeerMessage& message);
PeerMessage decodePeer(std::span<const uint8_t> bytes);

// ---- Between the halves -------------------------------------------------------------

// The session's half asks the machine's (in order); paths are the machine's.
// Lists what to send: each entry a source and the name it goes as.
struct ListRequest {
    uint64_t job = 0;
    uint32_t maxItems = 0;
    std::vector<std::pair<std::string, std::string>> entries;
};
// Checks a directory for a transfer accepted into it. Only one that resumes
// (its offer said so, as its host saw) goes on from what a transfer of its
// token kept there; any other fails where something of its token is there.
struct PrepareRequest {
    uint64_t transfer = 0;
    std::string directory;
    rcs_file_conflict conflict = RCS_FILE_CONFLICT_RENAME;
    Token token{};
    bool resuming = false;
    bool keep = false;
    std::vector<ItemSpec> items;
};
// A piece of a file: below `base` (a directory the host's path leads to),
// `relative` ('/'-separated names on the machine) is opened a component at
// a time, never through a link. Messages call it `name`, its path as it
// goes (the machine's paths and names stay here).
struct ReadRequest {
    uint64_t transfer = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    std::string name;
    std::string base;
    std::string relative;
    uint64_t size = 0; // As listed: a file that changed fails.
    int64_t modified = 0;
    uint64_t offset = 0;
    uint32_t length = 0;
};
struct WriteRequest {
    uint64_t transfer = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    std::string directory;
    ItemSpec spec;
    Token token{};
    bool keep = false;
    // The item's first piece of the transfer: at offset 0 its partial file
    // is made, and nothing may be there. Else, at 0 too, the transfer wrote
    // it before (a machine's half that left did).
    bool fresh = false;
    uint64_t offset = 0;
    Bytes data;
};
// An item whose bytes all came: it takes its name (a directory is made).
struct FinishRequest {
    uint64_t transfer = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    std::string directory;
    ItemSpec spec;
    rcs_file_conflict conflict = RCS_FILE_CONFLICT_RENAME;
    Token token{};
    bool keep = false;
};
// These items' partial files go: an item that failed, or (`ended`) the
// unfinished ones of a transfer that ended, whose record goes too.
struct DiscardRequest {
    uint64_t transfer = 0;
    std::string directory;
    Token token{};
    bool ended = true;
    std::vector<std::string> paths;
};
// A transfer that completed: its directories get their times (deepest
// first), its record goes.
struct CompleteRequest {
    uint64_t transfer = 0;
    std::string directory;
    Token token{};
    std::vector<std::pair<std::string, int64_t>> directories;
};
// The end of a transfer that asks nothing more of its files (sent, kept for
// another run, none written yet; any with HOST): they close.
struct CloseRequest {
    uint64_t transfer = 0;
};
// rcs_file_discard: what a transfer kept in a directory goes.
struct PurgeRequest {
    uint64_t job = 0;
    std::string directory;
    Token token{};
};
using Request = std::variant<ListRequest, PrepareRequest, ReadRequest, WriteRequest, FinishRequest, DiscardRequest,
                             CompleteRequest, CloseRequest, PurgeRequest>;
Bytes encode(const Request& request);
Request decodeRequest(std::span<const uint8_t> bytes);

// The machine's half answers.
struct Listed {
    uint64_t job = 0;
    rcs_result result = RCS_OK;
    std::string message;
    std::vector<ItemSpec> items;
    // Where each file is read (ReadRequest): the directory of its entry (the
    // entry itself, or a file's own) and the names below it. One base per
    // entry listed; one source per item, empty for what is no file.
    std::vector<std::string> bases;
    std::vector<std::pair<uint32_t, std::string>> sources;
};
// How an item of a transfer accepted starts.
struct Start {
    enum What : uint8_t { resume = 1, done = 2, skip = 3, failed = 4 };
    uint32_t item = 0;
    What what = resume;
    uint64_t offset = 0;        // resume: what an earlier run wrote.
    rcs_result result = RCS_OK; // failed: why.
    std::string text;           // done: where it is; skip, failed: why.
};
struct Prepared {
    uint64_t transfer = 0;
    rcs_result result = RCS_OK;
    std::string message;
    std::vector<Start> starts; // Items in increasing order.
};
struct Data {
    uint64_t transfer = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    uint64_t offset = 0;
    Bytes data;
};
struct ReadFailed {
    uint64_t transfer = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    rcs_result result = RCS_ERROR_DEVICE;
    std::string message;
};
struct Written {
    uint64_t transfer = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    uint64_t offset = 0; // Written up to here.
};
struct WriteFailed {
    uint64_t transfer = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    rcs_result result = RCS_ERROR_DEVICE;
    std::string message;
};
struct Finished {
    uint64_t transfer = 0;
    uint32_t epoch = 0;
    uint32_t item = 0;
    rcs_file_item_state state = RCS_FILE_ITEM_COMPLETED; // COMPLETED, SKIPPED or FAILED.
    rcs_result result = RCS_OK;
    std::string message;
    std::string saved; // COMPLETED: where it is, relative.
    bool whole = false; // FAILED: the whole transfer fails with it (RCS_FILE_CONFLICT_FAIL).
};
// The end of a transfer is done (COMPLETE, CLOSE, DISCARD `ended`), after
// all asked of it before: nothing of it is open or still to do here.
struct Ended {
    uint64_t transfer = 0;
};
struct Purged {
    uint64_t job = 0;
    rcs_result result = RCS_OK;
    std::string message;
    uint32_t files = 0;
};
using Reply = std::variant<Listed, Prepared, Data, ReadFailed, Written, WriteFailed, Finished, Ended, Purged>;
Bytes encode(const Reply& reply);
Reply decodeReply(std::span<const uint8_t> bytes);

} // namespace rcsdk::file_transfer
