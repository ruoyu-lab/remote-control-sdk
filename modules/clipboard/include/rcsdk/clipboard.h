/* Remote Control SDK: the clipboard, an official module (rcsdk/module.h).
 *
 * What one end copies, the other can paste. Each end's host decides for its
 * own machine whether its copies go to the other end (SEND) and whether the
 * other end's come into it (RECEIVE); a copy travels where one end sends and
 * the other receives. A session without the module has no clipboard (it may
 * hold passwords; the host turns it on):
 *
 *     rcs_register_module(rcs_clipboard_module());   once per process
 *     rcs_clipboard_settings settings = RCS_CLIPBOARD_SETTINGS_INIT;
 *     settings.flow = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
 *     rcs_module_config use = RCS_MODULE_CONFIG_INIT;
 *     use.module = RCS_CLIPBOARD_MODULE;
 *     use.settings = &settings;
 *     use.size = sizeof(settings);                     chained in the session's extensions
 *
 * Both ends use it to carry copies (RCS_EVENT_MODULE says where the other end
 * does not). A sharer whose agent runs in another process (rcs_agent) reads
 * and places its machine's clipboard there: that process registers the module
 * too.
 *
 * iOS (the general pasteboard): reading a copy another application made may
 * show the system's "Allow Paste" prompt (iOS 16 on, unless the user allowed
 * it in Settings), each time a new copy goes; noticing that there is one
 * never does. A concealed copy that arrives stays on the device (not on the
 * user's others through Universal Clipboard).
 *
 * target:
 *   SYSTEM  The module watches this machine's clipboard (a sharer's is its
 *           agent's machine's) and places what arrives there. on_clipboard
 *           sees each copy first and may keep it from going or from being
 *           placed. A copy placed here never goes back to where it came from
 *           (a sharer passes it on to its other viewers).
 *   HOST    The module never touches the system clipboard: the host sends
 *           copies with rcs_clipboard_send and gets those that arrive in
 *           on_clipboard (which RECEIVE needs), e.g. a clipboard of its own.
 *
 * formats: what this end sends and takes, as (1u << rcs_clipboard_format)
 * bits; each travels in one form, whatever the system calls it:
 *   TEXT   UTF-8, lines ending in LF (Windows' CRLF is converted both ways).
 *   HTML   An HTML document in UTF-8 (Windows' CF_HTML header is taken off
 *          and put back).
 *   RTF    Rich Text Format, as the system holds it.
 *   IMAGE  A PNG file (Windows also reads bitmaps and places a bitmap beside
 *          it; macOS also reads TIFF and places TIFF beside it; not for a
 *          picture of more than 64 million pixels, 256 MiB decoded, which
 *          goes as PNG alone). One that comes is at most 16384 x 16384.
 *   FILES  Files and directories copied (Windows' Explorer, macOS' Finder),
 *          by name: nothing of what they hold goes when they are copied.
 *          Only when the other end pastes them does it fetch them, through
 *          the file transfer module (rcsdk/file_transfer.h): this end
 *          publishes a copy's files to each end that takes them, and
 *          withdraws them as a newer copy replaces it (a paste under way
 *          goes on). FILES is never among the formats 0 stands for: a host
 *          names it, and both ends use file transfer as well, the copying
 *          end with RCS_FILE_SEND, the pasting end with RCS_FILE_RECEIVE (or
 *          FILES is left out, the event says why). A paste is a file
 *          transfer (RCS_CLIPBOARD_PASTE names it): both hosts follow it,
 *          pause or cancel it with file transfer's events and functions.
 *          Mind what SEND means here: an end that controls this machine can
 *          copy any file there and paste it at its own, so a sharer that
 *          sends FILES lets a viewer with control take whatever its agent's
 *          account can read; on_clipboard sees each copy first. Files cut
 *          are reported as cut (rcs_clipboard_files.cut); the module never
 *          deletes them.
 *          How a system paste takes the other end's files: Windows places
 *          virtual files (FileGroupDescriptorW and FileContents streams,
 *          read as Explorer writes them, with its own progress and
 *          cancellation); macOS gives Finder a hidden placeholder to paste,
 *          sees where it went, and fetches the files there; iOS places item
 *          providers that fetch when an application loads them; files
 *          fetched ahead (files_prefetch_bytes) are placed as real files.
 *          rcs_clipboard_system_files says what this system's clipboard
 *          does with files.
 *
 * max_bytes: the most one copy holds, its formats together, going out and
 * coming in (FILES: the entries' names and sizes, not what they hold; on iOS
 * and Android, where a copy's files are another application's, what they
 * hold too, copied into files_directory as the copy is read); the other end
 * learns it (rcs_clipboard_get_peer). Formats that do not fit are left out,
 * kept in the order TEXT, HTML, RTF, IMAGE, FILES.
 *
 * check_interval_ms: target SYSTEM, SEND: how often the system clipboard is
 * looked at for a new copy.
 *
 * send_existing: 1: the copy this end holds when a connection starts (or the
 * other end starts taking copies) goes too, e.g. a viewer's, so that what was
 * copied before connecting can be pasted there; 0 (default): only copies
 * made while the other end takes them go, and connecting never changes the
 * other end's clipboard.
 *
 * send_concealed: a copy the copying program marked as not to be kept or
 * shared (a password manager's: Windows' ExcludeClipboardContentFromMonitorProcessing,
 * or CanIncludeInClipboardHistory or CanUploadToCloudClipboard 0; macOS's
 * org.nspasteboard.ConcealedType or TransientType) stays on this machine
 * unless this is 1. A concealed copy that arrives is placed with the same
 * marks.
 *
 * A change while running (rcs_session_update with the module's settings)
 * applies at once, and the other end learns what this end takes now. */
#ifndef RCSDK_CLIPBOARD_H
#define RCSDK_CLIPBOARD_H

#include <rcsdk/file_transfer.h>
#include <rcsdk/module.h>

#if defined(RCSDK_STATIC)
#define RCSDK_CLIPBOARD_API
#elif defined(_WIN32)
#if defined(RCSDK_BUILDING_CLIPBOARD)
#define RCSDK_CLIPBOARD_API __declspec(dllexport)
#else
#define RCSDK_CLIPBOARD_API __declspec(dllimport)
#endif
#else
#define RCSDK_CLIPBOARD_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RCS_CLIPBOARD_MODULE "rcsdk.clipboard"

typedef uint32_t rcs_clipboard_format;
enum {
    RCS_CLIPBOARD_TEXT = 1,
    RCS_CLIPBOARD_HTML = 2,
    RCS_CLIPBOARD_RTF = 3,
    RCS_CLIPBOARD_IMAGE = 4,
    RCS_CLIPBOARD_FILES = 5
};
/* Every format but FILES (what formats 0 stands for at most): FILES is named
 * on its own. */
#define RCS_CLIPBOARD_ALL_FORMATS                                                                                      \
    ((1u << RCS_CLIPBOARD_TEXT) | (1u << RCS_CLIPBOARD_HTML) | (1u << RCS_CLIPBOARD_RTF) | (1u << RCS_CLIPBOARD_IMAGE))
/* Bits in rcs_clipboard_settings.flow; one of them in rcs_clipboard_content
 * and rcs_clipboard_event. */
typedef uint32_t rcs_clipboard_flow;
enum { RCS_CLIPBOARD_SEND = 1u << 0, RCS_CLIPBOARD_RECEIVE = 1u << 1 };
typedef uint32_t rcs_clipboard_target;
enum { RCS_CLIPBOARD_SYSTEM = 1, RCS_CLIPBOARD_HOST = 2 };

/* A copy on a clipboard: one item per format, no format twice, each in the
 * form described above.
 *
 * on_clipboard: flow SEND, a copy of this machine's clipboard about to go to
 * viewer_id (a sharer asks once per viewer that takes it; a viewer's is 0),
 * already without the formats that one does not take or that do not fit;
 * flow RECEIVE, a copy from viewer_id (0 on the view side).
 * rcs_clipboard_send: flow SEND; viewer_id 0 sends it to every viewer that
 * takes it, an id to that viewer only (a viewer's is 0). */
typedef struct rcs_clipboard_item {
    rcs_clipboard_format format; /* TEXT..IMAGE: FILES are rcs_clipboard_content.files. */
    uint32_t size;       /* 1..max_bytes */
    const uint8_t* data; /* Borrowed: copied by rcs_clipboard_send; valid during on_clipboard. */
} rcs_clipboard_item;
/* A file or directory a copy names at its top (FILES). */
typedef struct rcs_clipboard_file {
    const char* name;      /* One component of file transfer's path rules (rcs_file_item.name). */
    rcs_file_kind kind;    /* REGULAR or DIRECTORY. */
    uint64_t size;         /* REGULAR: its bytes; DIRECTORY: 0 (what it holds is listed as it is fetched). */
    int64_t modified_ns;   /* Nanoseconds since 1970 UTC; 0: none. */
    /* Going: where it is on the machine file transfer reads (target SYSTEM,
     * a full path; HOST: NULL, its read callback gives the bytes by name).
     * Coming: NULL. */
    const char* path;
} rcs_clipboard_file;
typedef struct rcs_clipboard_files {
    uint32_t struct_size;
    uint32_t cut; /* The copying program marked them cut (reported only: nothing is deleted). */
    /* Coming: the other end's publication of them, for rcs_file_fetch
     * (viewer_id the content's) when the host takes them itself. Going: 0
     * (the module publishes to each end that takes them). */
    uint8_t publication[RCS_FILE_PUBLICATION_SIZE];
    uint32_t file_size; /* sizeof(rcs_clipboard_file) */
    uint32_t count;     /* 1..100 000 (and each end's max_items) */
    const rcs_clipboard_file* files;
} rcs_clipboard_files;
#define RCS_CLIPBOARD_FILES_INIT                                                                                       \
    {sizeof(rcs_clipboard_files), 0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, sizeof(rcs_clipboard_file), 0, \
     NULL}
RCS_INLINE rcs_clipboard_files rcs_clipboard_files_init(void) {
    rcs_clipboard_files value = RCS_CLIPBOARD_FILES_INIT;
    return value;
}
typedef struct rcs_clipboard_content {
    uint32_t struct_size;
    rcs_viewer_id viewer_id;
    rcs_clipboard_flow flow;
    uint32_t concealed; /* The copying program marked it (send_concealed). */
    uint32_t item_size; /* sizeof(rcs_clipboard_item) */
    uint32_t count;     /* 1..4; 0 with files */
    const rcs_clipboard_item* items;
    const rcs_clipboard_files* files; /* FILES; NULL: none. Borrowed as items are. */
} rcs_clipboard_content;
#define RCS_CLIPBOARD_CONTENT_INIT                                                                                     \
    {sizeof(rcs_clipboard_content), 0, RCS_CLIPBOARD_SEND, 0, sizeof(rcs_clipboard_item), 0, NULL, NULL}
RCS_INLINE rcs_clipboard_content rcs_clipboard_content_init(void) {
    rcs_clipboard_content value = RCS_CLIPBOARD_CONTENT_INIT;
    return value;
}

/* The module's settings (rcs_module_config.settings, size sizeof). */
typedef struct rcs_clipboard_settings {
    uint32_t struct_size;
    rcs_clipboard_flow flow;     /* SEND and RECEIVE bits; 0 (default): nothing goes or comes. */
    rcs_clipboard_target target; /* Default SYSTEM. */
    /* (1u << rcs_clipboard_format) bits; 0 (default): every format the
     * clipboard carries (target SYSTEM: rcs_clipboard_system_formats; HOST:
     * every format). */
    uint32_t formats;
    uint32_t max_bytes;          /* 1 KiB..256 MiB; default 16 MiB. */
    uint32_t check_interval_ms;  /* 50..5 000; default 250. */
    uint32_t send_existing;      /* 0 (default) or 1. */
    uint32_t send_concealed;     /* 0 (default) or 1. */
    /* FILES, target SYSTEM. A copy that comes holding files only, all of
     * them together of at most this many bytes, is fetched as it comes, into
     * files_directory, and placed as the real files once there (any program
     * pastes them then; before, the system's paste takes them as it does
     * without); 0 (default): only a paste fetches anything. What the other
     * end lists for them when they are fetched counts, not what the copy
     * said: more than files, or more bytes, and the fetch is cancelled
     * (RCS_CLIPBOARD_PASTE, BUFFER_TOO_SMALL, says why) and left to a paste. */
    uint64_t files_prefetch_bytes;
    /* Where files fetched ahead, macOS' placeholders and, on iOS and
     * Android, copies of the files another application copied go: an
     * existing directory's full path on the machine (a sharer's agent's),
     * UTF-8; empty (default): a directory of the module's in the system's
     * temporary one, deleted when the machine's half stops. */
    char files_directory[RCS_FILE_PATH_MAX];
    /* macOS: 1 (default) gives Finder, as the user pastes there, a hidden
     * placeholder (".rcsdk-paste-<id>") that it copies where the files go,
     * which are then fetched there; another program that pastes gets no
     * file (RCS_CLIPBOARD_PASTE says which). 0: no placeholder, only files
     * fetched ahead paste. Elsewhere it is not looked at. */
    uint32_t files_stand_in;
    /* Each copy (see rcs_clipboard_content), never overlapping: a copy that
     * came is seen on the thread that reads its connection, one going out on
     * the thread that read the clipboard. flow SEND: 0 keeps the copy from
     * going to that viewer. flow RECEIVE, target SYSTEM: nonzero places it
     * on this machine's clipboard, 0 does not; target HOST: it is the host's
     * to place, and the return value is ignored (its files' publication is
     * fetched with rcs_file_fetch, when the host likes, until a newer copy
     * withdraws it). Copy what is needed and return. Target HOST with
     * RECEIVE needs it. */
    int32_t (*on_clipboard)(const rcs_clipboard_content* content, void* user_data);
    void* user_data;
} rcs_clipboard_settings;
#define RCS_CLIPBOARD_SETTINGS_INIT                                                                                    \
    {sizeof(rcs_clipboard_settings), 0u, RCS_CLIPBOARD_SYSTEM, 0u, 16777216u, 250u, 0u, 0u, 0u, {0}, 1u, NULL, NULL}
RCS_INLINE rcs_clipboard_settings rcs_clipboard_settings_init(void) {
    rcs_clipboard_settings value = RCS_CLIPBOARD_SETTINGS_INIT;
    return value;
}

/* Its events: RCS_EVENT_MODULE of module RCS_CLIPBOARD_MODULE.
 *   RCS_CLIPBOARD_COPY  payload rcs_clipboard_event: a copy went to the other
 *                       end (flow SEND, to viewer_id) or, with target
 *                       SYSTEM, was placed on this machine's clipboard
 *                       (RECEIVE, from viewer_id), or could not be. Copies
 *                       on_clipboard kept back are not reported
 *                       (rcs_clipboard_stats counts them).
 *     RCS_OK            It went, or was placed, with `formats`.
 *     BUFFER_TOO_SMALL  None of its formats fit the other end's max_bytes,
 *                       or this end's.
 *     UNSUPPORTED       The other end takes none of its formats, or of
 *                       its files (FILES) none could go: the message says
 *                       why (this session or the other end does not use
 *                       file transfer, or does not send or receive files;
 *                       a name the other end cannot take...).
 *     DEVICE            The system clipboard could not be read (SEND,
 *                       viewer_id 0) or written.
 *   RCS_CLIPBOARD_PEER  payload rcs_clipboard_peer: what viewer_id's end
 *                       takes changed (rcs_clipboard_get_peer).
 *   RCS_CLIPBOARD_PASTE payload rcs_clipboard_paste_event: a copy's files
 *                       are fetched (RECEIVE: this machine pastes viewer_id's
 *                       files, or fetches them ahead; SEND: viewer_id pastes
 *                       this end's), as file transfer `transfer`: its events
 *                       follow it (RCS_FILE_DONE says how it ended), its
 *                       functions pause or cancel it. Or a paste here could
 *                       not start (RECEIVE; result and message say why: the
 *                       files were withdrawn, a program that cannot take
 *                       them pasted...), or a fetch ahead was cancelled
 *                       (BUFFER_TOO_SMALL: files_prefetch_bytes). */
enum {
    RCS_CLIPBOARD_COPY = RCS_MODULE_EVENT_OWN,
    RCS_CLIPBOARD_PEER = RCS_MODULE_EVENT_OWN + 1,
    RCS_CLIPBOARD_PASTE = RCS_MODULE_EVENT_OWN + 2
};
typedef struct rcs_clipboard_event {
    rcs_clipboard_flow flow;
    rcs_result result;
    uint32_t formats;  /* What went or was placed, as (1u << rcs_clipboard_format) bits... */
    uint32_t left_out; /* ...and what the copy held besides: not taken there, or too large. */
    uint64_t bytes;    /* Of what went or was placed. */
    /* Not RCS_OK: why; RCS_OK with FILES left out for a reason the other
     * end's limits do not give: why (UTF-8, cut short to fit). */
    char message[128];
} rcs_clipboard_event;
typedef struct rcs_clipboard_paste_event {
    rcs_clipboard_flow flow;
    rcs_result result;  /* RCS_OK: the fetch started; else why there is none. */
    uint64_t transfer;  /* RCS_OK: file transfer's number of the fetch. */
    uint32_t cut;       /* The copying program marked the files cut. */
    uint32_t ahead;     /* RECEIVE: fetched ahead (files_prefetch_bytes), not pasted. */
    char message[256];  /* Not RCS_OK: why. */
} rcs_clipboard_paste_event;

/* What the other end takes from this one: its settings' formats (those its
 * system places, with target SYSTEM) while it has RECEIVE, and how large a
 * copy may be; 0 and 0: nothing. */
typedef struct rcs_clipboard_peer {
    uint32_t struct_size;
    uint32_t formats;
    uint32_t max_bytes;
} rcs_clipboard_peer;
#define RCS_CLIPBOARD_PEER_INIT {sizeof(rcs_clipboard_peer), 0, 0}
RCS_INLINE rcs_clipboard_peer rcs_clipboard_peer_init(void) {
    rcs_clipboard_peer value = RCS_CLIPBOARD_PEER_INIT;
    return value;
}

/* Copies, of this run. */
typedef struct rcs_clipboard_stats {
    uint32_t struct_size;
    uint64_t sent; /* Copies that went to the other end... */
    uint64_t sent_bytes;
    uint64_t received; /* ...and that came from it (placed, or handed to the host). */
    uint64_t received_bytes;
    uint64_t refused;   /* Kept back by on_clipboard, going or coming. */
    uint64_t concealed; /* Kept on this machine as concealed (send_concealed 0). */
    uint64_t failed;    /* Could not go or be placed: RCS_CLIPBOARD_COPY said why. */
    /* Fetches of copies' files (RCS_CLIPBOARD_PASTE): this machine's of the
     * other end's (pastes and fetches ahead), the other end's of this end's,
     * and pastes here that could not start. */
    uint64_t pastes_here;
    uint64_t pastes_there;
    uint64_t pastes_failed;
} rcs_clipboard_stats;
#define RCS_CLIPBOARD_STATS_INIT {sizeof(rcs_clipboard_stats), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
RCS_INLINE rcs_clipboard_stats rcs_clipboard_stats_init(void) {
    rcs_clipboard_stats value = RCS_CLIPBOARD_STATS_INIT;
    return value;
}

/* The module, for rcs_register_module. */
RCSDK_CLIPBOARD_API const rcs_module_definition* rcs_clipboard_module(void);
/* The formats this system's clipboard carries for target SYSTEM: every
 * format but FILES on Windows, macOS and iOS, TEXT and HTML on Android, 0
 * elsewhere (for files, rcs_clipboard_system_files). */
RCSDK_CLIPBOARD_API uint32_t rcs_clipboard_system_formats(void);
/* What this process's system clipboard does with files (FILES), as
 * rcs_clipboard_flow bits: SEND, it reads the files copied; RECEIVE, a paste
 * there takes the other end's. Where one is missing `reason` (may be NULL,
 * at most `capacity` bytes with its NUL) says why: Windows, macOS: both, but
 * not in a Windows process of LocalSystem (an agent that follows the secure
 * desktop: run the agent as the user, RCS_HELPER_USER) or a sandboxed macOS
 * application; iOS both; Android SEND (a paste of the other end's files
 * needs a content provider of the host's: not yet); 0 elsewhere. A sharer's
 * agent of another process says what it does there: a copy's files go
 * where it reads them, and the other end learns whether this one takes
 * them. target SYSTEM with FILES is UNSUPPORTED where this system has no
 * way at all. */
RCSDK_CLIPBOARD_API uint32_t rcs_clipboard_system_files(char* reason, uint32_t capacity);
/* Connected, with RCS_CLIPBOARD_SEND: sends a copy as this end's clipboard,
 * whatever the target (SYSTEM leaves the system clipboard as it is); copied,
 * nonblocking. It goes to the viewers that take clipboard copies, without
 * the formats each does not take or that do not fit its max_bytes;
 * RCS_CLIPBOARD_COPY reports each. A newer copy replaces one not sent yet.
 * Its files (FILES) are published to each viewer that takes them, as file
 * transfer reads them (target SYSTEM: paths on the machine; HOST: names its
 * read callback knows). INVALID_ARGUMENT for a format no version defines,
 * twice, not in this end's formats, an empty item, files as no version
 * defines them (a name that is not one component, twice, a kind but REGULAR
 * or DIRECTORY, a publication not 0), or more than its max_bytes (checked
 * against the run's settings: before the run INVALID_STATE comes first); INVALID_STATE
 * without SEND, before running or between connections, for a viewer_id not
 * connected (or not using the clipboard), and for a session that does not
 * use the module; a viewing session's viewer_id is 0. */
RCSDK_CLIPBOARD_API rcs_result rcs_clipboard_send(rcs_session session, const rcs_clipboard_content* content);
/* What `viewer`'s end takes now (a viewer's sharer: 0); INVALID_STATE where
 * the module is not active with it. */
RCSDK_CLIPBOARD_API rcs_result rcs_clipboard_get_peer(rcs_session session, rcs_viewer_id viewer,
                                                      rcs_clipboard_peer* peer);
/* The run's copies: viewer 0 the session's (a sharer's: every viewer's added
 * up, those that left too), an id one viewer's (share). INVALID_STATE for a
 * session that does not use the module, or a viewer it never knew. */
RCSDK_CLIPBOARD_API rcs_result rcs_clipboard_get_stats(rcs_session session, rcs_viewer_id viewer,
                                                       rcs_clipboard_stats* stats);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_CLIPBOARD_H */
