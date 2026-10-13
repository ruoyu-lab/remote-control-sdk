// Waking: the magic packet as a receiver on loopback gets it (nothing leaves
// the machine), what rcs_power_send_wake refuses, this machine's interfaces
// and capabilities as the system tells them (read only), and a test's
// machine in the system's place.
#include "messages.hpp"
#include "system.hpp"
#include "support/check.hpp"

#include <rcsdk/power.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using Socket = SOCKET;
void closeSocket(Socket s) {
    closesocket(s);
}
#else
using Socket = int;
void closeSocket(Socket s) {
    close(s);
}
#endif

// A UDP socket on 127.0.0.1, at a port the system picks.
struct Receiver {
    Receiver() {
#ifdef _WIN32
        WSADATA data;
        CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(bind(socket_, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0);
        socklen_t size = sizeof(at);
        CHECK(getsockname(socket_, reinterpret_cast<sockaddr*>(&at), &size) == 0);
        port = ntohs(at.sin_port);
#ifdef _WIN32
        const DWORD timeout = 5000;
#else
        const timeval timeout{5, 0};
#endif
        CHECK(setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                         sizeof(timeout)) == 0);
    }
    ~Receiver() {
        closeSocket(socket_);
#ifdef _WIN32
        WSACleanup();
#endif
    }
    std::vector<uint8_t> receive() {
        std::vector<uint8_t> bytes(2048);
        const auto got = recv(socket_, reinterpret_cast<char*>(bytes.data()), int(bytes.size()), 0);
        CHECK(got > 0);
        bytes.resize(size_t(got));
        return bytes;
    }

    Socket socket_;
    uint32_t port = 0;
};

std::vector<uint8_t> expected(const uint8_t (&mac)[6], const uint8_t* password, size_t passwordSize) {
    std::vector<uint8_t> packet(6, 0xFF);
    for (int i = 0; i < 16; ++i) {
        packet.insert(packet.end(), mac, mac + 6);
    }
    packet.insert(packet.end(), password, password + passwordSize);
    return packet;
}

void packets() {
    Receiver receiver;
    rcs_power_wake wake = RCS_POWER_WAKE_INIT;
    CHECK(wake.port == 9 && !wake.address && !wake.password_size);
    const uint8_t mac[6] = {0x00, 0x1b, 0x21, 0x3a, 0x4b, 0x5c};
    std::memcpy(wake.mac, mac, 6);
    wake.address = "127.0.0.1";
    wake.port = receiver.port;
    CHECK(rcs_power_send_wake(&wake) == RCS_OK);
    const auto plain = receiver.receive();
    CHECK(plain.size() == 102 && plain == expected(mac, nullptr, 0));
    // With a SecureOn password, six bytes and four.
    const uint8_t password[6] = {1, 2, 3, 4, 5, 6};
    std::memcpy(wake.password, password, 6);
    wake.password_size = 6;
    CHECK(rcs_power_send_wake(&wake) == RCS_OK);
    CHECK(receiver.receive() == expected(mac, password, 6));
    wake.password_size = 4;
    CHECK(rcs_power_send_wake(&wake) == RCS_OK);
    CHECK(receiver.receive() == expected(mac, password, 4));
}

rcs_result sent(void (*change)(rcs_power_wake&)) {
    rcs_power_wake wake = RCS_POWER_WAKE_INIT;
    const uint8_t mac[6] = {0x00, 0x1b, 0x21, 0x3a, 0x4b, 0x5c};
    std::memcpy(wake.mac, mac, 6);
    wake.address = "127.0.0.1";
    change(wake);
    return rcs_power_send_wake(&wake);
}

void refusals() {
    CHECK(rcs_power_send_wake(nullptr) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(sent([](rcs_power_wake& w) { w.struct_size = 4; }) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(sent([](rcs_power_wake& w) { std::memset(w.mac, 0, 6); }) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(sent([](rcs_power_wake& w) { std::memset(w.mac, 0xFF, 6); }) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(sent([](rcs_power_wake& w) { w.mac[0] = 0x01; }) == RCS_ERROR_INVALID_ARGUMENT); // Multicast.
    char reason[256] = {};
    rcs_get_last_call_error(reason, sizeof(reason));
    CHECK(std::string(reason).find("rcs_power_wake.mac") != std::string::npos);
    for (const char* address : {"0.0.0.0", "", "192.168.1", "192.168.1.256", "example.com", "::1", " 10.0.0.1"}) {
        static const char* current = nullptr;
        current = address;
        CHECK(sent([](rcs_power_wake& w) { w.address = current; }) == RCS_ERROR_INVALID_ARGUMENT);
    }
    CHECK(sent([](rcs_power_wake& w) { w.port = 0; }) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(sent([](rcs_power_wake& w) { w.port = 65536; }) == RCS_ERROR_INVALID_ARGUMENT);
    for (const uint32_t size : {1u, 2u, 3u, 5u, 7u}) {
        static uint32_t current = 0;
        current = size;
        CHECK(sent([](rcs_power_wake& w) { w.password_size = current; }) == RCS_ERROR_INVALID_ARGUMENT);
    }
}

// The system's answers, read only: whatever this machine has, each
// interface is whole.
void systemAnswers() {
    rcs_power_capabilities caps = RCS_POWER_CAPABILITIES_INIT;
    CHECK(rcs_power_get_capabilities(&caps) == RCS_OK);
    rcs_power_capabilities wrong = RCS_POWER_CAPABILITIES_INIT;
    wrong.struct_size = 8;
    CHECK(rcs_power_get_capabilities(&wrong) == RCS_ERROR_INVALID_ARGUMENT);
    std::printf("capabilities: keep_awake %u, actions 0x%x, interfaces %u, send_wake %u (%s)\n", caps.keep_awake,
                caps.actions, caps.interfaces, caps.send_wake, caps.message);
    CHECK(caps.send_wake == 1 && !(caps.actions & ~uint32_t(RCS_POWER_ALL_ACTIONS)));
    CHECK(caps.actions == RCS_POWER_ALL_ACTIONS || caps.message[0]);
#if defined(_WIN32) || (defined(__APPLE__) && !TARGET_OS_IPHONE)
    CHECK(caps.interfaces == 1 && caps.keep_awake == 1);
#endif

    uint32_t count = 99;
    CHECK(rcs_power_list_interfaces(nullptr, sizeof(rcs_power_interface), 1, &count) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_power_list_interfaces(nullptr, 4, 0, &count) == RCS_ERROR_INVALID_ARGUMENT);
    const rcs_result counted = rcs_power_list_interfaces(nullptr, sizeof(rcs_power_interface), 0, &count);
    if (!caps.interfaces) {
        CHECK(counted == RCS_ERROR_UNSUPPORTED);
        return;
    }
    CHECK(counted == RCS_OK);
    std::vector<rcs_power_interface> list(count + 4);
    CHECK(rcs_power_list_interfaces(list.data(), sizeof(rcs_power_interface), uint32_t(list.size()), &count) ==
          RCS_OK);
    list.resize(count);
    for (const auto& it : list) {
        std::printf("interface %s: %02x:%02x:%02x:%02x:%02x:%02x, %s, %s/%u, broadcast %s, wake %u\n", it.name,
                    it.mac[0], it.mac[1], it.mac[2], it.mac[3], it.mac[4], it.mac[5],
                    it.link == RCS_POWER_LINK_ETHERNET ? "Ethernet" : "Wi-Fi", it.address, it.prefix_length,
                    it.broadcast, it.wake);
        CHECK(it.link == RCS_POWER_LINK_ETHERNET || it.link == RCS_POWER_LINK_WIFI);
        CHECK(it.wake <= RCS_POWER_WAKE_DISABLED && it.prefix_length <= 32 && it.name[0]);
        CHECK(!(it.mac[0] & 1) && (it.mac[0] | it.mac[1] | it.mac[2] | it.mac[3] | it.mac[4] | it.mac[5]));
        uint32_t address = 0, broadcast = 0;
        CHECK(rcsdk::power::parseAddress(it.address, address) && rcsdk::power::parseAddress(it.broadcast, broadcast));
        const uint32_t host = it.prefix_length >= 32 ? 0u : ~0u >> it.prefix_length;
        CHECK(broadcast == (address | host));
    }
    if (count > 1) {
        rcs_power_interface one{};
        CHECK(rcs_power_list_interfaces(&one, sizeof(one), 1, &count) == RCS_ERROR_BUFFER_TOO_SMALL);
    }
}

// A test's machine in the system's place: what capabilities and the list say.
struct TestMachine final : rcsdk::power::Machine {
    rcsdk::power::Abilities abilities() override { return {1u << RCS_POWER_SLEEP, "Only sleep here"}; }
    void check(rcs_power_action, uint32_t) override {}
    void act(rcs_power_action, uint32_t) override { CHECK(false); } // Nothing here acts.
    std::vector<rcs_power_interface> interfaces() override {
        const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 7};
        return {rcsdk::power::makeInterface(mac, RCS_POWER_LINK_ETHERNET, RCS_POWER_WAKE_DISABLED, 0xAC100A0B, 16,
                                            "Test")};
    }
    std::string noInterfaces() override { return {}; }
};

void replaced() {
    rcsdk::power::setMachine(std::make_shared<TestMachine>());
    rcs_power_capabilities caps = RCS_POWER_CAPABILITIES_INIT;
    CHECK(rcs_power_get_capabilities(&caps) == RCS_OK);
    CHECK(caps.actions == 1u << RCS_POWER_SLEEP && caps.interfaces == 1 && std::string(caps.message) ==
                                                                               "Only sleep here");
    rcs_power_interface it{};
    uint32_t count = 0;
    CHECK(rcs_power_list_interfaces(&it, sizeof(it), 1, &count) == RCS_OK && count == 1);
    CHECK(std::string(it.broadcast) == "172.16.255.255" && it.wake == RCS_POWER_WAKE_DISABLED);
    rcsdk::power::setMachine(nullptr);
}

} // namespace

int main() {
    return runTest("power.wake", [] {
        packets();
        refusals();
        systemAnswers();
        replaced();
    });
}
