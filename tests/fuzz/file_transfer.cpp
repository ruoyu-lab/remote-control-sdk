// libFuzzer: the file transfer module's session half (modules/file_transfer/
// src/session_half.cpp), the state machine of transfers both ways: offers
// new, again and resuming, answers and their epochs, pieces, items' ends and
// outcomes, cancels and pauses from the peer; the store's answers (the
// machine's half, which may be an agent of another process), in order or
// not, right or wrong; the host's accepting, cancelling, pausing, sending;
// the peer and the machine leaving and coming back, time passing. The input
// spells out the steps, as file_transfer.flow's bench runs the hub (the test
// is the peer and the store). What the peer or the store may send never
// crashes it; a module's call that throws is caught and logged by the SDK
// (modules::call), as in a session. Nothing touches the file system: the
// store is the target's.
#include "modules/machine_host.hpp"
#include "modules/module.hpp"
#include "session_half.hpp"

#include <deque>
#include <functional>
#include <mutex>

using namespace rcsdk::file_transfer;

namespace {

class Steps {
public:
    Steps(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool more() const noexcept { return at_ < size_; }
    uint8_t byte() noexcept { return at_ < size_ ? data_[at_++] : 0; }
    uint32_t u32() noexcept { return uint32_t(byte()) | uint32_t(byte()) << 8 | uint32_t(byte()) << 16; }
    Bytes bytes(size_t count) {
        count = std::min(count, size_ - at_);
        Bytes out(data_ + at_, data_ + at_ + count);
        at_ += count;
        return out;
    }
    // A short relative path of a few names (some of them not taken: the hub checks).
    std::string path() {
        static const char* const names[] = {"a", "b.txt", "dir", "x/y", "..", "c:d", "e/", "f.rcsdk-part"};
        std::string path = names[byte() % 8];
        if (byte() & 1) {
            path += std::string("/") + names[byte() % 4];
        }
        return path;
    }
    Token token() {
        Token t{};
        t.fill(uint8_t(1 + byte() % 3));
        return t;
    }

private:
    const uint8_t* data_;
    size_t size_, at_ = 0;
};

// The hub, its store (what it asks, answered by the input) and its peer (the input).
struct Bench {
    std::mutex mutex;
    std::deque<Request> asked;
    rcsdk::modules::MachineHost machine{RCS_ROLE_SHARE,
                                        {nullptr,
                                         [this](const std::string&, Bytes bytes) {
                                             std::lock_guard lock(mutex);
                                             asked.push_back(decodeRequest(bytes));
                                         },
                                         nullptr}};
    rcs_module_impl half{*rcsdk::modules::find(RCS_FILE_TRANSFER_MODULE), nullptr};
    std::unique_ptr<Hub> hub;

    explicit Bench(rcs_file_flow flow) {
        half.host = &machine;
        hub = std::make_unique<Hub>(&half);
        call([&](Hub& h) {
            rcs_file_transfer_settings settings = RCS_FILE_TRANSFER_SETTINGS_INIT;
            settings.flow = flow;
            settings.max_items = 16;
            h.start(RCS_ROLE_SHARE, settings);
            h.peerJoined(1, encode(Hello{RCS_FILE_SEND | RCS_FILE_RECEIVE, Token{}, 16, 0}));
            h.machineJoined();
        });
    }
    ~Bench() {
        call([](Hub& h) { h.stop(); });
        hub.reset();
    }
    void call(const std::function<void(Hub&)>& body) {
        rcsdk::modules::call(half, [&] { body(*hub); });
    }
    std::optional<Request> next(size_t skip) {
        std::lock_guard lock(mutex);
        if (asked.empty()) {
            return std::nullopt;
        }
        const size_t at = skip % asked.size();
        Request request = std::move(asked[at]);
        asked.erase(asked.begin() + ptrdiff_t(at));
        return request;
    }
};

std::vector<ItemSpec> items(Steps& in) {
    std::vector<ItemSpec> out(1 + in.byte() % 3);
    for (auto& item : out) {
        item.kind = in.byte() % 4 ? RCS_FILE_REGULAR : RCS_FILE_DIRECTORY;
        item.size = item.kind == RCS_FILE_REGULAR ? in.u32() % (3u << 20) : 0;
        item.modified = in.byte();
        item.path = in.path();
    }
    return out;
}

// What the store answers `request` with: what it should, unless the input says otherwise.
std::optional<Reply> answer(const Request& request, Steps& in) {
    const bool wrong = in.byte() % 8 == 0;
    if (const auto* r = std::get_if<ListRequest>(&request)) {
        Listed listed{r->job, wrong ? RCS_ERROR_DEVICE : RCS_OK, wrong ? "no" : "", {}, {}, {}};
        for (uint32_t entry = 0; entry < r->entries.size() && !wrong; ++entry) {
            listed.items.push_back({RCS_FILE_REGULAR, in.u32() % (1u << 20), 1, r->entries[entry].second});
            listed.bases.push_back("/fuzz-source");
            listed.sources.push_back({entry, r->entries[entry].second});
        }
        return listed;
    }
    if (const auto* r = std::get_if<PrepareRequest>(&request)) {
        Prepared prepared{r->transfer, wrong ? RCS_ERROR_INVALID_STATE : RCS_OK, wrong ? "no" : "", {}};
        for (uint32_t i = 0; i < r->items.size() && !wrong; ++i) {
            if (in.byte() % 3 == 0) {
                const auto what = Start::What(1 + in.byte() % 4);
                prepared.starts.push_back({i, what, what == Start::resume ? in.u32() : 0,
                                           what == Start::failed ? RCS_ERROR_DEVICE : RCS_OK, "x"});
            }
        }
        return prepared;
    }
    if (const auto* r = std::get_if<ReadRequest>(&request)) {
        if (wrong) {
            return ReadFailed{r->transfer, r->epoch, r->item, RCS_ERROR_DEVICE, "unreadable"};
        }
        return Data{r->transfer, r->epoch, r->item, r->offset + (in.byte() % 16 == 0), Bytes(r->length, 7)};
    }
    if (const auto* r = std::get_if<WriteRequest>(&request)) {
        if (wrong) {
            return WriteFailed{r->transfer, r->epoch, r->item, RCS_ERROR_DEVICE, "full"};
        }
        return Written{r->transfer, r->epoch, r->item, r->offset + r->data.size() + (in.byte() % 16 == 0)};
    }
    if (const auto* r = std::get_if<FinishRequest>(&request)) {
        return Finished{r->transfer,
                        r->epoch,
                        r->item,
                        wrong ? RCS_FILE_ITEM_FAILED : RCS_FILE_ITEM_COMPLETED,
                        wrong ? RCS_ERROR_INVALID_STATE : RCS_OK,
                        wrong ? "taken" : "",
                        r->spec.path,
                        wrong};
    }
    if (const auto* r = std::get_if<DiscardRequest>(&request)) {
        return r->ended ? std::optional<Reply>(Ended{r->transfer}) : std::nullopt;
    }
    if (const auto* r = std::get_if<CompleteRequest>(&request)) {
        return Ended{r->transfer};
    }
    if (const auto* r = std::get_if<CloseRequest>(&request)) {
        return Ended{r->transfer};
    }
    if (const auto* r = std::get_if<PurgeRequest>(&request)) {
        return Purged{r->job, RCS_OK, "", 0};
    }
    return std::nullopt;
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    rcsdk::modules::registerModule(*rcs_file_transfer_module());
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Steps in(data, size);
    Bench bench(rcs_file_flow(1 + in.byte() % 3));
    std::vector<Bytes> chunks; // What chunks borrow, kept while they are sent.
    while (in.more()) {
        const uint8_t step = in.byte();
        bench.call([&](Hub& hub) {
            const auto peer = [&](const PeerMessage& message) {
                hub.received(1, encode(message));
            };
            const uint64_t id = 1 + in.byte() % 4;
            switch (step % 14) {
            case 0: { // An offer: new, again, resuming.
                Offer offer;
                offer.id = id;
                const uint8_t how = in.byte();
                offer.again = how & 1;
                offer.resuming = how & 2;
                offer.paused = how & 4;
                offer.token = in.token();
                offer.known = in.byte() % 4;
                offer.items = items(in);
                peer(offer);
                break;
            }
            case 1: { // An answer.
                Answer answer{id, uint32_t(in.byte() % 6), bool(in.byte() & 1), uint32_t(in.byte() % 4), {}};
                for (uint32_t item = answer.first, n = in.byte() % 3; n > 0; --n, item += 1 + in.byte() % 2) {
                    answer.starts.push_back({item, in.byte() % 8 ? uint64_t(in.u32()) : Answer::skipAll});
                }
                peer(answer);
                break;
            }
            case 2: { // A piece.
                chunks.push_back(Bytes(1 + in.byte() * 64u, 3));
                peer(Chunk{id, uint32_t(in.byte() % 6), uint32_t(in.byte() % 4), uint64_t(in.u32()) & ~uint64_t(0xFFF),
                           chunks.back()});
                break;
            }
            case 3: // An item's end.
                peer(ItemEnd{id, uint32_t(in.byte() % 6), uint32_t(in.byte() % 4),
                             in.byte() & 1 ? RCS_OK : RCS_ERROR_DEVICE, "end"});
                break;
            case 4: // What the receiver wrote, and an item's outcome.
                if (in.byte() & 1) {
                    peer(Ack{id, uint32_t(in.byte() % 4), uint64_t(in.u32())});
                } else {
                    peer(ItemDone{id, uint32_t(in.byte() % 4), rcs_file_item_state(2 + in.byte() % 3), RCS_OK, "",
                                  bool(in.byte() & 1)});
                }
                break;
            case 5: { // A cancel or a pause.
                const uint8_t how = in.byte();
                if (how & 1) {
                    peer(Cancel{bool(how & 2), bool(how & 4), id, rcs_file_end(1 + in.byte() % 4), RCS_ERROR_CANCELLED,
                                "stop"});
                } else {
                    peer(Pause{bool(how & 2), id, bool(how & 4)});
                }
                break;
            }
            case 6: // A peer message of the input's own.
                hub.received(1, in.bytes(in.byte()));
                break;
            case 7: // The store answers what it was asked (mostly in order).
                if (auto request = bench.next(in.byte() % 8 == 0 ? in.byte() : 0)) {
                    if (const auto reply = answer(*request, in)) {
                        hub.fromMachine(encode(*reply));
                    }
                }
                break;
            case 8: // The store's answer of the input's own.
                hub.fromMachine(in.bytes(in.byte()));
                break;
            case 9: // The host accepts, rejects, cancels or pauses.
                switch (in.byte() % 4) {
                case 0:
                    hub.accept(id, "/fuzz-receive", rcs_file_conflict(in.byte() % 5));
                    break;
                case 1:
                    hub.reject(id, "no");
                    break;
                case 2:
                    hub.cancel(id);
                    break;
                case 3:
                    hub.pause(id, in.byte() & 1);
                    break;
                }
                break;
            case 10: { // The host sends.
                rcs_file_item item{"/fuzz-source/file", nullptr, 0, 0, 0};
                rcs_file_offer offer = RCS_FILE_OFFER_INIT;
                offer.viewer_id = 1;
                offer.count = 1;
                offer.items = &item;
                uint64_t transfer = 0;
                hub.send(offer, transfer);
                break;
            }
            case 11: // The peer leaves and comes back.
                hub.peerLeft(1);
                hub.peerJoined(1, encode(Hello{RCS_FILE_SEND | RCS_FILE_RECEIVE, Token{}, 16, 0}));
                break;
            case 12: // The machine's half leaves and comes back.
                hub.machineLeft();
                hub.machineJoined();
                break;
            case 13:
                hub.tick();
                break;
            }
        });
    }
    return 0;
}
