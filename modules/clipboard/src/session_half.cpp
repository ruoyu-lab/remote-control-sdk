#include "session_half.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace rcsdk::clipboard {
namespace {

// Copies given to the clipboard and not answered yet: an agent that left
// answers none of its own, so only the newest are kept.
constexpr size_t maxPlacing = 16;
// Copies with files placed here whose stand-ins may still ask for them.
constexpr size_t standInsKept = 16;

rcs_clipboard_stats emptyStats() noexcept {
    rcs_clipboard_stats stats = RCS_CLIPBOARD_STATS_INIT;
    return stats;
}

void add(rcs_clipboard_stats& total, const rcs_clipboard_stats& more) noexcept {
    total.sent += more.sent;
    total.sent_bytes += more.sent_bytes;
    total.received += more.received;
    total.received_bytes += more.received_bytes;
    total.refused += more.refused;
    total.concealed += more.concealed;
    total.failed += more.failed;
    total.pastes_here += more.pastes_here;
    total.pastes_there += more.pastes_there;
    total.pastes_failed += more.pastes_failed;
}

Publication publicationOf(const uint8_t* id) {
    Publication value{};
    std::copy(id, id + value.size(), value.begin());
    return value;
}

// What file transfer's callbacks of one publication or fetch carry: the way
// back to the hub, and which fetch the machine half asked for. Its last
// callback deletes it.
struct Ticket {
    std::shared_ptr<Link> link;
    uint64_t request = 0;
};

void onFetch(const uint8_t* id, uint64_t transfer, void* user) {
    static_cast<Ticket*>(user)->link->run(
        [publication = publicationOf(id), transfer](Hub& hub) { hub.fetched(publication, transfer); });
}

void onUnpublished(const uint8_t* id, rcs_result, const char*, void* user) {
    const std::unique_ptr<Ticket> ticket(static_cast<Ticket*>(user));
    ticket->link->run([publication = publicationOf(id)](Hub& hub) { hub.unpublished(publication); });
}

void onListed(uint64_t transfer, void* user) {
    auto* ticket = static_cast<Ticket*>(user);
    ticket->link->run([request = ticket->request, transfer](Hub& hub) { hub.listed(request, transfer); });
}

void onRead(uint64_t, uint64_t tag, rcs_result result, const uint8_t* data, uint32_t size, void* user) {
    auto* ticket = static_cast<Ticket*>(user);
    ticket->link->run([request = ticket->request, tag, result, bytes = Bytes(data, data + size)](Hub& hub) mutable {
        hub.answered(request, tag, result, std::move(bytes));
    });
}

void onEnded(uint64_t transfer, rcs_result result, const char* message, void* user) {
    const std::unique_ptr<Ticket> ticket(static_cast<Ticket*>(user));
    ticket->link->run([request = ticket->request, transfer, result, why = std::string(message)](Hub& hub) {
        hub.ended(request, transfer, result, why);
    });
}

} // namespace

// ---- Link ---------------------------------------------------------------------------------

void Link::run(std::function<void(Hub&)> task) {
    {
        std::lock_guard lock(mutex_);
        if (!hub_) {
            return;
        }
        ++busy_;
    }
    struct Call {
        Link* link;
        std::function<void(Hub&)>* task;
    } call{this, &task};
    rcs_module_run(
        module_,
        [](rcs_module m, void* argument) -> rcs_result {
            auto& c = *static_cast<Call*>(argument);
            Hub* hub = nullptr;
            {
                std::lock_guard lock(c.link->mutex_);
                hub = c.link->hub_;
            }
            try {
                if (hub) { // Its destructor waits for this call.
                    (*c.task)(*hub);
                }
            } catch (const std::exception& error) {
                rcs_module_log(m, RCS_LOG_ERROR, error.what());
            }
            return RCS_OK;
        },
        &call);
    {
        std::lock_guard lock(mutex_);
        --busy_;
    }
    idle_.notify_all();
}

void Link::close() {
    std::unique_lock lock(mutex_);
    hub_ = nullptr;
    idle_.wait(lock, [&] { return busy_ == 0; });
}

Hub::~Hub() {
    link_->close();
}

Takes Hub::takes() const {
    if (!(settings_.flow & RCS_CLIPBOARD_RECEIVE)) {
        return {};
    }
    uint32_t formats = settings_.formats;
    const bool files = fileTransfer_ && (settings_.target == RCS_CLIPBOARD_HOST ||
                                         (machine_ && (machineFiles_ & RCS_CLIPBOARD_RECEIVE)));
    if (!files) {
        formats &= ~filesFormat;
    }
    return formats ? Takes{formats, settings_.max_bytes} : Takes{};
}

void Hub::tellTakes() {
    const Takes now = takes();
    if (told_ && *told_ == now) {
        return;
    }
    told_ = now;
    const Bytes hello = encodeTakes(now);
    rcs_module_set_hello(module_, hello.data(), uint32_t(hello.size()));
}

rcs_clipboard_stats& Hub::count(rcs_viewer_id peer) {
    return stats_.try_emplace(peer, emptyStats()).first->second;
}

// ---- The run ------------------------------------------------------------------------------

void Hub::start(rcs_role role, const rcs_clipboard_settings& settings) {
    role_ = role;
    settings_ = settings;
    running_ = true;
    peers_.clear();
    latest_.reset();
    placing_.clear();
    stats_.clear();
    departed_.clear();
    published_.clear();
    standIns_.clear();
    fetches_.clear();
    rcs_module files = nullptr;
    fileTransfer_ = rcs_module_find(session(), RCS_FILE_TRANSFER_MODULE, &files) == RCS_OK;
    told_.reset();
    tellTakes();
}

void Hub::update(const rcs_clipboard_settings& settings) {
    if (!(settings.flow & RCS_CLIPBOARD_SEND)) {
        latest_.reset(); // What it held goes nowhere now, nor when SEND comes back.
    }
    if (!(settings.flow & RCS_CLIPBOARD_SEND) || !(settings.formats & filesFormat)) {
        withdraw();
    }
    settings_ = settings;
    tellTakes();
    useMachine();
}

void Hub::stop() {
    running_ = false;
    peers_.clear();
    latest_.reset();
    placing_.clear();
    published_.clear(); // File transfer withdraws them as its run ends.
    standIns_.clear();
    fetches_.clear();
    machine_ = false;
    used_.reset();
}

// ---- The peers ----------------------------------------------------------------------------

void Hub::peerTakes(rcs_viewer_id peer, std::span<const uint8_t> hello) {
    Takes takes;
    try {
        takes = decodeTakes(hello);
    } catch (const Error& error) {
        rcs_module_log(module_, RCS_LOG_WARNING,
                       (std::string("The other end said what it takes in a way this end does not read; it gets "
                                    "nothing: ") +
                        error.what())
                           .c_str());
    }
    peers_[peer].takes = takes;
    rcs_clipboard_peer said = RCS_CLIPBOARD_PEER_INIT;
    said.formats = takes.formats;
    said.max_bytes = takes.maxBytes;
    rcs_module_emit(module_, peer, RCS_CLIPBOARD_PEER, &said, sizeof(said));
}

void Hub::peerJoined(rcs_viewer_id peer, std::span<const uint8_t> hello) {
    count(peer);
    peerTakes(peer, hello);
    useMachine();
    // It gets the newest copy (send_existing): also one a viewer held before
    // it connected. A copy: on_clipboard may send a newer one meanwhile.
    if (latest_ && latest_->content.formats() && settings_.send_existing) {
        const Copy copy = *latest_;
        offer(copy, peer, true);
    }
}

void Hub::peerHello(rcs_viewer_id peer, std::span<const uint8_t> hello) {
    if (!peers_.count(peer)) {
        return;
    }
    peerTakes(peer, hello);
    // It gets the newest copy if it takes more of it now.
    if (latest_ && latest_->content.formats() && settings_.send_existing) {
        const Copy copy = *latest_;
        offer(copy, peer, true);
    }
}

void Hub::peerLeft(rcs_viewer_id peer) {
    peers_.erase(peer);
    if (role_ == RCS_ROLE_SHARE) {
        departed_.push_back(peer);
        if (departed_.size() > departedKept) {
            stats_.erase(departed_.front());
            departed_.pop_front();
        }
    }
    if (peers_.empty()) {
        // Nothing is watched now, and the newest copy is forgotten: the next
        // watch reports the clipboard afresh.
        latest_.reset();
        placing_.clear();
    }
    useMachine();
}

void Hub::received(rcs_viewer_id peer, std::span<const uint8_t> data) {
    PeerMessage message;
    try {
        message = decodePeer(data);
    } catch (const Error& error) {
        count(peer).failed++;
        report(RCS_CLIPBOARD_RECEIVE, RCS_ERROR_PROTOCOL, peer, 0, 0, 0,
               std::string("The other end sent a copy this end does not read: ") + error.what());
        return;
    }
    if (auto* copy = std::get_if<CopyMessage>(&message)) {
        arrived(std::move(copy->content), peer);
    } else {
        withdrawn(peer, std::get<WithdrawnMessage>(message).publication);
    }
}

void Hub::sent(rcs_viewer_id peer, uint64_t tag) {
    const auto found = peers_.find(peer);
    if (found == peers_.end()) {
        return;
    }
    auto& sending = found->second.sending;
    const auto copy = sending.find(tag);
    if (copy == sending.end()) {
        return;
    }
    const Sending went = copy->second;
    sending.erase(sending.begin(), std::next(copy)); // Those a newer one replaced went nowhere.
    auto& s = count(peer);
    ++s.sent;
    s.sent_bytes += went.bytes;
    report(RCS_CLIPBOARD_SEND, RCS_OK, peer, went.formats, went.leftOut, went.bytes, went.why);
}

// ---- This machine's clipboard ---------------------------------------------------------------

void Hub::toMachine(const ToMachine& message) {
    const Bytes bytes = encode(message);
    rcs_module_send_machine(module_, bytes.data(), uint32_t(bytes.size()));
}

void Hub::useMachine(bool always) {
    std::optional<Use> want;
    if (running_ && !peers_.empty() && settings_.flow && settings_.target == RCS_CLIPBOARD_SYSTEM) {
        uint32_t formats = settings_.formats;
        if (!fileTransfer_ || !(machineFiles_ & RCS_CLIPBOARD_SEND)) {
            formats &= ~filesFormat; // Copies read here carry no files that could go.
        }
        if (formats) {
            want = Use{(settings_.flow & RCS_CLIPBOARD_SEND) != 0, settings_.check_interval_ms, formats,
                       settings_.max_bytes, settings_.files_directory, settings_.files_stand_in != 0};
        }
    }
    if (!machine_ || (want == used_ && !always)) {
        return;
    }
    used_ = want;
    toMachine(UseMessage{want});
}

void Hub::machineJoined() {
    machine_ = true;
    used_.reset(); // A half that starts uses nothing, and first says what its clipboard does with files.
    useMachine(true);
}

void Hub::machineLeft() {
    machine_ = false;
    used_.reset();
    machineFiles_ = 0;
    // Its pastes are gone with it: what they read ends; what goes into a
    // directory goes on (file transfer writes it there).
    for (const auto& [request, fetch] : fetches_) {
        if (fetch.mode == RCS_FILE_FETCH_READ) {
            rcs_file_close(session(), fetch.transfer, RCS_ERROR_DISCONNECTED, "The paste's machine half left");
        }
    }
    fetches_.clear();
    tellTakes();
}

void Hub::fromMachine(std::span<const uint8_t> data) {
    ToSession message;
    try {
        message = decodeToSession(data);
    } catch (const Error& error) {
        rcs_module_log(module_, RCS_LOG_ERROR,
                       (std::string("The machine half sent what this end does not read: ") + error.what()).c_str());
        return;
    }
    if (auto* copy = std::get_if<CopiedMessage>(&message)) {
        copied(std::move(copy->content), copy->existing);
    } else if (const auto* failed = std::get_if<ReadFailedMessage>(&message)) {
        readFailed(failed->result, failed->message);
    } else if (const auto* placedCopy = std::get_if<PlacedMessage>(&message)) {
        placed(placedCopy->id, placedCopy->result, placedCopy->message);
    } else if (const auto* abilities = std::get_if<AbilitiesMessage>(&message)) {
        machineFiles_ = abilities->files;
        if ((settings_.formats & filesFormat) && settings_.target == RCS_CLIPBOARD_SYSTEM &&
            abilities->files != (RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE)) {
            rcs_module_log(module_, RCS_LOG_WARNING,
                           ("This machine's clipboard does not carry files both ways: " + abilities->why).c_str());
        }
        tellTakes();
        useMachine();
    } else if (const auto* asked = std::get_if<WantMessage>(&message)) {
        want(*asked);
    } else if (const auto* read = std::get_if<ReadMessage>(&message)) {
        const auto found = fetches_.find(read->request);
        const rcs_result asked = found == fetches_.end()
                                     ? RCS_ERROR_INVALID_STATE
                                     : rcs_file_read(session(), found->second.transfer, read->item, read->offset,
                                                     read->size, read->tag);
        if (asked != RCS_OK) { // Answered at once.
            toMachine(DataMessage{read->request, read->tag, asked, {}});
        }
    } else if (const auto* close = std::get_if<CloseMessage>(&message)) {
        if (const auto found = fetches_.find(close->request); found != fetches_.end()) {
            rcs_file_close(session(), found->second.transfer, close->outcome,
                           close->outcome == RCS_OK ? nullptr : "The paste ended");
        }
    } else {
        const auto& refused = std::get<RefusedMessage>(message);
        const auto found = standIns_.find(refused.placing);
        const rcs_viewer_id from = found == standIns_.end() ? 0 : found->second.from;
        ++count(from).pastes_failed;
        reportPaste(RCS_CLIPBOARD_RECEIVE, from, refused.result, 0,
                    found != standIns_.end() && found->second.cut, false, refused.message);
    }
}

void Hub::copied(Content content, bool existing) {
    if (!used_ || !used_->watch) {
        return; // The watch ended meanwhile.
    }
    if (latest_ && latest_->content == content) {
        return; // A look afresh found what was there: nothing new, and what came from a peer stays where it came
                // from.
    }
    Copy copy;
    copy.serial = ++serial_;
    copy.content = std::move(content);
    latest(copy);
    // Nothing this end carries: the other end keeps what it has. What it held
    // before the connection goes only where the host says.
    if (copy.content.formats() && (!existing || settings_.send_existing)) {
        offer(copy, 0, true);
    }
}

void Hub::latest(const Copy& copy) {
    latest_ = copy;
    withdraw(copy.serial);
}

void Hub::readFailed(rcs_result result, const std::string& message) {
    ++count(0).failed;
    report(RCS_CLIPBOARD_SEND, result, 0, 0, 0, 0, "This machine's clipboard could not be read: " + message);
}

void Hub::placed(uint64_t id, rcs_result result, const std::string& message) {
    const auto found = placing_.find(id);
    if (found == placing_.end()) {
        return;
    }
    Placing placing = std::move(found->second);
    placing_.erase(found);
    const auto& content = placing.content;
    if (result != RCS_OK) {
        ++count(placing.from).failed;
        report(RCS_CLIPBOARD_RECEIVE, result, placing.from, 0, content.formats() | content.leftOut, 0,
               "The copy could not be placed on this machine's clipboard: " + message);
        return;
    }
    auto& s = count(placing.from);
    ++s.received;
    s.received_bytes += content.bytes();
    report(RCS_CLIPBOARD_RECEIVE, RCS_OK, placing.from, content.formats(), content.leftOut, content.bytes(), {});
    Copy copy;
    copy.serial = ++serial_;
    copy.content = std::move(placing.content);
    copy.content.files.reset(); // The peer's publication is for this end alone.
    copy.fromPeer = role_ == RCS_ROLE_VIEW;
    copy.from = placing.from;
    latest(copy);
    if (role_ == RCS_ROLE_SHARE && copy.content.formats()) {
        offer(copy, 0, true); // The sharer's clipboard holds it now: its other viewers get it.
    }
}

// ---- Files --------------------------------------------------------------------------------

std::optional<Publication> Hub::publish(rcs_viewer_id peer, const Copy& copy, const Files& files, std::string& why) {
    if (!fileTransfer_) {
        why = "This session does not use file transfer (rcsdk.file-transfer): the copy's files stay here";
        return std::nullopt;
    }
    rcs_file_peer there = RCS_FILE_PEER_INIT;
    if (rcs_file_get_peer(session(), peer, &there) != RCS_OK || !(there.flow & RCS_FILE_RECEIVE)) {
        why = "The other end's file transfer does not receive files: the copy's files stay here";
        return std::nullopt;
    }
    std::vector<rcs_file_item> items(files.entries.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& entry = files.entries[i];
        items[i].name = entry.name.c_str();
        if (!entry.path.empty()) { // On the machine: file transfer looks at it.
            items[i].path = entry.path.c_str();
        } else {
            items[i].kind = entry.kind;
            items[i].size = entry.size;
            items[i].modified_ns = entry.modified;
        }
    }
    rcs_file_publication publication = RCS_FILE_PUBLICATION_INIT;
    publication.viewer_id = role_ == RCS_ROLE_SHARE ? peer : 0;
    publication.count = uint32_t(items.size());
    publication.items = items.data();
    publication.on_fetch = onFetch;
    publication.on_unpublished = onUnpublished;
    auto ticket = std::make_unique<Ticket>(Ticket{link_, 0});
    publication.user_data = ticket.get();
    Publication id{};
    if (rcs_file_publish(session(), &publication, id.data()) != RCS_OK) {
        char reason[256] = {};
        rcs_get_last_call_error(reason, sizeof(reason));
        why = std::string("The copy's files could not be published: ") + reason;
        return std::nullopt;
    }
    ticket.release(); // on_unpublished deletes it.
    published_[id] = {peer, copy.serial, files.cut};
    return id;
}

void Hub::withdraw(std::optional<uint64_t> keep) {
    for (auto at = published_.begin(); at != published_.end();) {
        if (keep && at->second.serial == *keep) {
            ++at;
            continue;
        }
        rcs_file_unpublish(session(), at->first.data(), 0); // Pastes under way go on.
        if (peers_.count(at->second.peer)) { // Its stand-in there stops taking pastes.
            const Bytes message = encode(PeerMessage{WithdrawnMessage{at->first}});
            rcs_module_send(module_, at->second.peer, message.data(), uint32_t(message.size()), 0, 0);
        }
        at = published_.erase(at);
    }
}

void Hub::fetched(const Publication& publication, uint64_t transfer) {
    const auto found = published_.find(publication);
    if (!running_ || found == published_.end()) {
        return;
    }
    ++count(found->second.peer).pastes_there;
    reportPaste(RCS_CLIPBOARD_SEND, found->second.peer, RCS_OK, transfer, found->second.cut, false, {});
}

void Hub::unpublished(const Publication& publication) {
    const auto found = published_.find(publication);
    if (!running_ || found == published_.end()) {
        return; // Withdrawn here.
    }
    // File transfer withdrew it (it stopped sending, the peer did not come back).
    if (peers_.count(found->second.peer)) {
        const Bytes message = encode(PeerMessage{WithdrawnMessage{publication}});
        rcs_module_send(module_, found->second.peer, message.data(), uint32_t(message.size()), 0, 0);
    }
    published_.erase(found);
}

void Hub::withdrawn(rcs_viewer_id peer, const Publication& publication) {
    for (auto at = standIns_.begin(); at != standIns_.end();) {
        if (at->second.from == peer && at->second.publication == publication) {
            toMachine(RevokedMessage{at->first});
            at = standIns_.erase(at);
        } else {
            ++at;
        }
    }
}

void Hub::want(const WantMessage& want) {
    const auto found = standIns_.find(want.placing);
    const auto refuse = [&](rcs_result result, const std::string& why, rcs_viewer_id from, bool cut) {
        toMachine(EndedMessage{want.request, result, why, false});
        ++count(from).pastes_failed;
        reportPaste(RCS_CLIPBOARD_RECEIVE, from, result, 0, cut, want.ahead, why);
    };
    if (!running_ || found == standIns_.end()) {
        return refuse(RCS_ERROR_INVALID_STATE,
                      "The copy's files were withdrawn: the other end copied something else since", 0, false);
    }
    const StandIn& standIn = found->second;
    rcs_file_fetch_request request = RCS_FILE_FETCH_REQUEST_INIT;
    request.viewer_id = role_ == RCS_ROLE_SHARE ? standIn.from : 0;
    std::copy(standIn.publication.begin(), standIn.publication.end(), request.publication);
    request.mode = want.mode;
    if (want.mode == RCS_FILE_FETCH_INTO) {
        request.directory = want.directory.c_str();
        request.on_conflict = RCS_FILE_CONFLICT_RENAME; // Nothing there is replaced.
    } else {
        request.on_read = onRead;
    }
    request.on_listed = onListed;
    request.on_ended = onEnded;
    auto ticket = std::make_unique<Ticket>(Ticket{link_, want.request});
    request.user_data = ticket.get();
    uint64_t transfer = 0;
    if (rcs_file_fetch(session(), &request, &transfer) != RCS_OK) {
        char reason[256] = {};
        rcs_get_last_call_error(reason, sizeof(reason));
        return refuse(RCS_ERROR_INVALID_STATE, std::string("The files cannot be fetched: ") + reason, standIn.from,
                      standIn.cut);
    }
    ticket.release(); // on_ended deletes it.
    fetches_[want.request] = {transfer, want.placing, want.mode, want.ahead};
    ++count(standIn.from).pastes_here;
    reportPaste(RCS_CLIPBOARD_RECEIVE, standIn.from, RCS_OK, transfer, standIn.cut, want.ahead, {});
}

void Hub::listed(uint64_t request, uint64_t transfer) {
    const auto fetch = fetches_.find(request);
    if (!running_ || fetch == fetches_.end() || !machine_) {
        return;
    }
    uint32_t count = 0;
    rcs_file_get_items(session(), transfer, 0, nullptr, sizeof(rcs_file_item_info), 0, &count);
    std::vector<rcs_file_item_info> items(count);
    rcs_file_get_items(session(), transfer, 0, items.data(), sizeof(rcs_file_item_info), count, &count);
    ListedMessage listed{request, {}};
    for (const auto& item : items) {
        listed.items.push_back({item.path, item.kind, item.size, item.modified_ns});
    }
    // Fetched ahead, nobody pasting: only as much as the copy said, and the
    // settings allow (its end tells the machine's half, which keeps the stand-in).
    if (const auto problem =
            fetch->second.ahead ? prefetchProblem(listed.items, settings_.files_prefetch_bytes) : std::string();
        !problem.empty()) {
        const auto standIn = standIns_.find(fetch->second.placing);
        rcs_file_cancel(session(), transfer);
        reportPaste(RCS_CLIPBOARD_RECEIVE, standIn == standIns_.end() ? 0 : standIn->second.from,
                    RCS_ERROR_BUFFER_TOO_SMALL, transfer, standIn != standIns_.end() && standIn->second.cut, true,
                    problem + ": they are not fetched ahead; a paste fetches them");
        return;
    }
    toMachine(listed);
}

void Hub::answered(uint64_t request, uint64_t tag, rcs_result result, Bytes data) {
    if (running_ && fetches_.count(request) && machine_) {
        toMachine(DataMessage{request, tag, result, std::move(data)});
    }
}

void Hub::ended(uint64_t request, uint64_t transfer, rcs_result result, const std::string& message) {
    if (!running_ || !fetches_.erase(request) || !machine_) {
        return;
    }
    rcs_file_transfer_info info = RCS_FILE_TRANSFER_INFO_INIT;
    const bool complete = rcs_file_get_transfer(session(), transfer, &info) == RCS_OK &&
                          info.end == RCS_FILE_COMPLETED && info.items_completed == info.items;
    toMachine(EndedMessage{request, result, message, complete});
}

// ---- The host -----------------------------------------------------------------------------

rcs_result Hub::send(const rcs_clipboard_content& c) {
    const auto& config = settings_;
    if (c.flow != RCS_CLIPBOARD_SEND || c.concealed > 1 || c.item_size != sizeof(rcs_clipboard_item) ||
        c.count > RCS_CLIPBOARD_IMAGE || (c.count && !c.items) || (!c.count && !c.files)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "rcs_clipboard_content: flow RCS_CLIPBOARD_SEND, concealed 0 or 1, item_size "
                                       "sizeof(rcs_clipboard_item) and 1..4 items, or files (see "
                                       "RCS_CLIPBOARD_CONTENT_INIT)");
    }
    if (!running_) { // The settings a copy is checked against are the run's.
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "The session is not running");
    }
    Content content;
    content.concealed = c.concealed != 0;
    for (uint32_t i = 0; i < c.count; ++i) {
        const auto& item = c.items[i];
        if (item.format < RCS_CLIPBOARD_TEXT || item.format > RCS_CLIPBOARD_IMAGE ||
            (content.formats() & (1u << item.format))) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           "rcs_clipboard_content: an item's format is unknown or there twice");
        }
        if (!(config.formats & (1u << item.format))) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           ("rcs_clipboard_content: " + formatList(1u << item.format) +
                                            " is not in the clipboard's formats (rcs_clipboard_settings)")
                                               .c_str());
        }
        if (!item.size || !item.data) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "rcs_clipboard_content: an item is empty");
        }
        const std::string_view text(reinterpret_cast<const char*>(item.data), item.size);
        if ((item.format == RCS_CLIPBOARD_TEXT || item.format == RCS_CLIPBOARD_HTML) && !utf8Characters(text)) {
            return rcs_set_last_call_error(
                RCS_ERROR_INVALID_ARGUMENT,
                ("rcs_clipboard_content: " + formatList(1u << item.format) + " is not UTF-8").c_str());
        }
        content.items.push_back({item.format, Bytes(item.data, item.data + item.size)});
    }
    if (const auto* f = c.files) {
        if (f->struct_size != sizeof(rcs_clipboard_files) || f->file_size != sizeof(rcs_clipboard_file) ||
            f->cut > 1 || !f->count || f->count > maxEntries || !f->files ||
            std::any_of(std::begin(f->publication), std::end(f->publication), [](uint8_t b) { return b != 0; })) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           "rcs_clipboard_files: RCS_CLIPBOARD_FILES_INIT, cut 0 or 1, 1..100 000 "
                                           "files, publication 0");
        }
        if (!(config.formats & filesFormat)) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                           "rcs_clipboard_content: FILES is not in the clipboard's formats "
                                           "(rcs_clipboard_settings)");
        }
        Files files;
        files.cut = f->cut != 0;
        std::set<std::string> names;
        for (uint32_t i = 0; i < f->count; ++i) {
            const auto& file = f->files[i];
            FileEntry entry{file.name ? file.name : "", file.kind, file.size, file.modified_ns,
                            file.path ? file.path : ""};
            std::string problem = nameProblem(entry.name);
            if (problem.empty() && ((entry.kind != RCS_FILE_REGULAR && entry.kind != RCS_FILE_DIRECTORY) ||
                                    (entry.kind == RCS_FILE_DIRECTORY && entry.size))) {
                problem = "kind REGULAR, or DIRECTORY of size 0";
            }
            if (problem.empty() && !names.insert(entry.name).second) {
                problem = "\"" + entry.name + "\" twice";
            }
            if (!problem.empty()) {
                return rcs_set_last_call_error(
                    RCS_ERROR_INVALID_ARGUMENT,
                    ("rcs_clipboard_files: file " + std::to_string(i) + ": " + problem).c_str());
            }
            files.entries.push_back(std::move(entry));
        }
        content.files = std::move(files);
    }
    if (content.bytes() > config.max_bytes) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       ("rcs_clipboard_content: " + std::to_string(content.bytes()) +
                                        " bytes, more than the clipboard's max_bytes (rcs_clipboard_settings)")
                                           .c_str());
    }
    std::sort(content.items.begin(), content.items.end(),
              [](const Item& a, const Item& b) { return a.format < b.format; });
    if (!(config.flow & RCS_CLIPBOARD_SEND)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       "rcs_clipboard_settings: flow has no RCS_CLIPBOARD_SEND");
    }
    rcs_state state = RCS_STATE_IDLE;
    rcs_session_get_state(session(), &state);
    if (state != RCS_STATE_RUNNING) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "The session is not connected");
    }
    if (role_ == RCS_ROLE_VIEW && c.viewer_id) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "rcs_clipboard_content: a viewing session sends to its sharer, viewer_id 0");
    }
    if (c.viewer_id && !peers_.count(c.viewer_id)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                       ("Viewer " + std::to_string(c.viewer_id) +
                                        " is not connected, or does not use the clipboard")
                                           .c_str());
    }
    Copy copy;
    copy.serial = ++serial_;
    copy.content = std::move(content);
    if (!c.viewer_id) {
        latest(copy); // This end's clipboard: a viewer that joins later gets it too.
    }
    offer(copy, c.viewer_id, false); // The host decided.
    return RCS_OK;
}

rcs_result Hub::peer(rcs_viewer_id viewer, rcs_clipboard_peer& out) const {
    const auto found = peers_.find(viewer);
    if (found == peers_.end()) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "The clipboard is not active with that end");
    }
    out.formats = found->second.takes.formats;
    out.max_bytes = found->second.takes.maxBytes;
    return RCS_OK;
}

rcs_result Hub::stats(rcs_viewer_id viewer, rcs_clipboard_stats& out) const {
    rcs_clipboard_stats total = emptyStats();
    if (viewer) {
        const auto found = stats_.find(viewer);
        if (role_ != RCS_ROLE_SHARE || found == stats_.end()) {
            return rcs_set_last_call_error(
                RCS_ERROR_INVALID_STATE, "The clipboard knows no such viewer in this run (or one that left long ago)");
        }
        add(total, found->second);
    } else {
        for (const auto& [id, counted] : stats_) {
            add(total, counted);
        }
    }
    total.struct_size = out.struct_size;
    out = total;
    return RCS_OK;
}

// ---- Copies -------------------------------------------------------------------------------

void Hub::arrived(Content content, rcs_viewer_id peer) {
    const auto& config = settings_;
    if (!(config.flow & RCS_CLIPBOARD_RECEIVE)) {
        rcs_module_log(module_, RCS_LOG_DEBUG, "A copy came after this end stopped taking copies; dropped");
        return;
    }
    Content fitted = fitting(content, takes().formats, config.max_bytes);
    if (!fitted.formats()) {
        return; // It was on its way as this end took less.
    }
    if (config.target == RCS_CLIPBOARD_HOST) {
        askHost(fitted, RCS_CLIPBOARD_RECEIVE, peer); // The host places it, and fetches its files if it likes.
        auto& s = count(peer);
        ++s.received;
        s.received_bytes += fitted.bytes();
        return;
    }
    if (!askHost(fitted, RCS_CLIPBOARD_RECEIVE, peer)) {
        ++count(peer).refused;
        return;
    }
    const auto notPlaced = [&](rcs_result result, const std::string& why) {
        ++count(peer).failed;
        report(RCS_CLIPBOARD_RECEIVE, result, peer, 0, fitted.formats() | fitted.leftOut, 0,
               "The copy could not be placed on this machine's clipboard: " + why);
    };
    if (!used_) {
        return notPlaced(RCS_ERROR_INVALID_STATE, "the machine's half does not place copies now (a sharer's agent left)");
    }
    const uint64_t id = ++serial_;
    bool ahead = false;
    if (const auto& files = fitted.files) {
        uint64_t left = config.files_prefetch_bytes; // Files alone, of so many bytes together.
        ahead = left != 0;
        for (const auto& entry : files->entries) {
            ahead = ahead && entry.kind == RCS_FILE_REGULAR && entry.size <= left;
            left -= ahead ? entry.size : 0;
        }
        standIns_[id] = {peer, files->publication, files->cut};
        while (standIns_.size() > standInsKept) {
            standIns_.erase(standIns_.begin());
        }
    }
    const Bytes message = encode(ToMachine{PlaceMessage{id, fitted, ahead}});
    if (const rcs_result sent = rcs_module_send_machine(module_, message.data(), uint32_t(message.size()));
        sent != RCS_OK) {
        standIns_.erase(id);
        return notPlaced(sent, std::string("the machine's half did not take it (") + rcs_result_message(sent) + ")");
    }
    placing_[id] = {std::move(fitted), peer}; // Its answer comes after this call.
    while (placing_.size() > maxPlacing) {
        placing_.erase(placing_.begin());
    }
}

void Hub::offer(const Copy& copy, rcs_viewer_id only, bool ask) {
    const auto& config = settings_;
    if (!(config.flow & RCS_CLIPBOARD_SEND) || copy.fromPeer) {
        return;
    }
    // One wire form for the viewers that take the same, where no files make each one's its own.
    std::map<std::pair<uint32_t, uint32_t>, std::pair<Content, Bytes>> fittings;
    for (auto& [id, peer] : peers_) {
        if ((only && id != only) || !peer.takes.formats || (copy.from && id == copy.from)) {
            continue; // Not asked for, it takes none, or the copy came from it.
        }
        if (peer.decided && peer.decided->first == copy.serial && peer.decided->second == peer.takes) {
            continue; // It has this copy as it takes it now, or it was decided.
        }
        peer.decided = std::pair(copy.serial, peer.takes);
        if (copy.content.concealed && !config.send_concealed) {
            ++count(id).concealed;
            continue;
        }
        const auto key = std::pair(peer.takes.formats & config.formats, std::min(peer.takes.maxBytes, config.max_bytes));
        Content fitted = fitting(copy.content, key.first, key.second);
        const auto noneFits = [&](const std::string& why) {
            const uint32_t taken = copy.content.formats() & key.first;
            ++count(id).failed;
            report(RCS_CLIPBOARD_SEND, !why.empty() || !taken ? RCS_ERROR_UNSUPPORTED : RCS_ERROR_BUFFER_TOO_SMALL,
                   id, 0, fitted.leftOut, 0,
                   !why.empty() ? why
                   : taken      ? "The copy (" + formatList(taken) + ") is larger than " + std::to_string(key.second) +
                                 " bytes, the most the other end or this one takes"
                                : "The other end takes " + formatList(peer.takes.formats) + ", none of the copy's (" +
                                 formatList(copy.content.formats()) + ")");
        };
        if (!fitted.formats()) {
            noneFits({});
            continue;
        }
        if (ask && !askHost(fitted, RCS_CLIPBOARD_SEND, id)) {
            ++count(id).refused;
            continue;
        }
        std::string why;
        Bytes wire;
        if (fitted.files) { // Published to this peer alone.
            if (const auto published = publish(id, copy, *fitted.files, why)) {
                fitted.files->publication = *published;
            } else {
                fitted.files.reset();
                fitted.leftOut |= filesFormat;
                if (fitted.items.empty()) {
                    noneFits(why);
                    continue;
                }
            }
            wire = encode(PeerMessage{CopyMessage{fitted}});
        } else {
            auto found = fittings.find(key);
            if (found == fittings.end()) {
                found = fittings.emplace(key, std::pair(fitted, encode(PeerMessage{CopyMessage{fitted}}))).first;
            }
            wire = found->second.second;
        }
        // The newest copy takes the place of one that has not started to go.
        if (const rcs_result sent =
                rcs_module_send(module_, id, wire.data(), uint32_t(wire.size()), RCS_MODULE_REPLACE, copy.serial);
            sent != RCS_OK) {
            ++count(id).failed;
            report(RCS_CLIPBOARD_SEND, sent, id, 0, fitted.formats() | fitted.leftOut, 0,
                   std::string("The copy did not go: ") + rcs_result_message(sent));
        } else {
            peer.sending[copy.serial] = {fitted.formats(), fitted.leftOut, fitted.bytes(), why};
        }
    }
}

bool Hub::askHost(const Content& content, rcs_clipboard_flow flow, rcs_viewer_id viewer) {
    const auto callback = settings_.on_clipboard;
    if (!callback) {
        return true;
    }
    std::vector<rcs_clipboard_item> items;
    for (const auto& item : content.items) {
        items.push_back({item.format, uint32_t(item.bytes.size()), item.bytes.data()});
    }
    rcs_clipboard_content shown = RCS_CLIPBOARD_CONTENT_INIT;
    shown.viewer_id = viewer;
    shown.flow = flow;
    shown.concealed = content.concealed ? 1 : 0;
    shown.count = uint32_t(items.size());
    shown.items = items.data();
    std::vector<rcs_clipboard_file> files;
    rcs_clipboard_files listed = RCS_CLIPBOARD_FILES_INIT;
    if (content.files) {
        for (const auto& entry : content.files->entries) {
            files.push_back({entry.name.c_str(), entry.kind, entry.size, entry.modified,
                             entry.path.empty() ? nullptr : entry.path.c_str()});
        }
        listed.cut = content.files->cut ? 1 : 0;
        std::copy(content.files->publication.begin(), content.files->publication.end(), listed.publication);
        listed.count = uint32_t(files.size());
        listed.files = files.data();
        shown.files = &listed;
    }
    return callback(&shown, settings_.user_data) != 0;
}

void Hub::report(rcs_clipboard_flow flow, rcs_result result, rcs_viewer_id viewer, uint32_t formats,
                 uint32_t leftOut, uint64_t bytes, const std::string& message) {
    if (result != RCS_OK || !message.empty()) {
        rcs_module_log(module_, RCS_LOG_WARNING, message.c_str());
    }
    rcs_clipboard_event event{};
    event.flow = flow;
    event.result = result;
    event.formats = formats;
    event.left_out = leftOut;
    event.bytes = bytes;
    copyText(event.message, message);
    rcs_module_emit(module_, viewer, RCS_CLIPBOARD_COPY, &event, sizeof(event));
}

void Hub::reportPaste(rcs_clipboard_flow flow, rcs_viewer_id viewer, rcs_result result, uint64_t transfer, bool cut,
                      bool ahead, const std::string& message) {
    if (result != RCS_OK) {
        rcs_module_log(module_, RCS_LOG_WARNING, ("A paste of the copy's files: " + message).c_str());
    }
    rcs_clipboard_paste_event event{};
    event.flow = flow;
    event.result = result;
    event.transfer = transfer;
    event.cut = cut ? 1 : 0;
    event.ahead = ahead ? 1 : 0;
    copyText(event.message, message);
    rcs_module_emit(module_, viewer, RCS_CLIPBOARD_PASTE, &event, sizeof(event));
}

} // namespace rcsdk::clipboard
