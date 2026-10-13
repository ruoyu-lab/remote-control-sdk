// The module's own messages (version 2 of "rcsdk.clipboard"). Whatever comes
// is checked: the other end, and a sharer's agent of another process, may be
// anyone's. Decoding throws Error(PROTOCOL).
#pragma once

#include "content.hpp"
#include "watcher.hpp"

#include <variant>

namespace rcsdk::clipboard {

inline constexpr uint32_t moduleVersion = 1;

// ---- To the other end -------------------------------------------------------------

// The hello: what this end takes (formats 0 and maxBytes 0: nothing).
struct Takes {
    uint32_t formats = 0, maxBytes = 0;
    bool operator==(const Takes&) const = default;
};
Bytes encodeTakes(const Takes& takes);
Takes decodeTakes(std::span<const uint8_t> bytes);
// A copy, fitted to what the other end takes (its files: this end's
// publication of them to that end); or a publication withdrawn while its
// copy may still be on the other end's clipboard.
struct CopyMessage {
    Content content;
};
struct WithdrawnMessage {
    Publication publication{};
};
using PeerMessage = std::variant<CopyMessage, WithdrawnMessage>;
Bytes encode(const PeerMessage& message);
PeerMessage decodePeer(std::span<const uint8_t> bytes);

// ---- Between the halves -------------------------------------------------------------

// A file of a fetch as file transfer lists it: its relative path.
struct FileItem {
    std::string path;
    rcs_file_kind kind = RCS_FILE_REGULAR;
    uint64_t size = 0;
    int64_t modified = 0;
    bool operator==(const FileItem&) const = default;
};

// Why files a fetch ahead lists are not fetched ahead: more than files, or
// more than `limit` bytes (files_prefetch_bytes) together; empty when they
// are. The copy said what its files were, its publication says what comes:
// the other end's files changed since, or it said less than it sends.
std::string prefetchProblem(std::span<const FileItem> listed, uint64_t limit);

// The session's half to the machine's: watch and place as `use` says, or
// neither (nullopt); place a copy (`ahead`: its files are fetched into the
// files directory as it is placed); what came of a fetch the machine asked
// for (its items, a read, its end); a copy's files withdrawn.
struct UseMessage {
    std::optional<Use> use;
};
struct PlaceMessage {
    uint64_t id = 0;
    Content content;
    bool ahead = false;
};
struct ListedMessage {
    uint64_t request = 0;
    std::vector<FileItem> items;
};
struct DataMessage {
    uint64_t request = 0;
    uint64_t tag = 0;
    rcs_result result = RCS_OK;
    Bytes data;
};
struct EndedMessage {
    uint64_t request = 0;
    rcs_result result = RCS_OK;
    std::string message;
    bool complete = false; // Every item came whole.
};
struct RevokedMessage {
    uint64_t placing = 0;
};
using ToMachine =
    std::variant<UseMessage, PlaceMessage, ListedMessage, DataMessage, EndedMessage, RevokedMessage>;
Bytes encode(const ToMachine& message);
ToMachine decodeToMachine(std::span<const uint8_t> bytes);

// The machine's half to the session's: a new copy (Watcher's), why the
// clipboard could not be read, what became of a copy placed; what this
// process's clipboard does with files; what a system paste of a copy's
// files asks for: a fetch (READ, or INTO a directory: one the paste chose,
// or the files directory for `ahead`), a piece of it, its end; a paste that
// could not start.
struct CopiedMessage {
    bool existing = false;
    Content content; // May be empty; leftOut travels; its files' paths too.
};
struct ReadFailedMessage {
    rcs_result result = RCS_ERROR_DEVICE;
    std::string message;
};
struct PlacedMessage {
    uint64_t id = 0;
    rcs_result result = RCS_OK;
    std::string message;
};
struct AbilitiesMessage {
    uint32_t files = 0; // RCS_CLIPBOARD_SEND, RCS_CLIPBOARD_RECEIVE.
    std::string why;
};
struct WantMessage {
    uint64_t request = 0; // The machine's number for the fetch.
    uint64_t placing = 0;
    rcs_file_fetch_mode mode = RCS_FILE_FETCH_READ;
    std::string directory; // INTO.
    bool ahead = false;
};
struct ReadMessage {
    uint64_t request = 0;
    uint32_t item = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
    uint64_t tag = 0;
};
struct CloseMessage {
    uint64_t request = 0;
    rcs_result outcome = RCS_OK;
};
struct RefusedMessage {
    uint64_t placing = 0;
    rcs_result result = RCS_ERROR_UNSUPPORTED;
    std::string message;
};
using ToSession = std::variant<CopiedMessage, ReadFailedMessage, PlacedMessage, AbilitiesMessage, WantMessage,
                               ReadMessage, CloseMessage, RefusedMessage>;
Bytes encode(const ToSession& message);
ToSession decodeToSession(std::span<const uint8_t> bytes);

// The most a message of the module holds: a copy and its framing (between
// the halves, a copy's paths, a fetch's items or a piece read beside it).
inline constexpr size_t maxMessageBytes = maxWireBytes + 64;

} // namespace rcsdk::clipboard
