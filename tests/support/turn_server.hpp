// A minimal TURN server (RFC 8656) for the SDK's tests, on 127.0.0.1: long-
// term credentials (or none), allocations (one per 5-tuple: another Allocate
// on it is 437), permissions, channel bindings, Send and Data indications,
// over UDP (address()) and over TCP (tcpAddress(); its own port: Windows
// keeps some ports from TCP that UDP may bind), mobility tickets (RFC 8016:
// an allocation moves to the 5-tuple that refreshes with its ticket). Tests
// can make it refuse or lose requests. Not for production: deploy a full
// server such as coturn.
#pragma once

#include "transport/socket_error.hpp"
#include "transport/stun.hpp"
#include "transport/turn_tcp_socket.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace rcsdk::test {

class TurnServer {
public:
    // `bind`: where the server listens; `relays`: where its relays do
    // (defaults to `bind`; another family is how coturn serves IPv4 relays
    // over IPv6); `external`: the address peers reach relays at (defaults to
    // `relays`). An empty username: no authentication.
    TurnServer(std::string username, std::string password, const std::string& bind = "127.0.0.1", uint16_t port = 0,
               std::string external = {}, std::string relays = {})
        : username_(std::move(username)), password_(std::move(password)), bind_(relays.empty() ? bind : relays),
          external_(external.empty() ? bind_ : std::move(external)), socket_(transport::Endpoint::parse(bind, port)),
          listener_(transport::openTcpSocket(socket_.local().family())) {
        const auto tcp = transport::Endpoint::parse(bind, 0);
        if (::bind(listener_.get(), tcp.get(), tcp.size()) != 0 || ::listen(listener_.get(), 8) != 0) {
            fail(RCS_ERROR_NETWORK, "The test TURN server cannot listen on TCP " + tcp.toString());
        }
        thread_ = std::thread([this] { run(); });
    }
    ~TurnServer() {
        stop_ = true;
        thread_.join();
    }

    transport::Endpoint address() const { return socket_.local(); }
    std::string addressText() const { return address().toString(); }
    transport::Endpoint tcpAddress() const {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length);
        return transport::Endpoint::fromSockaddr(reinterpret_cast<const sockaddr*>(&address), size_t(length));
    }
    // The server's address for the given way of reaching it.
    transport::Endpoint address(rcs_turn_transport transport) const {
        return transport == RCS_TURN_TCP ? tcpAddress() : address();
    }
    // The next authenticated request is answered 438: clients must retry.
    void staleNonce() { rotate_ = true; }
    // Requests of `method` get the error `code` from now on.
    void refuse(uint16_t method, int code) {
        std::lock_guard lock(mutex_);
        refused_[method] = code;
    }
    // The next `count` requests of `method` are lost.
    void lose(uint16_t method, int count) {
        std::lock_guard lock(mutex_);
        lost_[method] = count;
    }
    // The connections of the clients over TCP close, as a NAT that forgot
    // them would; their allocations go with them.
    void dropTcp() {
        std::lock_guard lock(mutex_);
        for (const auto& [address, client] : tcp_) {
            allocations_.erase(Client{client.address, true}.key());
        }
        tcp_.clear();
    }
    // The lifetime allocations and refreshes get.
    std::atomic<uint32_t> lifetime{600};
    // How long a released allocation lingers before it is gone, as coturn's
    // does (about a second); an Allocate on its 5-tuple meanwhile is 437.
    std::atomic<int> releaseDelayMs{0};
    // Does as coturn 4.7 does where RFC 8656 leaves room: an Allocate's
    // 5-tuple is checked before its credentials (a new client learns of an
    // allocation before the realm and nonce), and releasing a released
    // allocation puts its deletion off again.
    std::atomic<bool> coturn{false};
    // Some allocation lets `peer` (its address) send to it.
    bool permits(const transport::Endpoint& peer) {
        std::lock_guard lock(mutex_);
        return std::any_of(allocations_.begin(), allocations_.end(),
                           [&](const auto& a) { return a.second.permitted.count(peer.addressBytes()) != 0; });
    }
    // Grants mobility tickets to the Allocates that ask (as coturn with
    // `mobility`); off: none, as coturn without it.
    std::atomic<bool> mobility{true};
    std::atomic<int> moves{0};        // Allocations a ticket moved to another 5-tuple.
    std::atomic<uint64_t> relayed{0}; // Datagrams forwarded either way.
    std::atomic<int> allocations{0};
    std::atomic<int> refreshes{0};  // Refresh requests that arrived (lost or not).
    std::atomic<int> tcpClients{0}; // Connections accepted over TCP.

private:
    struct TcpClient {
        transport::Socket socket;
        transport::Endpoint address;
        std::vector<uint8_t> inbound;
        size_t consumed = 0;
    };
    // A client and how it reaches the server: a TCP client may have the
    // address and port of a UDP one (the port spaces are separate).
    struct Client {
        transport::Endpoint address;
        bool tcp = false;
        std::string key() const { return (tcp ? "tcp " : "udp ") + address.toString(); }
    };

    void sendTo(const Client& to, std::span<const uint8_t> bytes) {
        const auto found = to.tcp ? tcp_.find(to.address.toString()) : tcp_.end();
        if (found == tcp_.end()) {
            if (!to.tcp) {
                socket_.send(to.address, bytes);
            }
            return;
        }
        const auto wire = transport::turnWire(bytes);
        size_t written = 0;
        while (written < wire.size()) {
            const auto result = ::send(found->second.socket.get(), reinterpret_cast<const char*>(wire.data() + written),
#ifdef _WIN32
                                       int(wire.size() - written), 0);
#elif defined(MSG_NOSIGNAL) // A client that went is an error, not SIGPIPE (macOS: SO_NOSIGPIPE on the socket).
                                       wire.size() - written, MSG_NOSIGNAL);
#else
                                       wire.size() - written, 0);
#endif
            if (result > 0) {
                written += size_t(result);
            } else if (result < 0 && transport::wouldBlock()) {
                std::this_thread::sleep_for(1ms);
            } else {
                return; // Gone.
            }
        }
    }

    void acceptTcp() {
        for (;;) {
            sockaddr_storage address{};
            socklen_t length = sizeof(address);
            const auto handle = ::accept(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length);
            if (handle == transport::invalidHandle) {
                return;
            }
            transport::makeNonBlocking(handle);
            const auto from = transport::Endpoint::fromSockaddr(reinterpret_cast<const sockaddr*>(&address), size_t(length));
            tcp_[from.toString()] = TcpClient{transport::Socket(handle), from, {}, 0};
            ++tcpClients;
        }
    }

    // Reads what TCP clients sent and handles each complete message; true when anything arrived.
    bool pumpTcp() {
        bool busy = false;
        for (auto it = tcp_.begin(); it != tcp_.end();) {
            auto& client = it->second;
            std::array<uint8_t, 65536> chunk;
            bool gone = false;
            for (;;) {
#ifdef _WIN32
                const int result = ::recv(client.socket.get(), reinterpret_cast<char*>(chunk.data()), int(chunk.size()), 0);
#else
                const auto result = ::recv(client.socket.get(), chunk.data(), chunk.size(), 0);
#endif
                if (result > 0) {
                    client.inbound.insert(client.inbound.end(), chunk.begin(), chunk.begin() + result);
                    busy = true;
                    continue;
                }
                gone = result == 0 || !transport::wouldBlock();
                break;
            }
            const Client from{client.address, true};
            try {
                for (;;) {
                    const auto pending = std::span<const uint8_t>(client.inbound).subspan(client.consumed);
                    const auto frame = transport::turnFrame(pending);
                    if (!frame || pending.size() < frame->wire) {
                        break;
                    }
                    handle(from, pending.first(frame->message));
                    client.consumed += frame->wire;
                }
            } catch (const Failure&) {
                gone = true; // Not TURN (a TLS hello, say): dropped, as coturn drops it.
            }
            if (client.consumed) {
                client.inbound.erase(client.inbound.begin(), client.inbound.begin() + ptrdiff_t(client.consumed));
                client.consumed = 0;
            }
            if (gone) {
                allocations_.erase(from.key());
                it = tcp_.erase(it);
            } else {
                ++it;
            }
        }
        return busy;
    }

    struct Allocation {
        transport::stun::TransactionId transaction{}; // Of the Allocate that made it.
        std::optional<Clock::time_point> gone;       // Released: when it is deleted.
        Client client;
        std::unique_ptr<transport::UdpSocket> relay;
        std::set<std::vector<uint8_t>> permitted; // Peer addresses (IP only).
        std::map<uint16_t, transport::Endpoint> channels;
        crypto::Bytes ticket; // RFC 8016's; empty: none.
    };

    static crypto::Bytes newTicket() {
        crypto::Bytes ticket(16);
        crypto::random(ticket);
        return ticket;
    }

    crypto::Bytes key() const { return transport::stun::longTermKey(username_, realm_, password_); }

    void respond(const Client& to, const transport::stun::Message& request, uint16_t klass,
                 const std::function<void(transport::stun::Message&)>& fill = {}, int error = 0) {
        transport::stun::Message response;
        response.method = request.method;
        response.klass = klass;
        response.transaction = request.transaction;
        if (error) {
            const std::string reason = error == 401 ? "Unauthorized" : error == 438 ? "Stale Nonce" : "Error";
            crypto::Bytes value{0, 0, uint8_t(error / 100), uint8_t(error % 100)};
            value.insert(value.end(), reason.begin(), reason.end());
            response.add(transport::stun::errorCode, value);
            response.addString(transport::stun::realm, realm_);
            response.addString(transport::stun::nonce, nonce_);
        }
        if (fill) {
            fill(response);
        }
        const auto k = key();
        const bool integrity = !username_.empty() && error != 401;
        sendTo(to, transport::stun::encode(
                       response, integrity ? std::span<const uint8_t>(k) : std::span<const uint8_t>(), false));
    }

    void handle(const Client& from, std::span<const uint8_t> bytes) {
        namespace stun = transport::stun;
        const std::string client = from.key();
        if (bytes.size() >= 4 && bytes[0] >= 0x40 && bytes[0] <= 0x7F) { // Channel data.
            auto found = allocations_.find(client);
            const uint16_t channel = uint16_t(bytes[0] << 8 | bytes[1]);
            const size_t length = size_t(bytes[2] << 8 | bytes[3]);
            if (found != allocations_.end() && found->second.channels.count(channel) && length + 4 <= bytes.size()) {
                found->second.relay->send(found->second.channels[channel], bytes.subspan(4, length));
                ++relayed;
            }
            return;
        }
        const auto message = stun::parse(bytes);
        if (!message) {
            return;
        }
        if (message->method == stun::sendMethod && message->klass == stun::indication) {
            auto found = allocations_.find(client);
            const auto peer = message->address(stun::xorPeerAddress);
            const auto data = stun::attributeIn(bytes, stun::data);
            if (found != allocations_.end() && peer && data && found->second.permitted.count(peer->addressBytes())) {
                found->second.relay->send(*peer, *data);
                ++relayed;
            }
            return;
        }
        if (message->klass != stun::request) {
            return;
        }
        if (message->method == stun::refresh) {
            ++refreshes;
        }
        if (auto lost = lost_.find(message->method); lost != lost_.end() && lost->second > 0) {
            --lost->second;
            return;
        }
        if (coturn && message->method == stun::allocate) {
            const auto found = allocations_.find(client);
            if (found != allocations_.end() &&
                (found->second.transaction != message->transaction || found->second.gone)) {
                respond(from, *message, stun::error, {}, 437);
                return;
            }
        }
        if (!username_.empty()) {
            if (!message->has(stun::messageIntegrity)) {
                respond(from, *message, stun::error, {}, 401);
                return;
            }
            if (message->string(stun::username) != username_ || !stun::verifyIntegrity(bytes, key())) {
                respond(from, *message, stun::error, {}, 401);
                return;
            }
            if (rotate_.exchange(false)) {
                nonce_ = "n" + std::to_string(++nonces_);
            }
            if (message->string(stun::nonce) != nonce_) {
                respond(from, *message, stun::error, {}, 438);
                return;
            }
        }
        if (const auto refused = refused_.find(message->method); refused != refused_.end()) {
            respond(from, *message, stun::error, {}, refused->second);
            return;
        }
        switch (message->method) {
        case stun::allocate: {
            auto& allocation = allocations_[client];
            if (!allocation.relay) {
                allocation.transaction = message->transaction;
                allocation.client = from;
                allocation.relay = std::make_unique<transport::UdpSocket>(transport::Endpoint::parse(bind_, 0));
                ++allocations;
            } else if (allocation.transaction != message->transaction || allocation.gone) {
                respond(from, *message, stun::error, {}, 437); // Not a retransmission.
                break;
            }
            const auto relayed = transport::Endpoint::parse(external_, allocation.relay->local().port());
            if (mobility && message->has(stun::mobilityTicket) && allocation.ticket.empty()) {
                allocation.ticket = newTicket();
            }
            respond(from, *message, stun::success, [&](stun::Message& r) {
                r.addAddress(stun::xorRelayedAddress, relayed);
                r.addAddress(stun::xorMappedAddress, from.address);
                r.addU32(stun::lifetime, lifetime);
                if (!allocation.ticket.empty()) {
                    r.add(stun::mobilityTicket, allocation.ticket);
                }
            });
            break;
        }
        case stun::refresh: {
            auto found = allocations_.find(client);
            if (const auto* ticket = message->find(stun::mobilityTicket); ticket && mobility) {
                // From the same 5-tuple: 400 (RFC 8016 3.2.2); from another: the allocation moves there.
                const auto owner = std::find_if(allocations_.begin(), allocations_.end(), [&](const auto& a) {
                    return !a.second.ticket.empty() && a.second.ticket == *ticket;
                });
                if (found != allocations_.end() || owner == allocations_.end()) {
                    respond(from, *message, stun::error, {}, found != allocations_.end() ? 400 : 437);
                    break;
                }
                auto node = allocations_.extract(owner);
                node.key() = client;
                node.mapped().client = from;
                node.mapped().ticket = newTicket();
                found = allocations_.insert(std::move(node)).position;
                ++moves;
                respond(from, *message, stun::success, [&](stun::Message& r) {
                    r.addU32(stun::lifetime, lifetime);
                    r.add(stun::mobilityTicket, found->second.ticket);
                });
                break;
            }
            if (found == allocations_.end() || (found->second.gone && !coturn)) {
                respond(from, *message, stun::error, {}, 437);
                break;
            }
            if (message->u32(stun::lifetime) == 0u) {
                found->second.gone = Clock::now() + std::chrono::milliseconds(releaseDelayMs.load());
            }
            respond(from, *message, stun::success, [&](stun::Message& r) { r.addU32(stun::lifetime, lifetime); });
            break;
        }
        case stun::createPermission:
        case stun::channelBind: {
            auto found = allocations_.find(client);
            const auto peer = message->address(stun::xorPeerAddress);
            if (found == allocations_.end() || !peer) {
                respond(from, *message, stun::error, {}, 437);
                break;
            }
            found->second.permitted.insert(peer->addressBytes());
            if (message->method == stun::channelBind) {
                found->second.channels[uint16_t(message->u32(stun::channelNumber).value_or(0) >> 16)] = *peer;
            }
            respond(from, *message, stun::success);
            break;
        }
        default:
            break;
        }
    }

    // A peer's datagram to a relayed address goes to its client.
    void relay(Allocation& allocation, const transport::Endpoint& from, std::span<const uint8_t> bytes) {
        namespace stun = transport::stun;
        if (!allocation.permitted.count(from.addressBytes())) {
            return;
        }
        const Client& to = allocation.client;
        for (const auto& [channel, peer] : allocation.channels) {
            if (peer == from) {
                crypto::Bytes data{uint8_t(channel >> 8), uint8_t(channel), uint8_t(bytes.size() >> 8),
                                   uint8_t(bytes.size())};
                data.insert(data.end(), bytes.begin(), bytes.end());
                sendTo(to, data);
                ++relayed;
                return;
            }
        }
        stun::Message indication;
        indication.method = stun::dataMethod;
        indication.klass = stun::indication;
        indication.transaction = stun::newTransaction();
        indication.addAddress(stun::xorPeerAddress, from);
        indication.add(stun::data, bytes);
        sendTo(to, stun::encode(indication, {}, false));
        ++relayed;
    }

    void run() {
        std::array<uint8_t, 2048> buffer;
        while (!stop_) {
            std::unique_lock lock(mutex_);
            bool idle = true;
            transport::Endpoint from;
            while (const auto size = socket_.receive(buffer, from)) {
                handle(Client{from, false}, std::span<const uint8_t>(buffer).first(*size));
                idle = false;
            }
            acceptTcp();
            idle = !pumpTcp() && idle;
            std::erase_if(allocations_, [](const auto& entry) { return entry.second.gone && *entry.second.gone <= Clock::now(); });
            for (auto& [client, allocation] : allocations_) {
                while (const auto size = allocation.relay->receive(buffer, from)) {
                    relay(allocation, from, std::span<const uint8_t>(buffer).first(*size));
                    idle = false;
                }
            }
            lock.unlock();
            if (idle) {
                std::this_thread::sleep_for(1ms);
            }
        }
    }

    std::string username_, password_, bind_, external_, realm_ = "rcsdk.test", nonce_ = "n0";
    int nonces_ = 0;
    std::atomic<bool> rotate_{false};
    std::mutex mutex_; // The server's state, against the test's thread.
    std::map<uint16_t, int> refused_, lost_;
    transport::UdpSocket socket_;
    transport::Socket listener_;
    std::map<std::string, TcpClient> tcp_; // By the client's address.
    std::map<std::string, Allocation> allocations_; // By Client::key().
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace rcsdk::test
