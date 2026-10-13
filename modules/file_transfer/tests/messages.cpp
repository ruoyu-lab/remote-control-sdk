// The module's messages: each one there and back, and what is refused of
// what comes (the other end, and an agent of another process, may be anyone's).
#include "messages.hpp"
#include "support/check.hpp"

using namespace rcsdk::file_transfer;

namespace {

template <class Decode>
bool refused(Decode decode, const Bytes& bytes) {
    try {
        decode(bytes);
        return false;
    } catch (const Error& error) {
        return error.result() == RCS_ERROR_PROTOCOL;
    }
}

bool peerRefused(const Bytes& bytes) {
    return refused([](const Bytes& b) { decodePeer(b); }, bytes);
}
bool requestRefused(const Bytes& bytes) {
    return refused([](const Bytes& b) { decodeRequest(b); }, bytes);
}
bool replyRefused(const Bytes& bytes) {
    return refused([](const Bytes& b) { decodeReply(b); }, bytes);
}

template <class T>
T peerRoundTrip(const T& message) {
    return std::get<T>(decodePeer(encode(PeerMessage{message})));
}
template <class T>
T requestRoundTrip(const T& message) {
    return std::get<T>(decodeRequest(encode(Request{message})));
}
template <class T>
T replyRoundTrip(const T& message) {
    return std::get<T>(decodeReply(encode(Reply{message})));
}

Token token(uint8_t seed) {
    Token t{};
    for (size_t i = 0; i < t.size(); ++i) {
        t[i] = uint8_t(seed + i);
    }
    return t;
}

std::vector<ItemSpec> someItems() {
    return {{RCS_FILE_DIRECTORY, 0, 1'700'000'000'000'000'000, "photos"},
            {RCS_FILE_REGULAR, 123456, -5, "photos/a.jpg"},
            {RCS_FILE_REGULAR, 0, 0, "photos/empty"},
            {RCS_FILE_OTHER, 0, 7, "photos/link"}};
}

void hello() {
    const Hello h{RCS_FILE_SEND | RCS_FILE_RECEIVE, token(3), 10000, 1u << 30};
    CHECK(decodeHello(encode(h)) == h);
    Bytes bytes = encode(h);
    bytes.push_back(0);
    CHECK(refused([](const Bytes& b) { decodeHello(b); }, bytes)); // Trailing.
    bytes = encode(Hello{4, token(1), 1, 0});
    CHECK(refused([](const Bytes& b) { decodeHello(b); }, bytes)); // A flow no version defines.
    bytes = encode(Hello{1, token(1), 0, 0});
    CHECK(refused([](const Bytes& b) { decodeHello(b); }, bytes)); // max_items 0.
    bytes = encode(Hello{1, token(1), maxItemsLimit + 1, 0});
    CHECK(refused([](const Bytes& b) { decodeHello(b); }, bytes));
    CHECK(refused([](const Bytes& b) { decodeHello(b); }, Bytes{}));
}

void peerMessages() {
    Offer offer;
    offer.id = 42;
    offer.again = true;
    offer.resuming = true;
    offer.paused = true;
    offer.token = token(9);
    offer.known = 2;
    offer.items = someItems();
    const Offer o = peerRoundTrip(offer);
    CHECK(o.id == 42 && o.again && o.resuming && o.paused && o.token == token(9) && o.known == 2 &&
          o.items == someItems());
    CHECK(totalBytes(o.items) == 123456);

    Answer answer;
    answer.id = 42;
    answer.epoch = 3;
    answer.paused = true;
    answer.first = 1;
    answer.starts = {{1, 100}, {3, Answer::skipAll}};
    const Answer a = peerRoundTrip(answer);
    CHECK(a.id == 42 && a.epoch == 3 && a.paused && a.first == 1 && a.starts == answer.starts);

    const Bytes data(1000, 0x5A);
    const Bytes wire = encode(PeerMessage{Chunk{7, 2, 1, 4096, data}});
    const Chunk c = std::get<Chunk>(decodePeer(wire)); // Its data borrowed from the message.
    CHECK(c.id == 7 && c.epoch == 2 && c.item == 1 && c.offset == 4096 &&
          Bytes(c.data.begin(), c.data.end()) == data);

    const ItemEnd e = peerRoundTrip(ItemEnd{7, 2, 1, RCS_ERROR_DEVICE, "unreadable"});
    CHECK(e.item == 1 && e.result == RCS_ERROR_DEVICE && e.message == "unreadable");
    CHECK(peerRoundTrip(ItemEnd{7, 2, 1, RCS_OK, {}}).result == RCS_OK);
    const Ack k = peerRoundTrip(Ack{7, 3, 1u << 20});
    CHECK(k.id == 7 && k.item == 3 && k.written == 1u << 20);
    const ItemDone d = peerRoundTrip(ItemDone{7, 1, RCS_FILE_ITEM_COMPLETED, RCS_OK, {}, true});
    CHECK(d.state == RCS_FILE_ITEM_COMPLETED && d.earlier);
    CHECK(peerRoundTrip(ItemDone{7, 1, RCS_FILE_ITEM_SKIPPED, RCS_ERROR_UNSUPPORTED, "link", false}).result ==
          RCS_ERROR_UNSUPPORTED);
    const Cancel x = peerRoundTrip(Cancel{true, false, 7, RCS_FILE_REJECTED, RCS_ERROR_TIMEOUT, "too slow"});
    CHECK(x.sending && x.end == RCS_FILE_REJECTED && x.result == RCS_ERROR_TIMEOUT && x.message == "too slow");
    // A receiver that completed a transfer before its sender heard answers the sender's offer so.
    const Cancel done = peerRoundTrip(Cancel{false, false, 7, RCS_FILE_COMPLETED, RCS_OK, {}});
    CHECK(!done.sending && done.end == RCS_FILE_COMPLETED && done.result == RCS_OK);
    const Pause p = peerRoundTrip(Pause{false, 7, true});
    CHECK(!p.sending && p.id == 7 && p.paused);

    // Publications: a fetch, its offer, its reads and their bytes; a fetch
    // refused, or ended before its offer, by the fetcher's number.
    const Fetch f = peerRoundTrip(Fetch{11, token(5), RCS_FILE_FETCH_READ});
    CHECK(f.id == 11 && f.publication == token(5) && f.mode == RCS_FILE_FETCH_READ);
    Offer fetched = offer;
    fetched.resuming = false;
    fetched.fetch = 11;
    fetched.mode = RCS_FILE_FETCH_INTO;
    const Offer fo = peerRoundTrip(fetched);
    CHECK(fo.fetch == 11 && fo.mode == RCS_FILE_FETCH_INTO && fo.items == someItems() && fo.known == 2);
    CHECK(peerRoundTrip(offer).fetch == 0);
    const Read r = peerRoundTrip(Read{42, 3, 1, 4096, RCS_FILE_READ_MAX});
    CHECK(r.id == 42 && r.serial == 3 && r.item == 1 && r.offset == 4096 && r.length == RCS_FILE_READ_MAX);
    const Bytes piece(300, 0x33);
    const Bytes pieceWire = encode(PeerMessage{ReadData{42, 3, RCS_OK, {}, piece}});
    const ReadData rd = std::get<ReadData>(decodePeer(pieceWire));
    CHECK(rd.serial == 3 && rd.result == RCS_OK && Bytes(rd.data.begin(), rd.data.end()) == piece);
    const ReadData failed = peerRoundTrip(ReadData{42, 4, RCS_ERROR_INVALID_STATE, "it changed", {}});
    CHECK(failed.result == RCS_ERROR_INVALID_STATE && failed.message == "it changed" && failed.data.empty());
    const Cancel refusedFetch = peerRoundTrip(Cancel{true, true, 11, RCS_FILE_REJECTED, RCS_ERROR_BUSY, "busy"});
    CHECK(refusedFetch.sending && refusedFetch.fetch && refusedFetch.id == 11 && refusedFetch.result == RCS_ERROR_BUSY);
    const Cancel endedFetch = peerRoundTrip(Cancel{false, true, 11, RCS_FILE_CANCELLED, RCS_ERROR_CANCELLED, {}});
    CHECK(!endedFetch.sending && endedFetch.fetch);

    // A reason too long is cut at a character's start.
    std::string longText;
    while (longText.size() < 2000) {
        longText += "\xE6\x96\x87";
    }
    const ItemEnd cut = peerRoundTrip(ItemEnd{1, 1, 0, RCS_ERROR_DEVICE, longText});
    CHECK(cut.message.size() <= maxText && utf8(cut.message));
}

void malformedPeerMessages() {
    CHECK(peerRefused({}));
    CHECK(peerRefused({99}));
    const Bytes offer = encode(PeerMessage{Offer{1, false, false, false, token(1), 0, 0, 0, someItems()}});
    for (size_t size = 0; size < offer.size(); ++size) { // Every message cut short.
        CHECK(peerRefused(Bytes(offer.begin(), offer.begin() + ptrdiff_t(size))));
    }
    Bytes trailing = offer;
    trailing.push_back(0);
    CHECK(peerRefused(trailing));
    const auto offerOf = [](std::vector<ItemSpec> items, uint32_t known = 0) {
        return encode(PeerMessage{Offer{1, false, false, false, token(1), known, 0, 0, std::move(items)}});
    };
    // Paths that would land outside, or nowhere on some system.
    for (const char* path : {"../etc/passwd", "/etc/passwd", "C:/x", "a\\b", "CON", "a/", "", "a//b", "x.",
                             "a:b", "f.rcsdk-part"}) {
        CHECK(peerRefused(offerOf({{RCS_FILE_REGULAR, 1, 0, path}})));
    }
    CHECK(peerRefused(offerOf({})));                                        // No items.
    CHECK(peerRefused(offerOf({{4, 0, 0, "a"}})));                          // A kind no version defines.
    CHECK(peerRefused(offerOf({{RCS_FILE_DIRECTORY, 5, 0, "d"}})));         // A directory with a size.
    CHECK(peerRefused(offerOf({{RCS_FILE_OTHER, 5, 0, "l"}})));
    CHECK(peerRefused(offerOf({{RCS_FILE_REGULAR, 1, 0, "a"}, {RCS_FILE_REGULAR, 2, 0, "a"}}))); // Twice.
    CHECK(peerRefused(offerOf({{RCS_FILE_REGULAR, ~uint64_t(0), 0, "a"}, {RCS_FILE_REGULAR, 2, 0, "b"}})));
    CHECK(peerRefused(offerOf({{RCS_FILE_REGULAR, 1, 0, "a"}}, 2)));        // Known past the items.
    Bytes flags = offerOf({{RCS_FILE_REGULAR, 1, 0, "a"}});
    flags[9] = 0x80;
    CHECK(peerRefused(flags));
    // Claiming more items than the message holds.
    Bytes many = offerOf({{RCS_FILE_REGULAR, 1, 0, "a"}});
    const size_t countAt = 1 + 8 + 1 + 16 + 4;
    many[countAt] = 0xFF;
    many[countAt + 1] = 0xFF;
    CHECK(peerRefused(many));

    Answer answer;
    answer.id = 1;
    answer.first = 2;
    answer.starts = {{1, 0}}; // Below first.
    CHECK(peerRefused(encode(PeerMessage{answer})));
    answer.starts = {{3, 0}, {3, 0}}; // Not increasing.
    CHECK(peerRefused(encode(PeerMessage{answer})));
    CHECK(peerRefused(encode(PeerMessage{Chunk{1, 1, 0, 0, {}}})));            // Empty.
    const Bytes tooMuch(chunkBytes + 1, 1);
    CHECK(peerRefused(encode(PeerMessage{Chunk{1, 1, 0, 0, tooMuch}})));
    CHECK(peerRefused(encode(PeerMessage{ItemEnd{1, 1, 0, 5, {}}})));          // A result no version defines.
    CHECK(peerRefused(encode(PeerMessage{ItemEnd{1, 1, 0, -1000, {}}})));
    CHECK(peerRefused(encode(PeerMessage{ItemDone{1, 0, RCS_FILE_ITEM_ACTIVE, RCS_OK, {}, false}})));
    CHECK(peerRefused(encode(PeerMessage{ItemDone{1, 0, RCS_FILE_ITEM_COMPLETED, RCS_ERROR_DEVICE, {}, false}})));
    CHECK(peerRefused(encode(PeerMessage{ItemDone{1, 0, RCS_FILE_ITEM_FAILED, RCS_OK, {}, false}})));
    CHECK(peerRefused(encode(PeerMessage{ItemDone{1, 0, RCS_FILE_ITEM_SKIPPED, RCS_OK, {}, true}})));
    CHECK(peerRefused(encode(PeerMessage{Cancel{false, false, 1, RCS_FILE_COMPLETED, RCS_ERROR_CANCELLED, {}}})));
    CHECK(peerRefused(encode(PeerMessage{Cancel{true, false, 1, RCS_FILE_COMPLETED, RCS_OK, {}}}))); // Only a receiver's.
    CHECK(peerRefused(encode(PeerMessage{Cancel{false, false, 1, RCS_FILE_CANCELLED, RCS_OK, {}}})));
    Bytes pause = encode(PeerMessage{Pause{true, 1, true}});
    pause.back() = 2;
    CHECK(peerRefused(pause));
    // Fetches: a mode no version defines, the number 0, a resumed fetch;
    // reads of nothing or past the most; bytes of a read empty or too many;
    // a fetch's end that says it completed.
    CHECK(peerRefused(encode(PeerMessage{Fetch{1, token(1), 3}})));
    CHECK(peerRefused(encode(PeerMessage{Fetch{0, token(1), RCS_FILE_FETCH_INTO}})));
    CHECK(peerRefused(encode(PeerMessage{Offer{1, false, false, false, token(1), 0, 5, 0, someItems()}})));
    CHECK(peerRefused(encode(PeerMessage{Offer{1, false, true, false, token(1), 0, 5, RCS_FILE_FETCH_READ, someItems()}})));
    CHECK(peerRefused(encode(PeerMessage{Read{1, 1, 0, 0, 0}})));
    CHECK(peerRefused(encode(PeerMessage{Read{1, 1, 0, 0, RCS_FILE_READ_MAX + 1}})));
    CHECK(peerRefused(encode(PeerMessage{ReadData{1, 1, RCS_OK, {}, {}}})));
    const Bytes tooMuchRead(RCS_FILE_READ_MAX + 1, 1);
    CHECK(peerRefused(encode(PeerMessage{ReadData{1, 1, RCS_OK, {}, tooMuchRead}})));
    CHECK(peerRefused(encode(PeerMessage{Cancel{false, true, 1, RCS_FILE_COMPLETED, RCS_OK, {}}})));
    Bytes whose = encode(PeerMessage{Cancel{true, true, 1, RCS_FILE_REJECTED, RCS_ERROR_BUSY, {}}});
    whose[1] = 4;
    CHECK(peerRefused(whose));
    Bytes text = encode(PeerMessage{ItemEnd{1, 1, 0, RCS_ERROR_DEVICE, "ok"}});
    text[text.size() - 1] = 0xFF; // Not UTF-8.
    CHECK(peerRefused(text));
}

void machineMessages() {
    const ListRequest list = requestRoundTrip(ListRequest{5, 100, {{"/home/me/a", ""}, {"/home/me/b", "x/y"}}});
    CHECK(list.job == 5 && list.maxItems == 100 && list.entries.size() == 2 && list.entries[1].second == "x/y");
    const PrepareRequest prepare =
        requestRoundTrip(PrepareRequest{6, "/tmp/in", RCS_FILE_CONFLICT_SKIP, token(2), false, true, someItems()});
    CHECK(prepare.directory == "/tmp/in" && prepare.conflict == RCS_FILE_CONFLICT_SKIP && prepare.keep &&
          !prepare.resuming && prepare.items == someItems());
    CHECK(requestRoundTrip(PrepareRequest{6, "/tmp/in", RCS_FILE_CONFLICT_SKIP, token(2), true, false, someItems()})
              .resuming);
    const ReadRequest read = requestRoundTrip(ReadRequest{6, 1, 1, "x/c", "/a", "b/c", 1000, 9, 512, 488});
    CHECK(read.name == "x/c" && read.base == "/a" && read.relative == "b/c" && read.offset == 512 &&
          read.length == 488 && read.modified == 9);
    const WriteRequest write = requestRoundTrip(
        WriteRequest{6, 1, 1, "/tmp/in", {RCS_FILE_REGULAR, 10, 0, "a"}, token(1), false, false, 4, Bytes{1, 2, 3}});
    CHECK(write.offset == 4 && write.data == Bytes({1, 2, 3}) && write.spec.size == 10 && !write.fresh);
    CHECK(requestRoundTrip(
              WriteRequest{6, 1, 1, "/tmp/in", {RCS_FILE_REGULAR, 10, 0, "a"}, token(1), true, true, 0, Bytes{1}})
              .fresh);
    const FinishRequest finish = requestRoundTrip(FinishRequest{6, 1, 2, "/tmp/in", {RCS_FILE_DIRECTORY, 0, 0, "d"},
                                                                RCS_FILE_CONFLICT_RENAME, token(1), true});
    CHECK(finish.spec.kind == RCS_FILE_DIRECTORY && finish.conflict == RCS_FILE_CONFLICT_RENAME);
    const DiscardRequest discard = requestRoundTrip(DiscardRequest{6, "/tmp/in", token(1), false, {"a", "b/c"}});
    CHECK(!discard.ended && discard.paths == std::vector<std::string>({"a", "b/c"}));
    CHECK(requestRoundTrip(DiscardRequest{6, "/tmp/in", token(1), true, {}}).ended);
    const CompleteRequest complete = requestRoundTrip(CompleteRequest{6, "/tmp/in", token(1), {{"d", 5}}});
    CHECK(complete.directories.size() == 1 && complete.directories[0].second == 5);
    CHECK(requestRoundTrip(CloseRequest{6}).transfer == 6);
    CHECK(requestRoundTrip(PurgeRequest{8, "/tmp/in", token(4)}).token == token(4));

    const Listed listed = replyRoundTrip(
        Listed{5, RCS_OK, {}, someItems(), {"/p"}, {{0, ""}, {0, "a.jpg"}, {0, "empty"}, {0, ""}}});
    CHECK(listed.items == someItems() && listed.bases == std::vector<std::string>({"/p"}) &&
          listed.sources[1].first == 0 && listed.sources[1].second == "a.jpg");
    CHECK(replyRoundTrip(Listed{5, RCS_ERROR_INVALID_ARGUMENT, "not there", {}, {}, {}}).message == "not there");
    Prepared prepared;
    prepared.transfer = 6;
    prepared.starts = {{1, Start::resume, 99, RCS_OK, {}},
                       {2, Start::done, 0, RCS_OK, "photos/empty (1)"},
                       {4, Start::failed, 0, RCS_ERROR_INVALID_STATE, "a directory"}};
    const Prepared p = replyRoundTrip(prepared);
    CHECK(p.starts.size() == 3 && p.starts[0].offset == 99 && p.starts[1].text == "photos/empty (1)" &&
          p.starts[2].result == RCS_ERROR_INVALID_STATE);
    CHECK(replyRoundTrip(Data{6, 1, 1, 0, Bytes(10, 7)}).data.size() == 10);
    CHECK(replyRoundTrip(ReadFailed{6, 1, 1, RCS_ERROR_DEVICE, "x"}).result == RCS_ERROR_DEVICE);
    CHECK(replyRoundTrip(Written{6, 1, 1, 77}).offset == 77);
    CHECK(replyRoundTrip(WriteFailed{6, 1, 1, RCS_ERROR_DEVICE, "full"}).message == "full");
    Finished done;
    done.transfer = 6;
    done.saved = "a (1).txt";
    CHECK(replyRoundTrip(done).saved == "a (1).txt");
    Finished whole;
    whole.state = RCS_FILE_ITEM_FAILED;
    whole.result = RCS_ERROR_INVALID_STATE;
    whole.whole = true;
    CHECK(replyRoundTrip(whole).whole);
    CHECK(replyRoundTrip(Ended{6}).transfer == 6);
    CHECK(encode(Reply{Ended{6}}).size() == 9); // Its type and the transfer.
    CHECK(replyRoundTrip(Purged{8, RCS_OK, {}, 3}).files == 3);

    // An agent of another process may be anyone's: the same checks.
    CHECK(requestRefused({}));
    CHECK(requestRefused(encode(Request{ListRequest{5, 100, {}}})));            // Nothing to list.
    CHECK(requestRefused(encode(Request{ListRequest{5, 100, {{"", ""}}}})));    // An empty path.
    CHECK(requestRefused(encode(Request{ListRequest{5, 100, {{"/a", "../b"}}}})));
    CHECK(requestRefused(encode(Request{ReadRequest{6, 1, 1, "b", "/a", "b", 100, 0, 90, 20}}))); // Past its end.
    CHECK(requestRefused(encode(Request{ReadRequest{6, 1, 1, "b", "/a", "b", 1u << 30, 0, 0, chunkBytes + 1}})));
    // A path of the machine as what messages call it.
    CHECK(requestRefused(encode(Request{ReadRequest{6, 1, 1, "/a/b", "/a", "b", 100, 0, 0, 20}})));
    CHECK(requestRefused(encode(Request{
        WriteRequest{6, 1, 1, "/t", {RCS_FILE_REGULAR, 2, 0, "a"}, token(1), false, false, 1, Bytes{1, 2}}})));
    CHECK(requestRefused(encode(Request{
        WriteRequest{6, 1, 1, "/t", {RCS_FILE_DIRECTORY, 0, 0, "a"}, token(1), false, true, 0, Bytes{1}}})));
    CHECK(requestRefused(encode(Request{DiscardRequest{6, "/t", token(1), true, {"../x"}}})));
    CHECK(requestRefused(encode(Request{PrepareRequest{6, "/t", 9, token(1), false, false, someItems()}})));
    Finished wrong = done;
    wrong.saved = "../escape";
    CHECK(replyRefused(encode(Reply{wrong})));
    wrong.saved.clear(); // Completed, but where to?
    CHECK(replyRefused(encode(Reply{wrong})));
    Finished skippedWhole;
    skippedWhole.state = RCS_FILE_ITEM_SKIPPED;
    skippedWhole.whole = true;
    CHECK(replyRefused(encode(Reply{skippedWhole})));
    Prepared outOfOrder;
    outOfOrder.starts = {{2, Start::resume, 1, RCS_OK, {}}, {1, Start::resume, 1, RCS_OK, {}}};
    CHECK(replyRefused(encode(Reply{outOfOrder})));
    // A source for a directory, a file without one, an entry past the bases, no base.
    CHECK(replyRefused(encode(Reply{Listed{5, RCS_OK, {}, someItems(), {"/p"}, {{0, "d"}, {0, "a"}, {0, "e"}, {0, ""}}}})));
    CHECK(replyRefused(encode(Reply{Listed{5, RCS_OK, {}, someItems(), {"/p"}, {{0, ""}, {0, ""}, {0, "e"}, {0, ""}}}})));
    CHECK(replyRefused(encode(Reply{Listed{5, RCS_OK, {}, someItems(), {"/p"}, {{0, ""}, {1, "a"}, {0, "e"}, {0, ""}}}})));
    CHECK(replyRefused(encode(Reply{Listed{5, RCS_OK, {}, someItems(), {}, {{0, ""}, {0, "a"}, {0, "e"}, {0, ""}}}})));
    Bytes ended = encode(Reply{Ended{6}});
    CHECK(replyRefused(Bytes(ended.begin(), ended.end() - 1))); // Cut short.
    ended.push_back(0);
    CHECK(replyRefused(ended)); // A byte too many.
}

} // namespace

int main() {
    return runTest("file_transfer.messages", [] {
        hello();
        peerMessages();
        malformedPeerMessages();
        machineMessages();
    });
}
