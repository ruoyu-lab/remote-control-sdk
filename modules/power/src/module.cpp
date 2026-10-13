// "rcsdk.power": the session's half, which decides what stays awake, tells a
// sharer's viewers what it offers and takes their requests (a viewer's: asks,
// and hears the answers); the machine's half, which holds the power request,
// lists the interfaces and carries out actions; and the public functions.
// The messages are messages.hpp's.
#include "messages.hpp"
#include "power_request.hpp"
#include "system.hpp"

#include <rcsdk/power.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace rcsdk::power {
namespace {

constexpr char reason[] = "Remote session in progress";

rcs_power_settings settingsOf(const void* settings) {
    return settings ? *static_cast<const rcs_power_settings*>(settings) : rcs_power_settings RCS_POWER_SETTINGS_INIT;
}

std::string checked(rcs_role role, const void* settings, uint32_t size) {
    if (!settings && !size) {
        return {};
    }
    if (!settings || size != sizeof(rcs_power_settings) ||
        static_cast<const rcs_power_settings*>(settings)->struct_size != sizeof(rcs_power_settings)) {
        return "rcs_power_settings: initialise it with RCS_POWER_SETTINGS_INIT from this library's headers, and "
               "pass its size (or NULL and 0 for the defaults)";
    }
    const auto c = settingsOf(settings);
    if (c.keep_awake > RCS_KEEP_AWAKE_DISPLAY) {
        return "rcs_power_settings: unknown keep_awake";
    }
    if (c.while_running > 1 || c.share_wake_info > 1) {
        return "rcs_power_settings: while_running and share_wake_info are 0 or 1";
    }
    if (c.actions & ~uint32_t(RCS_POWER_ALL_ACTIONS)) {
        return "rcs_power_settings: actions holds bits of no rcs_power_action";
    }
    if (c.action_delay_ms < minActionDelayMs || c.action_delay_ms > maxActionDelayMs) {
        return "rcs_power_settings: action_delay_ms is 1 000..60 000";
    }
    if (role == RCS_ROLE_VIEW && (c.share_wake_info || c.actions)) {
        return "rcs_power_settings: share_wake_info and actions are a sharer's; a viewer's are 0";
    }
    return {};
}

bool operator==(const rcs_power_status& a, const rcs_power_status& b) {
    return a.wanted == b.wanted && a.held == b.held && a.result == b.result &&
           std::strcmp(a.message, b.message) == 0 && a.actions == b.actions;
}

rcs_power_status statusOf(rcs_keep_awake wanted, rcs_keep_awake held, rcs_result result, const std::string& why) {
    rcs_power_status s = RCS_POWER_STATUS_INIT;
    s.wanted = wanted;
    s.held = held;
    s.result = result;
    std::snprintf(s.message, sizeof(s.message), "%s", why.c_str());
    return s;
}

std::string describe(rcs_power_action action, uint32_t flags) {
    return std::string(actionName(action)) + (flags & RCS_POWER_FORCE ? " (force)" : "");
}

// Items of a list, two passes (rcs_list_displays).
rcs_result listed(const std::vector<rcs_power_interface>& list, rcs_power_interface* items, uint32_t capacity,
                  uint32_t& count) {
    count = uint32_t(list.size());
    for (size_t i = 0; items && i < list.size() && i < capacity; ++i) {
        items[i] = list[i];
    }
    if (items && capacity < list.size()) {
        return rcs_set_last_call_error(RCS_ERROR_BUFFER_TOO_SMALL, "More interfaces than capacity");
    }
    return rcs_set_last_call_error(RCS_OK, nullptr);
}

// ---- The session's half -------------------------------------------------------------------

// A session's half (a sharer's, or a viewer's: its connection to the sharer
// is peer 0); calls into it never overlap (the module's calls).
class Half {
public:
    explicit Half(rcs_module module) : module_(module) {}

    void start(rcs_role role, const rcs_power_settings& settings) {
        role_ = role;
        settings_ = settings;
        connected_.clear();
        active_.clear();
        controlling_.clear();
        machineUp_ = false;
        told_.reset();
        held_ = statusOf(RCS_KEEP_AWAKE_OFF, RCS_KEEP_AWAKE_OFF, RCS_OK, {});
        can_ = 0;
        canWhy_.clear();
        interfaces_.clear();
        pending_.clear();
        hello_.reset();
        sharer_ = Hello{};
        asked_.clear();
        offer();
        review();
    }
    void update(const rcs_power_settings& settings) {
        const bool list = settings.share_wake_info && !settings_.share_wake_info;
        settings_ = settings;
        if (list) {
            askForInterfaces();
        }
        offer();
        review();
    }
    void stop() {
        connected_.clear();
        settings_.while_running = 0;
        review(); // The machine's half lets go, and stops after.
        pending_.clear();
        asked_.clear();
    }

    void connected(rcs_viewer_id viewer) {
        connected_.insert(viewer);
        review();
        askForInterfaces(); // Addresses change: a viewer that connects hears them as they are.
    }
    void disconnected(rcs_viewer_id viewer) {
        connected_.erase(viewer);
        review();
    }

    void peerJoined(rcs_viewer_id peer, std::span<const uint8_t> hello) {
        active_.insert(peer);
        if (role_ == RCS_ROLE_VIEW) {
            heard(hello);
        }
    }
    void peerHello(rcs_viewer_id, std::span<const uint8_t> hello) {
        if (role_ == RCS_ROLE_VIEW) {
            heard(hello);
        }
    }
    void peerControl(rcs_viewer_id peer, bool granted) {
        if (granted) {
            controlling_.insert(peer);
        } else {
            controlling_.erase(peer);
        }
    }
    void peerLeft(rcs_viewer_id peer) {
        active_.erase(peer);
        controlling_.erase(peer);
        if (role_ != RCS_ROLE_VIEW) {
            return;
        }
        // What the sharer told of its interfaces stays: its host may wake it later.
        sharer_.actions = 0;
        reportPeer();
        for (const auto& [request, asked] : asked_) {
            if (!asked.answered) {
                rcs_power_action_event e = eventOf(request, asked.action, asked.flags, RCS_POWER_ACTION_FAILED,
                                                   RCS_ERROR_DISCONNECTED, 0,
                                                   "The connection to the sharer ended before it answered");
                rcs_module_emit(module_, 0, RCS_POWER_ACTION, &e, sizeof(e));
            }
        }
        asked_.clear();
    }

    void received(rcs_viewer_id peer, std::span<const uint8_t> data) {
        PeerMessage message;
        try {
            message = decodePeer(data);
        } catch (const Refused& refused) {
            rcs_module_log(module_, RCS_LOG_WARNING, refused.what());
            return;
        }
        if (const auto* ask = std::get_if<Ask>(&message); ask && role_ == RCS_ROLE_SHARE) {
            decide(peer, *ask);
        } else if (const auto* answer = std::get_if<Answer>(&message); answer && role_ == RCS_ROLE_VIEW) {
            answered(*answer);
        } else {
            rcs_module_log(module_, RCS_LOG_WARNING, "The other end sent a power message its role does not send");
        }
    }

    void machineJoined() {
        machineUp_ = true;
        told_.reset();
        review();
        askForInterfaces();
    }
    void machineLeft() {
        machineUp_ = false;
        held_ = statusOf(held_.wanted, RCS_KEEP_AWAKE_OFF, RCS_OK, {});
        can_ = 0;
        offer();
        review();
        // Its answers do not come any more; an accepted action is carried out all the same.
        for (auto it = pending_.begin(); it != pending_.end(); it = pending_.erase(it)) {
            if (!it->second.accepted) {
                tell(it->second, RCS_POWER_ACTION_FAILED, RCS_ERROR_INVALID_STATE, 0,
                     "The machine's half left before it answered");
            }
        }
    }
    void fromMachine(std::span<const uint8_t> data) {
        ToSession message;
        try {
            message = decodeToSession(data);
        } catch (const Refused& refused) {
            rcs_module_log(module_, RCS_LOG_WARNING, refused.what());
            return;
        }
        if (const auto* held = std::get_if<Held>(&message)) {
            held_ = statusOf(told_.value_or(RCS_KEEP_AWAKE_OFF), held->held, held->result, held->message);
            if (held->result != RCS_OK) { // The system's refusal; a half that is not there is the SDK's to report.
                rcs_module_log(module_, RCS_LOG_WARNING,
                               (std::string("The machine is not kept awake: ") + held_.message).c_str());
            }
            report();
        } else if (const auto* can = std::get_if<Can>(&message)) {
            can_ = can->actions;
            canWhy_ = can->message;
            offer();
            report();
        } else if (const auto* acted = std::get_if<Acted>(&message)) {
            const auto found = pending_.find(acted->serial);
            if (found == pending_.end()) {
                return; // Of a run before, or of an agent before.
            }
            found->second.accepted = true;
            tell(found->second, acted->state, acted->result, acted->delayMs, acted->message);
            if (acted->state != RCS_POWER_ACTION_ACCEPTED) {
                pending_.erase(found);
            }
        } else if (const auto* list = std::get_if<Listed>(&message)) {
            if (list->result != RCS_OK) {
                rcs_module_log(module_, RCS_LOG_WARNING,
                               ("The machine's interfaces could not be listed: " + list->message).c_str());
            }
            interfaces_ = list->interfaces;
            offer();
        }
    }

    // ---- The public functions ----

    rcs_result status(rcs_power_status& out) const {
        out = reported_;
        return rcs_set_last_call_error(RCS_OK, nullptr);
    }

    rcs_result ask(rcs_power_action action, uint32_t flags, uint64_t* out) {
        if (role_ == RCS_ROLE_SHARE) {
            return rcs_set_last_call_error(RCS_ERROR_UNSUPPORTED,
                                           "A sharer's host acts on its own machine: viewers ask theirs");
        }
        if (!active_.count(0)) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                           "Not connected to a sharer that uses the power module");
        }
        const uint64_t request = ++requests_;
        const Bytes bytes = encode(PeerMessage{Ask{request, action, flags}});
        if (const rcs_result sent = rcs_module_send(module_, 0, bytes.data(), uint32_t(bytes.size()), 0, 0);
            sent != RCS_OK) {
            return sent; // Its reason stands.
        }
        asked_[request] = {action, flags, false};
        if (out) {
            *out = request;
        }
        return rcs_set_last_call_error(RCS_OK, nullptr);
    }

    rcs_result offered(rcs_power_peer& out) const {
        if (const rcs_result viewing = mustView(); viewing != RCS_OK) {
            return viewing;
        }
        out.actions = sharer_.actions;
        out.wake_info = sharer_.wakeInfo ? 1 : 0;
        out.interfaces = uint32_t(sharer_.interfaces.size());
        return rcs_set_last_call_error(RCS_OK, nullptr);
    }

    rcs_result peerInterfaces(rcs_power_interface* items, uint32_t capacity, uint32_t& count) const {
        if (const rcs_result viewing = mustView(); viewing != RCS_OK) {
            return viewing;
        }
        return listed(sharer_.interfaces, items, capacity, count);
    }

private:
    // A request at the sharer: who asked what, and whether the machine accepted it.
    struct Pending {
        rcs_viewer_id viewer = 0;
        Ask ask;
        bool accepted = false;
    };
    // A request at the viewer.
    struct Asked {
        rcs_power_action action = 0;
        uint32_t flags = 0;
        bool answered = false;
    };

    static rcs_power_action_event eventOf(uint64_t request, rcs_power_action action, uint32_t flags,
                                          rcs_power_action_state state, rcs_result result, uint32_t delayMs,
                                          const std::string& why) {
        rcs_power_action_event e{};
        e.request = request;
        e.action = action;
        e.flags = flags;
        e.state = state;
        e.result = result;
        e.delay_ms = delayMs;
        std::snprintf(e.message, sizeof(e.message), "%s", why.c_str());
        return e;
    }

    rcs_result mustView() const {
        return role_ == RCS_ROLE_SHARE
                   ? rcs_set_last_call_error(RCS_ERROR_UNSUPPORTED, "What a sharer offers is a viewer's to ask")
                   : RCS_OK;
    }

    rcs_keep_awake wanted() const noexcept {
        return settings_.while_running || !connected_.empty() ? settings_.keep_awake
                                                              : rcs_keep_awake(RCS_KEEP_AWAKE_OFF);
    }

    // Tells the machine's half what to hold, and the host what is held.
    void review() {
        const rcs_keep_awake now = wanted();
        if (machineUp_ && told_ != now) {
            told_ = now;
            toMachine(Hold{now});
            return; // Its answer reports it.
        }
        if (!machineUp_) {
            held_ = now == RCS_KEEP_AWAKE_OFF
                        ? statusOf(now, RCS_KEEP_AWAKE_OFF, RCS_OK, {})
                        : statusOf(now, RCS_KEEP_AWAKE_OFF, RCS_ERROR_INVALID_STATE,
                                   "The machine's half does not run: no agent runs the module now");
        }
        report();
    }

    void report() {
        rcs_power_status now = held_;
        now.wanted = wanted();
        now.actions = can_;
        if (!(now == reported_)) {
            reported_ = now;
            rcs_module_emit(module_, 0, RCS_POWER_CHANGED, &now, sizeof(now));
        }
    }

    void toMachine(const ToMachine& message) {
        const Bytes bytes = encode(message);
        rcs_module_send_machine(module_, bytes.data(), uint32_t(bytes.size()));
    }

    void askForInterfaces() {
        if (role_ == RCS_ROLE_SHARE && settings_.share_wake_info && machineUp_) {
            toMachine(List{});
        }
    }

    // A sharer's hello: what it offers its viewers now.
    void offer() {
        if (role_ != RCS_ROLE_SHARE) {
            return;
        }
        Hello hello;
        hello.actions = settings_.actions & can_;
        hello.wakeInfo = settings_.share_wake_info != 0;
        if (hello.wakeInfo) {
            hello.interfaces = interfaces_;
        }
        const Bytes bytes = encode(hello);
        if (hello_ != bytes) {
            hello_ = bytes;
            rcs_module_set_hello(module_, bytes.data(), uint32_t(bytes.size()));
        }
    }

    // A viewer heard what its sharer offers.
    void heard(std::span<const uint8_t> hello) {
        try {
            sharer_ = decodeHello(hello);
        } catch (const Refused& refused) {
            rcs_module_log(module_, RCS_LOG_WARNING, refused.what());
            sharer_ = Hello{};
        }
        reportPeer();
    }

    void reportPeer() {
        rcs_power_peer p = RCS_POWER_PEER_INIT;
        offered(p);
        rcs_module_emit(module_, 0, RCS_POWER_PEER, &p, sizeof(p));
    }

    // A sharer decides on a viewer's request: its host's settings, its
    // machine, its host's callback; what is left goes to the machine.
    void decide(rcs_viewer_id viewer, const Ask& ask) {
        Pending pending{viewer, ask, false};
        const uint32_t bit = 1u << ask.action;
        const std::string name = actionName(ask.action);
        if (!(settings_.actions & bit)) {
            return tell(pending, RCS_POWER_ACTION_REFUSED, RCS_ERROR_REFUSED, 0,
                        "The sharer's host does not let viewers ask to " + name);
        }
        if (!machineUp_) {
            return tell(pending, RCS_POWER_ACTION_REFUSED, RCS_ERROR_INVALID_STATE,
                        0, "The machine's half does not run: no agent runs the module now");
        }
        if (!(can_ & bit)) {
            return tell(pending, RCS_POWER_ACTION_REFUSED, RCS_ERROR_UNSUPPORTED, 0,
                        canWhy_.empty() ? "The machine cannot " + name : canWhy_);
        }
        rcs_power_request request{};
        request.struct_size = sizeof(request);
        request.viewer_id = viewer;
        request.request = ask.request;
        request.action = ask.action;
        request.flags = ask.flags;
        request.controlling = controlling_.count(viewer) ? 1 : 0;
        if (settings_.on_action ? !settings_.on_action(&request, settings_.user_data) : !request.controlling) {
            return tell(pending, RCS_POWER_ACTION_REFUSED, RCS_ERROR_REFUSED, 0,
                        settings_.on_action ? "The sharer's host refused it"
                                            : "Viewer " + std::to_string(viewer) + " does not hold remote control");
        }
        const uint64_t serial = ++serials_;
        pending_[serial] = pending;
        toMachine(Act{serial, ask.action, ask.flags, settings_.action_delay_ms});
    }

    // A step of a request, to the sharer's host and to the viewer that asked.
    void tell(const Pending& p, rcs_power_action_state state, rcs_result result, uint32_t delayMs,
              const std::string& why) {
        const auto e = eventOf(p.ask.request, p.ask.action, p.ask.flags, state, result, delayMs, why);
        static const char* const steps[] = {"", "refused", "accepted", "started", "failed"};
        std::string line = "Viewer " + std::to_string(p.viewer) + " asked to " + describe(p.ask.action, p.ask.flags) +
                           ": " + steps[state];
        if (state == RCS_POWER_ACTION_ACCEPTED) {
            line += ", carried out in " + std::to_string(delayMs) + " ms";
        } else if (!why.empty()) {
            line += " (" + why + ")";
        }
        rcs_module_log(module_, state == RCS_POWER_ACTION_FAILED ? RCS_LOG_WARNING : RCS_LOG_INFO, line.c_str());
        rcs_module_emit(module_, p.viewer, RCS_POWER_ACTION, &e, sizeof(e));
        if (active_.count(p.viewer)) {
            const Bytes bytes = encode(PeerMessage{Answer{p.ask.request, p.ask.action, p.ask.flags, state, result,
                                                          delayMs, why}});
            rcs_module_send(module_, p.viewer, bytes.data(), uint32_t(bytes.size()), 0, 0);
        }
    }

    // A viewer hears a step of its request.
    void answered(const Answer& answer) {
        const auto found = asked_.find(answer.request);
        if (found == asked_.end() || found->second.action != answer.action) {
            rcs_module_log(module_, RCS_LOG_WARNING, "The sharer answered a power request this end did not make");
            return;
        }
        const auto e = eventOf(answer.request, answer.action, answer.flags, answer.state, answer.result,
                               answer.delayMs, answer.message);
        rcs_module_emit(module_, 0, RCS_POWER_ACTION, &e, sizeof(e));
        if (answer.state == RCS_POWER_ACTION_ACCEPTED) {
            found->second.answered = true;
        } else {
            asked_.erase(found);
        }
    }

    const rcs_module module_;
    rcs_role role_ = 0; // As the run started.
    rcs_power_settings settings_ = RCS_POWER_SETTINGS_INIT;
    std::set<rcs_viewer_id> connected_, active_, controlling_;
    bool machineUp_ = false;
    std::optional<rcs_keep_awake> told_; // What the machine's half was told to hold.
    rcs_power_status held_ = RCS_POWER_STATUS_INIT;     // As the machine's half answered.
    rcs_power_status reported_ = RCS_POWER_STATUS_INIT; // As the host heard it.
    uint32_t can_ = 0;                                  // The actions the machine's half can carry out...
    std::string canWhy_;                                // ...and why not the others.
    // Share: the machine's interfaces, the hello told, the requests sent to the machine.
    std::vector<rcs_power_interface> interfaces_;
    std::optional<Bytes> hello_;
    std::map<uint64_t, Pending> pending_;
    uint64_t serials_ = 0;
    // View: what the sharer offers, and this end's requests.
    Hello sharer_;
    std::map<uint64_t, Asked> asked_;
    uint64_t requests_ = 0;
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

// ---- The machine's half -------------------------------------------------------------------

// The machine's half while it runs, for an action's thread to report to.
struct Link {
    std::mutex mutex;
    rcs_module module = nullptr;
};

// An action accepted in this process waits or is carried out: one at a time.
std::mutex actingMutex;
bool acting = false;

// Carries out `act` after its delay on a thread of its own, which outlives
// the machine's half that accepted it (its session may stop meanwhile: the
// machine restarts all the same); that half hears how it went while it runs.
void carryOut(std::shared_ptr<Machine> machine, std::shared_ptr<Link> link, Act act) {
    std::thread([machine = std::move(machine), link = std::move(link), act] {
        std::this_thread::sleep_for(std::chrono::milliseconds(act.delayMs));
        {
            std::lock_guard lock(link->mutex);
            if (link->module) {
                rcs_module_log(link->module, RCS_LOG_INFO,
                               ("Carrying out: " + describe(act.action, act.flags)).c_str());
            }
        }
        Acted acted{act.serial, RCS_POWER_ACTION_STARTED, RCS_OK, 0, {}};
        try {
            machine->act(act.action, act.flags);
        } catch (const Refused& refused) {
            acted.state = RCS_POWER_ACTION_FAILED;
            acted.result = refused.result() == RCS_OK ? RCS_ERROR_INTERNAL : refused.result();
            acted.message = refused.what();
        } catch (const std::exception& error) {
            acted.state = RCS_POWER_ACTION_FAILED;
            acted.result = RCS_ERROR_INTERNAL;
            acted.message = error.what();
        }
        {
            std::lock_guard lock(actingMutex);
            acting = false;
        }
        std::lock_guard lock(link->mutex);
        if (link->module) {
            const Bytes bytes = encode(ToSession{acted});
            rcs_module_send_machine(link->module, bytes.data(), uint32_t(bytes.size()));
        }
    }).detach();
}

// Holds a request for what the session's half asks, lists the interfaces on
// a thread of its own (the system may take a moment), and accepts actions.
class MachineHalf {
public:
    explicit MachineHalf(rcs_module module)
        : module_(module), link_(std::make_shared<Link>()), thread_([this] { run(); }) {
        link_->module = module;
        const Abilities abilities = machine()->abilities();
        send(Can{abilities.actions, abilities.why});
    }
    ~MachineHalf() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        thread_.join();
        std::lock_guard lock(link_->mutex);
        link_->module = nullptr;
    }

    void fromSession(std::span<const uint8_t> data) {
        ToMachine message;
        try {
            message = decodeToMachine(data);
        } catch (const Refused& refused) {
            rcs_module_log(module_, RCS_LOG_WARNING, refused.what());
            return;
        }
        if (const auto* hold = std::get_if<Hold>(&message)) {
            keepAwake(hold->mode);
        } else if (const auto* act = std::get_if<Act>(&message)) {
            accept(*act);
        } else {
            {
                std::lock_guard lock(mutex_);
                list_ = true;
            }
            wake_.notify_all();
        }
    }

private:
    void send(const ToSession& message) {
        const Bytes bytes = encode(message);
        rcs_module_send_machine(module_, bytes.data(), uint32_t(bytes.size()));
    }

    void keepAwake(rcs_keep_awake mode) {
        rcs_result result = RCS_OK;
        std::string why;
        if (mode != mode_) {
            request_.reset();
            mode_ = RCS_KEEP_AWAKE_OFF;
            if (mode != RCS_KEEP_AWAKE_OFF) {
                try {
                    request_ = createRequest(mode, reason);
                    mode_ = mode;
                } catch (const Refused& refused) {
                    result = refused.result();
                    why = refused.what();
                }
            }
        }
        send(Held{mode_, result, why});
    }

    void accept(const Act& act) {
        const auto system = machine();
        try {
            system->check(act.action, act.flags);
            std::lock_guard lock(actingMutex);
            if (acting) {
                throw Refused(RCS_ERROR_BUSY, "Another action waits to be carried out on this machine");
            }
            acting = true;
        } catch (const Refused& refused) {
            send(Acted{act.serial, RCS_POWER_ACTION_REFUSED,
                       refused.result() == RCS_OK ? RCS_ERROR_INTERNAL : refused.result(), 0, refused.what()});
            return;
        }
        send(Acted{act.serial, RCS_POWER_ACTION_ACCEPTED, RCS_OK, act.delayMs, {}});
        try {
            carryOut(system, link_, act);
        } catch (const std::exception& error) { // No thread to wait on.
            {
                std::lock_guard lock(actingMutex);
                acting = false;
            }
            send(Acted{act.serial, RCS_POWER_ACTION_FAILED, RCS_ERROR_INTERNAL, 0, error.what()});
        }
    }

    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return quit_ || list_; });
            if (quit_) {
                return;
            }
            list_ = false;
            lock.unlock();
            Listed listed;
            try {
                listed.interfaces = machine()->interfaces();
                if (listed.interfaces.size() > RCS_POWER_INTERFACES_MAX) {
                    listed.interfaces.resize(RCS_POWER_INTERFACES_MAX);
                }
            } catch (const Refused& refused) {
                listed.result = refused.result() == RCS_OK ? RCS_ERROR_INTERNAL : refused.result();
                listed.message = refused.what();
            } catch (const std::exception& error) {
                listed.result = RCS_ERROR_INTERNAL;
                listed.message = error.what();
            }
            send(listed);
            lock.lock();
        }
    }

    const rcs_module module_;
    rcs_keep_awake mode_ = RCS_KEEP_AWAKE_OFF;
    std::unique_ptr<Request> request_;
    const std::shared_ptr<Link> link_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool list_ = false, quit_ = false;
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

rcs_module_definition makeDefinition() {
    rcs_module_definition d{};
    d.struct_size = sizeof(d);
    d.id = RCS_POWER_MODULE;
    d.version = moduleVersion;
    d.flags = RCS_MODULE_PEER | RCS_MODULE_MACHINE;
    d.roles = (1u << RCS_ROLE_SHARE) | (1u << RCS_ROLE_VIEW);
    d.max_message_bytes = maxMessageBytes;
    d.check = [](rcs_role role, const void* settings, uint32_t size, char* why, uint32_t capacity,
                 void*) -> rcs_result {
        const std::string problem = checked(role, settings, size);
        if (problem.empty()) {
            return RCS_OK;
        }
        if (capacity) {
            std::snprintf(why, capacity, "%s", problem.c_str());
        }
        return RCS_ERROR_INVALID_ARGUMENT;
    };
    d.start = [](rcs_module module, rcs_role role, const void* settings, uint32_t, void*) {
        guarded(module, [&] { half(module).start(role, settingsOf(settings)); });
    };
    d.update = [](rcs_module module, const void* settings, uint32_t, void*) {
        guarded(module, [&] { half(module).update(settingsOf(settings)); });
    };
    d.connected = [](rcs_module module, rcs_viewer_id viewer, void*) {
        guarded(module, [&] { half(module).connected(viewer); });
    };
    d.disconnected = [](rcs_module module, rcs_viewer_id viewer, void*) {
        guarded(module, [&] { half(module).disconnected(viewer); });
    };
    d.peer_joined = [](rcs_module module, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void*) {
        guarded(module, [&] { half(module).peerJoined(peer, {hello, size}); });
    };
    d.peer_hello = [](rcs_module module, rcs_viewer_id peer, const uint8_t* hello, uint32_t size, void*) {
        guarded(module, [&] { half(module).peerHello(peer, {hello, size}); });
    };
    d.peer_control = [](rcs_module module, rcs_viewer_id peer, uint32_t granted, void*) {
        guarded(module, [&] { half(module).peerControl(peer, granted != 0); });
    };
    d.peer_left = [](rcs_module module, rcs_viewer_id peer, void*) {
        guarded(module, [&] { half(module).peerLeft(peer); });
    };
    d.received = [](rcs_module module, rcs_viewer_id peer, const uint8_t* data, uint32_t size,
                    rcs_module_message_flags, void*) {
        guarded(module, [&] { half(module).received(peer, {data, size}); });
    };
    d.machine_joined = [](rcs_module module, void*) { guarded(module, [&] { half(module).machineJoined(); }); };
    d.machine_left = [](rcs_module module, void*) { guarded(module, [&] { half(module).machineLeft(); }); };
    d.from_machine = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] { half(module).fromMachine({data, size}); });
    };
    d.stop = [](rcs_module module, void*) { guarded(module, [&] { half(module).stop(); }); };
    d.machine_start = [](rcs_module module, rcs_role, void*) {
        guarded(module, [&] {
            rcs_module_set_state(module, new MachineHalf(module),
                                 [](void* state) { delete static_cast<MachineHalf*>(state); });
        });
    };
    d.from_session = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] {
            if (auto* machine = static_cast<MachineHalf*>(rcs_module_state(module))) {
                machine->fromSession({data, size});
            }
        });
    };
    d.machine_stop = [](rcs_module module, void*) { rcs_module_set_state(module, nullptr, nullptr); };
    return d;
}

rcs_result notInitialised(const char* type, const char* init) {
    return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, (std::string("Initialise ") + type + " with " + init +
                                                                " from this library's headers")
                                                                   .c_str());
}

// Runs `task` on the session's half of the module, in its calls' order.
template <class Task>
rcs_result onHalf(rcs_session session, Task&& task) {
    if (!session) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    rcs_module module = nullptr;
    if (const rcs_result found = rcs_module_find(session, RCS_POWER_MODULE, &module); found != RCS_OK) {
        return found;
    }
    return rcs_module_run(
        module, [](rcs_module m, void* argument) -> rcs_result { return (*static_cast<Task*>(argument))(half(m)); },
        &task);
}

} // namespace
} // namespace rcsdk::power

using namespace rcsdk::power;

extern "C" {

const rcs_module_definition* rcs_power_module(void) {
    static const rcs_module_definition definition = makeDefinition();
    return &definition;
}

rcs_result rcs_power_get_capabilities(rcs_power_capabilities* out) {
    if (!out || out->struct_size != sizeof(*out)) {
        return notInitialised("rcs_power_capabilities", "RCS_POWER_CAPABILITIES_INIT");
    }
    const auto system = machine();
    const Abilities abilities = system->abilities();
    const std::string noInterfaces = system->noInterfaces();
    std::string why = abilities.why;
    if (!noInterfaces.empty()) {
        why += (why.empty() ? "" : "; ") + noInterfaces;
    }
    out->keep_awake = systemAvailable() ? 1 : 0;
    out->actions = abilities.actions;
    out->interfaces = noInterfaces.empty() ? 1 : 0;
    out->send_wake = sendsWake() ? 1 : 0;
    std::snprintf(out->message, sizeof(out->message), "%s", why.c_str());
    return rcs_set_last_call_error(RCS_OK, nullptr);
}

rcs_result rcs_power_get_status(rcs_session session, rcs_power_status* status) {
    if (!status || status->struct_size != sizeof(*status)) {
        return notInitialised("rcs_power_status", "RCS_POWER_STATUS_INIT");
    }
    return onHalf(session, [&](Half& h) { return h.status(*status); });
}

rcs_result rcs_power_list_interfaces(rcs_power_interface* items, uint32_t item_size, uint32_t capacity,
                                     uint32_t* out_count) {
    if (item_size != sizeof(rcs_power_interface) || (!items && capacity) || !out_count) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "item_size sizeof(rcs_power_interface), items for capacity, and out_count");
    }
    std::vector<rcs_power_interface> list;
    try {
        list = machine()->interfaces();
    } catch (const Refused& refused) {
        return rcs_set_last_call_error(refused.result(), refused.what());
    }
    return listed(list, items, capacity, *out_count);
}

rcs_result rcs_power_send_wake(const rcs_power_wake* wake) {
    if (!wake || wake->struct_size != sizeof(*wake)) {
        return notInitialised("rcs_power_wake", "RCS_POWER_WAKE_INIT");
    }
    bool zeros = true;
    for (const uint8_t b : wake->mac) {
        zeros = zeros && !b;
    }
    if (zeros || (wake->mac[0] & 1)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "rcs_power_wake.mac: a network interface's address, not all zeros nor a group "
                                       "(multicast or broadcast) address");
    }
    uint32_t address = 0;
    if (wake->address && (!parseAddress(wake->address, address) || !address)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "rcs_power_wake.address: a dotted IPv4 address (\"192.168.1.255\") other than "
                                       "0.0.0.0, or NULL for 255.255.255.255");
    }
    if (wake->port < 1 || wake->port > 65535) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "rcs_power_wake.port: 1..65535");
    }
    if (wake->password_size != 0 && wake->password_size != 4 && wake->password_size != 6) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "rcs_power_wake.password_size: 0, 4 or 6");
    }
    try {
        sendWake(*wake);
    } catch (const Refused& refused) {
        return rcs_set_last_call_error(refused.result(), refused.what());
    }
    return rcs_set_last_call_error(RCS_OK, nullptr);
}

rcs_result rcs_power_request_action(rcs_session session, rcs_power_action action, uint32_t flags,
                                    uint64_t* out_request) {
    if (action < RCS_POWER_RESTART || action > RCS_POWER_SIGN_OUT) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "rcs_power_action: no version defines it");
    }
    if (flags & ~uint32_t(RCS_POWER_FORCE)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "flags: RCS_POWER_FORCE or none");
    }
    return onHalf(session, [&](Half& h) { return h.ask(action, flags, out_request); });
}

rcs_result rcs_power_get_peer(rcs_session session, rcs_power_peer* out) {
    if (!out || out->struct_size != sizeof(*out)) {
        return notInitialised("rcs_power_peer", "RCS_POWER_PEER_INIT");
    }
    return onHalf(session, [&](Half& h) { return h.offered(*out); });
}

rcs_result rcs_power_get_peer_interfaces(rcs_session session, rcs_power_interface* items, uint32_t item_size,
                                         uint32_t capacity, uint32_t* out_count) {
    if (item_size != sizeof(rcs_power_interface) || (!items && capacity) || !out_count) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "item_size sizeof(rcs_power_interface), items for capacity, and out_count");
    }
    return onHalf(session, [&](Half& h) { return h.peerInterfaces(items, capacity, *out_count); });
}

} // extern "C"
