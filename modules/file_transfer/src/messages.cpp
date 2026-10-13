#include "messages.hpp"

#include "paths.hpp"

#include <unordered_set>

namespace rcsdk::file_transfer {
namespace {

enum : uint8_t { offerType = 1, answerType, chunkType, itemEndType, ackType, itemDoneType, cancelType, pauseType,
                 fetchType, pieceType, pieceDataType };
enum : uint8_t { listType = 1, prepareType, readType, writeType, finishType, discardType, completeType, closeType,
                 purgeType };
enum : uint8_t { listedType = 1, preparedType, dataType, readFailedType, writtenType, writeFailedType, finishedType,
                 endedType, purgedType };
enum : uint8_t { againFlag = 1, resumingFlag = 2, pausedFlag = 4, fetchFlag = 8 };
// Bits of a CANCEL: its sender sends the files; the id is the fetcher's number of a FETCH.
enum : uint8_t { senderCancel = 1, fetchCancel = 2 };

constexpr size_t helloBytes = 1 + RCS_FILE_TOKEN_SIZE + 4 + 8;
constexpr size_t maxMachinePath = 32768; // Bytes of a path on the machine (Windows' long ones).
constexpr size_t maxRelative = RCS_FILE_PATH_MAX - 1;

void put(Writer& w, const ItemSpec& item) {
    w.u8(uint8_t(item.kind)).u64(item.size).i64(item.modified).text(item.path, maxRelative);
}

void putAll(Writer& w, const std::vector<ItemSpec>& items) {
    w.u32(uint32_t(items.size()));
    for (const auto& item : items) {
        put(w, item);
    }
}

std::string relative(Reader& r) {
    std::string path = r.text(maxRelative);
    if (const auto problem = pathProblem(path)) {
        r.invalid("a path that is not taken: " + *problem);
    }
    return path;
}

ItemSpec spec(Reader& r, bool anyKind = true) {
    ItemSpec item;
    item.kind = r.u8();
    item.size = r.u64();
    item.modified = r.i64();
    item.path = relative(r);
    if (item.kind < RCS_FILE_REGULAR || item.kind > (anyKind ? RCS_FILE_OTHER : RCS_FILE_DIRECTORY)) {
        r.invalid("an item of a kind no version defines");
    }
    if (item.kind != RCS_FILE_REGULAR && item.size) {
        r.invalid("a size for an item that is not a file");
    }
    return item;
}

// Items: 1..maxItemsLimit, each path once.
std::vector<ItemSpec> specs(Reader& r) {
    const uint32_t count = r.u32();
    if (!count || count > maxItemsLimit) {
        r.invalid("1..100 000 items");
    }
    if (count > r.remaining() / 20) { // Each takes 20 bytes or more: no space for a lie.
        r.invalid("it ends early");
    }
    std::vector<ItemSpec> items;
    items.reserve(count);
    std::unordered_set<std::string> seen;
    for (uint32_t i = 0; i < count; ++i) {
        items.push_back(spec(r));
        if (!seen.insert(items.back().path).second) {
            r.invalid("a path twice");
        }
    }
    totalBytes(items);
    return items;
}

std::string machinePath(Reader& r) {
    std::string path = r.text(maxMachinePath);
    if (path.empty()) {
        r.invalid("an empty path");
    }
    return path;
}

rcs_file_conflict conflict(Reader& r) {
    const uint8_t value = r.u8();
    if (value < RCS_FILE_CONFLICT_OVERWRITE || value > RCS_FILE_CONFLICT_FAIL) {
        r.invalid("a conflict rule no version defines");
    }
    return value;
}

uint32_t length(Reader& r, uint64_t offset, uint64_t size) {
    const uint32_t value = r.u32();
    if (!value || value > chunkBytes || offset > size || value > size - offset) {
        r.invalid("a piece outside its item");
    }
    return value;
}

std::span<const uint8_t> piece(Reader& r) {
    const auto data = r.rest();
    if (data.empty() || data.size() > chunkBytes) {
        r.invalid("1..256 KiB of data");
    }
    return data;
}

rcs_file_item_state outcome(Reader& r) {
    const uint8_t value = r.u8();
    if (value < RCS_FILE_ITEM_COMPLETED || value > RCS_FILE_ITEM_FAILED) {
        r.invalid("an outcome no version defines");
    }
    return value;
}

rcs_file_fetch_mode fetchMode(Reader& r) {
    const uint8_t value = r.u8();
    if (value != RCS_FILE_FETCH_INTO && value != RCS_FILE_FETCH_READ) {
        r.invalid("a fetch's mode no version defines");
    }
    return value;
}

} // namespace

uint64_t totalBytes(const std::vector<ItemSpec>& items) {
    uint64_t total = 0;
    for (const auto& item : items) {
        if (item.size > ~uint64_t(0) - total) {
            fail(RCS_ERROR_PROTOCOL, "Invalid file transfer message: its sizes add up past 2^64");
        }
        total += item.size;
    }
    return total;
}

// ---- To the other end -------------------------------------------------------------

Bytes encode(const Hello& hello) {
    return Writer().u8(uint8_t(hello.flow)).token(hello.end).u32(hello.maxItems).u64(hello.maxOfferBytes).take();
}

Hello decodeHello(std::span<const uint8_t> bytes) {
    Reader r(bytes, "hello");
    if (bytes.size() != helloBytes) {
        r.invalid("not " + std::to_string(helloBytes) + " bytes");
    }
    Hello hello;
    hello.flow = r.u8();
    hello.end = r.token();
    hello.maxItems = r.u32();
    hello.maxOfferBytes = r.u64();
    if (hello.flow & ~uint32_t(RCS_FILE_SEND | RCS_FILE_RECEIVE)) {
        r.invalid("a flow no version defines");
    }
    if (!hello.maxItems || hello.maxItems > maxItemsLimit) {
        r.invalid("max_items out of range");
    }
    return hello;
}

Bytes encode(const PeerMessage& message) {
    Writer w;
    if (const auto* m = std::get_if<Offer>(&message)) {
        w.u8(offerType).u64(m->id);
        w.u8(uint8_t((m->again ? againFlag : 0) | (m->resuming ? resumingFlag : 0) | (m->paused ? pausedFlag : 0) |
                     (m->fetch ? fetchFlag : 0)));
        w.token(m->token).u32(m->known);
        if (m->fetch) {
            w.u64(m->fetch).u8(uint8_t(m->mode));
        }
        putAll(w, m->items);
    } else if (const auto* m = std::get_if<Answer>(&message)) {
        w.u8(answerType).u64(m->id).u32(m->epoch).u8(m->paused ? 1 : 0).u32(m->first).u32(uint32_t(m->starts.size()));
        for (const auto& [item, offset] : m->starts) {
            w.u32(item).u64(offset);
        }
    } else if (const auto* m = std::get_if<Chunk>(&message)) {
        w.u8(chunkType).u64(m->id).u32(m->epoch).u32(m->item).u64(m->offset).raw(m->data);
    } else if (const auto* m = std::get_if<ItemEnd>(&message)) {
        w.u8(itemEndType).u64(m->id).u32(m->epoch).u32(m->item).i32(m->result).text(m->message);
    } else if (const auto* m = std::get_if<Ack>(&message)) {
        w.u8(ackType).u64(m->id).u32(m->item).u64(m->written);
    } else if (const auto* m = std::get_if<ItemDone>(&message)) {
        w.u8(itemDoneType).u64(m->id).u32(m->item).u8(uint8_t(m->state)).i32(m->result).text(m->message);
        w.u8(m->earlier ? 1 : 0);
    } else if (const auto* m = std::get_if<Cancel>(&message)) {
        w.u8(cancelType).u8(uint8_t((m->sending ? senderCancel : 0) | (m->fetch ? fetchCancel : 0))).u64(m->id);
        w.u8(uint8_t(m->end)).i32(m->result).text(m->message);
    } else if (const auto* m = std::get_if<Pause>(&message)) {
        w.u8(pauseType).u8(m->sending ? 1 : 0).u64(m->id).u8(m->paused ? 1 : 0);
    } else if (const auto* m = std::get_if<Fetch>(&message)) {
        w.u8(fetchType).u64(m->id).token(m->publication).u8(uint8_t(m->mode));
    } else if (const auto* m = std::get_if<Read>(&message)) {
        w.u8(pieceType).u64(m->id).u64(m->serial).u32(m->item).u64(m->offset).u32(m->length);
    } else {
        const auto& d = std::get<ReadData>(message);
        w.u8(pieceDataType).u64(d.id).u64(d.serial).i32(d.result);
        if (d.result == RCS_OK) {
            w.raw(d.data);
        } else {
            w.text(d.message);
        }
    }
    return w.take();
}

PeerMessage decodePeer(std::span<const uint8_t> bytes) {
    Reader r(bytes, "from the other end");
    switch (r.u8()) {
    case offerType: {
        Offer m;
        m.id = r.u64();
        const uint8_t flags = r.u8();
        if (flags & ~uint8_t(againFlag | resumingFlag | pausedFlag | fetchFlag)) {
            r.invalid("an offer's flags");
        }
        m.again = flags & againFlag;
        m.resuming = flags & resumingFlag;
        m.paused = flags & pausedFlag;
        m.token = r.token();
        m.known = r.u32();
        if (flags & fetchFlag) {
            m.fetch = r.u64();
            m.mode = fetchMode(r);
            if (!m.fetch || m.resuming) {
                r.invalid("a fetch's offer");
            }
        }
        m.items = specs(r);
        r.end();
        if (m.known > m.items.size()) {
            r.invalid("an offer's known items");
        }
        return m;
    }
    case answerType: {
        Answer m;
        m.id = r.u64();
        m.epoch = r.u32();
        m.paused = r.flag();
        m.first = r.u32();
        const uint32_t count = r.u32();
        if (count > maxItemsLimit || count > r.remaining() / 12) {
            r.invalid("an answer's items");
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t item = r.u32();
            const uint64_t offset = r.u64();
            if (item < m.first || (!m.starts.empty() && item <= m.starts.back().first)) {
                r.invalid("an answer's items out of order");
            }
            m.starts.emplace_back(item, offset);
        }
        r.end();
        return m;
    }
    case chunkType: {
        Chunk m;
        m.id = r.u64();
        m.epoch = r.u32();
        m.item = r.u32();
        m.offset = r.u64();
        m.data = piece(r);
        return m;
    }
    case itemEndType: {
        ItemEnd m;
        m.id = r.u64();
        m.epoch = r.u32();
        m.item = r.u32();
        m.result = r.result(true);
        m.message = r.text();
        r.end();
        return m;
    }
    case ackType: {
        Ack m;
        m.id = r.u64();
        m.item = r.u32();
        m.written = r.u64();
        r.end();
        return m;
    }
    case itemDoneType: {
        ItemDone m;
        m.id = r.u64();
        m.item = r.u32();
        m.state = outcome(r);
        m.result = r.result(m.state != RCS_FILE_ITEM_FAILED);
        if (m.state == RCS_FILE_ITEM_COMPLETED && m.result != RCS_OK) {
            r.invalid("a completed item that failed");
        }
        m.message = r.text();
        m.earlier = r.flag();
        r.end();
        if (m.earlier && m.state != RCS_FILE_ITEM_COMPLETED) {
            r.invalid("an earlier run's item that is not completed");
        }
        return m;
    }
    case cancelType: {
        Cancel m;
        const uint8_t whose = r.u8();
        if (whose & ~(senderCancel | fetchCancel)) {
            r.invalid("whose transfer a cancel names");
        }
        m.sending = whose & senderCancel;
        m.fetch = whose & fetchCancel;
        m.id = r.u64();
        m.end = r.u8();
        if (m.end < RCS_FILE_COMPLETED || m.end > RCS_FILE_FAILED) {
            r.invalid("an end no version defines");
        }
        // COMPLETED: only the receiver's, without an error (an offer of a
        // transfer it completed, a READ fetch it closed).
        const bool completed = m.end == RCS_FILE_COMPLETED;
        if (completed && (m.sending || m.fetch)) {
            r.invalid("a sender's completed end");
        }
        m.result = r.result(completed);
        if (completed && m.result != RCS_OK) {
            r.invalid("a completed end with an error");
        }
        m.message = r.text();
        r.end();
        return m;
    }
    case pauseType: {
        Pause m;
        m.sending = r.flag();
        m.id = r.u64();
        m.paused = r.flag();
        r.end();
        return m;
    }
    case fetchType: {
        Fetch m;
        m.id = r.u64();
        m.publication = r.token();
        m.mode = fetchMode(r);
        r.end();
        if (!m.id) {
            r.invalid("a fetch numbered 0");
        }
        return m;
    }
    case pieceType: {
        Read m;
        m.id = r.u64();
        m.serial = r.u64();
        m.item = r.u32();
        m.offset = r.u64();
        m.length = r.u32();
        r.end();
        if (!m.length || m.length > RCS_FILE_READ_MAX) {
            r.invalid("a read of 1..256 KiB");
        }
        return m;
    }
    case pieceDataType: {
        ReadData m;
        m.id = r.u64();
        m.serial = r.u64();
        m.result = r.result(true);
        if (m.result == RCS_OK) {
            m.data = r.rest();
            if (m.data.empty() || m.data.size() > RCS_FILE_READ_MAX) {
                r.invalid("1..256 KiB read");
            }
        } else {
            m.message = r.text();
            r.end();
        }
        return m;
    }
    default:
        r.invalid("a type no version defines");
    }
}

// ---- Between the halves -------------------------------------------------------------

Bytes encode(const Request& request) {
    Writer w;
    if (const auto* m = std::get_if<ListRequest>(&request)) {
        w.u8(listType).u64(m->job).u32(m->maxItems).u32(uint32_t(m->entries.size()));
        for (const auto& [source, name] : m->entries) {
            w.text(source, maxMachinePath).text(name, maxRelative);
        }
    } else if (const auto* m = std::get_if<PrepareRequest>(&request)) {
        w.u8(prepareType).u64(m->transfer).text(m->directory, maxMachinePath).u8(uint8_t(m->conflict));
        w.token(m->token).u8(m->resuming ? 1 : 0).u8(m->keep ? 1 : 0);
        putAll(w, m->items);
    } else if (const auto* m = std::get_if<ReadRequest>(&request)) {
        w.u8(readType).u64(m->transfer).u32(m->epoch).u32(m->item).text(m->name, maxRelative);
        w.text(m->base, maxMachinePath).text(m->relative, maxMachinePath);
        w.u64(m->size).i64(m->modified).u64(m->offset).u32(m->length);
    } else if (const auto* m = std::get_if<WriteRequest>(&request)) {
        w.u8(writeType).u64(m->transfer).u32(m->epoch).u32(m->item).text(m->directory, maxMachinePath);
        put(w, m->spec);
        w.token(m->token).u8(m->keep ? 1 : 0).u8(m->fresh ? 1 : 0).u64(m->offset).raw(m->data);
    } else if (const auto* m = std::get_if<FinishRequest>(&request)) {
        w.u8(finishType).u64(m->transfer).u32(m->epoch).u32(m->item).text(m->directory, maxMachinePath);
        put(w, m->spec);
        w.u8(uint8_t(m->conflict)).token(m->token).u8(m->keep ? 1 : 0);
    } else if (const auto* m = std::get_if<DiscardRequest>(&request)) {
        w.u8(discardType).u64(m->transfer).text(m->directory, maxMachinePath).token(m->token);
        w.u8(m->ended ? 1 : 0).u32(uint32_t(m->paths.size()));
        for (const auto& path : m->paths) {
            w.text(path, maxRelative);
        }
    } else if (const auto* m = std::get_if<CompleteRequest>(&request)) {
        w.u8(completeType).u64(m->transfer).text(m->directory, maxMachinePath).token(m->token);
        w.u32(uint32_t(m->directories.size()));
        for (const auto& [path, modified] : m->directories) {
            w.text(path, maxRelative).i64(modified);
        }
    } else if (const auto* m = std::get_if<CloseRequest>(&request)) {
        w.u8(closeType).u64(m->transfer);
    } else {
        const auto& p = std::get<PurgeRequest>(request);
        w.u8(purgeType).u64(p.job).text(p.directory, maxMachinePath).token(p.token);
    }
    return w.take();
}

Request decodeRequest(std::span<const uint8_t> bytes) {
    Reader r(bytes, "from the session's half");
    switch (r.u8()) {
    case listType: {
        ListRequest m;
        m.job = r.u64();
        m.maxItems = r.u32();
        const uint32_t count = r.u32();
        if (!m.maxItems || m.maxItems > maxItemsLimit || !count || count > m.maxItems ||
            count > r.remaining() / 4) {
            r.invalid("a list's entries");
        }
        for (uint32_t i = 0; i < count; ++i) {
            std::string source = machinePath(r);
            std::string name = r.text(maxRelative);
            if (!name.empty() && pathProblem(name)) {
                r.invalid("a name that is not taken");
            }
            m.entries.emplace_back(std::move(source), std::move(name));
        }
        r.end();
        return m;
    }
    case prepareType: {
        PrepareRequest m;
        m.transfer = r.u64();
        m.directory = machinePath(r);
        m.conflict = conflict(r);
        m.token = r.token();
        m.resuming = r.flag();
        m.keep = r.flag();
        m.items = specs(r);
        r.end();
        return m;
    }
    case readType: {
        ReadRequest m;
        m.transfer = r.u64();
        m.epoch = r.u32();
        m.item = r.u32();
        m.name = relative(r);
        m.base = machinePath(r);
        m.relative = machinePath(r);
        m.size = r.u64();
        m.modified = r.i64();
        m.offset = r.u64();
        m.length = length(r, m.offset, m.size);
        r.end();
        return m;
    }
    case writeType: {
        WriteRequest m;
        m.transfer = r.u64();
        m.epoch = r.u32();
        m.item = r.u32();
        m.directory = machinePath(r);
        m.spec = spec(r, false);
        m.token = r.token();
        m.keep = r.flag();
        m.fresh = r.flag();
        m.offset = r.u64();
        const auto data = piece(r);
        if (m.spec.kind != RCS_FILE_REGULAR || m.offset > m.spec.size || data.size() > m.spec.size - m.offset) {
            r.invalid("a piece outside its item");
        }
        m.data.assign(data.begin(), data.end());
        return m;
    }
    case finishType: {
        FinishRequest m;
        m.transfer = r.u64();
        m.epoch = r.u32();
        m.item = r.u32();
        m.directory = machinePath(r);
        m.spec = spec(r, false);
        m.conflict = conflict(r);
        m.token = r.token();
        m.keep = r.flag();
        r.end();
        return m;
    }
    case discardType: {
        DiscardRequest m;
        m.transfer = r.u64();
        m.directory = machinePath(r);
        m.token = r.token();
        m.ended = r.flag();
        const uint32_t count = r.u32();
        if (count > maxItemsLimit || count > r.remaining() / 2) {
            r.invalid("a discard's paths");
        }
        for (uint32_t i = 0; i < count; ++i) {
            m.paths.push_back(relative(r));
        }
        r.end();
        return m;
    }
    case completeType: {
        CompleteRequest m;
        m.transfer = r.u64();
        m.directory = machinePath(r);
        m.token = r.token();
        const uint32_t count = r.u32();
        if (count > maxItemsLimit || count > r.remaining() / 10) {
            r.invalid("a completion's directories");
        }
        for (uint32_t i = 0; i < count; ++i) {
            std::string path = relative(r);
            m.directories.emplace_back(std::move(path), r.i64());
        }
        r.end();
        return m;
    }
    case closeType: {
        CloseRequest m;
        m.transfer = r.u64();
        r.end();
        return m;
    }
    case purgeType: {
        PurgeRequest m;
        m.job = r.u64();
        m.directory = machinePath(r);
        m.token = r.token();
        r.end();
        return m;
    }
    default:
        r.invalid("a type no version defines");
    }
}

Bytes encode(const Reply& reply) {
    Writer w;
    if (const auto* m = std::get_if<Listed>(&reply)) {
        w.u8(listedType).u64(m->job).i32(m->result).text(m->message);
        if (m->result == RCS_OK) {
            putAll(w, m->items);
            w.u32(uint32_t(m->bases.size()));
            for (const auto& base : m->bases) {
                w.text(base, maxMachinePath);
            }
            for (const auto& [entry, relative] : m->sources) {
                w.u32(entry).text(relative, maxMachinePath);
            }
        }
    } else if (const auto* m = std::get_if<Prepared>(&reply)) {
        w.u8(preparedType).u64(m->transfer).i32(m->result).text(m->message).u32(uint32_t(m->starts.size()));
        for (const auto& s : m->starts) {
            w.u32(s.item).u8(s.what).u64(s.offset).i32(s.result).text(s.text, maxRelative);
        }
    } else if (const auto* m = std::get_if<Data>(&reply)) {
        w.u8(dataType).u64(m->transfer).u32(m->epoch).u32(m->item).u64(m->offset).raw(m->data);
    } else if (const auto* m = std::get_if<ReadFailed>(&reply)) {
        w.u8(readFailedType).u64(m->transfer).u32(m->epoch).u32(m->item).i32(m->result).text(m->message);
    } else if (const auto* m = std::get_if<Written>(&reply)) {
        w.u8(writtenType).u64(m->transfer).u32(m->epoch).u32(m->item).u64(m->offset);
    } else if (const auto* m = std::get_if<WriteFailed>(&reply)) {
        w.u8(writeFailedType).u64(m->transfer).u32(m->epoch).u32(m->item).i32(m->result).text(m->message);
    } else if (const auto* m = std::get_if<Finished>(&reply)) {
        w.u8(finishedType).u64(m->transfer).u32(m->epoch).u32(m->item).u8(uint8_t(m->state)).i32(m->result);
        w.text(m->message).text(m->saved, maxRelative).u8(m->whole ? 1 : 0);
    } else if (const auto* m = std::get_if<Ended>(&reply)) {
        w.u8(endedType).u64(m->transfer);
    } else {
        const auto& p = std::get<Purged>(reply);
        w.u8(purgedType).u64(p.job).i32(p.result).text(p.message).u32(p.files);
    }
    return w.take();
}

Reply decodeReply(std::span<const uint8_t> bytes) {
    Reader r(bytes, "from the machine's half");
    switch (r.u8()) {
    case listedType: {
        Listed m;
        m.job = r.u64();
        m.result = r.result(true);
        m.message = r.text();
        if (m.result == RCS_OK) {
            m.items = specs(r);
            const uint32_t bases = r.u32();
            if (!bases || bases > m.items.size() || bases > r.remaining() / 3) {
                r.invalid("a list's entries");
            }
            for (uint32_t i = 0; i < bases; ++i) {
                m.bases.push_back(machinePath(r));
            }
            for (const auto& item : m.items) {
                const uint32_t entry = r.u32();
                std::string relative = r.text(maxMachinePath);
                if (entry >= bases || relative.empty() != (item.kind != RCS_FILE_REGULAR)) {
                    r.invalid("a file without its path, or a path for what is not a file");
                }
                m.sources.emplace_back(entry, std::move(relative));
            }
        }
        r.end();
        return m;
    }
    case preparedType: {
        Prepared m;
        m.transfer = r.u64();
        m.result = r.result(true);
        m.message = r.text();
        const uint32_t count = r.u32();
        if (count > maxItemsLimit || count > r.remaining() / 19) {
            r.invalid("a preparation's items");
        }
        for (uint32_t i = 0; i < count; ++i) {
            Start s;
            s.item = r.u32();
            const uint8_t what = r.u8();
            if (what < Start::resume || what > Start::failed) {
                r.invalid("a start no version defines");
            }
            s.what = Start::What(what);
            s.offset = r.u64();
            s.result = r.result(s.what != Start::failed);
            s.text = r.text(maxRelative);
            if ((!m.starts.empty() && s.item <= m.starts.back().item) ||
                (s.what == Start::done && pathProblem(s.text))) {
                r.invalid("a preparation's items");
            }
            m.starts.push_back(std::move(s));
        }
        r.end();
        return m;
    }
    case dataType: {
        Data m;
        m.transfer = r.u64();
        m.epoch = r.u32();
        m.item = r.u32();
        m.offset = r.u64();
        const auto data = piece(r);
        m.data.assign(data.begin(), data.end());
        return m;
    }
    case readFailedType:
    case writeFailedType: {
        const bool reading = bytes[0] == readFailedType;
        const uint64_t transfer = r.u64();
        const uint32_t epoch = r.u32();
        const uint32_t item = r.u32();
        const rcs_result result = r.result();
        std::string message = r.text();
        r.end();
        if (reading) {
            return ReadFailed{transfer, epoch, item, result, std::move(message)};
        }
        return WriteFailed{transfer, epoch, item, result, std::move(message)};
    }
    case writtenType: {
        Written m;
        m.transfer = r.u64();
        m.epoch = r.u32();
        m.item = r.u32();
        m.offset = r.u64();
        r.end();
        return m;
    }
    case finishedType: {
        Finished m;
        m.transfer = r.u64();
        m.epoch = r.u32();
        m.item = r.u32();
        m.state = outcome(r);
        m.result = r.result(m.state != RCS_FILE_ITEM_FAILED);
        m.message = r.text();
        m.saved = r.text(maxRelative);
        m.whole = r.flag();
        if ((m.state == RCS_FILE_ITEM_COMPLETED) != (m.result == RCS_OK && !m.saved.empty()) ||
            (!m.saved.empty() && pathProblem(m.saved)) || (m.whole && m.state != RCS_FILE_ITEM_FAILED)) {
            r.invalid("a finished item");
        }
        r.end();
        return m;
    }
    case endedType: {
        Ended m;
        m.transfer = r.u64();
        r.end();
        return m;
    }
    case purgedType: {
        Purged m;
        m.job = r.u64();
        m.result = r.result(true);
        m.message = r.text();
        m.files = r.u32();
        r.end();
        return m;
    }
    default:
        r.invalid("a type no version defines");
    }
}

} // namespace rcsdk::file_transfer
