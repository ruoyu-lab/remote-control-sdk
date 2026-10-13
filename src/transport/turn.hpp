// TURN client (RFC 8656) for UDP relaying through a standard TURN server such
// as coturn: an allocation with long-term credentials, permissions for peers,
// channel bindings for 4-byte-overhead data, and the refreshes that keep them.
// The relay forwards datagrams that stay end-to-end encrypted. The socket to
// the server is UDP, or a TCP connection (turn_tcp_socket.hpp) where only TCP
// passes. Where the server grants one (RFC 8016: coturn with `mobility`;
// 4.15 or later, whose takeover checks the ticket's owner), a mobility ticket
// moves the allocation to another socket of this end's (another network):
// the relayed address, permissions and channels stay, and the peer notices
// nothing.
#pragma once

#include "transport/socket.hpp"
#include "transport/stun.hpp"

#include <map>
#include <optional>
#include <string>

namespace rcsdk::transport {

class TurnClient {
public:
    TurnClient(Endpoint server, std::string username, std::string password);

    // Allocates a relayed address, using `socket` (which only this client
    // reads until it returns). Throws NETWORK (no allocation within
    // `timeout`) or AUTHENTICATION (credentials refused) with the server's
    // reason. With a username, the server must ask for the credentials: an
    // allocation granted without them is not taken (anyone on the path could
    // have answered so), and AUTHENTICATION says so at `timeout`.
    Endpoint allocate(DatagramSocket& socket, CancelToken cancelled, std::chrono::milliseconds timeout);

    const Endpoint& server() const noexcept { return server_; }
    const Endpoint& relayed() const noexcept { return relayed_; }

    // Whether the server granted a mobility ticket.
    bool mobile() const noexcept { return !ticket_.empty(); }
    // Moves the allocation to `socket`, another 5-tuple of this end's: a
    // Refresh with the ticket from there, answered there (receive() with it;
    // refused: Failure as receive() says). Each answer brings the next ticket.
    void move(DatagramSocket& socket);

    // Lets `peer` send to the relayed address and binds a channel to it.
    void open(DatagramSocket& socket, const Endpoint& peer);
    // Whether open() let `peer` in.
    bool permitted(const Endpoint& peer) const { return peers_.contains(key(peer)); }
    // Sends to `peer` through the relay: channel data once bound, a Send
    // indication before; marked `dscp` towards the server when not 0
    // (DatagramSocket::sendMarked). True when it went out marked.
    bool send(DatagramSocket& socket, const Endpoint& peer, std::span<const uint8_t> data, uint8_t dscp = 0);
    // What send() adds to a datagram for `peer` now: the channel data header
    // once bound, at most a Send indication's framing before.
    size_t overhead(const Endpoint& peer) const;
    // A datagram from the server: relayed data (with its peer), or nothing
    // for the client's own responses, which it handles. Throws when the
    // server refuses a request that keeps the relay working (a Refresh, a
    // permission, a channel): AUTHENTICATION for credentials it no longer
    // accepts (401, 403, 441), NETWORK otherwise (also a stale nonce, 438,
    // for a request already sent again with the new one); the reason names
    // the request and the server's error.
    std::optional<std::pair<Endpoint, std::span<const uint8_t>>> receive(DatagramSocket& socket,
                                                                         std::span<const uint8_t> datagram);
    // Refreshes the allocation (halfway through the lifetime the server
    // grants, a second on at least), permissions and channels before they
    // expire, and repeats unanswered requests; throws NETWORK when one stays
    // unanswered (five attempts in about 15 s).
    void tick(DatagramSocket& socket, Clock::time_point now);
    // Releases the allocation (best effort).
    void release(DatagramSocket& socket);

    // Channel data is 0x40..0x7F in its first byte.
    static bool isChannelData(std::span<const uint8_t> datagram) noexcept;

private:
    struct Request {
        stun::Message message;
        Clock::time_point sent;
        int attempts = 0;
        bool renewed = false; // Sent again after a stale nonce (438).
    };
    struct Peer {
        Endpoint address;
        uint16_t channel = 0;
        bool bound = false;
        Clock::time_point permitted{}, channelRefreshed{};
    };

    void submit(DatagramSocket& socket, stun::Message message, bool renewed = false);
    void transmit(DatagramSocket& socket, Request& request);
    void authenticate(stun::Message& message);
    std::span<const uint8_t> integrityKey() const;
    std::string key(const Endpoint& peer) const { return peer.toString(); }
    stun::Message request(uint16_t method) const;

    Endpoint server_, relayed_;
    std::string username_, password_, realm_, nonce_;
    crypto::Bytes key_, ticket_; // ticket_: RFC 8016's, the latest the server gave; empty: none.
    uint32_t lifetime_ = 600;
    bool allocated_ = false;
    Clock::time_point refreshAt_{}; // refreshAfter() the lifetime the server last granted.
    std::map<stun::TransactionId, Request> pending_;
    std::map<std::string, Peer> peers_;
    uint16_t nextChannel_ = 0x4000;
};

} // namespace rcsdk::transport
