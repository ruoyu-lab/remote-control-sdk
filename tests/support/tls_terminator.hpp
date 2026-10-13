// The tests' TLS terminator (support/tls_terminator.py) as a child process,
// and the self-signed certificate it serves: TURN over TLS (tls_relay.cpp),
// a forwarder's WebSocket entry behind it, wss:// (forwarder_tls.cpp). Not on
// Windows: they run there by hand against a Mac.
#pragma once

#ifndef _WIN32

#include "support/check.hpp"
#include "transport/socket.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace rcsdk::test {

// A process this one holds the input and output of; closing its input ends it
// (the terminator quits then). What it prints after the first line is kept.
class Child {
public:
    explicit Child(std::vector<std::string> arguments) {
        int in[2], out[2];
        CHECK(::pipe(in) == 0 && ::pipe(out) == 0);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, in[0], 0);
        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        posix_spawn_file_actions_addclose(&actions, in[1]);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        std::vector<char*> argv;
        for (auto& argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        const int spawned = posix_spawnp(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(in[0]);
        ::close(out[1]);
        CHECK(spawned == 0);
        input_ = in[1];
        output_ = fdopen(out[0], "r");
        first_ = readLine();
        reader_ = std::thread([this] {
            for (std::string line; !(line = readLine()).empty();) {
                std::printf("  terminator: %s\n", line.c_str());
                std::lock_guard lock(mutex_);
                lines_.push_back(line);
            }
        });
    }
    ~Child() {
        ::close(input_);
        int status = 0;
        waitpid(pid_, &status, 0);
        reader_.join();
        std::fclose(output_);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    const std::string& first() const noexcept { return first_; }
    bool printed(const std::string& text) {
        std::lock_guard lock(mutex_);
        return std::any_of(lines_.begin(), lines_.end(),
                           [&](const auto& line) { return line.find(text) != line.npos; });
    }

private:
    std::string readLine() {
        char text[512];
        if (!std::fgets(text, sizeof(text), output_)) {
            return {};
        }
        std::string line(text);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        return line;
    }

    pid_t pid_ = 0;
    int input_ = -1;
    FILE* output_ = nullptr;
    std::string first_;
    std::thread reader_;
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

// Runs a program to its end: its exit status, -1 when it did not run.
inline int run(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

struct Tools {
    std::string python = "python3", openssl = "openssl", script = "tls_terminator.py";
    bool available() const { return run({python, "--version"}) == 0 && run({openssl, "version"}) == 0; }
};

// A self-signed certificate for `name`, in this directory as STEM_cert.pem
// and STEM_key.pem (never a keychain).
inline void makeCertificate(const Tools& tools, const std::string& stem, const std::string& name) {
    CHECK(run({tools.openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
               "-keyout", stem + "_key.pem", "-out", stem + "_cert.pem", "-days", "30", "-subj", "/CN=" + name,
               "-addext", "subjectAltName=DNS:" + name}) == 0);
}

// The terminator in front of `backend` with STEM's certificate, agreeing to
// `alpn`, and what it said when it started: its port, the certificate's
// SHA-256, whether it offers TLS 1.3.
struct Terminator {
    Child child;
    uint16_t port = 0;
    std::array<uint8_t, 32> pin{};
    bool tls13 = false;

    Terminator(const Tools& tools, const transport::Endpoint& backend, const std::string& listen, uint16_t listenPort,
               const char* tls, const std::string& stem, const std::string& alpn = "stun.turn")
        : child({tools.python, tools.script, "--listen", listen, "--port", std::to_string(listenPort), "--backend",
                 backend.toString(), "--cert", stem + "_cert.pem", "--key", stem + "_key.pem", "--tls", tls, "--alpn",
                 alpn}) {
        char hex[65] = {}, tls13Text[8] = {};
        unsigned number = 0;
        if (std::sscanf(child.first().c_str(), "port %u sha256 %64s tls13 %7s", &number, hex, tls13Text) != 3) {
            std::fprintf(stderr, "the terminator did not start: \"%s\"\n", child.first().c_str());
            CHECK(false);
        }
        port = uint16_t(number);
        for (size_t i = 0; i < pin.size(); ++i) {
            pin[i] = uint8_t(std::stoul(std::string(hex + 2 * i, 2), nullptr, 16));
        }
        tls13 = std::string(tls13Text) == "yes";
    }
};

} // namespace rcsdk::test

#endif
