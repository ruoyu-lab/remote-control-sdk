/* Remote Control SDK: the agent in another process.
 *
 * A sharing session whose capture, encoding and input run in another process
 * of the host's (another account, another Windows session), and that process's
 * side: rcs_agent_* and, under a supervisor, rcs_helper_*. */
#ifndef RCSDK_AGENT_H
#define RCSDK_AGENT_H

#include <rcsdk/rcsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which processes an agent link takes, besides the key that both ends must
 * hold. The session's (rcs_agent_link_config) says who may open its endpoint:
 *   PRIVILEGED  The user the session runs as; on Windows also SYSTEM and
 *               (elevated) administrators. Elsewhere the socket's mode is 0600.
 *   ANY_USER    Every user of this machine, too. For an agent that runs as an
 *               ordinary user beside a session that runs as SYSTEM or root.
 *               On Windows they may connect but not serve the pipe in the
 *               session's place; elsewhere the mode is 0666.
 * The agent's (rcs_agent_config) says which session it serves, as the system
 * reports the process that serves the endpoint:
 *   PRIVILEGED  One that runs as the agent's own account, as LocalSystem or
 *               root, or as an elevated administrator.
 *   ANY_USER    Any that holds the key. */
typedef uint32_t rcs_agent_access;
enum { RCS_AGENT_ACCESS_PRIVILEGED = 0, RCS_AGENT_ACCESS_ANY_USER = 1 };

/* Where a sharer's machine-bound work runs: capturing and encoding the
 * picture, capturing the sound the machine plays, following the pointer and
 * the displays, and executing remote input. Without this extension (or with an
 * empty endpoint) it runs in the session's own process. With an endpoint it
 * runs in an agent: another process of this machine that attaches to the
 * session through that endpoint (rcs_agent_create). The session keeps what is
 * not tied to the machine: the connection, identity, negotiation, bitrate
 * control, data channels and events. That lets a service hold the connection
 * (and run without a desktop, a GPU or screen-recording permission) while an
 * agent in the user's login session does the capturing; when the agent leaves,
 * is replaced or restarts, the viewer stays connected and the picture comes
 * back from a keyframe.
 *
 * Fixed for a run. Share sessions only.
 *
 * A run stays in STARTING until the first agent has attached and passed its
 * checks, then opens its port (LISTENING). The agent's machine decides what the
 * settings can do (codecs, displays, permissions, sound backends), so those
 * checks are made when an agent attaches: a configuration the agent cannot
 * carry out ends the run with the reason (UNSUPPORTED, PERMISSION_REQUIRED...),
 * as start() would without an agent, also while it waits for a viewer. When
 * the agent leaves later (its user logged off, it crashed, it restarts, or it
 * failed: its capture could not follow a new desktop, its encoder broke), the
 * run goes on and viewers stay or still connect: the picture is paused
 * (RCS_EVENT_CAPTURE), control is not granted, the sound is inactive
 * (RCS_EVENT_AUDIO), and everything resumes, from a keyframe, when the next
 * agent attaches. The SDK does not give up on its own: RCS_EVENT_AGENT reports
 * agents attaching and leaving, with why one left (rcs_agent_info.result), and
 * the host decides how long to wait. The session's own agent (no endpoint)
 * is different: its failure ends the run.
 *
 * Who attaches: the system tells the session which process connected, as
 * which account and running which program (rcs_agent_info); one that holds
 * the key and speaks this library's version is offered to on_agent_attach,
 * which decides (without it, every one is taken). On macOS the system names
 * the process that used the connection last: one that handed its connection
 * to another process, or ran another program, between connecting and its
 * handshake is refused (RCS_ERROR_AUTHENTICATION) before on_agent_attach. A newer agent that is taken replaces the one before,
 * which is told so and ends (RCS_ERROR_REPLACED), whatever its reconnect
 * setting. Connections that fail are dropped, those of an account that keeps
 * failing wait a little longer each time, and the log sums them up.
 *
 * Not combined with capture source HOST (the host's own pictures are encoded
 * in the session's process) or the local frame callbacks on_frame and
 * on_gpu_frame (a GPU image does not leave the agent's process); sound source
 * HOST and input target HOST do not involve the agent. exclude_current_process
 * (picture and sound) means the agent's process.
 *
 * The link is a Windows named pipe (endpoint: its name) or a Unix domain
 * socket (endpoint: an absolute path, beside which the session keeps
 * "<endpoint>.lock" while it runs), opened with the access `access` says, and
 * inside it the SDK's secure channel keyed with `key`, which both ends must
 * hold: the key decides who may serve this session, the access setting who may
 * even try. Both ends must run this library's version; an agent of another
 * version learns so first thing (RCS_ERROR_UNSUPPORTED).
 *
 * The socket's directory must belong to the session's account or root and let
 * no other account change its entries: not writable by others, nor by its
 * group (on Android, but by the application's own), unless it is sticky as
 * /tmp is. The run fails with RCS_ERROR_NETWORK, saying why, where it is not
 * (another account could put its own file or link where the socket or the
 * lock is made), and where another process holds the endpoint's name: the
 * reason names whose socket or lock it is, or on Windows which process has the
 * pipe (where the pipe's access setting lets the session ask). Agents never
 * serve such a process (`access`), but the session cannot start: a host keeps
 * the name out of other accounts' reach (a directory only it may change, a
 * pipe name made anew for each run and handed to the agent). */
typedef struct rcs_agent_link_config {
    rcs_extension header;
    char endpoint[128];
    uint8_t key[32];
    rcs_agent_access access;
    void* user_data;
    /* An agent that passed the handshake asks to attach; its process, OS
     * session and account come from the operating system (agent->attached is
     * 0). Nonzero accepts it, and it replaces the agent attached now; 0
     * refuses it: the agent hears RCS_ERROR_AUTHENTICATION and tries again
     * after its reconnect interval (an account refused again and again waits
     * longer each time). NULL accepts every agent holding the key. Called on
     * the link's thread, never while the SDK holds a lock the host could
     * need; return promptly. */
    int32_t (*on_agent_attach)(const rcs_agent_info* agent, void* user_data);
} rcs_agent_link_config;
#define RCS_AGENT_LINK_CONFIG_INIT {{RCS_EXT_AGENT_LINK, sizeof(rcs_agent_link_config), NULL}, {0}, {0}, RCS_AGENT_ACCESS_PRIVILEGED, NULL, NULL}
RCS_INLINE rcs_agent_link_config rcs_agent_link_config_init(void) {
    rcs_agent_link_config value = RCS_AGENT_LINK_CONFIG_INIT;
    return value;
}


/* Share: the machine the session captures (see rcs_agent_info), and its
 * displays: what rcs_list_displays lists for this process, for the agent's
 * process. UNSUPPORTED for a viewing session. */
RCSDK_API rcs_result rcs_session_get_agent_info(rcs_session session, rcs_agent_info* info);

/* The agent of a sharing session that runs in a process of its own
 * (rcs_agent_link_config): it attaches to the session through the endpoint,
 * says what its machine offers, and then does what the session asks: captures
 * and encodes the picture, captures the sound, executes input. Run it where the
 * screen is, e.g. in the user's login session. Capture, codec and control
 * settings are the session's; nothing of them is configured here. When the link
 * ends (the session stopped or restarted) the agent's capture, sound and held
 * keys end with it, and it attaches again. When the agent itself fails (its
 * capture could not follow a new desktop, its encoder broke) it tells the
 * session why, which goes on without it, and attaches again the same way.
 *
 * Before anything else the agent checks which process serves the endpoint
 * (`access`), so that a process of another account that took the endpoint's
 * name never gets its attention. Both ends must run this library's version;
 * the agent's own process needs the permissions its machine asks for (macOS:
 * screen recording and accessibility for the agent's program). */

typedef struct rcs_agent_config {
    uint32_t struct_size;
    uint32_t version; /* RCSDK_VERSION of the headers the host was built with. */
    char endpoint[128]; /* The session's rcs_agent_link_config endpoint... */
    uint8_t key[32];    /* ...and key. */
    /* Which session it serves (see rcs_agent_access): PRIVILEGED (default),
     * one that runs as this process's account, LocalSystem / root or an
     * elevated administrator; ANY_USER, any that holds the key. */
    rcs_agent_access access;
    /* 1: when the link ends, the session is not there yet or refuses this
     * agent, connect again every reconnect_interval_ms (100..60 000) until
     * stopped. 0: end. */
    uint32_t reconnect;
    uint32_t reconnect_interval_ms;
    /* Optional chain, copied by rcs_agent_create: RCS_EXT_LOG, and
     * RCS_EXT_HOST_ENCODER (the agent encodes; the session learns its codecs
     * when it attaches). An extension of any other type is INVALID_ARGUMENT. */
    const rcs_extension* extensions;
} rcs_agent_config;
#define RCS_AGENT_CONFIG_INIT                                                                                          \
    {sizeof(rcs_agent_config), RCSDK_VERSION, {0}, {0}, RCS_AGENT_ACCESS_PRIVILEGED, 1u, 1000u, NULL}
RCS_INLINE rcs_agent_config rcs_agent_config_init(void) {
    rcs_agent_config value = RCS_AGENT_CONFIG_INIT;
    return value;
}

/* What an agent reports, as a session does (rcs_event): the member named after
 * the type is valid. Borrowed for the duration of on_event.
 *   STATE  state.value and state.result:
 *     CONNECTING    looking for the session (result: why the last try failed, or RCS_OK: e.g.
 *                   AUTHENTICATION for a session of an account `access` does not take, or one whose host refused
 *                   this agent; the log says it once)
 *     RUNNING       attached; the session drives this agent
 *     RECONNECTING  the link ended (result: why, e.g. this agent's own failure such as CAPTURE); trying again
 *     STOPPED       rcs_agent_stop; without reconnect, the session closed the link (result DISCONNECTED);
 *                   or another agent replaced this one at the session (result REPLACED: whatever reconnect
 *                   says, it ends; the host decides whether to start it again)
 *     FAILED        the session runs another version (UNSUPPORTED: trying again changes nothing), or without
 *                   reconnect the link failed (result) */
typedef uint32_t rcs_agent_event_type;
enum { RCS_AGENT_EVENT_STATE = 1 };
typedef struct rcs_agent_event {
    uint32_t struct_size;
    rcs_agent_event_type type;
    uint64_t timestamp_ns; /* Local monotonic clock. */
    union {
        rcs_state_event state;
    };
} rcs_agent_event;

typedef struct rcs_agent_callbacks {
    uint32_t struct_size;
    void* user_data;
    /* On an SDK thread, one at a time; return promptly; start/stop/destroy
     * from it return RCS_ERROR_CALLBACK_THREAD. */
    void (*on_event)(const rcs_agent_event* event, void* user_data);
} rcs_agent_callbacks;
#define RCS_AGENT_CALLBACKS_INIT {sizeof(rcs_agent_callbacks), NULL, NULL}
RCS_INLINE rcs_agent_callbacks rcs_agent_callbacks_init(void) {
    rcs_agent_callbacks value = RCS_AGENT_CALLBACKS_INIT;
    return value;
}

/* Copies config and callbacks. The agent is idle until rcs_agent_start. */
RCSDK_API rcs_result rcs_agent_create(const rcs_agent_config* config, const rcs_agent_callbacks* callbacks,
                                      rcs_agent* out_agent);
/* Starts asynchronously; the states are reported through on_event. */
RCSDK_API rcs_result rcs_agent_start(rcs_agent agent);
/* Ends the link and the agent's capture; joins its threads. */
RCSDK_API rcs_result rcs_agent_stop(rcs_agent agent);
RCSDK_API rcs_result rcs_agent_destroy(rcs_agent agent);
RCSDK_API rcs_result rcs_agent_get_state(rcs_agent agent, rcs_state* out_state);
/* Why the last try or link failed, or the agent ended: the result (RCS_OK when
 * nothing failed) and a description; message may be NULL with capacity 0. */
RCSDK_API rcs_result rcs_agent_get_failure(rcs_agent agent, rcs_result* out_result, char* message,
                                           uint32_t capacity);
/* Blocks until the agent has ended (STOPPED or FAILED) and on_event has
 * reported that state: RCS_OK, or RCS_ERROR_TIMEOUT after timeout_ms
 * (UINT32_MAX: no limit). Not from a callback. */
RCSDK_API rcs_result rcs_agent_wait(rcs_agent agent, uint32_t timeout_ms);

/* ------------------------------------------------------------------------ */
/* A supervisor's helper (Windows)                                          */
/* ------------------------------------------------------------------------ */

/* A supervisor (rcsdk/supervisor.h, RemoteControl::Supervisor) keeps a helper
 * process of the host's service running in a session that has a desktop.
 * What it hands the helper is taken here, in the helper; the helper's sessions
 * ask it for Ctrl+Alt+Del by themselves (RCS_INPUT_SECURE_ATTENTION). */

/* In a helper the supervisor started: waits until the supervisor asks it to end
 * (the service stops, the helper moves to another session, or its session is
 * no longer shown): RCS_OK; then stop the sessions
 * (rcs_session_stop_with_reason, RCS_STOP_RESTART) and exit.
 * RCS_ERROR_TIMEOUT after timeout_ms (UINT32_MAX: no limit); UNSUPPORTED when
 * this process was not started by a supervisor. */
RCSDK_API rcs_result rcs_helper_wait_for_stop(uint32_t timeout_ms);
/* In a helper the supervisor started: the supervisor's helper_key (size: 32).
 * The first call takes it from the supervisor; later ones return the same.
 * UNSUPPORTED when this process was not started by a supervisor, or its
 * supervisor handed it no key. */
RCSDK_API rcs_result rcs_helper_get_key(uint8_t* key, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_AGENT_H */
