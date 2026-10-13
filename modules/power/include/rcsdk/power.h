/* Remote Control SDK: the machine's power, an official module
 * (rcsdk/module.h).
 *
 * Three things, each off until the host turns it on:
 *
 * Keeping awake. Whether a machine stays awake during a session. A sharing
 * machine, so that a viewer can reach and see it: a machine that went to
 * sleep is gone, and a display that switched off shows nothing. A viewing one,
 * so that its display stays on while someone watches it: an iPad that
 * locked stops the picture. A session that uses the module asks its machine
 * for it while a peer is connected (a sharer: any viewer; a viewer: its
 * sharer), or for the whole run:
 *
 *     rcs_register_module(rcs_power_module());     once per process: the sharer's,
 *                                                    and an agent's of another process
 *     rcs_module_config use = RCS_MODULE_CONFIG_INIT;
 *     use.module = RCS_POWER_MODULE;                chained in the session's extensions;
 *                                                    settings NULL takes RCS_POWER_SETTINGS_INIT
 *
 * The machine's half holds a power request with a reason ("Remote session in
 * progress"; Windows: PowerCreateRequest, shown by `powercfg /requests`;
 * macOS: an IOPMAssertion, shown by `pmset -g assertions`; iOS: the
 * application's idle timer, off) on the session's machine: the sharer's
 * agent's, in another process too (rcs_agent_link_config), or the viewer's
 * own. iOS keeps an application running only while its display is on: it
 * takes DISPLAY (the device does not lock) and refuses SYSTEM (UNSUPPORTED).
 * The user's own power button, closing the lid and a battery running low
 * still win. A system that refuses the request is reported (RCS_POWER_CHANGED,
 * result and message), and the session goes on: the host decides what to do
 * about it.
 *
 * Waking. A machine that sleeps or is switched off runs no SDK: only another
 * device on its network can wake it, with a Wake-on-LAN "magic packet" to the
 * MAC address of a network interface that its system (and firmware) keeps
 * listening. The module gives the pieces; who wakes what, from where, is the
 * host's:
 *   - rcs_power_list_interfaces: this machine's interfaces that can wake it
 *     (MAC address, IPv4 address and subnet, Ethernet or Wi-Fi) and, where
 *     the system says, whether waking is turned on there. It only reports:
 *     the module never changes a system or firmware setting.
 *   - share_wake_info: a sharer tells its viewers the same of its machine
 *     (rcs_power_get_peer_interfaces), e.g. for a host to keep with the
 *     machine's record. MAC addresses identify a machine: off by default.
 *   - rcs_power_send_wake: sends the packet from this device, as a UDP
 *     broadcast, to a machine on the same network (the viewer's own device,
 *     or another device of the host's on that network, which its host tells
 *     what to wake).
 *
 * Actions. A viewer asks the sharer's machine to restart, shut down, sleep or
 * sign out its user (rcs_power_request_action). The sharer's host decides:
 * `actions` names what viewers may ask for (none by default), and on_action
 * (or, without it, whether the viewer holds remote control) each request.
 * The machine's half (the sharer's agent, in the session's process or in
 * another: under the supervisor, a Windows service's SYSTEM helper, or a
 * macOS LaunchAgent's agent) checks that its system lets it, answers, and
 * carries it out action_delay_ms later, so that the answer goes out first.
 * Both ends hear each step (RCS_POWER_ACTION). After a restart, a sharer that
 * the host's service starts again listens again (keep_listening): the viewer
 * reconnects or its host starts it again.
 *
 * What each system can do (rcs_power_get_capabilities; a sharer's agent's:
 * rcs_power_status.actions):
 *   Windows  Every action, for a process whose account has the shutdown
 *            privilege (SeShutdownPrivilege: LocalSystem and, on a
 *            workstation, signed-in users): InitiateShutdownW (RESTART,
 *            SHUT_DOWN: a full shutdown, not Fast Startup's), SetSuspendState
 *            (SLEEP), and for SIGN_OUT the user of the process's own Windows
 *            session: WTSLogoffSession with RCS_POWER_FORCE, ExitWindowsEx
 *            without (a process of that user only). A process in session 0
 *            signs out nobody. Waking: GetAdaptersAddresses, and whether the
 *            device may wake the machine as Device Manager sets it.
 *   macOS    SLEEP for root or the user at the console (IOPMSleepSystem).
 *            RESTART, SHUT_DOWN, SIGN_OUT: the console user's process asks
 *            loginwindow (applications are asked to quit, and one may keep it
 *            from happening), or with RCS_POWER_FORCE signs its own user out
 *            (`launchctl reboot logout`); a root process carries out
 *            RCS_POWER_FORCE only (`shutdown -r now`, `shutdown -h now`,
 *            `launchctl bootout gui/<console user>`). Waking: Ethernet and
 *            Wi-Fi interfaces, and "Wake for network access" (pmset's womp).
 *   iOS, Android, a browser  No action (an application may not), no wake
 *            information (a phone or tablet is not woken over the network);
 *            they send wake packets (iOS: broadcasts need the multicast
 *            networking entitlement; a browser sends nothing).
 * What a system cannot do is UNSUPPORTED with the reason, never done another
 * way. */
#ifndef RCSDK_POWER_H
#define RCSDK_POWER_H

#include <rcsdk/module.h>

#if defined(RCSDK_STATIC)
#define RCSDK_POWER_API
#elif defined(_WIN32)
#if defined(RCSDK_BUILDING_POWER)
#define RCSDK_POWER_API __declspec(dllexport)
#else
#define RCSDK_POWER_API __declspec(dllimport)
#endif
#else
#define RCSDK_POWER_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RCS_POWER_MODULE "rcsdk.power"

/* ---- Keeping awake --------------------------------------------------------- */

/* What stays awake.
 *   OFF      Nothing: the machine's own power settings decide.
 *   SYSTEM   The system does not go to sleep (the display may switch off).
 *   DISPLAY  Neither the system nor the display does. */
typedef uint32_t rcs_keep_awake;
enum { RCS_KEEP_AWAKE_OFF = 0, RCS_KEEP_AWAKE_SYSTEM = 1, RCS_KEEP_AWAKE_DISPLAY = 2 };

/* ---- Waking ---------------------------------------------------------------- */

/* The kind of a network interface. */
typedef uint32_t rcs_power_link;
enum { RCS_POWER_LINK_ETHERNET = 1, RCS_POWER_LINK_WIFI = 2 };
/* Whether the system lets an interface wake the machine, as far as it says:
 * Windows the device's "Allow this device to wake the computer" (matched by
 * the device's name), macOS "Wake for network access" (one setting for
 * every interface). Firmware settings (waking from switched off, from deep
 * sleep) are not seen. */
typedef uint32_t rcs_power_wake_setting;
enum { RCS_POWER_WAKE_UNKNOWN = 0, RCS_POWER_WAKE_ENABLED = 1, RCS_POWER_WAKE_DISABLED = 2 };

#define RCS_POWER_INTERFACES_MAX 8u /* The most interfaces a sharer tells its viewers. */

/* A network interface that can wake its machine: a physical Ethernet or
 * Wi-Fi interface that is up, with an IPv4 address. */
typedef struct rcs_power_interface {
    uint8_t mac[6];
    rcs_power_link link;
    rcs_power_wake_setting wake;
    char address[16];       /* Its IPv4 address, dotted ("192.168.1.20"). */
    uint32_t prefix_length; /* Its subnet's: 0..32. */
    char broadcast[16];     /* The subnet's broadcast address ("192.168.1.255"), for rcs_power_wake.address. */
    char name[64];          /* The system's name for it ("Ethernet", "Wi-Fi"), UTF-8, cut short to fit. */
} rcs_power_interface;

/* A magic packet to send (rcs_power_send_wake): six 0xFF bytes and the MAC
 * address sixteen times, then the SecureOn password, if any. */
typedef struct rcs_power_wake {
    uint32_t struct_size;
    uint8_t mac[6]; /* The interface to wake: a unicast address (not all zeros). */
    /* Where the UDP datagram goes: an IPv4 address, dotted. NULL takes
     * 255.255.255.255, which leaves through one interface the system picks;
     * the subnet's broadcast address (rcs_power_interface.broadcast) leaves
     * through the interface on that subnet. A unicast address works where
     * the network still knows the machine's MAC address. */
    const char* address;
    /* 1..65535; default 9 (discard). An interface listening for the packet
     * finds it in any port's datagram; 7 (echo) and 9 are the usual ones,
     * which routers that forward wake packets expect. */
    uint32_t port;
    uint32_t password_size; /* 0 (default), 4 or 6: the SecureOn password some network cards ask for. */
    uint8_t password[6];
} rcs_power_wake;
#define RCS_POWER_WAKE_INIT {sizeof(rcs_power_wake), {0, 0, 0, 0, 0, 0}, NULL, 9u, 0u, {0, 0, 0, 0, 0, 0}}
RCS_INLINE rcs_power_wake rcs_power_wake_init(void) {
    rcs_power_wake value = RCS_POWER_WAKE_INIT;
    return value;
}

/* ---- Actions --------------------------------------------------------------- */

/* What a viewer may ask the sharer's machine to do.
 *   RESTART    Restart the machine.
 *   SHUT_DOWN  Switch it off (a full shutdown).
 *   SLEEP      Put it to sleep.
 *   SIGN_OUT   Sign out the user signed in where the machine's half runs
 *              (Windows: its Windows session; macOS: the console's user). */
typedef uint32_t rcs_power_action;
enum { RCS_POWER_RESTART = 1, RCS_POWER_SHUT_DOWN = 2, RCS_POWER_SLEEP = 3, RCS_POWER_SIGN_OUT = 4 };
#define RCS_POWER_ALL_ACTIONS                                                                                          \
    ((1u << RCS_POWER_RESTART) | (1u << RCS_POWER_SHUT_DOWN) | (1u << RCS_POWER_SLEEP) | (1u << RCS_POWER_SIGN_OUT))
/* Bits in a request's flags.
 *   FORCE  Applications that would hold up RESTART, SHUT_DOWN or SIGN_OUT
 *          (unsaved work) are closed, their work lost; without it they are
 *          asked, and one may keep it from happening (it waits for someone at
 *          the machine). SLEEP is the same either way. */
enum { RCS_POWER_FORCE = 1u << 0 };

/* Where a request is (rcs_power_action_event.state):
 *   REFUSED   It does not happen: result says why. REFUSED: the sharer's host
 *             does not allow it (`actions`, on_action, a viewer without
 *             remote control); UNSUPPORTED: the machine cannot (its system,
 *             its rights, the flags: the message says); INVALID_STATE: the
 *             machine's half does not run (no agent now), or nobody is
 *             signed in to sign out; BUSY: another action waits to be
 *             carried out there.
 *   ACCEPTED  The machine carries it out in delay_ms.
 *   STARTED   The system took it: the machine restarts, shuts down, goes to
 *             sleep or signs out now. It may never arrive: the machine went.
 *   FAILED    The system refused it when the time came (result, message), or
 *             what happened is not known: DISCONNECTED (view: the connection
 *             ended before the sharer answered), INVALID_STATE (share: the
 *             machine's half left before it answered). */
typedef uint32_t rcs_power_action_state;
enum {
    RCS_POWER_ACTION_REFUSED = 1,
    RCS_POWER_ACTION_ACCEPTED = 2,
    RCS_POWER_ACTION_STARTED = 3,
    RCS_POWER_ACTION_FAILED = 4
};

/* A viewer's request, for the sharer's host to decide on (on_action). */
typedef struct rcs_power_request {
    uint32_t struct_size;
    rcs_viewer_id viewer_id;
    uint64_t request; /* The viewer's number for it (rcs_power_request_action). */
    rcs_power_action action;
    uint32_t flags;       /* RCS_POWER_FORCE */
    uint32_t controlling; /* Nonzero while the viewer holds remote control. */
} rcs_power_request;

/* ---- The module's settings ------------------------------------------------- */

/* rcs_module_config.settings, size sizeof(); NULL with size 0 takes the
 * defaults: the system stays awake while a peer is connected (a viewer that
 * wants its display on, and any iOS session, sets DISPLAY); no wake
 * information goes, and no action is allowed. rcs_session_update changes them
 * while the session runs. */
typedef struct rcs_power_settings {
    uint32_t struct_size;
    rcs_keep_awake keep_awake;
    /* 0: while a peer is connected; 1: from the start of the run to its
     * end, also while waiting for a viewer or connecting. */
    uint32_t while_running;
    /* Share: 1 tells the viewers that use the module this machine's
     * interfaces that can wake it (looked at again as each viewer connects);
     * 0 (default) tells them nothing. A viewer: 0. */
    uint32_t share_wake_info;
    /* Share: the actions viewers may ask for, as (1u << rcs_power_action)
     * bits; 0 (default): none, each request REFUSED. A viewer: 0. */
    uint32_t actions;
    /* Share: how long the machine waits between accepting an action and
     * carrying it out: 1 000..60 000 ms; default 3 000. */
    uint32_t action_delay_ms;
    /* Share: decides on each request that `actions` allows and the machine
     * can carry out: nonzero lets it go ahead, 0 refuses it (REFUSED). It is
     * called on the thread that reads that viewer's connection, one request
     * at a time; copy what is needed and return soon. NULL (default): a
     * viewer that holds remote control may, others may not. */
    int32_t (*on_action)(const rcs_power_request* request, void* user_data);
    void* user_data;
} rcs_power_settings;
#define RCS_POWER_SETTINGS_INIT {sizeof(rcs_power_settings), RCS_KEEP_AWAKE_SYSTEM, 0u, 0u, 0u, 3000u, NULL, NULL}
RCS_INLINE rcs_power_settings rcs_power_settings_init(void) {
    rcs_power_settings value = RCS_POWER_SETTINGS_INIT;
    return value;
}

/* ---- What the module reports ----------------------------------------------- */

/* What the session's machine holds and can do now. */
typedef struct rcs_power_status {
    uint32_t struct_size;
    rcs_keep_awake wanted; /* What the settings ask for now (with a peer connected, or the whole run). */
    rcs_keep_awake held;   /* What the machine's system holds: wanted, or OFF where it could not. */
    /* RCS_OK, or why held is not wanted: the system refused the request
     * (RCS_ERROR_INTERNAL), this system has no such thing (UNSUPPORTED), or
     * the machine's half does not run now (INVALID_STATE: no agent attached
     * yet, or one that cannot run it, which RCS_MODULE_NO_MACHINE explains). */
    rcs_result result;
    char message[128]; /* UTF-8, cut short to fit; empty with RCS_OK. */
    /* The actions the machine's half can carry out, as (1u << rcs_power_action)
     * bits (its system and its rights; a sharer's agent's); 0 while it does
     * not run. */
    uint32_t actions;
} rcs_power_status;
#define RCS_POWER_STATUS_INIT {sizeof(rcs_power_status), RCS_KEEP_AWAKE_OFF, RCS_KEEP_AWAKE_OFF, RCS_OK, {0}, 0u}
RCS_INLINE rcs_power_status rcs_power_status_init(void) {
    rcs_power_status value = RCS_POWER_STATUS_INIT;
    return value;
}

/* What this process can do on its machine (rcs_power_get_capabilities). */
typedef struct rcs_power_capabilities {
    uint32_t struct_size;
    uint32_t keep_awake;  /* 1: its system takes power requests. */
    uint32_t actions;     /* (1u << rcs_power_action) bits it can carry out (with or without FORCE). */
    uint32_t interfaces;  /* 1: rcs_power_list_interfaces lists this machine's interfaces. */
    uint32_t send_wake;   /* 1: rcs_power_send_wake sends from here. */
    char message[256];    /* Why actions or interfaces are not all there (UTF-8); empty when they are. */
} rcs_power_capabilities;
#define RCS_POWER_CAPABILITIES_INIT {sizeof(rcs_power_capabilities), 0u, 0u, 0u, 0u, {0}}
RCS_INLINE rcs_power_capabilities rcs_power_capabilities_init(void) {
    rcs_power_capabilities value = RCS_POWER_CAPABILITIES_INIT;
    return value;
}

/* What a viewer's sharer offers, as it said last (rcs_power_get_peer). */
typedef struct rcs_power_peer {
    uint32_t struct_size;
    /* The actions it may carry out for a viewer: its host allows them
     * (`actions`) and its machine can. A request may still be refused
     * (on_action, remote control). 0 while not connected. */
    uint32_t actions;
    /* 1: it tells its machine's interfaces (share_wake_info), `interfaces`
     * of them (0..RCS_POWER_INTERFACES_MAX), listed by
     * rcs_power_get_peer_interfaces. They stay after the connection ended,
     * until another sharer tells, or the next run starts. */
    uint32_t wake_info;
    uint32_t interfaces;
} rcs_power_peer;
#define RCS_POWER_PEER_INIT {sizeof(rcs_power_peer), 0u, 0u, 0u}
RCS_INLINE rcs_power_peer rcs_power_peer_init(void) {
    rcs_power_peer value = RCS_POWER_PEER_INIT;
    return value;
}

/* A step of a request, at both ends (RCS_POWER_ACTION). */
typedef struct rcs_power_action_event {
    uint64_t request; /* The viewer's number for it. */
    rcs_power_action action;
    uint32_t flags;
    rcs_power_action_state state;
    rcs_result result;  /* RCS_OK for ACCEPTED and STARTED; see rcs_power_action_state. */
    uint32_t delay_ms;  /* ACCEPTED: when the machine carries it out. */
    char message[128];  /* Not RCS_OK: why (UTF-8, cut short to fit). */
} rcs_power_action_event;

/* Its events: RCS_EVENT_MODULE of module RCS_POWER_MODULE.
 *   CHANGED  rcs_power_status: wanted, held, result or actions changed (either end).
 *   ACTION   rcs_power_action_event: a request's step; the sharer's about the
 *            viewer that asked (rcs_event.viewer_id), the viewer's about its sharer.
 *   PEER     rcs_power_peer: what the sharer offers or tells changed (view). */
enum {
    RCS_POWER_CHANGED = RCS_MODULE_EVENT_OWN,
    RCS_POWER_ACTION = RCS_MODULE_EVENT_OWN + 1,
    RCS_POWER_PEER = RCS_MODULE_EVENT_OWN + 2
};

/* ---- Functions ------------------------------------------------------------- */

/* The module, for rcs_register_module. */
RCSDK_POWER_API const rcs_module_definition* rcs_power_module(void);
/* What this process can do on its machine; message says why not the rest. */
RCSDK_POWER_API rcs_result rcs_power_get_capabilities(rcs_power_capabilities* out);
/* What the session's module holds and can do now. INVALID_STATE for a
 * session that does not use the module. */
RCSDK_POWER_API rcs_result rcs_power_get_status(rcs_session session, rcs_power_status* status);

/* This machine's interfaces that can wake it, Ethernet first; two passes, as
 * rcs_list_displays. It asks the system each time (Windows' device list may
 * take a moment: not on a UI thread). UNSUPPORTED where the system has none
 * to tell (iOS, Android, a browser), with the reason. */
RCSDK_POWER_API rcs_result rcs_power_list_interfaces(rcs_power_interface* items, uint32_t item_size,
                                                     uint32_t capacity, uint32_t* out_count);
/* Sends one magic packet from this device; a datagram may be lost, so a host
 * sends a few, a second apart. It returns once the system took it: nothing
 * says the machine woke (its host hears from it when it runs again).
 * INVALID_ARGUMENT for values as no version defines them (a MAC address that
 * is all zeros or a group address, an address that is not a dotted IPv4
 * address or is 0.0.0.0, a port or password size out of range);
 * UNSUPPORTED where this device cannot send (a browser); NETWORK or
 * PERMISSION_REQUIRED (iOS without the multicast entitlement, a local
 * network permission denied) with the system's reason. */
RCSDK_POWER_API rcs_result rcs_power_send_wake(const rcs_power_wake* wake);

/* View: asks the sharer's machine for `action` (flags: RCS_POWER_FORCE); its
 * number in out_request (may be NULL). It returns at once: the answer comes
 * as RCS_POWER_ACTION events (REFUSED, or ACCEPTED then STARTED or FAILED).
 * INVALID_ARGUMENT for an action or flags no version defines; UNSUPPORTED
 * for a sharing session (its host acts on its own machine); INVALID_STATE
 * while not connected to a sharer that uses the module; QUEUE_FULL as
 * rcs_module_send. */
RCSDK_POWER_API rcs_result rcs_power_request_action(rcs_session session, rcs_power_action action, uint32_t flags,
                                                    uint64_t* out_request);
/* View: what its sharer offers and tells (RCS_POWER_PEER says when it
 * changes). INVALID_STATE for a session that does not use the module;
 * UNSUPPORTED for a sharing one. */
RCSDK_POWER_API rcs_result rcs_power_get_peer(rcs_session session, rcs_power_peer* out);
/* View: the interfaces its sharer told (share_wake_info), two passes as
 * rcs_list_displays; 0 of them when it tells none. */
RCSDK_POWER_API rcs_result rcs_power_get_peer_interfaces(rcs_session session, rcs_power_interface* items,
                                                         uint32_t item_size, uint32_t capacity,
                                                         uint32_t* out_count);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_POWER_H */
