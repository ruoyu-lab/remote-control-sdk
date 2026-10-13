#include "session_half.hpp"

#include "files.hpp"
#include "paths.hpp"

#include <cmath>
#include <set>

namespace rcsdk::file_transfer {
namespace {

using namespace std::chrono_literals;

constexpr auto tickEvery = 50ms;
constexpr auto progressEvery = 250ms;
constexpr size_t finishedKept = 256;
constexpr size_t samplesKept = 5; // A quarter second apart: the speed over the last second.

std::string ms(uint32_t value) {
    return std::to_string(value) + " ms";
}

rcs_file_peer peerOf(const Hello& hello) {
    rcs_file_peer out = RCS_FILE_PEER_INIT;
    out.flow = hello.flow;
    out.max_items = hello.maxItems;
    out.max_offer_bytes = hello.maxOfferBytes;
    return out;
}

// What a host's callback said, as a result and a reason.
std::pair<rcs_result, std::string> called(rcs_result result) {
    if (result == RCS_OK) {
        return {RCS_OK, {}};
    }
    char reason[512] = {};
    rcs_get_last_call_error(reason, sizeof(reason));
    if (result > RCS_OK || result < RCS_ERROR_REFUSED) {
        return {RCS_ERROR_INTERNAL, "The host's callback returned " + std::to_string(result) + ", no rcs_result"};
    }
    return {result, reason[0] ? reason : "The host's callback failed"};
}

} // namespace

// ---- Ticker -------------------------------------------------------------------------------

Ticker::Ticker(rcs_module module, Hub& hub) : module_(module), hub_(hub), thread_([this] { run(); }) {}

Ticker::~Ticker() {
    stop();
}

void Ticker::stop() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Ticker::want(bool wanted) {
    {
        std::lock_guard lock(mutex_);
        wanted_ = wanted;
    }
    wake_.notify_all();
}

void Ticker::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return quit_ || wanted_; });
        if (wake_.wait_for(lock, tickEvery, [&] { return quit_; })) {
            return;
        }
        lock.unlock();
        rcs_module_run(
            module_,
            [](rcs_module module, void* hub) -> rcs_result {
                try {
                    static_cast<Hub*>(hub)->tick();
                } catch (const std::exception& error) {
                    rcs_module_log(module, RCS_LOG_ERROR, error.what());
                }
                return RCS_OK;
            },
            &hub_);
        lock.lock();
    }
}

// ---- HostStore ----------------------------------------------------------------------------

HostStore::HostStore(rcs_module module, Hub& hub) : module_(module), hub_(hub), thread_([this] { work(); }) {}

HostStore::~HostStore() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void HostStore::post(uint64_t run, Request request, std::shared_ptr<const HostContext> context,
                     rcs_viewer_id viewer) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back({run, std::move(request), std::move(context), viewer});
    }
    wake_.notify_all();
}

void HostStore::work() {
    struct Delivery {
        Hub* hub;
        uint64_t run;
        Reply reply;
    };
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
        if (quit_) {
            return;
        }
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        lock.unlock();
        if (auto reply = handle(job)) {
            Delivery delivery{&hub_, job.run, std::move(*reply)};
            rcs_module_run(
                module_,
                [](rcs_module module, void* argument) -> rcs_result {
                    auto& d = *static_cast<Delivery*>(argument);
                    try {
                        d.hub->hostReply(d.run, std::move(d.reply));
                    } catch (const std::exception& error) {
                        rcs_module_log(module, RCS_LOG_ERROR, error.what());
                    }
                    return RCS_OK;
                },
                &delivery);
        }
        lock.lock();
    }
}

std::optional<Reply> HostStore::handle(const Job& job) {
    const auto& c = *job.context;
    const auto chunkOf = [&](uint32_t item, uint64_t offset, uint8_t* data, uint32_t size) {
        const auto& spec = c.items[item];
        rcs_file_chunk chunk{};
        chunk.struct_size = sizeof(chunk);
        chunk.transfer = c.transfer;
        chunk.viewer_id = job.viewer;
        chunk.item = item;
        chunk.name = spec.path.c_str();
        chunk.item_size = spec.size;
        chunk.modified_ns = spec.modified;
        chunk.token = c.token.data();
        chunk.offset = offset;
        chunk.data = data;
        chunk.size = size;
        return chunk;
    };
    const auto& s = c.settings;
    if (const auto* r = std::get_if<PrepareRequest>(&job.request)) {
        Prepared out;
        out.transfer = r->transfer;
        for (uint32_t i = 0; i < uint32_t(c.items.size()) && s.resume; ++i) {
            const auto& spec = c.items[i];
            if (spec.kind != RCS_FILE_REGULAR) {
                continue;
            }
            const auto chunk = chunkOf(i, 0, nullptr, 0);
            const uint64_t held = s.resume(&chunk, s.user_data);
            if (held > spec.size) {
                out.starts.push_back({i, Start::failed, 0, RCS_ERROR_INVALID_ARGUMENT,
                                      "resume said " + std::to_string(held) + " bytes of an item of " +
                                          std::to_string(spec.size)});
            } else if (held && held == spec.size) {
                out.starts.push_back({i, Start::done, 0, RCS_OK, spec.path});
            } else if (held) {
                out.starts.push_back({i, Start::resume, held, RCS_OK, {}});
            }
        }
        return out;
    }
    if (const auto* r = std::get_if<ReadRequest>(&job.request)) {
        Data data{r->transfer, r->epoch, r->item, r->offset, Bytes(r->length)};
        const auto chunk = chunkOf(r->item, r->offset, data.data.data(), r->length);
        rcs_set_last_call_error(RCS_OK, nullptr);
        const auto [result, why] = called(s.read(&chunk, s.user_data));
        if (result != RCS_OK) {
            return ReadFailed{r->transfer, r->epoch, r->item, result, why};
        }
        return data;
    }
    if (const auto* r = std::get_if<WriteRequest>(&job.request)) {
        const auto chunk = chunkOf(r->item, r->offset, const_cast<uint8_t*>(r->data.data()), uint32_t(r->data.size()));
        rcs_set_last_call_error(RCS_OK, nullptr);
        const auto [result, why] = called(s.write(&chunk, s.user_data));
        if (result != RCS_OK) {
            return WriteFailed{r->transfer, r->epoch, r->item, result, why};
        }
        return Written{r->transfer, r->epoch, r->item, r->offset + r->data.size()};
    }
    if (const auto* r = std::get_if<FinishRequest>(&job.request)) {
        Finished out;
        out.transfer = r->transfer;
        out.epoch = r->epoch;
        out.item = r->item;
        return out;
    }
    if (const auto* r = std::get_if<CloseRequest>(&job.request)) {
        return Ended{r->transfer}; // Its calls asked before are through.
    }
    return std::nullopt; // The rest is the host's own business: its storage, its events.
}

// ---- Notifier -----------------------------------------------------------------------------

Notifier::Notifier() : thread_([this] { run(); }) {}

Notifier::~Notifier() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void Notifier::post(std::function<void()> call) {
    {
        std::lock_guard lock(mutex_);
        calls_.push_back(std::move(call));
    }
    wake_.notify_all();
}

void Notifier::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return quit_ || !calls_.empty(); });
        if (calls_.empty()) {
            return; // Quit, with nothing left to tell.
        }
        auto call = std::move(calls_.front());
        calls_.pop_front();
        lock.unlock();
        call();
        lock.lock();
    }
}

// ---- The run ------------------------------------------------------------------------------

Hub::~Hub() {
    // Their last calls into the hub (rcs_module_run) find it stopped; what
    // callbacks were to be told is told before the hub goes. The ticker's
    // thread ends before its pointer goes (a tick under way reads it).
    if (ticker_) {
        ticker_->stop();
    }
    ticker_.reset();
    hostStore_.reset();
    notifier_.reset();
}

void Hub::notify(std::function<void()> call) {
    notifier_->post(std::move(call));
}

Hello Hub::hello() const {
    return {settings_.flow, end_, settings_.max_items, settings_.max_offer_bytes};
}

void Hub::start(rcs_role role, const rcs_file_transfer_settings& settings) {
    role_ = role;
    settings_ = settings;
    running_ = true;
    ++run_;
    uint8_t random[32];
    rcs_generate_connection_key(random, sizeof(random));
    std::copy(random, random + end_.size(), end_.begin());
    peers_.clear();
    machine_ = false;
    machineAwaySince_ = lastTick_ = Clock::now();
    fetches_.clear();
    if (!notifier_) {
        notifier_ = std::make_unique<Notifier>();
    }
    if (!ticker_) {
        ticker_ = std::make_unique<Ticker>(module_, *this);
    }
    const Bytes bytes = encode(hello());
    rcs_module_set_hello(module_, bytes.data(), uint32_t(bytes.size()));
}

void Hub::update(const rcs_file_transfer_settings& settings) {
    const Hello before = hello();
    if (!(settings.flow & RCS_FILE_SEND)) {
        revokeAll(RCS_ERROR_INVALID_STATE, "This end does not send files any more (its flow has no RCS_FILE_SEND)");
    } else if (settings.target != settings_.target) {
        revokeAll(RCS_ERROR_INVALID_STATE, "This end's target changed: what it published is read another way");
    }
    settings_ = settings;
    if (hello() != before) {
        const Bytes bytes = encode(hello());
        rcs_module_set_hello(module_, bytes.data(), uint32_t(bytes.size()));
    }
    for (auto& [id, t] : transfers_) {
        pump(t); // The rate limit may have gone (a transfer that completes stays in the map).
    }
}

void Hub::stop() {
    std::vector<uint64_t> going;
    for (const auto& [id, t] : transfers_) {
        if (t.phase != Phase::finished) {
            going.push_back(id);
        }
    }
    for (const uint64_t id : going) {
        if (auto* t = find(id)) {
            finish(*t, RCS_FILE_FAILED, RCS_ERROR_DISCONNECTED, "The run ended before the transfer finished", false,
                   false);
        }
    }
    // Their stores' answers come no more (the machine's half left before, which told its own).
    std::vector<uint64_t> settling;
    for (const auto& [id, t] : transfers_) {
        if (t.settling) {
            settling.push_back(id);
        }
    }
    for (const uint64_t id : settling) {
        if (auto* t = find(id)) {
            announce(*t);
        }
    }
    revokeAll(RCS_ERROR_DISCONNECTED, "The run ended");
    running_ = false;
    peers_.clear();
    machine_ = false;
    purges_.clear();
    fetches_.clear();
    if (ticker_) {
        ticker_->want(false);
    }
}

// ---- The peers ----------------------------------------------------------------------------

void Hub::peerJoined(rcs_viewer_id peer, std::span<const uint8_t> bytes) {
    auto& p = peers_[peer];
    p = {};
    try {
        p.hello = decodeHello(bytes);
    } catch (const Error& error) {
        rcs_module_log(module_, RCS_LOG_WARNING,
                       (std::string("The other end said what it allows in a way this end does not read; nothing goes "
                                    "to it or comes from it: ") +
                        error.what())
                           .c_str());
        return;
    }
    emitPeer(peer, *p.hello);
    // The same end, back within the run: what it may fetch it still may, its
    // transfers go on.
    const Token end = p.hello->end;
    for (auto& [id, publication] : publications_) {
        if (publication.peerEnd == end) {
            publication.peer = peer;
            publication.present = true;
        }
    }
    for (auto& [id, t] : transfers_) {
        if (t.peerEnd != end || (t.present && t.peer == peer)) {
            continue;
        }
        if (t.phase == Phase::finished) {
            t.peer = peer;
            if (!t.peerTold) {
                sendTo(peer, cancelOf(t), t.id);
                t.peerTold = true;
            }
            continue;
        }
        if (t.present) {
            dropData(t); // Its old connection still lingers: it is done with.
        }
        t.peer = peer;
        t.present = true;
        if (t.sending) {
            t.steps.clear();
            t.unacked.clear();
            t.outstanding = 0;
            ++t.storeEpoch;
            t.synced = t.fetched && t.mode == RCS_FILE_FETCH_READ; // Asks no answer: it reads.
            if (t.phase == Phase::offered || t.phase == Phase::going) {
                offer(t, true); // It answers with where the transfer is (READ: it asks its reads again).
            }
        } else if (t.awaiting) {
            sendTo(peer, Fetch{t.id, t.publication, t.mode}, t.id); // The first may not have come.
        }
        refresh(t);
    }
}

void Hub::peerHello(rcs_viewer_id peer, std::span<const uint8_t> bytes) {
    const auto found = peers_.find(peer);
    if (found == peers_.end() || !found->second.hello) {
        return;
    }
    try {
        Hello said = decodeHello(bytes);
        said.end = found->second.hello->end; // Its run stays the one it joined with.
        found->second.hello = said;
        emitPeer(peer, said);
    } catch (const Error& error) {
        rcs_module_log(module_, RCS_LOG_WARNING, error.what());
    }
}

void Hub::peerLeft(rcs_viewer_id peer) {
    peers_.erase(peer);
    const auto now = Clock::now();
    for (auto& [id, publication] : publications_) {
        if (publication.peer == peer && publication.present) {
            publication.present = false;
            publication.absentSince = now;
        }
    }
    for (auto& [id, t] : transfers_) {
        if (t.peer != peer || !t.present) {
            continue;
        }
        t.present = false;
        t.absentSince = now;
        if (t.sending) { // What was asked of the store goes again from what the receiver says it has.
            t.steps.clear();
            t.unacked.clear();
            t.outstanding = 0;
            ++t.storeEpoch;
            t.synced = false;
            t.serving.clear(); // READ: the fetcher asks them again.
            t.pieceBytes = 0;
        } else {
            for (auto& [serial, piece] : t.pieces) {
                piece.asked = false; // READ: asked again once the sender offers it again.
            }
        }
        refresh(t);
    }
    wantTicks();
}

void Hub::sendTo(rcs_viewer_id peer, const PeerMessage& message, uint64_t transfer, bool data) {
    const auto found = peers_.find(peer);
    if (found == peers_.end() || !found->second.hello) {
        return; // Not connected: it learns where things are when it comes back.
    }
    Bytes bytes = encode(message);
    auto& waiting = found->second.waiting;
    if (waiting.empty()) {
        const rcs_result sent = rcs_module_send(module_, peer, bytes.data(), uint32_t(bytes.size()), 0, 0);
        if (sent == RCS_OK) {
            return;
        }
        if (sent != RCS_ERROR_QUEUE_FULL) {
            rcs_module_log(module_, RCS_LOG_WARNING,
                           ("A message to the other end did not go: " + std::string(rcs_result_message(sent))).c_str());
            return;
        }
    }
    found->second.waitingBytes += bytes.size();
    waiting.push_back({transfer, data, std::move(bytes)});
}

void Hub::room(rcs_viewer_id peer) {
    const auto found = peers_.find(peer);
    if (found == peers_.end()) {
        return;
    }
    auto& waiting = found->second.waiting;
    while (!waiting.empty()) {
        const auto& next = waiting.front().bytes;
        if (rcs_module_send(module_, peer, next.data(), uint32_t(next.size()), 0, 0) == RCS_ERROR_QUEUE_FULL) {
            return;
        }
        found->second.waitingBytes -= next.size();
        waiting.pop_front();
    }
    for (auto& [id, t] : transfers_) { // What held its transfers back has gone.
        if (t.sending && t.peer == peer) {
            pump(t);
        }
    }
}

void Hub::dropData(Transfer& t) {
    const auto found = peers_.find(t.peer);
    if (found == peers_.end()) {
        return;
    }
    auto& waiting = found->second.waiting;
    waiting.erase(std::remove_if(waiting.begin(), waiting.end(),
                                 [&](const Peer::Waiting& w) {
                                     const bool drop = w.transfer == t.id && w.data;
                                     found->second.waitingBytes -= drop ? w.bytes.size() : 0;
                                     return drop;
                                 }),
                  waiting.end());
}

void Hub::received(rcs_viewer_id peer, std::span<const uint8_t> data) {
    const auto found = peers_.find(peer);
    if (found == peers_.end() || !found->second.hello || !running_) {
        return;
    }
    PeerMessage message;
    try {
        message = decodePeer(data);
    } catch (const Error& error) {
        rcs_module_log(module_, RCS_LOG_WARNING, error.what());
        return;
    }
    std::visit(
        [&](auto& m) {
            using M = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<M, Offer>) {
                onOffer(peer, std::move(m));
            } else if constexpr (std::is_same_v<M, Answer>) {
                onAnswer(peer, m);
            } else if constexpr (std::is_same_v<M, Chunk>) {
                onChunk(peer, m);
            } else if constexpr (std::is_same_v<M, ItemEnd>) {
                onItemEnd(peer, m);
            } else if constexpr (std::is_same_v<M, Ack>) {
                onAck(peer, m);
            } else if constexpr (std::is_same_v<M, ItemDone>) {
                onItemDone(peer, m);
            } else if constexpr (std::is_same_v<M, Cancel>) {
                onCancel(peer, m);
            } else if constexpr (std::is_same_v<M, Pause>) {
                onPause(peer, m);
            } else if constexpr (std::is_same_v<M, Fetch>) {
                onFetch(peer, m);
            } else if constexpr (std::is_same_v<M, Read>) {
                onRead(peer, m);
            } else {
                onReadData(peer, m);
            }
        },
        message);
}

// ---- The store ----------------------------------------------------------------------------

bool Hub::needsMachine(const Transfer& t) const noexcept {
    // A READ fetch's fetcher writes nothing, and one waiting for its offer has nothing yet.
    const bool storeless = !t.sending && (t.awaiting || t.mode == RCS_FILE_FETCH_READ);
    return t.target == RCS_FILE_SYSTEM && !storeless && (t.phase == Phase::preparing || t.phase == Phase::going);
}

bool Hub::submit(Transfer& t, Request request, rcs_result* refused) {
    if (t.target == RCS_FILE_HOST) {
        if (!hostStore_) {
            hostStore_ = std::make_unique<HostStore>(module_, *this);
        }
        hostStore_->post(run_, std::move(request), t.host, t.peer);
        return true;
    }
    if (!machine_) {
        return false;
    }
    const Bytes bytes = encode(request);
    const rcs_result sent = rcs_module_send_machine(module_, bytes.data(), uint32_t(bytes.size()));
    if (sent != RCS_OK) {
        rcs_module_log(module_, RCS_LOG_WARNING,
                       ("The machine's half did not take a request: " + std::string(rcs_result_message(sent))).c_str());
        if (refused) {
            *refused = sent;
        }
    }
    return sent == RCS_OK;
}

void Hub::prepare(Transfer& t, Request request) {
    rcs_result refused = RCS_OK;
    if (!submit(t, std::move(request), &refused) && refused == RCS_ERROR_INVALID_ARGUMENT) {
        finish(t, RCS_FILE_FAILED, RCS_ERROR_INVALID_ARGUMENT,
               "The transfer is too large to hand to the module's machine half in one message", false, !t.sending);
    }
}

void Hub::machineJoined() {
    machine_ = true;
    for (auto& [id, t] : transfers_) {
        if (!needsMachine(t)) {
            continue;
        }
        if (t.phase == Phase::preparing) { // Asked of the half that left: again.
            if (t.sending && t.listing) {
                prepare(t, *t.listing);
            } else if (!t.sending) {
                PrepareRequest request{t.id, t.directory, t.conflict, t.token, t.resuming, t.keep, {}};
                for (const auto& item : t.items) {
                    request.items.push_back(item.spec);
                }
                prepare(t, std::move(request));
            }
        } else if (t.phase == Phase::going) {
            if (t.sending) {
                pump(t);
                servePieces(t);
            } else {
                answerWhenDue(t);
            }
        }
        refresh(t);
    }
}

void Hub::machineLeft() {
    machine_ = false;
    machineAwaySince_ = Clock::now();
    // Discards it was asked for get no answer from the half that left: the host hears so.
    for (const auto& [job, which] : purges_) {
        rcs_file_discard_event event{};
        std::copy(which.begin(), which.end(), event.token);
        event.result = RCS_ERROR_DEVICE;
        copyText(event.message, "The module's machine half left before it was done");
        post(0, RCS_FILE_DISCARDED, &event, sizeof(event));
    }
    purges_.clear();
    std::vector<uint64_t> settling; // Their end does not come from the half that left.
    for (auto& [id, t] : transfers_) {
        if (t.target == RCS_FILE_SYSTEM && t.settling) {
            settling.push_back(id);
        }
        if (!needsMachine(t) || t.phase != Phase::going) {
            continue;
        }
        ++t.storeEpoch;
        if (t.sending) {
            rewind(t);
            for (auto& piece : t.serving) {
                piece.asked = false; // READ: given again to the half that comes.
            }
        } else {
            // What was handed to it and not written is lost: the sender goes on
            // from what is written, in a new epoch (older chunks are dropped).
            t.pending.clear();
            t.unwritten = 0;
            for (auto& item : t.items) {
                if (!item.finished()) {
                    item.received = item.done;
                    item.ended = false;
                }
            }
            t.expected = nextExpected(t, 0);
            again(t);
        }
        refresh(t);
    }
    for (const uint64_t id : settling) {
        if (auto* t = find(id)) {
            if (t->tidying) {
                untidy(*t);
            }
            announce(*t);
        }
    }
    wantTicks();
}

void Hub::fromMachine(std::span<const uint8_t> data) {
    Reply reply;
    try {
        reply = decodeReply(data);
    } catch (const Error& error) {
        rcs_module_log(module_, RCS_LOG_ERROR,
                       (std::string("The machine's half sent what this end does not read: ") + error.what()).c_str());
        return;
    }
    onReply(std::move(reply));
}

void Hub::hostReply(uint64_t run, Reply reply) {
    if (running_ && run == run_) {
        onReply(std::move(reply));
    }
}

void Hub::onReply(Reply reply) {
    if (!running_) {
        return;
    }
    if (auto* r = std::get_if<Listed>(&reply)) {
        onListed(std::move(*r));
    } else if (const auto* r = std::get_if<Prepared>(&reply)) {
        onPrepared(*r);
    } else if (auto* r = std::get_if<Data>(&reply)) {
        onData(std::move(*r));
    } else if (const auto* r = std::get_if<ReadFailed>(&reply)) {
        onReadFailed(*r);
    } else if (const auto* r = std::get_if<Written>(&reply)) {
        onWritten(*r);
    } else if (const auto* r = std::get_if<WriteFailed>(&reply)) {
        onWriteFailed(*r);
    } else if (const auto* r = std::get_if<Finished>(&reply)) {
        onFinished(*r);
    } else if (const auto* r = std::get_if<Ended>(&reply)) {
        onEnded(*r);
    } else {
        onPurged(std::get<Purged>(reply));
    }
}

// ---- Sending ------------------------------------------------------------------------------

rcs_result Hub::checkItems(const rcs_file_item* given, uint32_t count, const char* what,
                           std::vector<std::pair<std::string, std::string>>& entries,
                           std::vector<ItemSpec>& specs) const {
    std::set<std::string> names;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& item = given[i];
        const std::string at = std::string(what) + ": item " + std::to_string(i) + ": ";
        std::string name = item.name ? item.name : "";
        if (item.name) {
            if (const auto problem = pathProblem(name)) {
                return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, (at + "name: " + *problem).c_str());
            }
        }
        if (settings_.target == RCS_FILE_SYSTEM) {
            if (!item.path || !utf8(item.path) || !files::absolute(item.path)) {
                return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                               (at + "path: a full path on the machine, UTF-8").c_str());
            }
            if (item.kind || item.size || item.modified_ns) {
                return rcs_set_last_call_error(
                    RCS_ERROR_INVALID_ARGUMENT,
                    (at + "kind, size and modified_ns are 0 with RCS_FILE_SYSTEM: the file system says").c_str());
            }
            entries.emplace_back(item.path, name);
            continue;
        }
        if (item.path || !item.name) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           (at + "with RCS_FILE_HOST, path NULL and a name").c_str());
        }
        if ((item.kind != RCS_FILE_REGULAR && item.kind != RCS_FILE_DIRECTORY) ||
            (item.kind == RCS_FILE_DIRECTORY && item.size)) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           (at + "kind REGULAR, or DIRECTORY of size 0").c_str());
        }
        if (!names.insert(name).second) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, (at + "\"" + name + "\" twice").c_str());
        }
        specs.push_back({item.kind, item.size, item.modified_ns, name});
    }
    return RCS_OK;
}

rcs_result Hub::hostTotal(const std::vector<ItemSpec>& specs, const Hello& there, const char* what,
                          uint64_t& total) const {
    try {
        total = totalBytes(specs);
    } catch (const Error&) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       (std::string(what) + ": the sizes add up past 2^64").c_str());
    }
    for (const uint64_t most : {settings_.max_offer_bytes, there.maxOfferBytes}) {
        if (most && total > most) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           (std::string(what) + ": " + std::to_string(total) + " bytes, more than " +
                                            std::to_string(most) + " (max_offer_bytes, this end's or the other's)")
                                               .c_str());
        }
    }
    Offer sized;
    sized.items = specs;
    if (encode(PeerMessage{sized}).size() > maxMessageBytes) {
        return rcs_set_last_call_error(
            RCS_ERROR_INVALID_ARGUMENT,
            (std::string(what) + ": too many items, or names too long, for one transfer").c_str());
    }
    return RCS_OK;
}

void Hub::hostItems(Transfer& t, std::vector<ItemSpec> specs) {
    auto context = std::make_shared<HostContext>();
    context->settings = settings_;
    context->transfer = t.id;
    context->token = t.token;
    context->items = specs;
    t.host = std::move(context);
    for (auto& spec : specs) {
        Item item;
        item.spec = std::move(spec);
        t.items.push_back(std::move(item));
    }
}

rcs_result Hub::send(const rcs_file_offer& o, uint64_t& transfer) {
    if (!running_) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "The session is not running");
    }
    if (o.item_size != sizeof(rcs_file_item) || !o.count || !o.items) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "rcs_file_offer: item_size sizeof(rcs_file_item), and 1.. items");
    }
    if ((role_ == RCS_ROLE_VIEW) != (o.viewer_id == 0)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       role_ == RCS_ROLE_VIEW
                                           ? "rcs_file_offer: a viewing session sends to its sharer, viewer_id 0"
                                           : "rcs_file_offer: a sharing session sends to one viewer: viewer_id");
    }
    if (o.count > settings_.max_items) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       ("rcs_file_offer: more than " + std::to_string(settings_.max_items) +
                                        " items (max_items)")
                                           .c_str());
    }
    const rcs_file_target target = settings_.target;
    ListRequest listing;
    std::vector<ItemSpec> specs;
    if (const rcs_result checked = checkItems(o.items, o.count, "rcs_file_offer", listing.entries, specs);
        checked != RCS_OK) {
        return checked;
    }
    if (!(settings_.flow & RCS_FILE_SEND)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "rcs_file_transfer_settings: flow has no RCS_FILE_SEND");
    }
    const auto found = peers_.find(o.viewer_id);
    if (found == peers_.end() || !found->second.hello) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       o.viewer_id ? "That viewer is not connected, or does not use file transfer"
                                                   : "The sharer is not connected, or does not use file transfer");
    }
    const Hello& there = *found->second.hello;
    if (!(there.flow & RCS_FILE_RECEIVE)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "The other end does not receive files (its flow has no RCS_FILE_RECEIVE)");
    }
    if (o.count > there.maxItems) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       ("The other end takes at most " + std::to_string(there.maxItems) +
                                        " items in one transfer")
                                           .c_str());
    }
    uint64_t total = 0;
    if (target == RCS_FILE_HOST) {
        if (const rcs_result sized = hostTotal(specs, there, "rcs_file_offer", total); sized != RCS_OK) {
            return sized;
        }
    } else if (!machine_) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "The module does not run on the machine: no agent is attached, or its process "
                                       "did not register rcsdk.file-transfer");
    }
    Token token{};
    bool resuming = false;
    if (std::any_of(std::begin(o.resume_token), std::end(o.resume_token), [](uint8_t b) { return b != 0; })) {
        std::copy(std::begin(o.resume_token), std::end(o.resume_token), token.begin());
        resuming = true;
        for (const auto& [id, t] : transfers_) {
            if (t.sending && t.token == token && (t.phase != Phase::finished || t.settling)) {
                return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                               ("Transfer " + std::to_string(id) + " of that token has not finished")
                                                   .c_str());
            }
        }
    } else {
        uint8_t random[32];
        rcs_generate_connection_key(random, sizeof(random));
        std::copy(random, random + token.size(), token.begin());
    }

    Transfer t;
    t.id = t.wire = ++serial_;
    t.sending = true;
    t.peer = o.viewer_id;
    t.peerEnd = there.end;
    t.present = true;
    t.token = token;
    t.resuming = resuming;
    t.target = target;
    t.total = total;
    if (target == RCS_FILE_HOST) {
        hostItems(t, std::move(specs));
        t.phase = Phase::offered;
    } else {
        listing.job = t.id;
        listing.maxItems = std::min(settings_.max_items, there.maxItems);
        t.listing = std::move(listing);
        t.phase = Phase::preparing;
    }
    auto& added = transfers_[t.id] = std::move(t);
    added.reported = stateOf(added);
    if (added.phase == Phase::offered) {
        offer(added, false);
    } else {
        prepare(added, *added.listing);
    }
    transfer = added.id;
    wantTicks();
    return RCS_OK;
}

void Hub::onListed(Listed listed) {
    Transfer* t = find(listed.job);
    if (!t || !t->sending || t->phase != Phase::preparing) {
        return;
    }
    t->listing.reset();
    if (listed.result != RCS_OK) { // A fetcher waits for it; a receiver never heard of it.
        finish(*t, RCS_FILE_FAILED, listed.result, listed.message, false, t->fetched);
        return;
    }
    t->total = totalBytes(listed.items);
    std::string limit;
    if (settings_.max_offer_bytes && t->total > settings_.max_offer_bytes) {
        limit = "more than this end's max_offer_bytes (" + std::to_string(settings_.max_offer_bytes) + ")";
    }
    if (const auto found = peers_.find(t->peer); found != peers_.end() && found->second.hello) {
        const auto& there = *found->second.hello;
        if (there.maxOfferBytes && t->total > there.maxOfferBytes) {
            limit = "more than the other end takes (" + std::to_string(there.maxOfferBytes) + ")";
        }
        if (listed.items.size() > there.maxItems) {
            limit = std::to_string(listed.items.size()) + " items, more than the other end takes";
        }
    }
    if (!limit.empty()) {
        finish(*t, RCS_FILE_FAILED, RCS_ERROR_INVALID_ARGUMENT,
               "The items hold " + std::to_string(t->total) + " bytes in " + std::to_string(listed.items.size()) +
                   " items: " + limit,
               false, t->fetched);
        return;
    }
    t->bases = std::move(listed.bases);
    for (size_t i = 0; i < listed.items.size(); ++i) {
        Item item;
        item.spec = std::move(listed.items[i]);
        item.entry = listed.sources[i].first;
        item.relative = std::move(listed.sources[i].second);
        t->items.push_back(std::move(item));
    }
    for (uint32_t i = 0; i < uint32_t(t->items.size()); ++i) {
        if (t->items[i].spec.kind == RCS_FILE_OTHER) {
            settle(*t, i, RCS_FILE_ITEM_SKIPPED, RCS_ERROR_UNSUPPORTED,
                   "A symbolic link, or something else that is not a file or a directory: not followed, not sent",
                   false);
        }
    }
    if (t->fetched) {
        offerFetched(*t);
        return;
    }
    t->phase = Phase::offered;
    if (t->present) {
        offer(*t, false);
    }
    refresh(*t);
}

void Hub::offerFetched(Transfer& t) {
    const bool reading = t.mode == RCS_FILE_FETCH_READ;
    t.phase = reading ? Phase::going : Phase::offered;
    t.synced = reading;
    if (t.present) {
        offer(t, false);
    }
    refresh(t);
}

void Hub::offer(Transfer& t, bool again) {
    Offer m;
    m.id = t.wire;
    m.again = again;
    m.resuming = t.resuming;
    m.paused = t.pausedHere;
    m.token = t.token;
    m.known = uint32_t(t.items.size());
    if (t.fetched) {
        m.fetch = t.fetchId;
        m.mode = t.mode;
    }
    for (uint32_t i = 0; i < uint32_t(t.items.size()); ++i) {
        if (!t.items[i].finished() && m.known == t.items.size()) {
            m.known = i;
        }
        m.items.push_back(t.items[i].spec);
    }
    sendTo(t.peer, m, t.id);
    t.offered = true;
}

void Hub::onAnswer(rcs_viewer_id peer, const Answer& a) {
    Transfer* t = outgoing(peer, a.id);
    if (!t || (t->phase != Phase::offered && t->phase != Phase::going)) {
        return;
    }
    if (t->mode == RCS_FILE_FETCH_READ) {
        return broken(*t, "an answer to a READ fetch, which its fetcher reads");
    }
    const auto count = uint32_t(t->items.size());
    if (a.first > count) {
        return broken(*t, "an answer past the items");
    }
    for (const auto& [item, offset] : a.starts) {
        if (item >= count || (offset != Answer::skipAll && offset > t->items[item].spec.size)) {
            return broken(*t, "an answer with an item out of range");
        }
    }
    const bool first = t->phase == Phase::offered;
    t->phase = Phase::going;
    t->synced = true;
    t->epoch = a.epoch;
    t->pausedThere = a.paused;
    // What the store was asked does not go, but stays in the window until it
    // comes back: answers, as often as they come, never have more read.
    for (auto& step : t->steps) {
        step.dropped = true;
    }
    t->unacked.clear();
    t->outstanding = 0;
    dropData(*t);
    for (uint32_t i = 0; i < count; ++i) {
        auto& item = t->items[i];
        item.skip = i < a.first;
        item.start = 0;
        item.endSent = false;
    }
    for (const auto& [i, offset] : a.starts) {
        auto& item = t->items[i];
        if (offset == Answer::skipAll) {
            item.skip = true;
        } else {
            item.start = offset;
            if (!item.finished()) {
                item.done = std::max(item.done, offset);
            }
            if (first) {
                t->resumed += offset;
            }
        }
    }
    t->cursor = 0;
    t->cursorOffset = count ? t->items[0].start : 0;
    t->allowance = chunkBytes;
    refresh(*t);
    pump(*t);
    checkComplete(*t);
}

ReadRequest Hub::readOf(const Transfer& t, uint32_t i, uint64_t offset, uint32_t length) const {
    const auto& item = t.items[i];
    const std::string base = t.target == RCS_FILE_SYSTEM ? t.bases.at(item.entry) : std::string();
    return {t.id,           t.storeEpoch,       i,      item.spec.path, base, item.relative,
            item.spec.size, item.spec.modified, offset, length};
}

void Hub::pump(Transfer& t) {
    if (!running_ || !t.sending || t.phase != Phase::going || !t.synced || !t.present || t.pausedHere ||
        t.pausedThere || (t.target == RCS_FILE_SYSTEM && !machine_) || t.mode == RCS_FILE_FETCH_READ) {
        return; // (A READ fetch's pieces go as the fetcher asks for them.)
    }
    const uint64_t rate = settings_.max_bytes_per_second;
    const auto count = uint32_t(t.items.size());
    // What this end holds for the peer (read or being read, and waiting for
    // its queue): within the window, whatever the peer acknowledges.
    const auto held = [&] {
        uint64_t bytes = peers_.count(t.peer) ? peers_.at(t.peer).waitingBytes : 0;
        for (const auto& step : t.steps) {
            bytes += step.length;
        }
        return bytes;
    };
    while (t.cursor < count && t.outstanding < windowBytes && held() < windowBytes && (!rate || t.allowance > 0)) {
        auto& item = t.items[t.cursor];
        if (item.skip || item.spec.kind == RCS_FILE_OTHER || item.endSent || (item.finished() && !item.failedHere)) {
            ++t.cursor;
            t.cursorOffset = t.cursor < count ? t.items[t.cursor].start : 0;
            continue;
        }
        if (item.failedHere) {
            pushEnd(t, item.result, item.message);
            continue;
        }
        if (item.spec.kind == RCS_FILE_REGULAR && t.cursorOffset < item.spec.size) {
            const auto length = uint32_t(std::min<uint64_t>(chunkBytes, item.spec.size - t.cursorOffset));
            if (!submit(t, readOf(t, t.cursor, t.cursorOffset, length))) {
                break;
            }
            t.steps.push_back({t.cursor, t.cursorOffset, length, RCS_OK, {}, false});
            t.unacked.push_back({t.cursor, t.cursorOffset + length, length});
            t.outstanding += length;
            if (rate) {
                t.allowance -= double(length);
            }
            item.state = RCS_FILE_ITEM_ACTIVE;
            t.cursorOffset += length;
            continue;
        }
        pushEnd(t, RCS_OK, {});
    }
    flushSteps(t);
}

void Hub::pushEnd(Transfer& t, rcs_result result, const std::string& message) {
    t.steps.push_back({t.cursor, 0, 0, result, message, false});
    t.items[t.cursor].endSent = true;
    ++t.cursor;
    t.cursorOffset = t.cursor < t.items.size() ? t.items[t.cursor].start : 0;
}

void Hub::flushSteps(Transfer& t) {
    while (!t.steps.empty() && t.steps.front().length == 0) {
        const Step step = std::move(t.steps.front());
        t.steps.pop_front();
        if (!step.dropped) {
            sendTo(t.peer, ItemEnd{t.wire, t.epoch, step.item, step.result, step.message}, t.id, true);
        }
    }
    checkComplete(t);
}

void Hub::rewind(Transfer& t) {
    // The pieces asked and not back are the newest of unacked, one each (a
    // dropped one's went when its item ended, or an answer came).
    size_t asked = 0;
    for (const auto& step : t.steps) {
        asked += step.length && !step.dropped ? 1 : 0;
    }
    for (; asked && !t.unacked.empty(); --asked) {
        t.outstanding -= t.unacked.back().length;
        t.unacked.pop_back();
    }
    const auto from = std::find_if(t.steps.begin(), t.steps.end(), [](const Step& step) { return !step.dropped; });
    if (from != t.steps.end()) {
        t.cursor = from->item;
        t.cursorOffset = from->length ? from->offset : t.items[from->item].start;
        for (auto step = from; step != t.steps.end(); ++step) {
            t.items[step->item].endSent = false;
        }
        if (!from->length && t.items[from->item].spec.kind == RCS_FILE_REGULAR) {
            t.cursorOffset = t.items[from->item].spec.size; // All of it went: its end goes again.
        }
    }
    t.steps.clear();
}

void Hub::abandon(Transfer& t, uint32_t item) {
    for (auto& step : t.steps) {
        if (step.item == item) {
            step.dropped = true;
        }
    }
    for (auto at = t.unacked.begin(); at != t.unacked.end();) {
        if (at->item == item) {
            t.outstanding -= at->length;
            at = t.unacked.erase(at);
        } else {
            ++at;
        }
    }
    if (t.cursor == item) {
        ++t.cursor;
        t.cursorOffset = t.cursor < t.items.size() ? t.items[t.cursor].start : 0;
    }
}

void Hub::onData(Data data) {
    Transfer* t = find(data.transfer);
    if (!t || !t->sending || t->phase != Phase::going || data.epoch != t->storeEpoch) {
        return;
    }
    if (t->mode == RCS_FILE_FETCH_READ) { // The store answers the pieces given to it in order.
        if (t->serving.empty() || !t->serving.front().asked || t->serving.front().item != data.item ||
            t->serving.front().offset != data.offset || t->serving.front().length != data.data.size()) {
            finish(*t, RCS_FILE_FAILED, RCS_ERROR_INTERNAL, "The store answered out of order", false, true);
            return;
        }
        const Piece piece = t->serving.front();
        t->serving.pop_front();
        t->pieceBytes -= piece.length;
        sendTo(t->peer, ReadData{t->wire, piece.serial, RCS_OK, {}, data.data}, t->id, true);
        t->transferred += piece.length;
        advance(*t, piece.item, piece.offset, piece.length);
        return;
    }
    if (t->steps.empty() || !t->steps.front().length || t->steps.front().item != data.item ||
        t->steps.front().offset != data.offset || t->steps.front().length != data.data.size()) {
        finish(*t, RCS_FILE_FAILED, RCS_ERROR_INTERNAL, "The store answered out of order", false, true);
        return;
    }
    const bool dropped = t->steps.front().dropped;
    t->steps.pop_front();
    if (!dropped) {
        sendTo(t->peer, Chunk{t->wire, t->epoch, data.item, data.offset, data.data}, t->id, true);
        t->transferred += data.data.size();
    }
    flushSteps(*t);
    pump(*t);
}

void Hub::onReadFailed(const ReadFailed& failed) {
    Transfer* t = find(failed.transfer);
    if (!t || !t->sending || t->phase != Phase::going || failed.epoch != t->storeEpoch) {
        return;
    }
    if (t->mode == RCS_FILE_FETCH_READ) {
        if (t->serving.empty() || !t->serving.front().asked || t->serving.front().item != failed.item) {
            finish(*t, RCS_FILE_FAILED, RCS_ERROR_INTERNAL, "The store answered out of order", false, true);
            return;
        }
        const Piece piece = t->serving.front();
        t->serving.pop_front();
        t->pieceBytes -= piece.length;
        sendTo(t->peer, ReadData{t->wire, piece.serial, failed.result, failed.message, {}}, t->id, true);
        if (!t->items[piece.item].finished()) { // It changed, or cannot be read: so it ends at both ends.
            settle(*t, piece.item, RCS_FILE_ITEM_FAILED, failed.result, failed.message, false);
        }
        return;
    }
    if (t->steps.empty() || !t->steps.front().length || t->steps.front().item != failed.item) {
        finish(*t, RCS_FILE_FAILED, RCS_ERROR_INTERNAL, "The store answered out of order", false, true);
        return;
    }
    const bool dropped = t->steps.front().dropped;
    t->steps.pop_front();
    auto& item = t->items[failed.item];
    if (!dropped && !item.finished()) { // (A dropped read is asked again, if it is needed.)
        // The receiver learns it from the item's ITEM_END, in its place.
        conclude(*t, failed.item, RCS_FILE_ITEM_FAILED, failed.result, failed.message);
        item.failedHere = true;
        bool endQueued = false;
        for (auto& step : t->steps) {
            if (step.item == failed.item) {
                if (step.length) {
                    step.dropped = true;
                } else {
                    step.result = failed.result;
                    step.message = failed.message;
                    endQueued = true;
                }
            }
        }
        for (auto at = t->unacked.begin(); at != t->unacked.end();) {
            if (at->item == failed.item) {
                t->outstanding -= at->length;
                at = t->unacked.erase(at);
            } else {
                ++at;
            }
        }
        if (!endQueued && t->cursor == failed.item) {
            pushEnd(*t, failed.result, failed.message);
        }
        emitItem(*t, failed.item);
    }
    flushSteps(*t);
    pump(*t);
}

void Hub::onAck(rcs_viewer_id peer, const Ack& ack) {
    Transfer* t = outgoing(peer, ack.id);
    if (!t || t->phase != Phase::going) {
        return;
    }
    if (ack.item >= t->items.size()) {
        return broken(*t, "an acknowledgement of an item out of range");
    }
    auto& item = t->items[ack.item];
    if (!item.finished()) {
        item.done = std::max(item.done, std::min(ack.written, item.spec.size));
    }
    while (!t->unacked.empty() && (t->unacked.front().item < ack.item ||
                                   (t->unacked.front().item == ack.item && t->unacked.front().end <= ack.written))) {
        t->outstanding -= t->unacked.front().length;
        t->unacked.pop_front();
    }
    pump(*t);
}

void Hub::onItemDone(rcs_viewer_id peer, const ItemDone& done) {
    Transfer* t = outgoing(peer, done.id);
    if (!t || t->phase != Phase::going) {
        return;
    }
    if (done.item >= t->items.size()) {
        return broken(*t, "an outcome of an item out of range");
    }
    auto& item = t->items[done.item];
    if (item.finished()) {
        return; // Said again after a reconnect, or the item failed here.
    }
    if (done.state == RCS_FILE_ITEM_COMPLETED) {
        item.done = item.spec.size;
        if (done.earlier) {
            t->resumed += item.spec.size;
        }
    }
    abandon(*t, done.item);
    settle(*t, done.item, done.state, done.result, done.message, false);
    flushSteps(*t);
    pump(*t);
}

// ---- Receiving ----------------------------------------------------------------------------

void Hub::onOffer(rcs_viewer_id peer, Offer m) {
    if (Transfer* t = incoming(peer, m.id)) { // Offered again, the peer back: where it is.
        if (t->phase == Phase::finished) {
            sendTo(peer, cancelOf(*t), t->id);
            t->peerTold = true;
            return;
        }
        t->pausedThere = m.paused;
        if (t->mode == RCS_FILE_FETCH_READ) {
            sendPieces(*t); // Those the sender had not answered when the connection ended.
        } else if (t->phase == Phase::going) {
            for (uint32_t i = m.known; i < t->items.size(); ++i) {
                const auto& item = t->items[i];
                if (item.finished() && item.spec.kind != RCS_FILE_OTHER) {
                    sendTo(peer, ItemDone{t->wire, i, item.state, item.result, item.message, item.earlier}, t->id);
                }
            }
            // From what the store took, once it wrote it: what it still has
            // stays counted, however often an offer comes again.
            again(*t);
        }
        refresh(*t);
        return;
    }
    if (m.fetch) {
        return onFetchOffer(peer, std::move(m));
    }
    const auto refuse = [&](rcs_result result, const std::string& why) {
        rcs_module_log(module_, RCS_LOG_INFO, ("Refused an offer: " + why).c_str());
        sendTo(peer, Cancel{false, false, m.id, RCS_FILE_REJECTED, result, why});
    };
    if (!(settings_.flow & RCS_FILE_RECEIVE)) {
        return refuse(RCS_ERROR_INVALID_STATE, "This end does not receive files (its flow has no RCS_FILE_RECEIVE)");
    }
    if (m.items.size() > settings_.max_items) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      "This end takes at most " + std::to_string(settings_.max_items) + " items in one transfer");
    }
    const uint64_t total = totalBytes(m.items);
    if (settings_.max_offer_bytes && total > settings_.max_offer_bytes) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "This end takes at most " +
                                                      std::to_string(settings_.max_offer_bytes) +
                                                      " bytes in one transfer");
    }
    // The offers waiting for this end's host, the peer's and all: within the
    // budget, or this one is refused before anything of it is kept.
    uint32_t waiting = 0;
    uint64_t itemsWaiting = 0, allWaiting = 0;
    for (const auto& [id, other] : transfers_) {
        if (!other.sending && other.phase == Phase::offered) {
            allWaiting += other.items.size();
            if (other.peer == peer) {
                ++waiting;
                itemsWaiting += other.items.size();
            }
        }
    }
    if (waiting >= settings_.max_pending_offers) {
        return refuse(RCS_ERROR_BUSY, "This end takes at most " + std::to_string(settings_.max_pending_offers) +
                                          " offers from one end waiting for its host's decision (max_pending_offers)");
    }
    if (itemsWaiting + m.items.size() > settings_.max_pending_items) {
        return refuse(RCS_ERROR_BUSY, "This end takes at most " + std::to_string(settings_.max_pending_items) +
                                          " items in offers from one end waiting for its host's decision "
                                          "(max_pending_items): " +
                                          std::to_string(itemsWaiting) + " wait, this offer has " +
                                          std::to_string(m.items.size()));
    }
    if (allWaiting + m.items.size() > settings_.max_pending_items_total) {
        return refuse(RCS_ERROR_BUSY, "This end takes at most " + std::to_string(settings_.max_pending_items_total) +
                                          " items in offers from all ends waiting for its host's decision "
                                          "(max_pending_items_total)");
    }
    Transfer t;
    t.id = ++serial_;
    t.wire = m.id;
    t.peer = peer;
    t.peerEnd = peers_.at(peer).hello->end;
    t.present = true;
    t.token = m.token;
    t.resuming = m.resuming;
    t.target = settings_.target;
    t.phase = Phase::offered;
    t.pausedThere = m.paused;
    t.total = total;
    t.deadline = Clock::now() + std::chrono::milliseconds(settings_.offer_timeout_ms);
    for (auto& spec : m.items) {
        Item item;
        item.spec = std::move(spec);
        t.items.push_back(std::move(item));
    }
    if (t.target == RCS_FILE_HOST) {
        auto context = std::make_shared<HostContext>();
        context->settings = settings_;
        context->transfer = t.id;
        context->token = t.token;
        for (const auto& item : t.items) {
            context->items.push_back(item.spec);
        }
        t.host = std::move(context);
    }
    auto& added = transfers_[t.id] = std::move(t);
    incoming_[{added.peerEnd, added.wire}] = added.id;
    added.reported = stateOf(added);
    emitOffer(added);
    wantTicks();
}

rcs_result Hub::accept(uint64_t transfer, const char* directory, rcs_file_conflict conflict) {
    Transfer* t = find(transfer);
    if (!t || t->sending || t->phase != Phase::offered) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       ("Transfer " + std::to_string(transfer) + " is not one offered to this end")
                                           .c_str());
    }
    // A token names one transfer: two of it would write the same partial files.
    for (const auto& [id, other] : transfers_) {
        if (!other.sending && other.token == t->token && other.phase != Phase::offered &&
            (other.phase != Phase::finished || other.settling)) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                           ("Transfer " + std::to_string(id) +
                                            ", received with the same token, has not finished")
                                               .c_str());
        }
    }
    if (t->target == RCS_FILE_HOST) {
        if (directory) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           "With RCS_FILE_HOST the host's callbacks write: directory is NULL");
        }
    } else { // With HOST the conflict rule is the host's own business.
        if (!directory || !utf8(directory) || !files::absolute(directory)) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           "directory: a full path on the machine, UTF-8");
        }
        if (conflict < RCS_FILE_CONFLICT_OVERWRITE || conflict > RCS_FILE_CONFLICT_FAIL) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "on_conflict: an rcs_file_conflict");
        }
        if (!machine_) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                           "The module does not run on the machine: no agent is attached, or its "
                                           "process did not register rcsdk.file-transfer");
        }
        t->directory = directory;
    }
    t->conflict = conflict;
    t->keep = settings_.keep_partial != 0;
    t->phase = Phase::preparing;
    PrepareRequest request{t->id, t->directory, conflict, t->token, t->resuming, t->keep, {}};
    for (const auto& item : t->items) {
        request.items.push_back(item.spec);
    }
    prepare(*t, std::move(request));
    refresh(*t);
    return RCS_OK;
}

rcs_result Hub::reject(uint64_t transfer, const char* reason) {
    Transfer* t = find(transfer);
    if (!t || t->sending || t->phase != Phase::offered) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       ("Transfer " + std::to_string(transfer) + " is not one offered to this end")
                                           .c_str());
    }
    std::string why = reason ? reason : "";
    if (!utf8(why)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "reason: UTF-8");
    }
    if (why.size() > 255) {
        char cut[256];
        copyText(cut, why);
        why = cut;
    }
    finish(*t, RCS_FILE_REJECTED, RCS_ERROR_CANCELLED, why.empty() ? "The receiving host rejected it" : why, false,
           true);
    return RCS_OK;
}

void Hub::onPrepared(const Prepared& prepared) {
    Transfer* t = find(prepared.transfer);
    if (!t || t->sending || t->phase != Phase::preparing) {
        return;
    }
    if (prepared.result != RCS_OK) {
        finish(*t, RCS_FILE_FAILED, prepared.result, prepared.message, false, true);
        return;
    }
    const auto count = uint32_t(t->items.size());
    for (const auto& start : prepared.starts) {
        if (start.item >= count || t->items[start.item].spec.kind == RCS_FILE_OTHER ||
            (start.what != Start::failed && t->items[start.item].spec.kind != RCS_FILE_REGULAR) ||
            (start.what == Start::resume && start.offset > t->items[start.item].spec.size)) {
            finish(*t, RCS_FILE_FAILED, RCS_ERROR_PROTOCOL, "The store answered with an item out of range", false,
                   true);
            return;
        }
    }
    std::vector<bool> settled(count);
    for (const auto& start : prepared.starts) {
        auto& item = t->items[start.item];
        switch (start.what) {
        case Start::resume:
            item.start = item.received = item.done = start.offset;
            t->resumed += start.offset;
            break;
        case Start::done:
            conclude(*t, start.item, RCS_FILE_ITEM_COMPLETED, RCS_OK, "Completed by an earlier run of the transfer");
            item.done = item.spec.size;
            item.saved = t->target == RCS_FILE_SYSTEM ? start.text : std::string();
            item.earlier = true;
            t->resumed += item.spec.size;
            settled[start.item] = true;
            break;
        case Start::skip:
            conclude(*t, start.item, RCS_FILE_ITEM_SKIPPED, RCS_OK, start.text);
            settled[start.item] = true;
            break;
        case Start::failed:
            conclude(*t, start.item, RCS_FILE_ITEM_FAILED, start.result, start.text);
            settled[start.item] = true;
            break;
        }
    }
    t->phase = Phase::going;
    t->epoch = 1;
    answer(*t); // The settled ones need nothing: their outcomes follow.
    for (uint32_t i = 0; i < count; ++i) {
        auto& item = t->items[i];
        if (item.spec.kind == RCS_FILE_OTHER) {
            conclude(*t, i, RCS_FILE_ITEM_SKIPPED, RCS_ERROR_UNSUPPORTED,
                     "Not a file or a directory at the sending end (a symbolic link...): not sent");
            emitItem(*t, i);
        } else if (settled[i]) {
            sendTo(t->peer, ItemDone{t->wire, i, item.state, item.result, item.message, item.earlier}, t->id);
            emitItem(*t, i);
        }
    }
    refresh(*t);
    checkComplete(*t);
}

void Hub::again(Transfer& t) {
    if (!t.answerDue) {
        ++t.epoch;
    }
    t.answerDue = true;
    answerWhenDue(t);
}

void Hub::answerWhenDue(Transfer& t) {
    if (t.answerDue && t.present && !t.unwritten && (t.target == RCS_FILE_HOST || machine_)) {
        answer(t);
    }
}

void Hub::took(Transfer& t) {
    if (!t.pending.empty()) {
        t.unwritten -= t.pending.front();
        t.pending.pop_front();
    }
}

void Hub::answer(Transfer& t) {
    // The sender's window starts again from here, with nothing of this end's
    // with the store: what it sends now is all the store gets.
    t.answerDue = false;
    Answer a;
    a.id = t.wire;
    a.epoch = t.epoch;
    a.paused = t.pausedHere;
    a.first = nextExpected(t, 0);
    for (uint32_t i = a.first; i < t.items.size(); ++i) {
        const auto& item = t.items[i];
        if (!needs(item)) {
            a.starts.emplace_back(i, Answer::skipAll);
        } else if (item.received) {
            a.starts.emplace_back(i, item.received);
        }
    }
    t.expected = a.first;
    sendTo(t.peer, a, t.id);
}

uint32_t Hub::nextExpected(const Transfer& t, uint32_t from) const {
    for (uint32_t i = from; i < t.items.size(); ++i) {
        if (needs(t.items[i])) {
            return i;
        }
    }
    return uint32_t(t.items.size());
}

void Hub::onChunk(rcs_viewer_id peer, const Chunk& c) {
    Transfer* t = incoming(peer, c.id);
    if (!t || t->phase != Phase::going || c.epoch != t->epoch || t->answerDue) {
        return; // Ended, or of an epoch before the newest answer (or before it went).
    }
    if (c.item >= t->items.size()) {
        return broken(*t, "a piece of an item out of range");
    }
    auto& item = t->items[c.item];
    if (item.finished() || item.ended) {
        return; // It failed or was skipped here meanwhile.
    }
    if (c.item != t->expected || item.spec.kind != RCS_FILE_REGULAR || c.offset != item.received ||
        c.data.size() > item.spec.size - c.offset) {
        return broken(*t, "a piece out of order");
    }
    // The store holds at most the sender's window (and the piece that passed
    // it) not written yet. A sender that went on regardless (as the pieces
    // of an item that failed were still being written) goes on from what
    // the store took, once it wrote it: nothing more waits for the store.
    if (t->unwritten + c.data.size() > windowBytes + chunkBytes) {
        return again(*t);
    }
    WriteRequest write{t->id,    t->storeEpoch, c.item,      t->directory, item.spec,
                       t->token, t->keep,       !item.begun, c.offset,     Bytes(c.data.begin(), c.data.end())};
    if (!submit(*t, std::move(write))) {
        return; // The store left: the sender goes on from what is written once it is back.
    }
    item.begun = true;
    item.received += c.data.size();
    item.state = RCS_FILE_ITEM_ACTIVE;
    t->transferred += c.data.size();
    t->pending.push_back(uint32_t(c.data.size()));
    t->unwritten += c.data.size();
}

void Hub::onItemEnd(rcs_viewer_id peer, const ItemEnd& e) {
    Transfer* t = incoming(peer, e.id);
    if (!t || t->phase != Phase::going || e.epoch != t->epoch || t->answerDue) {
        return;
    }
    if (e.item >= t->items.size()) {
        return broken(*t, "the end of an item out of range");
    }
    auto& item = t->items[e.item];
    if (item.finished() || item.ended) {
        return;
    }
    if (e.item != t->expected) {
        return broken(*t, "the end of an item out of order");
    }
    if (e.result != RCS_OK) { // It could not be read there.
        settle(*t, e.item, RCS_FILE_ITEM_FAILED, e.result, e.message, false);
        if (item.received && t->target == RCS_FILE_SYSTEM) {
            submit(*t, DiscardRequest{t->id, t->directory, t->token, false, {item.spec.path}});
        }
        t->expected = nextExpected(*t, e.item + 1);
        checkComplete(*t);
        return;
    }
    if (item.spec.kind == RCS_FILE_REGULAR && item.received != item.spec.size) {
        return broken(*t, "the end of an item not all of which came");
    }
    if (!submit(*t, FinishRequest{t->id, t->storeEpoch, e.item, t->directory, item.spec, t->conflict, t->token,
                                  t->keep})) {
        return;
    }
    item.ended = true;
    item.state = RCS_FILE_ITEM_ACTIVE;
    t->expected = nextExpected(*t, e.item + 1);
}

void Hub::onWritten(const Written& written) {
    Transfer* t = find(written.transfer);
    if (!t || t->sending || t->phase != Phase::going || written.epoch != t->storeEpoch ||
        written.item >= t->items.size()) {
        return;
    }
    took(*t);
    if (auto& item = t->items[written.item]; !item.finished()) {
        item.done = written.offset;
        if (t->present) {
            sendTo(t->peer, Ack{t->wire, written.item, written.offset}, t->id);
        }
    }
    answerWhenDue(*t);
}

void Hub::onWriteFailed(const WriteFailed& failed) {
    Transfer* t = find(failed.transfer);
    if (!t || t->sending || t->phase != Phase::going || failed.epoch != t->storeEpoch ||
        failed.item >= t->items.size()) {
        return;
    }
    took(*t);
    if (!t->items[failed.item].finished()) {
        const auto& item = t->items[failed.item];
        settle(*t, failed.item, RCS_FILE_ITEM_FAILED, failed.result, failed.message, true);
        if (t->target == RCS_FILE_SYSTEM) {
            submit(*t, DiscardRequest{t->id, t->directory, t->token, false, {item.spec.path}});
        }
        if (t->expected == failed.item) {
            t->expected = nextExpected(*t, failed.item + 1);
        }
        checkComplete(*t);
    }
    if (t->phase == Phase::going) {
        answerWhenDue(*t);
    }
}

void Hub::onFinished(const Finished& finished) {
    Transfer* t = find(finished.transfer);
    if (!t || t->sending || t->phase != Phase::going || finished.epoch != t->storeEpoch ||
        finished.item >= t->items.size() || t->items[finished.item].finished()) {
        return;
    }
    auto& item = t->items[finished.item];
    if (finished.state == RCS_FILE_ITEM_COMPLETED) {
        item.done = item.spec.size;
        item.saved = finished.saved;
    }
    settle(*t, finished.item, finished.state, finished.result, finished.message, true);
    if (finished.whole) {
        finish(*t, RCS_FILE_FAILED, finished.result, finished.message, false, true);
        return;
    }
    checkComplete(*t);
}

// ---- Either end ---------------------------------------------------------------------------

void Hub::onCancel(rcs_viewer_id peer, const Cancel& c) {
    Transfer* t = nullptr;
    if (!c.fetch) {
        t = c.sending ? incoming(peer, c.id) : outgoing(peer, c.id);
    } else if (c.sending) { // About this end's fetch, before its offer came.
        t = find(c.id);
        t = t && !t->sending && t->fetched && t->awaiting && t->peer == peer ? t : nullptr;
    } else if (const auto found = peers_.find(peer); found != peers_.end() && found->second.hello) {
        const auto fetched = fetches_.find({found->second.hello->end, c.id});
        t = fetched == fetches_.end() ? nullptr : find(fetched->second);
    }
    if (!t || t->phase == Phase::finished) {
        return;
    }
    if (c.end == RCS_FILE_COMPLETED) { // The receiver's, about a transfer of this end's.
        if (t->mode == RCS_FILE_FETCH_READ) {
            concludeReads(*t); // It closed it, having read what it read.
        } else {
            // The receiver finished it before this end heard of its last
            // items (it says so when this end comes back and offers it
            // again): every item arrived.
            for (uint32_t i = 0; i < uint32_t(t->items.size()); ++i) {
                if (auto& item = t->items[i]; !item.finished()) {
                    item.done = item.spec.size;
                    settle(*t, i, RCS_FILE_ITEM_COMPLETED, RCS_OK, {}, false);
                }
            }
        }
    }
    finish(*t, c.end, c.result, c.message, true, false);
}

void Hub::onPause(rcs_viewer_id peer, const Pause& p) {
    Transfer* t = p.sending ? incoming(peer, p.id) : outgoing(peer, p.id);
    if (!t || t->phase == Phase::finished) {
        return;
    }
    t->pausedThere = p.paused;
    refresh(*t);
    pump(*t);
    sendPieces(*t);
}

rcs_result Hub::cancel(uint64_t transfer) {
    Transfer* t = find(transfer);
    if (!t || t->phase == Phase::finished) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       ("Transfer " + std::to_string(transfer) + " is not going").c_str());
    }
    finish(*t, RCS_FILE_CANCELLED, RCS_ERROR_CANCELLED, "Cancelled by its host", false, true);
    return RCS_OK;
}

rcs_result Hub::pause(uint64_t transfer, uint32_t paused) {
    if (paused > 1) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "paused: 0 or 1");
    }
    Transfer* t = find(transfer);
    if (!t || t->phase == Phase::finished) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       ("Transfer " + std::to_string(transfer) + " is not going").c_str());
    }
    if (t->pausedHere != (paused != 0)) {
        t->pausedHere = paused != 0;
        if (t->present && !t->awaiting) { // (A fetch's answer says it once its offer came.)
            sendTo(t->peer, Pause{t->sending, t->wire, t->pausedHere}, t->id);
        }
        refresh(*t);
        pump(*t);
        sendPieces(*t);
        servePieces(*t);
    }
    return RCS_OK;
}

void Hub::conclude(Transfer& t, uint32_t i, rcs_file_item_state state, rcs_result result,
                   const std::string& message) {
    auto& item = t.items[i];
    t.finishedItems += item.finished() ? 0 : 1;
    item.state = state;
    item.result = result;
    item.message = message;
}

void Hub::settle(Transfer& t, uint32_t i, rcs_file_item_state state, rcs_result result, const std::string& message,
                 bool tellPeer) {
    conclude(t, i, state, result, message);
    if (tellPeer) {
        sendTo(t.peer, ItemDone{t.wire, i, state, result, message, false}, t.id);
    }
    emitItem(t, i);
}

void Hub::checkComplete(Transfer& t) {
    // (A READ fetch ends as its fetcher closes it.)
    if (t.phase != Phase::going || (t.sending && !t.steps.empty()) || t.finishedItems < t.items.size() ||
        t.mode == RCS_FILE_FETCH_READ) {
        return;
    }
    finish(t, RCS_FILE_COMPLETED, RCS_OK, {}, false, false);
}

Cancel Hub::cancelOf(const Transfer& t) const {
    if (t.fetched && (t.sending ? !t.offered : t.awaiting)) {
        return {t.sending, true, t.fetchId, t.end, t.result, t.message};
    }
    return {t.sending, false, t.wire, t.end, t.result, t.message};
}

void Hub::finish(Transfer& t, rcs_file_end end, rcs_result result, const std::string& message, bool byPeer,
                 bool tellPeer) {
    if (t.phase == Phase::finished) {
        return;
    }
    const Phase was = t.phase;
    t.phase = Phase::finished;
    t.end = end;
    t.result = result;
    t.message = message;
    t.byPeer = byPeer;
    t.peerTold = true;
    if (tellPeer) {
        if (t.present) {
            sendTo(t.peer, cancelOf(t), t.id);
        } else {
            t.peerTold = false; // It learns when it comes back within the run.
        }
    }
    if (end != RCS_FILE_COMPLETED) {
        dropData(t);
    }
    t.steps.clear();
    t.unacked.clear();
    t.outstanding = 0;
    t.serving.clear();
    // A READ fetch's reads not answered are answered now, with how it ended.
    const rcs_result unanswered = result == RCS_OK ? RCS_ERROR_CANCELLED : result;
    for (const auto& [serial, piece] : t.pieces) {
        if (const auto onRead = t.fetcher.onRead) {
            notify([onRead, transfer = t.id, tag = piece.tag, unanswered, user = t.fetcher.userData] {
                onRead(transfer, tag, unanswered, nullptr, 0, user);
            });
        }
    }
    t.pieces.clear();
    t.pieceBytes = 0;
    // Its store, asked something of it (an offered transfer was not, nor a
    // fetch this end makes before its offer came or that reads), does what
    // the end asks after that. Receive: what went and was written (an
    // earlier run's too) stays only as far as keep_partial and the reason say.
    const bool storeless = !t.sending && t.fetched && (t.awaiting || t.mode == RCS_FILE_FETCH_READ);
    std::optional<Request> last;
    if (was != Phase::offered && !storeless) {
        last = CloseRequest{t.id};
    }
    if (!t.sending && was == Phase::going && !storeless) {
        if (end == RCS_FILE_COMPLETED) {
            if (t.target == RCS_FILE_SYSTEM) {
                CompleteRequest complete{t.id, t.directory, t.token, {}};
                for (const auto& item : t.items) {
                    if (item.spec.kind == RCS_FILE_DIRECTORY && item.state == RCS_FILE_ITEM_COMPLETED) {
                        complete.directories.emplace_back(item.spec.path, item.spec.modified);
                    }
                }
                last = std::move(complete);
            }
        } else {
            std::vector<std::string> unfinished;
            bool written = false;
            for (const auto& item : t.items) {
                if (item.spec.kind == RCS_FILE_REGULAR && !item.finished()) {
                    unfinished.push_back(item.spec.path);
                    written = written || item.received || item.start;
                }
            }
            t.partialKept = result == RCS_ERROR_DISCONNECTED && written && (t.keep || t.target == RCS_FILE_HOST);
            if (t.target == RCS_FILE_SYSTEM && !t.partialKept) {
                last = DiscardRequest{t.id, t.directory, t.token, true, std::move(unfinished)};
            }
        }
    }
    // A record to delete, or directories' times to set: more than closing files.
    t.tidying = last && !std::holds_alternative<CloseRequest>(*last) && (t.keep || end == RCS_FILE_COMPLETED);
    t.settling = last && submit(t, std::move(*last));
    if (!t.settling) {
        if (t.tidying) {
            untidy(t);
        }
        announce(t);
    }
}

void Hub::onEnded(const Ended& ended) {
    if (Transfer* t = find(ended.transfer); t && t->settling) {
        announce(*t);
    }
}

void Hub::announce(Transfer& t) {
    t.settling = false;
    emitDone(t);
    refresh(t);
    if (const auto onEnded = t.fetcher.onEnded) { // Its last call.
        notify([onEnded, transfer = t.id, result = t.result, message = t.message, user = t.fetcher.userData] {
            onEnded(transfer, result, message.c_str(), user);
        });
    }
    finished_.push_back(t.id);
    finishedItems_ += t.items.size();
    // The latest finishedKept, and no more items than the offers waiting may hold together.
    while (finished_.size() > finishedKept ||
           (finished_.size() > 1 && finishedItems_ > settings_.max_pending_items_total)) {
        const uint64_t oldest = finished_.front();
        finished_.pop_front();
        if (const auto found = transfers_.find(oldest); found != transfers_.end()) {
            const auto& old = found->second;
            finishedItems_ -= old.items.size();
            if (!old.sending && !old.awaiting) {
                incoming_.erase({old.peerEnd, old.wire});
            } else if (old.sending && old.fetched) {
                fetches_.erase({old.peerEnd, old.fetchId});
            }
            transfers_.erase(found);
        }
    }
}

void Hub::untidy(const Transfer& t) {
    // Where is the host's to see (rcs_file_get_transfer): a log may go elsewhere.
    std::string why = "Transfer " + std::to_string(t.id) + " ended without the module's machine half, which left: ";
    if (t.end == RCS_FILE_COMPLETED) {
        why += "its directories may lack their times";
        if (t.keep) {
            why += ", and its record may remain in its directory (rcs_file_discard deletes it)";
        }
    } else { // Discarded, with keep_partial.
        why += "its partial files and its record may remain in its directory (rcs_file_discard deletes them)";
    }
    rcs_module_log(module_, RCS_LOG_WARNING, why.c_str());
}

void Hub::broken(Transfer& t, const std::string& why) {
    rcs_module_log(module_, RCS_LOG_WARNING, ("The other end broke the module's rules: " + why).c_str());
    finish(t, RCS_FILE_FAILED, RCS_ERROR_PROTOCOL, "The other end broke the module's rules: " + why, false, true);
}

// ---- The clock ----------------------------------------------------------------------------

void Hub::wantTicks() {
    if (ticker_) {
        ticker_->want(true);
    }
}

void Hub::tick() {
    if (!running_) {
        ticker_->want(false);
        return;
    }
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - lastTick_).count();
    lastTick_ = now;
    const auto wait = std::chrono::milliseconds(settings_.reconnect_wait_ms);
    bool any = false;
    std::vector<Token> gone; // Publications whose peer did not come back.
    for (const auto& [id, publication] : publications_) {
        if (!publication.present && now - publication.absentSince >= wait) {
            gone.push_back(id);
        }
        any = any || !publication.present;
    }
    for (const auto& id : gone) {
        revoke(id, RCS_ERROR_DISCONNECTED,
               "The end it was published to did not come back within " + ms(settings_.reconnect_wait_ms) +
                   " (reconnect_wait_ms)");
    }
    std::vector<uint64_t> going;
    for (const auto& [id, t] : transfers_) {
        if (t.phase != Phase::finished) {
            going.push_back(id);
        }
    }
    for (const uint64_t id : going) {
        Transfer* t = find(id);
        if (!t || t->phase == Phase::finished) {
            continue;
        }
        if (!t->sending && t->phase == Phase::offered && now >= t->deadline) {
            finish(*t, RCS_FILE_REJECTED, RCS_ERROR_TIMEOUT,
                   "No decision within " + ms(settings_.offer_timeout_ms) + " (offer_timeout_ms)", false, true);
            continue;
        }
        if (!t->present && now - t->absentSince >= wait) {
            finish(*t, RCS_FILE_FAILED, RCS_ERROR_DISCONNECTED,
                   "The other end did not come back within " + ms(settings_.reconnect_wait_ms) +
                       " (reconnect_wait_ms)",
                   false, false);
            continue;
        }
        if (needsMachine(*t) && !machine_ && now - machineAwaySince_ >= wait) {
            finish(*t, RCS_FILE_FAILED, RCS_ERROR_DEVICE,
                   "The module's machine half (a sharer's agent) did not come back within " +
                       ms(settings_.reconnect_wait_ms) + " (reconnect_wait_ms)",
                   false, true);
            continue;
        }
        any = true;
        if (const uint64_t rate = settings_.max_bytes_per_second; rate && t->sending) {
            t->allowance = std::min(t->allowance + double(rate) * seconds, double(rate) / 4 + chunkBytes);
            pump(*t);
        }
        if (t->phase == Phase::going) {
            emitProgress(*t);
        }
    }
    if (!any && ticker_) {
        ticker_->want(false);
    }
}

// ---- What the host asks -------------------------------------------------------------------

Hub::Transfer* Hub::find(uint64_t id) {
    const auto found = transfers_.find(id);
    return found == transfers_.end() ? nullptr : &found->second;
}

const Hub::Transfer* Hub::find(uint64_t id) const {
    const auto found = transfers_.find(id);
    return found == transfers_.end() ? nullptr : &found->second;
}

Hub::Transfer* Hub::incoming(rcs_viewer_id peer, uint64_t wire) {
    const auto found = peers_.find(peer);
    if (found == peers_.end() || !found->second.hello) {
        return nullptr;
    }
    const auto id = incoming_.find({found->second.hello->end, wire});
    Transfer* t = id == incoming_.end() ? nullptr : find(id->second);
    return t && t->peer == peer ? t : nullptr;
}

Hub::Transfer* Hub::outgoing(rcs_viewer_id peer, uint64_t id) {
    Transfer* t = find(id);
    return t && t->sending && t->peer == peer && t->present ? t : nullptr;
}

uint64_t Hub::doneBytes(const Transfer& t) const noexcept {
    uint64_t done = 0;
    for (const auto& item : t.items) {
        if (item.spec.kind == RCS_FILE_REGULAR) {
            done += item.finished() ? item.spec.size : std::min(item.done, item.spec.size);
        }
    }
    return done;
}

uint32_t Hub::current(const Transfer& t) const noexcept {
    for (uint32_t i = 0; i < t.items.size(); ++i) {
        if (!t.items[i].finished()) {
            return i;
        }
    }
    return uint32_t(t.items.size());
}

rcs_file_state Hub::stateOf(const Transfer& t) const noexcept {
    switch (t.phase) {
    case Phase::preparing:
        return RCS_FILE_PREPARING;
    case Phase::offered:
        return RCS_FILE_OFFERED;
    case Phase::finished:
        return t.settling ? t.reported : rcs_file_state(RCS_FILE_FINISHED); // FINISHED once RCS_FILE_DONE says so.
    case Phase::going:
        break;
    }
    if (!t.present || (needsMachine(t) && !machine_) || (t.sending && !t.synced)) {
        return RCS_FILE_WAITING;
    }
    return t.pausedHere || t.pausedThere ? RCS_FILE_PAUSED : RCS_FILE_RUNNING;
}

void Hub::refresh(Transfer& t) {
    const rcs_file_state state = stateOf(t);
    const uint32_t paused =
        (t.pausedHere ? uint32_t(RCS_FILE_PAUSED_HERE) : 0u) | (t.pausedThere ? uint32_t(RCS_FILE_PAUSED_THERE) : 0u);
    if (state == t.reported && paused == t.reportedPaused) {
        return;
    }
    t.reported = state;
    t.reportedPaused = paused;
    if (state != RCS_FILE_FINISHED) { // RCS_FILE_DONE says that.
        rcs_file_state_event event{t.id, state, paused};
        post(t.peer, RCS_FILE_STATE, &event, sizeof(event));
    }
}

rcs_result Hub::info(uint64_t transfer, rcs_file_transfer_info& out) const {
    const Transfer* t = find(transfer);
    if (!t) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       ("This session knows no transfer " + std::to_string(transfer) + " (any more)")
                                           .c_str());
    }
    rcs_file_transfer_info i = RCS_FILE_TRANSFER_INFO_INIT;
    i.struct_size = out.struct_size;
    i.viewer_id = t->peer;
    i.direction = t->sending ? RCS_FILE_SEND : RCS_FILE_RECEIVE;
    i.target = t->target;
    i.state = stateOf(*t);
    i.paused =
        (t->pausedHere ? uint32_t(RCS_FILE_PAUSED_HERE) : 0u) | (t->pausedThere ? uint32_t(RCS_FILE_PAUSED_THERE) : 0u);
    i.resuming = t->resuming ? 1 : 0;
    std::copy(t->token.begin(), t->token.end(), i.token);
    i.origin = t->fetched ? RCS_FILE_ORIGIN_FETCHED : RCS_FILE_ORIGIN_SENT;
    i.mode = t->mode;
    std::copy(t->publication.begin(), t->publication.end(), i.publication);
    i.items = uint32_t(t->items.size());
    for (const auto& item : t->items) {
        i.items_completed += item.state == RCS_FILE_ITEM_COMPLETED;
        i.items_skipped += item.state == RCS_FILE_ITEM_SKIPPED;
        i.items_failed += item.state == RCS_FILE_ITEM_FAILED;
    }
    i.item = current(*t);
    i.total_bytes = t->total;
    i.done_bytes = doneBytes(*t);
    i.resumed_bytes = t->resumed;
    i.transferred_bytes = t->transferred;
    i.bytes_per_second = t->phase == Phase::going ? t->speed : 0;
    if (i.state == RCS_FILE_FINISHED) {
        i.end = t->end;
        i.by_peer = t->byPeer ? 1 : 0;
        i.result = t->result;
        i.partial_kept = t->partialKept ? 1 : 0;
        copyText(i.message, t->message);
    }
    copyText(i.directory, t->directory);
    out = i;
    return RCS_OK;
}

rcs_result Hub::items(uint64_t transfer, uint32_t first, rcs_file_item_info* out, uint32_t capacity,
                      uint32_t& count) const {
    const Transfer* t = find(transfer);
    if (!t) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       ("This session knows no transfer " + std::to_string(transfer) + " (any more)")
                                           .c_str());
    }
    const auto all = uint32_t(t->items.size());
    count = first < all ? all - first : 0;
    for (uint32_t k = 0; k < std::min(count, capacity); ++k) {
        const auto& item = t->items[first + k];
        auto& o = out[k];
        o = rcs_file_item_info{};
        o.index = first + k;
        o.kind = item.spec.kind;
        o.state = item.state;
        o.result = item.result;
        o.size = item.spec.size;
        o.modified_ns = item.spec.modified;
        o.bytes = item.state == RCS_FILE_ITEM_COMPLETED ? item.spec.size : std::min(item.done, item.spec.size);
        copyText(o.path, item.spec.path);
        copyText(o.saved_as, item.saved);
        copyText(o.message, item.message);
    }
    if (out && capacity < count) { // NULL and 0 ask how many.
        return rcs_set_last_call_error(RCS_ERROR_BUFFER_TOO_SMALL,
                                       ("The transfer has " + std::to_string(count) + " items from " +
                                        std::to_string(first) + " on")
                                           .c_str());
    }
    return RCS_OK;
}

rcs_result Hub::peer(rcs_viewer_id viewer, rcs_file_peer& out) const {
    const auto found = peers_.find(viewer);
    if (found == peers_.end() || !found->second.hello) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "File transfer is not active with that end");
    }
    const uint32_t size = out.struct_size;
    out = peerOf(*found->second.hello);
    out.struct_size = size;
    return RCS_OK;
}

rcs_result Hub::discard(const char* directory, const uint8_t* token) {
    if (!directory || !utf8(directory) || !files::absolute(directory) || !token) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "directory: a full path on the machine, UTF-8; token: RCS_FILE_TOKEN_SIZE bytes");
    }
    if (!running_) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "The session is not running");
    }
    if (settings_.target == RCS_FILE_HOST) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "With RCS_FILE_HOST the host's storage holds what a transfer kept");
    }
    Token which{};
    std::copy(token, token + which.size(), which.begin());
    for (const auto& [id, t] : transfers_) {
        if (t.token == which && (t.phase != Phase::finished || t.settling)) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                           ("Transfer " + std::to_string(id) + " of that token has not finished")
                                               .c_str());
        }
    }
    if (!machine_) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "The module does not run on the machine: no agent is attached, or its process "
                                       "did not register rcsdk.file-transfer");
    }
    const uint64_t job = ++serial_;
    const Bytes bytes = encode(Request{PurgeRequest{job, directory, which}});
    if (const rcs_result sent = rcs_module_send_machine(module_, bytes.data(), uint32_t(bytes.size()));
        sent != RCS_OK) {
        return rcs_set_last_call_error(sent, "The module's machine half did not take the request");
    }
    purges_[job] = which;
    return RCS_OK;
}

void Hub::onPurged(const Purged& purged) {
    const auto found = purges_.find(purged.job);
    if (found == purges_.end()) {
        return;
    }
    rcs_file_discard_event event{};
    std::copy(found->second.begin(), found->second.end(), event.token);
    event.result = purged.result;
    event.files = purged.files;
    copyText(event.message, purged.message);
    purges_.erase(found);
    post(0, RCS_FILE_DISCARDED, &event, sizeof(event));
}

// ---- Publications -------------------------------------------------------------------------

rcs_result Hub::publish(const rcs_file_publication& p, uint8_t* id) {
    if (!running_) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "The session is not running");
    }
    if (p.item_size != sizeof(rcs_file_item) || !p.count || !p.items) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "rcs_file_publication: item_size sizeof(rcs_file_item), and 1.. items");
    }
    if ((role_ == RCS_ROLE_VIEW) != (p.viewer_id == 0)) {
        return rcs_set_last_call_error(
            RCS_ERROR_INVALID_ARGUMENT,
            role_ == RCS_ROLE_VIEW ? "rcs_file_publication: a viewing session publishes to its sharer, viewer_id 0"
                                   : "rcs_file_publication: a sharing session publishes to one viewer: viewer_id");
    }
    if (p.count > settings_.max_items) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       ("rcs_file_publication: more than " + std::to_string(settings_.max_items) +
                                        " items (max_items)")
                                           .c_str());
    }
    Publication added;
    if (const rcs_result checked = checkItems(p.items, p.count, "rcs_file_publication", added.entries, added.items);
        checked != RCS_OK) {
        return checked;
    }
    if (!(settings_.flow & RCS_FILE_SEND)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "rcs_file_transfer_settings: flow has no RCS_FILE_SEND");
    }
    const auto found = peers_.find(p.viewer_id);
    if (found == peers_.end() || !found->second.hello) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       p.viewer_id ? "That viewer is not connected, or does not use file transfer"
                                                   : "The sharer is not connected, or does not use file transfer");
    }
    const Hello& there = *found->second.hello;
    if (!(there.flow & RCS_FILE_RECEIVE)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "The other end does not receive files (its flow has no RCS_FILE_RECEIVE)");
    }
    if (p.count > there.maxItems) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       ("The other end takes at most " + std::to_string(there.maxItems) +
                                        " items in one transfer")
                                           .c_str());
    }
    if (settings_.target == RCS_FILE_HOST) {
        if (const rcs_result sized = hostTotal(added.items, there, "rcs_file_publication", added.total);
            sized != RCS_OK) {
            return sized;
        }
    }
    if (publications_.size() >= settings_.max_publications) {
        return rcs_set_last_call_error(RCS_ERROR_BUSY, ("This end has " + std::to_string(publications_.size()) +
                                                        " publications, the most it keeps (max_publications)")
                                                           .c_str());
    }
    added.peer = p.viewer_id;
    added.peerEnd = there.end;
    added.onFetch = p.on_fetch;
    added.onUnpublished = p.on_unpublished;
    added.userData = p.user_data;
    Token token{};
    do {
        uint8_t random[32];
        rcs_generate_connection_key(random, sizeof(random));
        std::copy(random, random + token.size(), token.begin());
    } while (publications_.count(token));
    publications_[token] = std::move(added);
    std::copy(token.begin(), token.end(), id);
    return RCS_OK;
}

rcs_result Hub::unpublish(const uint8_t* id, uint32_t flags) {
    if (!id || (flags & ~uint32_t(RCS_FILE_UNPUBLISH_END_FETCHES))) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "publication: RCS_FILE_PUBLICATION_SIZE bytes; flags: RCS_FILE_UNPUBLISH_* bits");
    }
    Token which{};
    std::copy(id, id + which.size(), which.begin());
    if (!publications_.count(which)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "This end has no such publication (never, or it was withdrawn)");
    }
    if (flags & RCS_FILE_UNPUBLISH_END_FETCHES) {
        std::vector<uint64_t> fetches;
        for (const auto& [number, t] : transfers_) {
            if (t.sending && t.fetched && t.publication == which && t.phase != Phase::finished) {
                fetches.push_back(number);
            }
        }
        for (const uint64_t number : fetches) {
            if (Transfer* t = find(number)) {
                finish(*t, RCS_FILE_CANCELLED, RCS_ERROR_CANCELLED, "Its publication was withdrawn", false, true);
            }
        }
    }
    revoke(which, RCS_ERROR_CANCELLED, "Withdrawn by its host (rcs_file_unpublish)");
    return RCS_OK;
}

void Hub::revoke(const Token& id, rcs_result result, const std::string& why) {
    const auto found = publications_.find(id);
    if (found == publications_.end()) {
        return;
    }
    if (const auto onUnpublished = found->second.onUnpublished) { // Its last call.
        notify([onUnpublished, id, result, why, user = found->second.userData] {
            onUnpublished(id.data(), result, why.c_str(), user);
        });
    }
    publications_.erase(found);
}

void Hub::revokeAll(rcs_result result, const std::string& why) {
    while (!publications_.empty()) {
        revoke(publications_.begin()->first, result, why);
    }
}

void Hub::onFetch(rcs_viewer_id peer, const Fetch& m) {
    const Token end = peers_.at(peer).hello->end;
    if (const auto known = fetches_.find({end, m.id}); known != fetches_.end()) {
        // Asked again: the fetcher came back before the offer reached it.
        if (Transfer* t = find(known->second); t && (t->phase == Phase::offered || t->phase == Phase::going)) {
            offer(*t, true);
        } else if (t && t->phase == Phase::finished) {
            sendTo(peer, cancelOf(*t), t->id);
        }
        return;
    }
    const auto refuse = [&](rcs_result result, const std::string& why) {
        rcs_module_log(module_, RCS_LOG_INFO, ("Refused a fetch: " + why).c_str());
        sendTo(peer, Cancel{true, true, m.id, RCS_FILE_REJECTED, result, why});
    };
    if (!(settings_.flow & RCS_FILE_SEND)) {
        return refuse(RCS_ERROR_INVALID_STATE, "This end does not send files (its flow has no RCS_FILE_SEND)");
    }
    const auto found = publications_.find(m.publication);
    if (found == publications_.end() || found->second.peerEnd != end) {
        return refuse(RCS_ERROR_INVALID_STATE,
                      "This end published nothing of that id to that end (or withdrew it: a newer copy took its place, "
                      "or its host withdrew it)");
    }
    const Publication& publication = found->second;
    uint32_t going = 0;
    for (const auto& [number, other] : transfers_) {
        going += other.sending && other.fetched && other.peer == peer && other.phase != Phase::finished ? 1 : 0;
    }
    if (going >= settings_.max_fetches) {
        return refuse(RCS_ERROR_BUSY, "That end has " + std::to_string(going) +
                                          " fetches of this end's going, the most this end takes (max_fetches)");
    }
    Transfer t;
    t.id = t.wire = ++serial_;
    t.sending = true;
    t.fetched = true;
    t.mode = m.mode;
    t.publication = m.publication;
    t.fetchId = m.id;
    t.peer = peer;
    t.peerEnd = end;
    t.present = true;
    t.target = settings_.target;
    uint8_t random[32];
    rcs_generate_connection_key(random, sizeof(random));
    std::copy(random, random + t.token.size(), t.token.begin());
    if (t.target == RCS_FILE_HOST) {
        t.total = publication.total;
        hostItems(t, publication.items);
    } else {
        ListRequest listing;
        listing.job = t.id;
        listing.maxItems = std::min(settings_.max_items, peers_.at(peer).hello->maxItems);
        listing.entries = publication.entries;
        t.listing = std::move(listing);
    }
    auto& added = transfers_[t.id] = std::move(t);
    fetches_[{end, m.id}] = added.id;
    added.reported = stateOf(added);
    if (const auto onFetch = publication.onFetch) {
        notify([onFetch, id = m.publication, transfer = added.id, user = publication.userData] {
            onFetch(id.data(), transfer, user);
        });
    }
    if (added.target == RCS_FILE_HOST) {
        offerFetched(added);
    } else {
        prepare(added, *added.listing);
    }
    wantTicks();
}

// ---- Fetches ------------------------------------------------------------------------------

rcs_result Hub::fetch(const rcs_file_fetch_request& r, uint64_t& transfer) {
    if (!running_) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "The session is not running");
    }
    if ((role_ == RCS_ROLE_VIEW) != (r.viewer_id == 0)) {
        return rcs_set_last_call_error(
            RCS_ERROR_INVALID_ARGUMENT,
            role_ == RCS_ROLE_VIEW ? "rcs_file_fetch_request: a viewing session fetches from its sharer, viewer_id 0"
                                   : "rcs_file_fetch_request: a sharing session fetches from one viewer: viewer_id");
    }
    const bool into = r.mode == RCS_FILE_FETCH_INTO;
    if (!into && r.mode != RCS_FILE_FETCH_READ) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "rcs_file_fetch_request: mode INTO or READ");
    }
    const bool writes = into && settings_.target == RCS_FILE_SYSTEM;
    if (writes) {
        if (!r.directory || !utf8(r.directory) || !files::absolute(r.directory)) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           "rcs_file_fetch_request: directory, a full path on the machine, UTF-8");
        }
        if (r.on_conflict < RCS_FILE_CONFLICT_OVERWRITE || r.on_conflict > RCS_FILE_CONFLICT_FAIL) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           "rcs_file_fetch_request: on_conflict, an rcs_file_conflict");
        }
    } else if (r.directory) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "rcs_file_fetch_request: directory NULL, but INTO with RCS_FILE_SYSTEM");
    }
    if (into == (r.on_read != nullptr)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "rcs_file_fetch_request: on_read for READ (and only for READ)");
    }
    if (!(settings_.flow & RCS_FILE_RECEIVE)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "rcs_file_transfer_settings: flow has no RCS_FILE_RECEIVE");
    }
    const auto found = peers_.find(r.viewer_id);
    if (found == peers_.end() || !found->second.hello) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       r.viewer_id ? "That viewer is not connected, or does not use file transfer"
                                                   : "The sharer is not connected, or does not use file transfer");
    }
    if (!(found->second.hello->flow & RCS_FILE_SEND)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "The other end does not send files (its flow has no RCS_FILE_SEND)");
    }
    if (writes && !machine_) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "The module does not run on the machine: no agent is attached, or its process "
                                       "did not register rcsdk.file-transfer");
    }
    Transfer t;
    t.id = ++serial_;
    t.fetched = true;
    t.mode = r.mode;
    std::copy(std::begin(r.publication), std::end(r.publication), t.publication.begin());
    t.fetchId = t.id;
    t.awaiting = true;
    t.peer = r.viewer_id;
    t.peerEnd = found->second.hello->end;
    t.present = true;
    t.target = settings_.target;
    t.phase = Phase::preparing;
    t.directory = writes ? r.directory : "";
    t.conflict = writes ? r.on_conflict : rcs_file_conflict(RCS_FILE_CONFLICT_RENAME);
    t.fetcher = {r.on_listed, r.on_read, r.on_ended, r.user_data};
    auto& added = transfers_[t.id] = std::move(t);
    added.reported = stateOf(added);
    sendTo(added.peer, Fetch{added.id, added.publication, added.mode}, added.id);
    transfer = added.id;
    wantTicks();
    return RCS_OK;
}

void Hub::onFetchOffer(rcs_viewer_id peer, Offer m) {
    Transfer* t = find(m.fetch);
    if (!t || t->sending || !t->fetched || t->peer != peer || !t->awaiting) {
        return; // Ended here meanwhile: the sender learns it from this end's CANCEL.
    }
    if (m.mode != t->mode) {
        return broken(*t, "an offer of another mode than this end fetched");
    }
    t->awaiting = false;
    t->wire = m.id;
    t->token = m.token;
    t->pausedThere = m.paused;
    incoming_[{t->peerEnd, t->wire}] = t->id;
    t->total = totalBytes(m.items);
    if (m.items.size() > settings_.max_items || (settings_.max_offer_bytes && t->total > settings_.max_offer_bytes)) {
        return finish(*t, RCS_FILE_FAILED, RCS_ERROR_INVALID_ARGUMENT,
                      std::to_string(m.items.size()) + " items of " + std::to_string(t->total) +
                          " bytes: more than this end takes (max_items, max_offer_bytes)",
                      false, true);
    }
    if (t->target == RCS_FILE_HOST && t->mode == RCS_FILE_FETCH_INTO) {
        hostItems(*t, std::move(m.items));
    } else {
        for (auto& spec : m.items) {
            Item item;
            item.spec = std::move(spec);
            t->items.push_back(std::move(item));
        }
    }
    if (const auto onListed = t->fetcher.onListed) {
        notify([onListed, transfer = t->id, user = t->fetcher.userData] { onListed(transfer, user); });
    }
    if (t->mode == RCS_FILE_FETCH_INTO) { // Accepted as it came: the directory is checked.
        PrepareRequest request{t->id, t->directory, t->conflict, t->token, false, false, {}};
        for (const auto& item : t->items) {
            request.items.push_back(item.spec);
        }
        prepare(*t, std::move(request));
        refresh(*t);
        return;
    }
    t->phase = Phase::going;
    for (uint32_t i = 0; i < uint32_t(t->items.size()); ++i) {
        if (t->items[i].spec.kind == RCS_FILE_OTHER) {
            settle(*t, i, RCS_FILE_ITEM_SKIPPED, RCS_ERROR_UNSUPPORTED,
                   "Not a file or a directory at the sending end (a symbolic link...): not sent", false);
        }
    }
    refresh(*t);
}

rcs_result Hub::read(uint64_t transfer, uint32_t i, uint64_t offset, uint32_t size, uint64_t tag) {
    Transfer* t = find(transfer);
    if (!t || t->sending || t->mode != RCS_FILE_FETCH_READ) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       ("Transfer " + std::to_string(transfer) + " is not a READ fetch of this end's")
                                           .c_str());
    }
    if (t->phase == Phase::finished || t->awaiting) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, t->awaiting ? "Its items have not come yet (on_listed)"
                                                                            : "It ended");
    }
    if (i >= t->items.size() || t->items[i].spec.kind != RCS_FILE_REGULAR) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "item: not a file of the fetch");
    }
    const auto& spec = t->items[i].spec;
    if (offset >= spec.size || !size || size > RCS_FILE_READ_MAX) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       ("offset before the file's end (" + std::to_string(spec.size) +
                                        " bytes), size 1..RCS_FILE_READ_MAX")
                                           .c_str());
    }
    const auto length = uint32_t(std::min<uint64_t>(size, spec.size - offset));
    if (t->pieceBytes + length > readWindowBytes) {
        return rcs_set_last_call_error(RCS_ERROR_QUEUE_FULL,
                                       "16 MiB of the fetch's reads are not answered yet: read again after on_read");
    }
    const uint64_t serial = ++t->pieceSerial;
    t->pieces[serial] = {serial, tag, i, offset, length, false};
    t->pieceBytes += length;
    sendPieces(*t);
    return RCS_OK;
}

void Hub::sendPieces(Transfer& t) {
    if (t.sending || t.mode != RCS_FILE_FETCH_READ || t.phase != Phase::going || !t.present || t.pausedHere ||
        t.pausedThere) {
        return;
    }
    for (auto& [serial, piece] : t.pieces) {
        if (!piece.asked) {
            sendTo(t.peer, Read{t.wire, serial, piece.item, piece.offset, piece.length}, t.id, true);
            piece.asked = true;
        }
    }
}

void Hub::onRead(rcs_viewer_id peer, const Read& m) {
    Transfer* t = outgoing(peer, m.id);
    if (!t || t->phase == Phase::finished) {
        return; // Ended: the fetcher learns it from this end's CANCEL.
    }
    if (t->mode != RCS_FILE_FETCH_READ || t->phase != Phase::going) {
        return broken(*t, "a read of a transfer that is not a READ fetch");
    }
    if (m.item >= t->items.size() || t->items[m.item].spec.kind != RCS_FILE_REGULAR ||
        m.offset >= t->items[m.item].spec.size || m.length > t->items[m.item].spec.size - m.offset) {
        return broken(*t, "a read outside its file");
    }
    if (t->pieceBytes + m.length > readWindowBytes) {
        return broken(*t, "more than 16 MiB of reads not answered");
    }
    t->serving.push_back({m.serial, 0, m.item, m.offset, m.length, false});
    t->pieceBytes += m.length;
    servePieces(*t);
}

void Hub::servePieces(Transfer& t) {
    if (!t.sending || t.mode != RCS_FILE_FETCH_READ || t.phase != Phase::going || t.pausedHere ||
        (t.target == RCS_FILE_SYSTEM && !machine_)) {
        return;
    }
    for (auto& piece : t.serving) {
        if (!piece.asked) {
            if (!submit(t, readOf(t, piece.item, piece.offset, piece.length))) {
                return;
            }
            piece.asked = true;
        }
    }
}

void Hub::onReadData(rcs_viewer_id peer, const ReadData& m) {
    Transfer* t = incoming(peer, m.id);
    if (!t || t->phase != Phase::going) {
        return;
    }
    const auto found = t->pieces.find(m.serial);
    if (t->mode != RCS_FILE_FETCH_READ || found == t->pieces.end() || !found->second.asked) {
        return broken(*t, "read bytes nobody asked for");
    }
    const Piece piece = found->second;
    if (m.result == RCS_OK && m.data.size() != piece.length) {
        return broken(*t, "read bytes of another length than asked");
    }
    t->pieces.erase(found);
    t->pieceBytes -= piece.length;
    if (m.result == RCS_OK) {
        t->transferred += piece.length;
        advance(*t, piece.item, piece.offset, piece.length);
    } else if (!t->items[piece.item].finished()) {
        settle(*t, piece.item, RCS_FILE_ITEM_FAILED, m.result, m.message, false);
    }
    if (const auto onRead = t->fetcher.onRead) {
        notify([onRead, transfer = t->id, tag = piece.tag, result = m.result, data = Bytes(m.data.begin(), m.data.end()),
                user = t->fetcher.userData] {
            onRead(transfer, tag, result, data.empty() ? nullptr : data.data(), uint32_t(data.size()), user);
        });
    }
}

void Hub::advance(Transfer& t, uint32_t i, uint64_t offset, uint32_t length) {
    auto& item = t.items[i];
    if (offset <= item.done && offset + length > item.done) {
        item.done = offset + length;
    }
    if (item.done == item.spec.size && !item.finished()) {
        conclude(t, i, RCS_FILE_ITEM_COMPLETED, RCS_OK, {});
        emitItem(t, i);
    }
}

void Hub::concludeReads(Transfer& t) {
    for (uint32_t i = 0; i < uint32_t(t.items.size()); ++i) {
        const auto& item = t.items[i];
        if (item.finished()) {
            continue;
        }
        if (item.spec.kind == RCS_FILE_DIRECTORY || item.done == item.spec.size) {
            conclude(t, i, RCS_FILE_ITEM_COMPLETED, RCS_OK, {});
        } else {
            conclude(t, i, RCS_FILE_ITEM_SKIPPED, RCS_OK, "Not read to its end");
        }
        emitItem(t, i);
    }
}

rcs_result Hub::close(uint64_t transfer, rcs_result outcome, const char* reason) {
    Transfer* t = find(transfer);
    if (!t || t->sending || t->mode != RCS_FILE_FETCH_READ || t->phase == Phase::finished || t->awaiting) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       ("Transfer " + std::to_string(transfer) +
                                        " is not a READ fetch of this end's going (rcs_file_cancel ends any)")
                                           .c_str());
    }
    std::string why = reason ? reason : "";
    if (outcome > RCS_OK || outcome < RCS_ERROR_REFUSED || !utf8(why)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "outcome: an rcs_result; reason: UTF-8");
    }
    if (why.size() > 255) {
        char cut[256];
        copyText(cut, why);
        why = cut;
    }
    if (outcome == RCS_OK) {
        concludeReads(*t);
        finish(*t, RCS_FILE_COMPLETED, RCS_OK, {}, false, true);
    } else {
        finish(*t, RCS_FILE_FAILED, outcome, why.empty() ? "Closed by its host" : why, false, true);
    }
    return RCS_OK;
}

// ---- Events -------------------------------------------------------------------------------

void Hub::post(rcs_viewer_id peer, uint32_t kind, const void* payload, size_t size) {
    rcs_module_emit(module_, peer, kind, payload, uint32_t(size));
}

void Hub::emitOffer(const Transfer& t) {
    rcs_file_offer_event event{};
    event.transfer = t.id;
    event.items = uint32_t(t.items.size());
    event.resuming = t.resuming ? 1 : 0;
    event.total_bytes = t.total;
    std::copy(t.token.begin(), t.token.end(), event.token);
    post(t.peer, RCS_FILE_OFFER, &event, sizeof(event));
}

void Hub::emitProgress(Transfer& t) {
    const auto now = Clock::now();
    if (t.samples.empty() || now - t.samples.back().first >= progressEvery) {
        t.samples.emplace_back(now, t.transferred);
        while (t.samples.size() > samplesKept) {
            t.samples.pop_front();
        }
        const double span = std::chrono::duration<double>(t.samples.back().first - t.samples.front().first).count();
        t.speed = span > 0 ? uint64_t(std::llround(double(t.samples.back().second - t.samples.front().second) / span))
                           : 0;
    }
    const uint64_t done = doneBytes(t);
    if (now - t.progressAt < progressEvery || done == t.progressDone) {
        return;
    }
    t.progressAt = now;
    t.progressDone = done;
    rcs_file_progress_event event{};
    event.transfer = t.id;
    event.item = current(t);
    for (const auto& item : t.items) {
        event.items_finished += item.finished() ? 1u : 0u;
    }
    event.done_bytes = done;
    event.total_bytes = t.total;
    event.bytes_per_second = t.speed;
    post(t.peer, RCS_FILE_PROGRESS, &event, sizeof(event));
}

void Hub::emitItem(const Transfer& t, uint32_t i) {
    const auto& item = t.items[i];
    rcs_file_item_event event{};
    event.transfer = t.id;
    event.item = i;
    event.state = item.state;
    event.result = item.result;
    event.bytes = item.state == RCS_FILE_ITEM_COMPLETED ? item.spec.size : std::min(item.done, item.spec.size);
    copyText(event.message, item.message);
    post(t.peer, RCS_FILE_ITEM_DONE, &event, sizeof(event));
}

void Hub::emitDone(const Transfer& t) {
    rcs_file_done_event event{};
    event.transfer = t.id;
    event.direction = t.sending ? RCS_FILE_SEND : RCS_FILE_RECEIVE;
    event.end = t.end;
    event.by_peer = t.byPeer ? 1 : 0;
    event.result = t.result;
    for (const auto& item : t.items) {
        event.items_completed += item.state == RCS_FILE_ITEM_COMPLETED;
        event.items_skipped += item.state == RCS_FILE_ITEM_SKIPPED;
        event.items_failed += item.state == RCS_FILE_ITEM_FAILED;
    }
    event.partial_kept = t.partialKept ? 1 : 0;
    event.done_bytes = doneBytes(t);
    copyText(event.message, t.message);
    post(t.peer, RCS_FILE_DONE, &event, sizeof(event));
}

void Hub::emitPeer(rcs_viewer_id peer, const Hello& hello) {
    const rcs_file_peer event = peerOf(hello);
    post(peer, RCS_FILE_PEER, &event, sizeof(event));
}

} // namespace rcsdk::file_transfer
