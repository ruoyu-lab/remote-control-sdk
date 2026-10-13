/* Remote Control SDK: file transfer, an official module (rcsdk/module.h).
 *
 * Files and directories from one end to the other, either way. Each end's
 * host decides what its end allows (flow: SEND, RECEIVE; nothing by
 * default), and the receiving host decides on each offer, when it likes
 * (rcs_file_accept: where to, and what to do with names already there; or
 * rcs_file_reject), before anything is written:
 *
 *     rcs_register_module(rcs_file_transfer_module());   once per process (an agent's too)
 *     rcs_file_transfer_settings settings = RCS_FILE_TRANSFER_SETTINGS_INIT;
 *     settings.flow = RCS_FILE_SEND | RCS_FILE_RECEIVE;
 *     rcs_module_config use = RCS_MODULE_CONFIG_INIT;
 *     use.module = RCS_FILE_TRANSFER_MODULE;
 *     use.settings = &settings;
 *     use.size = sizeof(settings);                         chained in the session's extensions
 *
 * Both ends use it (RCS_EVENT_MODULE says where the other end does not). A
 * transfer goes to one peer: a sharer's to one viewer, a viewer's to its
 * sharer. The bytes go over the session's encrypted connection in 256 KiB
 * chunks, beside the picture and never ahead of it (module messages yield
 * to video), and at most 4 MiB of each transfer are on their way (read,
 * queued, sent, not yet written at the other end) at a time, so a large
 * file takes neither memory nor the host's data or other modules' turn.
 * That holds whatever the other end sends: what an end asked its machine
 * (or its host's callbacks) to read or write counts until it is done, and
 * the other end offering or answering again does not start it over.
 *
 * target:
 *   SYSTEM  The module reads and writes files on the machine: a sharer's
 *           where its agent runs (in another process, another account, with
 *           rcs_agent_link_config: that process registers the module too; it
 *           reads and writes with that account's rights), a viewer's in its
 *           own process. Paths are that machine's, UTF-8.
 *   HOST    The module touches no file: the host names each item (name,
 *           kind, size, time) and gives its bytes in `read`; what arrives
 *           goes to `write` (the host's own storage, a database, memory).
 *
 * A transfer, at the sending end: rcs_file_send lists the items (SYSTEM: a
 * directory with all it holds, on the machine) and offers them; the other
 * end's host sees RCS_FILE_OFFER (rcs_file_get_items lists them) and
 * accepts or rejects; no decision within offer_timeout_ms rejects it
 * (TIMEOUT). An offer past that end's limits (max_items, max_offer_bytes,
 * and the budget of offers waiting for its host: max_pending_*) is refused
 * there at once, its host never seeing it. Then the items go one after the
 * other, in their order:
 *   - Receiving with SYSTEM, each file goes into "<name>.<token>.rcsdk-part"
 *     beside where it belongs (<token>: the transfer's, 32 hex digits, so
 *     transfers of the same name never share one; <name> and the 44
 *     characters after it must fit the file system's limit on a name, else
 *     the item fails with the system's reason), made new, and takes its
 *     name once whole, with its modification time (directories get theirs
 *     at the end; permissions and other attributes are not carried: what is
 *     made gets what anything this process makes gets, its umask on macOS
 *     and Android, the directory's inherited access on Windows). Anything
 *     in that name the transfer did not make fails the item, never written
 *     over or through. A name already there is the host's choice
 *     (on_conflict); a directory there takes what goes in it. Symbolic
 *     links are never followed: one in the way of a path fails that item.
 *     Everything is made, written, moved and deleted through handles of the
 *     directories it opened (below the host's directory), not by paths
 *     looked up again: another process that can write in the tree and swaps
 *     a directory for a link (a junction on Windows) while a file goes in
 *     cannot send it outside. The host's own directory path is resolved for
 *     each request as it is (links in it are followed: the host chose it).
 *     Other accounts that can write in that directory can still change what
 *     is in it, partial files and records too (even with the sticky bit,
 *     they can put files of their own in the names a transfer uses next):
 *     receive into a directory only this account can write. The free space
 *     of the directory's volume is checked before the transfer starts, less
 *     what the transfers this end's machine is receiving into that volume
 *     still have to write.
 *   - A symbolic link (or anything but a file or a directory) at the
 *     sending end is listed (RCS_FILE_OTHER) and skipped, not followed. A
 *     directory is listed and its files read through the handles of the
 *     directories below the host's path, as received ones are written: one
 *     swapped for a link after it was looked at goes as RCS_FILE_OTHER, and
 *     nothing behind the link is listed or read. A source that became a
 *     pipe (or a device) after it was listed fails its item
 *     (INVALID_STATE); the module never waits on it.
 *   - Reasons (an item's, a transfer's, what the log says) name files by
 *     their paths as items go, never by the machine's paths: the other end
 *     hears them too, and logs go where hosts send them. The host knows
 *     where its directory and its sources are.
 * Either end may cancel or pause it; a host's decisions are functions, never
 * callbacks, so it may ask its user first, or decide in on_event: the events
 * a function raises reach on_event after it returned, from a thread of the
 * module's. Files that arrive are never opened or run.
 *
 * Connections: when the connection to the peer ends, its transfers wait
 * (RCS_FILE_WAITING) for the same end to come back within the run (a viewer
 * that reconnects, rcs_reconnect_config) for reconnect_wait_ms, and then
 * go on from what is written. One that does not come back, or a run that
 * ends, ends them (FAILED, DISCONNECTED).
 *
 * Another run (resume): a transfer's token (rcs_file_transfer_info.token)
 * names it across runs. The sending host that keeps it (and its items) can
 * offer the same items again with it (rcs_file_offer.resume_token); the
 * receiving host accepts that offer (RCS_FILE_OFFER says resuming, with the
 * token) into the same directory, and what an earlier run wrote is kept:
 * whole files are done, partial ones go on from where they stopped (where
 * the record says the transfer started them, each a file of its own: no
 * link, no other name), while anything that changed at the sending end (its
 * size or time) starts over. Only an offer that says it resumes goes on
 * from what a transfer of its token kept: any other whose token names a
 * record or partial files there fails (INVALID_STATE) and takes or deletes
 * nothing. A token is the sending end's (random, unless its host gives an
 * earlier one): it names a transfer, it proves nothing about who offers it
 * again, and the module does not know which peer started the one it names.
 * A host accepts a resuming offer into the directory of an earlier transfer
 * only from the peer it took that transfer from (as its own control of who
 * is who says), since what that one wrote is continued.
 * For that a receiving end with keep_partial 1 (SYSTEM) keeps, in the
 * directory, the partial files of a transfer the connection or the run
 * ended, and a record of the transfer, ".rcsdk-resume-<token in hex>";
 * they stay until the transfer completes, is cancelled or fails otherwise
 * (gone by its RCS_FILE_DONE), or the host discards them
 * (rcs_file_discard). With HOST, the host keeps what it wrote and says how
 * much in `resume`.
 *
 * Publications (fetched later): rcs_file_publish names items one peer may
 * fetch, and sends nothing; its host (or a module: the clipboard's copies of
 * files, rcsdk/clipboard.h) hands that peer the publication's id its own way.
 * The peer's rcs_file_fetch then starts a transfer from this end to it of the
 * items as they are then (SYSTEM: listed on the machine at each fetch), with
 * no RCS_FILE_OFFER at either end: publishing was this end's decision,
 * fetching the other's. A fetch goes INTO a directory, written as a transfer
 * received is, or is READ: nothing is written, and the fetching host reads
 * any piece of its files when it likes (rcs_file_read), e.g. for a system
 * paste that asks for a file's bytes as it writes them, until it closes it
 * (rcs_file_close). Either end may pause or cancel a fetch as any transfer;
 * each end follows it with the usual events (rcs_file_transfer_info.origin
 * FETCHED). A publication is withdrawn by its host (rcs_file_unpublish), when
 * this end stops sending or changes its target, when its peer does not come
 * back within reconnect_wait_ms, and when the run ends: fetches of it are
 * refused from then on (REJECTED, INVALID_STATE), those under way go on. The
 * peer reads what the items hold when it fetches: of a directory published
 * with SYSTEM, whatever this end's account (a sharer's agent's) can read in
 * it. Directories are listed with the same rules and limits as a transfer
 * sent (max_items, max_offer_bytes, the size of one message).
 *
 * Settings changed while running (rcs_session_update) apply to transfers
 * offered from then on; the rate limit applies at once (to what goes as it
 * is sent, not to pieces read). */
#ifndef RCSDK_FILE_TRANSFER_H
#define RCSDK_FILE_TRANSFER_H

#include <rcsdk/module.h>

#if defined(RCSDK_STATIC)
#define RCSDK_FILE_TRANSFER_API
#elif defined(_WIN32)
#if defined(RCSDK_BUILDING_FILE_TRANSFER)
#define RCSDK_FILE_TRANSFER_API __declspec(dllexport)
#else
#define RCSDK_FILE_TRANSFER_API __declspec(dllimport)
#endif
#else
#define RCSDK_FILE_TRANSFER_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RCS_FILE_TRANSFER_MODULE "rcsdk.file-transfer"
#define RCS_FILE_TOKEN_SIZE 16u        /* Bytes of a transfer's token. */
#define RCS_FILE_PUBLICATION_SIZE 16u  /* Bytes of a publication's id. */
#define RCS_FILE_PATH_MAX 1024u        /* Bytes of an item's relative path with its NUL. */
#define RCS_FILE_READ_MAX 262144u      /* The most bytes one rcs_file_read asks for. */
#define RCS_FILE_READS_MAX 16777216u   /* Bytes a READ fetch's reads may ask for that are not answered yet. */

/* Bits in rcs_file_transfer_settings.flow, and a transfer's direction. */
typedef uint32_t rcs_file_flow;
enum { RCS_FILE_SEND = 1u << 0, RCS_FILE_RECEIVE = 1u << 1 };
typedef uint32_t rcs_file_target;
enum { RCS_FILE_SYSTEM = 1, RCS_FILE_HOST = 2 };
/* What an item is. */
typedef uint32_t rcs_file_kind;
enum { RCS_FILE_REGULAR = 1, RCS_FILE_DIRECTORY = 2, RCS_FILE_OTHER = 3 };
/* What a receiving end with SYSTEM does with a file whose name is taken:
 *   OVERWRITE  Replaces a file there (a directory there fails the item).
 *   RENAME     Writes it as "name (1).ext", "name (2).ext"... the first free.
 *   SKIP       Leaves what is there; the item is SKIPPED.
 *   FAIL       The whole transfer fails (before it starts when the name is
 *              taken then). */
typedef uint32_t rcs_file_conflict;
enum {
    RCS_FILE_CONFLICT_OVERWRITE = 1,
    RCS_FILE_CONFLICT_RENAME = 2,
    RCS_FILE_CONFLICT_SKIP = 3,
    RCS_FILE_CONFLICT_FAIL = 4
};
/* Where a transfer is:
 *   PREPARING  Send: its items are being listed. Receive: accepted, the
 *              directory is being checked (space, names, an earlier run's
 *              files).
 *   OFFERED    The receiving host has not decided yet.
 *   RUNNING    Items go.
 *   PAUSED     Either end paused it (rcs_file_transfer_info.paused).
 *   WAITING    The peer's connection ended (or a sharer's agent that reads
 *              or writes left): it waits for it to come back.
 *   FINISHED   It ended (rcs_file_end), as RCS_FILE_DONE says: until that
 *              comes, a transfer that is ending reads as it was. */
typedef uint32_t rcs_file_state;
enum {
    RCS_FILE_PREPARING = 1,
    RCS_FILE_OFFERED = 2,
    RCS_FILE_RUNNING = 3,
    RCS_FILE_PAUSED = 4,
    RCS_FILE_WAITING = 5,
    RCS_FILE_FINISHED = 6
};
/* Bits in rcs_file_transfer_info.paused. */
enum { RCS_FILE_PAUSED_HERE = 1u << 0, RCS_FILE_PAUSED_THERE = 1u << 1 };
/* How a transfer ended, with its rcs_result:
 *   COMPLETED  Every item is done, skipped or failed on its own (RCS_OK).
 *   REJECTED   The receiving host rejected it (CANCELLED), did not decide in
 *              time (TIMEOUT), or its end does not take it (INVALID_STATE:
 *              it does not receive; INVALID_ARGUMENT: beyond its limits;
 *              BUSY: offers waiting for its host fill max_pending_offers,
 *              max_pending_items or max_pending_items_total).
 *   CANCELLED  A host cancelled it (CANCELLED).
 *   FAILED     It could not go on: DISCONNECTED (the peer did not come
 *              back, the run ended), DEVICE (the files or the machine's half
 *              could not be used: not enough space, the agent did not come
 *              back...), INVALID_STATE (a name taken with
 *              RCS_FILE_CONFLICT_FAIL), INVALID_ARGUMENT (a path that is not
 *              there, a name the other end cannot take, beyond a limit),
 *              PROTOCOL (the other end broke the module's rules). */
typedef uint32_t rcs_file_end;
enum { RCS_FILE_COMPLETED = 1, RCS_FILE_REJECTED = 2, RCS_FILE_CANCELLED = 3, RCS_FILE_FAILED = 4 };
/* Where a transfer came from: this end's rcs_file_send or the peer's, or a
 * fetch of a publication (this end's of the peer's, or the peer's of this
 * end's). */
typedef uint32_t rcs_file_origin;
enum { RCS_FILE_ORIGIN_SENT = 1, RCS_FILE_ORIGIN_FETCHED = 2 };
/* How a fetch takes the items:
 *   INTO  As a transfer received: SYSTEM writes them into a directory, HOST
 *         hands them to `write`.
 *   READ  Nothing is written: the fetching host reads pieces of the files
 *         (rcs_file_read) in any order, as often as it likes, until it closes
 *         the fetch (rcs_file_close). A file read from its start to its end
 *         is COMPLETED as it is, a directory once the fetch is closed; what
 *         was not read to its end is SKIPPED then. */
typedef uint32_t rcs_file_fetch_mode;
enum { RCS_FILE_FETCH_INTO = 1, RCS_FILE_FETCH_READ = 2 };
/* Where an item is; SKIPPED and FAILED say why in result and message:
 *   COMPLETED  Written (send: at the other end), or by an earlier run.
 *   SKIPPED    RCS_OK: its name was taken (RCS_FILE_CONFLICT_SKIP).
 *              UNSUPPORTED: a symbolic link or something else that is not
 *              a file or a directory (RCS_FILE_OTHER).
 *   FAILED     It could not be read or written (DEVICE), changed while it
 *              went (INVALID_STATE), its name was taken (OVERWRITE over a
 *              directory, FAIL)... */
typedef uint32_t rcs_file_item_state;
enum {
    RCS_FILE_ITEM_PENDING = 1,
    RCS_FILE_ITEM_ACTIVE = 2,
    RCS_FILE_ITEM_COMPLETED = 3,
    RCS_FILE_ITEM_SKIPPED = 4,
    RCS_FILE_ITEM_FAILED = 5
};

/* Relative paths (rcs_file_item.name, what the other end offers): UTF-8,
 * components separated by '/', at most RCS_FILE_PATH_MAX - 1 bytes; each
 * component 1..255 bytes, not "." or "..", none of Windows' reserved names
 * (CON, PRN, AUX, NUL, COM1..9, LPT1..9, with or without an extension), no
 * control characters or \ : * ? " < > |, not ending in a dot or a space,
 * not ending in ".rcsdk-part" or starting with ".rcsdk-resume-". So it
 * lands under the receiving directory on any system, never outside it. */

/* An item to send. */
typedef struct rcs_file_item {
    /* SYSTEM: what to send, an absolute path on the machine the module reads
     * (a sharer's agent's): a file, or a directory with all it holds. HOST:
     * NULL. */
    const char* path;
    /* Its relative path at the other end. SYSTEM: NULL takes path's last
     * component; a directory's contents go below it. HOST: required. */
    const char* name;
    /* HOST: REGULAR (`size` bytes, given by `read`) or DIRECTORY (size 0),
     * and its modification time in nanoseconds since 1970 UTC (0: none).
     * SYSTEM: 0, 0 and 0: the file system says. */
    rcs_file_kind kind;
    uint64_t size;
    int64_t modified_ns;
} rcs_file_item;

/* What rcs_file_send offers. */
typedef struct rcs_file_offer {
    uint32_t struct_size;
    rcs_viewer_id viewer_id; /* Share: the viewer it goes to; view: 0. */
    uint32_t item_size;      /* sizeof(rcs_file_item) */
    uint32_t count;          /* 1..max_items (each end's) */
    const rcs_file_item* items;
    /* All zero: a new transfer (the module makes its token). An earlier
     * transfer's token (rcs_file_transfer_info.token): the same items again,
     * to go on where it stopped. */
    uint8_t resume_token[RCS_FILE_TOKEN_SIZE];
} rcs_file_offer;
#define RCS_FILE_OFFER_INIT                                                                                            \
    {sizeof(rcs_file_offer), 0, sizeof(rcs_file_item), 0, NULL, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}
RCS_INLINE rcs_file_offer rcs_file_offer_init(void) {
    rcs_file_offer value = RCS_FILE_OFFER_INIT;
    return value;
}

/* target HOST: a piece of an item, for the host's storage. Valid during the
 * call. */
typedef struct rcs_file_chunk {
    uint32_t struct_size;
    uint64_t transfer;
    rcs_viewer_id viewer_id;
    uint32_t item;       /* Its index in the transfer. */
    const char* name;    /* Its relative path. */
    uint64_t item_size;
    int64_t modified_ns;
    const uint8_t* token; /* The transfer's, RCS_FILE_TOKEN_SIZE bytes. */
    uint64_t offset;
    /* read: fill `size` bytes; write: the `size` bytes that go at offset;
     * resume: NULL and 0. */
    uint8_t* data;
    uint32_t size;
} rcs_file_chunk;

/* The module's settings (rcs_module_config.settings, size sizeof). */
typedef struct rcs_file_transfer_settings {
    uint32_t struct_size;
    rcs_file_flow flow;     /* SEND and RECEIVE bits: what this end allows; 0 (default): nothing. */
    rcs_file_target target; /* Default SYSTEM. */
    uint32_t max_items;     /* The most items of one transfer, either way: 1..100 000; default 10 000. */
    uint64_t max_offer_bytes; /* The most bytes of one transfer, either way; 0 (default): no limit. */
    /* Offers waiting for this end's host to decide (OFFERED), so that no peer
     * fills this end's memory with offers nobody decides on: the most of
     * them from one peer, and the most of their items from one peer and
     * from all peers together. An offer past any of them is refused at once
     * (REJECTED, BUSY: the reason names the limit), without RCS_FILE_OFFER;
     * the connection and other transfers go on. One accepted, rejected or
     * ended otherwise counts no more. */
    uint32_t max_pending_offers;      /* From one peer: 1..10 000; default 100. */
    uint32_t max_pending_items;       /* From one peer: max_items..1 000 000; default 20 000. */
    uint32_t max_pending_items_total; /* From all peers: max_items..1 000 000; default 100 000. */
    /* Publications of this end's not withdrawn (rcs_file_publish beyond:
     * BUSY): 1..10 000; default 256. Fetches of them one peer has going at
     * once (a fetch beyond: REJECTED, BUSY): 1..64; default 4. */
    uint32_t max_publications;
    uint32_t max_fetches;
    /* How long this end's host may take to decide on an offer: 1 000..3 600 000 ms; default 120 000. */
    uint32_t offer_timeout_ms;
    /* How long a transfer waits for its peer (or a sharer's agent) to come
     * back: 0..3 600 000 ms; default 60 000. */
    uint32_t reconnect_wait_ms;
    /* Each transfer this end sends goes at most this fast; 0 (default): as fast as the connection lets it. */
    uint64_t max_bytes_per_second;
    /* Receive, SYSTEM: 1 (default) keeps what a transfer the connection or
     * the run ended had written, for another run (see above); 0 deletes it
     * (also when a sharer's agent that writes is replaced midway). */
    uint32_t keep_partial;
    /* target HOST. Calls come on a thread of the module's, one at a time,
     * in each item's order; a failure (with rcs_set_last_call_error's
     * reason) fails that item. Send: `read` fills chunk->data. Receive:
     * `write` takes chunk->data, offsets in order from where the item
     * starts; an empty file or a directory gets no write: RCS_FILE_ITEM_DONE
     * says each item is whole, RCS_FILE_DONE how the transfer ended (keep
     * what partial_kept says), after its last call. `resume` (may be NULL:
     * 0), once per file of a transfer accepted: how many of its bytes the
     * host holds from an earlier run of the transfer (chunk->token),
     * 0..item_size. */
    rcs_result (*read)(const rcs_file_chunk* chunk, void* user_data);
    rcs_result (*write)(const rcs_file_chunk* chunk, void* user_data);
    uint64_t (*resume)(const rcs_file_chunk* chunk, void* user_data);
    void* user_data;
} rcs_file_transfer_settings;
#define RCS_FILE_TRANSFER_SETTINGS_INIT                                                                                \
    {sizeof(rcs_file_transfer_settings), 0u, RCS_FILE_SYSTEM, 10000u, 0u, 100u, 20000u, 100000u, 256u, 4u, 120000u,  \
     60000u, 0u, 1u, NULL, NULL, NULL, NULL}
RCS_INLINE rcs_file_transfer_settings rcs_file_transfer_settings_init(void) {
    rcs_file_transfer_settings value = RCS_FILE_TRANSFER_SETTINGS_INIT;
    return value;
}

/* What a transfer is now. */
typedef struct rcs_file_transfer_info {
    uint32_t struct_size;
    rcs_viewer_id viewer_id;  /* Share: its viewer; view: 0. */
    rcs_file_flow direction;  /* RCS_FILE_SEND: this end sends it; RCS_FILE_RECEIVE. */
    rcs_file_target target;
    rcs_file_state state;
    uint32_t paused;   /* RCS_FILE_PAUSED_HERE, RCS_FILE_PAUSED_THERE bits. */
    uint32_t resuming; /* Offered with an earlier transfer's token. */
    uint8_t token[RCS_FILE_TOKEN_SIZE];
    rcs_file_origin origin;
    rcs_file_fetch_mode mode;                       /* FETCHED: how. */
    uint8_t publication[RCS_FILE_PUBLICATION_SIZE]; /* FETCHED: of which publication. */
    uint32_t items;
    uint32_t items_completed, items_skipped, items_failed;
    uint32_t item; /* The one going now; `items` once none is. */
    uint64_t total_bytes;
    /* Of total_bytes: written (send: at the other end), by an earlier run
     * included, and those of items skipped or failed. */
    uint64_t done_bytes;
    uint64_t resumed_bytes;     /* Of done_bytes: what an earlier run wrote. */
    uint64_t transferred_bytes; /* Of files, over the connection, again after a reconnect included. */
    uint64_t bytes_per_second;  /* Over the last second or so. */
    rcs_file_end end;           /* FINISHED: how; 0 before. */
    uint32_t by_peer;           /* FINISHED: the other end ended it. */
    rcs_result result;          /* FINISHED: see rcs_file_end. */
    /* FINISHED, receive: what its unfinished items had written is kept for
     * another run (keep_partial). */
    uint32_t partial_kept;
    char message[256];                  /* FINISHED, not COMPLETED: why (UTF-8, cut short to fit). */
    char directory[RCS_FILE_PATH_MAX];  /* Receive, SYSTEM, once accepted: where it goes. */
} rcs_file_transfer_info;
#define RCS_FILE_TRANSFER_INFO_INIT                                                                                    \
    {sizeof(rcs_file_transfer_info), 0, 0, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 0,       \
     {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0}, {0}}
RCS_INLINE rcs_file_transfer_info rcs_file_transfer_info_init(void) {
    rcs_file_transfer_info value = RCS_FILE_TRANSFER_INFO_INIT;
    return value;
}

/* One item of a transfer. */
typedef struct rcs_file_item_info {
    uint32_t index;
    rcs_file_kind kind;
    rcs_file_item_state state;
    rcs_result result; /* SKIPPED, FAILED: see rcs_file_item_state. */
    uint64_t size;
    int64_t modified_ns;
    uint64_t bytes;                   /* Of it written (send: at the other end), an earlier run's included. */
    char path[RCS_FILE_PATH_MAX];     /* Its relative path. */
    char saved_as[RCS_FILE_PATH_MAX]; /* Receive, SYSTEM, COMPLETED: where it is under the directory. */
    char message[256];                /* SKIPPED, FAILED: why. */
} rcs_file_item_info;

/* What the other end allows, as its hello says: its flow, and its limits. */
typedef struct rcs_file_peer {
    uint32_t struct_size;
    rcs_file_flow flow;
    uint32_t max_items;
    uint64_t max_offer_bytes;
} rcs_file_peer;
#define RCS_FILE_PEER_INIT {sizeof(rcs_file_peer), 0, 0, 0}
RCS_INLINE rcs_file_peer rcs_file_peer_init(void) {
    rcs_file_peer value = RCS_FILE_PEER_INIT;
    return value;
}

/* Its events: RCS_EVENT_MODULE of module RCS_FILE_TRANSFER_MODULE, about
 * the transfer's viewer (view: 0); the payloads below.
 *   OFFER      rcs_file_offer_event: the peer offers a transfer; decide with
 *              rcs_file_accept or rcs_file_reject (a fetch has none).
 *   PROGRESS   rcs_file_progress_event: at most every 250 ms while it goes.
 *   ITEM_DONE  rcs_file_item_event: an item COMPLETED, SKIPPED or FAILED.
 *   DONE       rcs_file_done_event: the transfer FINISHED, and the module
 *              is through with it: received with SYSTEM, its files are in
 *              place with their times, its partial files and record gone
 *              (but for partial_kept); none of its files is open; with
 *              HOST, its calls (read, write, resume) are over. The receiving
 *              end's says the files are there (the sending end's may come
 *              first). Should the machine's half (a sharer's agent) leave
 *              before it is through, or the run stop, DONE comes then, and
 *              what was not done stays (the log says what): directories
 *              without their times, a record and partial files
 *              (keep_partial), which rcs_file_discard deletes; with HOST,
 *              calls asked before the run stopped may still come.
 *   STATE      rcs_file_state_event: it is PREPARING, OFFERED, RUNNING,
 *              PAUSED or WAITING now, or paused otherwise.
 *   PEER       rcs_file_peer: what the peer allows changed (it joined, or
 *              its settings changed).
 *   DISCARDED  rcs_file_discard_event: rcs_file_discard is through. */
enum {
    RCS_FILE_OFFER = RCS_MODULE_EVENT_OWN,
    RCS_FILE_PROGRESS = RCS_MODULE_EVENT_OWN + 1,
    RCS_FILE_ITEM_DONE = RCS_MODULE_EVENT_OWN + 2,
    RCS_FILE_DONE = RCS_MODULE_EVENT_OWN + 3,
    RCS_FILE_STATE = RCS_MODULE_EVENT_OWN + 4,
    RCS_FILE_PEER = RCS_MODULE_EVENT_OWN + 5,
    RCS_FILE_DISCARDED = RCS_MODULE_EVENT_OWN + 6
};
typedef struct rcs_file_offer_event {
    uint64_t transfer;
    uint32_t items;
    uint32_t resuming; /* The sender offers an earlier transfer again: the token is that one's. */
    uint64_t total_bytes;
    uint8_t token[RCS_FILE_TOKEN_SIZE];
} rcs_file_offer_event;
typedef struct rcs_file_progress_event {
    uint64_t transfer;
    uint32_t item;           /* The one going now. */
    uint32_t items_finished; /* Completed, skipped or failed. */
    uint64_t done_bytes;     /* As rcs_file_transfer_info's. */
    uint64_t total_bytes;
    uint64_t bytes_per_second;
} rcs_file_progress_event;
typedef struct rcs_file_item_event {
    uint64_t transfer;
    uint32_t item;
    rcs_file_item_state state; /* COMPLETED, SKIPPED or FAILED. */
    rcs_result result;
    uint64_t bytes;
    char message[256];
} rcs_file_item_event;
typedef struct rcs_file_done_event {
    uint64_t transfer;
    rcs_file_flow direction;
    rcs_file_end end;
    uint32_t by_peer;
    rcs_result result;
    uint32_t items_completed, items_skipped, items_failed;
    uint32_t partial_kept;
    uint64_t done_bytes;
    char message[256];
} rcs_file_done_event;
typedef struct rcs_file_state_event {
    uint64_t transfer;
    rcs_file_state state;
    uint32_t paused;
} rcs_file_state_event;
typedef struct rcs_file_discard_event {
    uint8_t token[RCS_FILE_TOKEN_SIZE];
    rcs_result result;
    uint32_t files; /* Partial files deleted. */
    char message[256];
} rcs_file_discard_event;

/* What rcs_file_publish names: items one peer may fetch. */
typedef struct rcs_file_publication {
    uint32_t struct_size;
    rcs_viewer_id viewer_id; /* Who may fetch it: a sharer's viewer; a viewer's sharer, 0. */
    uint32_t item_size;      /* sizeof(rcs_file_item) */
    uint32_t count;          /* 1..max_items (each end's) */
    /* As rcs_file_offer's, copied (SYSTEM: listed at each fetch, not now). */
    const rcs_file_item* items;
    /* Each may be NULL; called on a thread of the module's, one at a time and
     * in order, outside its calls (they may call its functions), with the
     * publication's id. on_fetch: the peer fetches it, as this end's
     * transfer `transfer`. on_unpublished: it was withdrawn: CANCELLED by
     * rcs_file_unpublish, INVALID_STATE this end stopped sending (or changed
     * its target), DISCONNECTED the peer did not come back or the run ended;
     * nothing of it is called after. */
    void (*on_fetch)(const uint8_t* publication, uint64_t transfer, void* user_data);
    void (*on_unpublished)(const uint8_t* publication, rcs_result result, const char* message, void* user_data);
    void* user_data;
} rcs_file_publication;
#define RCS_FILE_PUBLICATION_INIT {sizeof(rcs_file_publication), 0, sizeof(rcs_file_item), 0, NULL, NULL, NULL, NULL}
RCS_INLINE rcs_file_publication rcs_file_publication_init(void) {
    rcs_file_publication value = RCS_FILE_PUBLICATION_INIT;
    return value;
}

/* What rcs_file_fetch fetches, and how. */
typedef struct rcs_file_fetch_request {
    uint32_t struct_size;
    rcs_viewer_id viewer_id; /* Whose publication: a sharer's viewer; a viewer's sharer, 0. */
    uint8_t publication[RCS_FILE_PUBLICATION_SIZE];
    rcs_file_fetch_mode mode;
    /* INTO with SYSTEM: as rcs_file_accept's (an existing directory's full
     * path, the rule for names taken). Otherwise NULL, and on_conflict is not
     * looked at. */
    const char* directory;
    rcs_file_conflict on_conflict;
    /* Called on a thread of the module's, one at a time and in order,
     * outside its calls (they may call its functions). on_listed (may be
     * NULL): the items are known (rcs_file_get_items); a READ fetch reads from
     * then on. on_read (READ, and only READ): an rcs_file_read's answer, with
     * its tag: the bytes (valid during the call), or why not: INVALID_STATE
     * for a file that changed at the other end (its item FAILED), the
     * fetch's own result for reads it ended before they were answered.
     * on_ended (may be NULL): last, once the fetch FINISHED (as its
     * RCS_FILE_DONE says); user_data is the host's until then. */
    void (*on_listed)(uint64_t transfer, void* user_data);
    void (*on_read)(uint64_t transfer, uint64_t tag, rcs_result result, const uint8_t* data, uint32_t size,
                    void* user_data);
    void (*on_ended)(uint64_t transfer, rcs_result result, const char* message, void* user_data);
    void* user_data;
} rcs_file_fetch_request;
#define RCS_FILE_FETCH_REQUEST_INIT                                                                                    \
    {sizeof(rcs_file_fetch_request), 0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, RCS_FILE_FETCH_INTO, NULL,  \
     RCS_FILE_CONFLICT_RENAME, NULL, NULL, NULL, NULL}
RCS_INLINE rcs_file_fetch_request rcs_file_fetch_request_init(void) {
    rcs_file_fetch_request value = RCS_FILE_FETCH_REQUEST_INIT;
    return value;
}

/* Bits in rcs_file_unpublish's flags: fetches of it going end too (CANCELLED). */
enum { RCS_FILE_UNPUBLISH_END_FETCHES = 1u << 0 };

/* The module, for rcs_register_module. */
RCSDK_FILE_TRANSFER_API const rcs_module_definition* rcs_file_transfer_module(void);

/* Offers `offer`'s items to its peer; the transfer's number in
 * out_transfer (this session's, never used again). It returns at once: with
 * SYSTEM the items are listed on the machine first (RCS_FILE_PREPARING), and
 * a failure there ends the transfer (RCS_FILE_DONE: a path that is not
 * there, a name the other end cannot take, more than the limits).
 * INVALID_ARGUMENT for an offer as no version defines it (see
 * rcs_file_item), more items or bytes than this end's limits or the
 * peer's; INVALID_STATE without SEND here or RECEIVE at the peer (the
 * reason says which), while not connected to that peer (or it does not use
 * the module), with SYSTEM while the module does not run on the machine (no
 * agent attached, or its process did not register it). */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_send(rcs_session session, const rcs_file_offer* offer,
                                                 uint64_t* out_transfer);
/* An OFFERED transfer this end receives goes ahead. SYSTEM: into
 * `directory`, an absolute path of an existing directory on the machine,
 * with on_conflict for names taken; it is checked there first
 * (RCS_FILE_PREPARING), and a failure (not a directory, not enough space,
 * a name taken with FAIL) ends it. HOST: directory NULL; on_conflict is
 * the host's own business, any value. INVALID_ARGUMENT for values as no
 * version defines them; INVALID_STATE for a transfer not OFFERED to this
 * end, while another this end received with the same token has not
 * FINISHED (one token, one transfer at a time), and with SYSTEM while the
 * module does not run on the machine. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_accept(rcs_session session, uint64_t transfer, const char* directory,
                                                   rcs_file_conflict on_conflict);
/* An OFFERED transfer this end receives does not go: REJECTED, CANCELLED;
 * `reason` (UTF-8, NULL: none; cut to 255 bytes) goes to the sender. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_reject(rcs_session session, uint64_t transfer, const char* reason);
/* Either end ends a transfer that has not FINISHED: CANCELLED, and the
 * partial files of its unfinished items are deleted (gone by its
 * RCS_FILE_DONE). INVALID_STATE for one that ended, or is ending. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_cancel(rcs_session session, uint64_t transfer);
/* Either end pauses (1) or lets go on (0) a transfer that has not
 * FINISHED; it goes while neither end pauses it. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_pause(rcs_session session, uint64_t transfer, uint32_t paused);
/* A transfer as it is now; the session keeps its last 256 FINISHED ones
 * (fewer when their items pass max_pending_items_total together).
 * INVALID_STATE for a number it does not know (any more). */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_get_transfer(rcs_session session, uint64_t transfer,
                                                         rcs_file_transfer_info* out);
/* Its items from `first` on, two passes as rcs_list_displays (NULL and 0:
 * how many; BUFFER_TOO_SMALL when they do not all fit, the first
 * `capacity` of them filled in all the same, so a host can take them a
 * page at a time). */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_get_items(rcs_session session, uint64_t transfer, uint32_t first,
                                                      rcs_file_item_info* items, uint32_t item_size,
                                                      uint32_t capacity, uint32_t* out_count);
/* What `viewer`'s end allows now (a viewer's sharer: 0); INVALID_STATE where
 * the module is not active with it. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_get_peer(rcs_session session, rcs_viewer_id viewer, rcs_file_peer* out);
/* SYSTEM: deletes what a transfer with this token (RCS_FILE_TOKEN_SIZE
 * bytes) kept in `directory` (an absolute path on the machine) for another
 * run: its partial files and its record; RCS_FILE_DISCARDED says how it
 * went (0 files where there was nothing). INVALID_ARGUMENT for values as no
 * version defines them; INVALID_STATE with target HOST (the host's storage
 * holds them), for a transfer with that token that has not FINISHED, and
 * while the module does not run on the machine. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_discard(rcs_session session, const char* directory,
                                                    const uint8_t* token);

/* Lets `publication`'s peer fetch its items later; nothing goes now. The
 * publication's id (RCS_FILE_PUBLICATION_SIZE random bytes) in out_id: the
 * peer fetches with it. INVALID_ARGUMENT as rcs_file_send's; INVALID_STATE
 * without SEND here or RECEIVE at the peer, or while not connected to it (or
 * it does not use the module); BUSY with max_publications not withdrawn. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_publish(rcs_session session, const rcs_file_publication* publication,
                                                    uint8_t* out_id);
/* Withdraws a publication of this end's (RCS_FILE_PUBLICATION_SIZE bytes):
 * on_unpublished, CANCELLED. Fetches of it going go on, unless flags has
 * RCS_FILE_UNPUBLISH_END_FETCHES. INVALID_STATE for one this end does not
 * have (never, or withdrawn). */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_unpublish(rcs_session session, const uint8_t* publication,
                                                      uint32_t flags);
/* Fetches the peer's publication as `request` says; the transfer's number in
 * out_transfer. It returns at once: the transfer is PREPARING until the peer
 * listed the items and they came (on_listed); then INTO goes as a transfer
 * received (with SYSTEM the directory is checked first), READ waits for its
 * reads. Beyond this end's max_items or max_offer_bytes, or the peer's, it
 * fails then; a peer that has no such publication for this end (never, or
 * withdrawn), no longer sends, or has max_fetches of this end's going
 * refuses it (REJECTED; INVALID_STATE, BUSY). INVALID_ARGUMENT for values as
 * no version defines them; INVALID_STATE without RECEIVE here or SEND at the
 * peer, while not connected to it (or it does not use the module), and for
 * INTO with SYSTEM while the module does not run on the machine. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_fetch(rcs_session session, const rcs_file_fetch_request* request,
                                                  uint64_t* out_transfer);
/* A READ fetch's file `item`: `size` bytes (1..RCS_FILE_READ_MAX, fewer where
 * the file ends sooner) from `offset` (before its end); the answer comes in
 * on_read with `tag`. QUEUE_FULL while the fetch's reads not answered ask for
 * RCS_FILE_READS_MAX bytes and more; INVALID_ARGUMENT for an item that is not
 * a file, an offset or a size out of range; INVALID_STATE for a transfer that
 * is not a READ fetch of this end's going, or whose items have not come. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_read(rcs_session session, uint64_t transfer, uint32_t item,
                                                 uint64_t offset, uint32_t size, uint64_t tag);
/* This end is through with a READ fetch: outcome RCS_OK ends it COMPLETED
 * (what was read to its end is COMPLETED, the rest SKIPPED), an error FAILED
 * with `reason` (UTF-8, NULL: none; cut to 255 bytes). rcs_file_cancel ends
 * it CANCELLED. INVALID_STATE for a transfer that is not a READ fetch of this
 * end's going, or whose items have not come. */
RCSDK_FILE_TRANSFER_API rcs_result rcs_file_close(rcs_session session, uint64_t transfer, rcs_result outcome,
                                                  const char* reason);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_FILE_TRANSFER_H */
