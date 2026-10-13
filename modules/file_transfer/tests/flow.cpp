// The session half against a peer that breaks the module's flow (SA-11 of
// docs/SECURITY-ASSESSMENT.md): what a transfer has its store read or write
// stays within about its window, however often the peer offers or answers
// again. The hub runs on its own, called as the module's calls call it; its
// store is the test's, which takes requests and answers only when the test
// says (a disk that is slow), and its peer is the test, which sends what any
// authenticated peer could. Also what it logs names no path of the machine
// (SA-16).
#include "modules/machine_host.hpp"
#include "modules/module.hpp"
#include "session_half.hpp"
#include "support/check.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

using namespace rcsdk::file_transfer;

namespace {

// A directory of the machine, as its file system writes one.
#ifdef _WIN32
#define MACHINE_ROOT "C:\\"
#define MACHINE_SEPARATOR "\\"
#else
#define MACHINE_ROOT "/"
#define MACHINE_SEPARATOR "/"
#endif

// What the hub asks of its store, kept as it comes (on the hub's calls).
class Asked {
public:
    void add(const Bytes& bytes) {
        std::lock_guard lock(mutex_);
        requests_.push_back(decodeRequest(bytes));
    }
    template <class R>
    std::vector<R> all() const {
        std::lock_guard lock(mutex_);
        std::vector<R> found;
        for (const auto& request : requests_) {
            if (const auto* r = std::get_if<R>(&request)) {
                found.push_back(*r);
            }
        }
        return found;
    }
    uint64_t writtenBytes() const {
        uint64_t bytes = 0;
        for (const auto& write : all<WriteRequest>()) {
            bytes += write.data.size();
        }
        return bytes;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Request> requests_;
};

// A hub with a store that never answers by itself, and a peer.
struct Bench {
    Asked asked;
    rcsdk::modules::MachineHost machine{
        RCS_ROLE_SHARE, {nullptr, [this](const std::string&, Bytes bytes) { asked.add(bytes); }, nullptr}};
    rcs_module_impl half{*rcsdk::modules::find(RCS_FILE_TRANSFER_MODULE), nullptr};
    std::unique_ptr<Hub> hub;

    Bench(rcs_role role, rcs_file_flow flow, rcs_viewer_id peer, rcs_file_flow peerFlow) {
        half.host = &machine; // Its requests come to `asked`.
        hub = std::make_unique<Hub>(&half);
        call([&](Hub& h) {
            rcs_file_transfer_settings settings = RCS_FILE_TRANSFER_SETTINGS_INIT;
            settings.flow = flow;
            h.start(role, settings);
            Token end{};
            end.fill(0x5A);
            h.peerJoined(peer, encode(Hello{peerFlow, end, 100, 0}));
            h.machineJoined();
        });
    }
    ~Bench() {
        call([](Hub& h) { h.stop(); });
        hub.reset();
    }
    // As one of the module's calls (its ticker's run beside them).
    void call(const std::function<void(Hub&)>& body) {
        rcsdk::modules::call(half, [&] { body(*hub); });
    }
};

constexpr uint64_t fileSize = 1ull << 30;

// A receiver whose sender sends all its window, then offers the transfer
// again (as after a reconnect), and again: the store is handed no more than
// a window (and the piece that passed it) until it wrote that. Each offer
// used to start the window over, so the bytes waiting for a slow disk grew
// with each one, to the transfer's size.
void receiverKeepsAWindow() {
    Bench bench(RCS_ROLE_SHARE, RCS_FILE_RECEIVE, 1, RCS_FILE_SEND);
    Offer offer;
    offer.id = 5;
    offer.token.fill(7);
    offer.items = {{RCS_FILE_REGULAR, fileSize, 0, "big.bin"}};
    bench.call([&](Hub& hub) {
        hub.received(1, encode(PeerMessage{offer}));
        CHECK(hub.accept(1, MACHINE_ROOT "somewhere", RCS_FILE_CONFLICT_RENAME) == RCS_OK);
        hub.fromMachine(encode(Reply{Prepared{1, RCS_OK, {}, {}}})); // Its first answer: epoch 1.
    });
    offer.again = true;
    for (uint32_t epoch = 1; epoch <= 8; ++epoch) {
        bench.call([&](Hub& hub) {
            // As much as the receiver takes in this epoch, each piece where it expects it.
            const uint64_t from = bench.asked.writtenBytes();
            for (uint64_t at = from; at + chunkBytes <= from + windowBytes + chunkBytes; at += chunkBytes) {
                const Bytes data(chunkBytes, uint8_t(epoch));
                hub.received(1, encode(PeerMessage{Chunk{5, epoch, 0, at, data}}));
            }
            hub.received(1, encode(PeerMessage{offer}));
        });
    }
    const uint64_t handed = bench.asked.writtenBytes();
    std::printf("receiver: %llu bytes handed to the store after 8 offers again, none written yet\n",
                (unsigned long long)handed);
    CHECK(handed >= windowBytes && handed <= windowBytes + chunkBytes);
    rcs_file_transfer_info info = RCS_FILE_TRANSFER_INFO_INIT;
    bench.call([&](Hub& hub) { CHECK(hub.info(1, info) == RCS_OK); });
    CHECK(info.state == RCS_FILE_RUNNING);

    // The store writes what it had: the sender hears where to go on from (a
    // new epoch, 2), and what it sends then is taken.
    bench.call([&](Hub& hub) {
        for (const auto& write : bench.asked.all<WriteRequest>()) {
            hub.fromMachine(encode(Reply{Written{1, write.epoch, 0, write.offset + write.data.size()}}));
        }
        const Bytes data(chunkBytes, 9);
        hub.received(1, encode(PeerMessage{Chunk{5, 2, 0, handed, data}}));
    });
    CHECK(bench.asked.writtenBytes() == handed + chunkBytes);
}

// A sender whose receiver answers again and again: the reads asked of the
// store and not back yet stay in the window, so it is asked no more than a
// window until they come back. Each answer used to start the window over,
// so the reads waiting for a slow disk grew with each one.
void senderKeepsAWindow() {
    Bench bench(RCS_ROLE_VIEW, RCS_FILE_SEND, 0, RCS_FILE_RECEIVE);
    uint64_t transfer = 0;
    bench.call([&](Hub& hub) {
        rcs_file_item item{};
        item.path = MACHINE_ROOT "x" MACHINE_SEPARATOR "big.bin";
        rcs_file_offer offer = RCS_FILE_OFFER_INIT;
        offer.count = 1;
        offer.items = &item;
        CHECK(hub.send(offer, transfer) == RCS_OK);
        hub.fromMachine(encode(Reply{Listed{transfer,
                                            RCS_OK,
                                            {},
                                            {{RCS_FILE_REGULAR, fileSize, 7, "big.bin"}},
                                            {MACHINE_ROOT "x"},
                                            {{0, "big.bin"}}}}));
        for (uint32_t epoch = 1; epoch <= 8; ++epoch) {
            Answer answer;
            answer.id = transfer;
            answer.epoch = epoch;
            hub.received(0, encode(PeerMessage{answer}));
        }
    });
    const size_t asked = bench.asked.all<ReadRequest>().size();
    std::printf("sender: %zu reads asked of the store after 8 answers, none back yet\n", asked);
    CHECK(asked == windowBytes / chunkBytes);
    // They come back (and do not go: an answer came since); then it reads on.
    bench.call([&](Hub& hub) {
        for (const auto& read : bench.asked.all<ReadRequest>()) {
            hub.fromMachine(encode(Reply{Data{transfer, read.epoch, read.item, read.offset, Bytes(read.length)}}));
        }
    });
    CHECK(bench.asked.all<ReadRequest>().size() > asked);
}

// What it logs as a transfer ends without its machine's half names no
// directory of the machine (a host may pass its log on).
void logsNameNoPath() {
    static std::mutex mutex;
    static std::vector<std::string> lines;
    CHECK(rcs_set_log_callback(
              RCS_LOG_DEBUG,
              [](const rcs_log_record* record, void*) {
                  std::lock_guard lock(mutex);
                  lines.emplace_back(record->message);
              },
              nullptr) == RCS_OK);
    {
        Bench bench(RCS_ROLE_SHARE, RCS_FILE_RECEIVE, 1, RCS_FILE_SEND);
        Offer offer;
        offer.id = 5;
        offer.token.fill(8);
        offer.items = {{RCS_FILE_REGULAR, 100, 0, "a.bin"}};
        bench.call([&](Hub& hub) {
            hub.received(1, encode(PeerMessage{offer}));
            const char* directory = MACHINE_ROOT "home" MACHINE_SEPARATOR "someone" MACHINE_SEPARATOR "secret-project";
            CHECK(hub.accept(1, directory, RCS_FILE_CONFLICT_RENAME) == RCS_OK);
            hub.fromMachine(encode(Reply{Prepared{1, RCS_OK, {}, {}}}));
            hub.received(1, encode(PeerMessage{Chunk{5, 1, 0, 0, Bytes(10, 1)}}));
            CHECK(hub.cancel(1) == RCS_OK); // Its store is asked to delete what it wrote...
            hub.machineLeft();              // ...and leaves first.
        });
    }
    rcs_set_log_callback(RCS_LOG_ERROR, nullptr, nullptr);
    std::lock_guard lock(mutex);
    bool told = false;
    for (const auto& line : lines) {
        CHECK(line.find("someone") == std::string::npos && line.find("secret-project") == std::string::npos);
        told = told || line.find("may remain in its directory") != std::string::npos;
    }
    CHECK(told);
}

} // namespace

int main() {
    rcsdk::modules::registerModule(*rcs_file_transfer_module());
    return runTest("file_transfer.flow", [] {
        receiverKeepsAWindow();
        senderKeepsAWindow();
        logsNameNoPath();
    });
}
