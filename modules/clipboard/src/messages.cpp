#include "messages.hpp"

namespace rcsdk::clipboard {
namespace {

enum : uint8_t { copyType = 1, withdrawnType = 2 };
enum : uint8_t { useType = 1, stopType = 2, placeType = 3, listedType = 4, dataType = 5, endedType = 6,
                 revokedType = 7 };
enum : uint8_t { copiedType = 1, readFailedType = 2, placedType = 3, abilitiesType = 4, wantType = 5, readType = 6,
                 closeType = 7, refusedType = 8 };
constexpr size_t maxText = 1024;
constexpr size_t maxPath = 32768; // Bytes of a path on the machine (Windows' long ones).

[[noreturn]] void invalid(const char* what) {
    fail(RCS_ERROR_PROTOCOL, std::string("Invalid clipboard message: ") + what);
}

void putText(Bytes& out, const std::string& text, size_t most = maxText) {
    size_t size = std::min(text.size(), most);
    while (size > 0 && size < text.size() && (uint8_t(text[size]) & 0xC0) == 0x80) {
        --size;
    }
    putNumber(out, size, 2);
    out.insert(out.end(), text.begin(), text.begin() + ptrdiff_t(size));
}

std::string getText(std::span<const uint8_t> bytes, size_t& offset, size_t most = maxText) {
    const size_t size = getNumber(bytes, offset, 2);
    if (size > most || size > bytes.size() - offset) {
        invalid("its text");
    }
    std::string text(bytes.begin() + ptrdiff_t(offset), bytes.begin() + ptrdiff_t(offset + size));
    if (!utf8Characters(text)) {
        invalid("text that is not UTF-8");
    }
    offset += size;
    return text;
}

// A result some version of the SDK defines (RCS_OK with `orOk`).
rcs_result getResult(std::span<const uint8_t> bytes, size_t& offset, bool orOk = false) {
    const auto result = rcs_result(int32_t(uint32_t(getNumber(bytes, offset, 4))));
    if ((result == RCS_OK && !orOk) || result > RCS_OK || result < RCS_ERROR_REFUSED) {
        invalid("its result");
    }
    return result;
}

void putResult(Bytes& out, rcs_result result) {
    putNumber(out, uint32_t(result), 4);
}

bool getFlag(std::span<const uint8_t> bytes, size_t& offset) {
    const uint64_t flag = getNumber(bytes, offset, 1);
    if (flag > 1) {
        invalid("a flag neither 0 nor 1");
    }
    return flag == 1;
}

void end(std::span<const uint8_t> bytes, size_t offset) {
    if (offset != bytes.size()) {
        invalid("trailing bytes");
    }
}

Bytes rest(std::span<const uint8_t> bytes, size_t offset) {
    return Bytes(bytes.begin() + ptrdiff_t(offset), bytes.end());
}

} // namespace

std::string prefetchProblem(std::span<const FileItem> listed, uint64_t limit) {
    uint64_t left = limit;
    for (const auto& item : listed) {
        if (item.kind != RCS_FILE_REGULAR) {
            return "the other end lists more than files for its copy (\"" + item.path + "\")";
        }
        if (item.size > left) {
            return "the other end lists more bytes for its copy's files than files_prefetch_bytes (" +
                   std::to_string(limit) + ")";
        }
        left -= item.size;
    }
    return {};
}

Bytes encodeTakes(const Takes& takes) {
    Bytes out;
    putNumber(out, takes.formats, 4);
    putNumber(out, takes.maxBytes, 4);
    return out;
}

Takes decodeTakes(std::span<const uint8_t> bytes) {
    size_t offset = 0;
    Takes takes;
    takes.formats = uint32_t(getNumber(bytes, offset, 4));
    takes.maxBytes = uint32_t(getNumber(bytes, offset, 4));
    const bool none = !takes.formats && !takes.maxBytes;
    if (offset != bytes.size() ||
        (!none && ((takes.formats & ~knownFormats) || !takes.formats || takes.maxBytes < minBytes ||
                   takes.maxBytes > maxBytes))) {
        invalid("what the other end takes");
    }
    return takes;
}

Bytes encode(const PeerMessage& message) {
    if (const auto* copy = std::get_if<CopyMessage>(&message)) {
        Bytes out{copyType};
        const Bytes wire = encode(copy->content);
        out.insert(out.end(), wire.begin(), wire.end());
        return out;
    }
    const auto& withdrawn = std::get<WithdrawnMessage>(message);
    Bytes out{withdrawnType};
    out.insert(out.end(), withdrawn.publication.begin(), withdrawn.publication.end());
    return out;
}

PeerMessage decodePeer(std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        invalid("empty");
    }
    if (bytes[0] == copyType) {
        return CopyMessage{decode(bytes.subspan(1))};
    }
    if (bytes[0] != withdrawnType || bytes.size() != 1 + RCS_FILE_PUBLICATION_SIZE) {
        invalid("its type");
    }
    WithdrawnMessage withdrawn;
    std::copy(bytes.begin() + 1, bytes.end(), withdrawn.publication.begin());
    return withdrawn;
}

Bytes encode(const ToMachine& message) {
    Bytes out;
    if (const auto* use = std::get_if<UseMessage>(&message)) {
        if (!use->use) {
            out.push_back(stopType);
            return out;
        }
        out.push_back(useType);
        out.push_back(use->use->watch ? 1 : 0);
        putNumber(out, use->use->intervalMs, 4);
        putNumber(out, use->use->formats, 4);
        putNumber(out, use->use->maxBytes, 4);
        putText(out, use->use->filesDirectory, maxPath);
        out.push_back(use->use->standIn ? 1 : 0);
    } else if (const auto* place = std::get_if<PlaceMessage>(&message)) {
        out.push_back(placeType);
        putNumber(out, place->id, 8);
        out.push_back(place->ahead ? 1 : 0);
        const Bytes wire = clipboard::encode(place->content);
        out.insert(out.end(), wire.begin(), wire.end());
    } else if (const auto* listed = std::get_if<ListedMessage>(&message)) {
        out.push_back(listedType);
        putNumber(out, listed->request, 8);
        putNumber(out, listed->items.size(), 4);
        for (const auto& item : listed->items) {
            out.push_back(uint8_t(item.kind));
            putNumber(out, item.size, 8);
            putNumber(out, uint64_t(item.modified), 8);
            putText(out, item.path, RCS_FILE_PATH_MAX - 1);
        }
    } else if (const auto* data = std::get_if<DataMessage>(&message)) {
        out.push_back(dataType);
        putNumber(out, data->request, 8);
        putNumber(out, data->tag, 8);
        putResult(out, data->result);
        out.insert(out.end(), data->data.begin(), data->data.end());
    } else if (const auto* ended = std::get_if<EndedMessage>(&message)) {
        out.push_back(endedType);
        putNumber(out, ended->request, 8);
        putResult(out, ended->result);
        putText(out, ended->message);
        out.push_back(ended->complete ? 1 : 0);
    } else {
        out.push_back(revokedType);
        putNumber(out, std::get<RevokedMessage>(message).placing, 8);
    }
    return out;
}

ToMachine decodeToMachine(std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        invalid("empty");
    }
    size_t offset = 1;
    switch (bytes[0]) {
    case useType: {
        Use use;
        use.watch = getFlag(bytes, offset);
        use.intervalMs = uint32_t(getNumber(bytes, offset, 4));
        use.formats = uint32_t(getNumber(bytes, offset, 4));
        use.maxBytes = uint32_t(getNumber(bytes, offset, 4));
        use.filesDirectory = getText(bytes, offset, maxPath);
        use.standIn = getFlag(bytes, offset);
        end(bytes, offset);
        if (use.intervalMs < 50 || use.intervalMs > 5000 || !use.formats || (use.formats & ~knownFormats) ||
            use.maxBytes < minBytes || use.maxBytes > maxBytes) {
            invalid("its use");
        }
        return UseMessage{use};
    }
    case stopType:
        end(bytes, offset);
        return UseMessage{};
    case placeType: {
        PlaceMessage place;
        place.id = getNumber(bytes, offset, 8);
        place.ahead = getFlag(bytes, offset);
        place.content = decode(bytes.subspan(offset));
        return place;
    }
    case listedType: {
        ListedMessage listed;
        listed.request = getNumber(bytes, offset, 8);
        const uint64_t count = getNumber(bytes, offset, 4);
        if (count > maxEntries || count > (bytes.size() - offset) / 19) {
            invalid("a fetch's items");
        }
        for (uint64_t i = 0; i < count; ++i) {
            FileItem item;
            item.kind = rcs_file_kind(getNumber(bytes, offset, 1));
            item.size = getNumber(bytes, offset, 8);
            item.modified = int64_t(getNumber(bytes, offset, 8));
            item.path = getText(bytes, offset, RCS_FILE_PATH_MAX - 1);
            if (item.kind < RCS_FILE_REGULAR || item.kind > RCS_FILE_OTHER || item.path.empty()) {
                invalid("a fetch's item");
            }
            listed.items.push_back(std::move(item));
        }
        end(bytes, offset);
        return listed;
    }
    case dataType: {
        DataMessage data;
        data.request = getNumber(bytes, offset, 8);
        data.tag = getNumber(bytes, offset, 8);
        data.result = getResult(bytes, offset, true);
        data.data = rest(bytes, offset);
        if (data.data.size() > RCS_FILE_READ_MAX || (data.result != RCS_OK && !data.data.empty())) {
            invalid("a piece read");
        }
        return data;
    }
    case endedType: {
        EndedMessage ended;
        ended.request = getNumber(bytes, offset, 8);
        ended.result = getResult(bytes, offset, true);
        ended.message = getText(bytes, offset);
        ended.complete = getFlag(bytes, offset);
        end(bytes, offset);
        return ended;
    }
    case revokedType: {
        RevokedMessage revoked;
        revoked.placing = getNumber(bytes, offset, 8);
        end(bytes, offset);
        return revoked;
    }
    default:
        invalid("its type");
    }
}

Bytes encode(const ToSession& message) {
    Bytes out;
    if (const auto* copied = std::get_if<CopiedMessage>(&message)) {
        out.push_back(copiedType);
        out.push_back(copied->existing ? 1 : 0);
        putNumber(out, copied->content.leftOut, 4);
        const Bytes wire = clipboard::encode(copied->content, true);
        out.insert(out.end(), wire.begin(), wire.end());
    } else if (const auto* failed = std::get_if<ReadFailedMessage>(&message)) {
        out.push_back(readFailedType);
        putResult(out, failed->result);
        putText(out, failed->message);
    } else if (const auto* placed = std::get_if<PlacedMessage>(&message)) {
        out.push_back(placedType);
        putNumber(out, placed->id, 8);
        putResult(out, placed->result);
        putText(out, placed->message);
    } else if (const auto* abilities = std::get_if<AbilitiesMessage>(&message)) {
        out.push_back(abilitiesType);
        putNumber(out, abilities->files, 4);
        putText(out, abilities->why);
    } else if (const auto* want = std::get_if<WantMessage>(&message)) {
        out.push_back(wantType);
        putNumber(out, want->request, 8);
        putNumber(out, want->placing, 8);
        out.push_back(uint8_t(want->mode));
        putText(out, want->directory, maxPath);
        out.push_back(want->ahead ? 1 : 0);
    } else if (const auto* read = std::get_if<ReadMessage>(&message)) {
        out.push_back(readType);
        putNumber(out, read->request, 8);
        putNumber(out, read->item, 4);
        putNumber(out, read->offset, 8);
        putNumber(out, read->size, 4);
        putNumber(out, read->tag, 8);
    } else if (const auto* close = std::get_if<CloseMessage>(&message)) {
        out.push_back(closeType);
        putNumber(out, close->request, 8);
        putResult(out, close->outcome);
    } else {
        const auto& refused = std::get<RefusedMessage>(message);
        out.push_back(refusedType);
        putNumber(out, refused.placing, 8);
        putResult(out, refused.result);
        putText(out, refused.message);
    }
    return out;
}

ToSession decodeToSession(std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        invalid("empty");
    }
    size_t offset = 1;
    switch (bytes[0]) {
    case copiedType: {
        CopiedMessage copied;
        copied.existing = getFlag(bytes, offset);
        const auto leftOut = uint32_t(getNumber(bytes, offset, 4));
        if (leftOut & ~knownFormats) {
            invalid("its copy");
        }
        copied.content = decode(rest(bytes, offset), true, true);
        copied.content.leftOut = leftOut;
        return copied;
    }
    case readFailedType: {
        ReadFailedMessage failed;
        failed.result = getResult(bytes, offset);
        failed.message = getText(bytes, offset);
        end(bytes, offset);
        return failed;
    }
    case placedType: {
        PlacedMessage placed;
        placed.id = getNumber(bytes, offset, 8);
        placed.result = getResult(bytes, offset, true);
        placed.message = getText(bytes, offset);
        end(bytes, offset);
        return placed;
    }
    case abilitiesType: {
        AbilitiesMessage abilities;
        abilities.files = uint32_t(getNumber(bytes, offset, 4));
        abilities.why = getText(bytes, offset);
        end(bytes, offset);
        if (abilities.files & ~uint32_t(RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE)) {
            invalid("what the clipboard does with files");
        }
        return abilities;
    }
    case wantType: {
        WantMessage want;
        want.request = getNumber(bytes, offset, 8);
        want.placing = getNumber(bytes, offset, 8);
        want.mode = rcs_file_fetch_mode(getNumber(bytes, offset, 1));
        want.directory = getText(bytes, offset, maxPath);
        want.ahead = getFlag(bytes, offset);
        end(bytes, offset);
        if ((want.mode != RCS_FILE_FETCH_INTO && want.mode != RCS_FILE_FETCH_READ) ||
            (want.mode == RCS_FILE_FETCH_INTO) == want.directory.empty()) {
            invalid("a fetch asked for");
        }
        return want;
    }
    case readType: {
        ReadMessage read;
        read.request = getNumber(bytes, offset, 8);
        read.item = uint32_t(getNumber(bytes, offset, 4));
        read.offset = getNumber(bytes, offset, 8);
        read.size = uint32_t(getNumber(bytes, offset, 4));
        read.tag = getNumber(bytes, offset, 8);
        end(bytes, offset);
        return read;
    }
    case closeType: {
        CloseMessage close;
        close.request = getNumber(bytes, offset, 8);
        close.outcome = getResult(bytes, offset, true);
        end(bytes, offset);
        return close;
    }
    case refusedType: {
        RefusedMessage refused;
        refused.placing = getNumber(bytes, offset, 8);
        refused.result = getResult(bytes, offset);
        refused.message = getText(bytes, offset);
        end(bytes, offset);
        return refused;
    }
    default:
        invalid("its type");
    }
}

} // namespace rcsdk::clipboard
