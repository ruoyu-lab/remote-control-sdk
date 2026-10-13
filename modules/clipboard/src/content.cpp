#include "content.hpp"

#include <algorithm>
#include <set>

namespace rcsdk::clipboard {

std::optional<size_t> utf8Characters(std::string_view text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++count) {
        const auto lead = uint8_t(text[i]);
        const size_t length = lead < 0x80             ? 1
                              : (lead & 0xe0) == 0xc0 ? 2
                              : (lead & 0xf0) == 0xe0 ? 3
                              : (lead & 0xf8) == 0xf0 ? 4
                                                      : 0;
        if (!length || i + length > text.size()) {
            return std::nullopt;
        }
        uint32_t point = length == 1 ? lead : lead & (0x7f >> length);
        for (size_t k = 1; k < length; ++k) {
            if ((uint8_t(text[i + k]) & 0xc0) != 0x80) {
                return std::nullopt;
            }
            point = point << 6 | (uint8_t(text[i + k]) & 0x3f);
        }
        // Strict: no longer form than the code point needs, no surrogate
        // halves, nothing past U+10FFFF (what the system's converters refuse
        // or read differently).
        static constexpr uint32_t least[] = {0, 0, 0x80, 0x800, 0x10000};
        if (point < least[length] || (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) {
            return std::nullopt;
        }
        i += length;
    }
    return count;
}

std::string formatList(uint32_t formats) {
    static const char* names[] = {"", "TEXT", "HTML", "RTF", "IMAGE", "FILES"};
    std::string list;
    for (uint32_t format = RCS_CLIPBOARD_TEXT; format <= RCS_CLIPBOARD_FILES; ++format) {
        if (formats & (1u << format)) {
            list += std::string(list.empty() ? "" : ", ") + names[format];
        }
    }
    return list.empty() ? "nothing" : list;
}

bool fullPath(std::string_view path) noexcept {
#ifdef _WIN32
    const auto separator = [](char c) { return c == '\\' || c == '/'; };
    return (path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && separator(path[2])) ||
           (path.size() >= 3 && separator(path[0]) && separator(path[1]));
#else
    return !path.empty() && path[0] == '/';
#endif
}

void putNumber(Bytes& out, uint64_t value, size_t width) {
    for (size_t i = width; i-- > 0;) {
        out.push_back(uint8_t(value >> (8 * i)));
    }
}

uint64_t getNumber(std::span<const uint8_t> bytes, size_t& offset, size_t width) {
    if (bytes.size() < offset || bytes.size() - offset < width) {
        fail(RCS_ERROR_PROTOCOL, "A clipboard message is cut short");
    }
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
        value = value << 8 | bytes[offset + i];
    }
    offset += width;
    return value;
}

uint32_t Content::formats() const noexcept {
    uint32_t bits = files ? filesFormat : 0;
    for (const auto& item : items) {
        bits |= 1u << item.format;
    }
    return bits;
}

size_t filesBytes(const Files& files) noexcept {
    size_t total = 1 + RCS_FILE_PUBLICATION_SIZE + 4;
    for (const auto& entry : files.entries) {
        total += 19 + entry.name.size() + (entry.path.empty() ? 0 : 4 + entry.path.size());
    }
    return total;
}

size_t Content::bytes() const noexcept {
    size_t total = files ? filesBytes(*files) : 0;
    for (const auto& item : items) {
        total += item.bytes.size();
    }
    return total;
}

Content fitting(const Content& content, uint32_t formats, size_t maxBytes) {
    Content fitted;
    fitted.concealed = content.concealed;
    fitted.leftOut = content.leftOut;
    size_t total = 0;
    for (const auto& item : content.items) {
        if ((formats & (1u << item.format)) && item.bytes.size() <= maxBytes - total) {
            total += item.bytes.size();
            fitted.items.push_back(item);
        } else {
            fitted.leftOut |= 1u << item.format;
        }
    }
    if (content.files && (formats & filesFormat) && filesBytes(*content.files) <= maxBytes - total) {
        fitted.files = content.files;
    } else if (content.files) {
        fitted.leftOut |= filesFormat;
    }
    return fitted;
}

// File transfer's rules for a component of a path (its paths.cpp; the module
// builds on public headers alone): a copy's files are fetched by these names,
// and placed by them where they were fetched ahead.
std::string nameProblem(std::string_view name) {
    if (name.empty() || name.size() > 255 || !utf8Characters(name)) {
        return "a name of 1..255 bytes of UTF-8";
    }
    if (name == "." || name == "..") {
        return "\".\" or \"..\"";
    }
    const std::string quoted = "\"" + std::string(name) + "\"";
    for (const char c : name) {
        if (uint8_t(c) < 0x20 || c == 0x7F) {
            return "a control character in " + quoted;
        }
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) {
            return quoted + " holds one of / \\ : * ? \" < > | (Windows takes none of them)";
        }
    }
    if (name.back() == '.' || name.back() == ' ') {
        return quoted + " ends in a dot or a space (Windows drops them)";
    }
    // Windows' device names: the part before the first dot, its trailing
    // spaces left out, in any case; COM and LPT with a digit or a superscript 1..3.
    std::string base(name.substr(0, name.find('.')));
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    std::transform(base.begin(), base.end(), base.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; });
    const std::string number = base.size() > 3 ? base.substr(3) : std::string();
    if (base == "con" || base == "prn" || base == "aux" || base == "nul" || base == "conin$" || base == "conout$" ||
        ((base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) &&
         ((number.size() == 1 && number[0] >= '0' && number[0] <= '9') || number == "\xC2\xB9" ||
          number == "\xC2\xB2" || number == "\xC2\xB3"))) {
        return quoted + " is a name Windows reserves for a device";
    }
    if (name.ends_with(".rcsdk-part") || name.starts_with(".rcsdk-resume-")) {
        return quoted + " is a name of file transfer's own files";
    }
    return {};
}

Bytes encode(const Content& content, bool withPaths) {
    Bytes out;
    out.reserve(2 + content.items.size() * 5 + content.bytes());
    out.push_back(uint8_t((content.concealed ? 1 : 0) | (content.files ? 2 : 0)));
    out.push_back(uint8_t(content.items.size()));
    for (const auto& item : content.items) {
        out.push_back(uint8_t(item.format));
        putNumber(out, item.bytes.size(), 4);
        out.insert(out.end(), item.bytes.begin(), item.bytes.end());
    }
    if (const auto& files = content.files) {
        out.push_back(files->cut ? 1 : 0);
        out.insert(out.end(), files->publication.begin(), files->publication.end());
        putNumber(out, files->entries.size(), 4);
        for (const auto& entry : files->entries) {
            out.push_back(uint8_t(entry.kind));
            putNumber(out, entry.size, 8);
            putNumber(out, uint64_t(entry.modified), 8);
            putNumber(out, entry.name.size(), 2);
            out.insert(out.end(), entry.name.begin(), entry.name.end());
            if (withPaths) {
                putNumber(out, entry.path.size(), 4);
                out.insert(out.end(), entry.path.begin(), entry.path.end());
            }
        }
    }
    return out;
}

// A PNG's signature and header, before the system's decoder sees the peer's
// bytes: a size it would allocate gigabytes for is refused here.
static bool plausiblePng(std::span<const uint8_t> png) {
    static constexpr uint8_t start[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    if (png.size() < sizeof(start) + 8 || !std::equal(std::begin(start), std::end(start), png.begin())) {
        return false;
    }
    size_t offset = sizeof(start);
    const uint64_t width = getNumber(png, offset, 4), height = getNumber(png, offset, 4);
    return width && height && width <= 16384 && height <= 16384;
}

Content decode(std::span<const uint8_t> bytes, bool mayBeEmpty, bool withPaths) {
    const auto invalid = [](const std::string& what) {
        fail(RCS_ERROR_PROTOCOL, "Invalid clipboard copy: " + what);
    };
    if (bytes.size() < 2 || bytes[0] > 3 || bytes[1] > RCS_CLIPBOARD_IMAGE ||
        (!bytes[1] && !(bytes[0] & 2) && !mayBeEmpty)) {
        invalid("its flags or count");
    }
    Content content;
    content.concealed = bytes[0] & 1;
    size_t offset = 2;
    rcs_clipboard_format last = 0;
    for (uint8_t i = 0; i < bytes[1]; ++i) {
        if (bytes.size() - offset < 5) {
            invalid("truncated");
        }
        const rcs_clipboard_format format = bytes[offset++];
        const size_t size = getNumber(bytes, offset, 4);
        if (format <= last || format > RCS_CLIPBOARD_IMAGE) {
            invalid("a format no version defines, twice, or out of order");
        }
        if (!size || size > bytes.size() - offset) {
            invalid("an item's size");
        }
        const auto text = std::string_view(reinterpret_cast<const char*>(bytes.data() + offset), size);
        if ((format == RCS_CLIPBOARD_TEXT || format == RCS_CLIPBOARD_HTML) && !utf8Characters(text)) {
            invalid("text that is not UTF-8");
        }
        if (format == RCS_CLIPBOARD_IMAGE && !plausiblePng(bytes.subspan(offset, size))) {
            invalid("an image that is not a PNG of at most 16384 x 16384");
        }
        last = format;
        const auto begin = bytes.begin() + ptrdiff_t(offset);
        content.items.push_back({format, Bytes(begin, begin + ptrdiff_t(size))});
        offset += size;
    }
    if (bytes[0] & 2) {
        Files files;
        const uint64_t cut = getNumber(bytes, offset, 1);
        if (cut > 1 || bytes.size() - offset < files.publication.size()) {
            invalid("its files' flag");
        }
        std::copy_n(bytes.begin() + ptrdiff_t(offset), files.publication.size(), files.publication.begin());
        offset += files.publication.size();
        files.cut = cut == 1;
        const uint64_t count = getNumber(bytes, offset, 4);
        if (!count || count > maxEntries || count > (bytes.size() - offset) / 19) {
            invalid("1..100 000 files");
        }
        std::set<std::string> names;
        for (uint64_t i = 0; i < count; ++i) {
            FileEntry entry;
            entry.kind = rcs_file_kind(getNumber(bytes, offset, 1));
            entry.size = getNumber(bytes, offset, 8);
            entry.modified = int64_t(getNumber(bytes, offset, 8));
            const size_t nameSize = getNumber(bytes, offset, 2);
            if (nameSize > bytes.size() - offset) {
                invalid("a file's name");
            }
            entry.name.assign(reinterpret_cast<const char*>(bytes.data() + offset), nameSize);
            offset += nameSize;
            if (withPaths) {
                const size_t pathSize = getNumber(bytes, offset, 4);
                if (pathSize > bytes.size() - offset) {
                    invalid("a file's path");
                }
                entry.path.assign(reinterpret_cast<const char*>(bytes.data() + offset), pathSize);
                offset += pathSize;
            }
            if ((entry.kind != RCS_FILE_REGULAR && entry.kind != RCS_FILE_DIRECTORY) ||
                (entry.kind == RCS_FILE_DIRECTORY && entry.size)) {
                invalid("a file of a kind no version defines, or a directory with a size");
            }
            if (const auto problem = nameProblem(entry.name); !problem.empty()) {
                invalid("a file's name: " + problem);
            }
            if (!names.insert(entry.name).second) {
                invalid("a file's name twice");
            }
            files.entries.push_back(std::move(entry));
        }
        content.files = std::move(files);
    }
    if (offset != bytes.size()) {
        invalid("trailing bytes");
    }
    return content;
}

} // namespace rcsdk::clipboard
