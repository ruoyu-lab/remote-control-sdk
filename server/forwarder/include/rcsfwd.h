/* Remote Control SDK: the forwarder, RCS_TRANSPORT_FORWARDER's server
 * (RemoteControl::Forwarder; the rcs_forwarder program runs it).
 *
 * For groups: a sharer sends each picture once, sealed with a stream key that
 * only the viewers it let in hold, and the forwarder gives each viewer its
 * copy, with resends, parity, pacing and adaptive bitrate of its own for that
 * viewer's path, moving one whose path falls far behind to a lower encoding
 * at once. It never sees a picture, a stream key or what a viewer and its
 * sharer tell each other: each viewer's end-to-end connection with the sharer
 * (handshake, admission, control, input, data, sound, the pointer) passes
 * through it as it is, and of a sealed picture it reads the header only
 * (stream, encoding, layer, keyframe), which the seal covers. Who may use it
 * is the host's server's word: a token it signs per end (rcsfwd_sign_token,
 * with a secret it shares with the forwarder; or the host checks tokens
 * itself, verify_token). The host's control plane stays the host's: the
 * forwarder knows rooms by the name a token gives, ends by the route the
 * token gives, and nothing else of who they are.
 *
 * A room has one sharer (the first that publishes; a second is refused) and
 * up to max_viewers_per_room viewers, who may come before it. A viewer's
 * route reaches the sharer as rcs_viewer_info.route: the forwarder vouches
 * for it, nothing end to end does. It serves up to max_rooms rooms at once.
 *
 * Entries: UDP (listen_udp) for every end that can use it; TCP (listen_tcp)
 * and WebSocket (listen_websocket: RFC 6455, ws://, any path; what a browser
 * page reaches) for ends behind networks that drop UDP. The hop is the same
 * on each (datagrams, each behind its two-byte length on TCP, each a binary
 * message on a WebSocket). The forwarder speaks no TLS: for wss:// it runs
 * behind a TLS-terminating proxy (nginx, Caddy, IIS) that passes WebSocket
 * connections on to listen_websocket, with a certificate the ends' systems
 * (and browsers) trust; the hop inside is authenticated end to end with the
 * forwarder's identity either way, so TLS only gets it through networks
 * that pass HTTPS alone (docs/guide/24-forwarder.md).
 *
 * A viewer that comes later starts at once: the forwarder keeps each
 * encoding's newest keyframe and the pictures after it (cache_ms, sealed as
 * they came), and sends them to a viewer whose stream key opens them (the
 * sharer hands a newcomer the key of that keyframe's step: it opens at most
 * cache_ms of what came before it; none, with the sharer's from_admission).
 * Without one, it asks the sharer for a keyframe, once for all that wait.
 *
 * A separate library and program beside the SDK, of the same version: the
 * ends and the forwarder run the same version, which the hop's handshake
 * names (magic RCSFWD); rcsfwd_config.version is the RCSDK_VERSION of these
 * headers, compared exactly. Every function may be called from any thread;
 * the callbacks run on the forwarder's threads and must not block. */
#ifndef RCSDK_RCSFWD_H
#define RCSDK_RCSFWD_H

#include <rcsdk/rcsdk.h>

#if defined(RCSFWD_STATIC)
#define RCSFWD_API
#elif defined(_WIN32)
#if defined(RCSDK_BUILDING_FORWARDER)
#define RCSFWD_API __declspec(dllexport)
#else
#define RCSFWD_API __declspec(dllimport)
#endif
#else
#define RCSFWD_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Identity ----------------------------------------------------------- */

/* The forwarder proves this key in every handshake; the ends check the
 * SHA-256 of its public half (rcs_forwarder_config.forwarder_key_sha256): a
 * P-256 key pair, 0x04 || X || Y || d, as a device key (RCS_DEVICE_KEY_SIZE). */
#define RCSFWD_IDENTITY_KEY_SIZE 97u
RCSFWD_API rcs_result rcsfwd_generate_identity(uint8_t key[RCSFWD_IDENTITY_KEY_SIZE]);
/* INVALID_ARGUMENT unless `key` is such a key pair. */
RCSFWD_API rcs_result rcsfwd_public_key_sha256(const uint8_t key[RCSFWD_IDENTITY_KEY_SIZE], uint8_t sha256[32]);

/* ---- Tokens ------------------------------------------------------------- */

/* What an end may do: publish (share into the room) or subscribe (view it). */
typedef uint32_t rcsfwd_role;
enum { RCSFWD_ROLE_PUBLISH = 1, RCSFWD_ROLE_SUBSCRIBE = 2 };
#define RCSFWD_ROOM_SIZE 65u

/* A token's word: room (1 to 64 bytes, no NUL; prefer an opaque one, a hash
 * or a random number, to a chat's or user's own id: the forwarder's operator
 * reads it), role, the host's route for the end (the sharer hears a viewer's
 * as rcs_viewer_info.route), when it expires (Unix time, seconds), for a
 * viewer the most the forwarder sends it (0: max_viewer_bitrate_bps), and
 * the forwarder it is for: the SHA-256 of its public key
 * (rcsfwd_public_key_sha256, what the ends check), required. Forwarders that
 * share a secret take no token of each other's. */
typedef struct rcsfwd_grant {
    uint32_t struct_size;
    char room[RCSFWD_ROOM_SIZE];
    rcsfwd_role role;
    uint64_t route;
    uint64_t expires;
    uint32_t max_bitrate_bps;
    uint8_t audience[32];
} rcsfwd_grant;
#define RCSFWD_GRANT_INIT {sizeof(rcsfwd_grant), {0}, RCSFWD_ROLE_SUBSCRIBE, 0u, 0u, 0u, {0}}

/* A secret the host's server signs tokens with, and its number, which the
 * token names (a forwarder may hold two while the server moves to a new one). */
typedef struct rcsfwd_secret {
    uint32_t key_id;
    uint8_t secret[32];
} rcsfwd_secret;

/* A token for `grant`, as the host's server makes one: "rcsf1." then the
 * grant in base64url (RFC 4648, no padding) as fixed big-endian fields:
 * version 1 (1), key_id (4), role (1), route (8), expires (8),
 * max_bitrate_bps (4), audience (32), a random nonce (16), the room's length
 * (1) and bytes; then "." and the base64url HMAC-SHA256 under the secret of
 * "rcsf1." and those fields. Any language makes it with a few lines; this is
 * the C one (rcs_forwarder --sign-token the command line's).
 * Into `token` with its NUL (at most RCS_FORWARDER_TOKEN_SIZE bytes);
 * BUFFER_TOO_SMALL when it does not fit, INVALID_ARGUMENT for a grant no
 * token holds. A token may be used until it expires, as often as its end
 * reconnects; the forwarder keeps no list of tokens. */
RCSFWD_API rcs_result rcsfwd_sign_token(const rcsfwd_secret* secret, const rcsfwd_grant* grant, char* token,
                                       uint32_t capacity);

/* ---- The forwarder ------------------------------------------------------ */

typedef struct rcsfwd_impl* rcsfwd;

/* Where an end came in. */
typedef uint32_t rcsfwd_entry;
enum { RCSFWD_ENTRY_UDP = 1, RCSFWD_ENTRY_TCP = 2, RCSFWD_ENTRY_WEBSOCKET = 3 };

/* What happened, for the host's log and its own bookkeeping:
 *   ROOM_OPENED, ROOM_CLOSED  A room's first end came; its last went (or
 *                             rcsfwd_close_room).
 *   JOINED, LEFT              An end came into a room (`peer`: a viewer's
 *                             number there, 0 for the sharer); one went, and
 *                             why (`result`, `message`).
 *   REFUSED                   An end was turned away (`result`: REFUSED for
 *                             its token or a second sharer, BUSY for a full
 *                             room or forwarder, PROTOCOL for one that broke
 *                             the hop's rules), with the reason it also got. */
typedef uint32_t rcsfwd_event_type;
enum {
    RCSFWD_EVENT_ROOM_OPENED = 1,
    RCSFWD_EVENT_ROOM_CLOSED = 2,
    RCSFWD_EVENT_JOINED = 3,
    RCSFWD_EVENT_LEFT = 4,
    RCSFWD_EVENT_REFUSED = 5
};
typedef struct rcsfwd_event {
    uint32_t struct_size;
    rcsfwd_event_type type;
    char room[RCSFWD_ROOM_SIZE]; /* Empty where none was named yet. */
    rcsfwd_role role;            /* 0 before the end said. */
    uint64_t peer;
    uint64_t route;
    rcs_result result;
    char message[160];
    char address[64]; /* The end's address and port, as they reach the forwarder (behind a proxy: the proxy's). */
    uint32_t port;
    rcsfwd_entry entry; /* Where the end came in. */
} rcsfwd_event;

/* The forwarder's settings, fixed once created.
 * listen_udp, listen_tcp, listen_websocket: where each entry listens,
 *   "address:port", numeric (IPv4 or [IPv6]); port 0 picks one
 *   (rcsfwd_get_port says which); empty: no such entry. At least one.
 * identity_key: its key pair (rcsfwd_generate_identity). Required.
 * secrets, secret_count: the secrets tokens are signed with, 1..4 of
 *   distinct key_id; copied. Or verify_token instead (not both): the host
 *   checks each token (text, NUL-terminated) and fills the grant (struct_size
 *   set; its audience is not looked at), returning nonzero to accept; called
 *   on a client's thread, one token at a time per client.
 * max_rooms: rooms at once; 0 is 64, otherwise 1..65 536.
 * max_viewers_per_room: 0 is 64, otherwise 1..RCS_MAX_VIEWERS.
 * max_ends: ends connected at once, in rooms or not yet, over every entry
 *   (each a reader thread and its hop's); 0 is 1024, otherwise 1..65 536. A
 *   TCP or WebSocket connection counts from the moment it is accepted.
 * max_viewer_bitrate_bps: the most the forwarder sends one viewer; 0 is
 *   100 Mbit/s, otherwise 500 000..1 000 000 000. A grant's own is below it.
 * max_pipe_bps: what a pipe carries each way between a viewer and its sharer
 *   (control, input, data, the pointer), beyond which datagrams are dropped
 *   (counted): 0 is 50 Mbit/s, otherwise 64 000..1 000 000 000. A datagram
 *   counts as at least 256 bytes.
 * max_publish_bitrate_bps: the most a sharer's pictures and sound bring into
 *   its room, beyond which they are dropped (counted; its viewers recover as
 *   from loss): 0 is 400 Mbit/s, otherwise 1 000 000..4 000 000 000.
 * cache_ms: how long each encoding's newest keyframe, and the pictures after
 *   it, stay for viewers that come later; 0 is 3000, otherwise 100..60 000.
 * cache_bytes: what a room keeps so, all its encodings together, beyond
 *   which an encoding's is dropped until its next keyframe; 0 is 16 MiB,
 *   otherwise 1 MiB..1 GiB.
 * max_cache_bytes: what every room keeps together, likewise; 0 is 256 MiB,
 *   otherwise cache_bytes..64 GiB.
 * on_event, user_data: optional. */
typedef struct rcsfwd_config {
    uint32_t struct_size;
    uint32_t version; /* RCSDK_VERSION of the headers the host was built with. */
    char listen_udp[64];
    char listen_tcp[64];
    char listen_websocket[64];
    uint8_t identity_key[RCSFWD_IDENTITY_KEY_SIZE];
    const rcsfwd_secret* secrets;
    uint32_t secret_count;
    int32_t (*verify_token)(const char* token, rcsfwd_grant* grant, void* user_data);
    uint32_t max_rooms;
    uint32_t max_viewers_per_room;
    uint32_t max_ends;
    uint32_t max_viewer_bitrate_bps;
    uint32_t max_pipe_bps;
    uint32_t max_publish_bitrate_bps;
    uint32_t cache_ms;
    uint32_t cache_bytes;
    uint64_t max_cache_bytes;
    void (*on_event)(const rcsfwd_event* event, void* user_data);
    void* user_data;
} rcsfwd_config;
#define RCSFWD_CONFIG_INIT                                                                                             \
    {sizeof(rcsfwd_config), RCSDK_VERSION, {0}, {0}, {0}, {0}, NULL, 0u, NULL, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,     \
     NULL, NULL}

/* INVALID_ARGUMENT naming the field for settings no version defines, an
 * identity that is no key pair, a version or size of other headers. */
RCSFWD_API rcs_result rcsfwd_create(const rcsfwd_config* config, rcsfwd* out);
/* Binds every entry and serves (NETWORK when an address cannot be bound);
 * INVALID_STATE while it serves. */
RCSFWD_API rcs_result rcsfwd_start(rcsfwd forwarder);
/* Every end is told the forwarder stops (they fail as on a broken path, and
 * reconnect as their settings say); the port closes. Returns once the
 * forwarder's threads ended; it may start again. */
RCSFWD_API rcs_result rcsfwd_stop(rcsfwd forwarder);
/* Stops it first. Not from its own callbacks. */
RCSFWD_API void rcsfwd_destroy(rcsfwd forwarder);
/* The port an entry listens on (INVALID_STATE unless it serves,
 * INVALID_ARGUMENT for an entry it has not). */
RCSFWD_API rcs_result rcsfwd_get_port(rcsfwd forwarder, rcsfwd_entry entry, uint32_t* port);

/* The forwarder's counters since it was created. */
typedef struct rcsfwd_stats {
    uint32_t struct_size;
    uint32_t rooms;
    uint32_t ends;          /* Connected now, in rooms or not yet. */
    uint64_t refused;       /* Ends turned away: tokens, a second sharer, full. */
    uint64_t rejected;      /* Handshakes that failed (another magic or version), and ends that broke the hop's rules. */
    uint64_t hello_timeouts; /* Ends that said nothing within 5 s of their handshake. */
    uint32_t connections;    /* TCP and WebSocket connections now. */
    /* TCP and WebSocket connections closed for breaking the rules: no
     * WebSocket opening (or a bad one) within 5 s, a frame RFC 6455 forbids,
     * no hop within 10 s, one beyond max_ends. */
    uint64_t connections_refused;
    uint64_t cache_bytes; /* What every room keeps for viewers that come later. */
} rcsfwd_stats;
#define RCSFWD_STATS_INIT {sizeof(rcsfwd_stats), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}
RCSFWD_API rcs_result rcsfwd_get_stats(rcsfwd forwarder, rcsfwd_stats* stats);

/* A room's counters (INVALID_ARGUMENT for a room it does not have). */
typedef struct rcsfwd_room_stats {
    uint32_t struct_size;
    uint32_t sharer;  /* 1 while its sharer is there. */
    uint32_t viewers; /* There now. */
    uint64_t media_frames, media_bytes; /* Sealed pictures from the sharer, and their bytes. */
    uint64_t sent_frames, sent_bytes;   /* Pictures handed to the viewers' hops, all together, and their bytes. */
    uint64_t pipe_bytes;                /* Carried through the pipes, both ways. */
    uint64_t pipe_dropped;              /* Pipe datagrams over a pipe's limit. */
    uint64_t moves_down;                /* Viewers moved to a lower encoding on the spot. */
    uint64_t sound_packets, sound_bytes; /* Sealed sound from the sharer, and its bytes. */
    uint64_t publish_dropped;           /* Pictures and sound over max_publish_bitrate_bps. */
    uint64_t cache_starts;              /* Streams a viewer started on from what was kept... */
    uint64_t keyframes_asked;           /* ...and keyframes asked of the sharer instead, for every viewer waiting. */
    uint64_t cache_bytes;               /* Kept now. */
    /* Pictures the sharer's uplink lost (the hop gave up on them): the
     * stream's viewers start again at the next keyframe, asked for. */
    uint64_t media_lost;
} rcsfwd_room_stats;
#define RCSFWD_ROOM_STATS_INIT                                                                                         \
    {sizeof(rcsfwd_room_stats), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u}
RCSFWD_API rcs_result rcsfwd_get_room_stats(rcsfwd forwarder, const char* room, rcsfwd_room_stats* stats);

/* Each viewer of a room: `items` of `item_size` (sizeof(rcsfwd_viewer_stats));
 * `*count` says how many there are. BUFFER_TOO_SMALL when they do not fit
 * (`*count` says how many would). */
typedef struct rcsfwd_viewer_stats {
    uint32_t struct_size;
    uint64_t peer;
    uint64_t route;
    uint32_t streams;  /* What its sharer lets it have ((1u << stream) bits)... */
    uint32_t encoding; /* ...of which encoding. */
    uint32_t target_bps;    /* Its path's adaptive target (0: not measured yet). */
    uint32_t loss_permille;
    uint64_t round_trip_ns;
    uint64_t backlog_ns;    /* Pictures waiting to go to it. */
    uint64_t sent_frames, skipped_frames, sent_bytes;
    uint64_t moves_down;
    char address[64];
    uint32_t port;
    rcsfwd_entry entry;
    uint32_t sound; /* Which of the sharer's sounds it gets (0: none). */
} rcsfwd_viewer_stats;
#define RCSFWD_VIEWER_STATS_INIT                                                                                       \
    {sizeof(rcsfwd_viewer_stats), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, {0}, 0u, 0u, 0u}
RCSFWD_API rcs_result rcsfwd_list_viewers(rcsfwd forwarder, const char* room, rcsfwd_viewer_stats* items,
                                          uint32_t item_size, uint32_t capacity, uint32_t* count);

/* Ends every connection of the room (they fail as on a broken path); a
 * token for it may open it again. INVALID_ARGUMENT for a room it does not have. */
RCSFWD_API rcs_result rcsfwd_close_room(rcsfwd forwarder, const char* room);

/* Why the calling thread's last call failed, NUL-terminated into `message`. */
RCSFWD_API void rcsfwd_get_last_error(char* message, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_RCSFWD_H */
