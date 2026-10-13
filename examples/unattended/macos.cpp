// A macOS service that shares the machine around the clock, from the login
// window on: a test program for unattended access on macOS (launchd,
// rcs_agent_*, RCS_EVENT_DESKTOP). It is not part of the SDK and stands for no
// product; a host writes its own daemon, installer, key storage and consent.
//
// launchd does what the Windows supervisor does. A LaunchDaemon (root, from
// boot on) holds the sharing session: the network, the keys, the viewers. A
// LaunchAgent limited to the LoginWindow and Aqua session types is its agent
// (capture, encoding, sound, input): launchd runs one at the login window (as
// root) and one in each user's session (as that user), and starts and ends
// them as users log in and out. A viewer stays connected while the agent
// changes.
//
// The session takes one agent: this program, run by whoever is in front of the
// screen (root at the login window, else the user on the console), with Screen
// Recording granted. Any other is refused, and the log says why; it tries
// again, and is taken once the console or its permission changes.
//
// The agent needs Screen Recording (and Accessibility for remote input). macOS
// grants them per program, to a person at the machine (System Settings ›
// Privacy & Security) or, on a managed Mac, by an MDM profile (PPPC); nothing
// grants them unattended otherwise. The session needs neither.
//
//   sudo rcsdk_unattended install   [--port N] [--address A] [--keep-awake none|system|display]
//                                   [--label L] [--dir DIR] [--endpoint PATH]
//                                   [--power-actions LIST] [--share-wake-info yes|no]
//   sudo rcsdk_unattended uninstall [--label L]
//   rcsdk_unattended plists         [same options]   print both property lists
//   rcsdk_unattended session        [same options]   what the LaunchDaemon runs
//   rcsdk_unattended agent          [same options]   what the LaunchAgent runs
#include <rcsdk/agent.hpp>
#include <rcsdk/power.h>
#include <rcsdk/rcsdk.hpp>

#include <SystemConfiguration/SystemConfiguration.h>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

struct Options {
    std::string command;
    std::string address = "0.0.0.0";
    uint32_t port = 47995;
    fs::path directory = "/Library/Application Support/RcsdkUnattended";
    std::string endpoint; // Default: agent.sock in the directory, which only root may change (as the SDK asks).
    std::string label = "local.rcsdk.unattended";
    rcs_keep_awake keepAwake = RCS_KEEP_AWAKE_SYSTEM;
    uint32_t powerActions = 0; // What viewers may ask the machine to do (rcs_power_settings.actions).
    bool shareWakeInfo = false;

    fs::path connectionKey() const { return directory / "connection.key"; }
    fs::path linkKey() const { return directory / "link.key"; }
    // A user's copy of the link key: that user's alone.
    fs::path linkKey(uid_t user) const { return directory / ("link-" + std::to_string(user) + ".key"); }
    fs::path daemonPlist() const { return "/Library/LaunchDaemons/" + label + ".session.plist"; }
    fs::path agentPlist() const { return "/Library/LaunchAgents/" + label + ".agent.plist"; }
};

std::atomic<bool> interrupted{false};

fs::path selfPath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        throw std::runtime_error("This program's path cannot be read");
    }
    return fs::canonical(path.c_str());
}

// ---- Options --------------------------------------------------------------------------------

// --power-actions: restart, shut-down, sleep, sign-out (comma-separated), all or none.
uint32_t powerActions(const std::string& list) {
    if (list == "all") {
        return RCS_POWER_ALL_ACTIONS;
    }
    const char* names[] = {"", "restart", "shut-down", "sleep", "sign-out"};
    uint32_t bits = 0;
    size_t start = 0;
    while (list != "none" && start <= list.size()) {
        const size_t end = std::min(list.find(',', start), list.size());
        const std::string name = list.substr(start, end - start);
        uint32_t action = RCS_POWER_RESTART;
        while (action <= RCS_POWER_SIGN_OUT && name != names[action]) {
            ++action;
        }
        if (action > RCS_POWER_SIGN_OUT) {
            throw std::invalid_argument("--power-actions takes restart, shut-down, sleep, sign-out, all or none");
        }
        bits |= 1u << action;
        start = end + 1;
    }
    return bits;
}

std::string powerActionList(uint32_t bits) {
    if (bits == RCS_POWER_ALL_ACTIONS) {
        return "all";
    }
    const char* names[] = {"", "restart", "shut-down", "sleep", "sign-out"};
    std::string list;
    for (uint32_t action = RCS_POWER_RESTART; action <= RCS_POWER_SIGN_OUT; ++action) {
        if (bits & (1u << action)) {
            list += std::string(list.empty() ? "" : ",") + names[action];
        }
    }
    return list.empty() ? "none" : list;
}

void logLine(const std::string& text);

// The power module's steps, for the log.
void logPower(const rcs_event& event) {
    const auto& m = event.module;
    if (std::strcmp(m.module, RCS_POWER_MODULE) != 0 || m.kind != RCS_POWER_ACTION ||
        m.size != sizeof(rcs_power_action_event)) {
        return;
    }
    const auto& a = *static_cast<const rcs_power_action_event*>(m.payload);
    const char* actions[] = {"?", "restart", "shut down", "sleep", "sign out"};
    const char* states[] = {"?", "refused", "accepted", "started", "failed"};
    logLine("viewer " + std::to_string(event.viewer_id) + " asked to " + actions[a.action <= 4 ? a.action : 0] +
            (a.flags & RCS_POWER_FORCE ? " (force)" : "") + ": " + states[a.state <= 4 ? a.state : 0] +
            (a.result == RCS_OK ? std::string() : std::string(" (") + a.message + ")"));
}

[[noreturn]] void usage(int code) {
    std::cout << "sudo rcsdk_unattended install   [--port N] [--address A] [--keep-awake none|system|display]\n"
                 "                                [--label L] [--dir DIR] [--endpoint PATH]\n"
                 "                                [--power-actions restart,shut-down,sleep,sign-out|all]\n"
                 "                                [--share-wake-info yes|no]\n"
                 "                                install and start the LaunchDaemon and the LaunchAgent\n"
                 "sudo rcsdk_unattended uninstall [--label L]   stop and remove both (keys and logs stay)\n"
                 "rcsdk_unattended plists         [same options]   print both property lists\n"
                 "rcsdk_unattended session        [same options]   what the LaunchDaemon runs: the sharing session\n"
                 "rcsdk_unattended agent          [same options]   what the LaunchAgent runs: the session's agent\n"
                 "\n"
                 "The program and its directory must belong to root and be writable by root only (e.g.\n"
                 "/Library/RcsdkUnattended/bin). Keys, the agents' endpoint (agent.sock, unless --endpoint\n"
                 "names another in a directory only root may change) and the session's log live in --dir\n"
                 "(default /Library/Application Support/RcsdkUnattended): connection.key is the one a viewer\n"
                 "needs (rcsdk_cli --role view --key-file ...). The agent logs to that directory at the login window\n"
                 "and to ~/Library/Logs/RcsdkUnattended in a user's session. Grant Screen Recording (and\n"
                 "Accessibility, for remote input) to this program, or the session waits without a picture.\n"
                 "--power-actions lets a viewer holding control restart, shut down, sleep or sign out this Mac (for\n"
                 "real, 3 s after the answer). The agent carries it out: in a user's session through loginwindow\n"
                 "(without force; applications may hold it up), at the login window as root (with force only).\n"
                 "--share-wake-info yes tells viewers its MAC addresses for Wake-on-LAN.\n";
    std::exit(code);
}

Options parse(int argc, char** argv) {
    if (argc < 2) {
        usage(2);
    }
    Options o;
    o.command = argv[1];
    std::map<std::string, std::string> values;
    for (int i = 2; i < argc; ++i) {
        const std::string name = argv[i];
        if (name.rfind("--", 0) == 0 && i + 1 < argc) {
            values[name] = argv[++i];
        } else {
            std::cerr << "Unexpected argument: " << name << "\n";
            usage(2);
        }
    }
    const auto get = [&](const char* name, const std::string& fallback) {
        const auto found = values.find(name);
        return found == values.end() ? fallback : found->second;
    };
    o.address = get("--address", o.address);
    o.port = uint32_t(std::stoul(get("--port", std::to_string(o.port))));
    o.directory = get("--dir", o.directory.string());
    o.endpoint = get("--endpoint", (o.directory / "agent.sock").string());
    o.label = get("--label", o.label);
    const std::string awake = get("--keep-awake", "system");
    if (awake != "none" && awake != "system" && awake != "display") {
        std::cerr << "--keep-awake takes none, system or display\n";
        usage(2);
    }
    o.keepAwake = awake == "display"  ? RCS_KEEP_AWAKE_DISPLAY
                  : awake == "system" ? RCS_KEEP_AWAKE_SYSTEM
                                      : RCS_KEEP_AWAKE_OFF;
    try {
        o.powerActions = powerActions(get("--power-actions", "none"));
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << "\n";
        usage(2);
    }
    const std::string wake = get("--share-wake-info", "no");
    if (wake != "yes" && wake != "no") {
        std::cerr << "--share-wake-info takes yes or no\n";
        usage(2);
    }
    o.shareWakeInfo = wake == "yes";
    return o;
}

// What the installed daemon and agent are given.
std::vector<std::string> optionArguments(const Options& o) {
    return {"--address",
            o.address,
            "--port",
            std::to_string(o.port),
            "--dir",
            o.directory.string(),
            "--endpoint",
            o.endpoint,
            "--label",
            o.label,
            "--keep-awake",
            o.keepAwake == RCS_KEEP_AWAKE_DISPLAY  ? "display"
            : o.keepAwake == RCS_KEEP_AWAKE_SYSTEM ? "system"
                                                   : "none",
            "--power-actions",
            powerActionList(o.powerActions),
            "--share-wake-info",
            o.shareWakeInfo ? "yes" : "no"};
}

// ---- Who may change what --------------------------------------------------------------------

void requireRoot(const char* command) {
    if (geteuid() != 0) {
        throw std::runtime_error(std::string(command) + " needs root: run it with sudo (or from launchd)");
    }
}

// Why `path` is not protected, or empty when it is: `owner` (root) owns it and
// nobody else may write it; `secret`: nobody else may read it either.
std::string protectionProblem(const fs::path& path, bool secret, uid_t owner = 0) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0) {
        return std::string("it cannot be read (") + std::strerror(errno) + ")";
    }
    if (S_ISLNK(info.st_mode)) {
        return "it is a symbolic link";
    }
    if (info.st_uid != owner) {
        const passwd* owner = getpwuid(info.st_uid);
        return "it belongs to " + (owner ? std::string(owner->pw_name) : std::to_string(info.st_uid));
    }
    if (info.st_mode & (S_IWGRP | S_IWOTH)) {
        return "other accounts may change it";
    }
    if (secret && (info.st_mode & (S_IRWXG | S_IRWXO))) {
        return "other accounts may read it";
    }
    return {};
}

void requireProtected(const fs::path& path, bool secret, const char* what, uid_t owner = 0) {
    if (const auto problem = protectionProblem(path, secret, owner); !problem.empty()) {
        throw std::runtime_error(std::string(what) + " " + path.string() + " is not protected: " + problem +
                                 (secret ? ". Delete it as root; install creates it for root only."
                                         : ". Install the program where only root can change it, e.g. "
                                           "/Library/RcsdkUnattended/bin."));
    }
}

// The program launchd runs as root, and the directory it is loaded from:
// nobody else may change either.
void requireProtectedProgram() {
    const fs::path self = selfPath();
    requireProtected(self, false, "The program");
    requireProtected(self.parent_path(), false, "The program's directory");
}

// The directory of keys and logs: created for root, or checked to be so (a
// user who made it first could otherwise plant keys or read them).
void ensureDirectory(const Options& o) {
    if (mkdir(o.directory.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::runtime_error("Could not create " + o.directory.string() + " (" + std::strerror(errno) + ")");
    }
    requireProtected(o.directory, false, "The directory");
}

// A key file, `owner`'s and readable by it alone.
void writeKeyFile(const fs::path& path, const rcsdk::ConnectionKey& key, uid_t owner) {
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0) {
        throw std::runtime_error("Could not create " + path.string() + " (" + std::strerror(errno) + ")");
    }
    const bool wrote = write(file, key.data(), key.size()) == ssize_t(key.size()) &&
                       (owner == 0 || fchown(file, owner, gid_t(-1)) == 0);
    close(file);
    if (!wrote) {
        fs::remove(path);
        throw std::runtime_error("Could not write " + path.string());
    }
}

// connection.key: a viewer's; link.key: the agents'; both for root only. An
// agent runs as whoever is in front of the screen, so the session gives each
// console user a copy of the link key of its own (link-<uid>.key, see
// runSession), which no other account may read. Holding it lets a local
// program through the link's handshake, no further: the session takes only
// this program, by the path the system reports, in front of the screen
// (Admission), and the agent serves only a session run by root
// (rcs_agent_config.access).
void ensureKeys(const Options& o) {
    ensureDirectory(o);
    for (const auto& path : {o.connectionKey(), o.linkKey()}) {
        if (!fs::exists(path)) {
            writeKeyFile(path, rcsdk::generateConnectionKey(), 0);
            std::cout << "Created " << path.string() << "\n";
        }
    }
}

rcsdk::ConnectionKey readKey(const fs::path& path, bool secret, uid_t owner = 0) {
    requireProtected(path, secret, "The key", owner);
    rcsdk::ConnectionKey key{};
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(key.data()), key.size())) {
        throw std::runtime_error("Cannot read the key " + path.string() + " (create it with: sudo ... install)");
    }
    return key;
}

// ---- Logs ------------------------------------------------------------------------------------

std::mutex logMutex;
std::ofstream logFile;

void logLine(const std::string& text) {
    std::lock_guard lock(logMutex);
    if (!logFile.is_open()) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    logFile << stamp << ' ' << text << std::endl;
}

void openLog(const fs::path& path, const char* role) {
    std::error_code ignored;
    fs::create_directories(path.parent_path(), ignored);
    logFile.open(path, std::ios::app);
    rcsdk::setLogCallback(RCS_LOG_INFO, [](const rcs_log_record& record) {
        static const char* levels[] = {"", "error", "warning", "info", "debug"};
        static const char* sources[] = {"process", "session", "agent", "supervisor"};
        logLine(std::string("[") + levels[record.level] + "] " + sources[record.source] + " " + record.module + ": " +
                record.message);
    });
    logLine(std::string("--- ") + role + " started (process " + std::to_string(getpid()) + ", user " +
            std::to_string(geteuid()) + ") ---");
}

// launchd ends its jobs with SIGTERM.
void handleSignals() {
    std::signal(SIGTERM, [](int) { interrupted = true; });
    std::signal(SIGINT, [](int) { interrupted = true; });
}

// ---- The session (LaunchDaemon) ------------------------------------------------------------

// Who is in front of the screen: the console user's name, or empty at the
// login window.
std::string consoleUser() {
    uid_t uid = 0;
    CFStringRef name = SCDynamicStoreCopyConsoleUser(nullptr, &uid, nullptr);
    if (!name) {
        return {};
    }
    char text[256]{};
    CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8);
    CFRelease(name);
    const std::string user = text;
    return user == "loginwindow" ? std::string() : user;
}

// The console user's ID; 0 at the login window.
uid_t consoleUid() {
    uid_t uid = 0;
    if (CFStringRef name = SCDynamicStoreCopyConsoleUser(nullptr, &uid, nullptr)) {
        CFRelease(name);
    }
    return uid;
}

// Which agent the session takes (see the top of this file): the program, by
// the path the system reports for the process that spoke on the link (never
// by looking its process ID up, which may name another process by then). A
// refusal is logged once until the reason changes; the agent tries again by
// itself.
class Admission final : public rcsdk::AgentGate {
public:
    explicit Admission(std::string program) : program_(std::move(program)) {}

    bool attach(const rcs_agent_info& agent) override {
        const std::string console = consoleUser();
        const std::string expected = console.empty() ? "root" : console;
        std::string problem;
        if (agent.program != program_) {
            problem = "process " + std::to_string(agent.process_id) + " runs " +
                      (agent.program[0] ? agent.program : "?") + ", not " + program_;
        } else if (agent.account != expected) {
            problem = std::string("it runs as ") + agent.account + ", and " +
                      (console.empty() ? "the login window is in front (root's)" : "the console is " + console + "'s");
        } else if (agent.screen_permission != RCS_PERMISSION_GRANTED) {
            problem = std::string("Screen Recording is not granted to ") + program_ + " for " + agent.account +
                      " (System Settings > Privacy & Security, or an MDM profile)";
        }
        std::lock_guard lock(mutex_);
        if (problem.empty()) {
            last_.clear();
            if (agent.input_permission != RCS_PERMISSION_GRANTED) {
                logLine("note: Accessibility is not granted to the agent; a viewer can watch but not control");
            }
            return true;
        }
        if (problem != last_) {
            logLine("refused an agent: " + problem);
            last_ = problem;
        }
        return false;
    }

private:
    const std::string program_;
    std::mutex mutex_;
    std::string last_;
};

rcsdk::Callbacks sessionCallbacks() {
    rcsdk::Callbacks callbacks;
    callbacks.onEvent = [](const rcs_event& event) {
        switch (event.type) {
        case RCS_EVENT_STATE:
            logLine("state " + std::to_string(event.state.value) + " (" + rcs_result_message(event.state.result) + ")");
            break;
        case RCS_EVENT_PEER:
            logLine("a viewer connected" + std::string(event.peer.has_public_key ? " (with a device key)" : ""));
            break;
        case RCS_EVENT_DESKTOP:
            logLine(std::string("the machine shows ") +
                    (event.desktop.kind == RCS_DESKTOP_USER ? "the user's desktop"
                     : event.desktop.kind == RCS_DESKTOP_SECURE
                         ? (event.desktop.user_logged_on ? "a locked screen" : "the login window")
                         : "an unknown desktop"));
            break;
        case RCS_EVENT_CAPTURE:
            logLine(event.capture.paused ? "capture paused (reason " + std::to_string(event.capture.paused) + ")"
                                         : "capture resumed");
            break;
        case RCS_EVENT_MODULE:
            logPower(event);
            break;
        case RCS_EVENT_AGENT:
            logLine(std::string("agent ") + (event.agent.attached ? "attached" : "left") + " (process " +
                    std::to_string(event.agent.process_id) + ", " + event.agent.account + ")" +
                    (event.agent.attached
                         ? std::string()
                         : std::string(": ") + rcs_result_message(event.agent.result) + " " + event.agent.message));
            break;
        default:
            break;
        }
    };
    return callbacks;
}

int runSession(const Options& o) {
    openLog(o.directory / "session.log", "session");
    try {
        requireRoot("session");
        requireProtectedProgram();
        Admission admission(selfPath().string());
        rcsdk::SessionConfig config(RCS_ROLE_SHARE);
        // Agents run as whoever logs in, so any account may open the endpoint; which agent is
        // taken is decided by admission.
        const auto linkKey = readKey(o.linkKey(), true);
        config.setAddress(o.address, o.port)
            .setKey(readKey(o.connectionKey(), true))
            .setControl(true)
            .use(rcsdk::agentLink(o.endpoint, linkKey, RCS_AGENT_ACCESS_ANY_USER, &admission));
        if (o.keepAwake != RCS_KEEP_AWAKE_OFF || o.powerActions || o.shareWakeInfo) {
            // Reachable at any hour, not only while someone is connected: the agent holds the request.
            rcs_power_settings power = RCS_POWER_SETTINGS_INIT;
            power.keep_awake = o.keepAwake;
            power.while_running = 1;
            // A viewer holding control may restart (...) the Mac; the agent carries it out. After a
            // restart launchd starts the daemon (RunAtLoad) and the login window's agent again.
            power.actions = o.powerActions;
            power.share_wake_info = o.shareWakeInfo ? 1 : 0;
            config.useModule(RCS_POWER_MODULE, power);
        }
        rcsdk::Session session(config, sessionCallbacks());
        if (const auto result = session.start(); result != RCS_OK) {
            logLine(std::string("start failed: ") + rcs_result_message(result) + ": " + session.failure().message);
            return 1;
        }
        logLine("listening on " + o.address + ":" + std::to_string(o.port) + "; agents attach at " + o.endpoint);
        int code = 0;
        uid_t console = 0;
        while (!interrupted) {
            std::this_thread::sleep_for(200ms);
            // The console user's agent reads its own copy of the link key, made as that user comes to the
            // console (made again if it is no longer that user's alone).
            if (const uid_t now = consoleUid(); now != console) {
                console = now;
                const auto copy = o.linkKey(now);
                if (now && !protectionProblem(copy, true, now).empty()) {
                    std::error_code ignored;
                    fs::remove(copy, ignored);
                    try {
                        writeKeyFile(copy, linkKey, now);
                    } catch (const std::exception& error) {
                        logLine(std::string("error: ") + error.what());
                    }
                }
            }
            if (session.state() == RCS_STATE_FAILED) {
                logLine("the session failed: " + session.failure().message);
                code = 1;
                break;
            }
        }
        logLine("stopping");
        session.stop(RCS_STOP_RESTART); // A viewer reconnects when launchd has started the daemon again.
        return code;
    } catch (const std::exception& error) {
        logLine(std::string("error: ") + error.what());
        std::cerr << error.what() << "\n";
        return 1;
    }
}

// ---- The agent (LaunchAgent) --------------------------------------------------------------

int runAgent(const Options& o) {
    // Root runs it at the login window; a user's own copy logs where that user may write.
    const char* home = std::getenv("HOME");
    openLog(geteuid() == 0 || !home ? o.directory / "agent-loginwindow.log"
                                    : fs::path(home) / "Library/Logs/RcsdkUnattended/agent.log",
            "agent");
    try {
        // Root's at the login window; a user's own copy, which the session makes as the user comes to the
        // console (moments after the agent may have started).
        const uid_t self = geteuid();
        const fs::path key = self == 0 ? o.linkKey() : o.linkKey(self);
        for (int wait = 0; wait < 150 && !interrupted && !fs::exists(key); ++wait) {
            std::this_thread::sleep_for(200ms);
        }
        rcsdk::AgentConfig config(o.endpoint, readKey(key, true, self));
        config.setReconnect(true, 3000);
        rcsdk::Agent agent(config, [](const rcs_agent_event& event) {
            logLine("agent state " + std::to_string(event.state.value) + " (" + rcs_result_message(event.state.result) +
                    ")");
        });
        if (const auto result = agent.start(); result != RCS_OK) {
            logLine(std::string("start failed: ") + rcs_result_message(result));
            return 1;
        }
        int code = 0;
        while (!interrupted) {
            std::this_thread::sleep_for(200ms);
            const auto state = agent.state();
            if (state == RCS_STATE_FAILED || state == RCS_STATE_STOPPED) { // Another version; another agent took over.
                logLine("the agent ended: " + agent.failure().message);
                code = state == RCS_STATE_FAILED;
                break;
            }
        }
        agent.stop();
        return code;
    } catch (const std::exception& error) {
        logLine(std::string("error: ") + error.what());
        std::cerr << error.what() << "\n";
        return 1;
    }
}

// ---- Install -------------------------------------------------------------------------------

std::string escaped(const std::string& text) {
    std::string result;
    for (const char c : text) {
        result += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    }
    return result;
}

std::string plist(const Options& o, bool agent) {
    std::string text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                       "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                       "<plist version=\"1.0\">\n<dict>\n"
                       "    <key>Label</key>\n    <string>" +
                       escaped(o.label + (agent ? ".agent" : ".session")) +
                       "</string>\n"
                       "    <key>ProgramArguments</key>\n    <array>\n        <string>" +
                       escaped(selfPath().string()) + "</string>\n        <string>" + (agent ? "agent" : "session") +
                       "</string>\n";
    for (const auto& argument : optionArguments(o)) {
        text += "        <string>" + escaped(argument) + "</string>\n";
    }
    text += "    </array>\n"
            "    <key>RunAtLoad</key>\n    <true/>\n"
            "    <key>KeepAlive</key>\n    <true/>\n";
    if (agent) {
        // One at the login window (as root) and one in each user's session (as that user).
        text += "    <key>LimitLoadToSessionType</key>\n    <array>\n"
                "        <string>LoginWindow</string>\n        <string>Aqua</string>\n    </array>\n"
                "    <key>ProcessType</key>\n    <string>Interactive</string>\n";
    }
    return text + "</dict>\n</plist>\n";
}

// Runs launchctl; returns its exit status. `quiet`: without its messages
// (unloading what may not be loaded).
int launchctl(const std::vector<std::string>& arguments, bool quiet = false) {
    std::vector<char*> argv{const_cast<char*>("/bin/launchctl")};
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    if (quiet) {
        posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&files, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    pid_t child = 0;
    const int spawned = posix_spawn(&child, "/bin/launchctl", &files, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    if (spawned != 0) {
        return -1;
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void writePlist(const fs::path& path, const std::string& text) {
    const fs::path temporary = path.string() + ".new";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!(file << text)) {
            throw std::runtime_error("Could not write " + temporary.string());
        }
    }
    chmod(temporary.c_str(), 0644);
    fs::rename(temporary, path);
}

int install(const Options& o) {
    try {
        requireRoot("install");
        requireProtectedProgram();
        ensureKeys(o);
        const std::string session = o.label + ".session", agent = o.label + ".agent";
        const uid_t user = consoleUid();
        // Installing again replaces what runs.
        launchctl({"bootout", "system/" + session}, true);
        if (user) {
            launchctl({"bootout", "gui/" + std::to_string(user) + "/" + agent}, true);
        }
        writePlist(o.daemonPlist(), plist(o, false));
        writePlist(o.agentPlist(), plist(o, true));
        if (launchctl({"bootstrap", "system", o.daemonPlist().string()}) != 0) {
            throw std::runtime_error("launchctl could not start " + o.daemonPlist().string());
        }
        std::cout << "Started " << session << " (" << o.daemonPlist().string() << ")\n";
        if (user && launchctl({"bootstrap", "gui/" + std::to_string(user), o.agentPlist().string()}) == 0) {
            std::cout << "Started " << agent << " in the session of user " << user << "\n";
        } else {
            std::cout << agent << " starts at the login window and at each login (" << o.agentPlist().string() << ")\n";
        }
        std::cout << "Viewers connect to port " << o.port << " with " << o.connectionKey().string()
                  << ".\nGrant Screen Recording (and Accessibility, for remote input) to " << selfPath().string()
                  << ": System Settings > Privacy & Security, or an MDM profile.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}

int uninstall(const Options& o) {
    try {
        requireRoot("uninstall");
        const uid_t user = consoleUid();
        if (user) {
            launchctl({"bootout", "gui/" + std::to_string(user) + "/" + o.label + ".agent"}, true);
        }
        launchctl({"bootout", "system/" + o.label + ".session"}, true);
        std::error_code ignored;
        fs::remove(o.agentPlist(), ignored);
        fs::remove(o.daemonPlist(), ignored);
        std::cout << "Removed " << o.label << ".session and .agent; keys and logs stay in " << o.directory.string()
                  << "\n(an agent already running at the login window ends at the next login).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    handleSignals();
    // The session's keep-awake (--keep-awake); its machine's half runs where the agent is.
    rcsdk::registerModule(*rcs_power_module());
    if (options.command == "install") {
        return install(options);
    }
    if (options.command == "uninstall") {
        return uninstall(options);
    }
    if (options.command == "plists") {
        std::cout << "<!-- " << options.daemonPlist().string() << " -->\n"
                  << plist(options, false) << "\n<!-- " << options.agentPlist().string() << " -->\n"
                  << plist(options, true);
        return 0;
    }
    if (options.command == "session") {
        return runSession(options);
    }
    if (options.command == "agent") {
        return runAgent(options);
    }
    usage(2);
}
