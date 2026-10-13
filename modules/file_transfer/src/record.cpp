#include "record.hpp"

#include "paths.hpp"

namespace rcsdk::file_transfer::record {
namespace {

constexpr std::string_view magic = "RCSDKFT1";
// A record of 100 000 items with paths of about 200 bytes fits; a larger one
// is not read (its transfer starts over).
constexpr uint64_t mostBytes = 64u << 20;
enum : uint8_t { startedType = 1, completedType = 2 };

void append(const files::Directory& directory, const Token& token, const Bytes& entry) {
    const std::string name = recordName(token);
    const auto there = directory.status(name);
    const uint64_t size = there && there->kind == RCS_FILE_REGULAR ? there->size : 0;
    auto file = directory.write(name, size);
    if (!size) {
        file.append({reinterpret_cast<const uint8_t*>(magic.data()), magic.size()});
        file.append(token);
    }
    file.append(entry);
}

} // namespace

std::map<std::string, Entry> read(const files::Directory& directory, const Token& token) {
    std::map<std::string, Entry> entries;
    const std::string name = recordName(token);
    const auto there = directory.status(name);
    if (!there || there->kind != RCS_FILE_REGULAR || there->size < magic.size() + token.size() ||
        there->size > mostBytes) {
        return entries;
    }
    Bytes bytes(size_t(there->size));
    directory.read(name).readAt(0, bytes);
    if (!std::equal(magic.begin(), magic.end(), bytes.begin()) ||
        !std::equal(token.begin(), token.end(), bytes.begin() + ptrdiff_t(magic.size()))) {
        return entries;
    }
    Reader r(std::span<const uint8_t>(bytes).subspan(magic.size() + token.size()), "a record");
    try {
        while (r.remaining()) {
            const uint8_t type = r.u8();
            Entry entry;
            entry.size = r.u64();
            entry.modified = r.i64();
            std::string item = r.text(RCS_FILE_PATH_MAX - 1);
            if ((type != startedType && type != completedType) || pathProblem(item)) {
                break;
            }
            if (type == completedType) {
                entry.completed = true;
                entry.saved = r.text(RCS_FILE_PATH_MAX - 1);
                if (pathProblem(entry.saved)) {
                    break;
                }
            }
            entries[item] = entry;
        }
    } catch (const Error&) {
        // A record cut short (its writer stopped midway): what came before stands.
    }
    return entries;
}

void started(const files::Directory& directory, const Token& token, const ItemSpec& item) {
    append(directory, token, Writer().u8(startedType).u64(item.size).i64(item.modified).text(item.path).take());
}

void completed(const files::Directory& directory, const Token& token, const ItemSpec& item,
               const std::string& saved) {
    append(directory, token,
           Writer().u8(completedType).u64(item.size).i64(item.modified).text(item.path).text(saved).take());
}

void remove(const files::Directory& directory, const Token& token) {
    directory.remove(recordName(token));
}

} // namespace rcsdk::file_transfer::record
