#include "transport/turn.hpp"

#include "util/bytes.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <optional>

namespace rcsdk::transport {
namespace {

using crypto::Bytes;

constexpr auto firstRetransmit = 500ms;
constexpr int maxAttempts = 5;
constexpr auto permissionRefresh = 4min; // Permissions last 5 minutes.
constexpr auto channelRefresh = 9min;    // Channel bindings last 10 minutes.
constexpr uint32_t udpTransport = 17u << 24;
constexpr size_t channelHeader = 4;

// When to refresh what lasts `lifetime` seconds: halfway, and a second on
// at least, so that a server granting none is not asked again at every tick.
Clock::duration refreshAfter(uint32_t lifetime) {
    return std::max<Clock::duration>(std::chrono::seconds(lifetime / 2), 1s);
}

std::string reasonOf(const stun::Message& message) {
    const auto* value = message.find(stun::errorCode);
    const std::string phrase = value && value->size() > 4 ? std::string(value->begin() + 4, value->end()) : "";
    return std::to_string(message.errorCode()) + (phrase.empty() ? "" : " " + phrase);
}

const char* methodName(uint16_t method) {
    switch (method) {
    case stun::allocate:
        return "Allocate";
    case stun::refresh:
        return "Refresh";
    case stun::createPermission:
        return "CreatePermission";
    case stun::channelBind:
        return "ChannelBind";
    default:
        return "request";
    }
}

} // namespace

TurnClient::TurnClient(Endpoint server, std::string username, std::string password)
    : server_(std::move(server)), username_(std::move(username)), password_(std::move(password)) {}

bool TurnClient::isChannelData(std::span<const uint8_t> datagram) noexcept {
    return datagram.size() >= channelHeader && datagram[0] >= 0x40 && datagram[0] <= 0x7F;
}

stun::Message TurnClient::request(uint16_t method) const {
    stun::Message message;
    message.method = method;
    message.klass = stun::request;
    message.transaction = stun::newTransaction();
    return message;
}

// A server without authentication (no realm) gets and gives no integrity.
std::span<const uint8_t> TurnClient::integrityKey() const {
    return realm_.empty() ? std::span<const uint8_t>() : std::span<const uint8_t>(key_);
}

// Long-term credentials once the server named its realm and nonce.
void TurnClient::authenticate(stun::Message& message) {
    std::erase_if(message.attributes, [](const auto& a) {
        return a.first == stun::username || a.first == stun::realm || a.first == stun::nonce;
    });
    if (!realm_.empty()) {
        message.addString(stun::username, username_);
        message.addString(stun::realm, realm_);
        message.addString(stun::nonce, nonce_);
    }
}

void TurnClient::transmit(DatagramSocket& socket, Request& request) {
    socket.send(server_, stun::encode(request.message, integrityKey(), false));
    request.sent = Clock::now();
    ++request.attempts;
}

void TurnClient::submit(DatagramSocket& socket, stun::Message message, bool renewed) {
    authenticate(message);
    auto& request = pending_[message.transaction];
    request.message = std::move(message);
    request.attempts = 0;
    request.renewed = renewed;
    transmit(socket, request);
}

Endpoint TurnClient::allocate(DatagramSocket& socket, CancelToken cancelled, std::chrono::milliseconds timeout) {
    Waiter waiter;
    waiter.watch(socket);
    auto message = request(stun::allocate);
    message.addU32(stun::requestedTransport, udpTransport);
    message.addU32(stun::lifetime, lifetime_);
    message.add(stun::mobilityTicket, {}); // Asks for one (RFC 8016); servers without mobility ignore it.
    bool authenticated = false, renewed = false;
    // A success without integrity while this end has credentials: anyone on
    // the path could have sent it (a server asks for credentials first).
    bool unauthenticated = false;
    // Credentials asked for (401, once) or a stale nonce (438, once in a
    // row): takes the realm and nonce the error names; false for any other
    // error.
    const auto learn = [&](const stun::Message& error) {
        const int code = error.errorCode();
        if (!(code == 401 && !authenticated) && !(code == 438 && !renewed)) {
            return false;
        }
        renewed = code == 438;
        realm_ = error.string(stun::realm);
        nonce_ = error.string(stun::nonce);
        if (realm_.empty() || nonce_.empty()) {
            fail(RCS_ERROR_NETWORK, "The TURN server asked for credentials without a realm or nonce");
        }
        key_ = stun::longTermKey(username_, realm_, password_);
        authenticated = true;
        return true;
    };
    // This local port may still have an allocation (437): one this end
    // released that the server is still deleting (coturn takes about a
    // second), or one an earlier run left behind, e.g. across a lost network
    // or a crash. Only this end can have made it, so it releases it and asks
    // again. coturn answers 437 before asking for credentials, so the release
    // may be what learns them; and it puts the deletion off with every
    // release, so one that was answered is not sent again.
    std::optional<stun::Message> release;
    bool releaseAnswered = false;
    std::optional<stun::Message> releaseRefused;
    const auto sendRelease = [&] {
        releaseAnswered = false;
        release = request(stun::refresh);
        release->addU32(stun::lifetime, 0);
        authenticate(*release);
        socket.send(server_, stun::encode(*release, integrityKey(), false));
    };
    const auto deadline = Clock::now() + timeout;
    auto next = Clock::now();
    std::array<uint8_t, 2048> buffer;
    for (;;) {
        throwIfCancelled(cancelled);
        const auto now = Clock::now();
        if (now >= deadline) {
            const std::string server = "The TURN server " + server_.toString();
            if (unauthenticated) {
                fail(RCS_ERROR_AUTHENTICATION,
                     server + " granted an allocation without asking for the credentials (turn_username): refused, "
                              "since anyone on the path could have answered so; a server without authentication "
                              "takes an empty username");
            }
            if (!release) {
                fail(RCS_ERROR_NETWORK, server + " did not answer within " + secondsText(timeout));
            }
            const bool credentials = releaseRefused && releaseRefused->errorCode() == 401;
            fail(credentials ? RCS_ERROR_AUTHENTICATION : RCS_ERROR_NETWORK,
                 server + " keeps an allocation on this end's local port (437 Allocation Mismatch)" +
                     (releaseRefused ? " and refused to release it: " + reasonOf(*releaseRefused) +
                                           (credentials ? " (check the TURN username and password)" : "")
                                     : " and did not release it"));
        }
        if (now >= next) {
            authenticate(message);
            socket.send(server_, stun::encode(message, integrityKey(), false));
            next = now + firstRetransmit;
        }
        Endpoint from;
        while (const auto size = socket.receive(buffer, from)) {
            const auto bytes = std::span<const uint8_t>(buffer).first(*size);
            const auto response = stun::parse(bytes);
            if (from != server_ || !response) {
                continue;
            }
            if (release && response->transaction == release->transaction) {
                if (response->klass == stun::error && learn(*response)) {
                    sendRelease();
                    continue;
                }
                releaseAnswered = true;
                if (response->klass == stun::error && response->errorCode() != 437) { // 437: already gone.
                    releaseRefused = *response;
                }
                continue;
            }
            if (response->transaction != message.transaction) {
                continue;
            }
            if (response->klass == stun::error) {
                const int code = response->errorCode();
                if (learn(*response)) {
                    message.setTransaction(stun::newTransaction());
                    next = Clock::now();
                    continue;
                }
                if (code == 437) {
                    if (!releaseAnswered) {
                        sendRelease();
                    }
                    renewed = false;
                    message.setTransaction(stun::newTransaction());
                    next = Clock::now() + firstRetransmit;
                    continue;
                }
                fail(code == 401 ? RCS_ERROR_AUTHENTICATION : RCS_ERROR_NETWORK,
                     "The TURN server refused the allocation: " + reasonOf(*response) +
                         (code == 401 ? " (check the TURN username and password)" : ""));
            }
            if (response->klass == stun::success) {
                if (realm_.empty() && !username_.empty()) {
                    unauthenticated = true;
                    continue;
                }
                if (!realm_.empty() && !stun::verifyIntegrity(bytes, key_)) {
                    continue; // Not from the server we share the key with.
                }
                const auto relayed = response->address(stun::xorRelayedAddress);
                if (!relayed) {
                    fail(RCS_ERROR_NETWORK, "The TURN server's allocation has no relayed address");
                }
                relayed_ = *relayed;
                lifetime_ = response->u32(stun::lifetime).value_or(lifetime_);
                if (const auto* ticket = response->find(stun::mobilityTicket)) {
                    ticket_ = *ticket;
                }
                allocated_ = true;
                refreshAt_ = Clock::now() + refreshAfter(lifetime_);
                log::info("network", "TURN allocation " + relayed_.toString() + " on " + server_.toString() + " for " +
                                         std::to_string(lifetime_) + " s");
                return relayed_;
            }
        }
        waiter.wait(std::min(next, Clock::now() + 20ms));
    }
}

void TurnClient::open(DatagramSocket& socket, const Endpoint& peer) {
    auto [entry, added] = peers_.try_emplace(key(peer));
    if (!added) {
        return;
    }
    if (nextChannel_ > 0x7FFF) {
        fail(RCS_ERROR_NETWORK, "Out of TURN channels");
    }
    entry->second.address = peer;
    entry->second.channel = nextChannel_++;
    auto permission = request(stun::createPermission);
    permission.addAddress(stun::xorPeerAddress, peer);
    submit(socket, std::move(permission));
    auto bind = request(stun::channelBind);
    bind.addU32(stun::channelNumber, uint32_t(entry->second.channel) << 16);
    bind.addAddress(stun::xorPeerAddress, peer);
    submit(socket, std::move(bind));
    entry->second.permitted = entry->second.channelRefreshed = Clock::now();
}

size_t TurnClient::overhead(const Endpoint& peer) const {
    const auto found = peers_.find(key(peer));
    if (found != peers_.end() && found->second.bound) {
        return channelHeader;
    }
    // Header, XOR-PEER-ADDRESS, DATA's header and padding.
    return stun::headerSize + 4 + 4 + (peer.ipv6() ? 16 : 4) + 4 + 3;
}

bool TurnClient::send(DatagramSocket& socket, const Endpoint& peer, std::span<const uint8_t> data, uint8_t dscp) {
    const auto out = [&](std::span<const uint8_t> datagram) {
        if (dscp) {
            return socket.sendMarked(server_, datagram, dscp);
        }
        socket.send(server_, datagram);
        return false;
    };
    const auto found = peers_.find(key(peer));
    if (found != peers_.end() && found->second.bound) {
        Bytes channelData;
        channelData.reserve(channelHeader + data.size());
        appendBigEndian(channelData, found->second.channel, 2);
        appendBigEndian(channelData, data.size(), 2);
        channelData.insert(channelData.end(), data.begin(), data.end());
        return out(channelData);
    }
    stun::Message indication;
    indication.method = stun::sendMethod;
    indication.klass = stun::indication;
    indication.transaction = stun::newTransaction();
    indication.addAddress(stun::xorPeerAddress, peer);
    indication.add(stun::data, data);
    return out(stun::encode(indication, {}, false));
}

std::optional<std::pair<Endpoint, std::span<const uint8_t>>> TurnClient::receive(DatagramSocket& socket,
                                                                                 std::span<const uint8_t> datagram) {
    if (isChannelData(datagram)) {
        const uint16_t channel = uint16_t(loadBigEndian(datagram, 0, 2));
        const size_t length = size_t(loadBigEndian(datagram, 2, 2));
        if (length + channelHeader > datagram.size()) {
            return std::nullopt;
        }
        for (const auto& [name, peer] : peers_) {
            if (peer.channel == channel) {
                return std::pair{peer.address, datagram.subspan(channelHeader, length)};
            }
        }
        return std::nullopt;
    }
    const auto message = stun::parse(datagram);
    if (!message) {
        return std::nullopt;
    }
    if (message->method == stun::dataMethod && message->klass == stun::indication) {
        const auto peer = message->address(stun::xorPeerAddress);
        const auto data = stun::attributeIn(datagram, stun::data);
        if (!peer || !data) {
            return std::nullopt;
        }
        return std::pair{*peer, *data};
    }
    const auto found = pending_.find(message->transaction);
    if (found == pending_.end() || (message->klass != stun::success && message->klass != stun::error)) {
        return std::nullopt;
    }
    // A success must prove the server holds the key, or anyone who saw the
    // request could end its retransmission. Errors carry no integrity before
    // authentication (401, 438), so they are taken as they come; the 96-bit
    // transaction ID keeps off-path senders from matching one.
    if (message->klass == stun::success && !realm_.empty() && !stun::verifyIntegrity(datagram, key_)) {
        return std::nullopt;
    }
    stun::Message original = std::move(found->second.message);
    const bool renewed = found->second.renewed;
    pending_.erase(found);
    if (message->klass == stun::error) {
        const int code = message->errorCode();
        if (code == 438 && !renewed) { // Stale nonce: again with the new one, once.
            nonce_ = message->string(stun::nonce);
            original.setTransaction(stun::newTransaction());
            submit(socket, std::move(original), true);
            return std::nullopt;
        }
        const bool credentials = code == 401 || code == 403 || code == 441;
        fail(credentials ? RCS_ERROR_AUTHENTICATION : RCS_ERROR_NETWORK,
             std::string("The TURN server refused ") + methodName(original.method) + ": " + reasonOf(*message) +
                 (code == 401 ? " (the TURN credentials are no longer valid; time-limited ones expire)" : ""));
    }
    const auto now = Clock::now();
    if (original.method == stun::channelBind) {
        if (const auto peer = original.address(stun::xorPeerAddress)) {
            auto& entry = peers_[key(*peer)];
            entry.bound = true;
            entry.channelRefreshed = now;
        }
    } else if (original.method == stun::refresh) {
        lifetime_ = message->u32(stun::lifetime).value_or(lifetime_);
        refreshAt_ = now + refreshAfter(lifetime_);
        if (const auto* ticket = message->find(stun::mobilityTicket)) {
            ticket_ = *ticket;
        }
    }
    return std::nullopt;
}

void TurnClient::move(DatagramSocket& socket) {
    auto message = request(stun::refresh);
    message.addU32(stun::lifetime, 600);
    message.add(stun::mobilityTicket, ticket_); // Only from another 5-tuple: the same one is refused (RFC 8016 3.2).
    submit(socket, std::move(message));
}

void TurnClient::tick(DatagramSocket& socket, Clock::time_point now) {
    for (auto& [transaction, request] : pending_) {
        if (now - request.sent >= firstRetransmit * (1 << (request.attempts - 1))) {
            if (request.attempts >= maxAttempts) {
                fail(RCS_ERROR_NETWORK, "The TURN server " + server_.toString() + " did not answer " +
                                            methodName(request.message.method) + " (5 attempts in 15 s)");
            }
            transmit(socket, request);
        }
    }
    const bool refreshing = std::any_of(pending_.begin(), pending_.end(),
                                        [](const auto& p) { return p.second.message.method == stun::refresh; });
    if (allocated_ && !refreshing && now >= refreshAt_) {
        auto message = request(stun::refresh);
        message.addU32(stun::lifetime, 600);
        submit(socket, std::move(message)); // Repeated until answered, then scheduled from the answer.
    }
    for (auto& [name, peer] : peers_) {
        const Endpoint& address = peer.address;
        if (now - peer.permitted >= permissionRefresh) {
            auto message = request(stun::createPermission);
            message.addAddress(stun::xorPeerAddress, address);
            submit(socket, std::move(message));
            peer.permitted = now;
        }
        if (now - peer.channelRefreshed >= channelRefresh) {
            auto message = request(stun::channelBind);
            message.addU32(stun::channelNumber, uint32_t(peer.channel) << 16);
            message.addAddress(stun::xorPeerAddress, address);
            submit(socket, std::move(message));
            peer.channelRefreshed = now;
        }
    }
}

void TurnClient::release(DatagramSocket& socket) {
    if (!allocated_) {
        return;
    }
    auto message = request(stun::refresh);
    message.addU32(stun::lifetime, 0);
    authenticate(message);
    socket.send(server_, stun::encode(message, integrityKey(), false));
    allocated_ = false;
}

} // namespace rcsdk::transport
