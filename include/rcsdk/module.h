/* Remote Control SDK: modules.
 *
 * A module adds a feature to sessions through this interface alone: the
 * SDK's own (official modules, e.g. rcsdk/clipboard.h) and anyone else's
 * alike. It is a table of callbacks and an id. A process registers it once
 * (rcs_register_module); a session uses it when the host chains its settings
 * (RCS_EXT_MODULE, rcs_module_config). A module a session does not name does
 * nothing there: no thread, no message, no call.
 *
 * A module has two halves:
 *   The session's  Runs in the session. With RCS_MODULE_PEER it works with
 *                  its own kind at the other end: the two ends tell each
 *                  other their modules when they connect, and a module is
 *                  active on a connection where both ends use it in the same
 *                  version (RCS_EVENT_MODULE says where it is not, and why).
 *                  It sends its kind messages, and emits events for the host.
 *   The machine's  With RCS_MODULE_MACHINE: runs where the machine is, a
 *                  sharer's agent (in the session's process, or in another
 *                  one, which registers the module too: rcs_agent) or a
 *                  viewer's own process. It reaches the session's half only by
 *                  messages (rcs_module_send_machine), wherever it runs.
 *
 * Calls into one half never overlap: each waits for the one before, so a
 * module's state needs no lock against them; rcs_module_run puts the
 * module's own work (its public functions) in the same order. They come on
 * the SDK's threads (the connection's reader, the agent's, the host's
 * calling rcs_session_update) and return soon: long work goes to a thread of
 * the module's own. The functions below are thread-safe. An event a module
 * emits inside one of its calls reaches the host once that call returned, and
 * any call of another module's it was made from (a module may call another's
 * public functions, e.g. the clipboard file transfer's: its calls are held
 * meanwhile, so it must never be called back the other way round inside
 * them).
 *
 * Validate what comes from the other end: it may be anyone's module. A
 * module runs in the host's process and can do anything there; it sees the
 * session only through the public functions (rcs_module_session), and never
 * takes part in identity or the handshake. */
#ifndef RCSDK_MODULE_H
#define RCSDK_MODULE_H

#include <rcsdk/rcsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An id: 1..63 letters, digits, dots and dashes. "rcsdk." ids are the SDK's
 * own modules; others use a reversed domain they own, e.g.
 * "com.example.whiteboard". */
#define RCS_MODULE_ID_MAX 64u           /* Bytes of an id with its NUL. */
#define RCS_MODULE_HELLO_MAX 1024u      /* Bytes of a hello. */
#define RCS_MODULE_MAX_MESSAGE 1073741824u /* 1 GiB: the most max_message_bytes may be. */
#define RCS_MODULE_SETTINGS_MAX 65536u  /* Bytes of a module's settings (rcs_module_config.size). */
#define RCS_MODULES_MAX 32u             /* Modules one session uses. */

/* One half of a module: the session's, for one session (from
 * rcs_session_create to rcs_session_destroy), or the machine's, from
 * machine_start to machine_stop. */
typedef struct rcs_module_impl* rcs_module;

typedef uint32_t rcs_module_flags;
enum {
    RCS_MODULE_PEER = 1u << 0,   /* It works with its kind at the other end. */
    RCS_MODULE_MACHINE = 1u << 1 /* It has a machine's half. */
};

/* Bits in rcs_module_send's flags and received's. */
typedef uint32_t rcs_module_message_flags;
enum {
    /* As a datagram (at most RCS_DATA_MAX_DATAGRAM bytes): never resent, and
     * maybe lost (over TCP only where the path takes far less than is sent). */
    RCS_MODULE_UNRELIABLE = 1u << 0,
    /* Reliable: it replaces this module's newest message with REPLACE to the
     * same peer that has not started to go (its sent() never comes), e.g. a
     * clipboard's newest copy. */
    RCS_MODULE_REPLACE = 1u << 1
};

/* What a module is. The SDK copies the table and the id at registration;
 * every call gets user_data back. Every callback may be NULL. */
typedef struct rcs_module_definition {
    uint32_t struct_size; /* sizeof(rcs_module_definition) */
    const char* id;
    /* Its own messages' version: a connection where the other end's differs
     * leaves it inactive (RCS_MODULE_INACTIVE), as does an agent's process
     * with another one (RCS_MODULE_NO_MACHINE). */
    uint32_t version;
    rcs_module_flags flags;
    uint32_t roles;             /* The sessions it takes part in, as (1u << rcs_role) bits. */
    uint32_t max_message_bytes; /* 1..RCS_MODULE_MAX_MESSAGE: the most one message holds, either way. */
    void* user_data;

    /* ---- The session's half -------------------------------------------- */

    /* Whether a session of `role` takes `settings` (`size` bytes, the host's
     * rcs_module_config), at rcs_session_create and rcs_session_update:
     * RCS_OK, or INVALID_ARGUMENT (a value it does not take) or UNSUPPORTED
     * (this system cannot), with why in `reason` (UTF-8, at most `capacity`
     * bytes with the NUL). NULL takes any settings. */
    rcs_result (*check)(rcs_role role, const void* settings, uint32_t size, char* reason, uint32_t capacity,
                        void* user_data);
    /* A run starts (rcs_session_start), with the settings as they are now;
     * the borrowed copy is valid during the call. */
    void (*start)(rcs_module module, rcs_role role, const void* settings, uint32_t size, void* user_data);
    /* rcs_session_update gave new settings, which check() took, while it runs. */
    void (*update)(rcs_module module, const void* settings, uint32_t size, void* user_data);
    /* A connection passed the handshake: `peer` (a sharer's viewer; 0 is a
     * viewer's sharer), whether or not the module is active with it; before
     * its peer_joined. For a module that follows who is connected, e.g. to
     * keep the machine awake while anyone is. */
    void (*connected)(rcs_module module, rcs_viewer_id peer, void* user_data);
    /* That connection ended; after its peer_left. */
    void (*disconnected)(rcs_module module, rcs_viewer_id peer, void* user_data);
    /* RCS_MODULE_PEER: it is active with `peer` (a sharer's viewer; 0 is a
     * viewer's sharer), which said `hello` (rcs_module_set_hello; may be
     * empty), and with which it was not before. */
    void (*peer_joined)(rcs_module module, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void* user_data);
    /* That peer set another hello. */
    void (*peer_hello)(rcs_module module, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void* user_data);
    /* Whether `peer` holds remote control now (1) or not (0): a sharer's
     * viewer, or a viewer's own control (peer 0, as its sharer grants it;
     * RCS_EVENT_CONTROL's granted). Right after peer_joined while it holds
     * control, then at each change; it does not when peer_left came. */
    void (*peer_control)(rcs_module module, rcs_viewer_id peer, uint32_t granted, void* user_data);
    /* It is not any more: its connection ended. Nothing of it follows. */
    void (*peer_left)(rcs_module module, rcs_viewer_id peer, void* user_data);
    /* A message from its kind at `peer`, whole; flags RCS_MODULE_UNRELIABLE
     * for a datagram. Borrowed during the call. */
    void (*received)(rcs_module module, rcs_viewer_id peer, const uint8_t* data, uint32_t size,
                     rcs_module_message_flags flags, void* user_data);
    /* A reliable message given to rcs_module_send with `tag` went to `peer`:
     * the connection took all of it. */
    void (*sent)(rcs_module module, rcs_viewer_id peer, uint64_t tag, void* user_data);
    /* The queue to `peer` takes messages again after it refused one (QUEUE_FULL). */
    void (*room)(rcs_module module, rcs_viewer_id peer, void* user_data);
    /* RCS_MODULE_MACHINE: its machine's half runs, and rcs_module_send_machine
     * reaches it, from now on... */
    void (*machine_joined)(rcs_module module, void* user_data);
    /* ...or not any more: a sharer's agent of another process left, or
     * another took its place (machine_joined follows once that one runs it). */
    void (*machine_left)(rcs_module module, void* user_data);
    /* A message from its machine's half. Borrowed during the call. */
    void (*from_machine)(rcs_module module, const uint8_t* data, uint32_t size, void* user_data);
    /* The run ends; peer_left and machine_left came for what was there.
     * Nothing follows until the next start. */
    void (*stop)(rcs_module module, void* user_data);

    /* ---- The machine's half (RCS_MODULE_MACHINE) ------------------------- */

    /* It starts, for a session of `role`: a sharer's agent (SHARE) or a viewer (VIEW). */
    void (*machine_start)(rcs_module module, rcs_role role, void* user_data);
    /* A message from the session's half. Borrowed during the call. */
    void (*from_session)(rcs_module module, const uint8_t* data, uint32_t size, void* user_data);
    /* It ends: the session's half stopped, or its agent leaves. End the
     * module's own threads that use the handle here: it is gone after. */
    void (*machine_stop)(rcs_module module, void* user_data);
} rcs_module_definition;

/* ---- The process's modules ----------------------------------------------- */

/* Registers a module for this process's sessions and agents, before they
 * are created; for good. INVALID_ARGUMENT for an id taken or not as above,
 * or values out of range. */
RCSDK_API rcs_result rcs_register_module(const rcs_module_definition* definition);

typedef struct rcs_module_info {
    char id[RCS_MODULE_ID_MAX];
    uint32_t version;
    rcs_module_flags flags;
    uint32_t roles;
    uint32_t max_message_bytes;
} rcs_module_info;

/* The registered modules, in the order they were registered; two passes, as
 * rcs_list_displays. */
RCSDK_API rcs_result rcs_list_modules(rcs_module_info* modules, uint32_t item_size, uint32_t capacity,
                                      uint32_t* out_count);

/* ---- What the SDK does for a module -------------------------------------- */

/* The session's half: a message to its kind at `peer` (0: every peer where it
 * is active; a viewer's sharer is 0), over the session's connection,
 * encrypted like the rest. Reliable messages come whole and in order, beside
 * the video and behind input, like data messages (rcs_session_send_data);
 * `tag` comes back in sent(). Each peer's queue holds max_message_bytes of this
 * module's messages: QUEUE_FULL when the message does not fit beside those
 * waiting (room() follows; to peer 0 it was queued for none). Copied.
 * INVALID_ARGUMENT: 1..max_message_bytes (UNRELIABLE: RCS_DATA_MAX_DATAGRAM),
 * flags some version defines; INVALID_STATE: not active with that peer (or
 * with none), or a machine's half. */
RCSDK_API rcs_result rcs_module_send(rcs_module module, rcs_viewer_id peer, const uint8_t* data, uint32_t size,
                                     rcs_module_message_flags flags, uint64_t tag);
/* The session's half: what it tells its kind at each end it connects to
 * (peer_joined) and, set again, where it is active now (peer_hello), at
 * once, ahead of messages waiting. 0..RCS_MODULE_HELLO_MAX bytes, copied;
 * empty until set. */
RCSDK_API rcs_result rcs_module_set_hello(rcs_module module, const uint8_t* hello, uint32_t size);
/* A message to the other half (the machine's from the session's, and back),
 * in order: in this process directly, to an agent of another process over its
 * link. 1..max_message_bytes, copied. INVALID_STATE while the other half does
 * not run (machine_joined, machine_start), or without RCS_MODULE_MACHINE. */
RCSDK_API rcs_result rcs_module_send_machine(rcs_module module, const uint8_t* data, uint32_t size);
/* Where what a sharing session shares shows now on the screen of the
 * machine a machine's half runs on (the session's agent's), for a module that
 * works with what is shown (rcsdk/text_caret.h maps the caret across it):
 * source and display_id as the capture names them (a display of 0 resolved),
 * the window (WINDOW), the process whose window or application it is (WINDOW,
 * APPLICATION; 0 otherwise), and its edges in the system's own screen
 * coordinates, Windows' physical pixels of the virtual screen and macOS'
 * global points, the right and bottom ones just outside. Remote input maps
 * across the same area. INVALID_STATE while nothing of it shows (the session
 * has not set up capture there yet, the window is minimized or gone, the
 * region is off its display), for a session's half and on a viewer's machine. */
typedef struct rcs_module_shared {
    uint32_t struct_size;
    rcs_capture_source source;
    uint32_t display_id;
    uint64_t window_id;
    uint32_t process_id;
    double left, top, right, bottom;
} rcs_module_shared;
#define RCS_MODULE_SHARED_INIT {sizeof(rcs_module_shared), RCS_CAPTURE_DISPLAY, 0, 0, 0, 0.0, 0.0, 0.0, 0.0}
RCS_INLINE rcs_module_shared rcs_module_shared_init(void) {
    rcs_module_shared value = RCS_MODULE_SHARED_INIT;
    return value;
}
RCSDK_API rcs_result rcs_module_get_shared(rcs_module module, rcs_module_shared* out);
/* The session's half: an event for the host (RCS_EVENT_MODULE) of the
 * module's own `kind` (RCS_MODULE_EVENT_OWN and up) about `peer` (0: none in
 * particular), its payload as the module's header defines it, copied
 * (0..65536 bytes). */
RCSDK_API rcs_result rcs_module_emit(rcs_module module, rcs_viewer_id peer, uint32_t kind, const void* payload,
                                     uint32_t size);
/* The session's half: its session, for the public functions a host calls
 * (rcs_session_get_state, rcs_session_list_viewers...); not
 * rcs_session_stop, rcs_session_destroy or rcs_session_start, which are
 * CALLBACK_THREAD inside a call. NULL for a machine's half. */
RCSDK_API rcs_session rcs_module_session(rcs_module module);
/* The session's half of module `id` in `session`, for the module's own
 * public functions (e.g. rcs_clipboard_send): valid while the session
 * exists. INVALID_STATE when the session does not use it. */
RCSDK_API rcs_result rcs_module_find(rcs_session session, const char* id, rcs_module* out);
/* Runs task(module, argument) as one of the module's calls: after the one
 * that runs (nested in it when called from it), before the next. Returns what
 * the task returns. */
RCSDK_API rcs_result rcs_module_run(rcs_module module, rcs_result (*task)(rcs_module module, void* argument),
                                    void* argument);
/* The module's own state for this half; `release` (may be NULL) is called
 * with it when another replaces it, and when the half is gone (a session's
 * when the session is destroyed). */
RCSDK_API void rcs_module_set_state(rcs_module module, void* state, void (*release)(void* state));
RCSDK_API void* rcs_module_state(rcs_module module);
/* The module's line for the log (rcs_set_log_callback), as the session's or
 * the agent's, named after the module. */
RCSDK_API void rcs_module_log(rcs_module module, rcs_log_level level, const char* message);
/* A module's own public functions say why they fail as the SDK's do:
 * rcs_set_last_call_error (rcsdk.h). */

/* ---- What the SDK's capture sees ----------------------------------------- */

/* A picture of what a capture would share (rcs_capture_picture), in the
 * caller's buffer: BGRA8 rows, opaque, without the pointer. In: data and
 * capacity. Out: width, height, stride_bytes (width * 4), format and
 * data_size (stride_bytes * height); when the pixels do not fit,
 * RCS_ERROR_BUFFER_TOO_SMALL with these filled in and data left alone. */
typedef struct rcs_picture {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    rcs_pixel_format format;
    uint8_t* data;
    uint64_t capacity;
    uint64_t data_size;
} rcs_picture;
#define RCS_PICTURE_INIT {sizeof(rcs_picture), 0, 0, 0, RCS_PIXEL_BGRA8, NULL, 0, 0}
RCS_INLINE rcs_picture rcs_picture_init(void) {
    rcs_picture value = RCS_PICTURE_INIT;
    return value;
}

/* Takes a picture of what `what` shares on this machine, now: for a module
 * (rcsdk/thumbnail.h takes one where a session captures) and for the host's
 * own choice of what to share. `what` is the RCS_EXT_CAPTURE extension a
 * session would get, alone (next NULL), of source DISPLAY, REGION, WINDOW or
 * APPLICATION (HOST and ENCODED have nothing of the SDK's to capture:
 * INVALID_ARGUMENT). It is
 * scaled to fit max_width x max_height (each 1..8192) keeping its aspect
 * ratio, and never enlarged, so 4 * max_width * max_height bytes always take
 * it. Each call captures anew and keeps nothing; it takes tens of
 * milliseconds, the first of a process longer: call it off the UI thread.
 *
 * `what` is checked as a session's capture is: a source this system does not
 * offer is UNSUPPORTED, no screen recording permission PERMISSION_REQUIRED, a
 * display rcs_list_displays does not list or a region not inside its display
 * INVALID_ARGUMENT. Where a running capture would pause, INVALID_STATE says
 * why: a window minimized, hidden or closed, an application without a window
 * on the display or quit, a desktop that cannot be captured now (a lock
 * screen, a display asleep; a secure desktop without secure_desktop). TIMEOUT
 * when no picture comes within three seconds. Each field is carried out or
 * refused: exclude_current_process (DISPLAY, REGION) leaves this process's
 * windows out on macOS; on Windows, where one picture of desktop duplication
 * cannot leave them out, it is UNSUPPORTED.
 *
 * Windows lets a process duplicate each display once at a time: the picture
 * of a display that a capture of this process shares (a running session's,
 * or its agent's) is taken by that capture, from the desktop as it
 * duplicates it, and is INVALID_STATE while it is paused (TIMEOUT while its
 * stream is held up); a display another part of the process duplicates, or
 * four other processes do (Windows' limit), is INVALID_STATE too. Windows a
 * capture of this process hides (exclude_current_process) are hidden from
 * every capture meanwhile, pictures included. A window is captured with
 * Windows.Graphics.Capture, whose yellow border Windows 10 shows around it
 * meanwhile. */
RCSDK_API rcs_result rcs_capture_picture(const rcs_capture_config* what, uint32_t max_width, uint32_t max_height,
                                         rcs_picture* out);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_MODULE_H */
