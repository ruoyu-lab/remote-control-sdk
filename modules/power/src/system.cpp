// Which machine the module uses, and the magic packet: six 0xFF bytes, the
// MAC address sixteen times, the SecureOn password, in one UDP datagram.
#include "system.hpp"

#include "messages.hpp"

#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif !defined(__EMSCRIPTEN__)
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rcsdk::power {
namespace {

std::mutex machineMutex;
std::shared_ptr<Machine> replaced;

} // namespace

void setMachine(std::shared_ptr<Machine> machine) {
    std::lock_guard lock(machineMutex);
    replaced = std::move(machine);
}

std::shared_ptr<Machine> machine() {
    {
        std::lock_guard lock(machineMutex);
        if (replaced) {
            return replaced;
        }
    }
    static const std::shared_ptr<Machine> system = systemMachine();
    return system;
}

const char* actionName(rcs_power_action action) {
    switch (action) {
    case RCS_POWER_RESTART:
        return "restart";
    case RCS_POWER_SHUT_DOWN:
        return "shut down";
    case RCS_POWER_SLEEP:
        return "sleep";
    case RCS_POWER_SIGN_OUT:
        return "sign out";
    default:
        return "an unknown action";
    }
}

#if defined(__EMSCRIPTEN__)

bool sendsWake() {
    return false;
}

void sendWake(const rcs_power_wake&) {
    throw Refused(RCS_ERROR_UNSUPPORTED, "A browser sends no UDP datagrams of its own");
}

#else

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket noSocket = INVALID_SOCKET;
void closeSocket(Socket s) {
    closesocket(s);
}
int lastError() {
    return WSAGetLastError();
}
bool denied(int error) {
    return error == WSAEACCES;
}
std::string errorText(int error) {
    char* text = nullptr;
    const DWORD length =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, DWORD(error), 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = length ? std::string(text, length) : "error " + std::to_string(error);
    LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
}
// Winsock for the call's length.
struct Winsock {
    Winsock() {
        WSADATA data;
        if (const int error = WSAStartup(MAKEWORD(2, 2), &data)) {
            throw Refused(RCS_ERROR_NETWORK, "Windows sockets did not start (" + errorText(error) + ")");
        }
    }
    ~Winsock() { WSACleanup(); }
};
#else
using Socket = int;
constexpr Socket noSocket = -1;
void closeSocket(Socket s) {
    close(s);
}
int lastError() {
    return errno;
}
bool denied(int error) {
    return error == EACCES || error == EPERM;
}
std::string errorText(int error) {
    return std::strerror(error);
}
#endif

} // namespace

bool sendsWake() {
    return true;
}

void sendWake(const rcs_power_wake& wake) {
    std::vector<uint8_t> packet(6, 0xFF);
    for (int i = 0; i < 16; ++i) {
        packet.insert(packet.end(), std::begin(wake.mac), std::end(wake.mac));
    }
    packet.insert(packet.end(), wake.password, wake.password + wake.password_size);
    const std::string where = std::string(wake.address ? wake.address : "255.255.255.255") + ":" +
                              std::to_string(wake.port);
    const auto failed = [&](const char* step, int error) {
        throw Refused(denied(error) ? RCS_ERROR_PERMISSION_REQUIRED : RCS_ERROR_NETWORK,
                      std::string("The wake packet to ") + where + " could not be sent (" + step + ": " +
                          errorText(error) + ")");
    };
#ifdef _WIN32
    const Winsock winsock;
#endif
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(wake.port));
    uint32_t address = INADDR_BROADCAST;
    if (wake.address) {
        parseAddress(wake.address, address); // Checked before.
    }
    to.sin_addr.s_addr = htonl(address);
    const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == noSocket) {
        failed("socket", lastError());
    }
    const int on = 1;
    if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on)) != 0) {
        const int error = lastError();
        closeSocket(s);
        failed("SO_BROADCAST", error);
    }
    const auto sent = sendto(s, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
                             reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    const int error = lastError();
    closeSocket(s);
    if (sent < 0 || size_t(sent) != packet.size()) {
        failed("sendto", error);
    }
}

#endif

} // namespace rcsdk::power
