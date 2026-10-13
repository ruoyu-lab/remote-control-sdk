// "rcsdk.text-caret": the session's halves of both ends, the sharer's
// machine's half that looks, and the public functions.
//
// Hellos (one byte): a sharer's says whether it tells, a viewer's whether it
// asks. The sharer's half sends a viewer the caret, 17 bytes (visible, x, y,
// width, height: u8 and four u32, little-endian), while it tells, the viewer
// asks and holds control: at once when that begins, then at each change, and
// a caret gone (visible 0) when it ends while one showed.
//
// With its machine's half: the session's sends how often to look (u32
// milliseconds, 0: not now) when that changes and when the half joins; the
// machine's answers each with whether it can look (u8 0, then u8 can), and
// sends the caret at a watch's first look and at each change (u8 1, then the
// 17 bytes).
#include "reader.hpp"

#include <rcsdk/text_caret.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rcsdk::text_caret {
namespace {

constexpr uint32_t moduleVersion = 1;
constexpr uint32_t maxMessageBytes = 32;
constexpr uint8_t statusMessage = 0, caretMessage = 1;

using Bytes = std::vector<uint8_t>;

void putU32(Bytes& bytes, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes.push_back(uint8_t(value >> (8 * i)));
    }
}

uint32_t readU32(std::span<const uint8_t> data, size_t at) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= uint32_t(data[at + size_t(i)]) << (8 * i);
    }
    return value;
}

Bytes encode(const rcs_text_caret& c, Bytes bytes = {}) {
    bytes.push_back(uint8_t(c.visible));
    putU32(bytes, c.x);
    putU32(bytes, c.y);
    putU32(bytes, c.width);
    putU32(bytes, c.height);
    return bytes;
}

// A caret of 17 bytes at the start of `data`, if it is one a report may hold.
std::optional<rcs_text_caret> decodeCaret(std::span<const uint8_t> data) {
    if (data.size() != 17) {
        return std::nullopt;
    }
    const rcs_text_caret c{data[0], readU32(data, 1), readU32(data, 5), readU32(data, 9), readU32(data, 13)};
    return valid(c) ? std::optional(c) : std::nullopt;
}

// The settings the session's chain carries; check() took them.
rcs_text_caret_settings settingsOf(const void* settings) {
    return settings ? *static_cast<const rcs_text_caret_settings*>(settings)
                    : rcs_text_caret_settings RCS_TEXT_CARET_SETTINGS_INIT;
}

std::string checked(const void* settings, uint32_t size) {
    if (!settings && !size) {
        return {};
    }
    if (!settings || size != sizeof(rcs_text_caret_settings) ||
        static_cast<const rcs_text_caret_settings*>(settings)->struct_size != sizeof(rcs_text_caret_settings)) {
        return "rcs_text_caret_settings: initialise it with RCS_TEXT_CARET_SETTINGS_INIT from this library's "
               "headers, and pass its size (or NULL and 0 for the defaults)";
    }
    const auto c = settingsOf(settings);
    if (c.ask > 1) {
        return "rcs_text_caret_settings: ask must be 0 or 1";
    }
    if (c.interval_ms && (c.interval_ms < 10 || c.interval_ms > 10'000)) {
        return "rcs_text_caret_settings: interval_ms must be 0 or 10..10 000";
    }
    return {};
}

// ---- The session's halves -------------------------------------------------------------------

// Both ends' session's half; calls into it never overlap (the module's calls).
class Half {
public:
    explicit Half(rcs_module module) : module_(module) {}

    void start(rcs_role role, const rcs_text_caret_settings& settings) {
        role_ = role;
        settings_ = settings;
        peers_.clear();
        machineUp_ = machineCan_ = false;
        tells_ = false;
        told_ = 0;
        latest_ = {};
        helloAgain();
    }
    void update(const rcs_text_caret_settings& settings) {
        settings_ = settings;
        helloAgain();
        review();
    }
    void stop() {
        for (auto& [id, peer] : peers_) {
            peer.control = false;
        }
        review();
        peers_.clear();
    }

    void peerJoined(rcs_viewer_id id, std::span<const uint8_t> hello) {
        auto& peer = peers_[id];
        peer = {};
        peer.says = hello.size() == 1 && hello[0] == 1;
        if (role_ == RCS_ROLE_VIEW && peer.says) {
            emitTells(true);
        }
        review();
    }
    void peerHello(rcs_viewer_id id, std::span<const uint8_t> hello) {
        const auto found = peers_.find(id);
        if (found == peers_.end()) {
            return;
        }
        const bool says = hello.size() == 1 && hello[0] == 1;
        if (says != found->second.says) {
            found->second.says = says;
            if (role_ == RCS_ROLE_VIEW) {
                emitTells(says);
            }
        }
        review();
    }
    void peerControl(rcs_viewer_id id, bool granted) {
        if (const auto found = peers_.find(id); found != peers_.end()) {
            found->second.control = granted;
            review();
        }
    }
    void peerLeft(rcs_viewer_id id) {
        const auto found = peers_.find(id);
        if (found == peers_.end()) {
            return;
        }
        found->second.control = false;
        if (std::exchange(found->second.says, false) && role_ == RCS_ROLE_VIEW) {
            emitTells(false);
        }
        review(); // A viewer reports a caret that showed gone.
        peers_.erase(id);
        review();
    }
    void received(rcs_viewer_id id, std::span<const uint8_t> data) {
        const auto found = peers_.find(id);
        const auto caret = decodeCaret(data);
        if (role_ != RCS_ROLE_VIEW || found == peers_.end() || !caret) {
            rcs_module_log(module_, RCS_LOG_WARNING, "A message that is no caret, or not from a sharer, was dropped");
            return;
        }
        found->second.received = *caret;
        review();
    }

    // It answers with whether it can look; then it is told how often.
    void machineJoined() {
        machineUp_ = true;
        tellMachine(0);
    }
    void machineLeft() {
        machineUp_ = machineCan_ = false;
        latest_ = {};
        helloAgain();
        review();
    }
    void fromMachine(std::span<const uint8_t> data) {
        if (data.size() == 2 && data[0] == statusMessage && data[1] <= 1) {
            machineCan_ = data[1] == 1;
            helloAgain();
            review();
        } else if (const auto caret = data.empty() || data[0] != caretMessage ? std::nullopt
                                                                             : decodeCaret(data.subspan(1))) {
            latest_ = *caret;
            review();
        } else {
            rcs_module_log(module_, RCS_LOG_WARNING, "The machine's half sent a message of no kind it has");
        }
    }

    rcs_result peer(rcs_viewer_id id, rcs_text_caret_peer& out) const {
        const auto found = peers_.find(id);
        if (found == peers_.end()) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "The text caret is not active with that end");
        }
        const auto& p = found->second;
        out.tells = role_ == RCS_ROLE_VIEW && p.says;
        out.asks = role_ == RCS_ROLE_SHARE && p.says;
        out.caret = p.shown;
        return rcs_set_last_call_error(RCS_OK, nullptr);
    }

private:
    // An end the module is active with. `says`: a viewer asks, a sharer tells.
    struct Peer {
        bool says = false;
        bool control = false;   // A sharer's viewer holds control; a viewer's own (peer 0).
        rcs_text_caret received{}; // View: as the sharer said it last.
        rcs_text_caret shown{};    // View: as reported to the host; share: as sent to the viewer.
    };

    void helloAgain() {
        const bool says = role_ == RCS_ROLE_VIEW ? settings_.ask != 0
                                                 : settings_.interval_ms && machineUp_ && machineCan_;
        if (role_ == RCS_ROLE_SHARE) {
            tells_ = says;
        }
        const uint8_t hello = says ? 1 : 0;
        if (!helloSet_ || hello != hello_) {
            helloSet_ = true;
            hello_ = hello;
            rcs_module_set_hello(module_, &hello, 1);
        }
    }

    void emitTells(bool tells) {
        const uint32_t value = tells ? 1 : 0;
        rcs_module_emit(module_, 0, RCS_TEXT_CARET_TELLS, &value, sizeof(value));
    }

    // What each end is told or shown now, and how often the machine looks.
    void review() {
        if (role_ == RCS_ROLE_VIEW) {
            for (auto& [id, p] : peers_) {
                const rcs_text_caret now = settings_.ask && p.control && p.says ? p.received : rcs_text_caret{};
                if (!(now == p.shown)) {
                    p.shown = now;
                    rcs_module_emit(module_, id, RCS_TEXT_CARET_CHANGED, &now, sizeof(now));
                }
            }
            return;
        }
        bool any = false;
        for (auto& [id, p] : peers_) {
            const bool watched = tells_ && p.says && p.control;
            any |= watched;
            const rcs_text_caret now = watched ? latest_ : rcs_text_caret{};
            // A viewer that begins to watch hears the caret once the machine said one.
            if (!(now == p.shown) && (watched || p.shown.visible)) {
                const Bytes bytes = encode(now);
                // The newest takes the place of one still waiting: the queue never refuses it.
                if (rcs_module_send(module_, id, bytes.data(), uint32_t(bytes.size()), RCS_MODULE_REPLACE, 0) ==
                    RCS_OK) {
                    p.shown = now;
                }
            }
        }
        const uint32_t watching = any ? settings_.interval_ms : 0;
        if (machineUp_ && watching != told_) {
            tellMachine(watching);
        }
        if (!watching) {
            latest_ = {}; // The machine looks anew when a watch begins again.
        }
    }

    void tellMachine(uint32_t interval) {
        told_ = interval;
        Bytes bytes;
        putU32(bytes, interval);
        rcs_module_send_machine(module_, bytes.data(), uint32_t(bytes.size()));
    }

    const rcs_module module_;
    rcs_role role_ = RCS_ROLE_SHARE;
    rcs_text_caret_settings settings_ = RCS_TEXT_CARET_SETTINGS_INIT;
    std::map<rcs_viewer_id, Peer> peers_;
    bool helloSet_ = false;
    uint8_t hello_ = 0;
    // Share:
    bool machineUp_ = false, machineCan_ = false;
    bool tells_ = false;
    uint32_t told_ = 0;       // How often the machine was told to look (0: not now).
    rcs_text_caret latest_{}; // As the machine said it last, while it looks.
};

// The session's half's state, made at its first use, kept for the session's life.
Half& half(rcs_module module) {
    if (auto* made = static_cast<Half*>(rcs_module_state(module))) {
        return *made;
    }
    auto* made = new Half(module);
    rcs_module_set_state(module, made, [](void* state) { delete static_cast<Half*>(state); });
    return *made;
}

// ---- The machine's half ---------------------------------------------------------------------

// A sharer's machine's half: looks every interval while a watch runs, on a
// thread of its own (the reader lives there), and reports a watch's first look
// and each change. A look may wait for an application that hangs, bounded by
// the reader's timeouts.
class Watcher {
public:
    explicit Watcher(rcs_module module) : module_(module), thread_([this] { run(); }) {}
    ~Watcher() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    void fromSession(std::span<const uint8_t> data) {
        if (data.size() != 4) {
            rcs_module_log(module_, RCS_LOG_WARNING, "The session's half sent a message of no kind it has");
            return;
        }
        const uint32_t interval = readU32(data, 0);
        {
            std::lock_guard lock(mutex_);
            interval_ = interval && interval < 10 ? 10 : std::min<uint32_t>(interval, 10'000);
            ++watch_;
        }
        wake_.notify_all();
        const uint8_t status[] = {statusMessage, uint8_t(available() ? 1 : 0)};
        rcs_module_send_machine(module_, status, sizeof(status));
    }

private:
    void run() {
        std::unique_ptr<Reader> reader;
        bool readerTried = false;
        uint64_t reported = 0; // The watch whose first look was reported.
        rcs_text_caret last{};
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return quit_ || interval_ != 0; });
            if (quit_) {
                return;
            }
            const auto interval = std::chrono::milliseconds(interval_);
            const uint64_t watch = watch_;
            lock.unlock();
            if (!readerTried) {
                readerTried = true;
                reader = createReader(module_);
            }
            rcs_text_caret now{};
            rcs_module_shared shared = RCS_MODULE_SHARED_INIT;
            if (reader && rcs_module_get_shared(module_, &shared) == RCS_OK) {
                try {
                    now = reader->read(shared);
                } catch (const std::exception& error) { // No caret this time; the next look tries again.
                    rcs_module_log(module_, RCS_LOG_WARNING,
                                   (std::string("Looking for the text caret failed: ") + error.what()).c_str());
                }
            }
            if (reported != watch || !(now == last)) {
                reported = watch;
                last = now;
                const Bytes bytes = encode(now, {caretMessage});
                rcs_module_send_machine(module_, bytes.data(), uint32_t(bytes.size()));
            }
            lock.lock();
            wake_.wait_for(lock, interval, [&] { return quit_ || watch_ != watch; });
        }
    }

    const rcs_module module_;
    std::mutex mutex_;
    std::condition_variable wake_;
    uint32_t interval_ = 0; // 0: not now.
    uint64_t watch_ = 0;    // Moves on with each message of the session's.
    bool quit_ = false;
    std::thread thread_; // Last: it starts once the rest exists.
};

// What a call does, its failures logged: they never reach the SDK.
template <class Call>
void guarded(rcs_module module, Call&& call) noexcept {
    try {
        call();
    } catch (const std::exception& error) {
        rcs_module_log(module, RCS_LOG_ERROR, error.what());
    }
}

std::span<const uint8_t> bytes(const uint8_t* data, uint32_t size) {
    return {data, size};
}

rcs_module_definition makeDefinition() {
    rcs_module_definition d{};
    d.struct_size = sizeof(d);
    d.id = RCS_TEXT_CARET_MODULE;
    d.version = moduleVersion;
    d.flags = RCS_MODULE_PEER | RCS_MODULE_MACHINE;
    d.roles = (1u << RCS_ROLE_SHARE) | (1u << RCS_ROLE_VIEW);
    d.max_message_bytes = maxMessageBytes;
    d.check = [](rcs_role, const void* settings, uint32_t size, char* reason, uint32_t capacity,
                 void*) -> rcs_result {
        const std::string why = checked(settings, size);
        if (why.empty()) {
            return RCS_OK;
        }
        if (capacity) {
            std::snprintf(reason, capacity, "%s", why.c_str());
        }
        return RCS_ERROR_INVALID_ARGUMENT;
    };
    d.start = [](rcs_module module, rcs_role role, const void* settings, uint32_t, void*) {
        guarded(module, [&] { half(module).start(role, settingsOf(settings)); });
    };
    d.update = [](rcs_module module, const void* settings, uint32_t, void*) {
        guarded(module, [&] { half(module).update(settingsOf(settings)); });
    };
    d.peer_joined = [](rcs_module module, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void*) {
        guarded(module, [&] { half(module).peerJoined(peer, bytes(hello, size)); });
    };
    d.peer_hello = [](rcs_module module, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void*) {
        guarded(module, [&] { half(module).peerHello(peer, bytes(hello, size)); });
    };
    d.peer_control = [](rcs_module module, rcs_viewer_id peer, uint32_t granted, void*) {
        guarded(module, [&] { half(module).peerControl(peer, granted != 0); });
    };
    d.peer_left = [](rcs_module module, rcs_viewer_id peer, void*) {
        guarded(module, [&] { half(module).peerLeft(peer); });
    };
    d.received = [](rcs_module module, rcs_viewer_id peer, const uint8_t* data, uint32_t size,
                    rcs_module_message_flags, void*) {
        guarded(module, [&] { half(module).received(peer, bytes(data, size)); });
    };
    d.machine_joined = [](rcs_module module, void*) { guarded(module, [&] { half(module).machineJoined(); }); };
    d.machine_left = [](rcs_module module, void*) { guarded(module, [&] { half(module).machineLeft(); }); };
    d.from_machine = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] { half(module).fromMachine(bytes(data, size)); });
    };
    d.stop = [](rcs_module module, void*) { guarded(module, [&] { half(module).stop(); }); };
    d.machine_start = [](rcs_module module, rcs_role role, void*) {
        if (role == RCS_ROLE_SHARE) { // A viewer's machine has nothing to look at.
            guarded(module, [&] {
                rcs_module_set_state(module, new Watcher(module),
                                     [](void* state) { delete static_cast<Watcher*>(state); });
            });
        }
    };
    d.from_session = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] {
            if (auto* watcher = static_cast<Watcher*>(rcs_module_state(module))) {
                watcher->fromSession(bytes(data, size));
            }
        });
    };
    d.machine_stop = [](rcs_module module, void*) { rcs_module_set_state(module, nullptr, nullptr); };
    return d;
}

} // namespace
} // namespace rcsdk::text_caret

using namespace rcsdk::text_caret;

extern "C" {

const rcs_module_definition* rcs_text_caret_module(void) {
    static const rcs_module_definition definition = makeDefinition();
    return &definition;
}

uint32_t rcs_text_caret_available(void) {
    return available() ? 1 : 0;
}

rcs_result rcs_text_caret_get_peer(rcs_session session, rcs_viewer_id viewer, rcs_text_caret_peer* peer) {
    if (!session) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    if (!peer || peer->struct_size != sizeof(*peer)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "Initialise rcs_text_caret_peer with RCS_TEXT_CARET_PEER_INIT from this "
                                       "library's headers");
    }
    rcs_module module = nullptr;
    if (const rcs_result found = rcs_module_find(session, RCS_TEXT_CARET_MODULE, &module); found != RCS_OK) {
        return found;
    }
    struct Ask {
        rcs_viewer_id viewer;
        rcs_text_caret_peer* peer;
    } ask{viewer, peer};
    return rcs_module_run(
        module,
        [](rcs_module m, void* argument) -> rcs_result {
            const auto& a = *static_cast<Ask*>(argument);
            return half(m).peer(a.viewer, *a.peer);
        },
        &ask);
}

} // extern "C"
