// The modules a session uses (RCS_EXT_MODULE, rcsdk/module.h): the session's
// half of each, for the session's life, started with each run and stopped
// at its end.
//
// Each connection's ends tell each other their modules: the viewer's list
// goes first, the sharer answers with its own (DataPart moduleList, on the
// data stream before any module's message or hello, so each end has the
// other's list before it hears from a module). A module that both use in the
// same version is active with that peer; one is not where the other end does
// not use it or has another version (RCS_EVENT_MODULE says which).
//
// The machine's halves run where the machine is: a sharer's agent
// (agent::Port's modules), a viewer's own process (a MachineHost of this set).
//
// No lock of the set is held while it calls into a module.
#pragma once

#include "modules/machine_host.hpp"
#include "modules/module.hpp"
#include "protocol/messages.hpp"
#include "session/data_channel.hpp"
#include "util/api.hpp"

#include <rcsdk/module.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace rcsdk::session {

class Session;
struct SessionConfig;

class ModuleSet {
public:
    // The modules of session.config(), stopped.
    explicit ModuleSet(Session& session);
    ~ModuleSet();
    ModuleSet(const ModuleSet&) = delete;
    ModuleSet& operator=(const ModuleSet&) = delete;

    void setHandle(rcs_session handle) noexcept { handle_ = handle; }
    rcs_session handle() const noexcept { return handle_; }
    size_t size() const noexcept { return slots_.size(); }
    // The session's half of module `id`, or null; its place among them, or -1.
    rcs_module_impl* find(std::string_view id) const noexcept;
    int indexOf(std::string_view id) const noexcept;
    // Module `index`'s number in this end's list (its queue's), 0 when not listed.
    uint8_t numberOf(size_t index) const noexcept { return slots_[index].number; }
    // Each module's bytes waiting in `queue`, into `stats`.
    void addQueued(std::vector<rcs_module_stats>& stats, const DataQueue& queue) const;

    // ---- A run (the worker thread) -----------------------------------------------------

    // Each module's start, and (view) its machine's half.
    void start();
    // Once every connection ended: the machine's halves stop, then each
    // module's machine_left and stop.
    void stop();
    // The settings of `config` while running (rcs_session_update), to the
    // modules whose settings changed.
    void update(const SessionConfig& before, const SessionConfig& after);

    // ---- A connection's peer (its threads) ------------------------------------------------

    // A connection to `peer` passed the handshake (connected); peerGone ends it.
    void peerConnected(rcs_viewer_id peer);
    // This end's list goes to `peer` through `queue`, once: a viewer's first,
    // a sharer's in answer to the viewer's (peerList does).
    void listTo(rcs_viewer_id peer, DataQueue& queue);
    // The peer's list came: the modules both use alike are active with it.
    // Throws Failure(PROTOCOL) when it came before.
    void peerList(rcs_viewer_id peer, const protocol::ModuleList& list, DataQueue& queue);
    // A hello of the peer's module `number` (as its list numbers them).
    void peerHello(rcs_viewer_id peer, uint8_t number, const crypto::Bytes& hello);
    // Whether `peer` holds control now (a sharer's viewer; a viewer's own: 0),
    // for the modules active with it (peer_control).
    void peerControl(rcs_viewer_id peer, bool granted);
    // Its connection ended: no module is active with it any more (peer_left,
    // then disconnected).
    void peerGone(rcs_viewer_id peer);
    // The most a message of the peer's module `number` holds; throws
    // Failure(PROTOCOL) where that module is not active.
    size_t maxMessage(rcs_viewer_id peer, uint8_t number) const;
    // A message of the peer's module `number` came, whole. A datagram of a
    // module not active (yet: it may overtake the list) is dropped.
    void received(rcs_viewer_id peer, uint8_t number, const crypto::Bytes& bytes, bool unreliable);
    // A message of this end's module `number` (as this end's list numbers
    // them) went to `peer`; its queue there has room again.
    void sent(rcs_viewer_id peer, uint8_t number, uint64_t tag, bool unreliable, size_t bytes);
    void room(rcs_viewer_id peer, uint8_t number);
    // How much of module `number`'s messages a queue holds.
    size_t queueLimit(uint8_t number) const noexcept;

    // ---- The machine's halves ------------------------------------------------------------

    // Share: the run's agent exists; each module's half is asked of it.
    void agentReady();
    // Share: the agent that ran the halves left, or another took its place.
    void machineGone();
    // A half runs (RCS_OK), or why it cannot; its messages.
    void machineStatus(const std::string& id, rcs_result result, const std::string& message);
    void fromMachine(const std::string& id, const crypto::Bytes& bytes);

    // ---- What a module asks (rcs_module_*); throw Failure naming why not ----------------

    void send(rcs_module_impl& module, rcs_viewer_id peer, std::span<const uint8_t> data, uint32_t flags,
              uint64_t tag);
    void setHello(rcs_module_impl& module, std::span<const uint8_t> hello);
    void sendMachine(rcs_module_impl& module, std::span<const uint8_t> data);
    void emit(rcs_module_impl& module, rcs_viewer_id peer, uint32_t kind, std::span<const uint8_t> payload);

private:
    // One module. Its run's state, under its calls mutex (inside its calls):
    struct Slot {
        std::unique_ptr<rcs_module_impl> module;
        uint8_t number = 0; // In this end's list; 0: not listed (no RCS_MODULE_PEER).
        bool running = false;
        std::set<rcs_viewer_id> active;
        std::set<rcs_viewer_id> connected;
        bool machineJoined = false;
        crypto::Bytes hello; // Under peersMutex_.
    };
    // A connection's peer, under peersMutex_.
    struct Peer {
        DataQueue* queue = nullptr;
        bool listSent = false, listCame = false;
        bool control = false; // It holds control.
        std::vector<uint8_t> theirs; // By slot: its number in the peer's list where active, else 0.
        std::vector<int> ours;       // By the peer's number less one: the slot, -1 where not active.
    };

    const rcs_module_definition& table(const Slot& slot) const noexcept { return slot.module->definition.table; }
    Slot* slotOf(std::string_view id) noexcept;
    Slot& listed(uint8_t number) noexcept { return slots_[byNumber_[number - 1u]]; }
    const Slot& listed(uint8_t number) const noexcept { return slots_[byNumber_[number - 1u]]; }
    // The slot active with `peer` as its module `number`; -1 where none is,
    // and (`strict`) Failure(PROTOCOL) for a number its list does not have.
    int activeSlot(rcs_viewer_id peer, uint8_t number, bool strict) const;
    // `body` as a call into the slot's module, as one of the session's callbacks.
    void call(Slot& slot, const std::function<void()>& body);
    void report(rcs_viewer_id peer, const Slot& slot, rcs_module_event_kind kind, rcs_result result,
                const std::string& message);
    void updateStats(rcs_viewer_id peer, const Slot& slot, const std::function<void(rcs_module_stats&)>& update);

    Session& session_;
    rcs_session handle_ = nullptr;
    std::vector<Slot> slots_;
    std::vector<size_t> byNumber_; // The slots listed, by number less one.

    mutable std::mutex peersMutex_;
    std::map<rcs_viewer_id, Peer> peers_;

    bool agentAsked_ = false; // Share: this run's agent was asked for the halves (the worker thread's).
    std::mutex machineMutex_;
    bool machinesOpen_ = false;   // Between a run's start (share: its agent) and its stop: halves may run.
    std::set<size_t> machinesUp_; // The slots whose machine's half runs.
    std::unique_ptr<modules::MachineHost> machines_; // View: this process's halves, during a run.
};

} // namespace rcsdk::session
