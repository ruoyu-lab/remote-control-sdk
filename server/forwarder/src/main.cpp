// rcs_forwarder: the forwarder as a program (rcsfwd.h).
//
//   rcs_forwarder --config FILE          serve until SIGINT or SIGTERM, one line per event; SIGHUP
//                                         opens log_file again (logrotate moved it: systemctl reload)
//   rcs_forwarder --service --config FILE    Windows: the same as a service (sc.exe create ... binPath= "...
//                                         --service --config FILE"); it stops with the service
//   rcs_forwarder --generate-identity FILE   a new identity key (hex), and its SHA-256 for the ends
//   rcs_forwarder --identity-sha256 FILE     the SHA-256 the ends check (rcs_forwarder_config), a token's audience
//   rcs_forwarder --sign-token --secret ID:HEX --room NAME --role publish|subscribe
//                 --audience HEX | --identity FILE
//                 [--route N] [--expires-in SECONDS] [--max-bitrate BPS]
//                                         a token, as the host's server signs one (for trying it out)
//
// The configuration file has one "name = value" a line ("#" starts a comment;
// server/forwarder/deploy/forwarder.conf.example has every setting):
//   listen_udp = 0.0.0.0:47995            listen_tcp, listen_websocket likewise; at least one
//   identity_key_file = /etc/rcs-forwarder/identity.key   required: 194 hex digits
//   secret = 1:<64 hex digits>            1 to 4, of distinct ids
//   max_rooms = 64                        max_viewers_per_room, max_ends, max_viewer_bitrate_bps,
//                                         max_pipe_bps, max_publish_bitrate_bps, cache_ms, cache_bytes,
//                                         max_cache_bytes likewise (rcsfwd_config)
//   log_file = /var/log/rcs-forwarder/forwarder.log   the event lines go there (appended) instead of
//                                         standard output
#include <rcsfwd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

volatile std::sig_atomic_t interrupted = 0;
volatile std::sig_atomic_t reopen = 0; // SIGHUP: log_file again.
std::mutex outMutex;                   // Below: the forwarder's threads write events, the main thread reopens.
FILE* out = stdout;                    // Where the event lines go.

std::string hex(const uint8_t* bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < size; ++i) {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 15];
    }
    return text;
}

std::vector<uint8_t> unhex(const std::string& text, size_t size, const std::string& what) {
    if (text.size() != size * 2 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        throw std::invalid_argument(what + ": " + std::to_string(size * 2) + " hex digits");
    }
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = uint8_t(std::stoul(text.substr(i * 2, 2), nullptr, 16));
    }
    return bytes;
}

std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path);
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void check(rcs_result result, const char* what) {
    if (result != RCS_OK) {
        char message[256];
        rcsfwd_get_last_error(message, sizeof(message));
        throw std::runtime_error(std::string(what) + ": " + message);
    }
}

rcsfwd_secret secretOf(const std::string& text) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) {
        throw std::invalid_argument("secret: ID:HEX");
    }
    rcsfwd_secret secret{};
    secret.key_id = uint32_t(std::stoul(text.substr(0, colon)));
    const auto bytes = unhex(text.substr(colon + 1), sizeof(secret.secret), "secret");
    std::memcpy(secret.secret, bytes.data(), bytes.size());
    return secret;
}

// The identity key in `path` (hex), and the SHA-256 of its public half.
std::vector<uint8_t> identityOf(const std::string& path) {
    return unhex(trimmed(readFile(path)), RCSFWD_IDENTITY_KEY_SIZE, "the identity key");
}

std::string identitySha256Of(const std::vector<uint8_t>& key) {
    uint8_t sha256[32];
    check(rcsfwd_public_key_sha256(key.data(), sha256), "the identity key");
    return hex(sha256, sizeof(sha256));
}

const char* eventName(rcsfwd_event_type type) {
    switch (type) {
    case RCSFWD_EVENT_ROOM_OPENED:
        return "room opened";
    case RCSFWD_EVENT_ROOM_CLOSED:
        return "room closed";
    case RCSFWD_EVENT_JOINED:
        return "joined";
    case RCSFWD_EVENT_LEFT:
        return "left";
    case RCSFWD_EVENT_REFUSED:
        return "refused";
    default:
        return "?";
    }
}

const char* entryName(rcsfwd_entry entry) {
    return entry == RCSFWD_ENTRY_TCP ? "TCP" : entry == RCSFWD_ENTRY_WEBSOCKET ? "WebSocket" : "UDP";
}

void printEvent(const rcsfwd_event* e, void*) {
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
    std::lock_guard lock(outMutex);
    std::fprintf(out, "%s forwarder: %s, room \"%s\", %s, peer %llu, route %llu, %s %s:%u%s%s\n", when,
                 eventName(e->type), e->room,
                 e->role == RCSFWD_ROLE_PUBLISH     ? "sharer"
                 : e->role == RCSFWD_ROLE_SUBSCRIBE ? "viewer"
                                                    : "end",
                 static_cast<unsigned long long>(e->peer), static_cast<unsigned long long>(e->route),
                 entryName(e->entry), e->address, e->port, e->message[0] ? ": " : "", e->message);
    std::fflush(out);
}

// log_file opened again, once logrotate moved it away (SIGHUP); the old one
// is kept when it cannot be.
void reopenLog(const std::string& path) {
    FILE* again = std::fopen(path.c_str(), "a");
    const int error = errno;
    std::lock_guard lock(outMutex);
    if (again) {
        std::fclose(out);
        out = again;
    } else {
        std::fprintf(out, "forwarder: cannot open %s again (%s); writing on here\n", path.c_str(),
                     std::strerror(error));
    }
    std::fflush(out);
}

int serve(const std::string& path) {
    rcsfwd_config config = RCSFWD_CONFIG_INIT;
    std::vector<rcsfwd_secret> secrets;
    std::istringstream lines(readFile(path));
    std::string line;
    const auto text = [](char (&field)[64], const std::string& name, const std::string& value) {
        if (value.size() >= sizeof(field)) {
            throw std::invalid_argument(name + " is too long");
        }
        std::strcpy(field, value.c_str());
    };
    const std::map<std::string, uint32_t*> numbers = {
        {"max_rooms", &config.max_rooms},       {"max_viewers_per_room", &config.max_viewers_per_room},
        {"max_ends", &config.max_ends},         {"max_viewer_bitrate_bps", &config.max_viewer_bitrate_bps},
        {"max_pipe_bps", &config.max_pipe_bps}, {"max_publish_bitrate_bps", &config.max_publish_bitrate_bps},
        {"cache_ms", &config.cache_ms},         {"cache_bytes", &config.cache_bytes}};
    std::string log;
    while (std::getline(lines, line)) {
        line = trimmed(line.substr(0, line.find('#')));
        if (line.empty()) {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw std::invalid_argument("not \"name = value\": " + line);
        }
        const auto name = trimmed(line.substr(0, equals)), value = trimmed(line.substr(equals + 1));
        if (name == "listen_udp") {
            text(config.listen_udp, name, value);
        } else if (name == "listen_tcp") {
            text(config.listen_tcp, name, value);
        } else if (name == "listen_websocket") {
            text(config.listen_websocket, name, value);
        } else if (name == "identity_key_file") {
            const auto key = identityOf(value);
            std::memcpy(config.identity_key, key.data(), key.size());
        } else if (name == "secret") {
            secrets.push_back(secretOf(value));
        } else if (const auto number = numbers.find(name); number != numbers.end()) {
            *number->second = uint32_t(std::stoul(value));
        } else if (name == "max_cache_bytes") {
            config.max_cache_bytes = std::stoull(value);
        } else if (name == "log_file") {
            log = value;
        } else {
            throw std::invalid_argument("unknown setting \"" + name + "\"");
        }
    }
    if (!log.empty()) {
        out = std::fopen(log.c_str(), "a");
        if (!out) {
            throw std::runtime_error("cannot write " + log);
        }
    }
    config.secrets = secrets.data();
    config.secret_count = uint32_t(secrets.size());
    config.on_event = printEvent;
    rcsfwd forwarder = nullptr;
    check(rcsfwd_create(&config, &forwarder), "the configuration");
    struct Destroy {
        rcsfwd forwarder;
        ~Destroy() { rcsfwd_destroy(forwarder); }
    } destroy{forwarder};
    check(rcsfwd_start(forwarder), "listening");
    uint8_t sha256[32];
    rcsfwd_public_key_sha256(config.identity_key, sha256);
    {
        std::lock_guard lock(outMutex);
        std::fprintf(out, "forwarder: identity %s\n", hex(sha256, sizeof(sha256)).c_str());
        for (const rcsfwd_entry entry : {RCSFWD_ENTRY_UDP, RCSFWD_ENTRY_TCP, RCSFWD_ENTRY_WEBSOCKET}) {
            uint32_t port = 0;
            if (rcsfwd_get_port(forwarder, entry, &port) == RCS_OK) {
                std::fprintf(out, "forwarder: listening on %s port %u\n", entryName(entry), port);
            }
        }
        std::fflush(out);
    }
    while (!interrupted) {
        if (reopen) {
            reopen = 0;
            if (!log.empty()) {
                reopenLog(log);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    rcsfwd_stop(forwarder);
    std::fprintf(out, "forwarder: stopped\n");
    std::fflush(out);
    return 0;
}

int generateIdentity(const std::string& path) {
    uint8_t key[RCSFWD_IDENTITY_KEY_SIZE];
    check(rcsfwd_generate_identity(key), "a new identity");
#ifndef _WIN32
    umask(077); // A new key file is its owner's alone (Windows: the folder's ACL says).
#endif
    std::ofstream file(path);
    if (!(file << hex(key, sizeof(key)) << "\n" << std::flush)) {
        throw std::runtime_error("cannot write " + path);
    }
    uint8_t sha256[32];
    check(rcsfwd_public_key_sha256(key, sha256), "its SHA-256");
    std::printf("%s\n", hex(sha256, sizeof(sha256)).c_str());
    return 0;
}

int identitySha256(const std::string& path) {
    std::printf("%s\n", identitySha256Of(identityOf(path)).c_str());
    return 0;
}

int signToken(const std::map<std::string, std::string>& args) {
    const auto get = [&](const std::string& name, const std::string& otherwise = {}) {
        const auto found = args.find(name);
        if (found == args.end() && otherwise.empty()) {
            throw std::invalid_argument(name + " is required");
        }
        return found == args.end() ? otherwise : found->second;
    };
    const auto secret = secretOf(get("--secret"));
    rcsfwd_grant grant = RCSFWD_GRANT_INIT;
    const auto room = get("--room");
    if (room.size() >= sizeof(grant.room)) {
        throw std::invalid_argument("--room: at most 64 bytes");
    }
    std::strcpy(grant.room, room.c_str());
    const auto role = get("--role");
    grant.role = role == "publish" ? RCSFWD_ROLE_PUBLISH : role == "subscribe" ? RCSFWD_ROLE_SUBSCRIBE : 0;
    grant.route = std::stoull(get("--route", "0"));
    grant.expires = uint64_t(std::time(nullptr)) + std::stoull(get("--expires-in", "3600"));
    grant.max_bitrate_bps = uint32_t(std::stoul(get("--max-bitrate", "0")));
    const auto audience =
        args.count("--identity") ? identitySha256Of(identityOf(args.at("--identity"))) : get("--audience");
    const auto bytes = unhex(audience, sizeof(grant.audience), "--audience");
    std::memcpy(grant.audience, bytes.data(), bytes.size());
    char token[RCS_FORWARDER_TOKEN_SIZE];
    check(rcsfwd_sign_token(&secret, &grant, token, sizeof(token)), "the token");
    std::printf("%s\n", token);
    return 0;
}

void usage() {
    std::fprintf(stderr, "usage: rcs_forwarder [--service] --config FILE\n"
                         "       rcs_forwarder --generate-identity FILE\n"
                         "       rcs_forwarder --identity-sha256 FILE\n"
                         "       rcs_forwarder --sign-token --secret ID:HEX --room NAME --role publish|subscribe\n"
                         "                     --audience HEX | --identity FILE\n"
                         "                     [--route N] [--expires-in SECONDS] [--max-bitrate BPS]\n");
}

#ifdef _WIN32
// As a Windows service: the service control manager starts the program and
// asks it to stop; it serves meanwhile, as from a console.
std::string serviceConfig;
SERVICE_STATUS_HANDLE serviceHandle = nullptr;

void report(DWORD state, DWORD exitCode = NO_ERROR) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = exitCode;
    status.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 5000;
    SetServiceStatus(serviceHandle, &status);
}

DWORD WINAPI control(DWORD code, DWORD, void*, void*) {
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        report(SERVICE_STOP_PENDING);
        interrupted = 1;
    }
    return NO_ERROR;
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    serviceHandle = RegisterServiceCtrlHandlerExW(L"", control, nullptr);
    if (!serviceHandle) {
        return;
    }
    report(SERVICE_RUNNING);
    int result = 1;
    try {
        result = serve(serviceConfig);
    } catch (const std::exception& error) {
        std::fprintf(out, "rcs_forwarder: %s\n", error.what());
        std::fflush(out);
    }
    report(SERVICE_STOPPED, result ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR);
}

int runService(const std::string& config) {
    serviceConfig = config;
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(L""), serviceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        throw std::runtime_error("--service: not started by the service control manager (error " +
                                 std::to_string(GetLastError()) + ")");
    }
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> args;
    std::string command;
    bool service = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sign-token") {
            command = arg;
        } else if (arg == "--service") {
            service = true;
        } else if (arg.rfind("--", 0) == 0 && i + 1 < argc) {
            args[arg] = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    std::signal(SIGINT, [](int) { interrupted = 1; });
    std::signal(SIGTERM, [](int) { interrupted = 1; });
#ifndef _WIN32
    std::signal(SIGHUP, [](int) { reopen = 1; }); // Not the end: systemctl reload, logrotate.
#endif
    try {
        if (command == "--sign-token") {
            return signToken(args);
        }
        if (service && args.count("--config")) {
#ifdef _WIN32
            return runService(args.at("--config"));
#else
            throw std::invalid_argument("--service is for Windows; systemd and launchd run the program as it is");
#endif
        }
        if (args.count("--config")) {
            return serve(args.at("--config"));
        }
        if (args.count("--generate-identity")) {
            return generateIdentity(args.at("--generate-identity"));
        }
        if (args.count("--identity-sha256")) {
            return identitySha256(args.at("--identity-sha256"));
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "rcs_forwarder: %s\n", error.what());
        return 1;
    }
    usage();
    return 2;
}
