/* Remote Control SDK: the supervisor (Windows), an official component:
 * RemoteControl::Supervisor, beside the SDK (RemoteControl::Core) it builds on.
 *
 * A service cannot capture a screen: Windows runs services in session 0, which
 * has no desktop. A supervisor, run by the host's service, keeps one helper
 * process of the host running in a session that has one: the session at the
 * machine's console, or one the host chooses (os_session, e.g. a Remote
 * Desktop session; rcs_list_os_sessions). It runs the helper as LocalSystem
 * (it can then follow secure desktops: rcs_capture_config) or as the
 * signed-in user, and starts it again when it ends, when the console moves to
 * another session (sign-out, fast user switching), when the first session
 * appears at boot, or when the chosen session is shown again (a Remote
 * Desktop client reconnects). What the helper is, is the host's: the sharing
 * session itself, or an rcs_agent beside a session the service holds
 * (rcs_agent_link_config); the viewer's connection stays with the service in
 * the second case and reconnects (RCS_STOP_RESTART makes that immediate) in
 * the first.
 *
 * The SDK installs and registers nothing: the host's service (LocalSystem)
 * creates the supervisor, and the command line names a program only
 * administrators can change (the supervisor starts it with its own rights).
 * The supervisor gives the helper what the rcs_helper_* functions use, through
 * environment variables that name the supervisor's process too, so the SDK
 * takes them only in a process whose parent is that supervisor (and removes
 * them from the helper's environment when it first reads them; their handles
 * are not inherited further):
 *   RCSDK_SUPERVISOR_STOP  an inherited event that is set when the helper
 *                          should end: rcs_helper_wait_for_stop waits on it.
 *   RCSDK_SUPERVISOR_SAS   the helper's inherited end of a pipe through which
 *                          its sessions send Ctrl+Alt+Del
 *                          (RCS_INPUT_SECURE_ATTENTION), which only a service
 *                          in session 0 can send; the supervisor sends it to
 *                          the session of the helper's own token. Each helper
 *                          has a pipe of its own, connected before it starts:
 *                          no other process (of its account or another) can
 *                          open it, ask in its place or keep it from asking.
 *   RCSDK_SUPERVISOR_KEY   an inherited pipe holding helper_key, read once by
 *                          rcs_helper_get_key; never in the text of the
 *                          command line or the environment.
 * Where the system has no such thing (everywhere but Windows; macOS uses
 * launchd) rcs_supervisor_create returns UNSUPPORTED. The handle,
 * rcs_supervisor, is named in rcsdk.h, as log records name it; the helper's
 * side (rcs_helper_*) is in the SDK itself (rcsdk/agent.h), as the helper's
 * sessions use it. */
#ifndef RCSDK_SUPERVISOR_H
#define RCSDK_SUPERVISOR_H

#include <rcsdk/rcsdk.h>

#if defined(RCSDK_STATIC)
#define RCSDK_SUPERVISOR_API
#elif defined(_WIN32)
#if defined(RCSDK_BUILDING_SUPERVISOR)
#define RCSDK_SUPERVISOR_API __declspec(dllexport)
#else
#define RCSDK_SUPERVISOR_API __declspec(dllimport)
#endif
#else
#define RCSDK_SUPERVISOR_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The sessions of this Windows machine as Remote Desktop Services lists them:
 * the one at the console, Remote Desktop sessions, session 0 (where services
 * run) and the listener that waits for Remote Desktop connections. What each
 * shows now (Windows' connection states, grouped):
 *   ACTIVE        Someone is signed in, and the session is shown at the console
 *                 or to a connected client (also while it shadows another).
 *   CONNECTED     Shown, but nobody is signed in: the sign-in screen, at the
 *                 console or in a connected client.
 *   DISCONNECTED  Someone is signed in, but no client is connected: nothing is
 *                 shown, so nothing can be captured or controlled there until
 *                 a client, or the console, connects to it again.
 *   NO_DESKTOP    Never shows one: session 0 and the listener.
 *   OTHER         Shows nothing for now: an idle session waiting for a client,
 *                 one connecting, initialising or being reset, one that is down.
 * A supervisor's helper runs in an ACTIVE or CONNECTED one. */
typedef uint32_t rcs_os_session_state;
enum {
    RCS_OS_SESSION_ACTIVE = 1,
    RCS_OS_SESSION_CONNECTED = 2,
    RCS_OS_SESSION_DISCONNECTED = 3,
    RCS_OS_SESSION_NO_DESKTOP = 4,
    RCS_OS_SESSION_OTHER = 5
};
typedef struct rcs_os_session_info {
    uint32_t struct_size;
    uint32_t os_session; /* Its number, as rcs_agent_info.os_session gives it. */
    rcs_os_session_state state;
    uint32_t is_console; /* Nonzero for the one at the machine's console now... */
    uint32_t is_remote;  /* ...and where Windows reports a remote protocol (Remote Desktop), not the console's. */
    char account[128];   /* UTF-8 "DOMAIN\user" signed in; empty when nobody is. */
    /* A remote session's client: its computer's name and its address
     * ("192.0.2.7", "2001:db8::7") as the client itself reports them; empty
     * where it does not (the console; through a Remote Desktop Gateway or from
     * the Microsoft Store client there is no address). Behind NAT or a VPN the
     * address is not the one that connected: it proves nothing about who did. */
    char client_name[128];
    char client_address[64];
} rcs_os_session_info;
#define RCS_OS_SESSION_INFO_INIT {sizeof(rcs_os_session_info), 0, 0, 0, 0, {0}, {0}, {0}}
RCS_INLINE rcs_os_session_info rcs_os_session_info_init(void) {
    rcs_os_session_info value = RCS_OS_SESSION_INFO_INIT;
    return value;
}

/* Two passes, as rcs_list_displays; sessions come and go, so a second pass may
 * find more (BUFFER_TOO_SMALL). Querying another user's session needs Remote
 * Desktop Services' Query Information right on it (LocalSystem has it on all):
 * without it a session may be missing, or its text empty. UNSUPPORTED off
 * Windows. */
RCSDK_SUPERVISOR_API rcs_result rcs_list_os_sessions(rcs_os_session_info* os_sessions, uint32_t item_size,
                                                     uint32_t capacity, uint32_t* out_count);

typedef uint32_t rcs_helper_account;
enum {
    RCS_HELPER_SYSTEM = 0, /* LocalSystem: runs in the sign-in screen too, follows secure desktops. */
    RCS_HELPER_USER = 1    /* The user signed in to the session it runs in; none while the sign-in screen is up. */
};

#define RCS_MAX_COMMAND_LINE 1024
typedef struct rcs_supervisor_config {
    uint32_t struct_size;
    uint32_t version; /* RCSDK_VERSION of the headers the host was built with. */
    /* UTF-8: the program and its arguments, as for CreateProcess. The program
     * is a full path on one of this machine's drives with its extension, in
     * quotes when it holds a space ("\"C:\\Program Files\\MyApp\\helper.exe\"
     * --helper"); it is started as named, never searched for. Anything else
     * is INVALID_ARGUMENT, a network share's path too (\\server\share\...,
     * \\?\..., a drive letter a share is mapped to): whoever can write there
     * would run as LocalSystem. */
    char command_line[RCS_MAX_COMMAND_LINE];
    rcs_helper_account account;
    /* Where the helper runs. 0: in the session at the machine's console, moving
     * with it. Another value: in that session (rcs_list_os_sessions), wherever
     * the console is. The helper runs there while the session is ACTIVE or
     * CONNECTED; once it is not (its client disconnected, it signs out) the
     * helper is asked to end at once, and started again at once when the
     * session is shown again. A session that has ended stays ended, even when
     * a later session gets its number, until rcs_supervisor_update names one
     * again or the supervisor starts again. 0xFFFFFFFF names no session:
     * INVALID_ARGUMENT. */
    uint32_t os_session;
    /* A secret each helper is handed (rcs_helper_get_key), e.g. the key of the
     * agent link the service's session listens with, made anew for each run of
     * the service; all zero: none. */
    uint8_t helper_key[32];
    /* A helper that ends by itself is started again after restart_delay_ms
     * (100..60 000); the delay doubles each further time it ends within 30
     * seconds of starting, up to restart_delay_max_ms (at least
     * restart_delay_ms). Starts that fail are tried again the same way. The
     * delays start over when the helper's session changes or is shown again;
     * one moved to another session, or one waiting for somebody to sign in,
     * is started there at once. */
    uint32_t restart_delay_ms;
    uint32_t restart_delay_max_ms;
    /* When the supervisor stops or moves the helper it first asks it to end
     * (RCSDK_SUPERVISOR_STOP), and ends it after this long (0..60 000). */
    uint32_t stop_grace_ms;
} rcs_supervisor_config;
#define RCS_SUPERVISOR_CONFIG_INIT                                                                                     \
    {sizeof(rcs_supervisor_config), RCSDK_VERSION, {0}, RCS_HELPER_SYSTEM, 0, {0}, 2000u, 30000u, 3000u}
RCS_INLINE rcs_supervisor_config rcs_supervisor_config_init(void) {
    rcs_supervisor_config value = RCS_SUPERVISOR_CONFIG_INIT;
    return value;
}

typedef uint32_t rcs_supervisor_event_type;
enum {
    RCS_SUPERVISOR_NO_SESSION = 1,     /* No session the helper can run in: the console has none yet; os_session is
                                          not shown (disconnected, no desktop) or has ended; or, for the user account,
                                          nobody is signed in. message says which. Not a failure. */
    RCS_SUPERVISOR_HELPER_STARTED = 2, /* os_session, process_id */
    RCS_SUPERVISOR_HELPER_EXITED = 3,  /* os_session, process_id, exit_code */
    RCS_SUPERVISOR_HELPER_FAILED = 4,  /* The helper could not be started (or was ended before it ran: it could not
                                          be tied to the supervisor): result and message say why; it is tried again. */
    RCS_SUPERVISOR_FAILED = 5          /* The supervisor stopped on an error (result, message): no helper runs until
                                          rcs_supervisor_stop and rcs_supervisor_start. */
};
typedef struct rcs_supervisor_event {
    uint32_t struct_size;
    rcs_supervisor_event_type type;
    uint64_t timestamp_ns; /* Local monotonic clock. */
    uint32_t os_session;   /* The session it concerns. */
    uint32_t process_id;
    uint32_t exit_code;
    rcs_result result;
    char message[256]; /* UTF-8. */
} rcs_supervisor_event;

typedef struct rcs_supervisor_callbacks {
    uint32_t struct_size;
    void* user_data;
    /* On the supervisor's own thread, one at a time; return promptly;
     * start/stop/destroy from it return RCS_ERROR_CALLBACK_THREAD. */
    void (*on_event)(const rcs_supervisor_event* event, void* user_data);
} rcs_supervisor_callbacks;
#define RCS_SUPERVISOR_CALLBACKS_INIT {sizeof(rcs_supervisor_callbacks), NULL, NULL}
RCS_INLINE rcs_supervisor_callbacks rcs_supervisor_callbacks_init(void) {
    rcs_supervisor_callbacks value = RCS_SUPERVISOR_CALLBACKS_INIT;
    return value;
}

/* The helper now. */
typedef struct rcs_supervisor_status {
    uint32_t struct_size;
    uint32_t helper_running;
    uint32_t os_session;
    uint32_t process_id;
    uint64_t starts; /* Helpers started, and ended, since start. */
    uint64_t exits;
} rcs_supervisor_status;
#define RCS_SUPERVISOR_STATUS_INIT {sizeof(rcs_supervisor_status), 0, 0, 0, 0, 0}
RCS_INLINE rcs_supervisor_status rcs_supervisor_status_init(void) {
    rcs_supervisor_status value = RCS_SUPERVISOR_STATUS_INIT;
    return value;
}

/* Copies config and callbacks. INVALID_ARGUMENT for a config that cannot be
 * met (a command line whose program is no full path with its extension, an
 * account no version defines, os_session 0xFFFFFFFF), PERMISSION_REQUIRED when
 * this process is not LocalSystem (either account needs it: moving a token to
 * another session, and taking the signed-in user's), UNSUPPORTED off Windows. */
RCSDK_SUPERVISOR_API rcs_result rcs_supervisor_create(const rcs_supervisor_config* config,
                                                      const rcs_supervisor_callbacks* callbacks,
                                                      rcs_supervisor* out_supervisor);
/* Starts watching the session os_session names and keeping a helper in it. */
RCSDK_SUPERVISOR_API rcs_result rcs_supervisor_start(rcs_supervisor supervisor);
/* Asks the helper to end, ends it after stop_grace_ms, and joins the thread. */
RCSDK_SUPERVISOR_API rcs_result rcs_supervisor_stop(rcs_supervisor supervisor);
RCSDK_SUPERVISOR_API rcs_result rcs_supervisor_destroy(rcs_supervisor supervisor);
RCSDK_SUPERVISOR_API rcs_result rcs_supervisor_get_status(rcs_supervisor supervisor,
                                                          rcs_supervisor_status* status);
/* Takes os_session, restart_delay_ms, restart_delay_max_ms and stop_grace_ms
 * from config, checked as rcs_supervisor_create checks them, also while the
 * supervisor runs (from on_event too). A helper running elsewhere than the new
 * os_session says is moved as when the console moves: asked to end, ended
 * after stop_grace_ms, started there at once. After the chosen session ended,
 * an update, even with the same os_session, takes the session that has that
 * number now. command_line, account and helper_key are the supervisor's for
 * its life: a config whose ones differ from those it was created with is
 * INVALID_ARGUMENT. */
RCSDK_SUPERVISOR_API rcs_result rcs_supervisor_update(rcs_supervisor supervisor,
                                                      const rcs_supervisor_config* config);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_SUPERVISOR_H */
