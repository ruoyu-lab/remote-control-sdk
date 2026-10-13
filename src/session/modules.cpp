#include "session/modules.hpp"

#include "session/session.hpp"
#include "session/share_agent.hpp"
#include "util/log.hpp"

namespace rcsdk::session {

using protocol::DataPart;

ModuleSet::ModuleSet(Session& session) : session_(session) {
    for (const auto& use : session.config().modules) {
        Slot slot;
        slot.module = std::make_unique<rcs_module_impl>(*use.module, session.logSource());
        slot.module->set = this;
        slot.module->number = uint32_t(slots_.size());
        if (use.module->peer()) {
            byNumber_.push_back(slots_.size());
            slot.number = uint8_t(byNumber_.size());
        }
        slots_.push_back(std::move(slot));
    }
}

ModuleSet::~ModuleSet() = default;

rcs_module_impl* ModuleSet::find(std::string_view id) const noexcept {
    const int index = indexOf(id);
    return index < 0 ? nullptr : slots_[size_t(index)].module.get();
}

int ModuleSet::indexOf(std::string_view id) const noexcept {
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].module->definition.id == id) {
            return int(i);
        }
    }
    return -1;
}

void ModuleSet::addQueued(std::vector<rcs_module_stats>& stats, const DataQueue& queue) const {
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (const size_t waiting = slots_[i].number ? queue.bytes(slots_[i].number) : 0) {
            if (stats.size() <= i) {
                stats.resize(slots_.size(), sizedStruct<rcs_module_stats>());
            }
            stats[i].queued_bytes += waiting;
        }
    }
}

ModuleSet::Slot* ModuleSet::slotOf(std::string_view id) noexcept {
    const int index = indexOf(id);
    return index < 0 ? nullptr : &slots_[size_t(index)];
}

void ModuleSet::call(Slot& slot, const std::function<void()>& body) {
    const Session::CallbackThread mark(session_);
    modules::call(*slot.module, body);
}

void ModuleSet::report(rcs_viewer_id peer, const Slot& slot, rcs_module_event_kind kind, rcs_result result,
                       const std::string& message) {
    rcs_module_event event{};
    event.module = slot.module->definition.id.c_str();
    event.kind = kind;
    event.result = result;
    copyText(event.message, message);
    session_.moduleEvent(peer, event);
}

void ModuleSet::updateStats(rcs_viewer_id peer, const Slot& slot,
                            const std::function<void(rcs_module_stats&)>& update) {
    const size_t index = size_t(&slot - slots_.data());
    const size_t count = slots_.size();
    session_.updateStats(peer, [&](Stats& s) {
        if (s.modules.size() < count) {
            s.modules.resize(count, sizedStruct<rcs_module_stats>());
        }
        update(s.modules[index]);
    });
}

// ---- A run --------------------------------------------------------------------------------

void ModuleSet::start() {
    const auto live = session_.liveConfig();
    const rcs_role role = session_.config().role;
    {
        std::lock_guard lock(peersMutex_);
        peers_.clear();
    }
    for (size_t i = 0; i < slots_.size(); ++i) {
        auto& slot = slots_[i];
        const auto& settings = live->modules[i].settings;
        call(slot, [&] {
            slot.running = true;
            slot.active.clear();
            slot.connected.clear();
            slot.machineJoined = false;
            if (const auto start = table(slot).start) {
                start(slot.module.get(), role, settings.data(), uint32_t(settings.size()), table(slot).user_data);
            }
        });
    }
    if (role == RCS_ROLE_VIEW) {
        std::unique_ptr<modules::MachineHost> host;
        for (const auto& slot : slots_) {
            if (!slot.module->definition.machine()) {
                continue;
            }
            if (!host) {
                modules::MachineHost::Output output;
                output.status = [this](const std::string& id, rcs_result result, const std::string& message) {
                    machineStatus(id, result, message);
                };
                output.data = [this](const std::string& id, Bytes bytes) { fromMachine(id, bytes); };
                host = std::make_unique<modules::MachineHost>(role, std::move(output));
            }
            host->start(slot.module->definition.id, table(slot).version);
        }
        std::lock_guard lock(machineMutex_);
        machines_ = std::move(host);
        machinesOpen_ = true;
    }
}

void ModuleSet::stop() {
    std::unique_ptr<modules::MachineHost> host;
    std::set<size_t> up;
    {
        std::lock_guard lock(machineMutex_);
        host = std::move(machines_);
        up.swap(machinesUp_);
        machinesOpen_ = false; // A half's answer that comes now is too late.
    }
    if (std::exchange(agentAsked_, false)) {
        for (const auto& slot : slots_) {
            if (slot.module->definition.machine()) {
                session_.shareAgent().port().stopModule(slot.module->definition.id);
            }
        }
    }
    host.reset(); // A viewer's halves stop (their last messages still reach the modules).
    for (auto& slot : slots_) {
        call(slot, [&] {
            if (std::exchange(slot.machineJoined, false)) {
                if (const auto left = table(slot).machine_left) {
                    left(slot.module.get(), table(slot).user_data);
                }
            }
            slot.running = false;
            if (const auto stop = table(slot).stop) {
                stop(slot.module.get(), table(slot).user_data);
            }
        });
    }
}

void ModuleSet::update(const SessionConfig& before, const SessionConfig& after) {
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (after.modules[i].settings == before.modules[i].settings) {
            continue;
        }
        auto& slot = slots_[i];
        const auto& settings = after.modules[i].settings;
        call(slot, [&] {
            if (slot.running) {
                if (const auto update = table(slot).update) {
                    update(slot.module.get(), settings.data(), uint32_t(settings.size()), table(slot).user_data);
                }
            }
        });
    }
}

// ---- A connection's peer ------------------------------------------------------------------

void ModuleSet::peerConnected(rcs_viewer_id peer) {
    for (auto& slot : slots_) {
        call(slot, [&] {
            if (slot.running && slot.connected.insert(peer).second) {
                if (const auto connected = table(slot).connected) {
                    connected(slot.module.get(), peer, table(slot).user_data);
                }
            }
        });
    }
}

void ModuleSet::listTo(rcs_viewer_id peer, DataQueue& queue) {
    std::lock_guard lock(peersMutex_);
    auto& p = peers_[peer];
    if (p.listSent) {
        return;
    }
    p.queue = &queue;
    p.listSent = true;
    protocol::ModuleList list;
    for (const size_t index : byNumber_) {
        const auto& slot = slots_[index];
        list.modules.push_back({slot.module->definition.id, table(slot).version, slot.hello});
    }
    DataQueue::Item item;
    item.kind = DataPart::moduleList;
    item.bytes = protocol::encodeModuleList(list);
    queue.pushFirst(std::move(item));
}

void ModuleSet::peerList(rcs_viewer_id peer, const protocol::ModuleList& list, DataQueue& queue) {
    listTo(peer, queue); // A sharer answers; a viewer's went first.
    struct Found {
        size_t slot;
        std::string problem; // Empty: active.
        crypto::Bytes hello;
    };
    std::vector<Found> found;
    bool control = false;
    {
        std::lock_guard lock(peersMutex_);
        auto& p = peers_[peer];
        if (p.listCame) {
            fail(RCS_ERROR_PROTOCOL, "The other end sent its module list twice");
        }
        p.listCame = true;
        control = p.control;
        p.theirs.assign(slots_.size(), 0);
        p.ours.assign(list.modules.size(), -1);
        for (const size_t index : byNumber_) {
            const auto& definition = slots_[index].module->definition;
            const auto entry = std::find_if(list.modules.begin(), list.modules.end(),
                                            [&](const protocol::ModuleList::Entry& e) { return e.id == definition.id; });
            if (entry == list.modules.end()) {
                found.push_back({index, "The other end does not use module \"" + definition.id + "\"", {}});
            } else if (entry->version != definition.table.version) {
                found.push_back({index,
                                 "The other end has version " + std::to_string(entry->version) + " of module \"" +
                                     definition.id + "\", this end " + std::to_string(definition.table.version),
                                 {}});
            } else {
                const size_t number = size_t(entry - list.modules.begin()) + 1;
                p.theirs[index] = uint8_t(number);
                p.ours[number - 1] = int(index);
                found.push_back({index, {}, entry->hello});
            }
        }
    }
    for (auto& f : found) {
        auto& slot = slots_[f.slot];
        if (!f.problem.empty()) {
            log::info("modules", f.problem);
            report(peer, slot, RCS_MODULE_INACTIVE, RCS_ERROR_UNSUPPORTED, f.problem);
            continue;
        }
        report(peer, slot, RCS_MODULE_ACTIVE, RCS_OK, {});
        call(slot, [&] {
            if (!slot.running || !slot.active.insert(peer).second) {
                return;
            }
            if (const auto joined = table(slot).peer_joined) {
                joined(slot.module.get(), peer, f.hello.data(), uint32_t(f.hello.size()), table(slot).user_data);
            }
            if (const auto controls = table(slot).peer_control; controls && control) {
                controls(slot.module.get(), peer, 1, table(slot).user_data);
            }
        });
    }
}

int ModuleSet::activeSlot(rcs_viewer_id peer, uint8_t number, bool strict) const {
    std::lock_guard lock(peersMutex_);
    const auto found = peers_.find(peer);
    if (found == peers_.end() || !found->second.listCame || !number || number > found->second.ours.size()) {
        if (strict) {
            fail(RCS_ERROR_PROTOCOL, "A module message of a module the other end did not list");
        }
        return -1;
    }
    return found->second.ours[number - 1u];
}

void ModuleSet::peerHello(rcs_viewer_id peer, uint8_t number, const crypto::Bytes& hello) {
    const int index = activeSlot(peer, number, true);
    if (index < 0) {
        return; // A module of the peer's this end does not use alike.
    }
    auto& slot = slots_[size_t(index)];
    call(slot, [&] {
        if (slot.active.count(peer)) {
            if (const auto said = table(slot).peer_hello) {
                said(slot.module.get(), peer, hello.data(), uint32_t(hello.size()), table(slot).user_data);
            }
        }
    });
}

void ModuleSet::peerControl(rcs_viewer_id peer, bool granted) {
    std::vector<size_t> active;
    {
        std::lock_guard lock(peersMutex_);
        auto& p = peers_[peer]; // Before its list too: told once the modules are active.
        if (p.control == granted) {
            return;
        }
        p.control = granted;
        for (size_t i = 0; i < p.theirs.size(); ++i) {
            if (p.theirs[i]) {
                active.push_back(i);
            }
        }
    }
    for (const size_t index : active) {
        auto& slot = slots_[index];
        call(slot, [&] {
            if (const auto controls = table(slot).peer_control; controls && slot.active.count(peer)) {
                controls(slot.module.get(), peer, granted ? 1 : 0, table(slot).user_data);
            }
        });
    }
}

void ModuleSet::peerGone(rcs_viewer_id peer) {
    {
        std::lock_guard lock(peersMutex_);
        peers_.erase(peer);
    }
    for (auto& slot : slots_) {
        call(slot, [&] {
            if (slot.active.erase(peer)) {
                if (const auto left = table(slot).peer_left) {
                    left(slot.module.get(), peer, table(slot).user_data);
                }
            }
            if (slot.connected.erase(peer)) {
                if (const auto disconnected = table(slot).disconnected) {
                    disconnected(slot.module.get(), peer, table(slot).user_data);
                }
            }
        });
    }
}

size_t ModuleSet::maxMessage(rcs_viewer_id peer, uint8_t number) const {
    const int index = activeSlot(peer, number, true);
    if (index < 0) {
        fail(RCS_ERROR_PROTOCOL, "A message of a module that is not active with this end");
    }
    return table(slots_[size_t(index)]).max_message_bytes;
}

void ModuleSet::received(rcs_viewer_id peer, uint8_t number, const crypto::Bytes& bytes, bool unreliable) {
    const int index = activeSlot(peer, number, !unreliable);
    if (index < 0) {
        if (!unreliable) {
            fail(RCS_ERROR_PROTOCOL, "A message of a module that is not active with this end");
        }
        return;
    }
    auto& slot = slots_[size_t(index)];
    if (bytes.size() > table(slot).max_message_bytes) {
        fail(RCS_ERROR_PROTOCOL, "A message of module \"" + slot.module->definition.id + "\" larger than it takes");
    }
    updateStats(peer, slot, [&](rcs_module_stats& s) {
        ++s.received_messages;
        s.received_bytes += bytes.size();
    });
    call(slot, [&] {
        if (slot.active.count(peer)) {
            if (const auto receive = table(slot).received) {
                receive(slot.module.get(), peer, bytes.data(), uint32_t(bytes.size()),
                        unreliable ? uint32_t(RCS_MODULE_UNRELIABLE) : 0u, table(slot).user_data);
            }
        }
    });
}

void ModuleSet::sent(rcs_viewer_id peer, uint8_t number, uint64_t tag, bool unreliable, size_t bytes) {
    auto& slot = listed(number);
    updateStats(peer, slot, [&](rcs_module_stats& s) {
        ++s.sent_messages;
        s.sent_bytes += bytes;
    });
    if (unreliable || !table(slot).sent) {
        return;
    }
    call(slot, [&] {
        if (slot.active.count(peer)) {
            table(slot).sent(slot.module.get(), peer, tag, table(slot).user_data);
        }
    });
}

void ModuleSet::room(rcs_viewer_id peer, uint8_t number) {
    auto& slot = listed(number);
    if (!table(slot).room) {
        return;
    }
    call(slot, [&] {
        if (slot.active.count(peer)) {
            table(slot).room(slot.module.get(), peer, table(slot).user_data);
        }
    });
}

size_t ModuleSet::queueLimit(uint8_t number) const noexcept {
    return table(listed(number)).max_message_bytes;
}

// ---- The machine's halves -----------------------------------------------------------------

void ModuleSet::agentReady() {
    agentAsked_ = true;
    {
        std::lock_guard lock(machineMutex_);
        machinesOpen_ = true;
    }
    for (const auto& slot : slots_) {
        if (slot.module->definition.machine()) {
            session_.shareAgent().port().startModule(slot.module->definition.id, table(slot).version);
        }
    }
}

void ModuleSet::machineGone() {
    {
        std::lock_guard lock(machineMutex_);
        machinesUp_.clear();
    }
    for (auto& slot : slots_) {
        call(slot, [&] {
            if (std::exchange(slot.machineJoined, false)) {
                if (const auto left = table(slot).machine_left) {
                    left(slot.module.get(), table(slot).user_data);
                }
            }
        });
    }
}

void ModuleSet::machineStatus(const std::string& id, rcs_result result, const std::string& message) {
    Slot* slot = slotOf(id);
    if (!slot) {
        return;
    }
    if (result != RCS_OK) {
        report(0, *slot, RCS_MODULE_NO_MACHINE, result, message);
        return;
    }
    {
        std::lock_guard lock(machineMutex_);
        if (!machinesOpen_) {
            return; // The run ended meanwhile.
        }
        machinesUp_.insert(size_t(slot - slots_.data()));
    }
    call(*slot, [&] {
        if (slot->running && !std::exchange(slot->machineJoined, true)) {
            if (const auto joined = table(*slot).machine_joined) {
                joined(slot->module.get(), table(*slot).user_data);
            }
        }
    });
}

void ModuleSet::fromMachine(const std::string& id, const crypto::Bytes& bytes) {
    Slot* slot = slotOf(id);
    if (!slot) {
        return;
    }
    call(*slot, [&] {
        if (slot->running && slot->machineJoined) {
            if (const auto receive = table(*slot).from_machine) {
                receive(slot->module.get(), bytes.data(), uint32_t(bytes.size()), table(*slot).user_data);
            }
        }
    });
}

// ---- What a module asks -------------------------------------------------------------------

void ModuleSet::send(rcs_module_impl& module, rcs_viewer_id peer, std::span<const uint8_t> data, uint32_t flags,
                     uint64_t tag) {
    const auto& slot = slots_[module.number];
    const auto& definition = module.definition;
    if (flags & ~uint32_t(RCS_MODULE_UNRELIABLE | RCS_MODULE_REPLACE)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_send: flags takes RCS_MODULE_UNRELIABLE or RCS_MODULE_REPLACE");
    }
    const bool unreliable = flags & RCS_MODULE_UNRELIABLE;
    if (unreliable && (flags & RCS_MODULE_REPLACE)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_send: RCS_MODULE_REPLACE is for reliable messages");
    }
    const size_t most = unreliable ? std::min<size_t>(RCS_DATA_MAX_DATAGRAM, definition.table.max_message_bytes)
                                   : definition.table.max_message_bytes;
    if (data.empty() || data.size() > most) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_send: 1.." + std::to_string(most) + " bytes for module \"" +
                                             definition.id + "\"");
    }
    if (!slot.number) {
        fail(RCS_ERROR_INVALID_STATE, "Module \"" + definition.id + "\" has no RCS_MODULE_PEER: it sends nothing");
    }
    if (session_.config().role == RCS_ROLE_VIEW && peer) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_send: a viewer's peer, its sharer, is 0");
    }
    DataQueue::Item item;
    item.kind = DataPart::moduleMessage;
    item.module = slot.number;
    item.unreliable = unreliable;
    item.replace = flags & RCS_MODULE_REPLACE;
    item.tag = tag;
    item.bytes.assign(data.begin(), data.end());
    const size_t limit = definition.table.max_message_bytes;
    // Every peer where it is active, or none: all its sends go through this
    // lock, and the senders only take from the queues.
    std::lock_guard lock(peersMutex_);
    std::vector<DataQueue*> queues;
    for (const auto& [id, p] : peers_) {
        if ((!peer || id == peer) && !p.theirs.empty() && p.theirs[module.number]) {
            queues.push_back(p.queue);
        }
    }
    if (queues.empty()) {
        fail(RCS_ERROR_INVALID_STATE, "Module \"" + definition.id + "\" is not active with " +
                                          (peer ? "viewer " + std::to_string(peer) : std::string("any peer")));
    }
    for (auto* queue : queues) {
        if (queue->fits(item, limit) == DataQueue::Push::full) {
            fail(RCS_ERROR_QUEUE_FULL, "Module \"" + definition.id + "\"'s queue" +
                                           (queues.size() > 1 ? " to a viewer" : "") + " is full; wait for room()");
        }
    }
    for (auto* queue : queues) {
        queue->push(item, limit);
    }
}

void ModuleSet::setHello(rcs_module_impl& module, std::span<const uint8_t> hello) {
    if (hello.size() > RCS_MODULE_HELLO_MAX) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_set_hello: at most RCS_MODULE_HELLO_MAX bytes");
    }
    auto& slot = slots_[module.number];
    std::lock_guard lock(peersMutex_);
    slot.hello.assign(hello.begin(), hello.end());
    if (!slot.number) {
        return;
    }
    // Every end that has this end's list hears it, also one whose list has not
    // come yet: it comes after the list, and is dropped where not active.
    for (auto& [id, p] : peers_) {
        if (p.listSent) {
            DataQueue::Item item;
            item.kind = DataPart::moduleHello;
            item.module = slot.number;
            item.bytes = slot.hello;
            p.queue->pushFirst(std::move(item));
        }
    }
}

void ModuleSet::sendMachine(rcs_module_impl& module, std::span<const uint8_t> data) {
    const auto& definition = module.definition;
    if (data.empty() || data.size() > definition.table.max_message_bytes) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_send_machine: 1.." +
                                             std::to_string(definition.table.max_message_bytes) + " bytes");
    }
    std::lock_guard lock(machineMutex_);
    if (!machinesUp_.count(module.number)) {
        fail(RCS_ERROR_INVALID_STATE, definition.machine()
                                          ? "Module \"" + definition.id + "\"'s machine's half does not run now"
                                          : "Module \"" + definition.id + "\" has no RCS_MODULE_MACHINE");
    }
    Bytes bytes(data.begin(), data.end());
    if (machines_) {
        machines_->fromSession(definition.id, std::move(bytes));
    } else {
        session_.shareAgent().port().moduleData(definition.id, std::move(bytes));
    }
}

void ModuleSet::emit(rcs_module_impl& module, rcs_viewer_id peer, uint32_t kind, std::span<const uint8_t> payload) {
    if (kind < RCS_MODULE_EVENT_OWN) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_emit: a module's own kinds are RCS_MODULE_EVENT_OWN and up");
    }
    if (payload.size() > 65536) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_emit: at most 65536 bytes of payload");
    }
    modules::afterCall(module, [this, &module, peer, kind, bytes = Bytes(payload.begin(), payload.end())] {
        rcs_module_event event{};
        event.module = module.definition.id.c_str();
        event.kind = kind;
        event.payload = bytes.empty() ? nullptr : bytes.data();
        event.size = uint32_t(bytes.size());
        session_.moduleEvent(peer, event);
    });
}

} // namespace rcsdk::session
