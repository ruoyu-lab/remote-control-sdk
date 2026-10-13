// rcsdk_host_relay: a stand-in for the host's own server in tests of
// RCS_TRANSPORT_HOST, the way a chat app's server forwards between its
// clients. A room has one sharer and any number of viewers: what a viewer
// sends reaches the sharer behind the route the relay gave that viewer, and
// what the sharer sends behind a route reaches that viewer, untouched: over
// TCP the bytes of each viewer's stream, over UDP each datagram. It never
// looks inside: the SDK's connections are end-to-end encrypted. It also
// carries P2P and RELAY signals, as a host's server carries its users'
// messages: a signal room passes each line one member sends to all the
// others. Test software, not part of the SDK, and no product: no accounts,
// no limits beyond its buffers, no persistence.
//
//   rcsdk_host_relay [--listen ADDRESS:PORT]    TCP and UDP (default 0.0.0.0:47910)
//
// TCP: a viewer sends "RCSRELAY <room>\n", the sharer "RCSSHARE <room>\n",
// then its stream. The sharer's stream in both directions is route(8)
// length(4) bytes, again and again (relay_socket.hpp): a piece of that
// viewer's stream; length 0 is a viewer that left (to the sharer), or one the
// sharer drops (from it). A viewer's bytes before the sharer came are
// dropped (its session repeats its handshake); when the sharer leaves, its
// viewers' connections are closed (their hosts tell their sessions that the
// channel closed, and come back to wait for the next). A second sharer for a
// room is turned away.
// UDP: a client sends its hello ("RCSRELAY <room>", "RCSSHARE <room>") at
// least every 5 s, and "RCSRELAY leave" when it goes; a viewer's other
// datagrams reach the sharer as route(8) datagram, the sharer's reach the
// viewer of the route. A client silent for 10 s is dropped. Whenever a viewer
// leaves, the sharer gets route(8) "RCSRELAY closed"; when the sharer leaves,
// each viewer "RCSRELAY changed".
// Signals (TCP): a client sends "RCSSIGNAL <room>\n", then lines of text; each
// whole line reaches every other client of that signal room, in any number.
#include "../common/relay_socket.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr size_t maxBuffered = 16u << 20; // Per TCP client: beyond this its peer does not read.
constexpr size_t maxLine = 64u << 10;     // A signal room's line.
constexpr size_t maxPiece = 1u << 20;     // A piece of a viewer's stream from the sharer.
constexpr char signalTag[] = "RCSSIGNAL ";
constexpr auto udpSilence = 10s;

volatile std::sig_atomic_t interrupted = 0;

std::string describe(const relay::Address& address) {
    char text[INET6_ADDRSTRLEN] = {};
    uint16_t port = 0;
    if (address.storage.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address.storage);
        inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text));
        port = ntohs(v4->sin_port);
        return std::string(text) + ":" + std::to_string(port);
    }
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address.storage);
    inet_ntop(AF_INET6, &v6->sin6_addr, text, sizeof(text));
    port = ntohs(v6->sin6_port);
    return "[" + std::string(text) + "]:" + std::to_string(port);
}

enum class Role { none, sharer, viewer, signals };

struct TcpClient {
    relay::Handle socket = relay::noSocket;
    std::string name;  // Its address, for the log.
    std::string hello; // The hello line while it arrives.
    std::string room;  // Set once it named one.
    Role role = Role::none;
    uint64_t route = 0;          // A viewer's, in its room.
    std::string line;            // A signal room's line while it arrives.
    std::vector<uint8_t> routed; // The sharer's: the start of a routed piece while it arrives.
    std::vector<uint8_t> out;    // Bytes for it that its connection has not taken yet.
    bool closing = false;
};

struct TcpRoom {
    TcpClient* sharer = nullptr;
    std::map<uint64_t, TcpClient*> viewers;
};

struct UdpMember {
    relay::Address address;
    Clock::time_point heard;
    uint64_t route = 0; // A viewer's.
};

struct UdpRoom {
    std::optional<UdpMember> sharer;
    std::vector<UdpMember> viewers;
};

class Relay {
public:
    explicit Relay(const relay::Address& address) {
        listener_ = relay::openSocket(address.storage.ss_family, SOCK_STREAM);
        int on = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
        if (::bind(listener_, address.get(), address.length) != 0 || ::listen(listener_, 64) != 0) {
            throw std::runtime_error("cannot listen on TCP " + describe(address));
        }
        relay::makeNonBlocking(listener_);
        udp_ = relay::openSocket(address.storage.ss_family, SOCK_DGRAM);
        if (::bind(udp_, address.get(), address.length) != 0) {
            throw std::runtime_error("cannot bind UDP " + describe(address));
        }
        relay::makeNonBlocking(udp_);
        std::printf("relay: listening on TCP and UDP %s\n", describe(address).c_str());
        std::fflush(stdout);
    }
    ~Relay() {
        for (auto& client : clients_) {
            relay::closeSocket(client->socket);
        }
        relay::closeSocket(listener_);
        relay::closeSocket(udp_);
    }

    void run() {
        std::vector<uint8_t> buffer(65536 + 8);
        while (!interrupted) {
            std::vector<relay::PollItem> items;
            items.push_back({listener_, relay::readEvent, 0});
            items.push_back({udp_, relay::readEvent, 0});
            for (const auto& client : clients_) {
                short events = relay::readEvent;
                if (!client->out.empty()) {
                    events |= relay::writeEvent;
                }
                items.push_back({client->socket, events, 0});
            }
            relay::pollItems(items.data(), items.size(), 500);
            if (items[0].revents) {
                accept();
            }
            if (items[1].revents) {
                receiveDatagrams(buffer);
            }
            for (size_t i = 0; i < clients_.size() && i + 2 < items.size(); ++i) {
                auto& client = *clients_[i];
                const short ready = items[i + 2].revents;
                if (ready & relay::writeEvent) {
                    flush(client);
                }
                if (ready & ~relay::writeEvent) {
                    read(client, buffer);
                }
            }
            expireDatagramMembers();
            removeClosed();
        }
    }

private:
    void log(const std::string& text) {
        std::printf("relay: %s\n", text.c_str());
        std::fflush(stdout);
    }

    void accept() {
        for (;;) {
            relay::Address from;
            from.length = sizeof(from.storage);
            const relay::Handle socket =
                ::accept(listener_, reinterpret_cast<sockaddr*>(&from.storage), &from.length);
            if (socket == relay::noSocket) {
                return;
            }
            relay::makeNonBlocking(socket);
            int on = 1;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
            auto client = std::make_unique<TcpClient>();
            client->socket = socket;
            client->name = describe(from);
            clients_.push_back(std::move(client));
        }
    }

    void read(TcpClient& client, std::vector<uint8_t>& buffer) {
        while (!client.closing) {
#ifdef _WIN32
            const int got = ::recv(client.socket, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0);
#else
            const auto got = ::recv(client.socket, buffer.data(), buffer.size(), 0);
#endif
            if (got < 0 && relay::wouldBlock()) {
                return;
            }
            if (got <= 0) {
                close(client, "left");
                return;
            }
            const auto bytes = std::span(buffer).first(size_t(got));
            if (client.role == Role::none) {
                hello(client, bytes);
            } else {
                pass(client, bytes);
            }
        }
    }

    // The hello line, possibly in pieces; what follows it is the stream.
    void hello(TcpClient& client, std::span<const uint8_t> bytes) {
        size_t used = 0;
        while (used < bytes.size() && client.role == Role::none) {
            const char c = char(bytes[used++]);
            if (c != '\n') {
                client.hello.push_back(c);
                if (client.hello.size() > 200) {
                    close(client, "sent no hello");
                    return;
                }
                continue;
            }
            const auto named = [&](const char* tag) {
                const size_t size = std::strlen(tag);
                return client.hello.rfind(tag, 0) == 0 && client.hello.size() > size ? client.hello.substr(size)
                                                                                      : std::string();
            };
            if (const auto room = named(signalTag); !room.empty()) {
                client.room = room;
                client.role = Role::signals;
                log("signal room \"" + room + "\": " + client.name + " joins");
            } else if (const auto shared = named(relay::shareTag); !shared.empty()) {
                client.room = shared;
                joinAsSharer(client);
            } else if (const auto watched = named(relay::tag); !watched.empty()) {
                client.room = watched;
                joinAsViewer(client);
            } else {
                close(client, "sent no hello");
                return;
            }
        }
        if (used < bytes.size() && !client.closing) {
            pass(client, bytes.subspan(used));
        }
    }

    void pass(TcpClient& client, std::span<const uint8_t> bytes) {
        if (client.role == Role::signals) {
            lines(client, bytes);
        } else if (client.role == Role::viewer) {
            auto& room = tcpRooms_[client.room];
            if (room.sharer) { // Without one it waits: its session's handshake repeats.
                std::vector<uint8_t> piece;
                relay::putNumber(piece, client.route, 8);
                relay::putNumber(piece, bytes.size(), 4);
                piece.insert(piece.end(), bytes.begin(), bytes.end());
                forward(*room.sharer, piece);
            }
        } else if (client.role == Role::sharer) {
            fromSharer(client, bytes);
        }
    }

    // The sharer's routed pieces, each to its viewer.
    void fromSharer(TcpClient& sharer, std::span<const uint8_t> bytes) {
        auto& pending = sharer.routed;
        pending.insert(pending.end(), bytes.begin(), bytes.end());
        size_t at = 0;
        while (!sharer.closing && pending.size() - at >= relay::routeHeader) {
            const uint64_t route = relay::getNumber(pending.data() + at, 8);
            const size_t length = size_t(relay::getNumber(pending.data() + at + 8, 4));
            if (length > maxPiece) {
                close(sharer, "sent a piece too large");
                return;
            }
            if (pending.size() - at - relay::routeHeader < length) {
                break;
            }
            auto& viewers = tcpRooms_[sharer.room].viewers;
            if (const auto found = viewers.find(route); found != viewers.end()) {
                if (length) {
                    forward(*found->second, std::span(pending).subspan(at + relay::routeHeader, length));
                } else {
                    close(*found->second, "was dropped by its sharer");
                }
            }
            at += relay::routeHeader + length;
        }
        pending.erase(pending.begin(), pending.begin() + ptrdiff_t(std::min(at, pending.size())));
    }

    // A signal room: each whole line goes to every other member.
    void lines(TcpClient& client, std::span<const uint8_t> bytes) {
        for (const uint8_t byte : bytes) {
            if (byte != '\n') {
                client.line.push_back(char(byte));
                if (client.line.size() > maxLine) {
                    close(client, "sent a line too long");
                    return;
                }
                continue;
            }
            client.line.push_back('\n');
            const auto line = std::span(reinterpret_cast<const uint8_t*>(client.line.data()), client.line.size());
            for (const auto& other : clients_) {
                if (other.get() != &client && !other->closing && other->role == Role::signals &&
                    other->room == client.room) {
                    forward(*other, line);
                }
            }
            client.line.clear();
        }
    }

    void joinAsSharer(TcpClient& client) {
        auto& room = tcpRooms_[client.room];
        if (room.sharer) {
            close(client, "came to a room that has a sharer");
            return;
        }
        client.role = Role::sharer;
        room.sharer = &client;
        log("TCP room \"" + client.room + "\": " + client.name + " shares (" + std::to_string(room.viewers.size()) +
            " viewers wait)");
    }

    void joinAsViewer(TcpClient& client) {
        auto& room = tcpRooms_[client.room];
        client.role = Role::viewer;
        client.route = ++nextRoute_;
        room.viewers[client.route] = &client;
        log("TCP room \"" + client.room + "\": " + client.name + " views, route " + std::to_string(client.route));
    }

    void forward(TcpClient& to, std::span<const uint8_t> bytes) {
        if (to.out.size() + bytes.size() > maxBuffered) {
            close(to, "does not read");
            return;
        }
        to.out.insert(to.out.end(), bytes.begin(), bytes.end());
        flush(to);
    }

    void flush(TcpClient& client) {
        while (!client.out.empty() && !client.closing) {
#ifdef _WIN32
            const int sent = ::send(client.socket, reinterpret_cast<const char*>(client.out.data()),
                                    int(client.out.size()), 0);
#else
            const auto sent = ::send(client.socket, client.out.data(), client.out.size(), 0); // SIGPIPE is ignored.
#endif
            if (sent < 0 && relay::wouldBlock()) {
                return;
            }
            if (sent <= 0) {
                close(client, "broke");
                return;
            }
            client.out.erase(client.out.begin(), client.out.begin() + sent);
        }
    }

    // A viewer that goes is a closed route to its sharer; a sharer that goes
    // takes its viewers' connections along (their hosts learn that the
    // channel closed).
    void close(TcpClient& client, const char* why) {
        if (client.closing) {
            return;
        }
        client.closing = true;
        log("TCP " + client.name + " " + why + (client.room.empty() ? "" : " (room \"" + client.room + "\")"));
        if (client.role == Role::viewer) {
            auto& room = tcpRooms_[client.room];
            room.viewers.erase(client.route);
            if (room.sharer && !room.sharer->closing) {
                std::vector<uint8_t> gone;
                relay::putNumber(gone, client.route, 8);
                relay::putNumber(gone, 0, 4);
                forward(*room.sharer, gone);
            }
        } else if (client.role == Role::sharer) {
            auto& room = tcpRooms_[client.room];
            room.sharer = nullptr;
            const auto viewers = room.viewers;
            for (const auto& [route, viewer] : viewers) {
                close(*viewer, "is closed with its sharer");
            }
        }
    }

    void removeClosed() {
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->closing) {
                if (const auto room = tcpRooms_.find((*it)->room);
                    room != tcpRooms_.end() && !room->second.sharer && room->second.viewers.empty()) {
                    tcpRooms_.erase(room);
                }
                relay::closeSocket((*it)->socket);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- UDP --------------------------------------------------------------------------

    void sendTo(const relay::Address& to, std::span<const uint8_t> bytes) {
        ::sendto(udp_, reinterpret_cast<const char*>(bytes.data()), int(bytes.size()), 0, to.get(), to.length);
    }

    void receiveDatagrams(std::vector<uint8_t>& buffer) {
        for (;;) {
            relay::Address from;
            from.length = sizeof(from.storage);
#ifdef _WIN32
            const int got = ::recvfrom(udp_, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                                       reinterpret_cast<sockaddr*>(&from.storage), &from.length);
#else
            const auto got = ::recvfrom(udp_, buffer.data(), buffer.size(), 0,
                                        reinterpret_cast<sockaddr*>(&from.storage), &from.length);
#endif
            if (got < 0) {
                return; // Nothing more now (or an ICMP report about an earlier datagram).
            }
            const auto bytes = std::span(buffer).first(size_t(got));
            const std::string text(bytes.begin(), bytes.begin() + ptrdiff_t(std::min(bytes.size(), size_t(200))));
            if (text.rfind(relay::tag, 0) == 0) {
                const std::string what = text.substr(relay::tagSize);
                if (what == "leave") {
                    leaveDatagramRoom(from, "left");
                } else if (!what.empty() && what != "changed" && what != "closed") {
                    joinDatagramRoom(from, what, false);
                }
                continue;
            }
            if (text.rfind(relay::shareTag, 0) == 0 && text.size() > relay::shareTagSize) {
                joinDatagramRoom(from, text.substr(relay::shareTagSize), true);
                continue;
            }
            datagram(from, bytes);
        }
    }

    // A member's datagram: a viewer's to its sharer, behind its route; a
    // sharer's to the viewer of the route in front.
    void datagram(const relay::Address& from, std::span<const uint8_t> bytes) {
        for (auto& [name, room] : udpRooms_) {
            if (room.sharer && room.sharer->address == from) {
                room.sharer->heard = Clock::now();
                if (bytes.size() <= 8) {
                    return;
                }
                const uint64_t route = relay::getNumber(bytes.data(), 8);
                for (const auto& viewer : room.viewers) {
                    if (viewer.route == route) {
                        sendTo(viewer.address, bytes.subspan(8));
                    }
                }
                return;
            }
            for (auto& viewer : room.viewers) {
                if (viewer.address == from) {
                    viewer.heard = Clock::now();
                    if (room.sharer) {
                        std::vector<uint8_t> routed;
                        relay::putNumber(routed, viewer.route, 8);
                        routed.insert(routed.end(), bytes.begin(), bytes.end());
                        sendTo(room.sharer->address, routed);
                    }
                    return;
                }
            }
        }
    }

    void joinDatagramRoom(const relay::Address& from, const std::string& name, bool share) {
        auto& room = udpRooms_[name];
        if (room.sharer && room.sharer->address == from) {
            room.sharer->heard = Clock::now();
            return;
        }
        for (auto& viewer : room.viewers) {
            if (viewer.address == from) {
                viewer.heard = Clock::now();
                return;
            }
        }
        if (share) {
            if (room.sharer) {
                return; // It has one: another may try again once it leaves or goes silent.
            }
            room.sharer = UdpMember{from, Clock::now(), 0};
            log("UDP room \"" + name + "\": " + describe(from) + " shares");
        } else {
            room.viewers.push_back({from, Clock::now(), ++nextRoute_});
            log("UDP room \"" + name + "\": " + describe(from) + " views, route " +
                std::to_string(room.viewers.back().route));
        }
    }

    void leaveDatagramRoom(const relay::Address& from, const char* why) {
        static const std::string changed = std::string(relay::tag) + "changed";
        static const std::string closed = std::string(relay::tag) + "closed";
        for (auto room = udpRooms_.begin(); room != udpRooms_.end(); ++room) {
            auto& [name, members] = *room;
            if (members.sharer && members.sharer->address == from) {
                log("UDP room \"" + name + "\": sharer " + describe(from) + " " + why);
                members.sharer.reset();
                for (const auto& viewer : members.viewers) {
                    sendTo(viewer.address, std::span(reinterpret_cast<const uint8_t*>(changed.data()), changed.size()));
                }
            }
            for (size_t i = 0; i < members.viewers.size(); ++i) {
                if (members.viewers[i].address == from) {
                    log("UDP room \"" + name + "\": " + describe(from) + " " + why);
                    if (members.sharer) {
                        std::vector<uint8_t> notice;
                        relay::putNumber(notice, members.viewers[i].route, 8);
                        notice.insert(notice.end(), closed.begin(), closed.end());
                        sendTo(members.sharer->address, notice);
                    }
                    members.viewers.erase(members.viewers.begin() + ptrdiff_t(i));
                    break;
                }
            }
            if (!members.sharer && members.viewers.empty()) {
                udpRooms_.erase(room);
                return;
            }
        }
    }

    void expireDatagramMembers() {
        const auto now = Clock::now();
        std::vector<relay::Address> silent;
        for (const auto& [name, room] : udpRooms_) {
            if (room.sharer && now - room.sharer->heard > udpSilence) {
                silent.push_back(room.sharer->address);
            }
            for (const auto& viewer : room.viewers) {
                if (now - viewer.heard > udpSilence) {
                    silent.push_back(viewer.address);
                }
            }
        }
        for (const auto& address : silent) {
            leaveDatagramRoom(address, "went silent");
        }
    }

    relay::Handle listener_ = relay::noSocket, udp_ = relay::noSocket;
    std::vector<std::unique_ptr<TcpClient>> clients_;
    std::map<std::string, TcpRoom> tcpRooms_;
    std::map<std::string, UdpRoom> udpRooms_;
    uint64_t nextRoute_ = 0; // Each viewer's route, TCP and UDP alike.
};

} // namespace

int main(int argc, char** argv) {
    std::string listen = "0.0.0.0:47910";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            listen = argv[++i];
        } else {
            std::fprintf(stderr, "usage: rcsdk_host_relay [--listen ADDRESS:PORT]   (default 0.0.0.0:47910)\n");
            return arg == "--help" ? 0 : 2;
        }
    }
    std::signal(SIGINT, [](int) { interrupted = 1; });
    std::signal(SIGTERM, [](int) { interrupted = 1; });
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        Relay(relay::parseAddress(listen)).run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "relay: %s\n", error.what());
        return 1;
    }
    return 0;
}
