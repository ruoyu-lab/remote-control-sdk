// macOS: sleep through IOKit (root, or the user at the console); restart,
// shut down and sign out through loginwindow for the console's user (Apple
// events, as the Apple menu does: applications are asked to quit), or by
// force (`shutdown`, `launchctl`) as root; the interfaces that can wake the
// Mac (getifaddrs, SystemConfiguration's kinds and names, pmset's womp).
#include "system.hpp"

#include "messages.hpp"

#import <CoreServices/CoreServices.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <SystemConfiguration/SystemConfiguration.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace rcsdk::power {
namespace {

// Who is at the console: the owner of /dev/console (root at the login window).
uid_t consoleUser() {
    struct stat console{};
    return stat("/dev/console", &console) == 0 ? console.st_uid : uid_t(-1);
}

std::string cf(CFStringRef text) {
    if (!text) {
        return {};
    }
    char buffer[256] = {};
    return CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8) ? buffer : std::string();
}

// Runs `program` with `arguments`; its output when `output` is given. Throws
// Refused when it cannot start or ends with a status; one still running after
// `wait` goes on (the machine is going away).
void run(const char* program, std::vector<std::string> arguments, std::chrono::milliseconds wait,
         std::string* output = nullptr) {
    std::vector<char*> argv{const_cast<char*>(program)};
    for (auto& a : arguments) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    int pipe[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (output) {
        if (::pipe(pipe) != 0) {
            posix_spawn_file_actions_destroy(&actions);
            throw Refused(RCS_ERROR_INTERNAL, std::string("Could not run ") + program);
        }
        posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe[0]);
    }
    pid_t child = 0;
    const int spawned = posix_spawn(&child, program, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (output) {
        close(pipe[1]);
        if (spawned == 0) {
            char buffer[4096];
            for (ssize_t got; (got = read(pipe[0], buffer, sizeof(buffer))) > 0;) {
                output->append(buffer, size_t(got));
            }
        }
        close(pipe[0]);
    }
    if (spawned != 0) {
        throw Refused(RCS_ERROR_INTERNAL, std::string("Could not run ") + program + " (" + std::strerror(spawned) + ")");
    }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        int status = 0;
        const pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                throw Refused(RCS_ERROR_INTERNAL, std::string(program) + " ended with status " + std::to_string(status));
            }
            return;
        }
        if (done < 0 || std::chrono::steady_clock::now() >= deadline) {
            return; // Still at it, or reaped elsewhere: the system has it.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// An Apple event to loginwindow, as the Apple menu's Restart, Shut Down and
// Log Out send it (without their dialogs).
void tellLoginWindow(AEEventID event, rcs_power_action action) {
    static const char loginWindow[] = "com.apple.loginwindow";
    AEAddressDesc target = {typeNull, nullptr};
    AppleEvent message = {typeNull, nullptr}, reply = {typeNull, nullptr};
    OSStatus status = AECreateDesc(typeApplicationBundleID, loginWindow, sizeof(loginWindow) - 1, &target);
    if (status == noErr) {
        status = AECreateAppleEvent(kCoreEventClass, event, &target, kAutoGenerateReturnID, kAnyTransactionID, &message);
    }
    if (status == noErr) {
        status = AESendMessage(&message, &reply, kAENoReply, kAEDefaultTimeout);
    }
    AEDisposeDesc(&reply);
    AEDisposeDesc(&message);
    AEDisposeDesc(&target);
    if (status != noErr) {
        throw Refused(RCS_ERROR_INTERNAL, std::string("loginwindow did not take the request to ") + actionName(action) +
                                              " (OSStatus " + std::to_string(status) + ")");
    }
}

// pmset's "Wake for network access" (womp) for the power source in use.
rcs_power_wake_setting wakeForNetwork() {
    std::string text;
    try {
        run("/usr/bin/pmset", {"-g"}, std::chrono::seconds(5), &text);
    } catch (const Refused&) {
        return RCS_POWER_WAKE_UNKNOWN;
    }
    for (size_t at = text.find("womp"); at != std::string::npos; at = text.find("womp", at + 1)) {
        if (at && text[at - 1] != ' ' && text[at - 1] != '\n') {
            continue;
        }
        size_t value = text.find_first_not_of(' ', at + 4);
        if (value != std::string::npos && (text[value] == '0' || text[value] == '1')) {
            return text[value] == '1' ? RCS_POWER_WAKE_ENABLED : RCS_POWER_WAKE_DISABLED;
        }
    }
    return RCS_POWER_WAKE_UNKNOWN;
}

constexpr char notConsole[] = "macOS lets root or the user at the console do this; this process is neither";
constexpr char rootForces[] = "A root process restarts, shuts down or signs out a Mac by force only (shutdown, "
                              "launchctl): ask with RCS_POWER_FORCE";
constexpr char userAsks[] = "Restarting or shutting down a Mac by force takes root; the console's user asks "
                            "loginwindow, without RCS_POWER_FORCE";
constexpr char nobodySignedIn[] = "Nobody is signed in at the console";

class MacMachine final : public Machine {
public:
    Abilities abilities() override {
        Abilities a;
        const uid_t self = geteuid();
        if (self == 0 || self == consoleUser()) {
            a.actions = 1u << RCS_POWER_RESTART | 1u << RCS_POWER_SHUT_DOWN | 1u << RCS_POWER_SLEEP |
                        1u << RCS_POWER_SIGN_OUT;
        } else {
            a.why = notConsole;
        }
        return a;
    }

    void check(rcs_power_action action, uint32_t flags) override {
        const uid_t self = geteuid(), console = consoleUser();
        const bool force = flags & RCS_POWER_FORCE;
        if (self != 0 && self != console) {
            throw Refused(RCS_ERROR_UNSUPPORTED, notConsole);
        }
        if (action == RCS_POWER_SLEEP) {
            return;
        }
        if (self == 0 && !force) {
            throw Refused(RCS_ERROR_UNSUPPORTED, rootForces);
        }
        if (self != 0 && force && action != RCS_POWER_SIGN_OUT) {
            throw Refused(RCS_ERROR_UNSUPPORTED, userAsks);
        }
        if (action == RCS_POWER_SIGN_OUT && (console == 0 || console == uid_t(-1))) {
            throw Refused(RCS_ERROR_INVALID_STATE, nobodySignedIn);
        }
    }

    void act(rcs_power_action action, uint32_t flags) override {
        check(action, flags);
        const bool root = geteuid() == 0;
        const auto wait = std::chrono::seconds(5);
        switch (action) {
        case RCS_POWER_SLEEP: {
            const io_connect_t power = IOPMFindPowerManagement(MACH_PORT_NULL);
            if (!power) {
                throw Refused(RCS_ERROR_INTERNAL, "The power manager could not be reached");
            }
            const IOReturn result = IOPMSleepSystem(power);
            IOServiceClose(power);
            if (result != kIOReturnSuccess) {
                throw Refused(RCS_ERROR_INTERNAL,
                              "The system did not go to sleep (IOKit error " + std::to_string(result) + ")");
            }
            return;
        }
        case RCS_POWER_RESTART:
            return root ? run("/sbin/shutdown", {"-r", "now"}, wait) : tellLoginWindow(kAERestart, action);
        case RCS_POWER_SHUT_DOWN:
            return root ? run("/sbin/shutdown", {"-h", "now"}, wait) : tellLoginWindow(kAEShutDown, action);
        case RCS_POWER_SIGN_OUT:
            if (root) {
                return run("/bin/launchctl", {"bootout", "gui/" + std::to_string(consoleUser())}, wait);
            }
            return flags & RCS_POWER_FORCE ? run("/bin/launchctl", {"reboot", "logout"}, wait)
                                           : tellLoginWindow(kAEReallyLogOut, action);
        default:
            throw Refused(RCS_ERROR_INVALID_ARGUMENT, "An unknown action");
        }
    }

    std::vector<rcs_power_interface> interfaces() override {
        // The kinds and names of the system's own interfaces (not bridges,
        // tunnels, or AirDrop's), by BSD name.
        struct Kind {
            rcs_power_link link;
            std::string name;
        };
        std::map<std::string, Kind> kinds;
        if (CFArrayRef all = SCNetworkInterfaceCopyAll()) {
            for (CFIndex i = 0; i < CFArrayGetCount(all); ++i) {
                auto item = static_cast<SCNetworkInterfaceRef>(const_cast<void*>(CFArrayGetValueAtIndex(all, i)));
                const CFStringRef type = SCNetworkInterfaceGetInterfaceType(item);
                const std::string bsd = cf(SCNetworkInterfaceGetBSDName(item));
                if (!type || bsd.empty()) {
                    continue;
                }
                if (CFEqual(type, kSCNetworkInterfaceTypeEthernet)) {
                    kinds[bsd] = {RCS_POWER_LINK_ETHERNET, cf(SCNetworkInterfaceGetLocalizedDisplayName(item))};
                } else if (CFEqual(type, kSCNetworkInterfaceTypeIEEE80211)) {
                    kinds[bsd] = {RCS_POWER_LINK_WIFI, cf(SCNetworkInterfaceGetLocalizedDisplayName(item))};
                }
            }
            CFRelease(all);
        }
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) != 0) {
            throw Refused(RCS_ERROR_INTERNAL, std::string("The network interfaces could not be listed (") +
                                                  std::strerror(errno) + ")");
        }
        std::map<std::string, std::array<uint8_t, 6>> macs;
        struct Address {
            uint32_t address, prefix;
        };
        std::map<std::string, Address> addresses;
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || !kinds.count(i->ifa_name)) {
                continue;
            }
            if (i->ifa_addr->sa_family == AF_LINK) {
                const auto* link = reinterpret_cast<const sockaddr_dl*>(i->ifa_addr);
                if (link->sdl_type == IFT_ETHER && link->sdl_alen == 6) {
                    std::array<uint8_t, 6> mac{};
                    std::memcpy(mac.data(), LLADDR(link), 6);
                    macs[i->ifa_name] = mac;
                }
            } else if (i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) && (i->ifa_flags & IFF_RUNNING) &&
                       !(i->ifa_flags & IFF_LOOPBACK) && i->ifa_netmask && !addresses.count(i->ifa_name)) {
                const uint32_t address = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
                const uint32_t mask = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
                addresses[i->ifa_name] = {address, uint32_t(__builtin_popcount(mask))};
            }
        }
        freeifaddrs(list);
        const rcs_power_wake_setting wake = wakeForNetwork();
        std::vector<rcs_power_interface> found;
        for (const auto& [bsd, address] : addresses) {
            const auto mac = macs.find(bsd);
            if (mac == macs.end()) {
                continue;
            }
            uint8_t bytes[6];
            std::copy(mac->second.begin(), mac->second.end(), bytes);
            const auto& kind = kinds[bsd];
            found.push_back(makeInterface(bytes, kind.link, wake, address.address, address.prefix,
                                          kind.name.empty() ? bsd : kind.name));
        }
        std::stable_sort(found.begin(), found.end(), [](const rcs_power_interface& x, const rcs_power_interface& y) {
            return x.link < y.link;
        });
        return found;
    }

    std::string noInterfaces() override { return {}; }
};

} // namespace

std::shared_ptr<Machine> systemMachine() {
    return std::make_shared<MacMachine>();
}

} // namespace rcsdk::power
