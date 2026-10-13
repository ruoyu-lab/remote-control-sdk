/* Remote Control SDK: the host's own transport.
 *
 * RCS_TRANSPORT_HOST carries the session's datagrams over a channel the host
 * already has, and RCS_TRANSPORT_FORWARDER with link RCS_FORWARDER_HOST its
 * hop to the forwarder (both ends: one channel, route 0); this header is all
 * they need. */
#ifndef RCSDK_HOST_TRANSPORT_H
#define RCSDK_HOST_TRANSPORT_H

#include <rcsdk/rcsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HOST: what the host's channel carries.
 *   DATAGRAM  Messages, each kept whole: they may be lost, reordered or
 *             repeated (the connection copes), or arrive reliably (a WebRTC
 *             data channel either way, QUIC datagrams, the host's own UDP
 *             relay, WebSocket messages). Each on_transport_send is one
 *             message; each rcs_session_transport_receive passes one.
 *   STREAM    Reliable, ordered bytes without message boundaries (TCP, TLS,
 *             a QUIC stream, an SSH tunnel). The SDK frames its datagrams in
 *             the stream; the host moves the bytes in order, in any pieces. */
typedef uint32_t rcs_host_channel;
enum { RCS_HOST_CHANNEL_DATAGRAM = 0, RCS_HOST_CHANNEL_STREAM = 1 };
#define RCS_HOST_MIN_DATAGRAM 1136u
#define RCS_HOST_MAX_DATAGRAM 65535u

/* RCS_TRANSPORT_HOST's channel (both ends; fixed for a run).
 *
 * Routes: a sharer's channel leads to each of its viewers under a number of
 * the host's own, the route (a connection of its server, a user, a pointer;
 * the SDK never reads it), which comes with what arrives
 * (rcs_session_transport_receive) and goes with what is sent. The first
 * bytes of a route not seen before are a new viewer's handshake, as a new
 * address is over UDP: up to 16 handshakes run at once, the rest wait their
 * turn, and the viewer that joins keeps its route (rcs_viewer_info.route).
 * A route carries one connection at a time: a viewer that connects anew on
 * it ends the one before; a host that gives a route to another peer calls
 * rcs_session_transport_closed for it first. A viewer's channel leads to the
 * sharer alone: its route is 0; so is the channel of either end's hop to a
 * forwarder (RCS_FORWARDER_HOST), whose viewers come through the hop.
 *
 * host_channel: what the channel is; both ends use the same. A STREAM is one
 *   stream per route, framed on its own.
 * host_max_datagram: the largest datagram this end sends over it: 0 is 1200
 *   (passes any IP path); RCS_HOST_MIN_DATAGRAM (what the SDK's datagrams fit
 *   in over every path it takes, and QUIC's datagrams carry) up to
 *   RCS_HOST_MAX_DATAGRAM, e.g. 16384 over WebSocket messages for fewer of
 *   them. Each end sends up to its own and takes up to RCS_HOST_MAX_DATAGRAM.
 *
 * on_transport_send: send these bytes to the other end on `route` over the
 * host's channel (required; a viewer's route is 0).
 * DATAGRAM: one message of `size` bytes (at most host_max_datagram), to be
 * delivered as one message. STREAM: the next bytes of the route's stream
 * (whole frames), to be written in order. Nonzero: the channel took them (it
 * may still lose a datagram on the way); 0: it has no room now, and the
 * connection counts them as lost (resends what it resends, the adaptive
 * bitrate comes down; rcs_network_stats.host_refused). A STREAM channel
 * takes all of the bytes or none. Called on the connection's thread, one call
 * at a time per route (a sharer's calls for different routes may overlap),
 * at the pace the connection sends: return at once, never wait for room.
 * rcs_session_transport_receive may be called from inside it. When the
 * connection ends, its last datagrams tell the peer so: deliver them as the
 * others. */
typedef struct rcs_host_transport_config {
    rcs_extension header;
    rcs_host_channel host_channel;
    uint32_t host_max_datagram;
    void* user_data;
    int32_t (*on_transport_send)(uint64_t route, const uint8_t* data, uint32_t size, void* user_data);
} rcs_host_transport_config;
#define RCS_HOST_TRANSPORT_CONFIG_INIT                                                                                 \
    {{RCS_EXT_HOST_TRANSPORT, sizeof(rcs_host_transport_config), NULL}, RCS_HOST_CHANNEL_DATAGRAM, 0u, NULL, NULL}
RCS_INLINE rcs_host_transport_config rcs_host_transport_config_init(void) {
    rcs_host_transport_config value = RCS_HOST_TRANSPORT_CONFIG_INIT;
    return value;
}

/* HOST: what arrived from the other end on `route` over the host's channel,
 * from any thread; the SDK copies it and returns. A sharer's route names the
 * viewer's channel (see rcs_host_transport_config); a viewer's is 0.
 * DATAGRAM: one message, 1 to RCS_HOST_MAX_DATAGRAM bytes, as it came (calls
 * may overlap). STREAM: the next bytes of the route's stream, any amount, in
 * order (one call at a time per route). INVALID_STATE unless the session
 * runs (start to the end of the run); UNSUPPORTED for a session of another
 * transport (FORWARDER: of another link); INVALID_ARGUMENT for a route other
 * than 0 on a viewer or a forwarder's hop. QUEUE_FULL
 * when the session fell behind (it reads what arrives at once, so a host
 * sees this only when the machine stalls), and on a sharer for a route that
 * sent more than its handshake takes (64 KiB) or when 1 024 routes wait for
 * theirs: the datagram is dropped, as a network drops one; a stream that
 * misses bytes ends the route's connection (NETWORK) and starts over. */
RCSDK_API rcs_result rcs_session_transport_receive(rcs_session session, uint64_t route, const uint8_t* data,
                                                   uint32_t size);
/* HOST: the host's channel to the other end on `route` closed for good (the
 * host's server lost the peer) or now leads to another end. The connection
 * over it reads what arrived for it before (pass the last bytes, then this:
 * the other end's goodbye among them ends it as the goodbye says), then
 * ends as a broken path would (NETWORK): on a sharer that viewer leaves, the
 * others stay; a viewer reconnects (rcs_reconnect_config) over the channel
 * the host restores. A sharer still waiting for viewers goes on waiting.
 * The rest of what waits is dropped, and the STREAM starts over: call this
 * before passing the bytes of a new stream. Without it a silent channel is
 * a lost path after idle_timeout_ms (rcs_network_tuning_config), which the
 * connection outlives for a while (rcs_paths_config). A channel the host
 * replaced with one to the same end: rcs_session_transport_restarted.
 * Over a forwarder's hop it ends the hop: a viewer reconnects, a sharer that
 * keeps listening reaches the forwarder anew, over the channel the host opens
 * next (a WebSocket or TCP connection to an entry is one hop).
 * INVALID_STATE unless the session runs; UNSUPPORTED for another transport
 * (FORWARDER: of another link); INVALID_ARGUMENT for a route other than 0 on
 * a viewer or a forwarder's hop. */
RCSDK_API rcs_result rcs_session_transport_closed(rcs_session session, uint64_t route);
/* HOST: the host's channel on `route` was replaced by a new one to the same
 * other end (a WebSocket that closed and connected again, a WebRTC data
 * channel after an ICE restart, another server of the host's). The
 * connection goes on over the new one, its keys, viewer and messages in
 * flight as they were: a STREAM drops what it holds of a datagram cut short
 * and reads the new stream from its first byte (call this before passing
 * it); what was lost meanwhile the connection sends again. A DATAGRAM channel
 * needs nothing but the news: the round trip and adaptive bitrate start over
 * on the new path. Both report RCS_PATH_CHANGED (cause HOST). With
 * rcs_paths_config.migrate 0 on either end the connection ends as with
 * rcs_session_transport_closed. A forwarder's hop goes on only where the
 * forwarder sees the same peer (its UDP port, through the host's relay): a
 * new connection to its TCP or WebSocket entry is a new peer there, which
 * does not know the hop, and the hop ends (call
 * rcs_session_transport_closed instead).
 * INVALID_STATE unless the session runs; UNSUPPORTED for another transport
 * (FORWARDER: of another link); INVALID_ARGUMENT for a route other than 0 on
 * a viewer or a forwarder's hop. */
RCSDK_API rcs_result rcs_session_transport_restarted(rcs_session session, uint64_t route);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_HOST_TRANSPORT_H */
