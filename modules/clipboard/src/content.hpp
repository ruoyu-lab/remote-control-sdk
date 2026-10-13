// A clipboard copy as the module carries it (rcs_clipboard_content): one item
// per format, each in the one form rcsdk/clipboard.h describes, whatever the
// system calls it, and the files it names (FILES). The network and the
// agent's link carry it in one wire form; the files' paths on the machine go
// only between the halves, never to the other end.
#pragma once

#include "common.hpp"

#include <array>

namespace rcsdk::clipboard {

struct Item {
    rcs_clipboard_format format = RCS_CLIPBOARD_TEXT;
    Bytes bytes;
    bool operator==(const Item&) const = default;
};

using Publication = std::array<uint8_t, RCS_FILE_PUBLICATION_SIZE>;

// A file or directory a copy names at its top.
struct FileEntry {
    std::string name; // One component of file transfer's path rules.
    rcs_file_kind kind = RCS_FILE_REGULAR; // REGULAR or DIRECTORY.
    uint64_t size = 0;                     // A file's; a directory's is 0 (listed as it is fetched).
    int64_t modified = 0;                  // Nanoseconds since 1970 UTC; 0: none.
    std::string path;                      // On the machine (between the halves only): where it is.
    bool operator==(const FileEntry&) const = default;
};

// A copy's files: what one end copied (paths), or what the other end
// published of them for this end to fetch (its publication's id).
struct Files {
    bool cut = false; // The copying program marked them cut (only reported).
    Publication publication{};
    std::vector<FileEntry> entries;
    uint64_t placing = 0; // Placed on the machine: the copy its stand-in's pastes name (never carried).
    bool operator==(const Files&) const = default;
};

struct Content {
    std::vector<Item> items; // In format order, each format once.
    std::optional<Files> files;
    bool concealed = false;  // The copying program marked it (send_concealed).
    // Formats the clipboard held besides, not read or not passed on: too
    // large, or not taken. Never carried.
    uint32_t leftOut = 0;

    uint32_t formats() const noexcept;
    size_t bytes() const noexcept; // Of its items, and of its files' entries as they travel.
    bool operator==(const Content&) const = default;
};

// The bytes of `files` as they travel.
size_t filesBytes(const Files& files) noexcept;

// What of `content` goes where `formats` are taken and `maxBytes` fit: its
// items in format order, then its files, while they fit; the formats it held
// besides join leftOut.
Content fitting(const Content& content, uint32_t formats, size_t maxBytes);

// Why `name` cannot name a copy's file at the other end (file transfer's
// rules for a path of one component); empty when it can.
std::string nameProblem(std::string_view name);

// The wire form: flags(1: concealed, files) count(1), per item format(1)
// size(4) bytes; then the files: cut(1) publication(16) count(4), per entry
// kind(1) size(8) modified(8) name(2 + bytes), and `withPaths` (between the
// halves) path(4 + bytes).
inline constexpr size_t maxWireBytes = maxBytes + 2 + 5 * RCS_CLIPBOARD_IMAGE;
Bytes encode(const Content& content, bool withPaths = false);
// Throws Error(PROTOCOL) for anything but 1..4 items of formats some version
// defines, each once, in order, not empty, TEXT and HTML UTF-8, and files of
// 1..100 000 entries named by one component each, once (0 items with files,
// or nothing at all with `mayBeEmpty`).
Content decode(std::span<const uint8_t> bytes, bool mayBeEmpty = false, bool withPaths = false);

} // namespace rcsdk::clipboard
